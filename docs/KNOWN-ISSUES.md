# 已知问题（v2.1）

## K1 · 结构体数组参数 + 引用计数在递归调用里算错【高优先级】

**复现**：`/root/dsh/bst-bug-repro.o`（arena BST：insert 递归 + `int& n` 计数）

```o
struct Node { int val; int left; int right; }
int alloc(Node t[0..31], int& n, int v) { … }
int insert(Node t[0..31], int& n, int root, int v) {
    if (root == -1) { return alloc(t, n, v); }
    if (v < t[root].val) { t[root].left = insert(t, n, t[root].left, v); }
    …
}
```

**现象**：两个后端一致地算错 —— 节点 1 的 `val` 是 -1（应为 30），
`left/right` 被写成了其他节点的下标。

**已排除**（都单独验证正确）：
- `alloc` 连续调用三次（直接调用）：`0:50 1:30 2:70` ✅
- `int& n` 传 `int&`（ref 传 ref）：`f(x)` → 7，x 变 8 ✅
- 递归 + `int&` + 结构体数组参数（最小用例）：`n=1 v=42 l=-1` ✅
- 结构体数组形参写字段：`50 -2 -3` ✅
- 生成的 C 里 `t[id].v` 展开为 `_mem[base + id*3 + 0]`，`n` 的读写是双重解引用 ✅

**新证据（插桩 alloc 打印实参）**：

```
alloc id=0 v=50 back=50     ← 正确
alloc id=1 v=-1 back=-1     ← 递归进来的 v 变成 -1（应为 30）
alloc id=5 v=2  back=2      ← v 变成了节点下标
```

说明 **4 参数函数的递归调用里，第 4 个实参（v）在进入被调者时被破坏**，
而 `root`（第 3 个实参）是对的。已被破坏的值恰好是 `-1` / 节点下标 ——
像是参数槽与局部槽发生了重叠。

**补充排除**：4 参数纯标量递归 `f(a,b,c,d)` 正确（312）；
参数槽布局 `paramOfs` 对每个参数都分配槽，`frameSize` 覆盖全部参数（已读代码确认）。

**决定性证据（在 insert 入口打印实参）**：

```
insert root=-1 v=50     ← 来自 O{}，正确
insert root=0  v=-1     ← 递归调用：本应是 root=-1, v=50
```

被调者收到的是 (0, -1)，而调用点写的是 `insert(t, n, t[root].left, v)` = (t, n, -1, 50)。
即 **第 3、4 个实参整shift 了一位**：root 拿到了调用者的 root，v 拿到了调用者的 child。

`--emit-c` 里这一行看起来是对的：`o_f1(t, _mem[_fp+1].i, _mem[(t+_v2*3)+1], _v3, _fp-4)`。
既然两个后端给出**完全相同**的错误结果，问题大概率在**共享的 AST 层**（调用实参列表），
而不是某个后端的发射代码。

**下一步**：
1. 打印 `CallE->kids.size()` 与每个 kid 的 kind（递归调用那处），确认 AST 是否只有 3 个实参
2. 重点怀疑：`t[root].left = insert(...)` 这种「左值是数组字段、右值递归」的求值顺序
   （C 里赋值左右侧的求值顺序未定义，生成代码把调用放右边、地址放左边，可能踩到）
3. 用 ASAN/UBSAN 编译 `olang` 跑复现，让越界写直接报位置（最快）
4. 对比 `Node t[0..31]` 改写成 `int t[0..93]`（手工展平）是否仍错，以确认是否 struct stride 相关

## K2 · 结构体只能按引用传参

按值传会只拷第一个单元，已改为编译错误（`e_structval.o`）。
要支持按值拷贝需要按 `cellsOf(struct)` 逐单元复制。
