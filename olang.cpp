// ============================================================
//  O 语言 · 参考解释器 v0.1
//  对应 SPEC v0.6.1（扁平地址空间 + 显式地址，非纸带）
//
//  用法:  olang <程序.o> [--map]
//    --map   打印静态区的地址映射表（名字 / 地址 / 单元数）
//
//  已实现（SPEC §12 的四段示例全部可跑）：
//    &{T;name} = {SPEC} = {INIT}      静态/栈分配、显式地址区间、别名
//    &{T;f} = o{params}{body}         命名函数、匿名函数（不捕获局部）
//    ${x}  ${a}{i}  ${a}.size{}       引用 / 下标 / 长度
//    ^{e}                             显式解引用（编译期无操作）
//    x | f     fn{a,b}   ${f}{x}      管道 / 直接调用 / 间接调用
//    (c){...}  [c]{...}  ?{e}  !       条件 / 循环 / 返回 / 跳出
//    .{int;e}  .{char;e}  .{str;s}     输出
//    ,{int;lv}                         输入
//    + - * / %   == != < <= > >=   && ||   - ~       运算符
//
//  与 SPEC 的有意偏差（解释器比编译器更保守）：
//    · 下标越界 → 运行时错误（SPEC 规定 UB，编译器不检查）
//    · 未定义变量 / 未定义函数 / 参数个数不符 → 运行时错误
//    · 只在必要时报错，不做完整类型检查（那是 v0.2 的事）
// ============================================================
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <iostream>
#ifndef _WIN32
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace std;

#ifndef OLANG_VERSION
#define OLANG_VERSION "1.1.0"
#endif

enum { MEM_SIZE = 16384 };          // 默认地址空间（可被 #mem 或 --mem-size 覆盖）
enum { MEM_SIZE_MAX = 1 << 20 };   // 上限：1M 单元 = 8 MB

// ---------------------------------------------------------------- 错误
struct Err : runtime_error {
    int line;
    int where;
    Err(const string& m, int ln = 0, int w = -1) : runtime_error(m), line(ln), where(w) {}
};

static string fmt(const char* f, long long a = 0) {
    char b[256];
    snprintf(b, sizeof b, f, a);
    return string(b);
}

// ---------------------------------------------------------------- 词法
enum TK {
    T_END, T_NUM, T_STR, T_ID,
    T_AMP, T_LBRACE, T_RBRACE, T_LPAREN, T_RPAREN, T_LBRACK, T_RBRACK,
    T_COMMA, T_SEMI, T_DOLLAR, T_DOT, T_QUES, T_BANG, T_PIPE,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_ASSIGN,
    T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE, T_AND, T_OR, T_TILDE, T_CARET, T_AT, T_COLON, T_HASH, T_SHL, T_SHR
};

struct Token {
    TK k;
    string s;
    long long n;
    double fval;
    bool isFlt;
    int line;
    Token() : k(T_END), n(0), fval(0), isFlt(false), line(0) {}
};

struct Lexer {
    string src;
    size_t p;
    int line;

    explicit Lexer(const string& s) : src(s), p(0), line(1) {}

    char at(size_t q) const { return q < src.size() ? src[q] : '\0'; }

    void skip() {
        for (;;) {
            while (p < src.size() && (src[p] == ' ' || src[p] == '\t' || src[p] == '\r' || src[p] == '\n')) {
                if (src[p] == '\n') ++line;
                ++p;
            }
            if (at(p) == '/' && at(p + 1) == '/') {
                while (p < src.size() && src[p] != '\n') ++p;
                continue;
            }
            if (at(p) == '/' && at(p + 1) == '*') {
                int cstart = line;
                p += 2;
                while (p < src.size() && !(src[p] == '*' && at(p + 1) == '/')) {
                    if (src[p] == '\n') ++line;
                    ++p;
                }
                if (p >= src.size()) throw Err("块注释没有闭合（缺少 */）", cstart);
                p += 2;
                continue;
            }
            break;
        }
    }

    Token next() {
        skip();
        Token t;
        t.line = line;
        if (p >= src.size()) { t.k = T_END; return t; }

        char c = src[p];
        if (c >= '0' && c <= '9') {
            if (c == '0' && (at(p + 1) == 'x' || at(p + 1) == 'X')) {     // 0x 十六进制
                p += 2;
                long long hv = 0;
                while (p < src.size() && isxdigit((unsigned char)src[p])) {
                    char ch = src[p];
                    hv = hv * 16 + ((ch <= '9') ? ch - '0' : (tolower(ch) - 'a' + 10));
                    ++p;
                }
                t.k = T_NUM; t.n = hv; t.fval = (double)hv; t.isFlt = false;
                return t;
            }
            long long v = 0;
            while (p < src.size() && src[p] >= '0' && src[p] <= '9') v = v * 10 + (src[p++] - '0');
            t.k = T_NUM;
            t.n = v;
            t.fval = (double)v;
            t.isFlt = false;
            // 小数部分：数字 '.' 数字（要求点后紧跟数字，避免吞掉 ${a}.size 的点）
            if (at(p) == '.' && at(p + 1) >= '0' && at(p + 1) <= '9') {
                t.isFlt = true;
                ++p;
                double scale = 0.1;
                while (p < src.size() && src[p] >= '0' && src[p] <= '9') {
                    t.fval += (src[p++] - '0') * scale;
                    scale *= 0.1;
                }
            }
            // 指数部分：e/E [+-]? digits
            if (at(p) == 'e' || at(p) == 'E') {
                size_t save = p;
                ++p;
                int sign = 1;
                if (at(p) == '+') ++p;
                else if (at(p) == '-') { sign = -1; ++p; }
                if (at(p) >= '0' && at(p) <= '9') {
                    int e = 0;
                    while (p < src.size() && src[p] >= '0' && src[p] <= '9') e = e * 10 + (src[p++] - '0');
                    t.fval *= ::pow(10.0, sign * e);
                    t.isFlt = true;
                } else {
                    p = save;
                }
            }
            return t;
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            string s;
            while (p < src.size()) {
                char d = src[p];
                if (!((d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z') || (d >= '0' && d <= '9') || d == '_')) break;
                s += d; ++p;
            }
            t.k = T_ID; t.s = s;
            return t;
        }
        if (c == '\'') {
            ++p;
            if (p >= src.size()) throw Err("字符字面量没有结束的引号", t.line);
            int v = 0;
            if (src[p] == '\\' && p + 1 < src.size()) {
                char e = src[p + 1];
                if (e == 'n') v = 10;
                else if (e == 't') v = 9;
                else if (e == '\\') v = 92;
                else if (e == '\'') v = 39;
                else if (e == '0') v = 0;
                else v = (unsigned char)e;
                p += 2;
            } else {
                v = (unsigned char)src[p];
                p += 1;
            }
            if (p >= src.size() || src[p] != '\'') throw Err("字符字面量只能包含一个字符", t.line);
            ++p;
            t.k = T_NUM; t.n = v; t.isFlt = false;
            return t;
        }
        if (c == '"') {
            ++p;
            string s;
            while (p < src.size() && src[p] != '"') {
                if (src[p] == '\\' && p + 1 < src.size()) {
                    char e = src[p + 1];
                    if (e == 'n') s += '\n';
                    else if (e == 't') s += '\t';
                    else if (e == '\\') s += '\\';
                    else if (e == '"') s += '"';
                    else if (e == '0') s += '\0';
                    else s += e;
                    p += 2;
                } else {
                    if (src[p] == '\n') ++line;
                    s += src[p++];
                }
            }
            if (p >= src.size()) throw Err("字符串没有结束的引号", t.line);
            ++p;
            t.k = T_STR; t.s = s;
            return t;
        }

        ++p;
        switch (c) {
        case '{': t.k = T_LBRACE; return t;
        case '}': t.k = T_RBRACE; return t;
        case '(': t.k = T_LPAREN; return t;
        case ')': t.k = T_RPAREN; return t;
        case '[': t.k = T_LBRACK; return t;
        case ']': t.k = T_RBRACK; return t;
        case ',': t.k = T_COMMA; return t;
        case ';': t.k = T_SEMI; return t;
        case '$': t.k = T_DOLLAR; return t;
        case '.': t.k = T_DOT; return t;
        case '?': t.k = T_QUES; return t;
        case '!':
            if (at(p) == '=') { ++p; t.k = T_NE; return t; }
            t.k = T_BANG; return t;
        case '|':
            if (at(p) == '|') { ++p; t.k = T_OR; return t; }
            t.k = T_PIPE; return t;
        case '+': t.k = T_PLUS; return t;
        case '-': t.k = T_MINUS; return t;
        case '*': t.k = T_STAR; return t;
        case '/': t.k = T_SLASH; return t;
        case '%': t.k = T_PERCENT; return t;
        case '=':
            if (at(p) == '=') { ++p; t.k = T_EQ; return t; }
            t.k = T_ASSIGN; return t;
        case '<':
            if (at(p) == '<') { ++p; t.k = T_SHL; return t; }
            if (at(p) == '=') { ++p; t.k = T_LE; return t; }
            t.k = T_LT; return t;
        case '>':
            if (at(p) == '>') { ++p; t.k = T_SHR; return t; }
            if (at(p) == '=') { ++p; t.k = T_GE; return t; }
            t.k = T_GT; return t;
        case '&':
            if (at(p) == '&') { ++p; t.k = T_AND; return t; }
            t.k = T_AMP; return t;
        case '~': t.k = T_TILDE; return t;
        case '^': t.k = T_CARET; return t;
        case '@': t.k = T_AT; return t;
        case ':': t.k = T_COLON; return t;
        case '#': t.k = T_HASH; return t;
        default: break;
        }
        throw Err(string("无法识别的字符 '") + c + "'", t.line);
    }
};

// ---------------------------------------------------------------- 类型表
// 注意：TY_* 是「种类」常量，不是「类型表下标」。从 100 开始，杜绝下标与种类混用而不报错。
enum {
    TY_BOOL = 100, TY_INT = 101, TY_FLOAT = 102, TY_DBL = 103,
    TY_FN = 104, TY_STR = 105, TY_STRUCT = 106, TY_CHAR = 107, TY_HANDLE = 108
};

struct TypeInfo {
    string name;
    int kind;                      // TY_* 之一
    int cells;                     // sizeof(T)，以「单元」计
    map<string, int> fieldType;    // struct 字段 -> 类型下标
    map<string, int> fieldOfs;     // struct 字段 -> 单元偏移
    vector<string> fieldOrder;     // struct 字段的声明顺序（位置字面量用）
    bool hasSig;                   // fn 类型：是否带签名
    int sigRet;                    // 签名：返回类型下标
    vector<int> sigArgs;           // 签名：参数类型下标
    TypeInfo() : kind(TY_INT), cells(1), hasSig(false), sigRet(-1) {}
};

struct TypeTable {
    vector<TypeInfo> t;
    map<string, int> byName;
    int iBool, iInt, iFloat, iDbl, iFn, iStr, iChar, iHandle;   // 内置类型在表中的下标
    TypeTable() : iBool(0), iInt(0), iFloat(0), iDbl(0), iFn(0), iStr(0), iChar(0), iHandle(0) {
        iBool  = add("bool", TY_BOOL, 1);
        iInt   = add("int", TY_INT, 1);
        iFloat = add("float", TY_FLOAT, 1);
        iDbl   = add("double", TY_DBL, 1);
        iFn    = add("fn", TY_FN, 1);
        iStr   = add("str", TY_STR, 1);
        iChar  = add("char", TY_CHAR, 1);
        iHandle = add("handle", TY_HANDLE, 1);
    }
    // fn{ret;arg,arg} 带签名的函数类型（按规范名去重）
    int addSig(int ret, const vector<int>& args) {
        string nm = "fn{" + t[ret].name + ";";
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) nm += ",";
            nm += t[args[i]].name;
        }
        nm += "}";
        map<string, int>::iterator it = byName.find(nm);
        if (it != byName.end()) return it->second;
        int idx = add(nm, TY_FN, 1);
        t[idx].hasSig = true;
        t[idx].sigRet = ret;
        t[idx].sigArgs = args;
        return idx;
    }

    int add(const string& n, int kind, int cells) {        int idx = (int)t.size();
        TypeInfo ti;
        ti.name = n;
        ti.kind = kind;
        ti.cells = cells;
        t.push_back(ti);
        byName[n] = idx;
        return idx;
    }
    int find(const string& n) const {
        map<string, int>::const_iterator it = byName.find(n);
        return it == byName.end() ? -1 : it->second;
    }
    const TypeInfo& at(int i) const { return t[i]; }
    int cellsOf(int i) const { return t[i].cells; }
    int kindOf(int i) const { return t[i].kind; }
    bool isNum(int i) const { return t[i].kind == TY_INT || t[i].kind == TY_DBL || t[i].kind == TY_FLOAT || t[i].kind == TY_BOOL; }
    bool isFloat(int i) const { return t[i].kind == TY_DBL || t[i].kind == TY_FLOAT; }
};

static TypeTable gTypes;

// ---------------------------------------------------------------- 内置函数库
// 导库语法：  #{"file"}      把库里的函数名放进全局作用域（重复导入无副作用）
struct NativeSig {
    int ret;
    vector<int> ptypes;   // 按位置
    vector<char> pref;    // 按位置：1 = 引用参数（收地址）
    string cname;         // C 端实现名（emit 用）
    bool isExtern;        // extern 声明的 C 函数（只有 C 后端能调）
    NativeSig() : ret(gTypes.iInt), isExtern(false) {}
};

static map<string, map<string, NativeSig> > gLibs;   // 库名 -> 函数名 -> 签名
static map<string, NativeSig> gNativeTab;            // 已导入的函数
static vector<string> gLinkFlags;                     // #link 收集的链接参数
static vector<string> gStrPool;                      // 字符串常量池

static int internStr(const string& s) {
    for (size_t i = 0; i < gStrPool.size(); ++i) if (gStrPool[i] == s) return (int)i;
    gStrPool.push_back(s);
    return (int)gStrPool.size() - 1;
}

static void initLibs() {
    if (!gLibs.empty()) return;
    const int IT = gTypes.iInt;      // 注意：签名里存的是「类型表下标」，不是 kind
    const int ST = gTypes.iStr;
    {
        map<string, NativeSig>& L = gLibs["sys"];
        NativeSig s;
        s.ret = IT; s.ptypes.push_back(ST); s.cname = "system";
        L["system"] = s;
    }
    {
        map<string, NativeSig>& L = gLibs["file"];
        NativeSig n;
        n.ret = IT; n.ptypes.push_back(ST); n.ptypes.push_back(ST); n.cname = "open";   L["open"] = n;
        n = NativeSig(); n.ret = IT; n.ptypes.push_back(IT); n.cname = "close";          L["close"] = n;
        n = NativeSig(); n.ret = IT; n.ptypes.push_back(IT); n.cname = "getc";           L["getc"] = n;
        n = NativeSig(); n.ret = IT; n.ptypes.push_back(IT); n.ptypes.push_back(IT); n.cname = "putc"; L["putc"] = n;
        n = NativeSig(); n.ret = IT; n.ptypes.push_back(IT); n.ptypes.push_back(ST); n.cname = "puts"; L["puts"] = n;
        n = NativeSig(); n.ret = IT; n.ptypes.push_back(IT); n.cname = "eof";            L["eof"] = n;
        n = NativeSig(); n.ret = IT; n.ptypes.push_back(IT); n.ptypes.push_back(IT); n.cname = "seek"; L["seek"] = n;
        n = NativeSig(); n.ret = IT; n.ptypes.push_back(IT); n.cname = "tell";           L["tell"] = n;
        n = NativeSig(); n.ret = IT; n.ptypes.push_back(IT); n.ptypes.push_back(IT); n.ptypes.push_back(IT);
        n.pref.push_back(0); n.pref.push_back(1); n.pref.push_back(0); n.cname = "read";    L["read"] = n;
        n = NativeSig(); n.ret = IT; n.ptypes.push_back(IT); n.ptypes.push_back(IT); n.ptypes.push_back(IT);
        n.pref.push_back(0); n.pref.push_back(1); n.pref.push_back(0); n.cname = "write";   L["write"] = n;
        n = NativeSig(); n.ret = IT; n.ptypes.push_back(ST); n.cname = "remove";         L["remove"] = n;
        n = NativeSig(); n.ret = IT; n.ptypes.push_back(ST); n.cname = "exists";         L["exists"] = n;
        n = NativeSig(); n.ret = IT; n.ptypes.push_back(ST); n.ptypes.push_back(ST); n.cname = "rename"; L["rename"] = n;
    }
    {
        map<string, NativeSig>& L = gLibs["str"];
        NativeSig n;
        n = NativeSig(); n.ret = IT; n.ptypes.push_back(ST); n.cname = "slen"; L["len"] = n;
        n = NativeSig(); n.ret = IT; n.ptypes.push_back(ST); n.ptypes.push_back(ST); n.cname = "sfind"; L["find"] = n;
        n = NativeSig(); n.ret = ST; n.ptypes.push_back(ST); n.ptypes.push_back(IT); n.ptypes.push_back(IT);
        n.cname = "ssubstr"; L["substr"] = n;
        n = NativeSig(); n.ret = IT; n.ptypes.push_back(ST); n.ptypes.push_back(IT); n.cname = "scharat"; L["charat"] = n;
    }
}

static bool importLib(const string& lib) {
    initLibs();
    map<string, map<string, NativeSig> >::iterator it = gLibs.find(lib);
    if (it == gLibs.end()) return false;
    for (map<string, NativeSig>::iterator f = it->second.begin(); f != it->second.end(); ++f)
        gNativeTab[f->first] = f->second;
    return true;
}

static NativeSig* findNative(const string& n) {
    map<string, NativeSig>::iterator it = gNativeTab.find(n);
    return it == gNativeTab.end() ? 0 : &it->second;
}

// ---------------------------------------------------------------- AST
struct Expr;
struct Stmt;
struct Decl;
typedef shared_ptr<Expr> EP;
typedef shared_ptr<Stmt> SP;
typedef shared_ptr<Decl> DP;

struct Decl {
    string type;              // 类型名（"int" / "Point" …）
    string name;
    int ty;                   // 类型下标
    int elems;                // 元素个数（int{0;9} = 10）
    bool isType;              // struct 类型声明（不分配单元）
    bool hasSpec;
    bool hasFence;            // 显式地址（T{x} / T{x;y}）
    int x, y;                 // 显式区间（hasFence 时有效）
    int size;                 // 总单元数 = elems * sizeof(T)
    int addr;                 // 静态区地址 / 帧内偏移（layout 填入）
    bool isFn;
    EP init;                  // 变量初值
    vector<string> params;    // 函数参数名
    vector<int> paramTy;      // 函数参数类型
    vector<char> paramRef;    // 1 = 引用参数
    vector<int> paramElems;   // 引用参数的区段长度
    vector<SP> body;          // 函数体
    int fnIdx;                // 函数下标（layout 填入）
    int frameSize;            // 函数的栈帧大小
    int line;
    Decl() : ty(-1), elems(1), isType(false), hasSpec(false), hasFence(false), x(-1), y(-1), size(1),
             addr(-1), isFn(false), fnIdx(-1), frameSize(0), line(0) {}
};

struct Expr {
    enum K { Num, StrE, RefE, CallE, UnitE, PipeE, UnE, BinE, FnE, ListE } k;
    long long num;
    double fval;
    bool isFlt;
    int litTy;                // Num 字面量的类型提示（-1 = 推断）
    string s;                 // 字符串字面量 / 引用名 / 调用名 / 管道函数名
    string method;            // "size" 或 ""
    vector<string> fields;    // 字段路径 ${p}.a.b
    bool hasCast;
    int castTy;
    vector<EP> kids;          // 下标 / 实参
    EP a, b;
    int op;
    vector<string> params;    // FnE
    vector<int> paramTy;
    vector<char> paramRef;
    vector<int> paramElems;
    vector<SP> body;          // FnE
    int fnIdx;
    int frameSize;
    int ty;                   // 标注后的结果类型（C 后端用）
    int srcTy;                // 引用在 .as{} 之前的类型
    int line;
    Expr() : k(Num), num(0), fval(0), isFlt(false), litTy(-1), hasCast(false), castTy(-1),
             op(0), fnIdx(-1), frameSize(0), ty(-1), srcTy(-1), line(0) {}
};

struct Stmt {
    enum K { DeclS, AssignS, IfS, WhileS, RetS, BreakS, ContinueS, OutS, InS, ExprS } k;
    DP decl;
    EP e, e2;                 // 条件/值/左值
    vector<SP> body;
    vector<SP> elseBody;      // if/else 的 else 分支（C 风格语法）
    vector<EP> args;          // fmt 语句的实参（args[0] = 格式串字面量）
    bool isFmt;               // .{fmt; …} 格式化输出
    int ty;                   // out/in 的类型下标
    int elems;                // out/in 的元素个数
    int line;
    Stmt() : k(ExprS), isFmt(false), ty(-1), elems(1), line(0) {}
};

// 把 "a=%d b=%s\n" 切成「字面段 / 占位符段」；只认 %d %c %s %g %f %%
struct FmtPiece { bool isSpec; string text; char conv; };
static vector<FmtPiece> splitFmt(const string& f, int* nspec) {
    vector<FmtPiece> out;
    string lit;
    int n = 0;
    for (size_t i = 0; i < f.size(); ++i) {
        if (f[i] != '%') { lit += f[i]; continue; }
        if (i + 1 < f.size() && f[i + 1] == '%') { lit += '%'; ++i; continue; }
        if (i + 1 >= f.size()) break;
        char c = f[i + 1];
        if (!strchr("dcsgfx", c)) { lit += f[i]; continue; }   // 不认识的占位符当字面量
        if (!lit.empty()) { FmtPiece p; p.isSpec = false; p.text = lit; p.conv = 0; out.push_back(p); lit.clear(); }
        FmtPiece p; p.isSpec = true; p.text = string("%") + c; p.conv = c; out.push_back(p);
        ++i; ++n;
    }
    if (!lit.empty()) { FmtPiece p; p.isSpec = false; p.text = lit; p.conv = 0; out.push_back(p); }
    if (nspec) *nspec = n;
    return out;
}

// ---------------------------------------------------------------- 解析
struct Parser {
    Lexer lx;
    Token cur, nxt;
    vector<shared_ptr<Expr> > anonFns;   // 所有 o{} 字面量，layout 时统一分配
    map<string,int> scopeNames;          // 当前函数 / O{} 作用域里已声明的名字（O 的块不引入新作用域）

