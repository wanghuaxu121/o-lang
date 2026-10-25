# 30 秒上手

需要 `g++` 或 `clang++`（C++17）。没有别的依赖。

## 1. 编译

```bash
make
./olang --version
```

## 2. 第一行 O

```bash
$ ./olang examples/hello.o
Hello, O!
```

## 3. 看地址从哪来

```bash
$ ./olang examples/sum.o --map
 addr  名字
    0  int      x10  size=10  a
   10  int      x1   size=1  i
   11  int      x1   size=1  s
 静态区结束 = 12, 栈从 1023 向下
```

## 4. 看别名（O 的招牌）

```bash
$ ./olang examples/alias.o
7
```

`words` 和 `first` 是两个名字、同一段地址，写一个另一个立刻看得见。源码只有 8 行，见 `examples/alias.o`。

## 5. 看内存快照

```bash
$ ./olang examples/sms.o --mem | tail -12
 addr  类型     值                     名字
    0  int      1                      a[0]
    ...
（非零单元 12 / 1024；--mem=all 显示全部）
```

## 6. 单步看每条语句

```bash
$ ./olang examples/demo.o --trace 2>&1 >/dev/null | head -6
[     1] line 39   decl    sp=1024  writes: 0=5 1=3 2=9 3=1 4=7 5=2 6=8 7=4
[     2] line 41   out     sp=1024
```

stdout 仍然是程序自己的输出，轨迹走 stderr —— `2>/dev/null` 只看结果，`1>/dev/null` 只看轨迹。

## 7. 同一个程序，两种执行

```bash
./olang examples/sum.o            # 树遍历解释器
./olang examples/sum.o --cc       # 生成 C → gcc -O2 → 执行（首次编译约 2 s，之后走缓存）
./olang examples/sum.o --emit-c   # 只打印生成的 C
```

两者的地址布局完全一致 —— `--map` 里的 0/10/11 就是生成 C 里的 `_mem[0] / _mem[10] / _mem[11]`。

## 8. 一次看完

```bash
tools/demo.sh
```

## 接下来

| 想做的事 | 看哪里 |
|---|---|
| 学语法 | `SPEC.md` §12 的四个可运行示例 |
| 看设计动机 | `docs/INTRO.md` |
| 改代码 | `olang.cpp`（单文件，3000 行，词法→codegen 全在里面） |
| 跑测试 | `make check`（89 项 + 11 条验收 + 覆盖率） |
| 编辑器高亮 | `editors/vscode/`（复制到扩展目录即可） |
