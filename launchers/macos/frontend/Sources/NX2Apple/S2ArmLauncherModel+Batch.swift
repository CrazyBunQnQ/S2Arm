// S2Arm（不是上游文件）：批量出包（v0.2.4，维护者批）。一次放进多款 → 识别完列成清单，每款勾 iPhone / Mac（可以同时勾）→
// 一批共用一个存放位置 → 一款接一款地出；同一款的两个平台连着出，解包等平台无关阶段作业层本来就共用。
// 清单、状态和规则（下一个出谁、磁盘满暂停、继续 / 重试 / 取消、汇总）都在共用后端 pack/jobs/batch.py，Windows 同一份；这里只发起作业、画状态。
import Foundation
import S2ArmAppleAdapter

/// 出包前清单里的一行：识别出来的一款游戏，勾了哪些平台；不能出的写原因（缺密钥、缺文件），勾选置灰、不进批次
struct BatchRow: Identifiable, Equatable {
    let id: String
    let name: String
    var iphone: Bool
    var mac: Bool
    var problem: String?
    var platforms: [String] { (iphone ? ["iphone"] : []) + (mac ? ["mac"] : []) }
}

extension S2ArmLauncherModel {
    static func batchPlatform(_ p: ExportPlatform) -> String? { p == .ios ? "iphone" : p == .mac ? "mac" : nil }
    static func exportPlatform(_ p: String) -> ExportPlatform? { p == "iphone" ? .ios : p == "mac" ? .mac : nil }

    /// 后端批次里的各款（show 的 batch.items）
    var batchItems: [[String: Any]] { (batchState?["batch"] as? [String: Any])?["items"] as? [[String: Any]] ?? [] }
    var batchSummary: [String: Any] { batchState?["summary"] as? [String: Any] ?? [:] }
    var batchPaused: String? { (batchState?["batch"] as? [String: Any])?["paused"] as? String }
    /// 有批次、还有没出完的（还有平台没出的、取消的；暂停着时失败的也算，等「重试」）：库页顶部提示「上次还有 N 款没出完」（审计 P1-168）
    var batchUnfinished: Int {
        guard batchState != nil else { return 0 }
        return (batchSummary["pending"] as? Int ?? 0) + (batchSummary["cancelled"] as? Int ?? 0) + (batchPaused != nil ? batchSummary["failed"] as? Int ?? 0 : 0)
    }

    /// 放进来的几款列成清单（导入完调；从游戏库多选也走这里）：勾选默认跟着出包页上次勾的平台；缺东西的款写原因
    func openBatch(_ ids: [String], unrecognized: [ImportSource] = []) {
        let picked = Set(exportTargets.compactMap(Self.batchPlatform))
        batchDraft = ids.compactMap { id in
            guard let g = games.first(where: { $0.id == id }) else { return nil }
            let problem = !g.buildable ? Self.requirementsText(g) : (g.needsKeys && provider.library.keysReady == false) ? "keys" : nil
            return BatchRow(id: g.id, name: g.name, iphone: problem == nil && picked.contains("iphone"), mac: problem == nil && picked.contains("mac"), problem: problem)
        }
        batchUnrecognized = unrecognized
        if batchLocation.isEmpty, let first = ids.compactMap({ id in games.first { $0.id == id } }).first { batchLocation = defaultLocation(for: first) }
        exportFor = nil
        batchShown = true
    }

    /// 全选某个平台：没问题的款全勾上；已全勾就全取消
    func batchToggleAll(_ platform: String) {
        let ok = batchDraft.indices.filter { batchDraft[$0].problem == nil }
        let all = ok.allSatisfy { platform == "iphone" ? batchDraft[$0].iphone : batchDraft[$0].mac }
        for i in ok { if platform == "iphone" { batchDraft[i].iphone = !all } else { batchDraft[i].mac = !all } }
    }

    /// 点「出包（N 款）」：后端建批次，开始出第一款
    func startBatch() { Task { _ = await createBatch() } }

    /// 建批次并开始出第一款 → nil；没建成 → 原因（控制口 POST /batch 据此回 409，不先回成功，审计 P1-166）
    func createBatch() async -> String? {
        let items = batchDraft.filter { $0.problem == nil && !$0.platforms.isEmpty }.map { ["gameId": $0.id, "name": $0.name, "platforms": $0.platforms] as [String: Any] }
        guard !items.isEmpty else { return "没有能出的游戏" }
        guard !busy else { return "正在出包" }
        guard let data = try? JSONSerialization.data(withJSONObject: ["items": items, "location": batchLocation.isEmpty ? NSNull() : batchLocation] as [String: Any]),
              let json = String(data: data, encoding: .utf8) else { return "批次清单组不出来" }
        guard let state = await provider.client.batch("create", ["--json", json]) else { return "批次脚本起不来" }
        guard state["batch"] != nil else { return state["error"] as? String ?? "建批次失败" }
        batchState = state
        batchDraft = []
        batchRunning = true
        advanceBatch()
        return nil
    }