    explicit Parser(const string& s) : lx(s) {
        cur = lx.next();
        nxt = lx.next();
    }

    void advance() { cur = nxt; nxt = lx.next(); }

    bool is(TK k) const { return cur.k == k; }
    bool isId(const char* s) const { return cur.k == T_ID && cur.s == s; }

    void expect(TK k, const char* what) {
        if (cur.k != k) throw Err(string("这里应该是 ") + what, cur.line);
        advance();
    }

    string expectId(const char* what) {
        if (cur.k != T_ID) throw Err(string("这里应该是") + what, cur.line);
        string s = cur.s;
        advance();
        return s;
    }

    EP mk(Expr::K k) {
        EP e(new Expr());
        e->k = k;
        e->line = cur.line;
        return e;
    }

    // ---- 程序 ----
    vector<DP> topDecls;
    vector<SP> mainBody;
    int declaredMem = 0;              // 源码里 #mem N 指定的地址空间（0 = 未指定）

    // #link "m" / #link "/path/libfoo.so"
    void linkDirective() {
        advance();                                   // #
        expectId("'link'");
        if (!is(T_STR)) throw Err("#link 后面应该是库名字符串", cur.line);
        gLinkFlags.push_back(cur.s);
        advance();
    }

    // extern double sqrt(double)
    // extern int sqlite3_open(str, handle&)
    void externDecl() {
        int ln = cur.line;
        advance();                                   // extern
        string rt = expectId("返回类型");
        int rti = cTypeIndex(rt, ln);
        string nm = expectId("C 函数名");
        expect(T_LPAREN, "'('");
        vector<int> pts; vector<char> outs;
        if (!is(T_RPAREN)) {
            for (;;) {
                int tl = cur.line;
                string tn = expectId("参数类型");
                int ti = cTypeIndex(tn, tl);
                bool o = false;
                if (is(T_AMP)) { advance(); o = true; }
                pts.push_back(ti);
                outs.push_back(o ? 1 : 0);
                if (is(T_COMMA)) { advance(); continue; }
                break;
            }
        }
        expect(T_RPAREN, "')'");
        NativeSig s;
        s.ret = rti; s.ptypes = pts; s.pref = outs; s.cname = nm; s.isExtern = true;
        gNativeTab[nm] = s;
    }

    // C 类型名 -> O 类型下标（P0 只支持数值与字符串）
    int cTypeIndex(const string& t, int line) {
        if (t == "int") return gTypes.iInt;
        if (t == "bool") return gTypes.iBool;
        if (t == "char") return gTypes.iChar;
        if (t == "double") return gTypes.iDbl;
        if (t == "float") return gTypes.iFloat >= 0 ? gTypes.iFloat : gTypes.iDbl;
        if (t == "str") return gTypes.iStr;
        if (t == "handle") return gTypes.iHandle;
        throw Err("extern 暂不支持的类型 '" + t + "'（P0 可用：int/bool/char/double/float/str）", line);
    }

    // #mem N：在源码里声明本次运行需要的地址空间
    void memDirective() {
        int ln = cur.line;
        expect(T_HASH, "'#'");
        string kw = expectId("'mem'");
        if (kw != "mem")
            throw Err("未知的 # 指令 '" + kw + "'（可用： #{\"lib\"} 导库、 #mem N 设地址空间）", ln);
        if (!is(T_NUM)) throw Err("#mem 后面应该是单元数", cur.line);
        long long n = cur.n;
        if (n < 1 || n > MEM_SIZE_MAX)
            throw Err("#mem 的单元数必须在 1.." + fmt("%lld", (long long)MEM_SIZE_MAX) + " 之间", ln);
        declaredMem = (int)n;
        advance();
    }

    // #{"lib"}
    void importStmt() {
        int ln = cur.line;
        expect(T_HASH, "'#'");
        expect(T_LBRACE, "'{'");
        if (!is(T_STR)) throw Err("#{…} 里应该是库名字符串", ln);
        string lib = cur.s;
        advance();
        expect(T_RBRACE, "'}'");
        if (!importLib(lib)) throw Err("没有名为 '" + lib + "' 的库（可用: sys, file）", ln);
    }

    void program() {
        while (!is(T_END) && !isId("O")) {
            if (is(T_HASH)) {
                if (nxt.k == T_LBRACE) { importStmt(); continue; }             // #{"lib"}
                if (nxt.k == T_ID && nxt.s == "link") { linkDirective(); continue; }  // #link
                memDirective();                                                // #mem N
                continue;
            }
            if (isId("extern")) { externDecl(); continue; }
            if (isId("struct") && nxt.k == T_ID) { topDecls.push_back(structDecl()); continue; }
            if (is(T_ID) && (gTypes.find(cur.s) >= 0 || cur.s == "void") && nxt.k == T_ID) {
                topDecls.push_back(cdecl(true));
                continue;
            }
            topDecls.push_back(decl());
        }
        if (!isId("O")) throw Err("缺少唯一的入口 O{...}", cur.line);
        advance();
        scopeNames.clear();                      // O{} 体是新的静态作用域
        mainBody = block();
        if (!is(T_END)) throw Err("O{} 之后还有内容", cur.line);
    }

    // ---- 声明 ----
    DP decl() {
        int ln = cur.line;
        expect(T_AMP, "'&'（声明以 &{T;name} 开头）");
        expect(T_LBRACE, "'{'");
        string ty = expectId("类型名");
        int tyIdx = -1;
        if (ty == "fn" && is(T_LBRACE)) tyIdx = fnSigAfterName();   // &{fn{int;int};mk} = …
        expect(T_SEMI, "';'");
        string nm = expectId("变量名");
        expect(T_RBRACE, "'}'");
        expect(T_ASSIGN, "'='");

        DP d(new Decl());
        d->line = ln;
        d->type = ty;
        d->name = nm;

        // 函数字面量 o{params}{body} / {params}{body}，或 struct 类型声明 {fields}
        if ((isId("o") && nxt.k == T_LBRACE) || (is(T_LBRACE) && nxt.k == T_LBRACE)) {
            if (isId("o")) advance();
            vector<string> pn;
            vector<int> pt;
            paramList(pn, pt, d->paramRef, d->paramElems);
            if (is(T_LBRACE)) {                          // 后面跟着函数体 → 函数
                if (tyIdx >= 0)
                    throw Err("fn{…} 声明的名字是【变量】而不是函数；要把匿名函数当值用，请分两步："
                              "&{fn{...};h} = {} 然后 ${h} = o{…}{…}", ln);
                d->params = pn;
                d->paramTy = pt;
                d->body = block();
                d->isFn = true;
                int rt = (tyIdx >= 0 ? tyIdx : gTypes.find(ty));
                if (rt < 0) throw Err("未知的返回类型 '" + ty + "'", ln);
                d->ty = rt;
                return d;
            }
            if (ty != "struct") throw Err("{T;name} 列表后面缺少函数体", ln);
            int idx = gTypes.add(nm, TY_STRUCT, 0);
            int ofs = 0;
            for (size_t i = 0; i < pn.size(); ++i) {
                if (gTypes.kindOf(pt[i]) == TY_FN)
                    throw Err("struct 字段不能是 fn 类型（fn 只作参数/局部变量；否则 ${p}.cb{x} 的"
                              "含义会与数组下标冲突，且地址计算无法区分）", ln);
                gTypes.t[idx].fieldType[pn[i]] = pt[i];
                gTypes.t[idx].fieldOfs[pn[i]] = ofs;
                gTypes.t[idx].fieldOrder.push_back(pn[i]);
                ofs += gTypes.cellsOf(pt[i]);
            }
            gTypes.t[idx].cells = ofs < 1 ? 1 : ofs;
            d->isType = true;
            d->ty = idx;
            return d;
        }

        // 类型规格 {T} / {T{x}} / {T{x;y}}（T 可以是 struct 名）
        if (is(T_LBRACE) && nxt.k == T_ID && gTypes.find(nxt.s) >= 0) {
            advance();                       // {
            string t2 = expectId("类型名");
            int ti = gTypes.find(t2);
            if (ti < 0) throw Err("未知类型 '" + t2 + "'", ln);
            d->hasSpec = true;
            d->ty = ti;
            d->type = t2;
            if (is(T_LBRACE)) {
                advance();
                long long a = num();
                if (is(T_SEMI)) { advance(); long long b = num(); d->x = (int)a; d->y = (int)b; }
                else { d->x = (int)a; d->y = (int)a; }
                d->hasFence = true;
                expect(T_RBRACE, "'}'");
            }
            expect(T_RBRACE, "'}'");
            d->elems = d->hasFence ? (d->y - d->x + 1) : 1;
            if (d->elems < 1) throw Err("地址区间长度必须 >= 1", ln);
            if (is(T_ASSIGN)) {
                advance();
                if (is(T_LBRACE) && nxt.k == T_RBRACE) { advance(); advance(); }   // {} = 全零初始化
                else d->init = expr();
            }
            return d;
        }

        // 只给初值：&{int;x} = {5}
        {
            int ti0 = (tyIdx >= 0 ? tyIdx : gTypes.find(ty));
            if (ti0 < 0) throw Err("未知类型 '" + ty + "'", ln);
            d->ty = ti0;
            d->elems = 1;
        }
        if (is(T_LBRACE) && nxt.k == T_RBRACE) { advance(); advance(); }   // {} = 全零初始化
        else d->init = expr();
        return d;
    }

    // 类型规格：普通类型名，或带签名的 fn{ret;arg,arg}
    int typeSpec() {
        int ln = cur.line;
        string t = expectId("类型名");
        if (t == "fn" && is(T_LBRACE)) return fnSigAfterName();
        int ti = gTypes.find(t);
        if (ti < 0) throw Err("未知类型 '" + t + "'", ln);
        return ti;
    }

    // 已经吃掉 "fn"，解析 {ret;arg,arg}
    int fnSigAfterName() {
        int ln = cur.line;
        expect(T_LBRACE, "'{'");        vector<int> args;
        string rt = expectId("返回类型");
        int ri = gTypes.find(rt);
        if (ri < 0) throw Err("未知类型 '" + rt + "'", ln);
        while (is(T_SEMI)) {
            advance();
            if (!is(T_ID)) throw Err("fn 签名的段不能为空", cur.line);
            for (;;) {
                string at = expectId("参数类型");
                int ai = gTypes.find(at);
                if (ai < 0) throw Err("未知类型 '" + at + "'", ln);
                args.push_back(ai);
                if (is(T_COMMA)) { advance(); continue; }
                break;
            }
        }
        expect(T_RBRACE, "'}'");
        return gTypes.addSig(ri, args);
    }

    // 参数：{T;name} 值参；{T{...};&name} 引用参数（& 表示"按引用"）
    void paramList(vector<string>& names, vector<int>& tys, vector<char>& refs, vector<int>& elems) {
        expect(T_LBRACE, "'{'");
        while (!is(T_RBRACE)) {
            expect(T_LBRACE, "参数 {T;name} 或 {T{...};&name}");
            int ti = typeSpec();
            int cnt = 1;
            if (is(T_LBRACE)) {
                advance();
                long long a = num();
                if (is(T_SEMI)) { advance(); long long b = num(); cnt = (int)(b - a + 1); }
                expect(T_RBRACE, "'}'");
            }
            expect(T_SEMI, "';'");
            bool isRef = false;
            if (is(T_AMP)) { advance(); isRef = true; }
            if (isRef && gTypes.kindOf(ti) == TY_FN)
                throw Err("fn 类型不能作引用参数（函数值是下标，不是可寻址对象）", cur.line);
            string nm = expectId("参数名");
            expect(T_RBRACE, "'}'");
            tys.push_back(ti);
            names.push_back(nm);
            refs.push_back(isRef ? 1 : 0);
            if (!isRef && cnt != 1)
                throw Err("值参数不能声明区段；要传数组/缓冲请用引用参数 {T{0;n};&name}", cur.line);
            elems.push_back(isRef ? (cnt < 1 ? 1 : cnt) : 1);
            if (is(T_COMMA)) advance();
            else break;
        }
        expect(T_RBRACE, "'}'");
    }

    // ---- 块与语句 ----
    vector<SP> block() {
        expect(T_LBRACE, "'{'");
        vector<SP> v;
        while (!is(T_RBRACE)) {
            if (is(T_END)) throw Err("块没有结束的 '}'", cur.line);
            v.push_back(stmt());
        }
        expect(T_RBRACE, "'}'");
        return v;
    }

    SP wrap(Stmt::K k) {
        SP s(new Stmt());
        s->k = k;
        s->line = cur.line;
        return s;
    }

    // struct P { int x; int y; } → 与 &{struct;P} = {{int;x},{int;y}} 等价
    DP structDecl() {
        int ln = cur.line;
        advance();
        string nm = expectId("结构体名");
        expect(T_LBRACE, "'{'");
        vector<string> fnames; vector<int> ftys;
        while (!is(T_RBRACE)) {
            int fl = cur.line;
            string ft = expectId("字段类型");
            int fti = gTypes.find(ft);
            if (fti < 0) throw Err("未知字段类型 '" + ft + "'", fl);
            fnames.push_back(expectId("字段名")); ftys.push_back(fti);
            if (is(T_SEMI)) advance();
        }
        expect(T_RBRACE, "'}'");
        if (is(T_SEMI)) advance();
        int idx = gTypes.add(nm, TY_STRUCT, 0);
        int ofs = 0;
        for (size_t i = 0; i < fnames.size(); ++i) {
            if (gTypes.kindOf(ftys[i]) == TY_FN) throw Err("struct 字段不能是 fn 类型", ln);
            gTypes.t[idx].fieldType[fnames[i]] = ftys[i];
            gTypes.t[idx].fieldOfs[fnames[i]] = ofs;
            gTypes.t[idx].fieldOrder.push_back(fnames[i]);
            ofs += gTypes.cellsOf(ftys[i]);
        }
        gTypes.t[idx].cells = ofs;
        DP d(new Decl());
        d->line = ln; d->type = "struct"; d->name = nm; d->ty = idx; d->isType = true;
        return d;
    }

    // C 风格声明：int x = 5;  /  int a[0..9];  /  int f(int a, int& b) { … }
    DP cdecl(bool allowFn) {
        int ln = cur.line;
        string ty = expectId("类型名");
        bool isVoid = (ty == "void");
        int ti = isVoid ? gTypes.iInt : gTypes.find(ty);
        if (ti < 0) throw Err("未知类型 '" + ty + "'", ln);
        string nm = expectId("变量名");
        if (scopeNames.count(nm))
            throw Err("重复声明 '" + nm + "'（O 的块不引入新作用域，同一个函数里请换名）", ln);
        DP d(new Decl());
        d->line = ln; d->type = ty; d->name = nm; d->ty = ti; d->elems = 1;
        if (allowFn && is(T_LPAREN)) {                    // 函数定义
            advance();
            if (!is(T_RPAREN)) {
                for (;;) {
                    int pl = cur.line;
                    string pt = expectId("参数类型");
                    int pti = gTypes.find(pt);
                    if (pti < 0) throw Err("未知参数类型 '" + pt + "'", pl);
                    bool ref = false;
                    if (is(T_AMP)) { ref = true; advance(); }
                    string pn = expectId("参数名");
                    if (scopeNames.count(pn))
                        throw Err("参数名重复 '" + pn + "'", pl);
                    int pe = 1;
                    if (is(T_LBRACK)) {                  // 数组形参：int a[0..3] → &a + 长度 4
                        advance();
                        long long lo = num();
                        expect(T_DOT, "'..'"); expect(T_DOT, "'..'");
                        long long hi = num();
                        expect(T_RBRACK, "']'");
                        if (hi < lo) throw Err("数组形参区间长度必须 >= 1", pl);
                        ref = true;
                        pe = (int)(hi - lo + 1);
                    }
                    d->params.push_back(pn); d->paramTy.push_back(pti);
                    d->paramRef.push_back(ref ? 1 : 0);
                    d->paramElems.push_back(pe);
                    if (is(T_COMMA)) { advance(); continue; }
                    break;
                }
            }
            expect(T_RPAREN, "')'");
            if (!is(T_LBRACE)) throw Err("函数定义缺少函数体", ln);
            map<string,int> saved = scopeNames;
            scopeNames.clear();
            for (size_t k = 0; k < d->params.size(); ++k) scopeNames[d->params[k]] = 1;
            d->body = block();
            scopeNames = saved;
            d->isFn = true;
            if (isVoid) {                                 // void f() ≡ int f() { … return 0; }
                SP r = wrap(Stmt::RetS);
                EP z = mk(Expr::Num); z->num = 0; z->litTy = gTypes.iInt;
                r->e = z; r->line = ln;
                d->body.push_back(r);
            }
            return d;
        }
        if (is(T_LBRACK)) {                               // 显式区间 [a..b]
            advance();
            long long a = num();
            expect(T_DOT, "'..'"); expect(T_DOT, "'..'");
            long long b = num();
            expect(T_RBRACK, "']'");
            d->x = (int)a; d->y = (int)b; d->hasFence = true;
            d->elems = (int)(b - a + 1);
            if (d->elems < 1) throw Err("地址区间长度必须 >= 1", ln);
        }
        if (is(T_ASSIGN)) {
            advance();
            if (is(T_LBRACE) && nxt.k == T_RBRACE) { advance(); advance(); }
            else if (is(T_LBRACE)) {                      // {1, 2, 3} / {1;2;3}
                EP lst = mk(Expr::ListE);
                advance();
                for (;;) {
                    lst->kids.push_back(expr());
                    if (is(T_COMMA) || is(T_SEMI)) { advance(); continue; }
                    break;
                }
                expect(T_RBRACE, "'}'");
                d->init = lst;
            }
            else {
                EP ini = expr();
                // C 风格 int b = a; 是「取值拷贝」。旧语法里裸 ${x} 不能当一等值，
                // 这里把裸引用包成求值单元（UnitE），语义就是读值而不是存引用。
                if (ini->k == Expr::RefE && !ini->hasCast && ini->method.empty()) {
                    EP u = mk(Expr::UnitE);
                    u->a = ini;
                    u->line = ini->line;
                    u->ty = -1;
                    ini = u;
                }
                d->init = ini;
            }
        }
        if (is(T_SEMI)) advance();
        scopeNames[nm] = 1;
        return d;
    }

    // 裸变量名：a / a[i] / p.x
    EP bareRef() {
        EP e = mk(Expr::RefE);
        e->s = cur.s;
        e->line = cur.line;
        advance();
        for (;;) {
            if (is(T_LBRACK)) {
                advance();
                e->kids.push_back(expr());
                expect(T_RBRACK, "']'");
                continue;
            }
            if (is(T_DOT)) {
                advance();
                string fld = expectId("字段名");
                if (is(T_LPAREN) && nxt.k == T_RPAREN) {  // 零参方法：s.len() / s.size()
                    advance();
                    expect(T_RPAREN, "')'");
                    e->method = fld;
                    continue;
                }
                if (is(T_LPAREN)) {                       // 带参方法：s.find(x) → find(s, x)
                    advance();
                    EP call = mk(Expr::CallE);
                    call->s = fld;
                    call->line = e->line;
                    call->kids.push_back(e);              // 接收者作为第一个实参
                    if (!is(T_RPAREN)) {
                        call->kids.push_back(expr());
                        while (is(T_COMMA)) { advance(); call->kids.push_back(expr()); }
                    }
                    expect(T_RPAREN, "')'");
                    e = call;
                    continue;
                }
                e->fields.push_back(fld);
                continue;
            }
            break;
        }
        return e;
    }

    SP stmt() {
        if (is(T_HASH)) {                          // #{"lib"} 也可以写在 O{} 里
            importStmt();
            SP s = wrap(Stmt::ExprS);
            EP z = mk(Expr::Num);
            z->num = 0;
            s->e = z;
            return s;
        }
        if (is(T_AMP)) {
            SP s = wrap(Stmt::DeclS);
            s->decl = decl();
            return s;
        }
        if (is(T_ID) && (cur.s == "if" || cur.s == "while")) {
            bool isIf = (cur.s == "if");
            SP s = wrap(isIf ? Stmt::IfS : Stmt::WhileS);
            advance();
            if (!is(T_LPAREN)) throw Err("if/while 后面要跟 (条件)", cur.line);
            advance();
            s->e = expr();
            expect(T_RPAREN, "')'");
            s->body = block();
            if (isIf && isId("else")) {
                advance();
                if (isId("if")) {                      // else if → 嵌套
                    s->elseBody.push_back(stmt());
                } else {
                    s->elseBody = block();
                }
            }
            return s;
        }
        if (is(T_ID) && cur.s == "return") {
            SP s = wrap(Stmt::RetS);
            advance();
            if (!is(T_RBRACE) && !is(T_SEMI)) s->e = expr();
            else { EP z = mk(Expr::Num); z->num = 0; s->e = z; }
            if (is(T_SEMI)) advance();
            return s;
        }
        if (is(T_ID) && cur.s == "continue") {
            SP s = wrap(Stmt::ContinueS);
            advance();
            if (is(T_SEMI)) advance();
            return s;
        }
        if (is(T_ID) && cur.s == "break") {
            SP s = wrap(Stmt::BreakS);
            advance();
            if (is(T_SEMI)) advance();
            return s;
        }
        if (is(T_ID) && cur.s == "print") {
            SP s = wrap(Stmt::OutS);
            advance();
            if (!is(T_LPAREN)) throw Err("print 后面要跟 (表达式)", cur.line);
            advance();
            if (is(T_STR) && nxt.k == T_COMMA) {        // print("a=%d", x)：复用 fmt
                s->isFmt = true;
                s->ty = gTypes.iStr;
                s->args.push_back(expr());
                while (is(T_COMMA)) { advance(); s->args.push_back(expr()); }
                expect(T_RPAREN, "')'");
                if (is(T_SEMI)) advance();
                return s;
            }
            s->e = expr();
            expect(T_RPAREN, "')'");
            if (is(T_SEMI)) advance();
            return s;                                  // ty 留给标注期推断
        }
        if (is(T_ID) && gTypes.find(cur.s) >= 0 && nxt.k == T_ID) {   // int x = 5;
            SP s = wrap(Stmt::DeclS);
            s->decl = cdecl(false);
            return s;
        }
        if (is(T_LPAREN)) {                       // 条件
            SP s = wrap(Stmt::IfS);
            advance();
            s->e = expr();
            expect(T_RPAREN, "')'");
            s->body = block();
            return s;
        }
        if (is(T_LBRACK)) {                       // 循环
            SP s = wrap(Stmt::WhileS);
            advance();
            s->e = expr();
            expect(T_RBRACK, "']'");
            s->body = block();
            return s;
        }
        if (is(T_QUES)) {                         // 返回
            SP s = wrap(Stmt::RetS);
            advance();
            s->e = unit();
            return s;
        }
        if (is(T_BANG)) {                         // 跳出
            SP s = wrap(Stmt::BreakS);
            advance();
            return s;
        }
        if (is(T_DOT)) {                          // 输出
            SP s = wrap(Stmt::OutS);
            advance();
            expect(T_LBRACE, "'{'");
            if (is(T_ID) && cur.s == "fmt") {      // 格式化输出：.{fmt; "…"; 实参…}
                advance();
                s->isFmt = true;
                s->ty = gTypes.iStr;
                expect(T_SEMI, "';'");
                for (;;) {
                    s->args.push_back(expr());
                    if (is(T_SEMI)) { advance(); continue; }
                    break;
                }
                expect(T_RBRACE, "'}'");
                return s;
            }
            tySpec(s->ty, s->elems, s->line);
            expect(T_SEMI, "';'");
            s->e = expr();
            expect(T_RBRACE, "'}'");
            return s;
        }
        if (is(T_COMMA)) {                        // 输入
            SP s = wrap(Stmt::InS);
            advance();
            expect(T_LBRACE, "'{'");
            tySpec(s->ty, s->elems, s->line);
            expect(T_SEMI, "';'");
            s->e = expr();
            expect(T_RBRACE, "'}'");
            return s;
        }
        // 表达式语句 / 赋值
        SP s = wrap(Stmt::ExprS);
        s->e = expr();
        if (is(T_SEMI)) advance();                 // C 风格语句末尾的分号
        return s;
    }

