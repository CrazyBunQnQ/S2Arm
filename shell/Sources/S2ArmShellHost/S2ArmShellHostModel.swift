import Foundation
import Observation
import S2ArmShellBridge
import ShellControllers
import ShellUI

/// 我方宿主层的最小实现：清单 → 库界面条目；手柄 / 键盘语义输入（上游 ShellControllers 原样）；Play → C 出口 s2a_shell_play。
/// 只借上游 ShellHostModel 的设计（@Observable 模型 + actions + startControllers），不带它的 EngineLaunchPlan / DM_* / framework 扫描。
/// 设置行：每款游戏的「画面」「虚拟手柄」两组（S2ArmGameSettings，11-07 维护者）；数据目录、云存档等动作本刀不接（ShellActions 缺省空操作）。
/// 删除游戏（#227）：删本机这款的游戏数据（Documents/games/<游戏号>），勾了连存档才删存档；删完重判，库里随即不再显示（App 里的程序随下次从电脑装机摘掉）。
@MainActor @Observable
public final class S2ArmShellHostModel {
    /// 清单里的全部款（容器里的）
    private var allGames: [GameDetail]
    /// 这台设备上资源没到位的款：不在库里显示
    private var notOnDevice: Set<String> = []
    /// 库里显示的款：只有这台设备上真能玩的（清单列着、资源已到位）
    public var games: [GameDetail] { allGames.filter { !notOnDevice.contains($0.id) } }
    public private(set) var controllers: [ControllerInfo] = []
    public private(set) var input = ShellInputLog()
    public private(set) var padSnapshot: PadSnapshot?
    @ObservationIgnored private let adapter = GamepadAdapter()
    @ObservationIgnored private let play: (String) -> Void
    /// 本款装机阶段（宿主按 Documents/games/<游戏号> 判，见 s2a_shell.h）：1 就绪 / 2 传完待解开 / 3 传输中 / 0 没有（不显示）
    @ObservationIgnored private let installState: (String) -> Int32
    /// 在后台解开阶段 2 的那款（同时只解一款；正在解别的返回 false）；正在解的这款的进度（已解开包数, 总包数）
    @ObservationIgnored private let installGame: (String) -> Bool
    @ObservationIgnored private let installProgress: (String) -> (Int, Int)?
    @ObservationIgnored private let now: () -> Date
    /// 这次打开 App 已经起过解包的款：解完仍是阶段 2 = 没解成，显示「安装没完成」，点重试才再起（不自动反复解）
    @ObservationIgnored private var installTried: Set<String> = []
    /// 库开着就每秒重判一次（只判各款自己目录下的几个小文件，每款几次 stat / 读几十字节）：电脑随时可能开始推——
    ///   没显示的款一开始传就出现（真机实测：App 先被拉起、推包随后才写传输清单），已经能玩的款重推 / 装更新时也立刻画上蒙版
    ///   （#399 真机验收：原先「全都能玩了就停」，开着库重推一款已装好的游戏，整个推送过程库里一直显示可玩、没有进度）。App 退到后台时计时器随之暂停
    @ObservationIgnored private var ticker: Timer?
    var watching: Bool { ticker != nil }
    /// 传输清单和它列的文件这么久都没动 = 传断了（拔线、电脑那边停了）。90 秒：电脑经 devicectl 写 ≤1 GB 的一批要写完才落盘，实测约 33 秒，留余量
    static let transferStall: TimeInterval = 90
    @ObservationIgnored private let pickGameData: (String) -> Void
    /// 这台设备上每款数据的根（宿主同一取法：launch.ctx 的 docs = @home/Documents/games/<游戏号>）；单测可换
    @ObservationIgnored private let gamesRoot: URL
    /// 清单里写的状态（重判资源包时以它为底，不叠加）
    @ObservationIgnored private let listedStates: [String: GameState]
    @ObservationIgnored private let stores: S2ArmGameSettings.Stores
    @ObservationIgnored private let layoutState: (String) -> S2ArmGameSettings.LayoutState
    /// 正在调整虚拟手柄布局的那款（非空时宿主视图盖上编辑页）
    public var editingLayout: String?
    /// 正在改名字和封面的那款（非空时宿主视图盖上编辑页）
    public var editingIdentity: String?
    /// 编辑页上次点保存没存成的原因（存成 / 关页清掉）
    public private(set) var identityError: String?
    @ObservationIgnored private let identityStore: GameIdentityStore
    /// 清单里写的条目：恢复默认时回到它
    @ObservationIgnored private var listedTiles: [String: GameTile] = [:]
    @ObservationIgnored private var listedBanners: [String: URL] = [:]
    /// 编辑页底图：游戏号 → 封面文件
    public private(set) var bannerURLs: [String: URL]

