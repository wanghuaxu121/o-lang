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

**下一步**：
1. 用 `--trace` 逐条比对解释器的写地址，找出第一次写错的位置
2. 重点怀疑：`t[root].left = insert(...)` 这种「左值是数组字段、右值递归」的求值顺序
   （C 里赋值左右侧的求值顺序未定义，生成代码把调用放右边、地址放左边，可能踩到）
3. 若确认是求值顺序，修法：把左值地址**先算到临时变量**再调用（`int _t = addr; _mem[_t] = f(...)`）

## K2 · 结构体只能按引用传参

按值传会只拷第一个单元，已改为编译错误（`e_structval.o`）。
要支持按值拷贝需要按 `cellsOf(struct)` 逐单元复制。
