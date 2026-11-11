#!/bin/bash
# 门禁只看：tools/post_android_prepare.sh
# 合入后钩子「安卓 APK 准备标记跟上 main」单测（假仓库：两路 gate-apk.sh / prepare.sh 换成桩，约 1 秒）：bash tests/post_android_prepare_test.sh
# 期望（按钩子规则手写）：① 宿主标记相符 → 不跑 prepare、守望 last_checked 留着；② 宿主缺 / 不匹配（只核标记退 42）→ 跑宿主 prepare、删 last_checked；
#   ③ prepare 失败 → 退 1、last_checked 留着；④ 只核标记退别的码（不是备料能修的）→ 不跑 prepare、退 0；⑤ 锁被死进程占着 → 收回照跑；
#   ⑥ docker 路标记缺（macOS 门禁不跑 docker 步，#1327）→ 不核不备、退 0、last_checked 留着
set -u; H=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d); trap 'rm -rf "$T"' EXIT; bad=0
ck() { if eval "$2"; then echo "通 $1"; else echo "不通 $1"; bad=1; fi; }
[ "$(uname -s)" = Darwin ] || { echo "post_android_prepare：钩子只在 macOS 门禁机跑，非 macOS 只核跳过"; o=$(cd "$T" && bash "$H/tools/post_android_prepare.sh" 2>&1); r=$?
  [ $r = 0 ] && grep -q '非 macOS' <<< "$o" && { echo "post_android_prepare：非 macOS 只核了跳过分支，其余要 macOS（环境未就绪）"; exit 75; }; echo "非 macOS 没跳过：$o"; exit 1; }
mk() {   # mk <目录>：假仓库；两路桩读 <目录>/<路>.check（只核标记的退出码）、<目录>/<路>.prep（prepare 退出码），调用记进 calls
  local d=$1 r; mkdir -p "$d/td/watch"
  for r in docker host; do mkdir -p "$d/android/$r"
    printf '#!/bin/bash\n[ "${S2A_APK_RECEIPT_CHECK:-}" = 1 ] || { echo 不该出包; exit 9; }\nc=$(cat %s/%s.check)\n[ "$c" = 42 ] && echo "需备料：缺准备完成标记"\nexit $c\n' "$d" "$r" > "$d/android/$r/gate-apk.sh"
    printf '#!/bin/bash\necho %s >> %s/calls\nexit $(cat %s/%s.prep)\n' "$r" "$d" "$d" "$r" > "$d/android/$r/prepare.sh"
    echo 0 > "$d/$r.check"; echo 0 > "$d/$r.prep"; done
  echo deadbeef > "$d/td/watch/last_checked"; }
run() { (cd "$1" && S2A_TRAIN_DIR="$1/td" S2A_ANDROID_PREPARE_LOCK="$1/lock" bash "$H/tools/post_android_prepare.sh" > "$1/out" 2>&1); }
mk $T/a; run $T/a; r=$?
ck "① 宿主相符：退 0、不跑 prepare、last_checked 留着（rc=${r}）" '[ $r = 0 ] && [ ! -e $T/a/calls ] && [ -e $T/a/td/watch/last_checked ]'
mk $T/b; echo 42 > $T/b/host.check; run $T/b; r=$?
ck "② 宿主退 42：跑宿主 prepare、删 last_checked（rc=${r}）" '[ $r = 0 ] && [ "$(cat $T/b/calls)" = host ] && [ ! -e $T/b/td/watch/last_checked ]'
mk $T/c; echo 42 > $T/c/host.check; echo 1 > $T/c/host.prep; run $T/c; r=$?
ck "③ host prepare 失败：退 1、last_checked 留着（rc=${r}）" '[ $r = 1 ] && [ "$(cat $T/c/calls)" = host ] && [ -e $T/c/td/watch/last_checked ]'
mk $T/d; echo 3 > $T/d/host.check; run $T/d; r=$?
ck "④ 只核标记退 3（备料修不了）：不跑 prepare、退 0（rc=${r}）" '[ $r = 0 ] && [ ! -e $T/d/calls ] && grep -q "不是备料能修的" $T/d/out'
mk $T/e; mkdir $T/e/lock; echo 999999 > $T/e/lock/pid; echo 42 > $T/e/host.check; run $T/e; r=$?
ck "⑤ 锁被死进程占着：收回照跑（rc=${r}）" '[ $r = 0 ] && [ "$(cat $T/e/calls)" = host ] && [ ! -e $T/e/lock ]'
mk $T/f; echo 42 > $T/f/docker.check; echo 1 > $T/f/docker.prep; run $T/f; r=$?
ck "⑥ docker 退 42（macOS 不跑 docker 步）：不备、退 0、last_checked 留着（rc=${r}）" '[ $r = 0 ] && [ ! -e $T/f/calls ] && [ -e $T/f/td/watch/last_checked ] && ! grep -q docker $T/f/out'
[ $bad = 0 ] && echo "post_android_prepare 单测全过" || { cat $T/*/out 2> /dev/null | tail -20; echo "post_android_prepare 单测有不通"; exit 1; }
