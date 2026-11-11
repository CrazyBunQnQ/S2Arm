# iOS 游戏库界面：上游快照与我方改动

## 来源

- 上游：NX2Apple（MIT），目录 `frontend-ios/`，提交 `908e4e848fd232304381fc19d4d847fcc10fc4c2`。
- 许可：`LICENSE`、`NOTICE` 从上游仓库根原样拷来；字体 Manrope 的 OFL 许可在 `Sources/ShellUI/Resources/Manrope-OFL.txt`（随资源包进 App）。

## 取用（逐字未改，`diff -r` 与上游一致；例外见「我方改动」4 本地补丁）

| 目录 | 内容 |
|---|---|
| `Sources/ShellUI/` | 视图层：封面 / Cover Flow / 网格 / 单列、游戏详情与设置行、焦点系统、触屏布局编辑、按键映射卡、`Resources/`（Localizable.xcstrings、Manrope.ttf、Manrope-OFL.txt） |
| `Sources/ShellControllers/` | `GamepadAdapter`（GameController → `ShellInput` 语义导航）、`PadMapping` |

## 剔除

- `Sources/ShellHost/`：只借设计（@Observable 模型 + `ShellActions` + `startControllers`），不拷。它带 `EngineLaunchPlan`（DM_* 环境、`dm_main` ABI）、`.framework` 扫描、`GameLauncher` 的上游实现，与我方运行容器不同。
- `Sources/ShellDemo/`、`Demo/`、`Tests/`、`UITests/`、`AppIcon.icon`、`README.md`。上游 ShellUI 单测里的截图 / 布局测试以后要用再按需拷。

## 我方改动

