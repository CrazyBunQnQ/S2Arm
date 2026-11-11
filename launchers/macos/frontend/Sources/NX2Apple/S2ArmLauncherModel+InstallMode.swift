// S2Arm（不是上游文件）：「怎么签、怎么装」收成一套（v0.3.0，维护者 11-11 定）+ 更新 iPhone 上的 S2Arm。
//   装到 iPhone 的方式存在作业层 install.json（installMode 作业，Mac / Windows 同一份）：一键装到 iPhone（签名身份 = Xcode 账号 / 我的开发者证书）
//   或导出自己签名安装（导出作业合入前置灰）。完成页可临时换一次，不改这里。
//   更新：库页右上角按钮（catalog phoneUpdates：设备记录的壳号比本版旧）→ 有要重新出包的款先三选一 → installUpdate 作业只换壳、不重推资源。
import Foundation
import S2ArmAppleAdapter

extension S2ArmLauncherModel {
    func refreshInstallMode() {
        Task { if let i = try? await provider.installMode() { installMode = i } }
    }

    func setInstallMode(_ mode: String) {
        Task { if let i = try? await provider.installMode(set: mode) { installMode = i } }
    }

    /// 签名身份（一键装时）：开发者证书开着 = devcert，否则 Xcode 账号
    var signIdentity: String { devCert?.active == true ? "devcert" : "xcode" }

    /// 游戏库右上角按钮对应的那台（多台时取第一台：按钮点下去装的是现在连着的那台，作业里按它自己的记录再判一遍）
    var phoneUpdate: PhoneUpdate? { phoneUpdates.first }

    /// 点「更新 iPhone 上的 S2Arm」：没有要重新出包的款直接更新；有就先弹三选一
    func requestPhoneUpdate(language: String) {
        guard let u = phoneUpdate, !busy else { return }
        if u.stale.isEmpty { updatePhone(acceptStale: false, repack: [], language: language) } else { updateConfirm = u }
    }

    /// 三选一的结果：repack = 更新完要排进批量出包的款（「更新并重新出这几款」），空 = 「只更新，其他稍后」
    func updatePhone(acceptStale: Bool, repack: [String], language: String) {
        guard !busy else { return }
        updateConfirm = nil; updateRepack = repack
        start(JobSession(kind: .installUpdate, gameID: nil, name: BackendText.launcherText("update.running", language: language))) { [weak self] sink in
            _ = try await self?.provider.updatePhone(acceptStale: acceptStale, events: sink)
            return nil
        }
    }

    /// 更新作业结束：重读游戏库（按钮随检测结果消失）；选了重新出的款 → 排进批量出包（只出 iPhone），出完逐款装上
    func phoneUpdateEnded(completed: Bool) {
        let repack = completed ? updateRepack : []
        updateRepack = []
        refresh()
        guard !repack.isEmpty else { return }
        Task {
            openBatch(repack)
            for i in batchDraft.indices { batchDraft[i].iphone = batchDraft[i].problem == nil; batchDraft[i].mac = false }
            batchInstallAfter = true
            if await createBatch() != nil { batchInstallAfter = false }
        }
    }

    /// 批次出完（没有下一个了）：由更新发起的那批，出好的 iPhone 包逐款装上（暂停 / 取消的不装）
    func batchFinished() {
        guard batchInstallAfter else { return }
        batchInstallAfter = false
        guard batchPaused == nil else { return }
        installAfterQueue = batchItems.compactMap { item in
            let r = item["results"] as? [String: [String: Any]] ?? [:]
            return r["iphone"]?["state"] as? String == "completed" ? item["gameId"] as? String : nil
        }
        installNextQueued()
    }

    func installNextQueued() {
        guard !busy, !installAfterQueue.isEmpty else { return }
        let id = installAfterQueue.removeFirst()
        if let game = games.first(where: { $0.id == id }) { install(game) } else { installNextQueued() }
    }
}
