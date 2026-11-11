#!/usr/bin/env bash
# P0-0b' 宿主探针：生产入口 s2a_record_exec_one 回放 Deferred 纹理/采样器绑定时，必须读取游戏
# 句柄数组并按录制时的池基址平移（Metal 侧 nvn_mtl.m bh_fresh 的同一语义）；不能静默绑到 id 0。
#
# 只链生产 vk_record.c 及其直接依赖（vk_clear / nvn_vk_query / nvn_vk_format），不建 Vulkan
# device、不开 ICD：这些 opcode 只改 CPU 记录状态，vk* 符号由宿主 Vulkan loader 解析。
# 没有 libvulkan.so.1 的机器（例如 macOS）报“未运行”并退出 2，不伪装通过。
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
android="$repo/android"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# 只在 Linux 上跑：生产 vk_record.c 的 vk* 导入由宿主 Vulkan loader 解析（不建 device / 不开 ICD），
# macOS 没有这个 loader。不满足条件时明确退 2 写原因，不静默跳过、也不伪装通过。
if [ "$(uname -s)" != Linux ]; then
    echo "${0##*/} NOT RUN: 只在 Linux + 宿主 libvulkan.so.1 上跑（生产 vk_record.c 的 vk* 导入需由 loader 解析）" >&2
    exit 2
fi

loader=""
if command -v ldconfig >/dev/null 2>&1; then
    loader="$(ldconfig -p 2>/dev/null | awk '/libvulkan\.so\.1$/{print $NF; exit}')"
fi
if [ -z "$loader" ]; then
    for candidate in /usr/lib/libvulkan.so.1 /usr/lib/x86_64-linux-gnu/libvulkan.so.1 \
                     /usr/local/lib/libvulkan.so.1 /usr/lib/aarch64-linux-gnu/libvulkan.so.1; do
        [ -e "$candidate" ] && loader="$candidate" && break
    done
fi
if [ -z "$loader" ]; then
    echo "vk_record Deferred bind probe NOT RUN: 宿主没有 libvulkan.so.1（Linux 上只需它解析生产对象的 vk* 符号，不建 device）" >&2
    exit 2
fi

cc_bin="${CC:-cc}"
inc="-I $android/native -I $android/native/direct -I $android/native/nvn
     -I $android/tests/vkhost -I $android/tests/hostvk -I $repo/runtime/include
     -I $repo/graphics/nvn/gfx/nvn -I $repo/graphics/nvn/gfx/nvn/include -I $repo/graphics/nvn/gfx/tex
     -I $repo/graphics/nvn/third_party/vulkan-headers/include"
# 测试文件按仓库口径 -Wall -Wextra -Werror；生产 TU 用 host probe 的宽松告警（Android 侧由 NDK
# clang -Werror 把关，宿主 gcc 会报与本次改动无关的既有告警）。
# shellcheck disable=SC2086
"$cc_bin" -std=gnu11 -O1 -Wall -Wextra -Werror $inc \
  -c "$android/tests/vk_record_deferred_bind_test.c" -o "$tmp/test.o"
objs=()
for src in "$android/native/direct/vk_record.c" "$android/native/direct/vk_clear.c" "$android/native/direct/t2t_plan.c" "$android/native/direct/buffer_texture_plan.c" \
           "$android/native/nvn/nvn_vk_query.c" "$android/native/nvn/nvn_vk_format.c"; do
    obj="$tmp/$(basename "$src" .c).o"
    # shellcheck disable=SC2086
    "$cc_bin" -std=gnu11 -O1 $inc -c "$src" -o "$obj"
    objs+=("$obj")
done
"$cc_bin" -o "$tmp/t" "$tmp/test.o" "${objs[@]}" "$loader" -lpthread -lm
"$tmp/t"