    /// play 缺省走 C 出口；单测可换
    public init(catalogue: Catalogue, play: ((String) -> Void)? = nil, stores: S2ArmGameSettings.Stores = .live,
                layoutState: @escaping (String) -> S2ArmGameSettings.LayoutState = S2ArmGameSettings.layoutState,
                installState: ((String) -> Int32)? = nil, installGame: ((String) -> Bool)? = nil, installProgress: ((String) -> (Int, Int)?)? = nil,
                now: @escaping () -> Date = Date.init, pickGameData: ((String) -> Void)? = nil, identityStore: GameIdentityStore = .live,
                gamesRoot: URL = URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent("Documents/games")) {
        self.gamesRoot = gamesRoot
        self.stores = stores; self.layoutState = layoutState; bannerURLs = catalogue.bannerURLs
        self.identityStore = identityStore
        listedTiles = Dictionary(catalogue.tiles.map { ($0.id, $0) }, uniquingKeysWith: { a, _ in a })
        listedBanners = catalogue.bannerURLs
        let strays = Self.otherComputerGames(gamesRoot, listed: Set(catalogue.tiles.map(\.id)))
        allGames = catalogue.tiles.map { tile in
            var state = layoutState(tile.id); state.customIdentity = identityStore.load(tile.id) != nil
            return GameDetail(game: tile, sections: S2ArmGameSettings.sections(for: tile.id, stores: stores, state: state))
        } + strays.map { GameDetail(game: $0) }
        listedStates = Dictionary((catalogue.tiles + strays).map { ($0.id, $0.state) }, uniquingKeysWith: { a, _ in a })
        self.play = play ?? { id in _ = id.withCString { s2a_shell_play($0) } }
        self.installState = installState ?? { id in id.withCString { s2a_shell_game_install_state($0) } }
        self.installGame = installGame ?? { id in id.withCString { s2a_shell_install_game($0) } != 0 }
        self.installProgress = installProgress ?? { id in
            var done: Int32 = 0, total: Int32 = 0
            return id.withCString { s2a_shell_install_progress($0, &done, &total) } != 0 ? (Int(done), Int(total)) : nil
        }
        self.now = now
        self.pickGameData = pickGameData ?? { id in id.withCString { s2a_shell_pick_game_data($0) } }
        let savedOrder = stores.app.stringArray(forKey: "S2ALibraryOrder") ?? []
        var remaining = allGames
        allGames = savedOrder.compactMap { id in
            guard let index = remaining.firstIndex(where: { $0.id == id }) else { return nil }
            return remaining.remove(at: index)
        } + remaining
        for tile in catalogue.tiles { applyIdentity(tile.id) }
        for p in catalogue.problems { NSLog("S2Arm 游戏库：%@", p) }
        refreshGameData()
        // 测试开关（真机自动化要 XCTest 触控 ID 授权、人不在时点不进去；同 diag.m S2A_OPEN_SETTINGS，环境变量只有 devicectl / Xcode 起 App 能给，玩家碰不到）：
        //   S2A_SHELL_OPEN_LAYOUT_EDITOR=<游戏号> 起来直接盖上这款的「调整位置」编辑页
        //   S2A_SHELL_PLAY=<游戏号> 起来 2 秒后替人点这款的 Play（走同一条 requestPlay → C 出口；截图局别带 S2A_AUTOPLAY=1：没有视觉驾驶员时自动玩家判卡住、优雅退出）
        let env = ProcessInfo.processInfo.environment
        if let id = env["S2A_SHELL_OPEN_LAYOUT_EDITOR"], allGames.contains(where: { $0.id == id }) {
            editingLayout = id; NSLog("S2Arm 游戏库：S2A_SHELL_OPEN_LAYOUT_EDITOR 直接开 %@ 的调整位置页", id)
        }
        if let id = env["S2A_SHELL_PLAY"], allGames.contains(where: { $0.id == id }) {
            NSLog("S2Arm 游戏库：S2A_SHELL_PLAY 2 秒后开始 %@", id)
            DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in self?.requestPlay(id) }
        }
    }

    /// 手机上有、容器清单里没有的款：同一台手机在另一台电脑上装过，这台电脑重组容器时带不上（App 里的游戏程序读不回电脑）。
    /// 存档和资源包都还在，显示成不可玩并告诉玩家回原来那台电脑重装。只认目录名是 16 位游戏号、资源包已解开（资源包/.applied）的，别的跳过；
    /// 名字和图标取这款资源包自带的 ctl/control.nacp 与 icon_*.dat，读不到用游戏号、占位图
    static func otherComputerGames(_ root: URL, listed: Set<String>) -> [GameTile] {
        let fm = FileManager.default
        return ((try? fm.contentsOfDirectory(atPath: root.path)) ?? []).sorted().compactMap { id in
            let pack = root.appendingPathComponent(id).appendingPathComponent("资源包")
            guard isTitleID(id), !listed.contains(id), fm.fileExists(atPath: pack.appendingPathComponent(".applied").path) else { return nil }
            let ctl = pack.appendingPathComponent("ctl")
            let icon = ["SimplifiedChinese", "AmericanEnglish"].lazy.compactMap { try? Data(contentsOf: ctl.appendingPathComponent("icon_\($0).dat")) }.first
            NSLog("S2Arm 游戏库：%@ 在机上、不在这个 App 的清单里（别的电脑装的），显示需要重装", id)
            return GameTile(id: id, title: nacpTitle(try? Data(contentsOf: ctl.appendingPathComponent("control.nacp"))) ?? id,
                            icon: icon.flatMap { Artwork(id: id + ".icon", data: $0) },
                            state: .broken(reason: shellText("Reinstall from the original computer"),
                                           detail: shellText("This game was installed from another computer. Its saves and game data are still on this iPhone; on that computer, choose “Install to iPhone” again to bring it back.")))
        }
    }

    /// NACP 标题：16 种语言各 0x300 字节（名字占前 0x200）；中文界面先取简体中文（14）再英文（0），否则反过来
    static func nacpTitle(_ data: Data?) -> String? {
        guard let data, data.count >= 16 * 0x300 else { return nil }
        let order = Locale.preferredLanguages.first?.hasPrefix("zh") == true ? [14, 0] : [0, 14]
        for i in order {
            let start = data.startIndex + i * 0x300
            if let t = String(data: data[start ..< start + 0x200].prefix { $0 != 0 }, encoding: .utf8), !t.isEmpty { return t }
        }
        return nil
    }

    /// 重判各款装机阶段（建好、回前台、解完、宿主说明里「重新检查」后，库开着时每秒一次）：
    ///   就绪 → 清单写的状态；传着 → 按传输清单画进度（久不动 → 传输中断）；传完 → 后台解开、画解包进度（没解成 → 安装没完成）；没有 → 不显示
    public func refreshGameData() {
        var missing = Set<String>()
        defer {
            if missing != notOnDevice { notOnDevice = missing; NSLog("S2Arm 游戏库：这台设备上 %d / %d 款可显示", allGames.count - missing.count, allGames.count) }
            startTicker()
        }
        for i in allGames.indices {
            let id = allGames[i].id, listed = listedStates[id] ?? allGames[i].game.state
            let want: GameState
            switch installState(id) {
            case 1: want = listed; installTried.remove(id)
            case 2: want = .installing(unpackStage(id))
            case 3: want = .installing(transferStage(id))   // 断了也接着看：电脑重新插上续传时字节又涨
            default: missing.insert(id); want = listed == .playable ? .needsGameData : listed   // 不显示；Play 点到也只给导入说明，不放行
            }
            let fresh = want == .playable && isFresh(id)
            if allGames[i].game.isNew != fresh { allGames[i].game.isNew = fresh }
            if allGames[i].game.state != want {
                if want.installPhase != allGames[i].game.state.installPhase { NSLog("S2Arm 游戏库：%@ → %@", id, String(describing: want)) }
                allGames[i].game.state = want
            }
        }
    }

    /// 传完了：没起过就在后台解开（正在解别的款 → 排着，下一秒再试）；起过、宿主说已不在解、还是阶段 2 → 没解成
    private func unpackStage(_ id: String) -> InstallStage {
        if let (done, total) = installProgress(id) { return InstallStage(.installing, fraction: total > 0 ? Double(done) / Double(total) : nil) }
        if installTried.contains(id) { return InstallStage(.failed) }
        guard installGame(id) else { return InstallStage(.installing) }
        installTried.insert(id)
        return InstallStage(.installing, fraction: 0)
    }

    /// 传着：电脑开推时写的 资源包/.transfer 列了这次要传的文件和字节数，各文件已到多少字节（正在写的也算）÷ 总字节 = 进度；
    ///   传输清单和这些文件里最新的改动时刻离现在超过 transferStall → 传断了。只读这款自己的这几个文件
    private func transferStage(_ id: String) -> InstallStage {
        let pack = gamesRoot.appendingPathComponent(id).appendingPathComponent("资源包"), list = pack.appendingPathComponent(".transfer")
        let fm = FileManager.default
        func info(_ url: URL) -> (Int64, Date)? {
            guard let a = try? fm.attributesOfItem(atPath: url.path), let size = a[.size] as? NSNumber, let at = a[.modificationDate] as? Date else { return nil }
            return (size.int64Value, at)
        }
        guard let data = try? Data(contentsOf: list), let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let total = (json["total"] as? NSNumber)?.int64Value, total > 0, let files = json["files"] as? [[Any]], let listed = info(list) else {
            return InstallStage(.transferring)
        }
        var got: Int64 = 0, latest = listed.1
        for f in files {
            guard f.count == 2, let rel = f[0] as? String, let want = (f[1] as? NSNumber)?.int64Value, !rel.hasPrefix("/"), !rel.contains(".."),
                  let (size, at) = info(pack.appendingPathComponent(rel)) else { continue }
            got += min(size, want); latest = max(latest, at)
        }
        // base = 这款已在手机上的字节（续传 / 重装只传剩下的）：进度按整款算，从断点接着涨、不往回掉
        let base = max(0, (json["base"] as? NSNumber)?.int64Value ?? 0), fraction = Double(base + got) / Double(base + total)
        if got < total, now().timeIntervalSince(latest) > Self.transferStall { return InstallStage(.interrupted, fraction: fraction) }
        return InstallStage(.transferring, fraction: fraction)
    }

    /// 装好（或更新）后还没点开过：资源包解开的时刻（资源包/.applied）比这款最后一次点开始（.played，requestPlay 写）新。
    ///   没有 .played、但有存档目录 S2A/（这版之前就玩过）→ 算玩过，不因升级让每款都亮蓝点
    private func isFresh(_ id: String) -> Bool {
        let dir = gamesRoot.appendingPathComponent(id), fm = FileManager.default
        func mtime(_ url: URL) -> Date? { (try? fm.attributesOfItem(atPath: url.path))?[.modificationDate] as? Date }
        guard let applied = mtime(dir.appendingPathComponent("资源包/.applied")) else { return false }
        if let played = mtime(dir.appendingPathComponent(Self.playedMark)) { return applied > played }
        return !fm.fileExists(atPath: dir.appendingPathComponent(Self.saveRoot).path)
    }
    static let playedMark = ".played"

    private func startTicker() {
        guard ticker == nil else { return }
        ticker = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in MainActor.assumeIsolated { self?.refreshGameData() } }
    }

    public var actions: ShellActions {
        var actions = ShellActions(onPlay: { [weak self] id in self?.requestPlay(id) },
                     onChange: { [weak self] id, setting, value in self?.change(id, setting, value) },
                     onPickGameData: { [weak self] id in self?.requestGameData(id) },
                     onPickShellCloud: Self.pickCloudFolder, onClearShellCloud: Self.pickCloudFolder == nil ? nil : { CloudSaves.forget() },
                     onButtonTest: { [weak self] open in self?.buttonTest(open) },
                     onDeleteGame: { [weak self] id, saves in self?.deleteGame(id, saves: saves) })
        actions.onReorderGames = { [weak self] sources, before in self?.reorderGames(sources, before: before) ?? false }
        return actions
    }

    /// Native reordering supplies stable IDs and an insertion point after the drag finishes.
    private func reorderGames(_ sources: [String], before destination: String?) -> Bool {
        let moving = Set(sources)
        let visible = Set(games.map(\.id))
        guard !sources.isEmpty, moving.count == sources.count, moving.isSubset(of: visible) else { return false }
        if let destination {
            guard visible.contains(destination), !moving.contains(destination) else { return false }
        }
        let entries = Dictionary(uniqueKeysWithValues: allGames.map { ($0.id, $0) })
        var reordered = allGames.filter { !moving.contains($0.id) }
        let insertion = destination.flatMap { id in reordered.firstIndex { $0.id == id } } ?? reordered.count
        reordered.insert(contentsOf: sources.compactMap { entries[$0] }, at: insertion)
        allGames = reordered
        stores.app.set(allGames.map(\.id), forKey: "S2ALibraryOrder")
        return true
    }

    /// 库设置「云存档 → 选择文件夹…」（iCloud 存档同步，见 CloudSaves.swift）：iOS 弹系统文件夹选择器；Mac 上跑单测时没有界面
    static var pickCloudFolder: (() -> Void)? {
        #if canImport(UIKit)
        return { CloudFolderPicker.present() }
        #else
        return nil
        #endif
    }

    /// 只放行可运行的那款（界面本身也只在 playable 时调 onPlay，这里再守一道）；放行前当场重判资源包（库开着时资源被删 / 换机拷走）
    public func requestPlay(_ id: String) {
        if allGames.first(where: { $0.id == id })?.game.state.installPhase == .failed { installTried.remove(id) }   // 「安装没完成」点重试：再解一次
        refreshGameData()
        if allGames.first(where: { $0.id == id })?.game.state == .needsGameData { requestGameData(id); return }
        guard let g = allGames.first(where: { $0.id == id }), g.game.state == .playable else {
            NSLog("S2Arm 游戏库：Play %@ 被拒（不在列表或不可运行）", id)
            return
        }
        try? Data().write(to: gamesRoot.appendingPathComponent(id).appendingPathComponent(Self.playedMark))   // 点开过：蓝点消掉
        if let i = allGames.firstIndex(where: { $0.id == id }) { allGames[i].game.isNew = false }
        play(id)
    }

    /// 存档在这款数据根下的 S2A/（Ryujinx 数据根 <文档根>/S2A/ryujinx，存档 bis/user/save，同 pack/jobs/install.py SAVE_REL）
    static let saveRoot = "S2A"

    /// 删除这款的本机数据：saves = 连存档一起删（整个数据根），否则留下 S2A/（存档与用户资料）、其余（资源包、名字封面等）删掉。删完重判资源包，库里随即不显示
    public func deleteGame(_ id: String, saves: Bool) {
        guard allGames.contains(where: { $0.id == id }), id.allSatisfy({ $0.isHexDigit }) else { return }
        let fm = FileManager.default, dir = gamesRoot.appendingPathComponent(id)
        do {
            if saves {
                if fm.fileExists(atPath: dir.path) { try fm.removeItem(at: dir) }
            } else {
                for item in (try? fm.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? [] where item.lastPathComponent != Self.saveRoot {
                    try fm.removeItem(at: item)
                }
            }
            NSLog("S2Arm 游戏库：已删除 %@ 的本机数据%@", id, saves ? "（含存档）" : "（存档保留）")
        } catch {
            NSLog("S2Arm 游戏库：删除 %@ 失败：%@", id, error.localizedDescription)
        }
        refreshGameData()
    }

    /// 「需要游戏数据」那款：交宿主给导入说明（宿主「重新检查」后经 s2a_shell_refresh 回到 refreshGameData）
    public func requestGameData(_ id: String) { pickGameData(id) }

    /// 设置行改了：存下、刷新这一款的设置行；选「调整位置…」打开编辑页
    public func change(_ id: String, _ setting: String, _ value: String) {
        if setting == S2ArmGameSettings.identity {
            if value == S2ArmGameSettings.editIdentity { editingIdentity = id; identityError = nil }
            refresh(id)
            return
        }
        if S2ArmGameSettings.apply(id: id, setting: setting, value: value, stores: stores) { editingLayout = id }
        refresh(id)
    }
    /// 编辑页关了（存了 / 取消 / 恢复默认）：布局行跟着刷新
    public func closeLayoutEditor() { if let id = editingLayout { editingLayout = nil; refresh(id) } }
    func refresh(_ id: String) {
        guard let i = allGames.firstIndex(where: { $0.id == id }) else { return }
        var state = layoutState(id); state.customIdentity = identityStore.load(id) != nil
        allGames[i].sections = S2ArmGameSettings.sections(for: id, stores: stores, state: state)
    }

    /// 改名字和封面的编辑页要用：当前显示的名字，以及有没有自定义
    public func identityName(_ id: String) -> String { allGames.first(where: { $0.id == id })?.game.title ?? "" }
    public func hasCustomIdentity(_ id: String) -> Bool { identityStore.load(id) != nil }

    /// 名字和封面一次提交（GameIdentityStore.save）。存成了关编辑页；没存成不关页、留着玩家填的，identityError 给编辑页显示原因
    public func saveIdentity(_ id: String, name: String, cover: Data?) {
        // 名字没动时不存成自定义，否则电脑端以后改名就传不到这台手机
        let keep = name == listedTiles[id]?.title ? "" : name
        do { try identityStore.save(id, name: keep, cover: cover) }
        catch {
            NSLog("S2Arm 游戏库：保存 %@ 的名字和封面失败：%@", id, error.localizedDescription)
            identityError = error.localizedDescription
            return
        }
        closeIdentityEditor(id)
    }
    public func resetIdentity(_ id: String) { identityStore.clear(id); closeIdentityEditor(id) }
    public func closeIdentityEditor(_ id: String) { editingIdentity = nil; identityError = nil; applyIdentity(id); refresh(id) }

    /// 把这台手机上的名字和封面叠到清单条目上；没有就回到清单原样，状态保持不变。
    private func applyIdentity(_ id: String) {
        guard let i = allGames.firstIndex(where: { $0.id == id }), var tile = listedTiles[id] else { return }
        tile.state = allGames[i].game.state
        bannerURLs[id] = listedBanners[id]
        if let custom = identityStore.load(id) {
            if !custom.name.isEmpty { tile.title = custom.name }
            if let url = custom.cover, let data = try? Data(contentsOf: url), let art = Artwork(id: id + ".custom.\(data.hashValue)", data: data) {
                tile.banner = art; tile.icon = art; bannerURLs[id] = url
            }
        }
        allGames[i].game = tile
    }

    public func startControllers() {
        adapter.onInput = { [weak self] action, device in self?.input.append(action, device: device) }
        adapter.onRelease = { [weak self] action, device in self?.input.append(action, device: device, released: true) }
        adapter.onControllersChange = { [weak self] connected in
            self?.controllers = connected.map {
                ControllerInfo(id: $0.id, name: $0.name, player: "P\($0.player)", batteryPercent: $0.batteryPercent, charging: $0.charging)
            }
        }
        adapter.start()
    }

    public func stopControllers() { adapter.stop() }

    func buttonTest(_ open: Bool) {
        adapter.onSnapshot = open ? { [weak self] in self?.padSnapshot = $0 } : nil
        if !open { padSnapshot = nil }
    }
}
