#!/usr/bin/env bash
# 门禁层：合入后（路由命中时合入前也跑）
# 门禁路由：android/ pack/sysdata_synth.py runtime/rewriter/ runtime/src/svc_mem.c runtime/src/det.c runtime/src/layout.c runtime/include/ graphics/track/
# 门禁本机：linux 安卓 Linux runtime CTest 要 aarch64 Linux：本机进 colima 虚拟机跑（android/linux_ctest.sh），gate-host 没有 colima；10-28 main 上 guest_page_test 链接红（svc_mem.c 新外部符号漏补桩）门禁没拦下
# 门禁路由：services/S2A.Services/Native.cs services/S2A.Services/InputPump.cs services/S2A.Services/ControllerFeedback.cs services/aot/ services/build.mk runtime/include/s2a_native.h compat/tree/src/Ryujinx.Graphics.Nvdec/ graphics/vulkan/
# 门禁本机：apk 安卓单库 APK 构建按宿主选后端——macOS 走 host/gate-apk.sh 离线宿主门禁（不碰 Docker），Linux 走 docker 镜像；gate-host 没有 colima，本机另跑
# 门禁本机：vkreal 真 Vulkan 收尾探针（android/tests/nvn_vk_real_teardown_test.sh）在 macOS 上经 tests/vulkan/run.sh 进 docker（colima）跑，gate-host 没有 docker 自报 75（11-07：gate-host 安卓钩子从 11-04 起每次 75、退回本机补跑）；本机另跑
# 门禁路由：graphics/nvn/gfx/nvn/include/nvn.h
# 门禁覆盖：android/*
# 无设备 CPU 测（test.sh）：JDK 17 Java 单测 / App 纯 Java 类编译 / Python 契约，
# 以及可移植改写准备回归（具体子项由 test.sh 登记）；需要 CMake 3.18+、C 编译器、固定离线
# Capstone/LZ4 源码（共享 resolver 自动查找，S2A_REWRITER_DEPS 可覆盖）与固定 Python venv
# （S2A_PY → PY → ~/.s2arm/venv/bin/python3）。
# C 源码不等于 Python 扩展；版本从 tools/rewriter-requirements.txt 读取。缺依赖前置报红，不下载。
# Q19 真实链接（检测到 NDK 29 时）另需 libavc：先在同一 S2A_BUILD_CACHE 执行
# bash android/codecs/build.sh --prepare-sources [本地源码目录]，或设 S2A_Q19_AVC_ARCHIVE 指向真实归档。
# Q19 / codecs 默认缓存 $HOME/.cache/s2arm-build，源码位于 <缓存>/codecs/sources/<固定内容 SHA>.tar.gz；缺失报红，不下载。
# Q19 离线 libavc 用 CMake >=3.31.6：显式 CMAKE 优先，否则先 PATH、再 SDK、再 Homebrew；缺失报红。
# game core 真实交叉链接必须有 NDK 29.0.14206865（单独 NDK 或 SDK 中 ndk/29.0.14206865）；
# 也查 Homebrew 的 /opt/homebrew/share/android-ndk 和 /usr/local/share/android-ndk，逐项核固定版本。
# 可运行 "$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" --sdk_root="$ANDROID_HOME" 'ndk;29.0.14206865'。
# NDK 搜索顺序与 Q19 一致；找不到时报告全部候选和环境，不跳过。完整 SDK 的其他包留给 Gradle。
# Linux 必须运行 runtime CTest；macOS 跳过
# 该项（memory.c 的 mremap / MREMAP_MAYMOVE 在 macOS 编不了），仍运行可移植改写准备回归。
# 门禁预算：--budget → 1500 秒。整份实跑：11-10 列车 501–902 秒（902 秒那次撞上旧 900 秒上限判「环境未就绪」）；
#   另含 APK 备料锁等待（上限 S2A_ANDROID_PREPARE_WAIT_S，缺省 480 秒）和缺标记时的自动 prepare，预算要盖住「实跑最长 + 等锁上限」
[ "${1:-}" = --budget ] && { echo 1500; exit 0; }
set -euo pipefail
source "$(cd "$(dirname "$0")" && pwd)/tools/gate_step.sh"
s2a_gate_init
trap 's2a_gate_finish $?' EXIT
cd "$(cd "$(dirname "$0")/.." && pwd)"   # 仓库根

