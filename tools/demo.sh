#!/bin/bash
# 五个能力一次看完：Hello / 地址表 / 别名 / 内存快照 / 轨迹 / 生成网页
cd "$(dirname "$0")/.." || exit 1
BIN=./olang
[ -x "$BIN" ] || { echo "先编译：make"; exit 1; }
hr() { printf '── %s ' "$1"; printf '%.0s─' $(seq 1 $((60 - ${#1}))); echo; }

hr "① Hello"
$BIN examples/hello.o

hr "② 编译期地址表（--map）"
$BIN examples/sum.o --map

hr "③ 别名：两个名字，一段内存"
$BIN examples/alias.o

hr "④ 内存快照（--mem，只列非零单元）"
$BIN examples/sms.o --mem | sed -n '/内存 dump/,$p' | head -9

hr "⑤ 语句级轨迹（--trace 前 6 条）"
$BIN examples/demo.o --trace 2>&1 >/dev/null | sed -n '1,6p'

hr "⑥ 直接生成网页（report.o）"
$BIN examples/report.o > /tmp/olang-demo.html
echo "→ /tmp/olang-demo.html ($(wc -c < /tmp/olang-demo.html) 字节，含内联 JS 图表)"

hr "⑦ 生成 C 再编译（--cc，与解释执行同一套布局）"
$BIN examples/sum.o --cc
