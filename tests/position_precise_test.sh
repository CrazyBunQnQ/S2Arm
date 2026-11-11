#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# #82：降精度翻译下 gl_Position 依赖链仍标精确（S2APositionPrecise）。直接断言生产变换遍入口 TransformPasses.RunPass 的结果；不需要 GPU / 游戏数据。
# 门禁只看：tests/position_precise/* tests/position_precise_test.sh compat/tree/src/Ryujinx.Graphics.Shader/Translation/Transforms/*
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
# Resolve the repository's pinned SDK, not whichever SDK the caller's cwd selects.
cd "$ROOT/compat/tree"
DN=${DOTNET:-dotnet}
"$DN" build "$ROOT/tests/position_precise/PositionPrecise.csproj" -c Release \
  --artifacts-path "$OUT/dotnet" -o "$OUT/tool" \
  -p:UseSharedCompilation=false -m:1 -v quiet
"$DN" "$OUT/tool/dsh_cmp.dll"
