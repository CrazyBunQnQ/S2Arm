#!/bin/bash
# 阶段源码键（维护者 10-26 20:3x：直连库 / 抽取键收窄）：source 后 stage_files_key <阶段> [<仓库根>] → 打印 16 位键，取不到退出 1 并打【红】。
# 键 = pack/stage_inputs/<阶段>.tsv 里「入」那些文件在 HEAD 上的路径 + 对象号；「外」不进键。改了本表没列的文件（宿主运行时代码等）键不变，
#   改了「入」里任一文件键就变。表漏没漏由 tests/stage_keys_test.sh 用 tools/stage_inputs.py 扫依赖闭包核（闭包里有表外文件 → 红）
stage_files_key() {
  local st=${1:?阶段} r=${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)} t f out
  t=$r/pack/stage_inputs/$st.tsv; [ -f "$t" ] || { echo "【红】没有输入声明表 $t" >&2; return 1; }
  f=$(LC_ALL=C awk -F'\t' '$1 == "入" { print $2 }' "$t") || return 1
  [ -n "$f" ] || { echo "【红】$t 里没有「入」的文件" >&2; return 1; }
  # shellcheck disable=SC2086
  # 账本调用不吃工具链 env：上游解析后 DEVELOPER_DIR 指构建用的 Xcode，坏/假值会让 /usr/bin/git 垫片 xcrun 报红（stage_direct_test 夹具曾给 mktemp 目录复现）
  out=$(env -u DEVELOPER_DIR git -C "$r" ls-tree HEAD -- $f) || { echo "【红】取不到 $st 输入文件的对象号（git ls-tree）" >&2; return 1; }
  [ "$(printf '%s\n' "$out" | grep -c .)" = "$(printf '%s\n' "$f" | grep -c .)" ] \
    || { echo "【红】$t 里有文件不在 HEAD：$(printf '%s\n' "$f" | while read -r p; do grep -qF "	$p" < <(printf '%s\n' "$out") || printf '%s ' "$p"; done)（改名 / 删除要同笔改本表）" >&2; return 1; }
  printf '%s\n' "$out" | awk '{ print $3, $4 }' | shasum -a 256 | cut -c1-16
}

