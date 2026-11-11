#!/bin/bash
# 门禁覆盖：graphics/tools/macrogen/regen.sh graphics/tools/macrogen/MacroGen.cs tests/macrogen_synthetic/* tests/macrogen_private_test.py tests/macrogen_fixture_test.py tests/fixtures/generate_mme.py compat/tree/src/Ryujinx.Graphics.Gpu/Engine/MME/S2AMacroAot.cs compat/tree/src/Ryujinx.Graphics.Gpu/Engine/MME/MacroInterpreter.cs compat/tree/src/Ryujinx.Graphics.Gpu/Engine/MME/MacroJitContext.cs compat/tree/src/Ryujinx.Graphics.Gpu/Engine/MME/IMacroEE.cs compat/tree/src/Ryujinx.Graphics.Gpu/Engine/MME/AluOperation.cs compat/tree/src/Ryujinx.Graphics.Gpu/Engine/MME/AluRegOperation.cs compat/tree/src/Ryujinx.Graphics.Gpu/Engine/MME/AssignmentOperation.cs compat/tree/src/Ryujinx.Graphics.Device/IDeviceState.cs
# Only original temporary fixtures and explicit hand-written production sources.
set -euo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
# Fail clearly without downloading tools or treating missing SDK as a passing test.
if ! command -v dotnet >/dev/null 2>&1 || ! dotnet --list-sdks | grep -q '^10\.'; then
  echo "缺少 .NET 10 SDK：请按 CONTRIBUTING.md 安装微软官方 SDK 后重跑；未执行合成宏验证" >&2
  exit 75   # 缺 .NET 10 SDK = 这支没跑（环境未就绪，审计 P1-158），不是代码红也不是通过
fi
# Select an actually installed UTF-8 locale (Linux and macOS use different names).
UTF8=
for candidate in C.UTF-8 en_US.UTF-8 zh_CN.UTF-8; do
  if [ "$(LC_ALL="$candidate" locale charmap 2>/dev/null)" = UTF-8 ]; then
    UTF8=$candidate; break
  fi
done
if [ -z "$UTF8" ]; then
  echo "缺少可用 UTF-8 locale；未执行 locale 回归" >&2; exit 2
fi
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
python3 "$R/tests/fixtures/generate_mme.py" "$T/authored"
P=$R/tests/macrogen_synthetic/MacroSynthetic.csproj
timeout 120 dotnet run --project "$P" -c Release --artifacts-path "$T/gen-art" -- generate "$T/SyntheticTable.cs" "$T/authored"
timeout 120 dotnet run --project "$P" -c Release --artifacts-path "$T/test-art" -p:SyntheticTable="$T/SyntheticTable.cs" -- verify "$T/authored"
echo "private corpus locale regression: C / ${UTF8}"
LC_ALL=C python3 "$R/tests/macrogen_private_test.py"
LC_ALL="$UTF8" python3 "$R/tests/macrogen_private_test.py"
# Exercise the /var -> /private class of alias without claiming a macOS run.
mkdir "$T/real-temp"
ln -s "$T/real-temp" "$T/alias-temp"
TMPDIR="$T/alias-temp" LC_ALL="$UTF8" python3 "$R/tests/macrogen_private_test.py"
python3 "$R/tests/macrogen_fixture_test.py"

# Effective negative control: independently change the literal 7 to 8, regenerate
# that synthetic table, and require the unchanged semantic expectation to fail.
cp -R "$T/authored" "$T/changed"
python3 - "$T/changed/send_7.mme" <<'PYCODE'
from pathlib import Path
import sys
p=Path(sys.argv[1])
b=bytearray(p.read_bytes())
b[4:8]=bytes.fromhex("c1000200")  # fixed opcode: send 8 + exit, not 7
p.write_bytes(b)
PYCODE
timeout 120 dotnet run --project "$P" -c Release --artifacts-path "$T/gen-art" -- generate "$T/ChangedTable.cs" "$T/changed"
if timeout 120 dotnet run --project "$P" -c Release --artifacts-path "$T/negative-art" -p:SyntheticTable="$T/ChangedTable.cs" -- verify "$T/changed" > "$T/negative.log" 2>&1; then
  echo "FAIL: altered literal escaped independent expectation" >&2; exit 1
fi
grep -q "literal write expectation failed: send_7" "$T/negative.log" || { cat "$T/negative.log"; exit 1; }
echo "negative control: changed literal 7 -> 8 rejected by fixed expectation"
