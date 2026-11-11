#!/bin/bash
# NativeAOT 缓存键覆盖面单测（services/nxaot.sh --files / --key，tools/nxaot_inputs.py；约 20 秒，要 dotnet）：bash tests/nxaot_key_test.sh
# 期望（不跑被测实现就能定）：
#   ① MSBuild 列出的整个工程闭包编译输入（Compile / EmbeddedResource / AdditionalFiles / 工程文件），每一个都在键的文件集里——漏一个就会拿旧库（假绿）；
#   ② 在键目录副本里改一份 .tsv / .sh / .md（编译不读）→ 键不变；改一份被编进去的 .cs（含目录外单个编入的 graphics/vulkan/cache/ComputeTranslationProfile.cs）→ 键变
set -u; H=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d); trap 'rm -rf "$T"' EXIT; bad=0
ck() { if eval "$2"; then echo "通 $1"; else echo "不通 $1"; bad=1; fi; }
DN=${DOTNET:-$HOME/.dotnet/dotnet}; [ -x "$DN" ] || DN=$(command -v dotnet || true)
[ -n "$DN" ] || { echo "nxaot_key：没有 dotnet，MSBuild 列不出输入，没跑（环境未就绪）"; exit 75; }
DOTNET=$DN python3 "$H/tools/nxaot_inputs.py" "$H" > "$T/in.txt" 2> "$T/in.err"; r=$?
ck "MSBuild 列出工程闭包输入（rc=${r}，$(grep -c . "$T/in.txt") 个；$(tail -1 "$T/in.err" | cut -c1-120)）" '[ $r = 0 ] && [ "$(grep -c . "$T/in.txt")" -gt 1000 ]'
bash "$H/services/nxaot.sh" ios-arm64 --files > "$T/key.txt"
miss=$(grep -vxFf "$T/key.txt" "$T/in.txt" || true)
ck "① 编译输入全在键里（输入 $(grep -c . "$T/in.txt") 个、键 $(grep -c . "$T/key.txt") 个，漏：$(echo $miss | cut -c1-300)）" '[ -z "$miss" ]'
# ② 在副本上改：键目录与 nxaot.sh 克隆进临时仓库（APFS 克隆，秒级），被测脚本按自己所在仓库算键
C=$T/repo; mkdir -p "$C/services"; (cd "$H" && for p in $(sed 's|/.*||' "$T/key.txt" | sort -u); do cp -cR "$p" "$C/" 2> /dev/null || cp -R "$p" "$C/"; done)
key() { DOTNET=$DN bash "$C/services/nxaot.sh" ios-arm64 --key; }
K0=$(key); echo "注释" >> "$C/graphics/widen/scan/README.md" 2> /dev/null || { mkdir -p "$C/graphics/widen/scan"; echo x > "$C/graphics/widen/scan/README.md"; }
printf 'x\t1\n' >> "$C/graphics/widen/tables/verified.tsv"; echo '# x' >> "$C/graphics/widen/scan/closed_loop.sh"; K1=$(key)
ck "② 改说明 / 表格 / 脚本键不变（${K0} / ${K1}）" '[ ${#K0} = 16 ] && [ "$K1" = "$K0" ]'
echo '// x' >> "$C/graphics/vulkan/cache/ComputeTranslationProfile.cs"; K2=$(key)
ck "② 改目录外单个编入的源键变（${K1} → ${K2}）" '[ ${#K2} = 16 ] && [ "$K2" != "$K1" ]'
f=$(grep -m1 '^compat/tree/src/Ryujinx.HLE/.*\.cs$' "$T/in.txt"); echo '// x' >> "$C/$f"; K3=$(key)
ck "② 改 Ryujinx 源键变（${f##*/}：${K2} → ${K3}）" '[ ${#K3} = 16 ] && [ "$K3" != "$K2" ]'
[ $bad = 0 ] && echo "nxaot_key 单测全过"; exit $bad