# 与 doctor.sh / rewrite_c_gate_test.sh 使用同一固定 venv；兼容显式 PY。
# 显式给空值或无效解释器须报红，不能悄悄改用 PATH 的另一个 Python。
if [ "${S2A_PY+x}" ]; then
  PY=$S2A_PY
elif [ ! "${PY+x}" ]; then
  PY="$HOME/.s2arm/venv/bin/python3"
fi
python_bin=$(command -v "$PY" 2>/dev/null || true)
if [ -z "$python_bin" ] || [ ! -f "$python_bin" ] || [ ! -x "$python_bin" ]; then
  s2a_gate_note "【红】安卓门禁 Python 解释器无效：${PY:-<空值>}（S2A_PY → PY → \$HOME/.s2arm/venv/bin/python3；不自动安装）"
  exit 1
fi
PY=$python_bin
export PY S2A_PY="$PY"
# 既检查模块自报版本，也实际调用扩展。C 的固定源码目录不能用于此处 import。
if python_check=$("$PY" - tools/rewriter-requirements.txt 2>&1 <<'PY_CHECK'
import importlib
from pathlib import Path
import sys

if sys.version_info < (3, 12):
    print("Python 需 >=3.12，实为 " + sys.version.split()[0])
    raise SystemExit(1)
errors = []
versions = []
try:
    requirements = Path(sys.argv[1]).read_text(encoding="utf-8").splitlines()
    for requirement in requirements:
        requirement = requirement.split("#", 1)[0].strip()
        if not requirement:
            continue
        name, expected = requirement.split("==")
        try:
            module = importlib.import_module(name)
            actual = str(module.__version__)
            if actual != expected:
                errors.append(f"{name} 版本需 {expected}，实为 {actual}")
                continue
            if name == "lz4":
                block = importlib.import_module("lz4.block")
                sample = b"android-rewriter-dependency-check"
                if block.decompress(block.compress(sample)) != sample:
                    raise RuntimeError("lz4.block 压解字节不一致")
            elif name == "capstone":
                decoder = module.Cs(module.CS_ARCH_ARM64, module.CS_MODE_ARM)
                instructions = list(decoder.disasm(bytes.fromhex("1f2003d5"), 0))
                if len(instructions) != 1 or instructions[0].mnemonic != "nop":
                    raise RuntimeError("Capstone AArch64 解码不符")
            versions.append(f"{name}={actual}")
        except Exception as error:
            errors.append(f"{name} 导入/扩展失败 ({type(error).__name__}: {error})")
except Exception as error:
    errors.append(f"固定版本表不可用 ({type(error).__name__}: {error})")
if errors:
    print("；".join(" ".join(error.splitlines()) for error in errors))
    raise SystemExit(1)
print(f"{sys.executable} / Python {sys.version.split()[0]} / " + ", ".join(versions))
PY_CHECK
); then
  : # Print success only after both Python and C dependencies are complete.
else
  # 启动失败或扩展报错也统一为一行，不把 traceback 变成四项 unittest 断言失败。
  python_check=$(printf '%s' "$python_check" | tr '\r\n' '  ')
  s2a_gate_note "【红】安卓门禁 Python 固定依赖：${python_check:-解释器启动失败}（${PY}；见 tools/rewriter-requirements.txt；不自动安装）"
  exit 1
fi

# JNI 与 C/Python 矩阵都需同源构建；复用生产 resolver 的固定源码查找，
# 不能把 S2A_REWRITER_DEPS 当必填变量，也不能由外部 CLI 覆盖跳过此依赖。
if rewriter_deps=$("$PY" - 2>&1 <<'C_DEPS'
import sys
sys.path.insert(0, "pack")
from rewriter import dependency_dir
try:
    print(dependency_dir())
except (ValueError, OSError) as error:
    print(" ".join(str(error).split("显式改用 Python：", 1)[0].splitlines()).strip())
    raise SystemExit(1)
C_DEPS
); then
  export S2A_REWRITER_DEPS="$rewriter_deps"
else
  rewriter_deps=$(printf '%s' "$rewriter_deps" | tr '\r\n' '  ')
  s2a_gate_note "【红】安卓门禁 C 固定源码（Capstone/LZ4）：${rewriter_deps:-依赖解析失败}（不自动下载）"
  exit 1
fi

echo "门禁 Python：${python_check}"

