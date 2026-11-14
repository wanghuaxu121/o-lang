# O 语法速查（v2.0 新语法）

> 旧语法（`&{int;x}` / `${x}` / `f{}` / `?{}`）在 v2.1 前仍可解析，但新代码一律用本页写法。

> **实况（2026-11-14，实测）**：本页 §1/§2/§4/§5 的标量、数组、函数、引用参数、控制流
> 均已实现；下列写法**尚未实现**，会直接报语法错误：
> `void` 返回类型、`struct P { ... }` 新式声明、
> 带参方法 `s.find(",")`、数组形参 `int f(int a[0..3])`、`for` 循环。
> 字符字面量 `'A'` 与一元 `!x` 已实现（`'A'` 的类型是 int，存进 `char` 变量才按字符输出）。

## 1. 程序结构

```o
#{"file"}                 // 导入库（sys / file / str）
#link "m"                 // 链接 C 库
#mem 4096                 // 地址空间大小（默认 16384 单元 = 128 KB）

int g = 0;                // 全局变量

int add(int a, int b) {   // 函数
    return a + b;
}

O{                        // 入口
    print(add(1, 2));
}
```

## 2. 变量与类型

```o
int x = 5;                // 自动分配地址
double d = 3.14;
bool ok = true;
char c = 'A';             // 字符字面量
str s = "hello";

int a[0..9];              // 显式区间：10 个单元，地址 0..9
int b[10..19] = {1};      // 带初值（其余补 0）
struct P { int x; int y; }
P p;                      // 结构体
```

类型：`int` `double` `float` `bool` `char` `str` `handle` `fn` `struct`

## 3. 表达式

```o
x + y    x - y    x * y    x / y    x % y
x == y   x != y   x < y    x <= y   x > y   x >= y
x && y   x || y   !x
x & y    x | y    x ^ y    x << n   x >> n     // 位运算（32 位，移位计数 & 31）
0x1F                                            // 十六进制字面量
a[i]      p.x      f(1, 2)                     // 索引 / 字段 / 调用
s.len()   find(s, ",")                        // 无参方法可用；带参方法尚未实现
"a" + "b"                                      // 字符串拼接
```

## 4. 语句

```o
print(x);                 // 输出（类型自动）
print("hi\n");
x = 5;
a[i] = 7;
p.x = 1;

if (c) { ... } else { ... }
while (c) { ... }
return x;
break;
continue;

,{int; x}                 // 输入（保留原语法）
```

## 5. 引用参数

```o
void bump(int& x) { x = x + 1; }     // 声明侧标 &
O{ int v = 1; bump(v); print(v); }   // 调用侧直接传变量
```

## 6. 旧语法对照

| 旧 | 新 |
|---|---|
| `&{int;x} = {5}` | `int x = 5;` |
| `&{int;a} = {int{0;9}}` | `int a[0..9];` |
| `&{int;add} = o{{int;a},{int;b}}{ ?{${a}+${b}} }` | `int add(int a, int b) { return a + b; }` |
| `${x}` | `x` |
| `f{${x}}` | `f(x)` |
| `${a}{${i}}` | `a[i]` |
| `.{int; ${x}}` | `print(x);` |
| `(c){...}` | `if (c) { ... }` |
| `[c]{...}` | `while (c) { ... }` |
| `?{e}` | `return e;` |
| `!` | `break;` |
| `x \| f` | `f(x)` |