    void tySpec(int& ty, int& elems, int line) {
        ty = typeSpec();                       // 支持 fn{ret;args} 签名
        elems = 1;
        if (is(T_LBRACE)) {
            advance();
            long long a = num();
            if (is(T_SEMI)) { advance(); long long b = num(); elems = (int)(b - a + 1); }
            expect(T_RBRACE, "'}'");
        }
        if (elems < 1) elems = 1;
        (void)line;
    }

    long long num() {
        if (!is(T_NUM)) throw Err("这里应该是数字", cur.line);
        long long v = cur.n;
        advance();
        return v;
    }

    EP unit() {
        expect(T_LBRACE, "求值单元 '{'");
        EP first = expr();
        if (!is(T_SEMI)) {                       // 普通求值单元
            expect(T_RBRACE, "'}'");
            return first;
        }
        EP lst = mk(Expr::ListE);                // {v1;v2;…} 列表（数组初始化用）
        lst->kids.push_back(first);
        while (is(T_SEMI)) {
            advance();
            lst->kids.push_back(expr());
        }
        expect(T_RBRACE, "'}'");
        return lst;
    }

    // ---- 表达式 ----
    EP expr() {
        EP l = or_();
        if (is(T_ASSIGN)) {
            int ln = cur.line;
            if (l->k != Expr::RefE) throw Err("赋值左边必须是引用 ${x}", ln);
            advance();
            EP e = mk(Expr::BinE);
            e->op = T_ASSIGN;
            e->a = l;
            e->b = expr();
            e->line = ln;
            return e;
        }
        return l;
    }

    EP pipe() {
        EP l = or_();
        while (is(T_PIPE)) {
            int ln = cur.line;
            advance();
            EP e = mk(Expr::PipeE);
            e->line = ln;
            e->a = l;
            if (is(T_DOLLAR)) {
                e->b = ref();
            } else {
                string nm = expectId("函数名或 ${f}");
                e->s = nm;
            }
            l = e;
        }
        return l;
    }

    EP or_() {
        EP l = and_();
        while (is(T_OR)) { int ln = cur.line; advance(); EP e = mk(Expr::BinE); e->op = T_OR; e->a = l; e->b = and_(); e->line = ln; l = e; }
        return l;
    }

    EP and_() {
        EP l = bitOr();
        while (is(T_AND)) { int ln = cur.line; advance(); EP e = mk(Expr::BinE); e->op = T_AND; e->a = l; e->b = bitOr(); e->line = ln; l = e; }
        return l;
    }

    // 位运算优先级：| < ^ < & < 比较（与 C 一致）
    EP bitOr() {
        EP l = bitXor();
        while (is(T_PIPE)) { int ln = cur.line; advance(); EP e = mk(Expr::BinE); e->op = T_PIPE; e->a = l; e->b = bitXor(); e->line = ln; l = e; }
        return l;
    }
    EP bitXor() {
        EP l = bitAnd();
        while (is(T_CARET) && nxt.k != T_LBRACE) { int ln = cur.line; advance(); EP e = mk(Expr::BinE); e->op = T_CARET; e->a = l; e->b = bitAnd(); e->line = ln; l = e; }
        return l;
    }
    EP bitAnd() {
        EP l = cmp();
        while (is(T_AMP) && nxt.k != T_LBRACE) { int ln = cur.line; advance(); EP e = mk(Expr::BinE); e->op = T_AMP; e->a = l; e->b = cmp(); e->line = ln; l = e; }
        return l;
    }
    EP shift() {
        EP l = add();
        while (is(T_SHL) || is(T_SHR)) { int op = cur.k; int ln = cur.line; advance(); EP e = mk(Expr::BinE); e->op = op; e->a = l; e->b = add(); e->line = ln; l = e; }
        return l;
    }

    EP cmp() {
        EP l = shift();
        int op = 0;
        if (is(T_EQ)) op = T_EQ;
        else if (is(T_NE)) op = T_NE;
        else if (is(T_LT)) op = T_LT;
        else if (is(T_LE)) op = T_LE;
        else if (is(T_GT)) op = T_GT;
        else if (is(T_GE)) op = T_GE;
        if (!op) return l;
        int ln = cur.line;
        advance();
        EP e = mk(Expr::BinE);
        e->op = op; e->a = l; e->b = shift(); e->line = ln;
        return e;
    }

    EP add() {
        EP l = mul();
        while (is(T_PLUS) || is(T_MINUS)) {
            int op = cur.k; int ln = cur.line; advance();
            EP e = mk(Expr::BinE); e->op = op; e->a = l; e->b = mul(); e->line = ln; l = e;
        }
        return l;
    }

    EP mul() {
        EP l = unary();
        while (is(T_STAR) || is(T_SLASH) || is(T_PERCENT)) {
            int op = cur.k; int ln = cur.line; advance();
            EP e = mk(Expr::BinE); e->op = op; e->a = l; e->b = unary(); e->line = ln; l = e;
        }
        return l;
    }

    EP unary() {
        if (is(T_MINUS) || is(T_TILDE) || is(T_CARET)) {
            int op = cur.k; int ln = cur.line; advance();
            EP e = mk(Expr::UnE); e->op = op; e->a = unary(); e->line = ln;
            return e;
        }
        if (is(T_BANG)) {                       // 表达式里的 !x = 逻辑非（语句位置的 ! 仍是 break）
            int ln = cur.line; advance();
            EP e = mk(Expr::UnE); e->op = T_BANG; e->a = unary(); e->line = ln;
            return e;
        }
        return primary();
    }

    EP primary() {
        if (is(T_NUM)) {
            EP e = mk(Expr::Num);
            // int 字面量在解析期就截断到 32 位：两个后端必须看到同一个值
            // （{3000000000} == (int32_t)3000000000，解释器与 C 后端一致）
            e->num = cur.isFlt ? cur.n : (long long)(int32_t)(uint32_t)(unsigned long long)cur.n;
            e->isFlt = cur.isFlt;
            e->fval = cur.fval;
            advance();
            return e;
        }
        if (is(T_ID) && (cur.s == "true" || cur.s == "false")) {
            EP e = mk(Expr::Num);
            e->num = (cur.s == "true") ? 1 : 0;
            e->litTy = gTypes.iBool;
            advance();
            return e;
        }
        if (is(T_STR)) {
            EP e = mk(Expr::StrE);
            e->s = cur.s;
            e->num = internStr(cur.s);
            advance();
            return e;
        }
        if (is(T_LPAREN)) { advance(); EP e = expr(); expect(T_RPAREN, "')'"); return e; }
        if (is(T_LBRACE)) return unit();
        if (is(T_DOLLAR)) return ref();
        if (is(T_ID)) {
            // o{...}{...} 匿名函数
            if (cur.s == "o" && nxt.k == T_LBRACE) {
                EP e = mk(Expr::FnE);
                advance();
                paramList(e->params, e->paramTy, e->paramRef, e->paramElems);
                for (size_t i = 0; i < e->paramRef.size(); ++i)
                    if (e->paramRef[i])
                        throw Err("匿名函数不能有引用参数（它只能被间接调用，而间接调用只传值）", cur.line);
                e->body = block();
                anonFns.push_back(e);
                return e;
            }
            if (nxt.k == T_LPAREN) {                 // 调用 f(args)
                EP e = mk(Expr::CallE);
                e->s = cur.s;
                advance(); advance();
                if (!is(T_RPAREN)) {
                    e->kids.push_back(expr());
                    while (is(T_COMMA)) { advance(); e->kids.push_back(expr()); }
                }
                expect(T_RPAREN, "')'");
                return e;
            }
            if (nxt.k == T_LBRACE) {                 // 调用 f{...}
                EP e = mk(Expr::CallE);
                e->s = cur.s;
                advance();
                advance();                            // {
                if (!is(T_RBRACE)) {
                    e->kids.push_back(expr());
                    while (is(T_COMMA)) { advance(); e->kids.push_back(expr()); }
                }
                expect(T_RBRACE, "'}'");
                return e;
            }
            if (gTypes.find(cur.s) < 0) return bareRef();   // 裸变量名 / 字段 / 下标
            throw Err("标识符 '" + cur.s + "' 不能单独当值用；变量请写 ${" + cur.s + "}", cur.line);
        }
        throw Err("这里应该是一个值", cur.line);
    }

    vector<string> params() {
        vector<string> v;
        expect(T_LBRACE, "'{'");
        while (!is(T_RBRACE)) {
            expect(T_LBRACE, "参数 {T;name}");
            expectId("参数类型");
            expect(T_SEMI, "';'");
            v.push_back(expectId("参数名"));
            expect(T_RBRACE, "'}'");
            if (is(T_COMMA)) advance();
            else break;
        }
        expect(T_RBRACE, "'}'");
        return v;
    }

    EP ref() {
        int ln = cur.line;
        expect(T_DOLLAR, "'$'");
        expect(T_LBRACE, "'{'");
        EP e = mk(Expr::RefE);
        e->line = ln;
        e->s = expectId("变量名");
        expect(T_RBRACE, "'}'");
        for (;;) {
            if (is(T_LBRACE) && nxt.k != T_RBRACE) {         // 下标；fn 类型时是调用实参
                advance();
                e->kids.push_back(expr());
                while (is(T_COMMA)) { advance(); e->kids.push_back(expr()); }
                expect(T_RBRACE, "'}'");
                continue;
            }
            if (is(T_DOT)) {
                if (nxt.k != T_ID) break;                     // 后面不是标识符 → 留给语句层的输出命令 '.'
                int dl = cur.line;
                advance();
                if (is(T_ID) && cur.s == "size" && nxt.k == T_LBRACE) {
                    advance();
                    e->method = "size";
                    advance();                                // {
                    expect(T_RBRACE, "'}'");
                    continue;
                }
                if (is(T_ID) && cur.s == "len" && nxt.k == T_LBRACE) {   // str 长度（字节数）
                    advance();
                    e->method = "len";
                    advance();
                    expect(T_RBRACE, "'}'");
                    continue;
                }
                if (is(T_ID) && cur.s == "as" && nxt.k == T_LBRACE) {
                    advance();
                    advance();                                // {
                    string tn = expectId("目标类型名");
                    int ti = gTypes.find(tn);
                    if (ti < 0) throw Err("未知类型 '" + tn + "'", dl);
                    e->hasCast = true;
                    e->castTy = ti;
                    expect(T_RBRACE, "'}'");
                    continue;
                }
                if (is(T_ID)) {                               // 字段
                    e->fields.push_back(expectId("字段名"));
                    continue;
                }
                throw Err("'.' 后面应是字段名 / as{T} / size{}", dl);
            }
            break;
        }
        return e;
    }
};

// ---------------------------------------------------------------- 运行时
struct Func {
    string name;
    int retTy;
    vector<string> params;
    vector<int> paramTy;
    vector<int> paramOfs;
    vector<char> paramRef;     // 1 = 引用参数（帧槽里存地址）
    vector<int> paramElems;    // 引用参数指向的区段长度
    vector<SP> body;
    int frameSize;
    Func() : retTy(-1), frameSize(0) {}
};

struct VarInfo {
    int addr;      // 起始单元（引用参数时该单元里存的是真地址）
    int elems;     // 元素个数
    int ty;        // 类型下标
    bool isRef;    // 是否引用参数
    VarInfo() : addr(0), elems(1), ty(gTypes.iInt), isRef(false) {}
};


union Cell {
    long long i;
    double d;
    Cell() : i(0) {}
};

struct Val {
    int ty;
    long long i;
    double d;
    Val() : ty(TY_INT), i(0), d(0) {}
};

struct ReturnEx { Val v; };
struct BreakEx {};
struct ContinueEx {};

static bool isTrueV(const Val& v) { return (v.ty == TY_DBL || v.ty == TY_FLOAT) ? v.d != 0.0 : v.i != 0; }
static Val mkIntV(long long v) { Val x; x.ty = TY_INT; x.i = v; x.d = (double)v; return x; }
static Val mkDblV(double v) { Val x; x.ty = TY_DBL; x.d = v; x.i = (long long)v; return x; }
static Val mkBoolV(long long v) { Val x; x.ty = TY_BOOL; x.i = v ? 1 : 0; x.d = (double)x.i; return x; }
static Val mkFnV(long long idx) { Val x; x.ty = TY_FN; x.i = idx; x.d = 0; return x; }
static Val mkStrV(long long idx) { Val x; x.ty = TY_STR; x.i = idx; x.d = 0; return x; }

// 把 v 转成类型 ty（ty 是类型表下标；ty < 0 表示不转换）
static Val convV(const Val& v, int ty) {
    if (ty < 0) return v;
    int k = gTypes.kindOf(ty);
    if (v.ty == TY_STR && k != TY_STR) throw Err("str 不能转成数值（字符串不可当整数用）");
    bool vf = (v.ty == TY_DBL || v.ty == TY_FLOAT);
    if (k == TY_DBL) return mkDblV(vf ? v.d : (double)v.i);
    if (k == TY_FLOAT) { float f = (float)(vf ? v.d : (double)v.i); return mkDblV((double)f); }
    if (k == TY_INT) return mkIntV((long long)(int32_t)(vf ? (long long)v.d : v.i));
    if (k == TY_CHAR) return mkIntV((long long)(unsigned char)(vf ? (long long)v.d : v.i));
    if (k == TY_BOOL) return mkBoolV(isTrueV(v) ? 1 : 0);
    if (k == TY_FN) return mkFnV(v.i);
    if (k == TY_HANDLE) return mkIntV(v.i);        // 不透明指针值：不截断、不参与算术
    if (k == TY_STR) {
        if (v.ty != TY_STR) throw Err("不能把非字符串值转换为 str");
        return v;
    }
    return v;
}

struct Interp {
    int annCurFn = -1;
    int annLoopDepth = 0;                     // 正在标注的匿名函数下标（用于推断返回类型）
    vector<Cell> mem;
    map<string, VarInfo> globals;          // 静态作用域（顶层 + O{} 体）
    vector<map<string, VarInfo> > frames;  // 栈帧：name -> {起始单元, 元素数, 类型}
    vector<int> frameBases;                // 每个栈帧的基址（栈向低地址增长）
    vector<int> retTyStack;                // 每层函数的返回类型（-1 = 不转换）
    vector<Func> funcs;
    map<string, int> fnByName;
    int stackTop;                      // 栈向下增长
    bool mapping;

    bool warnAlias = false;        // --walias：显式地址区间重叠时给提示
    int memSize;                   // 本次运行的地址空间大小（单元数）
    Interp(int n = MEM_SIZE)
        : memSize(n), mem(n), stackTop(n), mapping(false), nextFree(0), staticBase(0) {}

    // ---- 布局：静态区 ----
    int nextFree;
    int staticBase;                    // 静态区上限
    vector<pair<string, string> > mapLines;

    // ---- 可观察性：--map --json / --mem / --trace ----
    struct MapEntry {
        string name, type;
        int addr, elems, cells;
        bool hasFence; int fx, fy;
        MapEntry() : addr(0), elems(1), cells(1), hasFence(false), fx(0), fy(0) {}
    };
    vector<MapEntry> mapEntries;
    bool tracing = false;
    long long traceSteps = 0;
    vector<int> traceWrites;
    vector<int> cellTy;              // 每个单元的类型（--mem 用）
    vector<string> cellName;         // 每个单元属于哪个变量/字段

    void markWrite(int a) { if (tracing) traceWrites.push_back(a); }
    void execInner(SP s);
    void traceStep(SP s);
    static const char* stmtName(int k);
    void buildCellInfo();
    void dumpMem(FILE* out, bool all);
    void dumpMapJson(FILE* out);

    int allocStatic(int size, bool hasSpec, int x, int y, int line) {
        if (hasSpec) {
            // 注意：size 是「单元数」= 元素数 * sizeof(T)，不能用 y-x+1
            if (x < 0 || y < x || x + size > memSize) throw Err("地址区间越界", line);
            int end = x + size;
            if (end > nextFree) nextFree = end;
            return x;
        }
        int a = nextFree;
        if (a + size > memSize) throw Err("静态区溢出", line);
        nextFree += size;
        return a;
    }

    void layoutDecl(DP d, bool inFunction, int& frameOfs) {
        if (d->isType) return;                       // struct 类型声明不占单元
        int el = gTypes.cellsOf(d->ty);               // sizeof(T)
        if (d->isFn) {
            Func f;
            f.name = d->name;
            f.retTy = d->ty;
            f.params = d->params;
            f.paramTy = d->paramTy;
            f.paramRef = d->paramRef;
            f.paramElems = d->paramElems;
            f.body = d->body;
            int ofs = 0;
            for (size_t i = 0; i < d->params.size(); ++i) {
                f.paramOfs.push_back(ofs);
                bool rf = (i < d->paramRef.size() && d->paramRef[i]);
                ofs += rf ? 1 : gTypes.cellsOf(d->paramTy[i]);   // 引用参数只占 1 槽（存地址）
            }
            layoutBody(d->body, true, ofs);
            f.frameSize = ofs;
            d->frameSize = ofs;
            d->fnIdx = (int)funcs.size();
            funcs.push_back(f);
            fnByName[d->name] = d->fnIdx;
            return;
        }
        d->size = d->elems * el;
        if (d->hasFence) {
            if (inFunction) throw Err("函数体内不允许显式地址 " + d->name, d->line);
            d->addr = allocStatic(d->size, true, d->x, d->y, d->line);
        } else {
            if (inFunction) {
                d->addr = frameOfs;                  // 帧内偏移
                frameOfs += d->size;
            } else {
                d->addr = allocStatic(d->size, false, 0, 0, d->line);
            }
        }
        if (!inFunction) {
            VarInfo vi;
            vi.addr = d->addr; vi.elems = d->elems; vi.ty = d->ty;
            globals[d->name] = vi;
            char mb[128];
            snprintf(mb, sizeof mb, "%4d  %-8s x%-3d size=%d  %s", d->addr, d->type.c_str(), d->elems, d->size, d->name.c_str());
            mapLines.push_back(make_pair(d->name, string(mb)));
            if (warnAlias && d->hasFence) {           // §12.4：允许别名，默认静默，--walias 才提示
                for (size_t k = 0; k < mapEntries.size(); ++k) {
                    const MapEntry& e = mapEntries[k];
                    if (!(d->addr < e.addr + e.cells && e.addr < d->addr + d->size)) continue;
                    bool accidental = !e.hasFence;    // 自动分配的地址是编译器选的，重叠几乎必是笔误
                    fprintf(stderr, "warning: 地址区间 %s[%d..%d] 与 %s[%d..%d] 重叠（别名）%s\n",
                            d->name.c_str(), d->addr, d->addr + d->size - 1,
                            e.name.c_str(), e.addr, e.addr + e.cells - 1,
                            accidental ? "—— 显式区间压到了自动分配的变量上" : "");
                }
            }
            MapEntry me;
            me.name = d->name; me.type = d->type; me.addr = d->addr;
            me.elems = d->elems; me.cells = d->size;
            me.hasFence = d->hasFence; me.fx = d->x; me.fy = d->y;
            mapEntries.push_back(me);
        }
    }

    void layoutBody(vector<SP>& body, bool inFunction, int& frameOfs) {
        for (size_t i = 0; i < body.size(); ++i) layoutStmt(body[i], inFunction, frameOfs);
    }

    void layoutStmt(SP s, bool inFunction, int& frameOfs) {
        if (s->k == Stmt::DeclS) { layoutDecl(s->decl, inFunction, frameOfs); return; }
        if (s->k == Stmt::IfS || s->k == Stmt::WhileS) { layoutBody(s->body, inFunction, frameOfs); return; }
    }