# Resolve and validate each JDK 17 candidate before selecting it. A newer JDK in
# an earlier location must not hide an available JDK 17 in a later location.
JAVAC=$(bash android/tools/jdk17.sh || true)
[ -n "$JAVAC" ] || { s2a_gate_note "🔴 门禁找不到 JDK 17 的 javac（可用 S2A_JAVAC=<javac 路径> 指定）"; exit 1; }
jv=$("$JAVAC" -version 2>&1)
# Homebrew bin/javac can be a symlink below the keg root. Ask the selected
# compiler's VM for its real Home; dirname(javac)/.. need not contain JNI libs.
JAVA_HOME=$("$JAVAC" -J-XshowSettings:properties -version 2>&1 | sed -n 's/^[[:space:]]*java.home = //p')
[ -n "$JAVA_HOME" ] && [ -x "$JAVA_HOME/bin/javac" ] && [ -f "$JAVA_HOME/include/jni.h" ] || {
  s2a_gate_note "🔴 选中的 javac 未返回完整 JDK Home，无法运行 JNI 检查"; exit 1;
}
export JAVA_HOME
export PATH="$JAVA_HOME/bin:$PATH"
echo "门禁 JDK：${JAVAC}（${jv}）"   # 变量后紧跟全角括号：必须 ${}，否则 C locale 下 bash 把全角字节并入变量名（set -u 报未绑定）

run_apk_gate() {
  s2a_gate_step python3 android/docker/test_gate_apk.py
  s2a_gate_step python3 android/docker/test_prepare.py
  s2a_gate_step python3 android/docker/test_cache_identity.py
  # 与合入后钩子 tools/post_android_prepare.sh 共用一把锁：Linux 上两边的容器共用同一缓存卷、macOS 上宿主 prepare 与宿主门禁读写同一缓存槽，同时 dotnet publish 会互相锁死
  #   （11-01：钩子 prepare 与两次 APK 门禁三个容器卡在 MSBuild、CPU 0 达 50 分钟）。准备在跑 → 本轮不跑 APK、报待宿主证，
  #   钩子跑完会删守望 last_checked 让这一提交补跑；门禁在跑时钩子排队等。持锁进程已死的锁收回
  #   11-10：安卓门禁改成「路由命中时合入前也跑」后，入车撞上合入后钩子的 prepare 就记待宿主证、列车按缺宿主证明退回（launcher/batch-audit）。
  #   改成先等 prepare 做完（上限 S2A_ANDROID_PREPARE_WAIT_S，缺省 480 秒，prepare 实测几分钟；和上面的门禁预算一起定），等满仍被占才报待宿主证
  local lk=${S2A_ANDROID_PREPARE_LOCK:-$HOME/.cache/s2arm-build/post-prepare.lock} p waited=0 wmax=${S2A_ANDROID_PREPARE_WAIT_S:-480}
  mkdir -p "$(dirname "$lk")"
  until mkdir "$lk" 2>/dev/null; do
    p=$(cat "$lk/pid" 2>/dev/null)
    if kill -0 "$p" 2>/dev/null; then
      if [ "$waited" -ge "$wmax" ]; then
        echo "未跑：安卓 APK 准备进行中（持锁 pid ${p}，合入后钩子在重跑 prepare），已等 ${waited} 秒仍被占，备料完成后守望补跑"; printf '待宿主证\t安卓宿主\tapk\n'; return 0; fi
      [ "$waited" = 0 ] && echo "安卓 APK 准备进行中（持锁 pid ${p}），先等它做完（上限 ${wmax} 秒）"
      sleep 10; waited=$((waited + 10)); continue; fi
    rm -rf "$lk"; done
  echo $$ > "$lk/pid"
  # #1299：与 host/gate-apk.sh 开头的 uname 判断对称——docker 镜像只证 Linux 宿主，
  # macOS 由宿主原生后端证（android/host/gate-apk.sh），不在 arm64 上模拟 amd64。
  if [ "$(uname -s)" = Darwin ]; then
    echo '安卓 APK 门禁：macOS 跳过 docker 镜像构建，由宿主原生后端证（android/host/gate-apk.sh）'
  else
    s2a_gate_step bash android/docker/gate-apk.sh
  fi
  # 缺本依赖身份的准备完成标记（只核标记模式退 42）→ 先在本锁里跑宿主 prepare.sh 再证，和合入后钩子 tools/post_android_prepare.sh 同一做法。
  #   11-10：安卓门禁合入前也跑后，安卓依赖刚变（前一支合入）、合入后钩子还没补备料的空档里入车的支被判红退回（fix/audit-p2-271-273）。
  #   prepare 本身失败照旧交下面的宿主门禁报红；S2A_ANDROID_AUTO_PREPARE=0 关掉（单测 / 只想看标记状态时用）
  if [ "${S2A_ANDROID_AUTO_PREPARE:-1}" = 1 ]; then
    # 不包 timeout：macOS 门禁机 PATH 里没有 GNU timeout（rc=127），本脚本 set -e 会直接退出；只核标记这步只算依赖清单、不编译
    local rc=0
    S2A_APK_RECEIPT_CHECK=1 bash android/host/gate-apk.sh > /dev/null 2>&1 || rc=$?
    if [ "$rc" = 42 ]; then echo "安卓 APK：本依赖身份没有准备完成标记，先跑宿主 prepare.sh"; bash android/host/prepare.sh || echo "安卓 APK：宿主 prepare.sh 没跑通（rc=$?），交宿主门禁照常报"; fi
  fi
  # #1131：宿主原生路径同一口径；只在 macOS 跑（脚本内自判），缺备料直接报红给命令。
  s2a_gate_step bash android/host/gate-apk.sh
  rm -rf "$lk"
}

