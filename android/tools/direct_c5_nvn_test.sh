#!/usr/bin/env bash
# C5：共享 NVN 前端在 Android/ARM64 上运行 CPU 侧 API 测试（t_nvn_fill）。
# 只证明前端对象/槽表/采样器/命令内存/缓冲 CPU 语义；不证明 GPU 回放、呈现、游戏。
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
. "$(dirname "$0")/ndk_toolchain.sh"
ndk=$(s2a_ndk_resolve) || { echo "no NDK; C5 NVN test NOT RUN" >&2; exit 2; }
serial=${S2A_ADB:-$(adb devices | awk 'NR>1 && $2=="device" {print $1; exit}')}
[ -n "$serial" ] || { echo "no adb device; C5 NVN test NOT RUN" >&2; exit 2; }
out=$(mktemp -d "${TMPDIR:-/tmp}/s2a-nvn-c5.XXXXXX")
trap 'rm -rf "$out"' EXIT
mkdir -p "$out/shaders"
glslc=$(s2a_ndk_glslc "$ndk" || true)
[ -n "$glslc" ] && [ -x "$glslc" ] || { echo "no glslc; draw test NOT RUN" >&2; exit 2; }
for shader in tri.vert tri.frag tex.frag; do
    "$glslc" --target-env=vulkan1.1 "$repo/android/tests/shaders/direct/$shader" -o "$out/shaders/$shader.spv"
done
if command -v dotnet >/dev/null 2>&1; then
    (cd "$repo/graphics/tools/direct/tests/compiler" && dotnet run -c Release -- --emit-requests "$out/c2req") >/dev/null
    dotnet run -c Release --project "$repo/graphics/tools/direct/compiler/cli" -- \
        compile --request "$out/c2req/c2draw.json" --output "$out/c2out" >/dev/null
    python3 "$repo/android/tools/extract_spirv.py" "$out/c2out/program.json" fs "$out/shaders/c2.frag.spv"
    dotnet run -c Release --project "$repo/graphics/tools/direct/compiler/cli" -- \
        compile --request "$out/c2req/c2vs.json" --output "$out/c2vsout" >/dev/null
    python3 "$repo/android/tools/extract_spirv.py" "$out/c2vsout/program.json" vs "$out/shaders/c2vs.vs.spv"
    python3 "$repo/android/tools/extract_spirv.py" "$out/c2vsout/program.json" fs "$out/shaders/c2vs.fs.spv"
    dotnet run -c Release --project "$repo/graphics/tools/direct/compiler/cli" -- \
        compile --request "$out/c2req/c2vspos.json" --output "$out/c2vsposout" >/dev/null
    python3 "$repo/android/tools/extract_spirv.py" "$out/c2vsposout/program.json" vs "$out/shaders/c2vspos.vs.spv"
    python3 "$repo/android/tools/extract_spirv.py" "$out/c2vsposout/program.json" fs "$out/shaders/c2vspos.fs.spv"
else
    echo "dotnet absent: C2 draw fixture NOT RUN" >&2
    exit 2
fi
cc=$(s2a_ndk_clang "$ndk" 29) || { echo "no NDK clang for this host; C5 NVN test NOT RUN" >&2; exit 2; }
flags=(-std=gnu11 -O2 -Wno-format -Wno-unused-function -Wno-unused-variable
  -include "$repo/android/native/nvn/nvn_be_compat.h"   # main 的 nvn_be_* 接口名 → 本线 mtl_* 实现
  -I "$repo" -I "$repo/runtime/src" -I "$repo/android/native/nvn/compat"
  -I "$repo/android/native/direct" -I "$repo/android/native/nvn" -I "$repo/graphics/nvn/gfx/nvn"
  -I "$repo/graphics/nvn/gfx/nvn/include" -I "$repo/graphics/nvn/gfx/nvn/host"
  -I "$repo/graphics/nvn" -I "$repo/runtime/include" -I "$repo/android/native")