1. `Package.swift` 重写：iOS 下限 26 → **18**（= `tools/toolchain.env` 的 `IPHONEOS_DEPLOYMENT_TARGET`）；macOS 保留 26，只为本机 `swift test` 跑宿主层单测；去掉 ShellHost / ShellDemo 目标，加 `S2ArmShellBridge`（C）与 `S2ArmShellHost`（Swift）。
2. iOS 18 门控：ShellUI / ShellControllers 在 iOS 18 下限下**零源码改动**即编过（Xcode 27.1、iOS SDK 27.1）。唯一的 18 之后 API 是上游原有的门控：`SideBar.swift` 的 `toolbarVerticalEdge`、`GeometryProxy.reservedRegions(kind: .occlusion)`（iOS 27.1），已有 `#if canImport(SwiftUI, _version: 8.1)` + `#available(iOS 27.1, *)` 双门，退路 `Color.clear`（侧边条走标准安全区）。本刀新加门控：0 个。
3. 新增 `Sources/S2ArmShellHost/`（我方最小宿主）与 `Sources/S2ArmShellBridge/`（C 边界 `include/s2a_shell.h`）。
4. **本地补丁**（「逐字一致」的例外，编不过时以能编为准；上游修了就删掉本条、换回原样）：
   | 文件 | 位置 | 原因 | 上游 SHA |
   |---|---|---|---|
   | `Sources/ShellUI/CoverFlowView.swift`、`PortraitLibrary.swift`、`LibraryView.swift`、`ShellInputs.swift`、`Resources/Localizable.xcstrings` | 单列排序入口、中英文文案、横屏标题与边缘卡片 | iOS 排序独立全屏展示单列 `List`，通过系统拖动柄上下移动，`onMove` 转为稳定 ID 后持久化。横屏标题按当前滚动位置更新，保留滚动 ID 占位，仅挂载变换后可见区及预加载邻卡；顶栏空间不足时使用图标排序入口，少于两款隐藏排序。排序操作通过触摸拖动，手柄支持返回关闭。新增 `GameReordering.swift` 为 S2Arm 自有实现 | — |
   | `Sources/ShellUI/TouchLayoutEditorView.swift` | `accessibilityElements` 的 `ForEach` 闭包（原 292 行） | Xcode 26.6 Release iOS「unable to type-check this expression in reasonable time」，带游戏库的容器壳编不过（27.0 / 27.1 编得过）。闭包体原样挪进 `accessibilityElement(_:_:k:)`，三元表达式提成带类型的局部变量，行为不变 | `908e4e848fd232304381fc19d4d847fcc10fc4c2` |
   | `Sources/ShellUI/CoverFlowView.swift`、`PortraitLibrary.swift`、`DeleteGameView.swift` | 封面 / 竖屏卡片的长按；`DeleteGameView.swift` 末尾加 `GameTileMenu` | 长按游戏卡片弹菜单：「游戏设置」+「删除游戏…」（同设置页底部的删除确认）；上游长按居中卡片只开设置、竖屏卡片没有长按（11-09 删除游戏要能长按） | — |
   | `Sources/ShellUI/ShellSettingsView.swift` | App 设置里整组「Touch controls」（大小、摇杆、自定义位置）去掉 | 上游存进自己的 ShellSettingsStore，我方游戏内虚拟手柄只读按游戏的 VPadLayouts，这组改了不生效、玩家以为存上了（#372）；按游戏的按键位置页在游戏设置里 | — |
   | `Sources/ShellUI/ShellStyle.swift` | `enum ShellStrings` 前加 `public func shellText(_:)` | 宿主层调整位置编辑页（`S2ArmShellHost/S2ArmTouchEditorView.swift`）取同一份文字表，不另起一份本地化资源（11-07 维护者 编辑页改版） | — |
   | `Sources/ShellUI/Resources/Localizable.xcstrings` | 加 5 条（en / zh-Hans）：Drag a button to move it (this game only)、Reset to default、Button opacity、Applies to all games; drag to preview here、Turn your phone sideways to adjust button positions | 同上 | — |
   | `Sources/ShellUI/Resources/Localizable.xcstrings` | #316 再加 54 条（只有 en / zh-Hans，键 = 英文原文，zh-Hans 是原中文句）：iCloud 同步页（`S2ArmShellHost/CloudSaves.swift`，含 SaveSync 三种失败原因）、名字和封面页、调整位置页、每款游戏设置行（`S2ArmGameSettings` 的标签经 `ShellStrings.setting` 查表）、清单 technical 两句。de / fr / it / ja / sv 不补：没有可核对的译文，缺了回退英文 | 英文 App 下游戏库不再出中文；我方代码一律经 `shellText(_:)` 取同一份表，键与占位符由 `tests/ios_l10n_test.sh` ⑫ 核 | — |
   | `Sources/ShellUI/ShellInputs.swift`、`ShellStyle.swift`、`GameStateView.swift`、`LibraryView.swift` | `GameState` 加 `installing(InstallStage)`（传输中 / 安装中 / 传输中断 / 安装没完成）及各处 `switch`；`CoverView` 加 `InstallMask` 灰色蒙版（`RemainingPie` / `DonePie`）；传着 / 解着时主按钮 `disabled` | v0.2.3 装机进度：App 装好就显示正在从电脑传的那款，蒙版随进度顺时针消掉，能玩了才给开始（宿主层 `S2ArmShellHostModel` 按 `s2a_shell_game_install_state` 出状态） | — |
   | `Sources/ShellUI/Resources/Localizable.xcstrings` | 装机进度加 10 条（en / zh-Hans）：Transferring – keep connected…、Installing…、Transfer stopped 等 | 同上 | — |
   门禁：`tests/shellui_ios_build_test.sh` 用门禁机上每个 Xcode 各编一遍 iOS Release（11-07 起）。

## catalogue（游戏目录清单 JSON）

字段取上游 `game.manifest.json`（schema 1）的必要子集，同名同义；不认识的键忽略：

```json
{ "schema": 1,
  "games": [ { "title_id": "0100ABCDEF012345", "name": "游戏名", "display_version": "1.0.0",
               "banner": "covers/x.jpg", "icon": "icons/x.png",
               "state": "playable", "reason": "", "detail": "" } ] }
```

