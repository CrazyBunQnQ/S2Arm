#!/bin/bash
# 独立检出自包含：先编真实服务程序集，再用临时动态库测 provider、远端描述符和 LDN 钩子。
set -euo pipefail
cd "$(dirname "$0")/.."
DOTNET=${DOTNET:-$HOME/.dotnet/dotnet}; [ -x "$DOTNET" ] || DOTNET=$(command -v dotnet || true)
[ -n "$DOTNET" ] || { echo "services_provider：没有 .NET SDK，没跑（环境未就绪，审计 P1-158）" >&2; exit 75; }
export DOTNET_ROOT=${DOTNET_ROOT:-$HOME/.dotnet}
T=$(mktemp -d "${TMPDIR:-/tmp}/s2a-provider-build.XXXXXX")
trap 'rm -rf "$T"' EXIT
# 与并行的 services/gate.sh 使用不同的 obj/bin 与生成目录。
timeout 240 "$DOTNET" build services/S2A.Services.Tests/S2A.Services.Tests.csproj -c Release -nologo -v q \
  --artifacts-path "$T/artifacts" -p:EmitCompilerGeneratedFiles=false
export S2A_PROVIDER_DLL="$T/artifacts/bin/S2A.Services.Tests/release/S2A.Services.Tests.dll"
bash services/tests/t_native_provider.sh