objs=()
compile() { "$cc" "${flags[@]}" -c "$repo/$1" -o "$out/$2.o"; objs+=("$out/$2.o"); }
compile graphics/nvn/gfx/nvn/hle_nvn.c hle_nvn
compile android/native/nvn/portable.c portable
compile android/native/nvn/vk_nvn_adapter.c adapter
compile android/native/nvn/frontend_test_host.c host
cmake -D NAMES="$repo/graphics/nvn/gfx/nvn/names.txt" -D OUT="$out/nvn_names_android.c" \
    -P "$repo/android/native/nvn/gen_nvn_names.cmake"
"$cc" "${flags[@]}" -I "$repo/graphics/nvn/gfx/nvn" -c "$out/nvn_names_android.c" -o "$out/names.o"; objs+=("$out/names.o")
compile graphics/nvn/gfx/nvn/nvn_dirty.c nvn_dirty
compile graphics/nvn/gfx/nvn/nvn_fps.c nvn_fps
compile android/native/nvn/nvn_be_android.c nvn_be_android
compile graphics/nvn/gfx/nvn/nvn_tex.c nvn_tex
compile graphics/nvn/gfx/tex/nvn_deswizzle.c deswizzle
compile graphics/nvn/tests/t_nvn_fill.c t_nvn_fill
compile android/native/direct/sha256.c sha256
compile android/native/direct/vk_device.c vk_device
compile android/native/direct/vk_record.c vk_record
compile android/native/direct/t2t_plan.c t2t_plan
compile android/native/direct/buffer_texture_plan.c buffer_texture_plan
compile android/native/direct/bind_defer.c bind_defer
compile android/native/nvn/nvn_vk_query.c nvn_vk_query
compile android/native/nvn/nvn_vk_format.c nvn_vk_format
compile android/native/direct/vk_clear.c vk_clear
"$cc" -o "$out/nvn_fill_test" "${objs[@]}" -lvulkan -llog -lm -pthread
"$cc" "${flags[@]}" -c "$repo/android/tests/nvn_frontend_submit_test.c" -o "$out/nvn_submit.o"
"$cc" "${flags[@]}" -c "$repo/android/tests/nvn_frontend_rt_test.c" -o "$out/nvn_rt.o"
"$cc" "${flags[@]}" -c "$repo/android/tests/nvn_frontend_draw_test.c" -o "$out/nvn_draw.o"
"$cc" "${flags[@]}" -c "$repo/android/tests/nvn_frontend_tex_test.c" -o "$out/nvn_texmain.o"
"$cc" "${flags[@]}" -c "$repo/android/tests/nvn_frontend_c2draw_test.c" -o "$out/nvn_c2draw.o"
"$cc" "${flags[@]}" -c "$repo/android/tests/nvn_frontend_c2point_test.c" -o "$out/nvn_c2point.o"
"$cc" "${flags[@]}" -c "$repo/android/tests/nvn_frontend_c2tri_test.c" -o "$out/nvn_c2tri.o"
submit_objs=()
for o in "${objs[@]}"; do [ "$(basename "$o")" = "t_nvn_fill.o" ] || submit_objs+=("$o"); done
submit_objs+=("$out/nvn_submit.o")
"$cc" -o "$out/nvn_submit_test" "${submit_objs[@]}" -lvulkan -llog -lm -pthread
rt_objs=()
for o in "${objs[@]}"; do [ "$(basename "$o")" = "t_nvn_fill.o" ] || rt_objs+=("$o"); done
rt_objs+=("$out/nvn_rt.o")
"$cc" -o "$out/nvn_rt_test" "${rt_objs[@]}" -lvulkan -llog -lm -pthread
draw_objs=()
for o in "${objs[@]}"; do [ "$(basename "$o")" = "t_nvn_fill.o" ] || draw_objs+=("$o"); done
draw_objs+=("$out/nvn_draw.o")
"$cc" -o "$out/nvn_draw_test" "${draw_objs[@]}" -lvulkan -llog -lm -pthread
tex_objs=()
for o in "${objs[@]}"; do [ "$(basename "$o")" = "t_nvn_fill.o" ] || tex_objs+=("$o"); done
tex_objs+=("$out/nvn_texmain.o")
"$cc" -o "$out/nvn_tex_test" "${tex_objs[@]}" -lvulkan -llog -lm -pthread
c2_objs=()
for o in "${objs[@]}"; do [ "$(basename "$o")" = "t_nvn_fill.o" ] || c2_objs+=("$o"); done
c2_objs+=("$out/nvn_c2draw.o")
"$cc" -o "$out/nvn_c2draw_test" "${c2_objs[@]}" -lvulkan -llog -lm -pthread
point_objs=()
for o in "${objs[@]}"; do [ "$(basename "$o")" = "t_nvn_fill.o" ] || point_objs+=("$o"); done
point_objs+=("$out/nvn_c2point.o")
"$cc" -o "$out/nvn_c2point_test" "${point_objs[@]}" -lvulkan -llog -lm -pthread
tri_objs=()
for o in "${objs[@]}"; do [ "$(basename "$o")" = "t_nvn_fill.o" ] || tri_objs+=("$o"); done
tri_objs+=("$out/nvn_c2tri.o")
"$cc" -o "$out/nvn_c2tri_test" "${tri_objs[@]}" -lvulkan -llog -lm -pthread
adb -s "$serial" push "$out/nvn_fill_test" /data/local/tmp/s2a-vk-micro/nvn_fill_test >/dev/null
adb -s "$serial" shell chmod 755 /data/local/tmp/s2a-vk-micro/nvn_fill_test
adb -s "$serial" push "$out/nvn_submit_test" /data/local/tmp/s2a-vk-micro/nvn_submit_test >/dev/null
adb -s "$serial" push "$out/nvn_rt_test" /data/local/tmp/s2a-vk-micro/nvn_rt_test >/dev/null
adb -s "$serial" push "$out/nvn_draw_test" /data/local/tmp/s2a-vk-micro/nvn_draw_test >/dev/null
adb -s "$serial" push "$out/nvn_tex_test" /data/local/tmp/s2a-vk-micro/nvn_tex_test >/dev/null
adb -s "$serial" push "$out/nvn_c2draw_test" /data/local/tmp/s2a-vk-micro/nvn_c2draw_test >/dev/null
adb -s "$serial" push "$out/nvn_c2point_test" /data/local/tmp/s2a-vk-micro/nvn_c2point_test >/dev/null
adb -s "$serial" push "$out/nvn_c2tri_test" /data/local/tmp/s2a-vk-micro/nvn_c2tri_test >/dev/null
adb -s "$serial" push "$out/shaders"/*.spv /data/local/tmp/s2a-vk-micro/shaders/ >/dev/null
adb -s "$serial" shell chmod 755 /data/local/tmp/s2a-vk-micro/nvn_submit_test /data/local/tmp/s2a-vk-micro/nvn_rt_test /data/local/tmp/s2a-vk-micro/nvn_draw_test /data/local/tmp/s2a-vk-micro/nvn_tex_test /data/local/tmp/s2a-vk-micro/nvn_c2draw_test /data/local/tmp/s2a-vk-micro/nvn_c2point_test /data/local/tmp/s2a-vk-micro/nvn_c2tri_test
adb -s "$serial" shell /data/local/tmp/s2a-vk-micro/nvn_fill_test
adb -s "$serial" shell /data/local/tmp/s2a-vk-micro/nvn_submit_test
adb -s "$serial" shell /data/local/tmp/s2a-vk-micro/nvn_rt_test
adb -s "$serial" shell /data/local/tmp/s2a-vk-micro/nvn_draw_test /data/local/tmp/s2a-vk-micro/shaders
adb -s "$serial" shell /data/local/tmp/s2a-vk-micro/nvn_tex_test /data/local/tmp/s2a-vk-micro/shaders
adb -s "$serial" shell /data/local/tmp/s2a-vk-micro/nvn_c2draw_test /data/local/tmp/s2a-vk-micro/shaders
adb -s "$serial" shell /data/local/tmp/s2a-vk-micro/nvn_c2point_test /data/local/tmp/s2a-vk-micro/shaders
adb -s "$serial" shell /data/local/tmp/s2a-vk-micro/nvn_c2tri_test /data/local/tmp/s2a-vk-micro/shaders
