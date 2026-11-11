#!/bin/bash
# 门禁覆盖：compat/tree/src/Ryujinx.Graphics.Gpu/Engine/Threed/StateUpdater.cs compat/tree/src/Ryujinx.Graphics.Gpu/Engine/Threed/DrawManager.cs compat/tree/src/Ryujinx.Graphics.Gpu/Engine/Threed/ThreedClass.cs tests/gpu_omap_mask_state/*
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 输出映射并进颜色写掩码之后的两件事（#468，接 tests/gpu_omap_mask_test.sh）：送给宿主的掩码并映射、交给着色器缓存的
#   _pipeline.ColorWriteMask 仍是寄存器掩码；DrawTexture 之前恢复寄存器掩码并把着色器状态标脏。
#   按文本切出生产方法，配记录用的桩编成小程序（不引 Ryujinx 工程、不用 GPU），期望值全部手算；两处调用点按文本核。
#   正跑必须全绿，8 个变异每个都必须红。要 dotnet 和 python3，约 10 秒：bash tests/gpu_omap_mask_state_test.sh
set -u; H=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d); rc=0
run() { MSBUILDDISABLENODEREUSE=1 python3 "$H/tests/gpu_omap_mask_state/omap_state_harness.py" --out "$T/$1" ${2:+--mutation "$2"}; }
run plain > "$T/plain.txt" 2>&1 || { echo "正跑没过："; tail -30 "$T/plain.txt"; rc=1; }
[ $rc = 0 ] && tail -1 "$T/plain.txt"
for m in and stored clear-reset clear-masks clear-dirty site-comment site-guard site-forward; do
  case $m in site-*) want='^call site check failed: ' ;; *) want='^(run failed: )?FAIL: ' ;; esac
  if run "m-$m" "$m" > "$T/m-$m.txt" 2>&1; then echo "变异 $m 没有变红（单测抓不住这种错）"; rc=1
  elif ! grep -Eq "$want" "$T/m-$m.txt"; then echo "变异 $m 红的不是断言："; tail -5 "$T/m-$m.txt"; rc=1
  fi
done
[ $rc = 0 ] && echo "gpu_omap_mask_state: 8 mutations each turned the run red"
rm -rf "$T"; exit $rc