    void layoutAnon(EP e) {
        int ofs = 0;
        for (size_t i = 0; i < e->params.size(); ++i) {
            bool rf = (i < e->paramRef.size() && e->paramRef[i]);
            ofs += rf ? 1 : gTypes.cellsOf(e->paramTy[i]);      // 引用参数只占 1 槽
        }
        layoutBody(e->body, true, ofs);
        e->frameSize = ofs;
        Func f;
        f.name = "<anon>";
        f.retTy = -1;                 // 匿名函数不转换返回值
        f.params = e->params;
        f.paramTy = e->paramTy;
        f.paramRef = e->paramRef;
        f.paramElems = e->paramElems;
        f.body = e->body;
        int po = 0;
        for (size_t i = 0; i < e->params.size(); ++i) {
            f.paramOfs.push_back(po);
            bool rf = (i < e->paramRef.size() && e->paramRef[i]);
            po += rf ? 1 : gTypes.cellsOf(e->paramTy[i]);
        }
        f.frameSize = ofs;
        e->fnIdx = (int)funcs.size();
        funcs.push_back(f);
    }

    // ---- 作用域查找 ----
    VarInfo* findVar(const string& nm) {
        if (!frames.empty()) {                     // 只查当前帧：词法作用域，不能看调用者的局部变量
            map<string, VarInfo>::iterator fit = frames.back().find(nm);
            if (fit != frames.back().end()) return &fit->second;
        }
        map<string, VarInfo>::iterator g = globals.find(nm);
        if (g != globals.end()) return &g->second;
        return 0;
    }

    Val readCell(int addr, int ty, int line) {
        if (addr < 0 || addr >= memSize) throw Err(fmt("运行时错误：地址 %lld 越界", addr), line);
        int k = gTypes.kindOf(ty);
        if (k == TY_STR) return mkStrV(mem[addr].i);
        if (k == TY_FN) return mkFnV(mem[addr].i);
        if (k == TY_DBL || k == TY_FLOAT) return mkDblV(mem[addr].d);
        return mkIntV(mem[addr].i);
    }

    void writeCell(int addr, int ty, const Val& v, int line) {
        markWrite(addr);
        if (addr < 0 || addr >= memSize) throw Err(fmt("运行时错误：地址 %lld 越界", addr), line);
        Val c = convV(v, ty);
        int k = gTypes.kindOf(ty);
        if (k == TY_DBL || k == TY_FLOAT) mem[addr].d = c.d;
        else mem[addr].i = c.i;
    }

    Val eval(EP e);

    Val evalRefValue(EP e) {
        // ${f}{x, y}：fn 变量的间接调用。必须在 resolveRef 之前判断，否则会被当成数组下标
        VarInfo* fv = findVar(e->s);
        if (fv && gTypes.kindOf(fv->ty) == TY_FN && !e->kids.empty()) {
            long long idx = mem[fv->addr].i;
            if (idx < 0) throw Err("fn 未初始化（还不能调用）", e->line);
            if (idx >= (long long)funcs.size()) throw Err("间接调用的函数值非法", e->line);
            return callFunc((int)idx, e->kids, e->line);
        }
        int addr = 0, ty = gTypes.iInt;
        VarInfo* vi = resolveRef(e, addr, ty);
        if (!vi) throw Err("未定义的变量 '" + e->s + "'", e->line);
        if (e->method == "size") {
            // ${a}.size{} = 数组长度；${a}{i}.size{} / ${p}.f.size{} = 1（单元素）
            bool single = !e->kids.empty() || !e->fields.empty();
            return mkIntV(single ? 1 : vi->elems);
        }
        if (e->method == "len") {
            Val v0 = readCell(addr, ty, e->line);
            if (v0.ty != TY_STR) throw Err(".len{} 只能用于 str", e->line);
            return mkIntV((long long)strAt(v0.i).size());
        }
        Val v = readCell(addr, ty, e->line);
        if (e->hasCast) {
            if (v.ty == TY_STR && gTypes.kindOf(e->castTy) != TY_STR)
                throw Err("str 不能转成数值（字符串不可当整数用）", e->line);
            return convV(v, e->castTy);
        }
        return v;
    }

    // 解析引用 → 起始单元 + 最终类型；返回变量信息（供 .size{} 用）
    VarInfo* resolveRef(EP e, int& addr, int& ty) {
        VarInfo* vi = findVar(e->s);
        if (!vi) return 0;
        addr = vi->addr;
        ty = vi->ty;
        int elems = vi->elems;
        if (vi->isRef) {                       // 引用参数：槽里存的是真地址
            long long a = mem[addr].i;
            if (a < 0 || a >= memSize) throw Err("引用参数指向的地址非法", e->line);
            addr = (int)a;
        }
        if (!e->kids.empty()) {
            Val iv = eval(e->kids[0]);
            long long idx = (iv.ty == TY_DBL || iv.ty == TY_FLOAT) ? (long long)iv.d : iv.i;
            if (idx < 0 || idx >= elems) {
                char b[128];
                snprintf(b, sizeof b, "下标 %lld 越界（长度 %lld）", idx, (long long)elems);
                throw Err(string(b), e->line);
            }
            addr = addr + (int)idx * gTypes.cellsOf(ty);
        }
        for (size_t i = 0; i < e->fields.size(); ++i) {
            const TypeInfo& ti = gTypes.at(ty);
            if (ti.kind != TY_STRUCT) throw Err("不是结构体，没有字段 ." + e->fields[i], e->line);
            map<string, int>::const_iterator ft = ti.fieldType.find(e->fields[i]);
            if (ft == ti.fieldType.end()) throw Err("结构体 " + ti.name + " 没有字段 ." + e->fields[i], e->line);
            addr += ti.fieldOfs.find(e->fields[i])->second;
            ty = ft->second;
        }
        return vi;
    }

    void storeRef(EP e, const Val& v) {
        int addr = 0, ty = gTypes.iInt;
        VarInfo* vi = resolveRef(e, addr, ty);
        if (!vi) throw Err("未定义的变量 '" + e->s + "'", e->line);
        if (e->hasCast) throw Err("不能给 .as{} 的结果赋值", e->line);
        writeCell(addr, ty, v, e->line);
    }

    // 引用参数：取实参的地址（实参必须是一个变量）
    int argAddr(EP e, int* elemsOut = 0) {
        if (e->k != Expr::RefE) throw Err("引用参数必须传变量（例如 ${x}）", e->line);
        int addr = 0, ty = gTypes.iInt;
        VarInfo* vi = resolveRef(e, addr, ty);
        if (!vi) throw Err("未定义的变量 '" + e->s + "'", e->line);
        if (elemsOut) *elemsOut = vi->elems;
        return addr;
    }

    Val callDirect(const string& nm, vector<EP>& args, int line) {
        map<string, int>::iterator it = fnByName.find(nm);
        if (it != fnByName.end()) return callFunc(it->second, args, line);

        NativeSig* ns = findNative(nm);
        if (!ns) throw Err("未定义的函数 '" + nm + "'", line);
        if (args.size() != ns->ptypes.size()) throw Err("函数 '" + nm + "' 参数个数不对", line);
        vector<Val> vals;
        for (size_t i = 0; i < args.size(); ++i) {
            bool rf = (i < ns->pref.size() && ns->pref[i]);
            if (rf) vals.push_back(mkIntV(argAddr(args[i])));          // 引用参数传地址
            else vals.push_back(convV(eval(args[i]), ns->ptypes[i]));
        }
        return callNative(nm, vals, line);
    }

    vector<FILE*> files;

    FILE* fh(long long h) {
        if (h < 0 || h >= (long long)files.size()) return 0;
        return files[(size_t)h];
    }

    const string& strAt(long long i) {
        if (i < 0 || i >= (long long)gStrPool.size()) throw Err("内部错误：字符串索引越界");
        return gStrPool[(size_t)i];
    }

    Val callNative(const string& nm, const vector<Val>& a, int line) {
        map<string, NativeSig>::iterator xi = gNativeTab.find(nm);
        if (xi != gNativeTab.end() && xi->second.isExtern)
            throw Err("extern 函数 '" + nm + "' 只能在 --cc（C 后端）下调用，解释器不支持", line);
        if (nm == "len")  return mkIntV((long long)strAt(a[0].i).size());
        if (nm == "find") {
            size_t p = strAt(a[0].i).find(strAt(a[1].i));
            return mkIntV(p == string::npos ? -1 : (long long)p);
        }
        if (nm == "substr") {
            const string& src = strAt(a[0].i);
            long long from = a[1].i, n = a[2].i;
            if (from < 0) from = 0;
            if (from > (long long)src.size()) from = (long long)src.size();
            if (n < 0) n = 0;
            if (from + n > (long long)src.size()) n = (long long)src.size() - from;
            return mkStrV((long long)internStr(src.substr((size_t)from, (size_t)n)));
        }
        if (nm == "charat") {
            const string& src = strAt(a[0].i);
            long long i = a[1].i;
            if (i < 0 || i >= (long long)src.size()) return mkIntV(-1);
            return mkIntV((unsigned char)src[(size_t)i]);
        }
        if (nm == "system") {
            int r = system(strAt(a[0].i).c_str());
#ifndef _WIN32
            if (r != -1 && WIFEXITED(r)) r = WEXITSTATUS(r);   // POSIX: system() 返回 wait 状态，取退出码
#endif
            return mkIntV(r);
        }
        if (nm == "open") {
            FILE* f = fopen(strAt(a[0].i).c_str(), strAt(a[1].i).c_str());
            if (!f) return mkIntV(-1);
            for (size_t i = 0; i < files.size(); ++i) {      // 优先复用空槽
                if (!files[i]) { files[i] = f; return mkIntV((long long)i); }
            }
            files.push_back(f);
            return mkIntV((long long)files.size() - 1);
        }
        if (nm == "close") {
            FILE* f = fh(a[0].i);
            if (!f) return mkIntV(-1);
            fclose(f);
            files[(size_t)a[0].i] = 0;
            return mkIntV(0);
        }
        if (nm == "getc") {
            FILE* f = fh(a[0].i);
            if (!f) return mkIntV(-1);
            int c = fgetc(f);
            return mkIntV(c == EOF ? -1 : (long long)(unsigned char)c);
        }
        if (nm == "putc") {
            FILE* f = fh(a[0].i);
            if (!f) return mkIntV(-1);
            return mkIntV(fputc((int)a[1].i, f) == EOF ? -1 : 0);
        }
        if (nm == "puts") {
            FILE* f = fh(a[0].i);
            if (!f) return mkIntV(-1);
            return mkIntV(fputs(strAt(a[1].i).c_str(), f) < 0 ? -1 : 0);
        }
        if (nm == "eof") {
            FILE* f = fh(a[0].i);
            return mkIntV(!f || feof(f) ? 1 : 0);
        }
        if (nm == "seek") {
            FILE* f = fh(a[0].i);
            return mkIntV(f ? fseek(f, (long)a[1].i, SEEK_SET) : -1);
        }
        if (nm == "tell") {
            FILE* f = fh(a[0].i);
            return mkIntV(f ? ftell(f) : -1);
        }
        if (nm == "read") {                        // read{h, &buf, n}：按字节读进单元
            FILE* f = fh(a[0].i);
            if (!f) return mkIntV(-1);
            int addr = (int)a[1].i, n = (int)a[2].i;
            if (addr < 0 || n < 0 || addr + n > memSize) throw Err("read 的缓冲越界", line);
            int k = 0;
            for (; k < n; ++k) {
                int c = fgetc(f);
                if (c == EOF) break;
                markWrite(addr + k); mem[addr + k].i = (long long)(unsigned char)c;
            }
            return mkIntV(k);
        }
        if (nm == "write") {                       // write{h, &buf, n}
            FILE* f = fh(a[0].i);
            if (!f) return mkIntV(-1);
            int addr = (int)a[1].i, n = (int)a[2].i;
            if (addr < 0 || n < 0 || addr + n > memSize) throw Err("write 的缓冲越界", line);
            int k = 0;
            for (; k < n; ++k) {
                if (fputc((int)(unsigned char)mem[addr + k].i, f) == EOF) break;
            }
            return mkIntV(k);
        }
        if (nm == "remove") return mkIntV(remove(strAt(a[0].i).c_str()));
        if (nm == "exists") {
            FILE* f = fopen(strAt(a[0].i).c_str(), "r");
            if (!f) return mkIntV(0);
            fclose(f);
            return mkIntV(1);
        }
        if (nm == "rename") return mkIntV(rename(strAt(a[0].i).c_str(), strAt(a[1].i).c_str()));
        (void)line;
        throw Err("内部错误：未实现的库函数 " + nm);
    }

    Val callFunc(int idx, vector<EP>& args, int line) {
        Func& f = funcs[idx];
        if (args.size() != f.params.size())
            throw Err(fmt("函数参数个数不对（%lld）", (long long)args.size()), line);

        // 先求实参，再压帧：实参本身可能又调用函数
        vector<Val> vals;
        for (size_t i = 0; i < args.size(); ++i) {
            bool rf = (i < f.paramRef.size() && f.paramRef[i]);
            if (rf) {
                int got = 0;
                int ad = argAddr(args[i], &got);
                if (i < f.paramElems.size() && f.paramElems[i] > got)
                    throw Err("引用参数的区段太小：形参声明 " + fmt("%lld", f.paramElems[i]) +
                              " 个，实参只有 " + fmt("%lld", got) + " 个", line);
                vals.push_back(mkIntV(ad));
            } else {
                vals.push_back(eval(args[i]));
            }
        }

        if (stackTop - f.frameSize < staticBase)
            throw Err("栈溢出（静态区已占 " + fmt("%lld", staticBase) + " 个单元）", line);
        int base = stackTop - f.frameSize;
        stackTop = base;
        frameBases.push_back(base);

        map<string, VarInfo> fr;
        for (size_t i = 0; i < f.params.size(); ++i) {
            bool rf = (i < f.paramRef.size() && f.paramRef[i]);
            VarInfo vi;
            vi.addr = base + f.paramOfs[i];
            vi.ty = f.paramTy[i];
            vi.isRef = rf;
            vi.elems = rf ? f.paramElems[i] : 1;
            fr[f.params[i]] = vi;
            if (rf) { markWrite(vi.addr); mem[vi.addr].i = vals[i].i; }                       // 槽里放地址（int64，不走 convV 的 int32 截断）
            else writeCell(vi.addr, vi.ty, vals[i], line);
        }
        frames.push_back(fr);
        retTyStack.push_back(f.retTy);

        Val ret;
        bool returned = false;
        try {
            execBody(f.body);
        } catch (ReturnEx& r) {
            ret = r.v;
            returned = true;
        } catch (BreakEx&) {
            // ! 不能跨函数边界（SPEC §7.4）：清理帧后报错，而不是去打断调用者的循环
            retTyStack.pop_back();
            frames.pop_back();
            frameBases.pop_back();
            stackTop = base + f.frameSize;
            throw Err("'!' 只能在循环内使用（不能跨函数）", line);
        }
        retTyStack.pop_back();
        frames.pop_back();
        frameBases.pop_back();
        stackTop = base + f.frameSize;
        if (!returned)
            throw Err("函数 '" + f.name + "' 执行到末尾却没有 ?{…} 返回值", line);
        return ret;
    }

    // ---------------------------------------------------------------- 类型标注（C 后端用）
    // 解释器是动态的，不需要类型；C 后端需要每个表达式的静态类型来决定取 .i 还是 .d。
    map<string, VarInfo> annLoc;

    static bool isFloatTy(int t) { int k = gTypes.kindOf(t); return k == TY_DBL || k == TY_FLOAT; }
    static int promoteTy(int a, int b) {
        if (isFloatTy(a) || isFloatTy(b)) return gTypes.iDbl;
        return gTypes.iInt;
    }

    VarInfo* annVar(const string& nm) {
        map<string, VarInfo>::iterator it = annLoc.find(nm);
        if (it != annLoc.end()) return &it->second;
        map<string, VarInfo>::iterator g = globals.find(nm);
        if (g != globals.end()) return &g->second;
        return 0;
    }

    int annTy(EP e);
    void annBody(const vector<SP>& body);
    void annStmt(SP s);

    void annLocals(const vector<SP>& body) {
        for (size_t i = 0; i < body.size(); ++i) {
            SP s = body[i];
            if (s->k == Stmt::DeclS && !s->decl->isFn && !s->decl->isType) {
                VarInfo vi;
                vi.addr = s->decl->addr; vi.elems = s->decl->elems; vi.ty = s->decl->ty;
                annLoc[s->decl->name] = vi;
            }
            if (s->k == Stmt::IfS || s->k == Stmt::WhileS) { annLocals(s->body); annLocals(s->elseBody); }
        }
    }

    // 间接调用的静态结果类型：有浮点候选就提升为 double（否则丢精度）
    int indRetTy(size_t argc) {
        int t = -1;
        for (size_t i = 0; i < funcs.size(); ++i) {
            if (funcs[i].params.size() != argc) continue;
            int rt = funcs[i].retTy < 0 ? gTypes.iInt : funcs[i].retTy;
            if (t < 0) t = rt;
            else if (isFloatTy(t) != isFloatTy(rt)) t = gTypes.iDbl;
        }
        return t < 0 ? gTypes.iInt : t;
    }

    int annFuncRet(const string& nm, int line) {        map<string, int>::iterator it = fnByName.find(nm);
        if (it == fnByName.end()) throw Err("未定义的函数 '" + nm + "'", line);
        int rt = funcs[it->second].retTy;
        return rt < 0 ? gTypes.iInt : rt;
    }

    void annArgs(const vector<EP>& args, const vector<int>& tys, int line) {
        if (args.size() != tys.size()) throw Err("函数参数个数不对", line);
        for (size_t i = 0; i < args.size(); ++i) annTy(args[i]);
    }

    void annotateAll(const vector<DP>& topDecls, const vector<EP>& anonFns, const vector<SP>& mainBody) {
        // 先标匿名函数：要把 ?{} 的返回类型推出来，后面调用点才知道该按什么类型派发
        for (size_t i = 0; i < anonFns.size(); ++i) {
            EP e = anonFns[i];
            if (e->fnIdx < 0) continue;
            const Func& f = funcs[e->fnIdx];
            annCurFn = e->fnIdx;
            annLoc.clear();
            for (size_t k = 0; k < e->params.size(); ++k) {
                VarInfo vi;
                vi.addr = f.paramOfs[k];
                vi.ty = f.paramTy[k];
                vi.isRef = (k < f.paramRef.size() && f.paramRef[k]) != 0;
                vi.elems = vi.isRef ? f.paramElems[k] : 1;
                annLoc[e->params[k]] = vi;
            }
            annLocals(e->body);
            annBody(e->body);
            annCurFn = -1;
        }
        for (size_t i = 0; i < topDecls.size(); ++i) {
            DP d = topDecls[i];
            if (!d->isFn) continue;
            const Func& f = funcs[d->fnIdx];
            annLoc.clear();
            for (size_t k = 0; k < d->params.size(); ++k) {
                VarInfo vi;
                vi.addr = f.paramOfs[k];
                vi.ty = f.paramTy[k];
                vi.isRef = (k < f.paramRef.size() && f.paramRef[k]) != 0;
                vi.elems = vi.isRef ? f.paramElems[k] : 1;
                annLoc[d->params[k]] = vi;
            }
            annLocals(d->body);
            annBody(d->body);
        }
        annLoc.clear();
        annBody(mainBody);
    }

    void execBody(vector<SP>& body) {
        for (size_t i = 0; i < body.size(); ++i) exec(body[i]);
    }

