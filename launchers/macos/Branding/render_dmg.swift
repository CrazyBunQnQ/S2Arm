// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Original artwork. Palette and wordmark follow docs/site/index.html.
// Finder supplies the draggable icons; they appear only in the review preview.
import AppKit

// 所有输入错误都用中文报出可照做的原因并 exit(2)，不留下 SIGTRAP / SIGABRT 崩溃现场。
func fail(_ message: String) -> Never {
    let line = "【红】" + message + "\n"
    if let data = line.data(using: .utf8) {
        FileHandle.standardError.write(data)
    }
    exit(2)
}

let assets = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
let root = assets.deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()

struct Layout {
    let width: Int
    let height: Int
    let iconSize: Int
    let positions: [String: (x: Int, y: Int)]
}

// 与 build_dmg.py 的 LAYOUT_LIMITS 保持一致；Python 入口已先校验，这里是第二道防线。
// 不用 as!：JSON 值先按 NSNumber 安全取整，字符串/数组/布尔都走中文报错。
func jsonNumber(_ value: Any, label: String) -> Int {
    if let boolean = value as? Bool {
        fail("\(label) 必须是整数，当前是布尔值 \(boolean)")
    }
    guard let number = value as? NSNumber else {
        fail("\(label) 必须是整数（不带引号），当前是 \(value)")
    }
    let integer = number.intValue
    guard number.doubleValue == Double(integer) else {
        fail("\(label) 必须是整数，当前是小数 \(number.doubleValue)")
    }
    return integer
}

func jsonInteger(_ value: Any?, label: String, range: ClosedRange<Int>) -> Int {
    guard let value else {
        fail("布局字段 \(label) 缺失；需要整数，允许范围 \(range.lowerBound)–\(range.upperBound)")
    }
    let integer = jsonNumber(value, label: "布局字段 \(label)")
    guard range.contains(integer) else {
        fail("布局字段 \(label)=\(integer) 超出允许范围 \(range.lowerBound)–\(range.upperBound)")
    }
    return integer
}

func loadLayout() -> Layout {
    let path = assets.appendingPathComponent("dmg-layout.json")
    guard let data = try? Data(contentsOf: path) else {
        fail("找不到或读不了布局文件：\(path.path)；请确认 Branding/dmg-layout.json 存在且可读")
    }
    var parsed: Any?
    var parseError: Error?
    do {
        parsed = try JSONSerialization.jsonObject(with: data)
    } catch {
        parseError = error
    }
    guard let object = parsed else {
        fail("布局文件不是合法 JSON：\(path.path)：\(parseError?.localizedDescription ?? "未知错误")；可对照仓库中的 Branding/dmg-layout.json 修正")
    }
    guard let layout = object as? [String: Any] else {
        fail("布局文件顶层必须是 JSON 对象 {…}：\(path.path)")
    }
    let allowed = Set(["width", "height", "iconSize", "iconPositions"])
    let keys = Set(layout.keys)
    let missing = allowed.subtracting(keys).sorted()
    if !missing.isEmpty {
        fail("布局文件缺少键：\(missing.joined(separator: "、"))；需要 width、height、iconSize、iconPositions")
    }
    let unknown = keys.subtracting(allowed).sorted()
    if !unknown.isEmpty {
        fail("布局文件有多余的键：\(unknown.joined(separator: "、"))；只允许 width、height、iconSize、iconPositions")
    }
    let width = jsonInteger(layout["width"], label: "width", range: 480...1600)
    let height = jsonInteger(layout["height"], label: "height", range: 360...1200)
    let iconSize = jsonInteger(layout["iconSize"], label: "iconSize", range: 48...256)

    guard let rawPositions = layout["iconPositions"] as? [String: Any] else {
        fail("布局字段 iconPositions 必须是 JSON 对象 {…}，当前是 \(String(describing: layout["iconPositions"]))")
    }
    for name in ["S2Arm.app", "Applications"] where !rawPositions.keys.contains(name) {
        fail("布局字段 iconPositions 缺少图标条目：\(name)；必须同时包含 S2Arm.app 与 Applications")
    }
    let half = Double(iconSize) / 2
    var positions: [String: (x: Int, y: Int)] = [:]
    for (name, value) in rawPositions {
        guard let pair = value as? [Any], pair.count == 2 else {
            fail("图标坐标 \(name) 必须是 [x, y] 两个整数，当前是 \(value)；例如 [190, 280]")
        }
        let x = jsonNumber(pair[0], label: "iconPositions.\(name)[0]")
        let y = jsonNumber(pair[1], label: "iconPositions.\(name)[1]")
        guard Double(x) >= half, Double(x) <= Double(width) - half,
              Double(y) >= half, Double(y) <= Double(height) - half else {
            let margin = Int((half).rounded(.up))
            fail("图标坐标 \(name)=[\(x), \(y)] 超出画布：iconSize=\(iconSize) 时中心需在 x \(margin)–\(width - margin)、y \(margin)–\(height - margin) 内，否则图标会被画布裁掉")
        }
        positions[name] = (x, y)
    }
    return Layout(width: width, height: height, iconSize: iconSize, positions: positions)
}

