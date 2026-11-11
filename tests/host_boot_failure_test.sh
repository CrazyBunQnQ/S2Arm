#!/bin/bash
# 门禁覆盖：services/S2A.Services/Host.cs tests/host_boot_failure/
# Real filesystem failures through Host.Boot; no fake native layer or renderer.
set -euo pipefail
cd "$(dirname "$0")/.."
DOTNET=${DOTNET:-$HOME/.dotnet/dotnet}; [ -x "$DOTNET" ] || DOTNET=$(command -v dotnet || true)
[ -n "$DOTNET" ] || { echo "host_boot_failure：没有 .NET SDK，没跑（环境未就绪，审计 P1-158）" >&2; exit 75; }
"$DOTNET" build tests/host_boot_failure/HostBootFailure.csproj -c Release --nologo
"$DOTNET" tests/host_boot_failure/bin/Release/net10.0/HostBootFailure.dll