- `state`：`playable`（缺省）/ `needs_game_data` / `needs_app_update` / `broken`（带 `reason` 一行原因、`detail` 一句话）；不认识的值按 `broken`。
- `banner` / `icon`：相对清单所在目录（或绝对路径）；读不到不显示图，界面画占位。
- `title_id` 非 16 位大写十六进制 → 照列出但不可用；重复的 `title_id` 只留第一条。
- 清单读不了 / 坏了 / schema 不认 → 空列表，问题只进日志（NSLog）。

## C 边界（`Sources/S2ArmShellBridge/include/s2a_shell.h`）

- `void *s2a_shell_library_vc(const char *catalogue_json_path)`：返回 +1 的 `UIHostingController`（ARC：`(__bridge_transfer UIViewController *)`），主线程调；由 app.m 挂到自己的 Scene，不另起 UIApplicationMain。
- `void s2a_shell_set_play_handler(s2a_shell_play_fn fn, void *ctx)`：登记 Play 处理函数（NULL 取消）。
- `int s2a_shell_play(const char *game_id)`：界面 Play 的出口（只放行 `playable`）；有处理函数转给它返回 1，没有只记一行日志返回 0。宿主侧由 `ios/app.m` 在多款容器模式下登记。

## 本地编译与单测

```sh
cd shell
# iOS 18 真机目标（产物：4 个 .o + S2ArmShellUI_ShellUI.bundle 资源包）
DEVELOPER_DIR=<Xcode>/Contents/Developer bash ../tools/mem_admit.sh iosbuild shellui --cores 6 -- \
  xcodebuild -scheme S2ArmShellHost -destination 'generic/platform=iOS' -configuration Release \
  -derivedDataPath <产物目录> IPHONEOS_DEPLOYMENT_TARGET=18.0 build
# 宿主层单测（本机 macOS）
DEVELOPER_DIR=<Xcode>/Contents/Developer swift test --scratch-path <产物目录>/spm
```

## 接入 make ios（11-07 P2 单款启动门已接；同日起出壳带库界面）

- 开关：`make ios S2A_SHELLUI=1`（`runtime/build.mk`）。缺省 0 = 主程序不带库界面、整个 make 不读本目录。
- 构建：`ios-shellui` 目标跑上面的 xcodebuild（产物 `$(BUILD)/ios/shellui`），`ios` 依赖它；`S2ArmShellHost.o S2ArmShellBridge.o ShellUI.o ShellControllers.o` 进主程序链接行，另加 `-L<工具链>/usr/lib/swift/iphoneos -L$(IOS_SDK)/usr/lib/swift -framework SwiftUI`。
- Swift 运行时：下限 18 时全部来自系统 `/usr/lib/swift`，不随包（`otool -L` 实测无 `@rpath`、无 LC_RPATH）。
- 资源：`S2ArmShellUI_ShellUI.bundle`（本地化 + 字体）拷到 `$(IOS_APP)/` 根（SwiftPM 的 `Bundle.module` 按主 bundle 根找）。
- app.m：弱链接 `s2a_shell_library_vc` / `s2a_shell_set_play_handler`（本地声明，不 include 本目录的头，缺省出壳不读本目录）。多款容器（App 包根有 `s2a_catalogue.json` 与 `Games/`）时场景先挂库界面；Play → `s2a_launch_ctx_select(gid)` → 起局前检查 → 换游戏界面 → 现行起宿主；同进程第二次 Play 拒绝（一进程一款）。容器包配了不带库界面的壳 = 提示页、不起游戏。
- 壳编号：出壳（`pack/shell_build.sh`）不收外部开关，按 `pack/stage_inputs/shell.tsv` 有没有「入	shell」行定 `S2A_SHELLUI`。表与表里的目录都进壳编号：开关、本目录源码一变壳编号必变，同一编号不会对应带 / 不带库界面两种壳；11-07 起壳输入表带这一行（多款容器真机门验过），本目录任何改动都改壳编号（壳与游戏无关：只要重出壳、各款重组装，不冷任何按款阶段）。`make ios` 本身缺省仍是 0（门禁 `tools/gate_ios.sh` 不编本目录）。