guard CommandLine.arguments.count >= 2, !CommandLine.arguments[1].isEmpty else {
    fail("用法：render_dmg.swift <输出目录> [S2Arm.app]；通常由 launchers/macos/build_dmg.py 调用")
}
let layout = loadLayout()
let width = layout.width
let height = layout.height
let positions = layout.positions
let iconSize = CGFloat(layout.iconSize)
let output = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
let app = CommandLine.arguments.count > 2 ? CommandLine.arguments[2] : ""
if !app.isEmpty && !FileManager.default.fileExists(atPath: app) {
    fail("预览用应用包不存在：\(app)；请传入有效的 S2Arm.app 路径，或省略该参数")
}
do {
    try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
} catch {
    fail("创建输出目录失败：\(output.path)：\(error.localizedDescription)；请检查目录权限")
}
let canvas = NSSize(width: width, height: height)

func color(_ hex: UInt32, _ alpha: CGFloat = 1) -> NSColor {
    NSColor(srgbRed: CGFloat((hex >> 16) & 255) / 255,
            green: CGFloat((hex >> 8) & 255) / 255,
            blue: CGFloat(hex & 255) / 255, alpha: alpha)
}
let ink = color(0xf2f0eb), muted = color(0xa3a09a), cream = color(0xf6ead2)
let line = color(0x2a2a2d), cyan = color(0x2cc8f0), red = color(0xe5484d)