# 实际读入进判跳过（10-30 出包线根修，tools/stage_deps.py）：声明的输入算阶段键（各阶段原样，存进 .stage_<阶段>），实际读入另存清单 .stage_<阶段>.deps
#   （动态拼出的路径、数据表、函数里才导入的模块，静态扫不到；本阶段脚本也记进清单）。跳过要两条都成立：阶段键相同，且清单里每个文件的内容与当时相同。
#   stage_deps_on                                     阶段开头调一次（第一个 Python 调用之前）：之后起的每个 Python 进程都记读入（钩子 tools/stage_deps_hook，经 PYTHONPATH 自动装）
#   stage_deps_key  <现行根> <阶段> <基础键> <脚本…>  → 清单还对得上 = 原样打印基础键；对不上 / 没有清单 / 清单里没有这些脚本 = 打印「<基础键>~d」
#                                                       （判跳过必然不中；只算键模式记的也是它，P4 快路径拿它比登记包的键就不相等）
#   stage_deps_save <新版本根> <阶段> <基础键> <脚本…> → 本次读入 + 本阶段脚本整理成 <新版本根>/.stage_<阶段>.deps，原样打印基础键（写 .stage_<阶段>）
#   记什么：仓库里 git 跟踪 / 未忽略的文件（构建产物、缓存不算），仓库外只认 S2A_DEPS_EXTERNAL（冒号分隔，缺省社区补丁目录）下的；
#   S2A_DEPS_KEYED（冒号分隔、仓库相对路径）= 阶段已经更细地进键的文件（如验证账只取本款行），不再整份记
stage_deps_on() {
  local base d
  # 记录放在本款产物根旁（游戏库卷上，同 refresh.sh 的 $S2A_ROOTS/.refresh），不放系统临时目录：macOS 盘紧时会清 ${TMPDIR}，
  #   #265 怪猎崛起直连库编了 880 秒，收尾时记录目录已被清掉，整阶段判失败。没给产物根（单测单独 source）才退回 $TMPDIR
  #   作业临时目录 S2A_JOB_TMP（<游戏所在存放位置>/temp/<任务号>，和游戏数据同卷，作业开始建、结束删；启动器线游戏库 v3 定的接口）有就用它
  if [ -n "${S2A_JOB_TMP:-}" ]; then base=${S2A_JOB_TMP%/}/stage_deps; else base=${S2A_ROOTS:-${TMPDIR:-/tmp}}; base=${base%/}/.stage_deps; fi
  mkdir -p "$base" || return 1
  # 只清本机制自己的记录目录：名字全是数字（进程号）且进程已经不在的（跳过 / 只算键的路径不收尾）
  case $base in */.stage_deps|*/stage_deps) for d in "$base"/*; do [ -d "$d" ] || continue; case ${d##*/} in *[!0-9]*) continue;; esac; kill -0 "${d##*/}" 2> /dev/null || rm -rf "$d"; done;; esac
  _SD_DIR=$base/$$; rm -rf "$_SD_DIR"; mkdir -p "$_SD_DIR" || return 1
  export S2A_DEPS_OUT=$_SD_DIR PYTHONPATH="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/tools/stage_deps_hook${PYTHONPATH:+:$PYTHONPATH}"
}
_sd_py() { local p=${S2A_PYTHON:-${S2A_PY:-$(bash "$(_sd_repo)/tools/machine.sh" get S2A_PY 2> /dev/null)}}; echo "${p:-python3}"; }   # 开发机钉版 venv 在 tools/machine.local 声明
_sd_repo() { cd -P "$(dirname "${BASH_SOURCE[0]}")/.." && pwd; }
stage_deps_key() {
  local cur=$1 st=$2 base=$3 r m f rel; shift 3; r=$(_sd_repo); m=$cur/.stage_$st.deps
  [ -n "$cur" ] && [ -f "$m" ] || { echo "$base~d"; return 0; }
  for f in "$@" "$r/pack/stage_keyfiles.sh" "$r/tools/stage_deps.py" "$r/tools/stage_deps_hook/sitecustomize.py"; do   # 本阶段脚本与本机制自身要在清单里（旧清单没有 = 过期）
    f=$(cd -P "$(dirname "$f")" 2> /dev/null && pwd)/$(basename "$f"); case $f in "$r"/*) rel=R:${f#"$r"/};; *) rel=$f;; esac
    grep -q "^f	$rel	" "$m" || { echo "$base~d"; return 0; }
  done
  cmp -s <(env -u S2A_DEPS_OUT "$(_sd_py)" "$r/tools/stage_deps.py" rehash "$m" "$r") "$m" && echo "$base" || echo "$base~d"
}
stage_deps_save() {
  local w=$1 st=$2 base=$3 r f; shift 3; r=$(_sd_repo)
  [ -n "${_SD_DIR:-}" ] && [ -d "$_SD_DIR" ] || { echo "【红】$st 没开实际读入记录（阶段开头要调 stage_deps_on）" >&2; return 1; }
  for f in "$@" "$r/pack/stage_keyfiles.sh" "$r/tools/stage_deps.py" "$r/tools/stage_deps_hook/sitecustomize.py"; do printf 'f\t%s\n' "$(cd -P "$(dirname "$f")" && pwd)/$(basename "$f")"; done > "$_SD_DIR/scripts.tsv"
  env -u S2A_DEPS_OUT S2A_DEPS_EXTERNAL="${S2A_DEPS_EXTERNAL-${S2A_COMMUNITY_DIR:-$(bash "$r/tools/machine.sh" get S2A_COMMUNITY_DIR 2> /dev/null)}}" "$(_sd_py)" "$r/tools/stage_deps.py" collect "$_SD_DIR" "$r" \
    "${S2A_ROOTS:-}" "${S2A_ROOTS_SHARED:-}" "${S2A_ROOTS_PRIV:-}" "$w" > "$w/.stage_$st.deps" || { echo "【红】$st 整理实际读入清单失败" >&2; return 1; }
  rm -rf "$_SD_DIR"
  echo "$base"
}
