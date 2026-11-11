#!/usr/bin/env bash
# C3：Android native Vulkan 真 GPU 微例（clear / signed clear / draw / compute / copy / readback）。
# 只证明给定设备上的执行原语与手算结果；不证明 NVN 记录回放、游戏、窗口呈现代或性能。
# 用法：bash android/tools/direct_c3_vk_probe.sh
# 环境：ANDROID_NDK_HOME / S2A_ADB
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
. "$(dirname "$0")/ndk_toolchain.sh"
ndk=$(s2a_ndk_resolve) || { echo "no NDK; C3 GPU probe NOT RUN" >&2; exit 2; }
serial=${S2A_ADB:-$(adb devices | awk 'NR>1 && $2=="device" {print $1; exit}')}
[ -n "$serial" ] || { echo "no adb device; C3 GPU probe NOT RUN" >&2; exit 2; }
out=$(mktemp -d "${TMPDIR:-/tmp}/s2a-vk-micro.XXXXXX")
trap 'rm -rf "$out"' EXIT
glslc=${GLSLC:-$(s2a_ndk_glslc "$ndk" || true)}
[ -n "$glslc" ] || { echo "no glslc; C3 GPU probe NOT RUN" >&2; exit 2; }
for shader in tri.vert tri.frag fill.comp; do
    "$glslc" --target-env=vulkan1.1 "$repo/android/tests/shaders/direct/$shader" -o "$out/$shader.spv"
done
cc=$(s2a_ndk_clang "$ndk" 29) || { echo "no NDK clang for this host; probe NOT RUN" >&2; exit 2; }
"$cc" -std=c11 -O2 -Wall -Wextra -Werror -I "$repo/android/native/direct" \
    -o "$out/vk_micro" "$repo/android/tests/vk_micro_test.c" \
    "$repo/android/native/direct/vk_device.c" -lvulkan -llog -lm
"$cc" -std=c11 -O2 -Wall -Wextra -Werror -I "$repo/android/native/direct" \
    -I "$repo/graphics/nvn/gfx/nvn" -I "$repo/graphics/nvn/gfx/nvn/include" -I "$repo/android/native/nvn" \
    -o "$out/vk_record" "$repo/android/tests/vk_record_test.c" \
    "$repo/android/native/direct/vk_record.c" "$repo/android/native/direct/t2t_plan.c" "$repo/android/native/direct/buffer_texture_plan.c" "$repo/android/native/direct/bind_defer.c" "$repo/android/native/direct/vk_device.c" \
    "$repo/android/native/nvn/nvn_vk_format.c" "$repo/android/native/direct/vk_clear.c" \
    "$repo/android/native/nvn/nvn_vk_query.c" \
    -lvulkan -llog -lm
adb -s "$serial" shell mkdir -p /data/local/tmp/s2a-vk-micro/shaders
adb -s "$serial" push "$out/vk_micro" /data/local/tmp/s2a-vk-micro/vk_micro >/dev/null
adb -s "$serial" push "$out/vk_record" /data/local/tmp/s2a-vk-micro/vk_record >/dev/null
adb -s "$serial" push "$out"/*.spv /data/local/tmp/s2a-vk-micro/shaders/ >/dev/null
adb -s "$serial" shell chmod 755 /data/local/tmp/s2a-vk-micro/vk_micro /data/local/tmp/s2a-vk-micro/vk_record
echo "device: $serial $(adb -s "$serial" shell getprop ro.product.model | tr -d '\r') / Android $(adb -s "$serial" shell getprop ro.build.version.release | tr -d '\r')"
adb -s "$serial" shell /data/local/tmp/s2a-vk-micro/vk_micro /data/local/tmp/s2a-vk-micro/shaders
# vk_record_test 的 query 期望按“报告时立即解析并写回”的旧行为写；产品默认是帧完成点
# 延迟写回（debug.s2a.query_wait 未设）。C3 直接调 record replay 没有帧完成点，所以这里
# 显式临时打开 immediate 模式，跑完恢复。
adb -s "$serial" shell setprop debug.s2a.query_wait 1
adb -s "$serial" shell /data/local/tmp/s2a-vk-micro/vk_record /data/local/tmp/s2a-vk-micro/shaders
adb -s "$serial" shell setprop debug.s2a.query_wait 0
