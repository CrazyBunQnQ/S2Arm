#!/bin/bash
# 门禁覆盖：tools/gate.sh
# 门禁台账（tools/gate.sh 的 GL / gl_put，11-10 降门禁压力先量）：假仓库两支单测（foo 绿、red 红），真跑 tools/gate.sh。约 30 秒。期望按规则手写：
#   ① GATE_LEDGER=<文件>：每步一行 JSON；foo 那行 rc=0、host=本机、key 是十六进制缓存键、hit=0、wait 是非负整数、role=预跑（不给 GATE_ROLE；GATE_NO_LOADQ=1 也不推断成列车）、rev=本仓 HEAD 前 12 位；red 那行 rc=1
#   ② 再跑一遍：foo 那行 hit=1（缓存命中也记）
#   ③ GATE_ROLE=热修 → role=热修
#   ④ 不给 GATE_LEDGER：仓库在临时目录（单测假仓库）→ 缺省台账文件不生成；⑤ GATE_LEDGER=- → 不写
# 反证：S2A_GATE_SH=<旧版 gate.sh> 跑本单测应红（旧版没有台账）
set -u; H=$(cd "$(dirname "$0")/.." && pwd); T=$(cd "$(mktemp -d)" && pwd -P); bad=0; GS=${S2A_GATE_SH:-$H/tools/gate.sh}
ck() { if eval "$2"; then echo "通 $1"; else echo "不通 $1"; bad=1; fi; }
trap 'rm -rf "$T"' EXIT
unset GATE_TIER GATE_CHANGED GATE_PLAN GATE_OUT GATE_HOST GATE_REV GATE_REMOTE_NAME GATE_SKIP GATE_ONLY GATE_HOOK_S GATE_LAYER GATE_TEST_CACHE GATE_RERUN GATE_LIST GATE_LEDGER GATE_ROLE GATE_IN_STEP
export S2A_REMOTE_FREE_GB=999 GATE_NO_LOADQ=1 S2A_RUN_ROOT=$T/run HOME=$T/home S2A_TRAIN_NO_POST=1
mkdir -p $T/home $T/repo/tools $T/repo/tests; cd $T/repo || exit 1
git init -q; git config user.email t@t; git config user.name t
cp "$GS" tools/gate.sh; cp $H/tools/test_cache.py $H/tools/gate_route.py $H/tools/gate_decl.py $H/tools/idle_timeout.sh $H/tools/safe_killpg.sh tools/
echo build/ > .gitignore; echo 'X=1' > tools/toolchain.env; echo ': # 单测桩' > tools/toolchain.sh
printf '#!/bin/bash\necho foo 过\n' > tests/foo_test.sh; printf '#!/bin/bash\necho red 红; exit 1\n' > tests/red_test.sh
git add -A; git commit -qm 1; REV=$(git rev-parse --short=12 HEAD)
# 取某步那行的若干字段（| 分隔）；没有该行 = 空
fld() { python3 -c 'import json, sys
rows = [json.loads(l) for l in open(sys.argv[1]) if l.strip()] if __import__("os").path.exists(sys.argv[1]) else []
r = [x for x in rows if sys.argv[2] in x["step"]][-1:] 
print("|".join(str(r[0][k]) for k in sys.argv[3:]) if r else "")' "$@"; }
L1=$T/l.jsonl
GATE_LEDGER=$L1 bash tools/gate.sh > $T/o1 2>&1
f=$(fld $L1 foo_test rc host hit role rev); k=$(fld $L1 foo_test key); w=$(fld $L1 foo_test wait)
ck "① foo 那行 rc=0、本机、hit=0、role=预跑、rev=HEAD（真结果：${f}）" '[ "$f" = "0|本机|0|预跑|$REV" ]'
ck "① foo 有缓存键（十六进制）、wait 非负整数（键=${k} wait=${w}）" '[[ $k =~ ^[0-9a-f]{16,}$ ]] && [[ $w =~ ^[0-9]+$ ]]'
ck "① red 那行 rc=1（真结果：$(fld $L1 red_test rc)）" '[ "$(fld $L1 red_test rc)" = 1 ]'
GATE_LEDGER=$L1 bash tools/gate.sh > $T/o2 2>&1
ck "② 再跑：foo 那行 hit=1（真结果：$(fld $L1 foo_test hit)）" '[ "$(fld $L1 foo_test hit)" = 1 ]'
GATE_LEDGER=$L1 GATE_ROLE=热修 bash tools/gate.sh > $T/o3 2>&1
ck "③ GATE_ROLE=热修 → role=热修（真结果：$(fld $L1 foo_test role)）" '[ "$(fld $L1 foo_test role)" = 热修 ]'
bash tools/gate.sh > $T/o4 2>&1
ck "④ 不给 GATE_LEDGER、仓库在临时目录 → 缺省台账不生成" '[ ! -e $T/run/s2arm/gate_ledger.jsonl ]'
n0=$(wc -l < $L1); GATE_LEDGER=- bash tools/gate.sh > $T/o5 2>&1
ck "⑤ GATE_LEDGER=- → 不写（行数 ${n0} → $(wc -l < $L1)）" '[ "$(wc -l < $L1)" = "$n0" ] && [ ! -e $T/run/s2arm/gate_ledger.jsonl ] && [ ! -e ./- ]'
[ $bad = 0 ] && echo "gate_ledger 单测全过" || { echo "gate_ledger 单测有不通"; sed -n 1,40p $T/o1; exit 1; }
