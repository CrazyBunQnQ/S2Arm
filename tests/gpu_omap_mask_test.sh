#!/bin/bash
# 门禁覆盖：compat/tree/src/Ryujinx.Graphics.Gpu/Engine/Threed/StateUpdater.cs compat/tree/src/Ryujinx.Graphics.Gpu/Ryujinx.Graphics.Gpu.csproj tests/gpu_omap_mask/*
# 颜色写掩码与片元着色器输出映射相与（#468）：S2AOmapColorMask 的手算用例，加两条源码检查——
#   UpdateRtColorMask 确实调用它、换程序时映射变了确实重算（只测纯函数的话，调用点被删掉也照样绿）。约 60 秒：bash tests/gpu_omap_mask_test.sh
set -u; H=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d); LOG=$T/run.log; bad=0
S=$H/compat/tree/src/Ryujinx.Graphics.Gpu/Engine/Threed/StateUpdater.cs
grep -q 'componentMask = S2AOmapColorMask(componentMask, _s2aFragmentOutputMap, index);' "$S" && echo "✓ UpdateRtColorMask 调用 S2AOmapColorMask" || { echo "✗ UpdateRtColorMask 没调用 S2AOmapColorMask"; bad=1; }
grep -q '_s2aFragmentOutputMap = fragmentOutputMap;' "$S" && grep -q 'gs.Shaders\[5\].Info.FragmentOutputMap' "$S" && echo "✓ 换程序时按片元输出映射重算" || { echo "✗ 换程序时没按片元输出映射重算"; bad=1; }
MSBUILDDISABLENODEREUSE=1 timeout 600 dotnet run -c Release --project "$H/tests/gpu_omap_mask/GpuOmapMaskTest.csproj" --artifacts-path "$T/art" > "$LOG" 2>&1; rc=$?
if [ $rc = 0 ]; then grep -E '^(✓|✗) |^gpu_omap_mask' "$LOG"; else echo "🔴 gpu_omap_mask 退出 ${rc}："; tail -30 "$LOG"; bad=1; fi
rm -rf "$T"; exit $bad
