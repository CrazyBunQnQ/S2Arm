#!/bin/bash
# 门禁覆盖：services/*
# 服务层生命周期 C# 单测（10-29 基础设施线：bb82b5511 合入的 *.Lifecycle.Tests 没有门禁脚本调，接线扫描纳入 C# 工程后查出）。
#   AppleAudio.Lifecycle.Tests 要 NativeAOT 产物才覆盖 GCHandle 槽复用（dotnet run 走 JIT 时测试自己抛「本回归必须运行NativeAOT产物」，10-29 普查的「单跑红」就是这个）：
#   按 services/README.md 发布后跑产物（11-01 审计 P1-64；本机与 gate-host 实测发布 + 运行全过）。非 macOS 没有 Apple 音频后端：其余全过时退出 75（环境未就绪）
set -u; H=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
DN=${DOTNET:-$HOME/.dotnet/dotnet}; [ -x "$DN" ] || DN=$(command -v dotnet || true)
# 没有 .NET SDK：这支没跑，退 75（环境未就绪，审计 P1-154）；原先 DN 为空仍去执行 timeout 600 ""，rc=127 记成真红
[ -n "$DN" ] || { echo "services_lifecycle：没有 .NET SDK，没跑（环境未就绪）" >&2; exit 75; }
bad=0; n=0
run() { n=$((n + 1)); local o r; o=$(MSBUILDDISABLENODEREUSE=1 timeout 600 "$DN" run -c Release --project "$1" --artifacts-path "$T/$n" 2>&1 < /dev/null); r=$?
  if [ $r = 0 ]; then echo "通 ${1#$H/}：$(tail -1 <<< "$o" | cut -c1-120)"; else echo "不通 ${1#$H/}（rc=${r}）：$(tail -3 <<< "$o" | tr '\n' ' ' | cut -c1-300)"; bad=1; fi; }
run "$H/services/FileSystem.Lifecycle.Tests/FileSystem.Lifecycle.Tests.csproj"
run "$H/services/MemDiag.Lifecycle.Tests/MemDiag.Lifecycle.Tests.csproj"
aot() { n=$((n + 1)); local o r; o=$(MSBUILDDISABLENODEREUSE=1 timeout 600 "$DN" publish "$1" -c Release -r "$2" --artifacts-path "$T/$n" -o "$T/$n/pub" 2>&1 < /dev/null) && o=$(timeout 60 "$T/$n/pub/$3" 2>&1 < /dev/null); r=$?
  if [ $r = 0 ]; then echo "通 ${1#$H/}（NativeAOT）：$(tail -1 <<< "$o" | cut -c1-120)"; else echo "不通 ${1#$H/}（NativeAOT，rc=${r}）：$(tail -3 <<< "$o" | tr '\n' ' ' | cut -c1-300)"; bad=1; fi; }
apple=1
case "$(uname -s)/$(uname -m)" in
  Darwin/arm64) aot "$H/services/AppleAudio.Lifecycle.Tests/AppleAudio.Lifecycle.Tests.csproj" osx-arm64 Ryujinx.Audio.Backends.Apple ;;
  Darwin/x86_64) aot "$H/services/AppleAudio.Lifecycle.Tests/AppleAudio.Lifecycle.Tests.csproj" osx-x64 Ryujinx.Audio.Backends.Apple ;;
  *) apple=0 ;;
esac
[ $bad = 0 ] || { echo "services_lifecycle 单测有不通"; exit 1; }
[ $apple = 1 ] || { echo "services_lifecycle：${n} 个工程全过；Apple 音频生命周期要 macOS，本机 $(uname -s) 未测（环境未就绪，退出 75）"; exit 75; }
echo "services_lifecycle 单测全过（${n} 个工程）"