    void exec(SP s);
};

// ---------------------------------------------------------------- 求值
Val Interp::eval(EP e) {
    switch (e->k) {
    case Expr::Num:
        if (e->isFlt) return mkDblV(e->fval);
        if (e->litTy == gTypes.iBool) return mkBoolV(e->num ? 1 : 0);
        return mkIntV(e->num);
    case Expr::StrE: return mkStrV(e->num);
    case Expr::ListE: throw Err("列表 {a;b;…} 只能用于数组初始化", e->line);
    case Expr::UnitE: return eval(e->a);
    case Expr::RefE: return evalRefValue(e);
    case Expr::CallE: return callDirect(e->s, e->kids, e->line);
    case Expr::FnE:
        if (e->fnIdx < 0) throw Err("内部错误：匿名函数未布局", e->line);
        return mkFnV(e->fnIdx);
    case Expr::PipeE: {
        vector<EP> one;
        one.push_back(e->a);
        if (e->b) {                        // x | ${f}：间接调用
            Val fv = eval(e->b);
            if (fv.i < 0) throw Err("fn 未初始化（还不能调用）", e->line);
            if (fv.i >= (long long)funcs.size()) throw Err("间接调用的函数值非法", e->line);
            return callFunc((int)fv.i, one, e->line);
        }
        return callDirect(e->s, one, e->line);
    }
    case Expr::UnE: {
        Val v = eval(e->a);
        if (e->op == T_MINUS) {
            if (v.ty == TY_DBL) return mkDblV(-v.d);
            return mkIntV((long long)(int32_t)(-v.i));
        }
        if (e->op == T_TILDE || e->op == T_BANG) return mkBoolV(isTrueV(v) ? 0 : 1);
        return v;                          // ^ 显式解引用：无操作
    }
    case Expr::BinE: {
        if (e->op == T_ASSIGN) {
            Val v = eval(e->b);
            storeRef(e->a, v);
            return v;
        }
        Val l = eval(e->a);
        if (e->op == T_AND) return mkBoolV(isTrueV(l) && isTrueV(eval(e->b)) ? 1 : 0);
        if (e->op == T_OR) return mkBoolV(isTrueV(l) || isTrueV(eval(e->b)) ? 1 : 0);
        Val r = eval(e->b);

        if (l.ty == TY_HANDLE || r.ty == TY_HANDLE) {
            if (e->op != T_EQ && e->op != T_NE) throw Err("handle 只能比较 == / !=", e->line);
            bool eq = (l.i == r.i);
            return mkBoolV(e->op == T_EQ ? (eq ? 1 : 0) : (eq ? 0 : 1));
        }
        if (e->op == T_AMP || e->op == T_PIPE || e->op == T_CARET || e->op == T_SHL || e->op == T_SHR) {
            uint32_t a = (uint32_t)(int32_t)l.i, b = (uint32_t)(int32_t)r.i, x = 0;
            if (e->op == T_AMP) x = a & b;
            else if (e->op == T_PIPE) x = a | b;
            else if (e->op == T_CARET) x = a ^ b;
            else if (e->op == T_SHL) x = a << (b & 31);
            else x = a >> (b & 31);
            return mkIntV((long long)(int32_t)x);
        }
        // 字符串：按内容比较相等性；+ 为拼接
        if (l.ty == TY_STR || r.ty == TY_STR) {
            if (l.ty != TY_STR || r.ty != TY_STR) throw Err("str 不能与非字符串比较", e->line);
            if (e->op == T_PLUS) return mkStrV((long long)internStr(strAt(l.i) + strAt(r.i)));
            if (e->op != T_EQ && e->op != T_NE) throw Err("str 只能比较 == / != 或拼接 +", e->line);
            bool eq = (strAt(l.i) == strAt(r.i));
            return mkBoolV(e->op == T_EQ ? (eq ? 1 : 0) : (eq ? 0 : 1));
        }

        bool lf = (l.ty == TY_DBL || l.ty == TY_FLOAT);
        bool rf = (r.ty == TY_DBL || r.ty == TY_FLOAT);
        if (lf || rf) {                    // 浮点优先
            double a = lf ? l.d : (double)l.i;
            double b = rf ? r.d : (double)r.i;
            switch (e->op) {
            case T_PLUS:  return mkDblV(a + b);
            case T_MINUS: return mkDblV(a - b);
            case T_STAR:  return mkDblV(a * b);
            case T_SLASH: if (b == 0.0) throw Err("除以 0", e->line); return mkDblV(a / b);
            case T_PERCENT: throw Err("浮点不能取模（请先 .as{int}）", e->line);
            case T_EQ: return mkBoolV(a == b ? 1 : 0);
            case T_NE: return mkBoolV(a != b ? 1 : 0);
            case T_LT: return mkBoolV(a < b ? 1 : 0);
            case T_LE: return mkBoolV(a <= b ? 1 : 0);
            case T_GT: return mkBoolV(a > b ? 1 : 0);
            case T_GE: return mkBoolV(a >= b ? 1 : 0);
            default: throw Err("内部错误：未知运算符", e->line);
            }
        }
        long long a = l.i, b = r.i;
        switch (e->op) {
        case T_PLUS:  return mkIntV((long long)(int32_t)(a + b));
        case T_MINUS: return mkIntV((long long)(int32_t)(a - b));
        case T_STAR:  return mkIntV((long long)(int32_t)(a * b));
        case T_SLASH: if (!b) throw Err("除以 0", e->line); return mkIntV((long long)(int32_t)(a / b));
        case T_PERCENT: if (!b) throw Err("对 0 取模", e->line); return mkIntV((long long)(int32_t)(a % b));
        case T_EQ: return mkBoolV(a == b ? 1 : 0);
        case T_NE: return mkBoolV(a != b ? 1 : 0);
        case T_LT: return mkBoolV(a < b ? 1 : 0);
        case T_LE: return mkBoolV(a <= b ? 1 : 0);
        case T_GT: return mkBoolV(a > b ? 1 : 0);
        case T_GE: return mkBoolV(a >= b ? 1 : 0);
        default: throw Err("内部错误：未知运算符", e->line);
        }
    }
    }
    throw Err("内部错误：未知表达式", e->line);
}

// ---------------------------------------------------------------- 类型标注实现
int Interp::annTy(EP e) {
    switch (e->k) {
    case Expr::Num:
        return e->ty = (e->isFlt ? gTypes.iDbl : (e->litTy >= 0 ? e->litTy : gTypes.iInt));
    case Expr::StrE:
        return e->ty = gTypes.iStr;
    case Expr::UnitE:
        return e->ty = annTy(e->a);
    case Expr::ListE:
        for (size_t i = 0; i < e->kids.size(); ++i) annTy(e->kids[i]);
        return e->ty = gTypes.iInt;
    case Expr::FnE:
        return e->ty = gTypes.iFn;
    case Expr::RefE: {
        VarInfo* vi = annVar(e->s);
        if (!vi) throw Err("未定义的变量 '" + e->s + "'", e->line);
        int t = vi->ty;
        if (gTypes.kindOf(t) == TY_FN && !e->kids.empty()) {      // ${f}{x, y}：直接间接调用
            for (size_t i = 0; i < e->kids.size(); ++i) annTy(e->kids[i]);
            const TypeInfo& fi = gTypes.at(t);
            if (fi.hasSig) {
                if (fi.sigArgs.size() != e->kids.size())
                    throw Err("调用实参个数与 fn 签名不符", e->line);
                return e->ty = fi.sigRet;
            }
            return e->ty = indRetTy(e->kids.size());
        }
        if (!e->kids.empty()) annTy(e->kids[0]);
        if (e->kids.size() > 1)
            throw Err("下标只能一个；多参数调用只对 fn 类型有效", e->line);
        for (size_t i = 0; i < e->fields.size(); ++i) {
            const TypeInfo& ti = gTypes.at(t);
            if (ti.kind != TY_STRUCT) throw Err("不是结构体，没有字段 ." + e->fields[i], e->line);
            map<string, int>::const_iterator ft = ti.fieldType.find(e->fields[i]);
            if (ft == ti.fieldType.end()) throw Err("结构体没有字段 ." + e->fields[i], e->line);
            t = ft->second;
        }
        if (e->method == "size") return e->ty = gTypes.iInt;
        if (e->method == "len") {
            if (gTypes.kindOf(t) != TY_STR) throw Err(".len{} 只能用于 str", e->line);
            return e->ty = gTypes.iInt;
        }
        e->srcTy = t;
        if (e->hasCast) {
            if (gTypes.kindOf(t) == TY_STR && gTypes.kindOf(e->castTy) != TY_STR)
                throw Err("str 不能转成数值（字符串不可当整数用）", e->line);
            return e->ty = e->castTy;
        }
        return e->ty = t;
    }
    case Expr::CallE: {
        map<string, int>::iterator it = fnByName.find(e->s);
        if (it == fnByName.end()) {
            NativeSig* ns = findNative(e->s);
            if (!ns) throw Err("未定义的函数 '" + e->s + "'", e->line);
            if (e->kids.size() != ns->ptypes.size())
                throw Err("函数 '" + e->s + "' 参数个数不对", e->line);
            for (size_t i = 0; i < e->kids.size(); ++i) annTy(e->kids[i]);
            return e->ty = ns->ret;
        }
        for (size_t i = 0; i < e->kids.size(); ++i) annTy(e->kids[i]);
        {
            const Func& cf = funcs[it->second];
            for (size_t i = 0; i < e->kids.size() && i < cf.paramTy.size(); ++i) {
                const TypeInfo& pti = gTypes.at(cf.paramTy[i]);
                if (!pti.hasSig) continue;
                EP ag = e->kids[i];
                if (ag->k != Expr::FnE || ag->fnIdx < 0) continue;
                const Func& af = funcs[ag->fnIdx];
                if (af.params.size() != pti.sigArgs.size())      // fn 签名：检查参数个数
                    throw Err("传入的函数参数个数与 fn 签名不符（签名要 " + fmt("%lld", (long long)pti.sigArgs.size()) +
                              " 个，实际 " + fmt("%lld", (long long)af.params.size()) + " 个）", e->line);
                int ar = af.retTy < 0 ? gTypes.iInt : af.retTy;
                if (isFloatTy(ar) && !isFloatTy(pti.sigRet))
                    throw Err("传入的函数返回 double，但 fn 签名要求 int", e->line);
            }
        }
        int rt = funcs[it->second].retTy;
        return e->ty = (rt < 0 ? gTypes.iInt : rt);
    }
    case Expr::PipeE: {
        annTy(e->a);
        if (e->b) {                                                // x | ${f}：间接调用
            if (e->b->k == Expr::RefE && !e->b->kids.empty())
                throw Err("管道只能传 1 个参数；${f}{…} 不能写在管道右侧（改用 ${f}{x}）", e->line);
            int bt = annTy(e->b);
            if (gTypes.kindOf(bt) != TY_FN)                       // 管道右侧必须是函数值
                throw Err("管道右侧必须是 fn 类型（当前是 " + gTypes.at(bt).name + "）", e->line);
            const TypeInfo& fi = gTypes.at(bt);
            if (fi.hasSig) {                                       // 有签名就按签名定类型
                if (fi.sigArgs.size() != 1)
                    throw Err("管道只能传 1 个参数，与 fn 签名不符", e->line);
                return e->ty = fi.sigRet;
            }
            return e->ty = indRetTy(1);
        }
        map<string, int>::iterator it = fnByName.find(e->s);
        if (it == fnByName.end()) {
            NativeSig* ns = findNative(e->s);
            if (!ns) throw Err("未定义的函数 '" + e->s + "'", e->line);
            return e->ty = ns->ret;
        }
        return e->ty = annFuncRet(e->s, e->line);
    }
    case Expr::UnE: {
        int t = annTy(e->a);
        if (e->op == T_TILDE || e->op == T_BANG) return e->ty = gTypes.iBool;
        if (e->op == T_MINUS) return e->ty = (isFloatTy(t) ? gTypes.iDbl : gTypes.iInt);
        return e->ty = t;
    }
    case Expr::BinE: {
        if (e->op == T_ASSIGN) {
            int lt = annTy(e->a);
            annTy(e->b);
            const TypeInfo& lti = gTypes.at(lt);
            if (lti.hasSig && e->b->k == Expr::FnE && e->b->fnIdx >= 0) {   // 赋值给 fn 变量时验签名
                const Func& af = funcs[e->b->fnIdx];
                if (af.params.size() != lti.sigArgs.size())
                    throw Err("赋给 fn 变量的函数参数个数与签名不符（签名要 " +
                              fmt("%lld", (long long)lti.sigArgs.size()) + " 个，实际 " +
                              fmt("%lld", (long long)af.params.size()) + " 个）", e->line);
                int ar = af.retTy < 0 ? gTypes.iInt : af.retTy;
                if (isFloatTy(ar) && !isFloatTy(lti.sigRet))
                    throw Err("赋给 fn 变量的函数返回 double，但签名要求 int", e->line);
            }
            if (gTypes.kindOf(lt) == TY_FN && e->b->k == Expr::Num)
                throw Err("fn 变量不能用整数字面量赋值（写 {} 表示空函数）", e->line);
            return e->ty = (gTypes.kindOf(lt) == TY_FLOAT ? gTypes.iDbl : lt);
        }
        int l = annTy(e->a), r = annTy(e->b);
        if (gTypes.kindOf(l) == TY_HANDLE || gTypes.kindOf(r) == TY_HANDLE) {
            if (e->op == T_EQ || e->op == T_NE) return e->ty = gTypes.iBool;
            throw Err("handle 只能比较 == / !=（不可解引用、不可算术）", e->line);
        }
        if (gTypes.kindOf(l) == TY_STR || gTypes.kindOf(r) == TY_STR) {
            if (gTypes.kindOf(l) != TY_STR || gTypes.kindOf(r) != TY_STR)
                throw Err("str 不能与非字符串混合运算", e->line);
            if (e->op == T_PLUS) return e->ty = gTypes.iStr;
            if (e->op == T_EQ || e->op == T_NE) return e->ty = gTypes.iBool;
            throw Err("str 只能比较 == / != 或拼接 +", e->line);
        }
        if (e->op == T_AMP || e->op == T_PIPE || e->op == T_CARET || e->op == T_SHL || e->op == T_SHR) {
            if (isFloatTy(l) || isFloatTy(r)) throw Err("位运算只能用于整数", e->line);
            return e->ty = gTypes.iInt;
        }
        if (e->op == T_PERCENT && (isFloatTy(l) || isFloatTy(r)))    // 浮点取模：标注期就报，两端一致
            throw Err("浮点不能取模（请先 .as{int}）", e->line);
        switch (e->op) {
        case T_EQ: case T_NE: case T_LT: case T_LE: case T_GT: case T_GE:
        case T_AND: case T_OR:
            return e->ty = gTypes.iBool;
        default: break;
        }
        return e->ty = promoteTy(l, r);
    }
    }
    throw Err("内部错误：未知表达式", e->line);
}

void Interp::annBody(const vector<SP>& body) {
    for (size_t i = 0; i < body.size(); ++i) annStmt(body[i]);
}

void Interp::annStmt(SP s) {
    switch (s->k) {
    case Stmt::DeclS:
        if (s->decl->init) {
            // §6.2：引用不是一等值 —— &{…} = ${x} 必须报编译错误
            EP ini = s->decl->init;
            if (ini->k == Expr::RefE && !ini->hasCast && ini->method.empty())
                throw Err("不能把引用存起来：&{…} = ${x} 是编译错误（${…} 不是一等值）", s->line);
            int it = annTy(s->decl->init);
            int dt = gTypes.kindOf(s->decl->ty);
            int itk = gTypes.kindOf(it);
            if (dt == TY_STR && itk != TY_STR)
                throw Err("str 变量不能用非字符串值初始化", s->line);
            if (dt != TY_STR && itk == TY_STR)
                throw Err("非 str 变量不能用字符串字面量初始化", s->line);
            // 结构体不能整体拷贝：RefE/UnitE(RefE) 会静默得到全零，直接报错更诚实
            if (dt == TY_STRUCT) {
                EP r = ini;
                if (r->k == Expr::UnitE && r->a) r = r->a;
                if (r->k == Expr::RefE && r->fields.empty() && !r->hasCast)
                    throw Err("结构体不能整体赋值/拷贝，请逐字段赋值（p.x = q.x）", s->line);
            }
            // fn 变量不能用整数字面量初始化：那会被当成"函数下标"，等于凭空指向某个函数
            if (dt == TY_FN && s->decl->init->k == Expr::Num)
                throw Err("fn 变量不能用整数字面量初始化（写 {} 表示\"还没有函数\"）", s->line);
        }
        return;
    case Stmt::ExprS: annTy(s->e); return;
    case Stmt::IfS: annTy(s->e); annBody(s->body); annBody(s->elseBody); return;
    case Stmt::WhileS:
        annTy(s->e);
        annLoopDepth++;
        annBody(s->body);
        annLoopDepth--;
        return;
    case Stmt::RetS: {
        int t = annTy(s->e);
        if (annCurFn >= 0 && annCurFn < (int)funcs.size()) {   // 匿名函数：用 ?{} 推断返回类型
            Func& f = funcs[annCurFn];
            if (f.retTy < 0) f.retTy = t;
            else if (isFloatTy(f.retTy) != isFloatTy(t)) f.retTy = gTypes.iDbl;
        }
        return;
    }
    case Stmt::OutS:
        if (s->isFmt) {                                  // .{fmt; "…"; 实参…}
            if (s->args.empty() || s->args[0]->k != Expr::StrE)
                throw Err("fmt 的第一个参数必须是格式串字面量", s->line);
            int n = 0;
            splitFmt(s->args[0]->s, &n);
            if ((size_t)n != s->args.size() - 1)
                throw Err("格式串有 " + fmt("%lld", (long long)n) + " 个占位符，但给了 " +
                          fmt("%lld", (long long)s->args.size() - 1) + " 个实参", s->line);
            for (size_t i = 1; i < s->args.size(); ++i) annTy(s->args[i]);
            return;
        }
        if (s->ty < 0 && s->e) s->ty = annTy(s->e);       // print(e)：类型从表达式推断
        else annTy(s->e);
        return;
    case Stmt::InS: if (s->e) annTy(s->e); return;
    case Stmt::ContinueS:
        if (annLoopDepth == 0) throw Err("continue 只能在循环内使用", s->line);
        return;
    case Stmt::BreakS:
        if (annLoopDepth == 0)
            throw Err(annCurFn >= 0 ? "'!' 只能在循环内使用（不能跨函数）"
                                    : "'!' 只能在循环内使用", s->line);
        return;
    case Stmt::AssignS: return;
    }
}

// ---------------------------------------------------------------- 语句
void Interp::buildCellInfo() {
    cellTy.assign(memSize, gTypes.iInt);
    cellName.assign(memSize, string());
    for (map<string, VarInfo>::iterator it = globals.begin(); it != globals.end(); ++it) {
        const VarInfo& vi = it->second;
        const TypeInfo& T = gTypes.at(vi.ty);
        int stride = gTypes.cellsOf(vi.ty);
        for (int e = 0; e < vi.elems; ++e) {
            int base = vi.addr + e * stride;
            string ename = it->first;
            if (vi.elems > 1) { char b[32]; snprintf(b, sizeof b, "[%d]", e); ename += b; }
            if (T.kind == TY_STRUCT) {
                for (size_t f = 0; f < T.fieldOrder.size(); ++f) {
                    const string& fn = T.fieldOrder[f];
                    int ft = T.fieldType.find(fn)->second;
                    int fo = T.fieldOfs.find(fn)->second;
                    int cn = gTypes.cellsOf(ft);
                    for (int k = 0; k < cn; ++k) {
                        int a = base + fo + k;
                        if (a < 0 || a >= memSize) continue;
                        cellTy[a] = ft;
                        cellName[a] = ename + "." + fn;
                    }
                }
            } else {
                if (base < 0 || base >= memSize) continue;
                cellTy[base] = vi.ty;
                cellName[base] = ename;
            }
        }
    }
}

void Interp::dumpMem(FILE* out, bool all) {
    fprintf(out, "--- 内存 dump（退出时）---\n");
    fprintf(out, "MEM_SIZE=%d  静态区 0..%d  栈从 %d 向下  退出时 sp=%d\n",
            memSize, staticBase - 1, memSize - 1, stackTop);
    fprintf(out, " addr  类型     值                     名字\n");
    int shown = 0;
    for (int a = 0; a < memSize; ++a) {
        bool nz = mem[a].i != 0;
        if (!all && !nz) continue;
        int t = (a < (int)cellTy.size()) ? cellTy[a] : gTypes.iInt;
        const TypeInfo& T = gTypes.at(t);
        char val[64];
        if (gTypes.kindOf(t) == TY_DBL || gTypes.kindOf(t) == TY_FLOAT) snprintf(val, sizeof val, "%.17g", mem[a].d);
        else snprintf(val, sizeof val, "%lld", mem[a].i);
        fprintf(out, " %4d  %-8s %-21s  %s\n", a, T.name.c_str(), val,
                (a < (int)cellName.size()) ? cellName[a].c_str() : "");
        ++shown;
    }
    if (!all)
        fprintf(out, "（非零单元 %d / %d；--mem=all 显示全部）\n", shown, memSize);
    // 字符串池不在地址空间里（见 SPEC §5.5），但把内容一并打出来，透明性不留洞
    if (!gStrPool.empty()) {
        fprintf(out, "--- 字符串池（不在地址空间内，共 %d 条）---\n", (int)gStrPool.size());
        for (size_t i = 0; i < gStrPool.size(); ++i) {
            string q, e;
            const string& src = gStrPool[i];
            for (size_t k = 0; k < src.size(); ++k) {
                if (src[k] == '\n') e += "\\n";
                else if (src[k] == '\t') e += "\\t";
                else e += src[k];
            }
            if (e.size() > 48) e = e.substr(0, 45) + "...";
            fprintf(out, " %4d  \"%s\"\n", (int)i, e.c_str());
        }
    }
}

void Interp::dumpMapJson(FILE* out) {
    fprintf(out, "{\"mem_size\":%d,\"static_end\":%d,\"stack_top\":%d,\"entries\":[",
            memSize, staticBase, memSize - 1);
    for (size_t i = 0; i < mapEntries.size(); ++i) {
        const MapEntry& e = mapEntries[i];
        if (i) fputc(',', out);
        fprintf(out, "{\"name\":\"%s\",\"type\":\"%s\",\"addr\":%d,\"elems\":%d,\"cells\":%d",
                e.name.c_str(), e.type.c_str(), e.addr, e.elems, e.cells);
        if (e.hasFence) fprintf(out, ",\"fence\":[%d,%d]", e.fx, e.fy);
        fputc('}', out);
    }
    fprintf(out, "]}\n");
}

void Interp::exec(SP s) {
    if (!tracing) { execInner(s); return; }
    traceWrites.clear();
    execInner(s);
    traceStep(s);
}

const char* Interp::stmtName(int k) {
    switch (k) {
    case Stmt::DeclS:   return "decl";
    case Stmt::AssignS: return "assign";
    case Stmt::IfS:     return "if";
    case Stmt::WhileS:  return "while";
    case Stmt::RetS:    return "return";
    case Stmt::BreakS:  return "break";
    case Stmt::ContinueS: return "continue";
    case Stmt::OutS:    return "out";
    case Stmt::InS:     return "in";
    default:            return "expr";
    }
}

void Interp::traceStep(SP s) {
    fprintf(stderr, "[%6lld] line %-4d %-7s sp=%-5d", ++traceSteps, s->line, stmtName(s->k), stackTop);
    if (!traceWrites.empty()) {
        fprintf(stderr, " writes:");
        for (size_t i = 0; i < traceWrites.size(); ++i) {
            int a = traceWrites[i];
            if (a < 0 || a >= memSize) { fprintf(stderr, " %d=?", a); continue; }
            int t = (a < (int)cellTy.size()) ? cellTy[a] : gTypes.iInt;
            if (gTypes.kindOf(t) == TY_DBL || gTypes.kindOf(t) == TY_FLOAT) fprintf(stderr, " %d=%.17g", a, mem[a].d);
            else fprintf(stderr, " %d=%lld", a, mem[a].i);
        }
    }
    fprintf(stderr, "\n");
}

void Interp::execInner(SP s) {
    switch (s->k) {
    case Stmt::DeclS: {
        DP d = s->decl;
        if (d->isFn || d->isType) return;          // 函数与类型声明不产生运行时代码

        int addr;
        if (frames.empty()) {
            addr = d->addr;                        // 静态区：绝对地址
        } else {
            addr = frameBases.back() + d->addr;    // 帧内：基址 + 偏移
            VarInfo vi;
            vi.addr = addr;
            vi.elems = d->elems;
            vi.ty = d->ty;
            frames.back()[d->name] = vi;
        }
        if (d->init) {
            const TypeInfo& ti0 = gTypes.at(d->ty);
            if (ti0.kind == TY_STRUCT && d->elems == 1) {           // 结构体字面量：按字段声明顺序
                vector<EP> xs;
                if (d->init->k == Expr::ListE) xs = d->init->kids;
                else xs.push_back(d->init);                         // {v} 等价于 {v; 0; …}
                if (xs.size() > ti0.fieldOrder.size()) throw Err("结构体初始化字段过多", d->line);
                for (size_t i = 0; i < ti0.fieldOrder.size(); ++i) {
                    const string& fn = ti0.fieldOrder[i];
                    int ft = ti0.fieldType.find(fn)->second;
                    int fo = ti0.fieldOfs.find(fn)->second;
                    if (i < xs.size()) writeCell(addr + fo, ft, eval(xs[i]), d->line);
                    else for (int k = 0; k < gTypes.cellsOf(ft); ++k) { markWrite(addr + fo + k); mem[addr + fo + k].i = 0; }
                }
                return;
            }
            if (d->init->k == Expr::ListE) {                        // 数组字面量
                const vector<EP>& xs = d->init->kids;
                if ((int)xs.size() > d->elems) throw Err("初始化元素过多", d->line);
                int stride = gTypes.cellsOf(d->ty);
                for (size_t i = 0; i < xs.size(); ++i)
                    writeCell(addr + (int)i * stride, d->ty, eval(xs[i]), d->line);
                for (int i = (int)xs.size(); i < d->elems; ++i)
                    for (int k = 0; k < stride; ++k) { markWrite(addr + i * stride + k); mem[addr + i * stride + k].i = 0; }
            } else if (d->elems == 1 && gTypes.cellsOf(d->ty) == 1) {
                writeCell(addr, d->ty, eval(d->init), d->line);
            } else if (d->elems == 1) {
                throw Err("结构体暂不支持字面量初始化（请零初始化后逐字段赋值）", d->line);
            } else {
                throw Err("数组初始化请用 {v1;v2;…} 列表形式", d->line);
            }
        } else {
            long long z = (gTypes.kindOf(d->ty) == TY_FN) ? -1 : 0;   // fn 的"空"是 -1（未初始化）
            for (int k = 0; k < d->size; ++k) { markWrite(addr + k); mem[addr + k].i = z; }
        }
        return;
    }
    case Stmt::AssignS:
        throw Err("内部错误：赋值不该走到这里", s->line);
    case Stmt::ExprS:
        eval(s->e);
        return;
    case Stmt::IfS:
        if (isTrueV(eval(s->e))) execBody(s->body);
        else if (!s->elseBody.empty()) execBody(s->elseBody);
        return;
    case Stmt::WhileS:
        while (isTrueV(eval(s->e))) {
            try { execBody(s->body); }
            catch (ContinueEx&) { continue; }
            catch (BreakEx&) { break; }
        }
        return;
    case Stmt::RetS: {
        Val v = eval(s->e);
        int rt = retTyStack.empty() ? -1 : retTyStack.back();
        throw ReturnEx{ convV(v, rt) };
    }
    case Stmt::BreakS:
        throw BreakEx();
    case Stmt::ContinueS:
        throw ContinueEx();
    case Stmt::OutS: {
        if (s->isFmt) {                                    // .{fmt; "…"; 实参…}
            if (s->args.empty() || s->args[0]->k != Expr::StrE) throw Err("fmt 的第一个参数必须是格式串字面量", s->line);
            vector<FmtPiece> ps = splitFmt(s->args[0]->s, 0);
            size_t ai = 1;
            for (size_t i = 0; i < ps.size(); ++i) {
                if (!ps[i].isSpec) { fputs(ps[i].text.c_str(), stdout); continue; }
                if (ai >= s->args.size()) throw Err("格式串的实参不够", s->line);
                Val v = eval(s->args[ai++]);
                char c = ps[i].conv;
                if (c == 's') fputs(strAt(convV(v, gTypes.iStr).i).c_str(), stdout);
                else if (c == 'd') printf("%d", (int)convV(v, gTypes.iInt).i);
                else if (c == 'c') putchar((unsigned char)convV(v, gTypes.iChar).i);
                else if (c == 'x') printf("%x", (unsigned)convV(v, gTypes.iInt).i);
                else printf("%g", convV(v, gTypes.iDbl).d);
            }
            fflush(stdout);
            return;
        }
        if (s->elems > 1) throw Err("输出不支持 {x;y} 形式，请逐个输出", s->line);
        if (gTypes.kindOf(s->ty) == TY_STR) {               // 字符串：字面量或 str 变量
            if (s->e->k == Expr::StrE) fputs(s->e->s.c_str(), stdout);
            else fputs(strAt(eval(s->e).i).c_str(), stdout);
            fflush(stdout);
            return;
        }
        Val v = eval(s->e);
        switch (gTypes.kindOf(s->ty)) {
        case TY_INT:  printf("%d", (int)convV(v, gTypes.iInt).i); break;
        case TY_CHAR: putchar((unsigned char)convV(v, gTypes.iChar).i); break;
        case TY_BOOL: printf("%d", (int)convV(v, gTypes.iBool).i); break;
        case TY_DBL:
        case TY_FLOAT: printf("%g", convV(v, gTypes.iDbl).d); break;
        default: throw Err("不支持输出的类型 " + gTypes.at(s->ty).name, s->line);
        }
        fflush(stdout);
        return;
    }
    case Stmt::InS: {
        if (s->e->k != Expr::RefE) throw Err(",{T; …} 的参数必须是引用 ${x}", s->line);
        int addr = 0, ty = gTypes.iInt;
        VarInfo* vi = resolveRef(s->e, addr, ty);
        if (!vi) throw Err("未定义的变量 '" + s->e->s + "'", s->line);
        int stride = gTypes.cellsOf(ty);
        int kind = gTypes.kindOf(ty);
        if (kind == TY_STR) {
            string line;
            if (!getline(cin, line)) line = "";
            markWrite(addr);
            mem[addr].i = (long long)internStr(line);
            return;
        }
        for (int k = 0; k < s->elems; ++k) {
            int a = addr + k * stride;
            if (kind == TY_DBL || kind == TY_FLOAT) {
                double dv = 0;
                if (scanf("%lf", &dv) != 1) throw Err("输入失败（期望浮点数）", s->line);
                mem[a].d = convV(mkDblV(dv), ty).d;
            } else {
                int n = 0;
                if (scanf("%d", &n) != 1) throw Err("输入失败（期望整数）", s->line);
                mem[a].i = convV(mkIntV(n), ty).i;
            }
        }
        return;
    }
    }
}

// ---------------------------------------------------------------- C 后端（int 子集）
// 说明：支持 int / bool / char / str 字面量 / 函数 / 递归 / 循环 / 数组 / 别名。
// 不支持：double、float、struct、间接调用（函数值）——这些需要类型标注 pass，留给 v0.8。
struct CEmit {
    string o;
    map<string, VarInfo> loc;                    // 当前函数局部：name -> {帧内偏移, elems, ty}
    const map<string, VarInfo>* globals;
    const Interp* ip;                            // 供 callExpr 查参数类型
    int ind;
    string lend;
    string retCT;                                // 当前函数的返回 C 类型
    int curRetTy;                                // 当前函数的返回类型下标
    bool inMain;                                 // O{} 体里的 ?{} 直接 return
    int loopDepth;                               // 生成 break 前必须 > 0
    bool bad;
    string why;
    map<string,string> pvar;                     // 值参数 -> C 局部变量名（寄存器化）
    map<string,int> pty;                         // 值参数/局部变量 -> 类型下标
    const vector<SP>* curBody = 0;               // 当前函数体（局部变量逃逸分析用）
    vector<char> needFp;                         // 每个函数是否需要 _fp（有内存槽才需要）

