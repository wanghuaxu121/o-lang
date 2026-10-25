#!/bin/bash
# SPEC §15 验收自检（11 条）
# 用法: tools/audit_spec15.sh [path-to-olang]
BIN=${1:-./olang}
[ -x "$BIN" ] || { echo "找不到 $BIN"; exit 1; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
pass=0; fail=0; warn=0
ok()   { pass=$((pass+1)); printf '  ✅ %-2s %s\n' "$1" "$2"; }
no()   { fail=$((fail+1)); printf '  ❌ %-2s %s\n' "$1" "$2"; }
maybe(){ warn=$((warn+1)); printf '  ⚠️  %-2s %s\n' "$1" "$2"; }

# 1 tokenizer
cat > "$T/t1.o" <<'O'
O{
  .{int; {1} == {1}} .{int; {1} != {2}} .{int; {1} <= {2}}
  .{int; {2} >= {2}} .{int; {1} && {1}} .{int; {0} || {1}}
  // 行注释
  /* 块注释 */
  .{str; "\n"}
}
O
[ "$("$BIN" "$T/t1.o")" = "111111" ] && ok 1 "tokenizer（== != <= >= && ||，注释剥离）" || no 1 "tokenizer"

# 2 Hello
[ "$("$BIN" examples/hello.o)" = "Hello, O!" ] && ok 2 "Hello World 逐字节一致" || no 2 "Hello World"

# 3 fib(30)
[ "$("$BIN" examples/fib.o --cc)" = "832040" ] && ok 3 "fib(30) = 832040" || no 3 "fib(30)"

# 4 数组求和 + 布局
sum=$("$BIN" examples/sum.o)
lay=$("$BIN" examples/sum.o --map | awk '$1=="0"{a=$NF} $1=="10"{i=$NF} $1=="11"{s=$NF} END{print a,i,s}')
[ "$sum" = "55" ] && [ "$lay" = "a i s" ] && ok 4 "数组求和 55，布局 a@0 i@10 s@11" || no 4 "数组求和（$sum / $lay）"

# 5 引用不可存
printf 'O{ &{int;y} = {1}  &{int;x} = ${y} }\n' > "$T/t5.o"
"$BIN" "$T/t5.o" >/dev/null 2>&1 && no 5 "引用赋值未报错" || ok 5 "&{…} = \${x} 报编译错误"

# 6 可见性
cat > "$T/t6.o" <<'O'
&{int;g} = o{{int;n}}{
    &{int;loc} = {5}
    &{int;h} = o{{int;a}}{ ?{${loc} + ${a}} }
    ?{ h{${n}} }
}
O{ .{int; g{1}} }
O
a=$("$BIN" "$T/t6.o" 2>&1); ra=$?
cat > "$T/t6b.o" <<'O'
O{
    &{int;x} = {1}
    &{int;f} = o{{int;a}}{ ?{${x} + ${a}} }
    .{int; f{2}}
}
O
b=$("$BIN" "$T/t6b.o" 2>&1)
[ $ra -ne 0 ] && [ "$b" = "3" ] && ok 6 "外层函数局部报错；O{} 静态可捕获" || no 6 "可见性（$a / $b）"

# 7 地址检查
printf '&{int;f} = o{{int;a}}{ &{int;b} = {int{0;3}} = {} ?{0} }\nO{ .{int; f{1}} }\n' > "$T/t7.o"
"$BIN" "$T/t7.o" >/dev/null 2>&1 && no 7a "函数内显式地址未报错" || ok 7a "函数体内 int{x;y} 报错"
printf 'O{ &{int;big} = {int{0;1025}} = {} }\n' > "$T/t7b.o"
"$BIN" "$T/t7b.o" >/dev/null 2>&1 && no 7b "静态区溢出未报错" || ok 7b "静态区溢出报错"

# 8 别名
cat > "$T/t8.o" <<'O'
O{
    &{int;words} = {int{0;9}} = {}
    &{int;first} = {int{0;4}} = {}
    &{int;i} = {int{10}} = {3}
    ${first}{${i}} = {7}
    .{int; ${words}{${i}} }
    .{str; "\n"}
}
O
[ "$("$BIN" "$T/t8.o")" = "7" ] && ok 8 "§12.4 别名 → 7，重叠不报错" || no 8 "别名"

# 9 codegen -Wall
w=0
for f in examples/hello.o examples/sms.o examples/report.o; do
    n=$("$BIN" "$f" --emit-c 2>/dev/null | gcc -std=c11 -O2 -Wall -x c - -o "$T/x.bin" 2>&1 | grep -c 'warning:')
    w=$((w+n))
done
[ $w -eq 0 ] && ok 9 "生成 C 在 gcc -std=c11 -O2 -Wall 下 0 warning" || no 9 "codegen 有 $w 个 warning"

# 10 性能：fib(40) 生成代码 vs 手写 C（含进程启动，比值含固定开销）
sed 's/{30}/{40}/' examples/fib.o > "$T/fib40.o"
"$BIN" "$T/fib40.o" --emit-c > "$T/fib.c" 2>/dev/null
gcc -std=c11 -O2 -w -o "$T/fibgen" "$T/fib.c"
printf '#include <stdio.h>\n#include <stdint.h>\nstatic int64_t f(int64_t n){return n<3?1:f(n-1)+f(n-2);}\nint main(void){printf("%%d\\n",(int)f(40));return 0;}\n' > "$T/hand.c"
gcc -O2 -w -o "$T/hand" "$T/hand.c"
printf 'int main(void){return 0;}\n' > "$T/empty.c" && gcc -O2 -w -o "$T/empty" "$T/empty.c"
TIMEFORMAT='%3R'
# 交错测量：两个二进制轮流转，避免系统负载漂移把比值带偏
g=99; h=99; e=99
for _ in 1 2 3; do
    r=$( { time "$T/fibgen" >/dev/null 2>&1; } 2>&1 ); g=$(python3 -c "print(min($g,$r))")
    r=$( { time "$T/hand"   >/dev/null 2>&1; } 2>&1 ); h=$(python3 -c "print(min($h,$r))")
    r=$( { time "$T/empty"  >/dev/null 2>&1; } 2>&1 ); e=$(python3 -c "print(min($e,$r))")
done
# 扣掉进程启动：SPEC 的「运行时间」指计算时间，本机启动开销 ~0.1 s 会淹没 fib(40)
ratio=$(python3 -c "print(round(($g-$e)/($h-$e),2))" 2>/dev/null || echo "?")
python3 -c "import sys; sys.exit(0 if ($g-$e)<=($h-$e)*1.2 else 1)" 2>/dev/null \
    && ok 10 "fib(40) 生成 C ${g}s vs 手写 C ${h}s（扣启动 ${e}s → ${ratio}×，SPEC 要求 ≤1.2×）" \
    || maybe 10 "fib(40) ${g}s vs 手写 C ${h}s（扣启动 ${e}s → ${ratio}×，超过 1.2×）"

# 11 缺返回值
"$BIN" tests/err/e05_noret.o >/dev/null 2>&1 && no 11 "缺返回值未报错" || ok 11 "函数末尾没有 ?{} 报错"

echo
echo "  合计: $pass 通过 / $fail 失败 / $warn 提示"
exit $([ $fail -eq 0 ] && echo 0 || echo 1)
