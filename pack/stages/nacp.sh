#!/bin/bash
# 出包·共享·元数据阶段（J07 #183）：pack/stages/nacp.sh <款> <xci / nsp 文件…>
# 与目标无关：产物是产物根里的 ctl/control.nacp 与过程日志 nacp.log，相对路径不含 ios / android / ohos；
#   元数据由游戏包本身决定，跟出到哪个平台无关，各目标插件只是把它带上。
# 实现搬自 pack/stage_nacp.sh（对外命令行与行为不变，旧入口只剩一层转发）：从游戏原始包（卡带 / 安装包 + 更新）
# 抽 ctl/control.nacp 进新版本产物根 <款>@nacp-<main 构建号前 8 位> 并切现行（pack/nx_unpack --nacp-only，
# 取最新更新的控制分区）。版本核对：包里 main 构建号须与产物根 exefs/main 的一致（元数据必须和程序同版本，由 nx_unpack 按构建号取）。
# 已有 ctl/control.nacp 的跳过（本阶段没有阶段键，原实现如此，本片不改语义）。
# 钥匙：S2A_KEYS（缺省 ~/.switch/prod.keys）；工具：S2A_NX_UNPACK（缺省 ~/.s2arm/run/s2arm/bin/nx_unpack/nx_unpack，由 pack/nx_unpack 构建）。
set -u
G=${1:?款}; shift; [ $# -gt 0 ] || { echo "【红】要给 xci / nsp 文件"; exit 1; }
H=$(cd "$(dirname "$0")/.." && pwd); source "$H/roots.sh"
N=${S2A_NX_UNPACK:-${S2A_RUN_ROOT:-$HOME/.s2arm/run}/s2arm/bin/nx_unpack/nx_unpack}; K=${S2A_KEYS:-$HOME/.switch/prod.keys}
[ -x "$N" ] || { echo "【红】没有 ${N}（dotnet build -c Release pack/nx_unpack -o $(dirname "$N")）"; exit 1; }
CUR=$(root_cur "$G"); [ -n "$CUR" ] && [ -f "$CUR/exefs/main" ] || { echo "【红】$G 现行产物根没有 exefs/main"; exit 1; }
[ -f "$CUR/ctl/control.nacp" ] && { echo "🟢 $G 已有 ctl/control.nacp，跳过"; exit 0; }
# 构建号用 od 读（P2-278：原先用 xxd，玩家机没装 xxd 时写出「nacp-」空构建号仍退 0；od 是系统自带，输出同 xxd -p）
BID=$(od -An -v -tx1 -j 64 -N 4 "$CUR/exefs/main" | tr -d ' \n')
[[ "$BID" =~ ^[0-9a-f]{8}$ ]] || { echo "【红】$G 读不出 exefs/main 的构建号（0x40 起 4 字节，读到「${BID}」）"; exit 1; }
TAG=nacp-$BID
TAG=$(root_tag "$G" "$TAG"); W=$(root_begin "$G" "$TAG") || exit 1
"$N" "$K" "$W" --nacp-only "$@" > "$W/nacp.log" 2>&1 || { echo "【红】${G}：$(tail -1 "$W/nacp.log")"; root_abort "$G" "$TAG"; exit 1; }
root_commit "$G" "$TAG" || exit 1
echo "$(tail -1 "$(root_cur "$G")/nacp.log")"