    CEmit() : globals(0), ip(0), ind(0), curRetTy(-1), inMain(false), loopDepth(0), bad(false) {}

    static string fmtF(double v) {
        char b[64];
        snprintf(b, sizeof b, "%.17g", v);
        return string(b);
    }
    static string cTypeOf(int ty) { if (ty < 0) return "int64_t"; return isF(ty) ? "double" : "int64_t"; }
    static bool isF(int ty) { int k = gTypes.kindOf(ty); return k == TY_DBL || k == TY_FLOAT; }

    // 按目标类型生成存储用的转换（char 截到 8 位、bool 标准化 0/1、str 保留指针位、其余 32 位）
    static string storeCast(int ty, const string& e) {
        if (ty < 0) return "(int64_t)(" + e + ")";
        int k = gTypes.kindOf(ty);
        if (k == TY_DBL) return "(double)(" + e + ")";
        if (k == TY_FLOAT) return "(double)(float)(" + e + ")";   // float 要按 32 位舍入，与解释器一致
        if (k == TY_CHAR) return "(int64_t)(unsigned char)(" + e + ")";
        if (k == TY_STR) return "(int64_t)(" + e + ")";
        if (k == TY_HANDLE) return "(int64_t)(" + e + ")";
        if (k == TY_BOOL) return "(int64_t)((" + e + ") ? 1 : 0)";
        return "(int64_t)(int32_t)(" + e + ")";
    }

    void put(const string& s) { o += string((size_t)ind * 4, ' ') + s + "\n"; }

    static string esc(const string& s) {
        string r;
        for (size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            if (c == '"') r += "\\\"";
            else if (c == '\\') r += "\\\\";
            else if (c == '\n') r += "\\n";
            else if (c == '\t') r += "\\t";
            else r += c;
        }
        return r;
    }

    void fail(const string& w) { if (!bad) { bad = true; why = w; } }

    void collect(const vector<SP>& body) {
        for (size_t i = 0; i < body.size(); ++i) {
            SP s = body[i];
            if (s->k == Stmt::DeclS && !s->decl->isFn && !s->decl->isType) {
                VarInfo vi;
                vi.addr = s->decl->addr; vi.elems = s->decl->elems; vi.ty = s->decl->ty;
                loc[s->decl->name] = vi;
            }
            if (s->k == Stmt::IfS || s->k == Stmt::WhileS) { collect(s->body); collect(s->elseBody); }
        }
    }

    // 只算基址（不加下标/字段）——fn 变量的间接调用要用它，否则实参会被当成索引
    string addrBase(EP e, int& vty) {
        string a;
        map<string, VarInfo>::iterator li = loc.find(e->s);
        if (li != loc.end()) {
            if (li->second.isRef) a = "((int64_t)_mem[_fp + " + fmt("%lld", li->second.addr) + "].i)";
            else a = "(_fp + " + fmt("%lld", li->second.addr) + ")";
            vty = li->second.ty;
        } else {
            map<string, VarInfo>::const_iterator gi = globals->find(e->s);
            if (gi == globals->end()) { fail("未定义的变量 " + e->s); vty = gTypes.iInt; return "0"; }
            a = "(" + fmt("%lld", gi->second.addr) + ")";
            vty = gi->second.ty;
        }
        return a;
    }

    string addrIdx(EP e, string a, int& vty) {
        if (!e->kids.empty()) {
            int stride = gTypes.cellsOf(vty);
            a = "(" + a + " + (int)(" + asI(e->kids[0]) + ")" + (stride == 1 ? "" : " * " + fmt("%lld", stride)) + ")";
        }
        for (size_t i = 0; i < e->fields.size(); ++i) {
            const TypeInfo& ti = gTypes.at(vty);
            if (ti.kind != TY_STRUCT) { fail("不是结构体，没有字段 ." + e->fields[i]); return a; }
            map<string, int>::const_iterator ft = ti.fieldType.find(e->fields[i]);
            map<string, int>::const_iterator fo = ti.fieldOfs.find(e->fields[i]);
            if (ft == ti.fieldType.end()) { fail("结构体没有字段 ." + e->fields[i]); return a; }
            a = "(" + a + " + " + fmt("%lld", fo->second) + ")";
            vty = ft->second;
        }
        return a;
    }

    static bool plainRef(EP e) {
        return e->k == Expr::RefE && e->method.empty() && e->kids.empty()
               && e->fields.empty() && !e->hasCast;
    }

    // 参数是否可能被取地址：作为引用实参传出 / 传给 read、write 的缓冲地址 / 下标或字段访问
    bool escExpr(EP e, const string& nm) {
        if (!e) return false;
        if (e->k == Expr::RefE) {
            if (e->s != nm) return false;
            if (!e->kids.empty() || !e->fields.empty() || e->hasCast) return true;   // ${p}{i} / ${p}.f / ${p}.as{T} 走地址
            return false;
        }
        if (e->k == Expr::CallE) {
            NativeSig* xs = findNative(e->s);
            if (xs && xs->isExtern) {
                for (size_t i = 0; i < e->kids.size() && i < xs->pref.size(); ++i)
                    if (xs->pref[i] && plainRef(e->kids[i]) && e->kids[i]->s == nm) return true;
            }
            bool native = (e->s == "read" || e->s == "write");
            map<string,int>::const_iterator it = ip->fnByName.find(e->s);
            for (size_t i = 0; i < e->kids.size(); ++i) {
                if (native && plainRef(e->kids[i]) && e->kids[i]->s == nm) return true;
                if (it != ip->fnByName.end()) {
                    const Func& f = ip->funcs[it->second];
                    if (i < f.paramRef.size() && f.paramRef[i] && plainRef(e->kids[i]) && e->kids[i]->s == nm)
                        return true;
                }
                if (escExpr(e->kids[i], nm)) return true;
            }
            return false;
        }
        if (e->k == Expr::FnE) {                       // 匿名函数引用外层参数：保守判逃逸
            for (size_t i = 0; i < e->body.size(); ++i) if (escStmt(e->body[i], nm)) return true;
            return false;
        }
        if (escExpr(e->a, nm)) return true;
        if (escExpr(e->b, nm)) return true;
        for (size_t i = 0; i < e->kids.size(); ++i) if (escExpr(e->kids[i], nm)) return true;
        return false;
    }

    bool escStmt(SP st, const string& nm) {
        if (!st) return false;
        switch (st->k) {
        case Stmt::DeclS:  return st->decl && escExpr(st->decl->init, nm);
        case Stmt::ContinueS: return false;
        case Stmt::InS:    return plainRef(st->e) && st->e->s == nm;   // 输入直接写入参数槽
        case Stmt::IfS:
        case Stmt::WhileS: return escExpr(st->e, nm) || escBody(st->body, nm) || escBody(st->elseBody, nm);
        case Stmt::RetS:   return escExpr(st->e, nm);
        case Stmt::OutS:   return escExpr(st->e, nm);
        case Stmt::ExprS:  return escExpr(st->e, nm);
        default:           return false;
        }
    }

    bool escBody(const vector<SP>& body, const string& nm) {
        for (size_t i = 0; i < body.size(); ++i) if (escStmt(body[i], nm)) return true;
        return false;
    }

    string addrOf(EP e, int& vty) {
        return addrIdx(e, addrBase(e, vty), vty);
    }

    string asI(EP e) {
        string s = rv(e);
        return isF(e->ty) ? "((int64_t)(" + s + "))" : s;
    }

    // 32 位整数二元运算：用无符号 / int64 运算避免 C 的有符号溢出 UB，
    // 结果按 int32 回绕 —— 与解释器“写入截断”的语义逐位一致（含 INT_MIN / -1）。
    string intBin32(int op, const string& a, const string& b) {
        switch (op) {
        case T_PLUS:    return "((int32_t)((uint32_t)(" + a + ") + (uint32_t)(" + b + ")))";
        case T_MINUS:   return "((int32_t)((uint32_t)(" + a + ") - (uint32_t)(" + b + ")))";
        case T_STAR:    return "((int32_t)((uint32_t)(" + a + ") * (uint32_t)(" + b + ")))";
        case T_SLASH:   return "((int32_t)((int64_t)(" + a + ") / (int64_t)(" + b + ")))";
        case T_PERCENT: return "((int32_t)((int64_t)(" + a + ") % (int64_t)(" + b + ")))";
        default:        return "((int32_t)0)";
        }
    }

    // 32 位整数表达式：寄存器局部/参数直接用 int32，其余在运算点收窄
    string asI32(EP e) {
        if (gTypes.kindOf(e->ty) == TY_INT) {
            if (e->k == Expr::Num && !e->isFlt) return "((int32_t)" + fmt("%lld", e->num) + ")";
            if (e->k == Expr::RefE && plainRef(e) && pvar.find(e->s) != pvar.end()
                && !isF(pty[e->s]))
                return pvar[e->s];
            if (e->k == Expr::BinE && e->op != T_ASSIGN && e->op != T_AND && e->op != T_OR) {
                const char* op = 0;
                switch (e->op) {
                case T_PLUS: op = "+"; break;  case T_MINUS: op = "-"; break;
                case T_STAR: op = "*"; break;  case T_SLASH: op = "/"; break;
                case T_PERCENT: op = "%"; break;
                default: op = 0;
                }
                if (op) return intBin32(e->op, asI32(e->a), asI32(e->b));
            }
        }
        return "((int32_t)(" + rv(e) + "))";
    }
    string asD(EP e) {
        string s = rv(e);
        return isF(e->ty) ? s : "((double)(" + s + "))";
    }

    static string fname(size_t idx) { return "o_f" + fmt("%lld", (long long)idx); }

    static string cTypeExtern(int ty) {
        int k = gTypes.kindOf(ty);
        if (k == TY_DBL || k == TY_FLOAT) return "double";
        if (k == TY_STR) return "const char*";
        if (k == TY_HANDLE) return "void*";
        return "int32_t";
    }
    static bool isHandleTy(int ty) { return gTypes.kindOf(ty) == TY_HANDLE; }

    // 间接调用：按函数下标生成派发链（候选 = 参数个数相同的所有函数）
    string callIndirect(const string& idxEx, const vector<EP>& args, int resultTy, int calleeTy = -1) {
        bool hasSig = (calleeTy >= 0) && gTypes.at(calleeTy).hasSig;
        size_t sigN = hasSig ? gTypes.at(calleeTy).sigArgs.size() : 0;
        int sigR = hasSig ? gTypes.at(calleeTy).sigRet : -1;
        string chain = "((int64_t)0)";
        for (size_t fi = 0; fi < ip->funcs.size(); ++fi) {
            const Func& f = ip->funcs[fi];
            if (hasSig) {                                     // 有签名：只派发签名匹配的函数
                if (f.params.size() != sigN) continue;
                int fr = f.retTy < 0 ? gTypes.iInt : f.retTy;
                if (isF(fr) != isF(sigR)) continue;
            } else if (f.params.size() != args.size()) {
                continue;
            }
            string call = fname(fi) + "(";
            for (size_t i = 0; i < args.size(); ++i) {
                if (i) call += ", ";
                bool wantD = (i < f.paramTy.size()) && isF(f.paramTy[i]);
                call += wantD ? ("((double)(" + asD(args[i]) + "))") : ("((int64_t)(" + asI(args[i]) + "))");
            }
            if (needFp[fi]) {
                if (!args.empty()) call += ", ";
                call += (inMain ? string("_SP_TOP") : string("_fp")) + " - " + fmt("%lld", (long long)f.frameSize);
            }
            call += ")";
            string val = isF(resultTy) ? ("((double)(" + call + "))") : ("((int64_t)(" + call + "))");
            chain = "(" + idxEx + " == " + fmt("%lld", (long long)fi) + " ? " + val + " : " + chain + ")";
        }
        // 未初始化的 fn（-1）在运行时直接报错，而不是静默返回 0
        return "(((" + idxEx + ") < 0) ? ((int64_t)(fprintf(stderr, \"error: fn not initialized\\n\"), exit(2), 0)) : (" + chain + "))";
    }

