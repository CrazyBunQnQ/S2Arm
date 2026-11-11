// S2Arm（不是上游文件）：Switch 2 Pro 连接状态。启动并监看启动器旁的 S2ArmNS2Connection 进程（独占蓝牙连接，游戏经本机通道读输入），
//   持久化「开启 S2Arm 时自动连接」「关闭游戏后自动断开」两个开关（docs/mac-switch2-pro.md）。
import AppKit
import Combine
import Foundation

@MainActor
final class NS2LauncherController: ObservableObject {
    enum Phase: Equatable {
        case off, starting, scanning, connecting, initializing, ready, failed, handedOff
    }

    @Published private(set) var autoConnectOnOpen: Bool
    @Published private(set) var disconnectAfterGame: Bool
    @Published private(set) var phase: Phase = .off
    @Published private(set) var detail = ""
    @Published private(set) var deviceName = "Switch 2 Pro"

    private let defaults: UserDefaults
    let socketURL: URL
    private var process: Process?
    private var output: Pipe?
    private var buffer = Data()
    private var intentionalStop = false

    init(socketURL: URL) {
        self.socketURL = socketURL
        defaults = UserDefaults(suiteName: "org.s2arm.macos.game") ?? .standard
        autoConnectOnOpen = defaults.object(forKey: "S2AMacNS2AutoConnectOnOpen") as? Bool ?? false
        disconnectAfterGame = defaults.object(forKey: "S2AMacNS2DisconnectAfterGame") as? Bool ?? false
    }

    var buttonStatus: String {
        switch phase {
        case .ready: return "已连接"
        case .starting: return "启动中"
        case .scanning: return "搜索中"
        case .connecting, .initializing: return "连接中"
        case .failed: return "未连接"
        case .handedOff: return "游戏使用中"
        case .off: return "未连接"
        }
    }

    var buttonColor: NSColor {
        switch phase {
        case .ready: return .systemGreen
        case .starting, .scanning, .connecting, .initializing: return .systemOrange
        case .failed: return .systemRed
        case .handedOff, .off: return .secondaryLabelColor
        }
    }

    var hasConnectionProcess: Bool { process != nil }

    func startIfEnabled() {
        guard autoConnectOnOpen else {
            phase = .off
            detail = "手柄未连接。需要时可在上方手动连接。"
            return
        }
        connectManually()
    }

    func setAutoConnectOnOpen(_ value: Bool) {
        autoConnectOnOpen = value
        defaults.set(value, forKey: "S2AMacNS2AutoConnectOnOpen")
    }

    func setDisconnectAfterGame(_ value: Bool) {
        disconnectAfterGame = value
        defaults.set(value, forKey: "S2AMacNS2DisconnectAfterGame")
    }

    func connectManually() {
        guard process == nil else { return }
        startProcess()
    }

    func disconnectManually() {
        stopProcess()
        phase = .off
        detail = "手柄已断开。开始游戏不受影响。"
    }

    func retry() {
        guard process != nil else { connectManually(); return }
        disconnectManually()
        connectManually()
    }

    /// 通道路径固定，开局时手柄还没连也照样交给游戏：游戏没接上时每秒重试，玩家开局后再连手柄，游戏也会接回
    ///   （以前没连接程序就不给，游戏开着时再连手柄，游戏这边一直没反应）。连接本身始终留在 S2Arm 的连接程序内。
    func relaySocketForGame() -> String? {
        socketURL.path
    }

    func gameDidStart() {
        guard process != nil else { return }
        phase = .handedOff
        detail = "游戏正在使用 Switch 2 Pro；关闭游戏后连接会保留。"
    }

    func gameDidExit() {
        guard process != nil else {
            phase = .off
            detail = "手柄未连接。需要时可在上方手动连接。"
            return
        }
        if disconnectAfterGame {
            stopProcess()
            phase = .off
            detail = "游戏已关闭，已按设置断开手柄。"
        } else if phase == .handedOff {
            phase = .ready
            detail = "\(deviceName) 仍保持连接，可以继续使用。"
        }
    }

    func shutdown() {
        stopProcess()
    }

