#!/bin/bash
# O 语言 · Linux 测试脚本（替代 run_tests.bat / run_tests.ps1）
# 用法: ./run_tests.sh [--fast] [--no-build]
#   --fast      跳过 fib(30) 这类慢用例
#   --no-build  直接用已有的 ./olang
cd "$(dirname "$0")" || exit 1
CXX=${CXX:-g++}
BIN=./olang
FAST=0; NOBUILD=0
for a in "$@"; do
    [ "$a" = "--fast" ] && FAST=1
    [ "$a" = "--no-build" ] && NOBUILD=1
done

if [ $NOBUILD = 0 ]; then
    echo "== 编译 =="
    $CXX -std=c++17 -O2 -w -o "$BIN" olang.cpp || { echo "编译失败"; exit 1; }
else
    echo "== 跳过编译（--no-build）=="
fi

norm() { sed 's/[[:space:]]*$//' | sed -e :a -e '/^\n*$/{$d;N;ba' -e '}'; }

pass=0; fail=0; skip=0
echo "== 输出用例（examples/）=="
for f in examples/*.o; do
    n=$(basename "$f" .o)
    exp="examples/$n.expected"
    [ -f "$exp" ] || continue
    if [ "$n" = "fib" ] && [ $FAST = 1 ]; then echo "[SKIP] fib          （--fast）"; skip=$((skip+1)); continue; fi
    fin=/dev/null; [ -f "${f%.o}.in" ] && fin="${f%.o}.in"
    got=$(timeout 120 "$BIN" "$f" < "$fin" 2>&1 | norm)
    want=$(norm < "$exp")
    if [ "$got" = "$want" ]; then pass=$((pass+1)); echo "[ OK ] $n"
    else fail=$((fail+1)); echo "[FAIL] $n"; echo "   期望: $want"; echo "   实际: $got"; fi
done

echo "== 错误用例（tests/err，期望非 0 退出）=="
for f in tests/err/*.o; do
    n=$(basename "$f" .o)
    out=$(timeout 60 "$BIN" "$f" </dev/null 2>&1); rc=$?
    if [ $rc -ne 0 ]; then pass=$((pass+1)); echo "[ OK ] err/$n -> $(printf '%s' "$out" | head -1)"
    else fail=$((fail+1)); echo "[FAIL] err/$n（本应报错）"; fi
done

echo "== 双后端一致性（tests/probe：解释 vs --cc）=="
cc_run() {   # 这台手机上 gcc/LLVM 偶发 OOM 段错误，失败一次就重试
    CC_OUT=$(timeout 60 "$BIN" "$1" --cc </dev/null 2>&1); CC_RC=$?
    if [ $CC_RC -ne 0 ] && printf '%s' "$CC_OUT" | grep -q 'Segmentation fault\|PLEASE submit a bug report'; then
        CC_OUT=$(timeout 60 "$BIN" "$1" --cc </dev/null 2>&1); CC_RC=$?
    fi
}
for f in tests/probe/*.o; do
    n=$(basename "$f" .o)
    a=$(timeout 60 "$BIN" "$f" </dev/null 2>&1); ra=$?
    cc_run "$f"; b=$CC_OUT; rb=$CC_RC
    if [ "$a" = "$b" ] && [ $ra -eq $rb ]; then pass=$((pass+1)); echo "[ OK ] probe/$n"
    else fail=$((fail+1)); echo "[DIFF] probe/$n"; echo "   解释[$ra]: $a"; echo "   --cc[$rb]: $b"; fi
done

echo "== C 互操作（tests/cc，仅 --cc）=="
for f in tests/cc/*.o; do
    n=$(basename "$f" .o)
    lib=$(grep -o '#link "/[^"]*"' "$f" | sed 's/#link "//; s/"$//')
    if [ -n "$lib" ] && [ ! -e "$lib" ]; then
        skip=$((skip+1)); echo "[SKIP] cc/$n（缺 $lib）"; continue
    fi
    got=$(timeout 120 "$BIN" "$f" --cc </dev/null 2>&1 | norm)
    want=$(norm < "tests/cc/$n.expected")
    if [ "$got" = "$want" ]; then pass=$((pass+1)); echo "[ OK ] cc/$n"
    else fail=$((fail+1)); echo "[FAIL] cc/$n"; echo "   期望: $want"; echo "   实际: $got"; fi
done
cat > /tmp/.handle_arith.o <<'OO'
extern int sqlite3_close(handle)
O{
    &{handle;h} = {}
    .{int; ${h} + {1}}
}
OO
if "$BIN" /tmp/.handle_arith.o 2>&1 | grep -q 'handle 只能比较'; then
    pass=$((pass+1)); echo "[ OK ] handle 禁止算术"
else fail=$((fail+1)); echo "[FAIL] handle 禁止算术"; fi
rm -f /tmp/.handle_arith.o
if "$BIN" tests/cc/math.o </dev/null 2>&1 | grep -q 'extern 函数'; then
    pass=$((pass+1)); echo "[ OK ] 解释器拒绝 extern"
else fail=$((fail+1)); echo "[FAIL] 解释器拒绝 extern"; fi

echo "== 工具自检（--map --json / --mem / --trace）=="
if "$BIN" examples/sum.o --map --json 2>/dev/null | python3 -c 'import json,sys
d=json.load(sys.stdin)
assert d["mem_size"]==1024 and d["stack_top"]==1023
assert any(e["name"]=="a" and e["elems"]==10 and e["addr"]==0 for e in d["entries"])' 2>/dev/null; then
    pass=$((pass+1)); echo "[ OK ] --map --json"
else fail=$((fail+1)); echo "[FAIL] --map --json"; fi

if "$BIN" examples/sum.o --map 2>/dev/null | grep -q '^55$'; then
    fail=$((fail+1)); echo "[FAIL] --map 不应执行程序"
else pass=$((pass+1)); echo "[ OK ] --map 只输出映射表"; fi

if "$BIN" examples/sum.o --mem 2>/dev/null | awk '$4=="a[9]" && $3==10 {ok=1} END{exit !ok}'; then
    pass=$((pass+1)); echo "[ OK ] --mem 变量标注"
else fail=$((fail+1)); echo "[FAIL] --mem 变量标注"; fi

lines=$("$BIN" examples/sum.o --trace 2>&1 >/dev/null | wc -l)
if [ "$lines" -gt 10 ] && "$BIN" examples/sum.o --trace 2>&1 >/dev/null | grep -q 'writes:'; then
    pass=$((pass+1)); echo "[ OK ] --trace（$lines 条语句）"
else fail=$((fail+1)); echo "[FAIL] --trace"; fi

if "$BIN" examples/sum.o --mem-size 64 --map --json 2>/dev/null | python3 -c 'import json,sys; assert json.load(sys.stdin)["mem_size"]==64' 2>/dev/null; then
    pass=$((pass+1)); echo "[ OK ] --mem-size"
else fail=$((fail+1)); echo "[FAIL] --mem-size"; fi

if "$BIN" tests/probe/p_bigmem.o --mem-size 1024 2>&1 | grep -q '地址区间越界'; then
    pass=$((pass+1)); echo "[ OK ] --mem-size 过小会拒绝"
else fail=$((fail+1)); echo "[FAIL] --mem-size 过小会拒绝"; fi

echo
echo "== 合计: $pass 通过 / $fail 失败 / $skip 跳过 =="
exit $([ $fail -eq 0 ] && echo 0 || echo 1)
