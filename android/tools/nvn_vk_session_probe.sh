#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
. "$(dirname "$0")/ndk_toolchain.sh"
ndk=$(s2a_ndk_resolve) || { echo "no NDK; session probe NOT RUN" >&2; exit 2; }
serial=${S2A_ADB:-$(adb devices | awk 'NR>1 && $2=="device" {print $1; exit}')}
[ -n "$serial" ] || { echo "no adb device; probe NOT RUN" >&2; exit 2; }
cc=$(s2a_ndk_clang "$ndk" 29) || { echo "no NDK clang; probe NOT RUN" >&2; exit 2; }
out=$(mktemp -d "${TMPDIR:-/tmp}/s2a-nvn-vk-session.XXXXXX")
trap 'rm -rf "$out"' EXIT
"$cc" -std=c11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-nonnull \
    -I "$repo/android/native/direct" -I "$repo/android/native/nvn" \
    -I "$repo/graphics/nvn/gfx/nvn" -I "$repo/graphics/nvn/gfx/nvn/include" -I "$repo/graphics/nvn/gfx/tex" \
    -o "$out/t" "$repo/android/tests/nvn_vk_session_test.c" \
    "$repo/android/native/nvn/nvn_vk_session.c" \
    "$repo/android/native/nvn/nvn_vk_pipeline.c" \
    "$repo/android/native/nvn/nvn_vk_swapchain.c" \
    "$repo/android/native/nvn/nvn_vk_surface.c" \
    "$repo/android/native/nvn/nvn_vk_texcache.c" "$repo/android/native/nvn/nvn_vk_bcdec.c" \
    "$repo/android/native/nvn/nvn_vk_sampler_cache.c" \
    "$repo/android/native/nvn/nvn_vk_sampler.c" \
    "$repo/android/native/nvn/nvn_vk_format.c" \
    "$repo/android/native/direct/vk_record.c" "$repo/android/native/direct/t2t_plan.c" "$repo/android/native/direct/buffer_texture_plan.c" "$repo/android/native/direct/bind_defer.c" "$repo/android/native/direct/vk_clear.c" \
    "$repo/android/native/nvn/nvn_vk_query.c" \
    "$repo/graphics/nvn/gfx/nvn/nvn_tex.c" "$repo/graphics/nvn/gfx/tex/nvn_deswizzle.c" \
    "$repo/android/native/direct/vk_device.c" -lvulkan -llog -landroid -lm
adb -s "$serial" push "$out/t" /data/local/tmp/t >/dev/null
adb -s "$serial" shell chmod 755 /data/local/tmp/t
adb -s "$serial" shell /data/local/tmp/t