    private func startProcess() {
        guard let helper = helperURL else {
            phase = .failed
            detail = "找不到启动器的手柄连接组件。"
            return
        }
        phase = .starting
        detail = "正在启动蓝牙连接。首次使用时，请在 macOS 提示中允许。"
        deviceName = "Switch 2 Pro"
        buffer.removeAll(keepingCapacity: true)
        intentionalStop = false

        let task = Process()
        let pipe = Pipe()
        var environment = ProcessInfo.processInfo.environment
        environment["S2A_NS2_DEFAULTS_SUITE"] = "org.s2arm.macos.game"
        environment["S2A_NS2_SOCKET"] = socketURL.path
        task.executableURL = helper
        task.environment = environment
        task.standardOutput = pipe
        task.standardError = pipe
        process = task
        output = pipe
        pipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty else { return }
            Task { @MainActor [weak self] in self?.consume(data) }
        }
        task.terminationHandler = { [weak self] finished in
            Task { @MainActor [weak self] in
                guard let self, self.process === finished else { return }
                self.process = nil
                self.output?.fileHandleForReading.readabilityHandler = nil
                self.output = nil
                if !self.intentionalStop {
                    self.phase = .failed
                    self.detail = "连接程序已停止，请手动重新连接。"
                }
            }
        }
        do {
            try task.run()
        } catch {
            process = nil
            output = nil
            phase = .failed
            detail = error.localizedDescription
        }
    }

    private func stopProcess() {
        intentionalStop = true
        output?.fileHandleForReading.readabilityHandler = nil
        output = nil
        guard let task = process else { return }
        process = nil
        task.terminationHandler = nil
        if task.isRunning { task.terminate(); task.waitUntilExit() }
    }

    private func consume(_ data: Data) {
        buffer.append(data)
        while let newline = buffer.firstIndex(of: 10) {
            let line = buffer[..<newline]
            buffer.removeSubrange(...newline)
            guard let object = try? JSONSerialization.jsonObject(with: Data(line)) as? [String: Any],
                  let code = (object["status"] as? NSNumber)?.intValue else { continue }
            apply(status: code, object)
        }
    }

    private func apply(status code: Int, _ object: [String: Any]) {
        if let name = object["name"] as? String, !name.isEmpty { deviceName = name }
        switch code {
        case 0:
            phase = .starting
            detail = "正在启动蓝牙连接。"
        case 1:
            phase = .scanning
            detail = "正在搜索手柄。请按一下手柄；未出现时长按顶部 SYNC。"
        case 2:
            phase = .connecting
            detail = "已找到手柄，正在连接。"
        case 3:
            phase = .initializing
            detail = "已连接，正在读取校准资料。"
        case 4:
            phase = .ready
            if let battery = (object["battery"] as? NSNumber)?.intValue {
                detail = "\(deviceName) 已连接，电量约 \(battery * 10)%。关闭游戏后仍会保持连接。"
            } else {
                detail = "\(deviceName) 已连接。关闭游戏后仍会保持连接。"
            }
        case 5, 9:
            phase = .failed
            detail = "手柄未连接。请唤醒手柄后按 SYNC，再点「重新连接」。"
        case 6:
            phase = .failed
            detail = "蓝牙不可用，请检查 macOS 蓝牙权限。"
        case 7:
            phase = .failed
            detail = (object["message"] as? String).flatMap { $0.isEmpty ? nil : $0 } ?? "连接失败，请重试。"
        case 8:
            if phase != .handedOff {
                phase = .failed
                detail = "连接已停止。"
            }
        default:
            break
        }
    }

    private var helperURL: URL? {
        let bundled = Bundle.main.executableURL?.deletingLastPathComponent().appendingPathComponent("S2ArmNS2Connection")
        if let bundled, FileManager.default.isExecutableFile(atPath: bundled.path) { return bundled }
#if DEBUG
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0..<6 { root.deleteLastPathComponent() }
        let built = root.appendingPathComponent("build/S2ArmNS2Connection")
        if FileManager.default.isExecutableFile(atPath: built.path) { return built }
#endif
        return nil
    }
}
