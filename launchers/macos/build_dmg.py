#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Package an existing signed S2Arm.app with the branded Finder installation window.

打包入口：
  --preview <目录>        只渲染背景和示意预览，不打包应用（不需要 dmgbuild）
  --app/--output          用已完成签名/公证的 S2Arm.app 产出 DMG
  --check <DMG>           挂载成品 DMG（只读）逐项校验签名、背景、快捷方式和 Finder 布局

所有输入错误都用中文报出可照做的原因并以退出码 2 结束；调用 Swift 渲染器之前先校验
布局与资产，避免渲染器里的强制转换/强制解包把错误变成 SIGABRT / SIGTRAP。
"""
import argparse
import json
import os
from pathlib import Path
import plistlib
import re
import struct
import subprocess
import sys
import tempfile

from frontend_layout import FRONTENDS

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

LAYOUT_FILE = HERE / "Branding/dmg-layout.json"
RENDERER = HERE / "Branding/render_dmg.swift"
# 字标必须用站点同款透明底版本：opaque 版 docs/images/s2arm-wordmark.png 没有 alpha，
# 铺在近黑背景上会露出黑色圆角块接缝（审计 build/dmg-audit/preview/wordmark-crop.png）。
WORDMARK = ROOT / "docs/images/s2arm-wordmark-transparent.png"
APP_ICON = HERE / "Branding/AppIcon-1024.png"

# 布局允许范围：当前设计为 760×560、iconSize 104；范围只用来挡住明显写错的输入，
# 不是新的设计约束。改设计时同步改这里与 Branding/render_dmg.swift 的同名常量。
LAYOUT_LIMITS = {"width": (480, 1600), "height": (360, 1200), "iconSize": (48, 256)}
LAYOUT_KEYS = ("width", "height", "iconSize", "iconPositions")
REQUIRED_POSITIONS = ("S2Arm.app", "Applications")

# 窗口 rect 与 Finder 标题栏增量（依据：同一 AppKit API 复测 macOS 27.2，
# NSWindow(title, contentRect 760×560) → frame 760×592，contentLayout 760×560，
# 标题栏增量 32 点；审计 probe.swift 的 stdout 没有落盘，读数为复测而非原始输出）。
# dmgbuild 的 window_rect 写的是 Finder 窗口 frame 尺寸，所以高度 = 背景高 + 32。
TITLEBAR = 32
WINDOW_ORIGIN = (180, 160)
TEXT_SIZE = 13  # Finder 图标名称字号，与 .DS_Store 的 textSize 对应

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


def fail(message):
    """打印中文错误并以退出码 2 结束（调用方脚本据此判定红）。"""
    print(f"【红】{message}", file=sys.stderr)
    raise SystemExit(2)


def relative(path):
    """优先用仓库相对路径写错误信息，照做时更好复制。"""
    try:
        return str(Path(path).resolve().relative_to(ROOT))
    except ValueError:
        return str(path)


def is_int(value):
    """JSON 里的布尔值在 Python 是 int 子类，必须单独排除。"""
    return isinstance(value, int) and not isinstance(value, bool)


def load_layout(path=LAYOUT_FILE):
    """读取并校验 dmg-layout.json，返回归一化后的布局。

    校验项：合法 JSON、顶层对象、键集合、width/height/iconSize 为整数且在范围内、
    iconPositions 为对象且至少包含 S2Arm.app 与 Applications、坐标为两个整数并让
    图标完整落在画布内。任何一项不合格都给出中文原因和修正办法。
    """
    path = Path(path)
    if not path.is_file():
        fail(f"找不到布局文件：{relative(path)}；请确认在仓库内运行，并保留 Branding/dmg-layout.json")
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as error:
        fail(f"读不了布局文件 {relative(path)}：{error}；请检查文件权限与编码（应为 UTF-8）")
    try:
        data = json.loads(text)
    except json.JSONDecodeError as error:
        fail(
            f"布局文件不是合法 JSON：{relative(path)} 第 {error.lineno} 行第 {error.colno} 列：{error.msg}；"
            "可对照仓库中的 Branding/dmg-layout.json 修正（常见原因是少了引号或多了逗号）"
        )
    if not isinstance(data, dict):
        fail(f"布局文件顶层必须是 JSON 对象 {{...}}，当前是 {type(data).__name__}：{relative(path)}")

    missing = [key for key in LAYOUT_KEYS if key not in data]
    if missing:
        fail(
            f"布局文件缺少键：{'、'.join(missing)}；需要 {'、'.join(LAYOUT_KEYS)}，"
            f"参考 {relative(LAYOUT_FILE)}"
        )
    unknown = sorted(str(key) for key in data if key not in LAYOUT_KEYS)
    if unknown:
        fail(
            f"布局文件有多余的键：{'、'.join(unknown)}；只允许 {'、'.join(LAYOUT_KEYS)}，"
            "未知键不会被读取，请删掉或改成受支持的键"
        )

    layout = {}
    for key, (low, high) in LAYOUT_LIMITS.items():
        value = data[key]
        if not is_int(value):
            fail(
                f"布局字段 {key} 必须是整数（不带引号），当前是 {value!r}（{type(value).__name__}）；"
                f"例如 {key}: {low}，允许范围 {low}–{high}"
            )
        if not low <= value <= high:
            fail(f"布局字段 {key}={value} 超出允许范围 {low}–{high}；请按设计稿填写整数")
        layout[key] = value

    positions = data["iconPositions"]
    if not isinstance(positions, dict):
        fail(f"布局字段 iconPositions 必须是 JSON 对象 {{...}}，当前是 {type(positions).__name__}")
    for name in REQUIRED_POSITIONS:
        if name not in positions:
            fail(
                f"布局字段 iconPositions 缺少图标条目：{name}；必须同时包含 "
                f"{' 和 '.join(REQUIRED_POSITIONS)}（名称与 Finder 里显示的一致）"
            )
    icon_size = layout["iconSize"]
    half = icon_size / 2
    normalized = {}
    for name, value in positions.items():
        if not isinstance(value, list) or len(value) != 2:
            fail(f"图标坐标 {name} 必须是 [x, y] 两个整数，当前是 {value!r}")
        x, y = value
        if not is_int(x) or not is_int(y):
            fail(f"图标坐标 {name} 的 x/y 必须是整数，当前是 {value!r}；例如 [190, 280]")
        if not (half <= x <= layout["width"] - half) or not (half <= y <= layout["height"] - half):
            fail(
                f"图标坐标 {name}=[{x}, {y}] 超出画布：iconSize={icon_size} 时中心需在 "
                f"x {half:g}–{layout['width'] - half:g}、y {half:g}–{layout['height'] - half:g} 内，"
                "否则图标会被画布裁掉"
            )
        normalized[str(name)] = (x, y)
    layout["iconPositions"] = normalized
    return layout


def load_png(path, description):
    """确认资产存在且是有效 PNG（只读文件头，解码仍由渲染器负责）。"""
    path = Path(path)
    if not path.is_file():
        fail(f"缺少{description}：{relative(path)}；该文件应随仓库检出，请检查是否被删除或未拉取")
    try:
        with path.open("rb") as handle:
            header = handle.read(len(PNG_MAGIC))
    except OSError as error:
        fail(f"读不了{description} {relative(path)}：{error}；请检查文件权限")
    if header != PNG_MAGIC:
        fail(f"{description}不是有效的 PNG：{relative(path)}；请从仓库重新检出该文件")
    return path


def validate_assets(app=None):
    """在启动 Swift 渲染器之前确认布局与所有绘图资产可用。"""
    layout = load_layout()
    load_png(WORDMARK, "DMG 字标（站点同款透明底）")
    load_png(APP_ICON, "应用图标源图 1024×1024")
    if not RENDERER.is_file():
        fail(f"找不到背景渲染器：{relative(RENDERER)}；请检查仓库是否完整")
    if app is not None:
        app = Path(app)
        if not app.is_dir():
            fail(f"--app 指向的 S2Arm.app 不存在：{app}；请先运行 launchers/macos/build_app.py 并完成签名")
    return layout


def run_checked(command, what):
    """跑外部命令，失败时把工具原话收进中文错误，避免裸 traceback。"""
    try:
        return subprocess.run(command, check=True, capture_output=True, text=True)
    except FileNotFoundError:
        fail(f"找不到命令 {command[0]}：{what}需要它；请在 macOS 上运行并安装 Xcode 命令行工具")
    except subprocess.CalledProcessError as error:
        lines = [line for line in (error.stderr or error.stdout or "").splitlines() if line.strip()]
        detail = "；".join(lines[-3:]) if lines else "工具没有输出"
        fail(f"{what}失败（退出码 {error.returncode}）：{detail}")


def artwork(destination, app=None):
    """校验输入后用 Swift 渲染背景与预览，输出到 destination。"""
    destination = Path(destination)
    validate_assets(app)
    destination.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    # The compiler cache is a build artifact; keep it in the requested output tree.
    env["CLANG_MODULE_CACHE_PATH"] = str(destination / "module-cache")
    command = ["xcrun", "swift", str(RENDERER), str(destination)]
    if app:
        command.append(str(app))
    try:
        result = subprocess.run(command, capture_output=True, text=True, env=env)
    except FileNotFoundError:
        fail("找不到 xcrun：背景渲染需要 Xcode 命令行工具；请先安装 Command Line Tools")
    if result.returncode != 0:
        lines = [line for line in (result.stderr or result.stdout or "").splitlines() if line.strip()]
        detail = "；".join(lines[-3:]) if lines else "渲染器没有输出"
        fail(
            f"背景渲染失败（退出码 {result.returncode}）：{detail}；"
            "布局与资产已校验通过，请按上面的中文提示修 Branding/ 下的输入"
        )
    if result.stdout.strip():
        print(result.stdout.strip())


def app_identity(app):
    """读取并核对应用包的 Info.plist，确认是 S2Arm.app。"""
    info_path = app / "Contents/Info.plist"
    try:
        info = plistlib.loads(info_path.read_bytes())
    except Exception as error:  # 坏 plist 的异常类型不统一，统一转成中文错误
        fail(f"读不了应用包信息：{info_path}：{error}；请传入 build_app.py 打出的完整 S2Arm.app")
    identities = {(front["bundle_id"], front["product"]) for front in FRONTENDS.values()}
    identity = (info.get("CFBundleIdentifier"), info.get("CFBundleExecutable"))
    if app.name != "S2Arm.app" or identity not in identities:
        fail(
            f"指定的包不是 S2Arm.app：名称={app.name}、CFBundleIdentifier={identity[0]}、"
            f"CFBundleExecutable={identity[1]}；请传入 build_app.py 打出的完整 S2Arm.app"
        )
    return info


def build(app, output):
    try:
        import dmgbuild  # noqa: F401
    except ImportError:
        fail(
            "缺少 DMG 打包依赖 dmgbuild；请先准备维护者环境："
            "python3 -m venv build/dmg-venv && "
            "build/dmg-venv/bin/python -m pip install -r launchers/macos/requirements-dmg.txt，"
            "然后用 build/dmg-venv/bin/python 运行本脚本"
        )

    app, output = Path(app).resolve(), Path(output).resolve()
    layout = validate_assets(app)
    info = app_identity(app)
    if app == output or app in output.parents:
        fail(f"DMG 输出不能放进应用包内：{output}；请换一个仓库内路径，例如 build/macos/S2Arm-mac-arm64.dmg")

    run_checked(["codesign", "--verify", "--deep", "--strict", str(app)], "应用包签名校验")
    try:
        output.parent.mkdir(parents=True, exist_ok=True)
    except OSError as error:
        fail(f"创建输出目录失败：{output.parent}：{error}；请检查路径与权限")

    with tempfile.TemporaryDirectory(prefix=".s2arm-dmg-", dir=output.parent) as directory:
        work = Path(directory)
        artwork(work, app)
        settings = {
            "format": "ULFO", "filesystem": "HFS+",
            "files": [str(app)], "symlinks": {"Applications": "/Applications"},
            "background": str(work / "background.tiff"),
            "window_rect": (WINDOW_ORIGIN, (layout["width"], layout["height"] + TITLEBAR)),
            "icon_locations": {name: point for name, point in layout["iconPositions"].items()},
            "icon_size": layout["iconSize"], "text_size": TEXT_SIZE, "label_pos": "bottom",
            "default_view": "icon-view", "include_icon_view_settings": True,
            "show_toolbar": False, "show_sidebar": False, "show_status_bar": False,
            "show_tab_view": False, "show_pathbar": False, "show_icon_preview": False,
            "arrange_by": None, "scroll_position": (0, 0),
            # Hiding .app adds FinderInfo to the signed bundle. Preserve its bytes.
            "hide_extensions": [],
            "create_hook": lambda mount, _: run_checked(
                ["codesign", "--verify", "--deep", "--strict", str(Path(mount) / app.name)], "DMG 内应用签名校验"
            ),
        }
        icon_name = info.get("CFBundleIconFile")
        icon = app / "Contents/Resources" / (icon_name if isinstance(icon_name, str) and icon_name else "S2Arm")
        if not icon.suffix:
            icon = icon.with_suffix(".icns")
        if icon.is_file():
            settings["icon"] = str(icon)
        temporary = work / "installer.dmg"
        try:
            dmgbuild.build_dmg(str(temporary), "S2Arm Installer", settings=settings)
        except SystemExit:
            raise
        except Exception as error:  # dmgbuild 用各种异常类型报错，统一转成中文
            fail(
                f"dmgbuild 打包失败：{error}；请确认 build/dmg-venv 已按 requirements-dmg.txt 装好依赖，"
                "并检查上面渲染步骤的输出"
            )
        run_checked(["hdiutil", "verify", str(temporary)], "DMG 映像校验")
        try:
            os.replace(temporary, output)
        except OSError as error:
            fail(f"写出成品 DMG 失败：{output}：{error}；请检查目标路径与权限")
    return output


def _command_text(command, what):
    result = run_checked(command, what)
    return (result.stdout or "") + (result.stderr or "")


def check_background(path, layout):
    """确认背景图存在、可解码，并且包含 1× / 2× 两帧（Retina 输出）。"""
    path = Path(path)
    if not path.is_file():
        fail("DMG 里缺少背景图 .background.tiff；安装窗口会退回纯色背景，请重新用本脚本打包")
    info = _command_text(["sips", "-g", "pixelWidth", "-g", "pixelHeight", str(path)], "背景图解码")
    width = re.search(r"pixelWidth: (\d+)", info)
    height = re.search(r"pixelHeight: (\d+)", info)
    if not width or not height:
        fail("背景图 .background.tiff 解不出像素尺寸；请确认它是 Swift 渲染器输出的 1×/2× TIFF")
    if (int(width.group(1)), int(height.group(1))) != (layout["width"] * 2, layout["height"] * 2):
        fail(
            f"背景图尺寸 {width.group(1)}×{height.group(1)} 与布局不符；"
            f"期望 2× 帧 {layout['width'] * 2}×{layout['height'] * 2}（布局 {layout['width']}×{layout['height']} 的 Retina 输出）"
        )
    frames = _command_text(["tiffutil", "-info", str(path)], "背景图帧信息")
    found = [(int(w), int(h)) for w, h in re.findall(r"Image Width: (\d+) Image Length: (\d+)", frames)]
    expected = {(layout["width"] * 2, layout["height"] * 2), (layout["width"], layout["height"])}
    missing = expected.difference(found)
    if missing:
        detail = "、".join(f"{w}×{h}" for w, h in sorted(missing))
        fail(
            f"背景图缺少帧：{detail}；当前帧为 {found}，Finder 需要 1× 与 2× 两帧才能在 Retina 下清晰，"
            "请重新用本脚本打包"
        )
    return found


def check_ds_store(mount, layout):
    """用 dmgbuild 依赖里的 ds_store 读 .DS_Store，核对窗口 rect 与图标坐标。"""
    try:
        from ds_store import DSStore
    except ImportError:
        fail(
            "缺少 ds_store 包（dmgbuild 依赖），--check 需要它读取 .DS_Store；请准备维护者环境："
            "python3 -m venv build/dmg-venv && "
            "build/dmg-venv/bin/python -m pip install -r launchers/macos/requirements-dmg.txt"
        )
    path = Path(mount) / ".DS_Store"
    if not path.is_file():
        fail("DMG 里没有 .DS_Store；Finder 窗口布局没有写入，请重新用本脚本打包")
    try:
        with DSStore.open(str(path), "r") as store:
            window = store["."]["bwsp"]
            view = store["."]["icvp"]
            locations = {name: store[name]["Iloc"] for name in layout["iconPositions"]}
    except KeyError as error:
        fail(f".DS_Store 缺少 Finder 布局项：{error}；请重新用本脚本打包，不要手改 DMG")
    except (OSError, ValueError, IndexError, struct.error) as error:
        fail(f"解析 .DS_Store 失败：{error}；请重新用本脚本打包")

    if not isinstance(window, dict) or not isinstance(view, dict):
        fail(".DS_Store 里的 Finder 布局项不是字典；请重新用本脚本打包，不要手改 DMG")
    bounds = window.get("WindowBounds")
    match = re.fullmatch(r"\{\{(-?\d+), (-?\d+)\}, \{(-?\d+), (-?\d+)\}\}", str(bounds))
    if not match:
        fail(f".DS_Store 的窗口 rect 格式无法识别：{bounds!r}；请重新用本脚本打包")
    x, y, width, height = (int(value) for value in match.groups())
    expected = (WINDOW_ORIGIN[0], WINDOW_ORIGIN[1], layout["width"], layout["height"] + TITLEBAR)
    if (x, y, width, height) != expected:
        fail(
            f".DS_Store 窗口 rect 与布局不一致：实际 {{ {{{x}, {y}}}, {{{width}, {height}}} }}，"
            f"期望 {{ {{{expected[0]}, {expected[1]}}}, {{{expected[2]}, {expected[3]}}} }}"
            f"（布局 {layout['width']}×{layout['height']} + Finder 标题栏 {TITLEBAR} 点）；请重新用本脚本打包"
        )
    for key, want in (("ShowPathbar", False), ("ShowSidebar", False), ("ShowToolbar", False),
                      ("ShowStatusBar", False), ("PreviewPaneVisibility", False), ("ContainerShowSidebar", False)):
        if window.get(key, want) != want:
            fail(f".DS_Store 的 {key} 应为 {want}，实际 {window.get(key)!r}；请重新用本脚本打包")
    def number(key):
        try:
            return float(view.get(key))
        except (TypeError, ValueError):
            fail(f".DS_Store 的 {key} 不是数字：{view.get(key)!r}；请重新用本脚本打包")

    if abs(number("iconSize") - layout["iconSize"]) > 0.001:
        fail(f".DS_Store 的 iconSize 应为 {layout['iconSize']}，实际 {view.get('iconSize')!r}")
    if abs(number("textSize") - TEXT_SIZE) > 0.001:
        fail(f".DS_Store 的 textSize 应为 {TEXT_SIZE}，实际 {view.get('textSize')!r}")
    if view.get("labelOnBottom") is not True:
        fail(".DS_Store 的名称标签不在图标下方（labelOnBottom 应为 true）")
    if view.get("arrangeBy") not in (None, "none"):
        fail(f".DS_Store 不应重排图标（arrangeBy={view.get('arrangeBy')!r}）；请重新用本脚本打包")
    if view.get("showIconPreview") is not False:
        fail(".DS_Store 的 showIconPreview 应为 false")

    for name, point in layout["iconPositions"].items():
        actual = locations.get(name)
        if actual is None:
            fail(f".DS_Store 缺少图标坐标：{name}；请重新用本脚本打包")
        if tuple(actual) != tuple(point):
            fail(
                f".DS_Store 的图标坐标 {name} 与布局不一致：实际 {tuple(actual)}，期望 {tuple(point)}；"
                "请重新用本脚本打包或修正 dmg-layout.json"
            )
    return width, height


def check(dmg):
    """挂载成品 DMG（只读）逐项校验，任一项不合格以退出码 2 报红。"""
    dmg = Path(dmg).resolve()
    layout = load_layout()
    if not dmg.is_file():
        fail(f"找不到要校验的 DMG：{dmg}；请传入 build_dmg.py 打出的 S2Arm-mac-arm64.dmg")
    if not dmg.stat().st_size:
        fail(f"DMG 是空文件：{dmg}；请重新打包")
    with tempfile.TemporaryDirectory(prefix=".s2arm-dmg-check-") as directory:
        mount = Path(directory) / "vol"
        mount.mkdir()
        run_checked(
            ["hdiutil", "attach", "-readonly", "-nobrowse", "-noautoopen", "-mountpoint", str(mount), str(dmg)],
            "DMG 挂载",
        )
        try:
            app = mount / "S2Arm.app"
            if not app.is_dir():
                fail(f"DMG 里没有 S2Arm.app：{app}；请确认打包时 --app 传入的是完整应用包")
            run_checked(["codesign", "--verify", "--deep", "--strict", str(app)], "成品 DMG 内应用签名校验")

            applications = mount / "Applications"
            if not applications.is_symlink() or os.readlink(applications) != "/Applications":
                actual = os.readlink(applications) if applications.is_symlink() else "不是符号链接"
                fail(f"DMG 里的 Applications 应是指向 /Applications 的符号链接，实际：{actual}")

            frames = check_background(mount / ".background.tiff", layout)
            width, height = check_ds_store(mount, layout)
        finally:
            # 只读挂载也要无条件卸载；卸载失败不掩盖上面的校验错误。
            subprocess.run(["hdiutil", "detach", "-force", str(mount)], capture_output=True, text=True)
    coordinates = "、".join(f"{name}={point}" for name, point in layout["iconPositions"].items())
    window = "{{%d, %d}, {%d, %d}}" % (WINDOW_ORIGIN[0], WINDOW_ORIGIN[1], width, height)
    print(
        f"✅ DMG 校验通过：{dmg.name}\n"
        f"   app 签名：codesign --verify --deep --strict S2Arm.app\n"
        f"   /Applications：符号链接 → /Applications\n"
        f"   背景：.background.tiff 帧 {frames}（1×/2× 齐全）\n"
        f"   .DS_Store：窗口 {window}、iconSize={layout['iconSize']}、"
        f"textSize={TEXT_SIZE}、路径栏/侧栏/工具栏/状态栏隐藏\n"
        f"   图标坐标：{coordinates}"
    )
    return dmg


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--app", type=Path, default=ROOT / "build/macos/S2Arm.app",
                        help="已完成签名/公证的 S2Arm.app（默认 build/macos/S2Arm.app）")
    parser.add_argument("--output", type=Path, default=ROOT / "build/macos/S2Arm-mac-arm64.dmg",
                        help="成品 DMG 路径（默认 build/macos/S2Arm-mac-arm64.dmg）")
    parser.add_argument("--preview", type=Path, help="仅生成背景和示意预览，不打包应用；不需要 dmgbuild")
    parser.add_argument("--check", type=Path, metavar="DMG", help="挂载成品 DMG（只读），校验签名、背景与 Finder 布局")
    args = parser.parse_args()
    if args.check:
        check(args.check)
    elif args.preview:
        artwork(args.preview.resolve(), args.app.resolve() if args.app.is_dir() else None)
    else:
        print("Created:", build(args.app, args.output))


if __name__ == "__main__":
    main()