    /// 出下一款：后端说出谁就出谁（同一款还没出的平台一起连着出）；没有下一个 = 出完 / 暂停 / 取消了
    func advanceBatch() {
        guard batchRunning, !busy else { return }
        guard let next = batchState?["next"] as? [String: Any], let gid = next["gameId"] as? String,
              let item = batchItems.first(where: { $0["gameId"] as? String == gid }) else { batchRunning = false; batchFinished(); return }
        let results = item["results"] as? [String: [String: Any]] ?? [:]
        let platforms = (item["platforms"] as? [String] ?? []).filter { results[$0]?["state"] as? String == "pending" }.compactMap(Self.exportPlatform)
        guard let game = games.first(where: { $0.id == gid }), !platforms.isEmpty else {
            // 游戏库里已经没有这款（玩家删了）：记失败、接着下一款
            Task {
                for p in platforms.compactMap(Self.batchPlatform) { await batchMark(gid, p, state: "failed", message: "游戏库里已经没有这款游戏") }
                advanceBatch()
            }
            return
        }
        sessionHidden = true   // 批量出包时留在清单页看整批进度（单款运行页可从顶部胶囊打开）
        runExport(QueuedExport(game: game, platforms: platforms, target: currentDeviceTarget, spaceOverride: false, location: batchLocation.isEmpty ? nil : batchLocation))
    }

    /// 出包流程里：一个平台开始出 / 出完（不在批次里的款直接跳过）
    func batchMark(_ gid: String, _ platform: ExportPlatform, state: String, message: String = "", code: String? = nil, output: String? = nil) async {
        guard batchRunning, let p = Self.batchPlatform(platform) else { return }
        await batchMark(gid, p, state: state, message: message, code: code, output: output)
    }

    private func batchMark(_ gid: String, _ p: String, state: String, message: String = "", code: String? = nil, output: String? = nil) async {
        var args = ["--game", gid, "--platform", p, "--state", state]
        if !message.isEmpty { args += ["--message", message] }
        if let code { args += ["--code", code] }
        if let output { args += ["--output", output] }
        if let s = await provider.client.batch("mark", args), s["batch"] != nil { batchState = s }
    }

    func batchRecord(_ gid: String, _ platform: ExportPlatform) async {
        guard batchRunning, let outcome = session?.export?.outcomes[platform], outcome.state != .cancelled else { return }
        await batchMark(gid, platform, state: outcome.state == .completed ? "completed" : "failed",
                        message: outcome.message, code: outcome.code, output: outcome.ipa ?? outcome.output)
    }

    /// 一款出完（出包会话结束）：取消 = 整批停（已出好的保留）；否则接着出下一款
    func batchSessionEnded(cancelled: Bool) {
        Task {
            if cancelled {
                if let s = await provider.client.batch("cancel"), s["batch"] != nil { batchState = s }
                batchRunning = false
                return
            }
            advanceBatch()   // 上一款的会话已结束（busy = false），runExport 换上新会话
        }
    }

    /// 继续出剩下的（取消后 / 关掉启动器再打开后）；重试失败的（给游戏号只重试这一款，磁盘满那款释放空间后也走这里）
    func resumeBatch() { batchCommand("resume") }
    func retryBatch(_ gid: String? = nil) { batchCommand("retry", gid.map { ["--game", $0] } ?? []) }

    private func batchCommand(_ command: String, _ args: [String] = []) {
        guard !busy else { return }
        Task {
            guard let s = await provider.client.batch(command, args), s["batch"] != nil else { return }
            batchState = s
            batchRunning = true
            batchShown = true
            advanceBatch()
        }
    }

    /// 这批的结果看完了：清掉批次（不影响已出好的包）
    func closeBatch() {
        guard !batchRunning else { batchShown = false; return }
        batchShown = false; batchDraft = []; batchUnrecognized = []
        Task { _ = await provider.client.batch("clear"); batchState = nil }
    }

    /// 启动器刚打开：上次没出完的批次读回来（正在出的那款记成被打断、重新排队），出包页顶部提示继续
    func recoverBatch() {
        Task {
            guard let s = await provider.client.batch("recover"), s["batch"] != nil else { return }
            batchState = s
        }
    }
}
