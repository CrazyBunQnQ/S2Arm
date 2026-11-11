#!/bin/bash
# 门禁只看：android/gate.sh
# 安卓 APK 门禁与合入后钩子 prepare 串行（11-01：两边容器共用缓存卷，同时 dotnet publish 锁死 50 分钟）单测：抽出 run_apk_gate、步骤换桩，约 0 秒
# 期望（按规则手写）：① 锁被活进程占着、等待上限 0 → 不跑 gate-apk、打待宿主证、返回 0；①b 上限内锁被放掉 → 等到锁照跑、不报待宿主证；② 没锁 → Darwin 只跑宿主 gate-apk（docker 步跳过，#1299）、Linux docker + 宿主两路都跑，跑完锁删掉；③ 锁被死进程占着 → 收回照跑（两种宿主同 ②）
set -u; H=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d); trap 'rm -rf "$T"' EXIT; bad=0
ck() { if eval "$2"; then echo "通 $1"; else echo "不通 $1"; bad=1; fi; }
s2a_gate_step() { echo "$*" >> "$T/calls"; }
OS=Linux; uname() { echo "$OS"; }
eval "$(awk '/^run_apk_gate\(\) \{/ { f = 1 } f { print } f && /^\}/ { exit }' "$H/android/gate.sh")"
export S2A_ANDROID_PREPARE_LOCK=$T/lock S2A_ANDROID_AUTO_PREPARE=0   # 自动备料另测（④），这里只看锁
mkdir $T/lock; sleep 300 & live=$!; echo $live > $T/lock/pid
o=$(S2A_ANDROID_PREPARE_WAIT_S=0 run_apk_gate); r=$?; kill $live 2> /dev/null   # 等待上限 0 = 旧行为：一撞上就报待宿主证
ck "① 准备在跑：不跑 gate-apk、打待宿主证、返回 0（rc=${r}）" '[ $r = 0 ] && ! grep -q gate-apk $T/calls && grep -q "待宿主证	安卓宿主	apk" <<< "$o" && [ -d $T/lock ]'
# ①b（11-10：安卓门禁合入前也跑后，撞上 prepare 就被列车按缺宿主证明退回）等待上限内锁被放掉 → 等到锁、照跑 gate-apk、不报待宿主证
rm -f $T/calls; rm -rf $T/lock; mkdir $T/lock; ( sleep 3; rm -rf $T/lock ) & sleep 30 & live=$!; echo $live > $T/lock/pid
o=$(S2A_ANDROID_PREPARE_WAIT_S=60 run_apk_gate); r=$?; kill $live 2> /dev/null
ck "①b 等待上限内 prepare 做完：等到锁照跑 gate-apk、不报待宿主证（rc=${r}）" '[ $r = 0 ] && grep -q gate-apk $T/calls && ! grep -q "待宿主证" <<< "$o" && grep -q "先等它做完" <<< "$o"'
for OS in Darwin Linux; do
  [ $OS = Darwin ] && nd=0 || nd=1
  rm -rf $T/lock $T/calls; o=$(run_apk_gate); r=$?
  ck "② ${OS} 没锁：docker gate-apk ${nd} 次、宿主 gate-apk 1 次、锁删掉（rc=${r}）" '[ $r = 0 ] && [ "$(grep -c docker/gate-apk $T/calls)" = $nd ] && [ "$(grep -c host/gate-apk $T/calls)" = 1 ] && [ ! -e $T/lock ]'
  rm -f $T/calls; mkdir $T/lock; echo 999999 > $T/lock/pid; o=$(run_apk_gate); r=$?
  ck "③ ${OS} 锁被死进程占着：收回照跑，docker ${nd} 次、宿主 1 次（rc=${r}）" '[ $r = 0 ] && [ "$(grep -c docker/gate-apk $T/calls)" = $nd ] && [ "$(grep -c host/gate-apk $T/calls)" = 1 ] && [ ! -e $T/lock ]'
done
# ④ 缺准备完成标记（只核标记退 42）→ 先跑宿主 prepare.sh 再证；有标记（退 0）→ 不跑 prepare。bash 替身代替真脚本，只看调用顺序
bash() { case "$*" in *host/gate-apk.sh) if [ -n "${S2A_APK_RECEIPT_CHECK:-}" ]; then return "$RC"; fi;; *host/prepare.sh) echo prepare >> $T/calls; return 0;; esac; command bash "$@"; }
for RC in 42 0; do rm -rf $T/lock; : > $T/calls; OS=Darwin; o=$(S2A_ANDROID_AUTO_PREPARE=1 run_apk_gate); r=$?
  [ $RC = 42 ] && want=1 || want=0
  ck "④ 只核标记退 ${RC}：prepare 跑 ${want} 次、且在宿主 gate-apk 之前（rc=${r}）" '[ $r = 0 ] && [ "$(grep -c "^prepare$" $T/calls)" = $want ] && { [ $want = 0 ] || [ "$(grep -n -m1 "^prepare$" $T/calls | cut -d: -f1)" -lt "$(grep -n -m1 host/gate-apk $T/calls | cut -d: -f1)" ]; }'
done
unset -f bash timeout
[ $bad = 0 ] && echo "android_apk_lock 单测全过" || { echo "android_apk_lock 单测有不通"; exit 1; }