    string callExpr(const string& nm, const vector<EP>& args) {
        map<string, int>::const_iterator it = ip->fnByName.find(nm);
        if (it == ip->fnByName.end()) {
            NativeSig* ns = findNative(nm);
            if (!ns) { fail("未定义的函数 " + nm); return "0"; }
            if (ns->isExtern) {
                string s = ns->cname + "(";
                for (size_t i = 0; i < args.size(); ++i) {
                    if (i) s += ", ";
                    int pt = ns->ptypes[i];
                    if (i < ns->pref.size() && ns->pref[i]) {          // 出参：传 _mem 里的格子地址
                        if (args[i]->k != Expr::RefE) { fail("extern 的出参必须传变量（如 ${x}）"); return "0"; }
                        int vt = gTypes.iInt;
                        string a = addrOf(args[i], vt);
                        s += "(" + cTypeExtern(pt) + "*)&_mem[" + a + "]." + (isF(pt) ? "d" : "i");
                    } else if (gTypes.kindOf(pt) == TY_HANDLE) {
                        s += "(void*)(intptr_t)(" + asI(args[i]) + ")";
                    } else if (gTypes.kindOf(pt) == TY_STR) {
                        s += "(const char*)(intptr_t)(" + asI(args[i]) + ")";
                    } else if (isF(pt)) {
                        s += "(double)(" + asD(args[i]) + ")";
                    } else {
                        s += "(int32_t)(" + asI(args[i]) + ")";
                    }
                }
                s += ")";
                int rk = gTypes.kindOf(ns->ret);
                if (rk == TY_STR) return "((int64_t)(intptr_t)(" + s + "))";
                if (isF(ns->ret)) return "((double)(" + s + "))";
                return "((int64_t)(" + s + "))";
            }
            string s = "n_" + ns->cname + "(";
            for (size_t i = 0; i < args.size(); ++i) {
                if (i) s += ", ";
                bool rf = (i < ns->pref.size() && ns->pref[i]);
                if (rf) {
                    if (args[i]->k != Expr::RefE) { fail("引用参数必须传变量（如 ${x}）"); return "0"; }
                    int vt = gTypes.iInt;
                    s += "((int64_t)(" + addrOf(args[i], vt) + "))";
                } else {
                    s += "((int64_t)(" + asI(args[i]) + "))";
                }
            }
            return s + ")";
        }
        const Func& f = ip->funcs[it->second];
        string s = fname((size_t)it->second) + "(";
        (void)0;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) s += ", ";
            bool rf = (i < f.paramRef.size() && f.paramRef[i]);
            if (rf) {
                if (args[i]->k != Expr::RefE) { fail("引用参数必须传变量（如 ${x}）"); return "0"; }
                int vt = gTypes.iInt;
                s += "((int64_t)(" + addrOf(args[i], vt) + "))";
            } else {
                bool wantD = (i < f.paramTy.size()) && isF(f.paramTy[i]);
                s += wantD ? ("((double)(" + asD(args[i]) + "))") : ("((int64_t)(" + asI(args[i]) + "))");
            }
        }
        if (needFp[it->second]) {
            if (!args.empty()) s += ", ";
            s += (inMain ? string("_SP_TOP") : string("_fp")) + " - " + fmt("%lld", (long long)f.frameSize);
        }
        return s + ")";
    }

    string rv(EP e) {
        switch (e->k) {
        case Expr::Num:
            if (e->isFlt) return "(" + fmtF(e->fval) + ")";
            return "((int64_t)" + fmt("%lld", e->num) + ")";
        case Expr::StrE:
            return "((int64_t)(intptr_t)\"" + esc(e->s) + "\")";
        case Expr::ListE:
            fail("列表 {a;b;…} 只能用于数组初始化");
            return "0";
        case Expr::UnitE:
            return rv(e->a);
        case Expr::RefE: {
            if (e->method == "len") {
                string v;
                if (e->kids.empty() && e->fields.empty() && pvar.find(e->s) != pvar.end())
                    v = pvar[e->s];
                else {
                    int t2 = gTypes.iInt;
                    v = "_mem[" + addrOf(e, t2) + "].i";
                }
                return "((int64_t)strlen((const char*)(intptr_t)(" + v + ")))";
            }
            if (e->method == "size") {
                if (!e->kids.empty() || !e->fields.empty()) return "((int64_t)1)";
                map<string, VarInfo>::iterator li = loc.find(e->s);
                if (li != loc.end()) return fmt("%lld", (long long)li->second.elems);
                map<string, VarInfo>::const_iterator gi = globals->find(e->s);
                if (gi == globals->end()) { fail("未定义的变量 " + e->s); return "0"; }
                return fmt("%lld", (long long)gi->second.elems);
            }
            if (plainRef(e) && pvar.find(e->s) != pvar.end()) {
                string v = pvar[e->s];
                int t = pty[e->s];
                return isF(t) ? v : "((int64_t)(" + v + "))";
            }
            int vty = gTypes.iInt;
            string base = addrBase(e, vty);
            if (gTypes.kindOf(vty) == TY_FN && !e->kids.empty())      // ${f}{x, y}：直接间接调用
                return callIndirect("((int64_t)(_mem[" + base + "].i))", e->kids, e->ty, vty);
            string a = addrIdx(e, base, vty);
            string v = isF(vty) ? ("_mem[" + a + "].d") : ("_mem[" + a + "].i");
            if (e->hasCast) {
                int ks = gTypes.kindOf(e->srcTy), kc = gTypes.kindOf(e->castTy);
                if (ks == TY_STR && kc != TY_STR) { fail("str 不能转成数值（字符串不可当整数用）"); return "0"; }
                if (kc == TY_STR && ks != TY_STR) { fail("只有 str 才能转成 str"); return "0"; }
                return storeCast(e->castTy, v);
            }
            return v;
        }
        case Expr::CallE:
            return callExpr(e->s, e->kids);
        case Expr::FnE:
            return "((int64_t)" + fmt("%lld", (long long)e->fnIdx) + ")";
        case Expr::PipeE: {
            vector<EP> one;
            one.push_back(e->a);
            if (e->b) {
                int ct = (e->b->k == Expr::RefE) ? e->b->ty : -1;   // 有签名就按签名派发
                return callIndirect("((int64_t)(" + asI(e->b) + "))", one, e->ty, ct);
            }
            return callExpr(e->s, one);
        }
        case Expr::UnE:
            if (e->op == T_TILDE || e->op == T_BANG) return "((int64_t)((int32_t)(" + asI(e->a) + ") ? 0 : 1))";
            if (e->op == T_MINUS) {
                if (isF(e->ty)) return "(-(" + asD(e->a) + "))";
                return "((int64_t)(int32_t)(-(int32_t)(" + asI(e->a) + ")))";
            }
            return rv(e->a);
        case Expr::BinE: {
            if (e->op == T_ASSIGN) {
                if (plainRef(e->a) && pvar.find(e->a->s) != pvar.end()) {
                    string v = pvar[e->a->s];
                    int t = pty[e->a->s];
                    if (isF(t)) return "(" + v + " = (double)(" + asD(e->b) + "))";
                    return "(" + v + " = " + storeCast(t, asI(e->b)) + ")";
                }
                int vty = gTypes.iInt;
                string a = addrOf(e->a, vty);
                if (isF(vty)) return "(_mem[" + a + "].d = (double)(" + asD(e->b) + "))";
                return "(_mem[" + a + "].i = " + storeCast(vty, asI(e->b)) + ")";
            }
            if (e->op == T_AND) return "((int64_t)((int32_t)(" + asI(e->a) + ") && (int32_t)(" + asI(e->b) + ")))";
            if (e->op == T_OR) return "((int64_t)((int32_t)(" + asI(e->a) + ") || (int32_t)(" + asI(e->b) + ")))";
            if (gTypes.kindOf(e->a->ty) == TY_STR || gTypes.kindOf(e->b->ty) == TY_STR) {
                string sa = "(const char*)(intptr_t)(" + asI(e->a) + ")";
                string sb = "(const char*)(intptr_t)(" + asI(e->b) + ")";
                if (e->op == T_EQ) return "((int64_t)(strcmp(" + sa + ", " + sb + ") == 0))";
                if (e->op == T_NE) return "((int64_t)(strcmp(" + sa + ", " + sb + ") != 0))";
                if (e->op == T_PLUS) return "((int64_t)(intptr_t)n_strcat(" + sa + ", " + sb + "))";
                fail("str 只能比较 == / != 或拼接 +");
                return "0";
            }
            if (e->op == T_AMP || e->op == T_PIPE || e->op == T_CARET || e->op == T_SHL || e->op == T_SHR) {
                string a = "((uint32_t)(" + asI32(e->a) + "))";
                string b = "((uint32_t)(" + asI32(e->b) + "))";
                string x;
                if (e->op == T_AMP) x = "(" + a + " & " + b + ")";
                else if (e->op == T_PIPE) x = "(" + a + " | " + b + ")";
                else if (e->op == T_CARET) x = "(" + a + " ^ " + b + ")";
                else if (e->op == T_SHL) x = "(" + a + " << (" + b + " & 31))";
                else x = "(" + a + " >> (" + b + " & 31))";
                return "((int64_t)(int32_t)" + x + ")";
            }
            const char* op = 0;
            switch (e->op) {
            case T_PLUS: op = "+"; break;
            case T_MINUS: op = "-"; break;
            case T_STAR: op = "*"; break;
            case T_SLASH: op = "/"; break;
            case T_PERCENT: op = "%"; break;
            case T_EQ: op = "=="; break;
            case T_NE: op = "!="; break;
            case T_LT: op = "<"; break;
            case T_LE: op = "<="; break;
            case T_GT: op = ">"; break;
            case T_GE: op = ">="; break;
            default: fail("未知运算符"); return "0";
            }
            bool anyF = isF(e->a->ty) || isF(e->b->ty);
            if (anyF) {
                if (e->op == T_PERCENT) { fail("C 后端：浮点不能取模（请先 .as{int}）"); return "0"; }
                string x = "(" + asD(e->a) + " " + op + " " + asD(e->b) + ")";
                return isF(e->ty) ? x : ("((int64_t)(" + x + "))");
            }
            string x;
            if (e->op == T_PLUS || e->op == T_MINUS || e->op == T_STAR ||
                e->op == T_SLASH || e->op == T_PERCENT)
                x = intBin32(e->op, asI32(e->a), asI32(e->b));
            else
                x = "(" + asI32(e->a) + " " + op + " " + asI32(e->b) + ")";
            return isF(e->ty) ? ("((double)(" + x + "))") : ("((int64_t)(" + x + "))");
        }
        }
        fail("未知表达式");
        return "0";
    }

    void stmt(SP s) {
        switch (s->k) {
        case Stmt::DeclS: {
            DP d = s->decl;
            if (d->isFn || d->isType) return;
            string ad;
            map<string, VarInfo>::iterator li = loc.find(d->name);
            if (li != loc.end()) ad = "(_fp + " + fmt("%lld", li->second.addr) + ")";
            else ad = "(" + fmt("%lld", d->addr) + ")";

            // 函数内局部变量：标量、不逃逸 → 直接用 C 局部量（与值参数同一套逃逸规则）
            if (li != loc.end() && curBody) {
                int kd = gTypes.kindOf(d->ty);
                if (kd != TY_STRUCT && kd != TY_FN && kd != TY_STR
                    && d->elems == 1 && gTypes.cellsOf(d->ty) == 1
                    && !escBody(*curBody, d->name)) {
                    string v = "_l" + fmt("%lld", (long long)li->second.addr);
                    bool isDbl = isF(d->ty);
                    pvar[d->name] = v; pty[d->name] = d->ty;
                    if (d->init && d->init->k != Expr::ListE) {
                        put(string(isDbl ? "double " : "int32_t ") + v + " = " +
                            (isDbl ? ("(double)(" + asD(d->init) + ")")
                                   : ("(int32_t)(" + asI(d->init) + ")")) + ";");
                    } else {
                        put(string(isDbl ? "double " : "int32_t ") + v + " = 0;");
                    }
                    return;
                }
            }
            if (d->init) {
                const TypeInfo& ti0 = gTypes.at(d->ty);
                if (ti0.kind == TY_STRUCT && d->elems == 1) {     // 结构体字面量：按字段顺序
                    vector<EP> xs;
                    if (d->init->k == Expr::ListE) xs = d->init->kids;
                    else xs.push_back(d->init);
                    if (xs.size() > ti0.fieldOrder.size()) { fail("结构体初始化字段过多"); return; }
                    for (size_t i = 0; i < ti0.fieldOrder.size(); ++i) {
                        int ft = ti0.fieldType.find(ti0.fieldOrder[i])->second;
                        int fo = ti0.fieldOfs.find(ti0.fieldOrder[i])->second;
                        string a2 = "(" + ad + " + " + fmt("%lld", fo) + ")";
                        if (i < xs.size()) {
                            if (isF(ft)) put("_mem[" + a2 + "].d = (double)(" + asD(xs[i]) + ");");
                            else put("_mem[" + a2 + "].i = " + storeCast(ft, asI(xs[i])) + ";");
                        } else {
                            for (int k = 0; k < gTypes.cellsOf(ft); ++k)
                                put("_mem[(" + ad + " + " + fmt("%lld", fo + k) + ")].i = 0;");
                        }
                    }
                    return;
                }
                if (d->init->k == Expr::ListE) {                  // 数组字面量
                    const vector<EP>& xs = d->init->kids;
                    if ((int)xs.size() > d->elems) { fail("初始化元素过多"); return; }
                    int stride = gTypes.cellsOf(d->ty);
                    for (size_t i = 0; i < xs.size(); ++i) {
                        string a2 = "(" + ad + " + " + fmt("%lld", (int)i * stride) + ")";
                        if (isF(d->ty)) put("_mem[" + a2 + "].d = (double)(" + asD(xs[i]) + ");");
                        else put("_mem[" + a2 + "].i = " + storeCast(d->ty, asI(xs[i])) + ";");
                    }
                    for (int i = (int)xs.size(); i < d->elems; ++i)   // 剩余元素补 0（与解释器一致）
                        for (int k = 0; k < stride; ++k)
                            put("_mem[(" + ad + " + " + fmt("%lld", i * stride + k) + ")].i = 0;");
                    return;
                }
                if (d->elems != 1) { fail("C 后端暂不支持该初始化形式"); return; }
                if (isF(d->ty)) put("_mem[" + ad + "].d = (double)(" + asD(d->init) + ");");
                else put("_mem[" + ad + "].i = " + storeCast(d->ty, asI(d->init)) + ";");
            } else {
                long long z = (gTypes.kindOf(d->ty) == TY_FN) ? -1 : 0;   // fn 的空值是 -1
                for (int k = 0; k < d->size; ++k) {
                    if (gTypes.kindOf(d->ty) == TY_STR) put("_mem[" + ad + " + " + fmt("%lld", k) + "].i = (int64_t)(intptr_t)\"\";");
                    else put("_mem[" + ad + " + " + fmt("%lld", k) + "].i = " + fmt("%lld", z) + ";");
                }
            }
            return;
        }
        case Stmt::ExprS:
            put("(void)(" + rv(s->e) + ");");
            return;
        case Stmt::IfS:
            put("if ((int32_t)(" + asI(s->e) + ")) {");
            ind++; block(s->body); ind--;
            if (!s->elseBody.empty()) {
                put("} else {");
                ind++; block(s->elseBody); ind--;
            }
            put("}");
            return;
        case Stmt::WhileS:
            put("while ((int32_t)(" + asI(s->e) + ")) {");
            ind++; loopDepth++; block(s->body); loopDepth--; ind--;
            put("}");
            return;
        case Stmt::RetS:
            if (inMain) put("return (int)(" + asI(s->e) + ");");
            else if (retCT == "double") put("{ _ret = (double)(" + asD(s->e) + "); goto " + lend + "; }");
            else put("{ _ret = " + storeCast(curRetTy, asI(s->e)) + "; goto " + lend + "; }");
            return;
        case Stmt::BreakS:
            if (loopDepth <= 0) { fail("'!' 只能在循环内使用"); return; }
            put("break;");
            return;
        case Stmt::ContinueS:
            put("continue;");
            return;
        case Stmt::AssignS:
            fail("内部错误：AssignS 不该出现在 AST 里");
            return;
        case Stmt::OutS: {
            if (s->isFmt) {                                 // .{fmt; "…"; 实参…}
                if (s->args.empty() || s->args[0]->k != Expr::StrE) { fail("fmt 的第一个参数必须是格式串字面量"); return; }
                vector<FmtPiece> ps = splitFmt(s->args[0]->s, 0);
                size_t ai = 1;
                for (size_t i = 0; i < ps.size(); ++i) {
                    if (!ps[i].isSpec) { put("fputs(\"" + esc(ps[i].text) + "\", stdout);"); continue; }
                    if (ai >= s->args.size()) { fail("格式串的实参不够"); return; }
                    EP a = s->args[ai++];
                    char c = ps[i].conv;
                    if (c == 's') put("fputs((const char*)(intptr_t)(" + asI(a) + "), stdout);");
                    else if (c == 'd') put("printf(\"%d\", (int)(" + asI(a) + "));");
                    else if (c == 'c') put("putchar((unsigned char)(" + asI(a) + "));");
                    else if (c == 'x') put("printf(\"%x\", (unsigned)(" + asI(a) + "));");
                    else put("printf(\"%g\", (double)(" + asD(a) + "));");
                }
                return;
            }
            if (s->elems > 1) { fail("输出不支持 {x;y} 形式，请逐个输出"); return; }
            int k = gTypes.kindOf(s->ty);
            if (k == TY_STR) {
                if (s->e->k == Expr::StrE) put("fputs(\"" + esc(s->e->s) + "\", stdout);");
                else put("fputs((const char*)(intptr_t)(" + asI(s->e) + "), stdout);");
                return;
            }
            if (k == TY_CHAR) { put("putchar((unsigned char)(int)(" + asI(s->e) + "));"); return; }
            if (k == TY_DBL || k == TY_FLOAT) { put("printf(\"%g\", (double)(" + asD(s->e) + "));"); return; }
            put("printf(\"%d\", (int)(" + asI(s->e) + "));");
            return;
        }
        case Stmt::InS: {
            if (s->e->k != Expr::RefE) { fail(",{…} 需要引用"); return; }
            if (s->elems != 1) { fail("C 后端暂不支持数组输入"); return; }
            int vty = gTypes.iInt;
            string ad = addrOf(s->e, vty);
            if (gTypes.kindOf(vty) == TY_STR) { put("_mem[" + ad + "].i = n_readline();"); return; }
            if (isF(vty)) put("{ double _t; if (scanf(\"%lf\", &_t) != 1) return 2; _mem[" + ad + "].d = _t; }");
            else put("{ int _t; if (scanf(\"%d\", &_t) != 1) return 2; _mem[" + ad + "].i = " + storeCast(vty, "_t") + "; }");
            return;
        }
        }
    }

    void block(const vector<SP>& body) {
        for (size_t i = 0; i < body.size(); ++i) stmt(body[i]);
    }

    static string nativeImpl(const string& n) {
        if (n == "len")    return "static int64_t n_slen(int64_t p){return (int64_t)strlen((const char*)(intptr_t)p);}\n";
        if (n == "find")   return "static int64_t n_sfind(int64_t a,int64_t b){const char* s=(const char*)(intptr_t)a;const char* p=strstr(s,(const char*)(intptr_t)b);return p?(int64_t)(p-s):-1;}\n";
        if (n == "charat") return "static int64_t n_scharat(int64_t a,int64_t i){const char* s=(const char*)(intptr_t)a;int n=(int)strlen(s);if(i<0||i>=n)return -1;return (unsigned char)s[i];}\n";
        if (n == "substr") return
            "static int64_t n_ssubstr(int64_t a,int64_t from,int64_t n){const char* s=(const char*)(intptr_t)a;"
            "int len=(int)strlen(s);if(from<0)from=0;if(from>len)from=len;if(n<0)n=0;if(from+n>len)n=len-from;"
            "char* p=_salloc((int)n+1);memcpy(p,s+from,(size_t)n);p[n]=0;return (int64_t)(intptr_t)p;}\n";
        if (n == "system") return
            "static int64_t n_system(int64_t c){int r=system((const char*)(intptr_t)c);"
#ifndef _WIN32
            "if(r!=-1&&WIFEXITED(r))r=WEXITSTATUS(r);"
#endif
            "return r;}\n";
        if (n == "open") return "static int64_t n_open(int64_t p,int64_t m){FILE* f=fopen((const char*)(intptr_t)p,(const char*)(intptr_t)m);if(!f)return -1;for(int i=0;i<64;i++)if(!_f[i]){_f[i]=f;return i;}fclose(f);return -2;}\n";
        if (n == "close") return "static int64_t n_close(int64_t h){if(!_fok(h))return -1;fclose(_f[h]);_f[h]=0;return 0;}\n";
        if (n == "getc") return "static int64_t n_getc(int64_t h){if(!_fok(h))return -1;int c=fgetc(_f[h]);return c==EOF?-1:c;}\n";
        if (n == "putc") return "static int64_t n_putc(int64_t h,int64_t b){if(!_fok(h))return -1;return fputc((int)b,_f[h])==EOF?-1:0;}\n";
        if (n == "puts") return "static int64_t n_puts(int64_t h,int64_t s){if(!_fok(h))return -1;return fputs((const char*)(intptr_t)s,_f[h])<0?-1:0;}\n";
        if (n == "eof") return "static int64_t n_eof(int64_t h){return _fok(h)?(feof(_f[h])?1:0):1;}\n";
        if (n == "seek") return "static int64_t n_seek(int64_t h,int64_t p){return _fok(h)?fseek(_f[h],(long)p,SEEK_SET):-1;}\n";
        if (n == "tell") return "static int64_t n_tell(int64_t h){return _fok(h)?ftell(_f[h]):-1;}\n";
        if (n == "read") return "static int64_t n_read(int64_t h,int64_t a,int64_t n){if(!_fok(h))return -1;int64_t k=0;for(;k<n;k++){int c=fgetc(_f[h]);if(c==EOF)break;_mem[a+k].i=(unsigned char)c;}return k;}\n";
        if (n == "write") return "static int64_t n_write(int64_t h,int64_t a,int64_t n){if(!_fok(h))return -1;int64_t k=0;for(;k<n;k++){if(fputc((int)(unsigned char)_mem[a+k].i,_f[h])==EOF)break;}return k;}\n";
        if (n == "remove") return "static int64_t n_remove(int64_t p){return remove((const char*)(intptr_t)p);}\n";
        if (n == "exists") return "static int64_t n_exists(int64_t p){FILE* f=fopen((const char*)(intptr_t)p,\"r\");if(!f)return 0;fclose(f);return 1;}\n";
        if (n == "rename") return "static int64_t n_rename(int64_t a,int64_t b){return rename((const char*)(intptr_t)a,(const char*)(intptr_t)b);}\n";
        return "";
    }

    // 局部变量是否会占用内存槽（与 stmt() 里的寄存器化条件保持一致）
    bool localsNeedFrame(const vector<SP>& body, const vector<SP>& fnBody) {
        for (size_t i = 0; i < body.size(); ++i) {
            SP s = body[i];
            if (s->k == Stmt::DeclS && s->decl && !s->decl->isFn && !s->decl->isType) {
                DP d = s->decl;
                int kd = gTypes.kindOf(d->ty);
                bool simple = (kd != TY_STRUCT && kd != TY_FN && kd != TY_STR
                               && d->elems == 1 && gTypes.cellsOf(d->ty) == 1);
                if (!simple || escBody(fnBody, d->name)) return true;
            }
            if ((s->k == Stmt::IfS || s->k == Stmt::WhileS) && localsNeedFrame(s->body, fnBody)) return true;
        }
        return false;
    }

    bool fnNeedsFrame(const Func& fn) {
        for (size_t k = 0; k < fn.params.size(); ++k) {
            bool rf = (k < fn.paramRef.size() && fn.paramRef[k]);
            int kd = gTypes.kindOf(fn.paramTy[k]);
            if (rf || kd == TY_STRUCT || kd == TY_FN || kd == TY_STR
                || gTypes.cellsOf(fn.paramTy[k]) != 1
                || escBody(fn.body, fn.params[k]))
                return true;
        }
        return localsNeedFrame(fn.body, fn.body);
    }

    // 调用点要传「自己的 _fp - 被调者帧大小」，所以只要调用了需要 _fp 的函数，
    // 调用者自己就必须持有帧基址（哪怕它一个内存槽都没有）。
    bool exprCallsFrame(EP e, const vector<char>& nf) {
        if (!e) return false;
        switch (e->k) {
        case Expr::CallE: {
            map<string, int>::const_iterator it = ip->fnByName.find(e->s);
            if (it != ip->fnByName.end() && nf[it->second]) return true;
            for (size_t i = 0; i < e->kids.size(); ++i)
                if (exprCallsFrame(e->kids[i], nf)) return true;
            return false;
        }
        case Expr::RefE:
            if (e->ty >= 0 && gTypes.kindOf(e->ty) == TY_FN && !e->kids.empty()) {   // ${f}{…}：按函数下标派发
                for (size_t i = 0; i < nf.size(); ++i) if (nf[i]) return true;
            }
            for (size_t i = 0; i < e->kids.size(); ++i)
                if (exprCallsFrame(e->kids[i], nf)) return true;
            return false;
        case Expr::PipeE:
            if (e->b) {                                                // x | ${f}：间接调用
                for (size_t i = 0; i < nf.size(); ++i) if (nf[i]) return true;
            } else {
                map<string, int>::const_iterator it = ip->fnByName.find(e->s);
                if (it != ip->fnByName.end() && nf[it->second]) return true;
            }
            if (exprCallsFrame(e->a, nf) || exprCallsFrame(e->b, nf)) return true;
            return false;
        case Expr::UnE:
            return exprCallsFrame(e->a, nf);
        case Expr::BinE:
            return exprCallsFrame(e->a, nf) || exprCallsFrame(e->b, nf);
        case Expr::UnitE:
            return exprCallsFrame(e->a, nf);
        case Expr::ListE:
            for (size_t i = 0; i < e->kids.size(); ++i)
                if (exprCallsFrame(e->kids[i], nf)) return true;
            return false;
        default:
            return false;                       // Num / StrE / FnE（FnE 体是独立函数）
        }
    }

    bool stmtCallsFrame(SP s, const vector<char>& nf) {
        switch (s->k) {
        case Stmt::DeclS:   return s->decl ? exprCallsFrame(s->decl->init, nf) : false;
        case Stmt::AssignS: return exprCallsFrame(s->e, nf) || exprCallsFrame(s->e2, nf);
        case Stmt::IfS:
        case Stmt::WhileS: {
            if (exprCallsFrame(s->e, nf) || exprCallsFrame(s->e2, nf)) return true;
            for (size_t i = 0; i < s->body.size(); ++i)
                if (stmtCallsFrame(s->body[i], nf)) return true;
            for (size_t i = 0; i < s->elseBody.size(); ++i)
                if (stmtCallsFrame(s->elseBody[i], nf)) return true;
            return false;
        }
        case Stmt::RetS:    return exprCallsFrame(s->e, nf);
        case Stmt::OutS:
        case Stmt::InS: {
            if (exprCallsFrame(s->e, nf)) return true;
            for (size_t i = 0; i < s->args.size(); ++i)
                if (exprCallsFrame(s->args[i], nf)) return true;
            return false;
        }
        case Stmt::ExprS:   return exprCallsFrame(s->e, nf);
        default:            return false;
        }
    }

    bool bodyCallsFrame(const vector<SP>& body, const vector<char>& nf) {
        for (size_t i = 0; i < body.size(); ++i)
            if (stmtCallsFrame(body[i], nf)) return true;
        return false;
    }

    string emit(Interp& I, const vector<DP>& topDecls, const vector<SP>& mainBody) {
        globals = &I.globals;
        ip = &I;
        // 1) 前部
        o += "#include <stdio.h>\n#include <stdint.h>\n#include <stdlib.h>\n#include <string.h>\n";
#ifndef _WIN32
        o += "#include <sys/wait.h>\n";      // system() 的 wait 状态 → 退出码
#endif
        o += "\n";
        for (map<string, NativeSig>::iterator it = gNativeTab.begin(); it != gNativeTab.end(); ++it) {
            if (!it->second.isExtern) continue;
            const NativeSig& ns = it->second;
            // 生成代码自己 include 的 libc 函数已经有原型，重复声明会冲突 —— 直接跳过
            static const char* kPredeclared[] = {
                "strlen","strcmp","strstr","memcpy","printf","fprintf","fputs","fopen","fclose",
                "fgets","fputc","fgetc","fwrite","fflush","remove","rename","system","exit",
                "scanf","snprintf", 0
            };
            bool pre = false;
            for (int pi = 0; kPredeclared[pi]; ++pi) if (ns.cname == kPredeclared[pi]) { pre = true; break; }
            if (pre) continue;
            string d = "extern " + cTypeExtern(ns.ret) + " " + ns.cname + "(";
            for (size_t i = 0; i < ns.ptypes.size(); ++i) {
                if (i) d += ", ";
                d += cTypeExtern(ns.ptypes[i]);
                if (i < ns.pref.size() && ns.pref[i]) d += "*";
            }
            if (ns.ptypes.empty()) d += "void";
            d += ");";
            o += d + "\n";
        }
        o += "typedef union { long long i; double d; } Cell;\n";
        o += "static Cell _mem[" + fmt("%lld", (long long)I.memSize) + "];\n";
        o += "#define _SP_TOP " + fmt("%lld", (long long)I.memSize) + "\n";
        o += "/* 字符串分配器：1 MB 分块 malloc，指针永远有效（不回收，与解释器的字符串池一致） */\n";
        o += "#define _SBLK (1 << 20)\n";
        o += "static char* _sblk = 0; static int _sleft = 0;\n";
        o += "static char* _salloc(int n) {\n"
             "  if (n > _SBLK / 2) { char* p = (char*)malloc((size_t)n); if (!p) { fprintf(stderr, \"error: out of memory\\n\"); exit(2); } return p; }\n"
             "  if (_sleft < n) { _sblk = (char*)malloc(_SBLK); if (!_sblk) { fprintf(stderr, \"error: out of memory\\n\"); exit(2); } _sleft = _SBLK; }\n"
             "  char* p = _sblk; _sblk += n; _sleft -= n; return p;\n}\n";
        o += "static int64_t n_strcat(const char* x, const char* y) {\n"

             "  int n=(int)strlen(x), m=(int)strlen(y);\n"
             "  char* p=_salloc(n+m+1); memcpy(p,x,n); memcpy(p+n,y,m); p[n+m]=0;\n"
             "  return (int64_t)(intptr_t)p;\n}\n";
        o += "static int64_t n_readline(void) {\n"
             "  char tmp[1024]; if (!fgets(tmp,sizeof tmp,stdin)) tmp[0]=0;\n"
             "  int n=(int)strlen(tmp); while (n>0 && (tmp[n-1]=='\\n'||tmp[n-1]=='\\r')) tmp[--n]=0;\n"
             "  char* p=_salloc(n+1); memcpy(p,tmp,n+1); return (int64_t)(intptr_t)p;\n}\n";
        // 静态区上界：栈往下长，越过它就是踩静态变量（解释器在 callFunc 里同样检查）
        o += "#define _SBASE " + fmt("%lld", (long long)I.staticBase) + "\n\n";
        // 库函数前置（必须在 _mem 之后：read/write 要直接访问内存）
        if (!gNativeTab.empty()) {
            bool anyFile = false;
            for (map<string, NativeSig>::iterator it = gNativeTab.begin(); it != gNativeTab.end(); ++it)
                if (it->first != "system") anyFile = true;
            if (anyFile) o += "static FILE* _f[64];\nstatic int _fok(int64_t h){return h>=0&&h<64&&_f[h]!=0;}\n";
            for (map<string, NativeSig>::iterator it = gNativeTab.begin(); it != gNativeTab.end(); ++it)
                o += nativeImpl(it->first);
            o += "\n";
        }

        // topDecls 的初值在 main() 开头按源码顺序补发（见下方「静态初始化」）
        needFp.assign(I.funcs.size(), 0);
        for (size_t i = 0; i < I.funcs.size(); ++i) needFp[i] = fnNeedsFrame(I.funcs[i]);
        // 传播：f 调用了需要 _fp 的 g ⇒ f 自己也必须持有帧基址（否则调用点无从取 _fp）
        for (bool changed = true; changed; ) {
            changed = false;
            for (size_t i = 0; i < I.funcs.size(); ++i) {
                if (needFp[i]) continue;
                if (bodyCallsFrame(I.funcs[i].body, needFp)) { needFp[i] = 1; changed = true; }
            }
        }

        // 2) 前向声明（按下标命名：o_f0, o_f1, …）
        for (size_t i = 0; i < I.funcs.size(); ++i) {
            const Func& fn = I.funcs[i];
            string sig = "static " + cTypeOf(fn.retTy) + " " + fname(i) + "(";
            for (size_t k = 0; k < fn.params.size(); ++k) {
                if (k) sig += ", ";
                bool rf = (k < fn.paramRef.size() && fn.paramRef[k]);
                sig += (rf ? string("int64_t") : cTypeOf(fn.paramTy[k])) + " _p" + fmt("%lld", (long long)k);
            }
            if (needFp[i]) { if (!fn.params.empty()) sig += ", "; sig += "int _fp"; }
            o += sig + ");\n";
        }
        o += "\n";

        // 3) 函数定义（命名函数与匿名函数一视同仁）
        for (size_t i = 0; i < I.funcs.size(); ++i) {
            const Func& fn = I.funcs[i];
            loc.clear(); pvar.clear(); pty.clear();
            curBody = &fn.body;
            vector<char> reg(fn.params.size(), 0);
            for (size_t k = 0; k < fn.params.size(); ++k) {
                VarInfo vi;
                vi.addr = fn.paramOfs[k];
                vi.ty = fn.paramTy[k];
                vi.isRef = (k < fn.paramRef.size() && fn.paramRef[k]) != 0;
                vi.elems = vi.isRef ? fn.paramElems[k] : 1;
                loc[fn.params[k]] = vi;
                int kd = gTypes.kindOf(fn.paramTy[k]);
                if (!vi.isRef && kd != TY_FN && kd != TY_STRUCT && kd != TY_STR
                    && gTypes.cellsOf(fn.paramTy[k]) == 1
                    && !escBody(fn.body, fn.params[k]))
                    reg[k] = 1;
            }
            collect(fn.body);
            lend = "_lend_f" + fmt("%lld", (long long)i);
            retCT = cTypeOf(fn.retTy);
            curRetTy = fn.retTy;
            string sig = "static " + retCT + " " + fname(i) + "(";
            for (size_t k = 0; k < fn.params.size(); ++k) {
                if (k) sig += ", ";
                bool rf = (k < fn.paramRef.size() && fn.paramRef[k]);
                sig += (rf ? string("int64_t") : cTypeOf(fn.paramTy[k])) + " _p" + fmt("%lld", (long long)k);
            }
            if (needFp[i]) { if (!fn.params.empty()) sig += ", "; sig += "int _fp"; }
            sig += ") {";
            put(sig);
            ind++;
            // 有内存槽的函数才需要帧基址与栈界检查；全寄存器化的函数两者都省掉
            if (needFp[i])
                put("if (_fp < _SBASE) { fprintf(stderr, \"错误: 栈溢出（静态区已占 %d 个单元）\\n\", (int)_SBASE); exit(2); }");
            for (size_t k = 0; k < fn.params.size(); ++k) {
                string dst = "_mem[_fp + " + fmt("%lld", (long long)fn.paramOfs[k]) + "]";
                string src = "_p" + fmt("%lld", (long long)k);
                bool rf = (k < fn.paramRef.size() && fn.paramRef[k]);
                if (reg[k]) {                                  // 不逃逸的值参数：留在 C 寄存器
                    string v = "_v" + fmt("%lld", (long long)k);
                    pvar[fn.params[k]] = v; pty[fn.params[k]] = fn.paramTy[k];
                    if (isF(fn.paramTy[k])) put("double " + v + " = (double)(" + src + ");");
                    else put("int32_t " + v + " = (int32_t)(" + src + ");");
                }
                else if (rf) put(dst + ".i = (int64_t)(" + src + ");");
                else if (isF(fn.paramTy[k])) put(dst + ".d = (double)(" + src + ");");
                else put(dst + ".i = " + storeCast(fn.paramTy[k], src) + ";");
            }
            put(retCT + " _ret = 0;");
            block(fn.body);
            put(lend + ":");
            put("return _ret;");
            ind--;
            put("}");
            put("");
        }

        // 4) main（O{} 体：静态作用域）
        curBody = 0;
        loc.clear();
        pvar.clear(); pty.clear();          // 不能沿用最后一个函数里寄存器化的局部名
        lend = "_lend_main";
        retCT = "int64_t";
        curRetTy = gTypes.iInt;
        inMain = true;
        put("int main(void) {");
        ind++;
        // 标记已使用：只做 I/O 的程序否则在 -Wall 下报 _mem/_sp unused-variable
        put("(void)_mem; (void)n_strcat; (void)n_readline;");
        // 静态初始化：顶层声明的初值按源码顺序（与解释器一致，含 fn 的 -1 与数组补零）
        for (size_t i = 0; i < topDecls.size(); ++i) {
            DP d = topDecls[i];
            if (d->isFn || d->isType) continue;
            SP s(new Stmt());
            s->k = Stmt::DeclS;
            s->decl = d;
            s->line = d->line;
            stmt(s);
        }
        block(mainBody);
        put("return 0;");
        ind--;
        put("}");
        return o;
    }
};


