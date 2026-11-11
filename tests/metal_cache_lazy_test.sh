#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁覆盖：graphics/tools/direct/dsh/batch/Build.cs graphics/tools/direct/tests/t_metal_cache_lazy.py tests/metal_cache_lazy_test.sh
# 方法级缓存契约：真实 Compile 方法体、合成依赖、BCL SHA-256；不运行 Metal、不下载工具。
set -eu
H=$(cd "$(dirname "$0")/.." && pwd)
exec "${S2A_PY:-python3}" "$H/graphics/tools/direct/tests/t_metal_cache_lazy.py"
