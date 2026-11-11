#!/bin/bash
# 发布包签名 + 公证 + 钉票据（维护方内部文档，发布步骤）。原地处理 build_app.py 打出的 S2Arm.app，
# 结束后同目录出最终发行产物 S2Arm-mac-arm64.dmg（过程 zip 只用于 notarytool 提交，通过后删除）。
# 为什么：ad-hoc 签名的身份就是本版内容哈希，玩家每升级一版系统都当新 App，「可移动卷宗 / 下载 / 桌面」等文件访问授权全部重弹；
#   Developer ID 签名的身份 = 团队号 + 包名，跨版本不变。公证后玩家首次打开也不用再去「隐私与安全性」里放行。
# 公证要求包里每个 Mach-O 都用 Developer ID + 强化运行时 + 时间戳签：由里到外逐个签，再签嵌套 .app，最后签整包。
#   强化运行时的授权项（release.entitlements）放宽到与 ad-hoc 时等效：.NET 要 JIT、游戏宿主要可写可执行内存、各运行库互相加载不校验签名者。
# 环境：S2A_SIGN_KEYCHAIN 放证书的钥匙串；S2A_SIGN_KEYCHAIN_PASS_FILE 钥匙串口令文件（只从文件读，不打印、不进日志）；
#       ASC_KEY_ID / ASC_ISSUER / ASC_KEY_PATH App Store Connect 接口密钥（公证用）。S2A_NOTARIZE=0 只签不公证。
#       S2A_DMG_PYTHON 可选：装有 DMG 打包依赖的解释器（缺省用 build/dmg-venv/bin/python，
#       没有则用 S2A_PYTHON 或 python3；准备命令见 DMG 准备段与 README）。
set -euo pipefail
APP=$(cd "$1" && pwd); HERE=$(cd "$(dirname "$0")" && pwd); ENT=$HERE/release.entitlements; KC=${S2A_SIGN_KEYCHAIN:?请设 S2A_SIGN_KEYCHAIN}
if [ -n "${S2A_SIGN_KEYCHAIN_PASS_FILE:-}" ]; then security unlock-keychain -p "$(cat "$S2A_SIGN_KEYCHAIN_PASS_FILE")" "$KC"; fi
ID=$(security find-identity -v -p codesigning "$KC" | sed -n 's/.*"\(Developer ID Application:.*\)"/\1/p' | head -1)
[ -n "$ID" ] || { echo "【红】$KC 里没有 Developer ID Application 证书" >&2; exit 2; }
xattr -cr "$APP"
# 时间戳服务器偶发不应答：每个对象最多试 3 次，还不行把 codesign 的原话打出来。不并行（多路同时取私钥实测出错）
sign() {
  local i err
  for i in 1 2 3; do
    err=$(codesign --force --timestamp --options runtime --entitlements "$ENT" --keychain "$KC" -s "$ID" "$1" 2>&1) && return 0
    sleep 2
  done
  echo "【红】签名失败：$1：$err" >&2; return 1
}
# 按文件头认 Mach-O（不信扩展名），路径深的先签
python3 -I - "$APP" > "$APP.machos" <<'PY'
import sys, pathlib
magic = {b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"}
root = pathlib.Path(sys.argv[1])
SHELL = root / "Contents/Resources/Shell"   # 预编 iOS 壳：不签（见下）
files = [p for p in root.rglob("*") if p.is_file() and not p.is_symlink() and "_CodeSignature" not in p.parts and SHELL not in p.parents and p.open("rb").read(4) in magic]
for p in sorted(files, key=lambda p: -len(p.parts)):
    print(p)
PY
n=$(wc -l < "$APP.machos" | tr -d ' ')
while IFS= read -r f; do sign "$f"; done < "$APP.machos"
rm -f "$APP.machos"
# 再签捆绑包：.app / .framework，以及自带 _CodeSignature 的目录（Runtime/python 本身就是以 Python 为主程序的捆绑包，
#   要在它里面的 Python.app 之后签，否则封存的资源被后签的改掉、公证报「signature of the binary is invalid」）。由深到浅
python3 -I - "$APP" > "$APP.bundles" <<'PY'
import sys, pathlib
root = pathlib.Path(sys.argv[1])
SHELL = root / "Contents/Resources/Shell"
dirs = {p for p in (root / "Contents").rglob("*") if p.is_dir() and not p.is_symlink() and SHELL not in p.parents and p != SHELL and (p.suffix in (".app", ".framework") or (p.name != "Contents" and (p / "_CodeSignature").is_dir()))}
for p in sorted(dirs, key=lambda p: -len(p.parts)):
    print(p)
PY
while IFS= read -r b; do sign "$b"; done < "$APP.bundles"
rm -f "$APP.bundles"
# 预编 iOS 壳（Contents/Resources/Shell）不签（11-08 #1669 / beta.8 验收）：它是出 iPhone 包的组装模板，出包要求它未签名且字节与 Shell/manifest.json 一致
#   （pack/app_bundle.py「组装模板须未签名」、启动器开工前核壳清单）；上一版（beta.7）把它也签了，签名改写 Mach-O、新增 _CodeSignature，启动器核壳清单 4 处不符，
#   补重算清单后出包仍被「发现签名残留」挡住。公证不要求签 iOS 平台的二进制（本脚本不签壳的包 11-08 实测 notarytool status: Accepted、spctl Notarized Developer ID）
# 引擎清单按签名后的最终字节重算（放在内嵌捆绑包都签完之后、签整包之前：签 S2ArmGame.app 会重写其主程序 s2arm-host；Engine/engine.json、Prepare/prepare.json 记逐文件大小 + sha256，启动器开工前校验；不重算报「准备产物缺失或大小不符」）
"${S2A_PYTHON:-python3}" -I -c 'import json, sys; sys.path.insert(0, sys.argv[1]); from bundle_engine import write_manifest; e = sys.argv[2]; write_manifest(e, json.load(open(e + "/engine.json"))["source"])' "$HERE" "$APP/Contents/Resources/Engine"
sign "$APP"
codesign --verify --deep --strict "$APP"
"${S2A_PYTHON:-python3}" -I - "$APP/Contents/Resources/Engine" <<'PY'
import hashlib, json, pathlib, sys
root = pathlib.Path(sys.argv[1]); bad = []
for name, (size, sha) in json.load(open(root / "engine.json"))["files"].items():
    p = root / name
    if not p.is_file() or p.stat().st_size != size or hashlib.sha256(p.read_bytes()).hexdigest() != sha:
        bad.append(name)
if bad:
    sys.exit("【红】引擎清单与签名后的文件对不上：" + "、".join(bad[:5]))
PY
python3 -I "$HERE/../../pack/shell/manifest.py" verify --root "$APP/Contents/Resources/Shell/root" --manifest "$APP/Contents/Resources/Shell/manifest.json" > /dev/null || { echo "【红】壳清单与签名后的文件对不上" >&2; exit 3; }
codesign -d -r- "$APP" 2>&1 | grep -q 'designated => .*certificate leaf' || { echo "【红】指定要求不是证书形式" >&2; exit 3; }
echo "签名：$n 个 Mach-O + 整包，Developer ID，强化运行时，校验通过"
DIR=$(dirname "$APP"); Z=$DIR/S2Arm.zip; DMG=$DIR/S2Arm-mac-arm64.dmg
rm -f "$Z" "$DMG"
# zip 只当 notarytool 的提交载体；公证 Accepted 并钉票据后立刻删除，最终发行产物只有 DMG。
if [ "${S2A_NOTARIZE:-1}" != 0 ]; then
  ditto -c -k --keepParent "$APP" "$Z"
  log=$DIR/notary.log
  xcrun notarytool submit "$Z" --key "${ASC_KEY_PATH:?}" --key-id "${ASC_KEY_ID:?}" --issuer "${ASC_ISSUER:?}" --wait > "$log" 2>&1 || true
  grep -q 'status: Accepted' "$log" || { echo "【红】公证没通过，见 ${log}（xcrun notarytool log <id> 看逐项原因）" >&2; exit 4; }
  xcrun stapler staple "$APP" > /dev/null
  rm -f "$Z"
  spctl -a -vv -t exec "$APP" 2>&1 | sed -n 1,2p
  spctl -a -t exec "$APP" || { echo "【红】Gatekeeper 不认（spctl rejected）" >&2; exit 5; }
fi
# 钉完票据后用同一个 App 出 DMG：build_dmg.py 校验布局/资产/App 签名后打包，
# --check 挂载成品只读核对 App 签名、/Applications 符号链接、1×/2× 背景、.DS_Store 窗口 rect 与图标坐标。
ROOT=$(cd "$HERE/../.." && pwd)
DMG_PY=${S2A_DMG_PYTHON:-}
if [ -z "$DMG_PY" ]; then
  if [ -x "$ROOT/build/dmg-venv/bin/python" ]; then DMG_PY=$ROOT/build/dmg-venv/bin/python; else DMG_PY=${S2A_PYTHON:-python3}; fi
fi
"$DMG_PY" -I -c 'import dmgbuild, ds_store' 2>/dev/null || {
  echo "【红】发版需要 DMG 打包依赖。准备命令：python3 -m venv build/dmg-venv && build/dmg-venv/bin/python -m pip install -r launchers/macos/requirements-dmg.txt（也可用 S2A_DMG_PYTHON 指定已装好 dmgbuild 与 ds_store 的解释器）" >&2
  exit 6
}
"$DMG_PY" "$HERE/build_dmg.py" --app "$APP" --output "$DMG"
"$DMG_PY" "$HERE/build_dmg.py" --check "$DMG"
shasum -a 256 "$DMG"