func render(scale: Int, preview: Bool = false) -> NSBitmapImageRep {
    guard let colorSpace = CGColorSpace(name: CGColorSpace.sRGB) else {
        fail("无法创建 sRGB 色彩空间；请检查系统图形环境")
    }
    guard let cg = CGContext(data: nil, width: width * scale, height: height * scale,
                             bitsPerComponent: 8, bytesPerRow: width * scale * 4,
                             space: colorSpace,
                             bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else {
        fail("无法创建 \(width * scale)×\(height * scale) 绘图上下文；可能是内存不足，请关掉其它大程序再试")
    }
    cg.translateBy(x: 0, y: CGFloat(height * scale))
    cg.scaleBy(x: CGFloat(scale), y: -CGFloat(scale))
    NSGraphicsContext.saveGraphicsState()
    defer { NSGraphicsContext.restoreGraphicsState() }
    // NSGraphicsContext(cgContext:flipped:) 在 SDK 里是非空工厂方法；赋值后检查一次，
    // 兼容它被导入为可空初始化器的情况，也不使用强制解包。
    NSGraphicsContext.current = NSGraphicsContext(cgContext: cg, flipped: true)
    guard NSGraphicsContext.current != nil else {
        fail("无法创建 AppKit 绘图上下文；请检查系统图形环境")
    }

    func round(_ rect: NSRect, _ radius: CGFloat, _ fill: NSColor, _ stroke: NSColor? = nil) {
        let path = NSBezierPath(roundedRect: rect, xRadius: radius, yRadius: radius)
        fill.setFill(); path.fill()
        if let stroke { stroke.setStroke(); path.lineWidth = 1; path.stroke() }
    }
    func text(_ value: String, _ x: CGFloat, _ y: CGFloat, _ size: CGFloat,
              _ weight: NSFont.Weight = .regular, _ tint: NSColor = ink) {
        (value as NSString).draw(at: NSPoint(x: x, y: y), withAttributes: [
            .font: NSFont.systemFont(ofSize: size, weight: weight), .foregroundColor: tint
        ])
    }
    func rule(_ x1: CGFloat, _ y: CGFloat, _ x2: CGFloat, _ tint: NSColor = line) {
        let path = NSBezierPath(); path.move(to: NSPoint(x: x1, y: y)); path.line(to: NSPoint(x: x2, y: y))
        path.lineWidth = 1; tint.setStroke(); path.stroke()
    }
    func glow(_ x: CGFloat, _ y: CGFloat, _ radius: CGFloat, _ tint: NSColor) {
        guard let gradient = CGGradient(colorsSpace: colorSpace,
                                        colors: [tint.cgColor, tint.withAlphaComponent(0).cgColor] as CFArray,
                                        locations: [0, 1]) else {
            fail("无法创建发光渐变；请检查系统图形环境")
        }
        cg.drawRadialGradient(gradient, startCenter: CGPoint(x: x, y: y), startRadius: 0,
                             endCenter: CGPoint(x: x, y: y), endRadius: radius, options: [])
    }
    func image(_ image: NSImage, _ rect: NSRect) {
        image.draw(in: rect, from: .zero, operation: .sourceOver, fraction: 1, respectFlipped: true, hints: nil)
    }

    color(0x0b0b0c).setFill(); NSRect(origin: .zero, size: canvas).fill()
    glow(100, -40, 460, color(0x756044, 0.14))
    // 字标用站点同款透明底版本：opaque 版 docs/images/s2arm-wordmark.png 没有 alpha，
    // 铺在近黑背景上会露出黑色圆角块接缝（审计 build/dmg-audit/preview/wordmark-crop.png）。
    // 透明图按内容边界裁切（957×247）；旧 opaque 图 1020×300 绘制 122×36 时内容实际是
    // 114.3×29.5、起点 43.8/31.2，这里取 44/31、116×30（站点导航同为 116×30），内容等大且不变形。
    let wordmarkPath = root.appendingPathComponent("docs/images/s2arm-wordmark-transparent.png")
    guard let wordmark = NSImage(contentsOf: wordmarkPath) else {
        fail("找不到或无法解码字标：\(wordmarkPath.path)；它是站点下载页同款透明底字标，应随仓库检出")
    }
    image(wordmark, NSRect(x: 44, y: 31, width: 116, height: 30))
    round(NSRect(x: 579, y: 34, width: 141, height: 26), 13, color(0x151516), line)
    text("macOS · Apple Silicon", 593, 40, 10, .medium, cream)
    rule(40, 82, 720)

    text("安装 S2Arm", 40, 105, 34, .bold)
    text("拖入右侧文件夹，即可安装。", 42, 155, 15, .regular, muted)
    text("Drag S2Arm into Applications to install.", 42, 179, 12, .regular, muted)

    for name in ["S2Arm.app", "Applications"] {
        guard let center = positions[name] else {
            fail("布局 iconPositions 缺少图标条目：\(name)；必须同时包含 S2Arm.app 与 Applications")
        }
        let x = CGFloat(center.x), y = CGFloat(center.y)
        let card = NSRect(x: x - 118, y: y - 58, width: 236, height: 178)
        round(card, 16, color(0x151516), line)
        cg.saveGState(); NSBezierPath(roundedRect: card, xRadius: 16, yRadius: 16).addClip()
        glow(x, y, 130, (name == "S2Arm.app" ? red : cyan).withAlphaComponent(0.07))
        cg.restoreGState()
        // Finder 用黑色标签，深色背景上不可读；奶油底板垫在真实名称下面。
        // 读数（同一 AppKit API 复测，非审计 probe.swift 的原始 stdout）：13 点名称高 16，
        // 模拟 Finder 名称上沿在 y+64；原底板 y+61…y+89 上留 3、下留 9，名称偏上。
        // 底板上移到 y+58…y+86，上下各留 6 点，名称在底板里居中。
        round(NSRect(x: x - 88, y: y + 58, width: 176, height: 28), 14, cream)
        text(name == "S2Arm.app" ? "01  拖动应用 / DRAG" : "02  放入文件夹 / DROP",
             x - 78, y + 100, 10, .medium, muted)
        if preview {
            let rect = NSRect(x: x - iconSize / 2, y: y - iconSize / 2,
                              width: iconSize, height: iconSize)
            if name == "S2Arm.app", app.isEmpty {
                let iconPath = assets.appendingPathComponent("AppIcon-1024.png")
                guard let appIcon = NSImage(contentsOf: iconPath) else {
                    fail("找不到或无法解码应用图标：\(iconPath.path)；应随仓库检出 Branding/AppIcon-1024.png")
                }
                cg.saveGState()
                NSBezierPath(roundedRect: rect.insetBy(dx: 5, dy: 5), xRadius: 21, yRadius: 21).addClip()
                image(appIcon, rect.insetBy(dx: 5, dy: 5))
                cg.restoreGState()
            } else if name == "Applications" {
                // Read the system artwork directly: NSWorkspace's deferred folder
                // image can render blank in an offscreen command-line context.
                let folderPath = "/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/ApplicationsFolderIcon.icns"
                guard let folderIcon = NSImage(contentsOfFile: folderPath) else {
                    fail("找不到系统 Applications 文件夹图标：\(folderPath)；请换一台完整安装的 macOS 运行 --preview")
                }
                image(folderIcon, rect)
            } else {
                image(NSWorkspace.shared.icon(forFile: app), rect)
            }
            let label = name as NSString
            let attributes: [NSAttributedString.Key: Any] = [.font: NSFont.systemFont(ofSize: 13), .foregroundColor: color(0x1a1712)]
            label.draw(at: NSPoint(x: x - label.size(withAttributes: attributes).width / 2, y: y + 64), withAttributes: attributes)
        }
    }
    guard let appCenter = positions["S2Arm.app"] else {
        fail("布局 iconPositions 缺少图标条目：S2Arm.app；必须同时包含 S2Arm.app 与 Applications")
    }
    let arrowY = CGFloat(appCenter.y)
    rule(322, arrowY, 438, color(0x5a5346))
    round(NSRect(x: 357, y: arrowY - 23, width: 46, height: 46), 23, cream)
    let arrow = NSBezierPath(); arrow.move(to: NSPoint(x: 369, y: arrowY)); arrow.line(to: NSPoint(x: 391, y: arrowY))
    arrow.move(to: NSPoint(x: 383, y: arrowY - 8)); arrow.line(to: NSPoint(x: 391, y: arrowY)); arrow.line(to: NSPoint(x: 383, y: arrowY + 8))
    arrow.lineWidth = 2; arrow.lineCapStyle = .round; arrow.lineJoinStyle = .round
    color(0x1a1712).setStroke(); arrow.stroke()

    rule(40, 409, 720)
    text("复制完成后，从「应用程序」打开 S2Arm。", 42, 430, 13, .medium)
    text("Once copied, open S2Arm from Applications.", 42, 452, 11, .regular, muted)
    text("MACOS 15+", 42, 491, 9, .semibold, muted)
    text("安装后可推出此磁盘 / Eject this disk after installing", 441, 491, 9, .regular, muted)

    guard let drawing = cg.makeImage() else {
        fail("无法生成背景位图；请检查图形上下文状态")
    }
    let bitmap = NSBitmapImageRep(cgImage: drawing)
    bitmap.size = canvas
    return bitmap
}

let background = NSImage(size: canvas)
// 代码按 1× 后 2× 添加 rep；NSImage 序列化后 TIFF 里第一帧是 2×（144 dpi）、第二帧是
// 1×（72 dpi），两帧逻辑尺寸都是 canvas（审计 `tiffutil -info` 与 NSImage.representations
// 读数一致），Finder 在 Retina 下取 2×、普通屏取 1×。
background.addRepresentation(render(scale: 1))
background.addRepresentation(render(scale: 2))
guard let tiff = background.tiffRepresentation else {
    fail("无法序列化 1×/2× TIFF：\(output.path)；请检查绘图上下文")
}
do {
    try tiff.write(to: output.appendingPathComponent("background.tiff"))
} catch {
    fail("写入 background.tiff 失败：\(output.path)：\(error.localizedDescription)；请检查目录权限")
}
guard let previewPNG = render(scale: 2, preview: true).representation(using: .png, properties: [:]) else {
    fail("无法序列化 2× 预览图：\(output.path)；请检查绘图上下文")
}
do {
    try previewPNG.write(to: output.appendingPathComponent("preview.png"))
} catch {
    fail("写入 preview.png 失败：\(output.path)：\(error.localizedDescription)；请检查目录权限")
}
print("Created Retina installer artwork and preview: \(output.path)")
