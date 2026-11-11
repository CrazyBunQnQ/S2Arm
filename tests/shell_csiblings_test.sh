#!/bin/bash
# 门禁只看：pack/shell/closure.py pack/stage_inputs/shell.tsv *.c *.m *.mm *.cpp *.h *.hpp *.inc
# 壳输入表同目录 C 系核对（pack/shell/closure.py csiblings，不出壳的静态核对，约 1 秒）：bash tests/shell_csiblings_test.sh
# 起因（11-10 v0.2.3 发版）：单只 Joy-Con 2 新增 runtime/ns2pro/NS2JoyCon.c 编进 iOS 壳，没登记进 pack/stage_inputs/shell.tsv，
#   列车没有出壳这一步，发版出壳时闭包检查才红。这里在列车上就拦。
# 期望（按规则手写）：临时 git 仓库，表里「入 a/x.c」
#   ① 同目录新增 a/y.c 没登记 → 红，点名 a/y.c；② 加「入 a/y.c」→ 绿；③ 同目录 a/z_test.c 记「不」→ 绿；
#   ④ 别的目录 b/w.c 没登记（表里没有逐文件登记 b/ 下的东西）→ 不管，绿；⑤ 同目录 a/notes.txt（不是 C 系）→ 不管，绿；
#   ⑥ 整目录登记「入 c」，c/k.c 不用逐个登记 → 绿；⑦ 真仓库：当前表下 0 个漏登记
set -u
H=$(cd "$(dirname "$0")/.." && pwd); C=$H/pack/shell/closure.py; T=$(mktemp -d); bad=0
trap 'rm -rf "$T"' EXIT
ck() { if eval "$2"; then echo "通 $1"; else echo "不通 $1"; bad=1; fi; }
R=$T/r; mkdir -p "$R/pack/stage_inputs" "$R/a" "$R/b" "$R/c"; git -C "$R" init -q
echo x > "$R/a/x.c"; echo y > "$R/a/y.c"; echo z > "$R/a/z_test.c"; echo w > "$R/b/w.c"; echo n > "$R/a/notes.txt"; echo k > "$R/c/k.c"
printf '入\ta/x.c\n不\ta/z_test.c\n入\tc\n' > "$R/pack/stage_inputs/shell.tsv"; git -C "$R" add -A
o1=$(python3 "$C" csiblings "$R" 2>&1); r1=$?
ck "① a/y.c 没登记 → 红并点名（rc=${r1}）" '[ $r1 = 1 ] && grep -q "a/y.c" <<< "$o1" && [ "$(grep -c "【红】" <<< "$o1")" = 1 ]'
printf '入\ta/y.c\n' >> "$R/pack/stage_inputs/shell.tsv"
o2=$(python3 "$C" csiblings "$R" 2>&1); r2=$?
ck "②–⑥ 登记后绿；「不」、别的目录、非 C 系、整目录都不误报（rc=${r2}）" '[ $r2 = 0 ]'
o7=$(python3 "$C" csiblings "$H" 2>&1); r7=$?
ck "⑦ 真仓库当前表 0 个漏登记（rc=${r7}）" '[ $r7 = 0 ]'
[ $bad = 0 ] && echo "shell_csiblings 单测全过"; exit $bad