# linux、apk、vkreal 可被 tools/gate.sh 合并派到同一个 GATE_ONLY。
for k in ${GATE_SKIP:-} ${GATE_ONLY:-}; do
  case "$k" in linux|apk|vkreal) ;; *) s2a_gate_note "【红】安卓门禁：子项键 $k 不认识（只有 linux、apk、vkreal）"; exit 1 ;; esac
done
if [ -n "${GATE_ONLY:-}" ]; then
  for k in ${GATE_ONLY}; do
    case "$k" in
      linux) s2a_gate_step bash android/linux_ctest.sh ;;
      apk) run_apk_gate ;;
      vkreal) s2a_gate_step bash android/tests/nvn_vk_real_teardown_test.sh ;;
    esac
  done
  exit 0
fi
case " ${GATE_SKIP:-} " in *" linux "*) linux_ctest=0 ;; *) linux_ctest=1 ;; esac
case " ${GATE_SKIP:-} " in *" apk "*) apk_gate=0 ;; *) apk_gate=1 ;; esac
case " ${GATE_SKIP:-} " in *" vkreal "*) export S2A_ANDROID_SKIP_VKREAL=1 ;; esac   # test.sh 里那一步按它跳过（本机另跑 GATE_ONLY=vkreal）
command -v cmake >/dev/null 2>&1 || { s2a_gate_note "🔴 安卓门禁需要 CMake 3.18+ 和 C 编译器运行可移植改写准备回归；不跳过、不自动下载"; exit 1; }
if [ "$(uname -s)" = Linux ]; then
  ( unset S2A_TEST_NO_NATIVE; s2a_gate_step bash android/test.sh ) >&2
  s2a_gate_step "$PY" android/native/runtime/tracking/tests/run.py --out build/tests/android-tracking >&2
  (
    tmp=$(mktemp -d "${TMPDIR:-/tmp}/s2a-two-guest.XXXXXXXX")
    trap 'rm -rf "$tmp"' EXIT
    s2a_gate_step "$PY" android/core-link/runtime-tests/writers-gate/run.py --two-guest-only --out "$tmp/results"
  ) >&2
  echo "安卓 Java / Python / 改写准备、Linux runtime CTest 与 tracking 测试已完成；跳过项以实际输出为准"
else
  S2A_TEST_NO_NATIVE=1 s2a_gate_step bash android/test.sh >&2
  if [ $linux_ctest = 1 ] && ! command -v colima >/dev/null 2>&1; then
    printf '待宿主证\t安卓宿主\tlinux\n'
  elif [ $linux_ctest = 1 ]; then
    s2a_gate_step bash android/linux_ctest.sh >&2 || { s2a_gate_note "【红】安卓门禁：期望 Java / Python / 改写准备 + Linux runtime CTest 全过 → 真结果 Linux runtime CTest 红"; exit 1; }
    echo "安卓 Java 单测 / App 纯 Java 类编译 / Python 契约 / 可移植改写准备回归 + Linux runtime CTest（colima）：期望 全过 → 真结果 通过"
  else
    echo "安卓 Java 单测 / App 纯 Java 类编译 / Python 契约 / 可移植改写准备回归：期望 全过 → 真结果 通过（Linux runtime CTest 由本机另跑：GATE_SKIP=linux）"
  fi
fi

if [ "$apk_gate" = 1 ]; then
  run_apk_gate
fi
