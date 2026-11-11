#!/usr/bin/env bash
# 门禁覆盖：compat/tree/src/Ryujinx.Graphics.Nvdec/Vp9NativeFrame.cs android/codecs/vpxbench/hdr.c android/codecs/vp9audit/vp9audit.c tests/data/vp9_synth/interp_lit*.ivf
# VP9 重打包帧的 interp_filter 字面量回归：NVDEC pic[121]/Vp9NativeInput.InterpFilter 携带的是位流字面量
# （托管 Decoder.cs 自己做 LiteralToFilter），写侧必须直通。曾把 enum→literal 的 {1,0,2,3} 重映射错用在
# 字面量上（二重交换）：声明 lit=0(SMOOTH)/lit=1(EIGHTTAP) 的流首个 inter 帧像素失配、原生段崩。
# 本用例对 tests/data/vp9_synth 的 lit=0/lit=1 合成流跑 vp9audit（重打包 + libvpx 逐像素对拍），
# 断言首个 inter（f0002）Decoded 且全程 mismatch=0——修复前是 f0002 PIXEL-MISMATCH、段终结。
set -euo pipefail
H=$(cd "$(dirname "$0")/.." && pwd -P)
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
if ! bash "$H/android/codecs/vp9audit/build.sh" "$W" >/dev/null 2>&1; then
  bash "$H/android/codecs/build.sh" --prepare-sources >/dev/null 2>&1 || true
  bash "$H/android/codecs/vp9audit/build.sh" "$W" >/dev/null 2>&1 || { echo 'vp9_interp_literal：缺钉住版 libvpx 源码包，未测'; exit 75; }
fi
rc=0
for lit in 0 1; do
  out=$("$W/vp9audit" "$H/tests/data/vp9_synth/interp_lit$lit.ivf") || { echo "lit=${lit}：audit 运行失败"; rc=1; continue; }
  printf '%s\n' "$out" | grep -q 'f0002 I .* Decoded' || { echo "lit=${lit}：f0002 未逐像素一致（应为 Decoded）"; printf '%s\n' "$out" | tail -4; rc=1; }
  printf '%s\n' "$out" | grep -q 'mismatch=0' || { echo "lit=${lit}：存在像素失配"; printf '%s\n' "$out" | tail -4; rc=1; }
done
[ "$rc" = 0 ] && echo 'vp9_interp_literal：lit=0/1 流首 inter 逐像素一致、全程零失配'
exit "$rc"
