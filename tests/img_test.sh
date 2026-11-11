#!/bin/bash
# 单一 IMG v1（pack/img.py + services/romfs_index --std）单测外壳：头 / 目录表 / 标准 RomFS 段（独立解析）/ write / convert，期望来自夹具本身（见 tests/img_test.py 文件头）
# 门禁覆盖：pack/img.py services/romfs_index/ services/romfs_index.sh services/S2A.Services/RomFsIndex.cs tests/img_test.py tests/img_test.sh
set -u
H=$(cd "$(dirname "$0")/.." && pwd)
export DOTNET_ROOT=${DOTNET_ROOT:-$HOME/.dotnet}; export PATH=$DOTNET_ROOT:$PATH
python3 "$H/tests/img_test.py"