static string readFile(const string& p) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) throw Err("打不开文件 " + p);
    string s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

int main(int argc, char** argv) {
    internStr("");                    // 0 号串固定为空串：未初始化的 str 读出来是 ""，不是第一个字面量
    if (argc >= 2 && string(argv[1]) == "--version") {
        printf("olang %s\n", OLANG_VERSION);
        printf("built %s %s\n", __DATE__, __TIME__);
#ifdef __clang__
        printf("compiler clang %s\n", __clang_version__);
#elif defined(__VERSION__)
        printf("compiler gcc %s\n", __VERSION__);
#endif
        printf("spec     SPEC v1.1（见 SPEC.md）\n");
        return 0;
    }
    if (argc < 2 || string(argv[1]) == "-h" || string(argv[1]) == "--help") {
        fprintf(stderr,
            "用法: olang <程序.o> [选项]\n"
            "  （默认）      解释执行\n"
            "  --map         打印静态地址映射（名字/地址/长度）\n"
            "  --map --json  同上，机器可读 JSON\n"
            "  --mem         退出时 dump 内存（只列非零单元）\n"
            "  --mem=all     退出时 dump 全部单元\n"
            "  --trace       逐条语句打印 line / 语句类型 / sp / 被写单元（stderr）\n"
            "  --emit-c      生成 C 代码到 stdout\n"
            "  --cc          生成 C → gcc 编译 → 直接运行（需要 gcc 在 PATH 里）\n"
            "  --rebuild     --cc 强制重新编译（默认会复用没变过的 .gen.exe）\n"
            "  --mem-size N  本次运行/生成的地址空间大小（单元数，默认 %d，上限 %d）\n"
            "  --walias      显式地址区间重叠时给提示（默认静默，见 SPEC §12.4）\n"
            "  --version     打印版本与构建信息\n",
            (int)MEM_SIZE, (int)MEM_SIZE_MAX);
        return argc < 2 ? 1 : 0;
    }
    bool wantMap = false;
    bool wantC = false;
    bool wantCC = false;
    bool wantJson = false;
    bool wantMem = false;
    bool memAll = false;
    bool wantTrace = false;
    bool wantRebuild = false;
    bool wantWalias = false;
    int cliMem = 0;                   // --mem-size N
    bool cliMemGiven = false;         // 是否显式给了 --mem-size
    for (int i = 2; i < argc; ++i) {
        string a = argv[i];
        // 容忍单横线写法（-cc / -map），统一成双横线
        if (a.size() > 1 && a[0] == '-' && a[1] != '-') a = "-" + a;
        if (a == "--map") wantMap = true;
        else if (a == "--json") wantJson = true;
        else if (a == "--mem") wantMem = true;
        else if (a == "--mem=all") { wantMem = true; memAll = true; }
        else if (a == "--trace") wantTrace = true;
        else if (a == "--rebuild") wantRebuild = true;
        else if (a == "--walias") wantWalias = true;
        else if (a == "--mem-size") {
            if (i + 1 < argc) { cliMem = atoi(argv[++i]); cliMemGiven = true; }
            else { fprintf(stderr, "错误: --mem-size 后面要跟单元数\n"); return 1; }
        }
        else if (a.compare(0, 11, "--mem-size=") == 0) {
            cliMem = atoi(a.c_str() + 11); cliMemGiven = true;
        }
        else if (a == "--emit-c") wantC = true;
        else if (a == "--cc") wantCC = true;
        else { fprintf(stderr, "错误: 未知选项 '%s'（--help 看用法）\n", argv[i]); return 1; }
    }

    try {
        string src = readFile(argv[1]);
        Parser p(src);
        p.program();

        int memSize = cliMemGiven ? cliMem : (p.declaredMem ? p.declaredMem : MEM_SIZE);
        if (memSize < 1 || memSize > MEM_SIZE_MAX) {
            fprintf(stderr, "错误: --mem-size 必须在 1..%d 之间\n", (int)MEM_SIZE_MAX);
            return 1;
        }

        Interp it(memSize);
        it.warnAlias = wantWalias;
        it.mapping = wantMap;

        // 布局：顶层声明 → 匿名函数 → O{} 体（静态）
        int ofs = 0;
        for (size_t i = 0; i < p.topDecls.size(); ++i) it.layoutDecl(p.topDecls[i], false, ofs);
        for (size_t i = 0; i < p.anonFns.size(); ++i) it.layoutAnon(p.anonFns[i]);
        int ofs2 = 0;
        it.layoutBody(p.mainBody, false, ofs2);
        it.staticBase = it.nextFree;

        if (wantMap && wantJson) {
            FILE* mout = (wantC || wantCC) ? stderr : stdout;
            it.dumpMapJson(mout);
        } else if (wantMap) {
            // --emit-c / --cc 时映射表走 stderr，否则人类看的表会混进生成的 C 源码
            FILE* mout = (wantC || wantCC) ? stderr : stdout;
            fprintf(mout, "--- 静态地址映射 (MEM_SIZE=%d) ---\n", it.memSize);
            fprintf(mout, " addr  名字\n");
            for (size_t i = 0; i < it.mapLines.size(); ++i)
                fprintf(mout, " %s\n", it.mapLines[i].second.c_str());
            fprintf(mout, " 静态区结束 = %d, 栈从 %d 向下\n", it.nextFree, it.memSize - 1);
        }
        if (wantMap && !wantC && !wantCC) return 0;   // --map 只输出映射表，不执行程序

        // 两种模式都做标注：静态错误（签名/类型/str 转数值…）要尽早报，而不是只在 C 后端
        it.annotateAll(p.topDecls, p.anonFns, p.mainBody);

        if (wantC || wantCC) {
            CEmit ce;
            string code = ce.emit(it, p.topDecls, p.mainBody);
            if (ce.bad) {
                fprintf(stderr, "C 后端暂不支持: %s\n", ce.why.c_str());
                return 4;
            }
            if (!wantCC) {                       // --emit-c：只打印
                fputs(code.c_str(), stdout);
                return 0;
            }
            // --cc：一步式（生成 → gcc → 运行）
            string base = argv[1];
            size_t dot = base.find_last_of('.');
            if (dot != string::npos) base = base.substr(0, dot);
            string cpath = base + ".gen.c", epath = base + ".gen.exe";
            string runp = epath.find('/') == string::npos ? "./" + epath : epath;   // 相对路径要带 ./，否则 shell 只查 PATH

            // 缓存：源程序和我们自己都没变过，就直接跑上次编好的 exe（省掉 ~1.5-3 s 的 gcc）
            bool fresh = false;
#ifndef _WIN32
            if (!wantRebuild) {
                struct stat sp, se, ss;
                char selfbuf[4096];
                string self;
                ssize_t ln = readlink("/proc/self/exe", selfbuf, sizeof selfbuf - 1);
                if (ln > 0) { selfbuf[ln] = 0; self = selfbuf; }
                if (stat(argv[1], &sp) == 0 && stat(epath.c_str(), &se) == 0) {
                    time_t ref = sp.st_mtime;
                    if (!self.empty() && stat(self.c_str(), &ss) == 0 && ss.st_mtime > ref)
                        ref = ss.st_mtime;
                    if (se.st_mtime >= ref) fresh = true;
                }
            }
#endif
            if (fresh) {
                string cmd2 = "\"" + runp + "\"";
                int rc2 = system(cmd2.c_str());
                if (rc2 == -1) return 4;
                if (WIFEXITED(rc2)) return WEXITSTATUS(rc2);
                return 4;
            }
            FILE* gf = fopen(cpath.c_str(), "wb");
            if (!gf) { fprintf(stderr, "错误: 无法写入 %s\n", cpath.c_str()); return 4; }
            fwrite(code.data(), 1, code.size(), gf);
            fclose(gf);
            string links;
            for (size_t li = 0; li < gLinkFlags.size(); ++li) {
                const string& lf = gLinkFlags[li];
                if (lf.find('/') != string::npos || lf.find(".so") != string::npos)
                    links += " \"" + lf + "\"";          // 全路径：直接交给链接器
                else
                    links += " -l" + lf;                   // 短名：-lm / -lsqlite3
            }
            string cmd = "gcc -std=c99 -O2 -w -o \"" + epath + "\" \"" + cpath + "\"" + links;
            if (system(cmd.c_str()) != 0) {
                fprintf(stderr, "错误: gcc 编译失败（%s）\n", cmd.c_str());
                return 4;
            }
            cmd = "\"" + runp + "\"";
            int rc = system(cmd.c_str());
            remove(cpath.c_str());
#ifdef _WIN32
            return rc;                                   // cmd.exe 直接透传退出码
#else
            if (rc == -1) return 4;
            if (WIFEXITED(rc)) return WEXITSTATUS(rc);    // POSIX：退出码在 status >> 8
            return 4;
#endif
        }

        // 静态初始化：顶层声明的初值按源码顺序求值（SPEC: &{T;name} = {SPEC} = {INIT}）
        for (size_t i = 0; i < p.topDecls.size(); ++i) {
            DP d = p.topDecls[i];
            if (d->isFn || d->isType) continue;
            SP s(new Stmt());
            s->k = Stmt::DeclS;
            s->decl = d;
            s->line = d->line;
            it.exec(s);
        }

        if (wantMem || wantTrace) it.buildCellInfo();
        it.tracing = wantTrace;

        // 执行 O{} 体
        int rc = 0;
        try {
            it.execBody(p.mainBody);
        } catch (ReturnEx& r) {
            rc = (int)(r.v.ty == TY_DBL || r.v.ty == TY_FLOAT ? (long long)r.v.d : r.v.i);
        } catch (BreakEx&) {
            fprintf(stderr, "错误: '!' 只能在循环内使用\n");
            rc = 2;
        }
        if (wantMem) it.dumpMem(stdout, memAll);
        return rc;
    } catch (Err& e) {
        fprintf(stderr, "错误%s: %s\n", e.line ? fmt("（第 %lld 行）", e.line).c_str() : "", e.what());
        return 2;
    } catch (exception& e) {
        fprintf(stderr, "内部错误: %s\n", e.what());
        return 3;
    }
}
