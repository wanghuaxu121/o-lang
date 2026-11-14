# O 语法速查（v2.0 新语法）

> 旧语法（`&{int;x}` / `${x}` / `f{}` / `?{}`）在 v2.1 前仍可解析，但新代码一律用本页写法。

> **实况（2026-11-14，实测）**：本页 §1/§2/§4/§5 的标量、数组、函数、引用参数、控制流
> 均已实现；下列写法**尚未实现**，会直接报语法错误：
> `for` 循环。
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
struct P { int x; int y; }   // 新式声明（等价于旧语法 &{struct;P} = …）
P p;                      // 结构体变量
void show(int a) { print(a); }   // void = 隐式 return 0
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
s.len()   s.find(",")   s.substr(2, 1)        // 方法调用（带参方法降级为函数调用）
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

## 5. 数组参数

```o
int sum(int a[0..3]) {          // 拿到 4 个单元（降级为引用参数）
    int i = 0, s = 0;
    while (i < 4) { s = s + a[i]; i = i + 1; }
    return s;
}
O{ int v[0..3] = {1,2,3,4}; print(sum(v)); }   // 10
```

数组参数是**引用**语义：函数里改 `a[i]` 会改到调用者的数组。

> ⚠️ 结构体**只能按引用传参**（`P& p` 或 `P p[0..N]`）：按值传会只拷第一个单元，
> 所以直接报编译错误。

## 5.5 块级作用域

```o
O{
    int i = 100;
    while (k < 2) { int i = k; print(i); }   // 内层 i 遮蔽外层
    print(i);                                 // 还是 100
}
```

实现方式：内层重名时编译器给变量改名（`i#1`），出块后恢复。

## 6. 引用参数

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
