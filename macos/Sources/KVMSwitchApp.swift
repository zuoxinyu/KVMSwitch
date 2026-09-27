import Cocoa
import SwiftUI

// MARK: - Port Icons & SF Symbol Helpers

enum PortIcon {
    static func icon(for sourceName: String) -> NSImage {
        let lower = sourceName.lowercased()
        if lower.contains("type-c") || lower.contains("usb-c") {
            return typeC
        } else if lower.contains("displayport") || lower.contains("dp") {
            return displayPort
        } else if lower.contains("hdmi") {
            return hdmi
        } else {
            return defaultConnector
        }
    }

    /// Type-C (USB-C) 矢量图标：药丸对称椭圆轮廓 + 中心接触舌片
    static let typeC: NSImage = {
        let size = NSSize(width: 16, height: 16)
        let img = NSImage(size: size, flipped: false) { _ in
            let pillRect = NSRect(x: 1.5, y: 4.5, width: 13, height: 7)
            let path = NSBezierPath(roundedRect: pillRect, xRadius: 3.5, yRadius: 3.5)
            path.lineWidth = 1.25
            NSColor.black.setStroke()
            path.stroke()

            let tongue = NSBezierPath(roundedRect: NSRect(x: 4.5, y: 7.25, width: 7, height: 1.5), xRadius: 0.75, yRadius: 0.75)
            NSColor.black.setFill()
            tongue.fill()
            return true
        }
        img.isTemplate = true
        return img
    }()

    /// DisplayPort (DP) 矢量图标：标准右上角 45° 切角斜边轮廓 + 内部接触导片
    static let displayPort: NSImage = {
        let size = NSSize(width: 16, height: 16)
        let img = NSImage(size: size, flipped: false) { _ in
            let path = NSBezierPath()
            path.move(to: NSPoint(x: 2.2, y: 4))
            path.line(to: NSPoint(x: 2.2, y: 12))
            path.line(to: NSPoint(x: 9.8, y: 12))
            path.line(to: NSPoint(x: 13.8, y: 8))
            path.line(to: NSPoint(x: 13.8, y: 4))
            path.close()
            path.lineWidth = 1.25
            path.lineJoinStyle = .round
            path.lineCapStyle = .round
            NSColor.black.setStroke()
            path.stroke()

            let inner = NSBezierPath(roundedRect: NSRect(x: 4.5, y: 6.2, width: 6.5, height: 1.5), xRadius: 0.75, yRadius: 0.75)
            NSColor.black.setFill()
            inner.fill()
            return true
        }
        img.isTemplate = true
        return img
    }()

    /// HDMI 矢量图标：经典上宽下窄梯形切角阶梯轮廓 + 内部金手指导片
    static let hdmi: NSImage = {
        let size = NSSize(width: 16, height: 16)
        let img = NSImage(size: size, flipped: false) { _ in
            let path = NSBezierPath()
            path.move(to: NSPoint(x: 1.8, y: 12))
            path.line(to: NSPoint(x: 14.2, y: 12))
            path.line(to: NSPoint(x: 14.2, y: 7.5))
            path.line(to: NSPoint(x: 11.8, y: 4))
            path.line(to: NSPoint(x: 4.2, y: 4))
            path.line(to: NSPoint(x: 1.8, y: 7.5))
            path.close()
            path.lineWidth = 1.25
            path.lineJoinStyle = .round
            path.lineCapStyle = .round
            NSColor.black.setStroke()
            path.stroke()

            let inner = NSBezierPath(roundedRect: NSRect(x: 4.2, y: 6.8, width: 7.6, height: 1.5), xRadius: 0.75, yRadius: 0.75)
            NSColor.black.setFill()
            inner.fill()
            return true
        }
        img.isTemplate = true
        return img
    }()

    static let defaultConnector: NSImage = {
        let config = NSImage.SymbolConfiguration(pointSize: 12, weight: .regular)
        let img = NSImage(systemSymbolName: "cable.connector", accessibilityDescription: nil)?.withSymbolConfiguration(config) ?? NSImage()
        img.isTemplate = true
        return img
    }()
}

private func menuSymbol(_ name: String) -> NSImage? {
    let config = NSImage.SymbolConfiguration(pointSize: 13, weight: .regular)
    guard let img = NSImage(systemSymbolName: name, accessibilityDescription: nil)?.withSymbolConfiguration(config) else {
        return nil
    }
    img.isTemplate = true
    return img
}

private func setMenuItemImage(_ item: NSMenuItem, _ image: NSImage?) {
    guard let image = image else { return }
    item.image = image
    if item.responds(to: NSSelectorFromString("setPreferredImageVisibility:")) {
        item.setValue(1, forKey: "preferredImageVisibility")
    }
}

enum KVMSwitchIcon {
    /// 简化自 KVMSwitch.png 的 macOS 菜单栏图标（外接显示器轮廓 + 屏幕内部向右切换信号箭头）
    static let statusBarIcon: NSImage = {
        let size = NSSize(width: 18, height: 18)
        let img = NSImage(size: size, flipped: false) { _ in
            // 1. 显示器屏幕轮廓 (Apple 风格平滑圆角矩形)
            let screenRect = NSRect(x: 1.5, y: 5.5, width: 15.0, height: 10.5)
            let screen = NSBezierPath(roundedRect: screenRect, xRadius: 1.8, yRadius: 1.8)
            screen.lineWidth = 1.3
            screen.lineJoinStyle = .round
            NSColor.black.setStroke()
            screen.stroke()

            // 2. 支架颈部与底座 (Stand Neck & Base)
            let neck = NSBezierPath()
            neck.move(to: NSPoint(x: 7.75, y: 5.5))
            neck.line(to: NSPoint(x: 7.25, y: 3.5))
            neck.line(to: NSPoint(x: 10.75, y: 3.5))
            neck.line(to: NSPoint(x: 10.25, y: 5.5))
            neck.close()
            NSColor.black.setFill()
            neck.fill()

            let baseRect = NSRect(x: 5.25, y: 2.0, width: 7.5, height: 1.6)
            let base = NSBezierPath(roundedRect: baseRect, xRadius: 0.8, yRadius: 0.8)
            NSColor.black.setFill()
            base.fill()

            // 3. 屏幕内部向右切换箭头 (源自 KVMSwitch.png 品牌标识)
            let arrow = NSBezierPath()
            arrow.move(to: NSPoint(x: 12.8, y: 10.75))
            arrow.line(to: NSPoint(x: 9.2, y: 13.5))
            arrow.line(to: NSPoint(x: 9.2, y: 11.9))
            arrow.line(to: NSPoint(x: 4.8, y: 11.9))
            arrow.line(to: NSPoint(x: 4.8, y: 9.6))
            arrow.line(to: NSPoint(x: 9.2, y: 9.6))
            arrow.line(to: NSPoint(x: 9.2, y: 8.0))
            arrow.close()
            arrow.lineJoinStyle = .round
            NSColor.black.setFill()
            arrow.fill()

            return true
        }
        img.isTemplate = true
        return img
    }()
}

// MARK: - AppKit AppDelegate (Native Menu Bar Item)

class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    static var shared: AppDelegate!

    var statusItem: NSStatusItem!
    let menu = NSMenu()
    var monitorManager: MonitorManager!
    var presetManager: PresetManager!

    func applicationDidFinishLaunching(_ notification: Notification) {
        AppDelegate.shared = self
        monitorManager = MonitorManager()
        presetManager = PresetManager()

        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        if let button = statusItem.button {
            button.image = KVMSwitchIcon.statusBarIcon
        }

        menu.delegate = self
        statusItem.menu = menu

        appLog("KVMSwitch 已就绪 (使用 AppKit 原生 NSMenu)")
    }

    // 每次点击菜单图标弹出前，立即从硬件读取并自动刷新当前实时状态
    func menuWillOpen(_ menu: NSMenu) {
        appLog("点击状态栏图标，正在从物理显示器读取并自动刷新最新状态...")
        // 同步从硬件读取最新输入源 (约 50-70ms)
        monitorManager.refreshAllMonitorsStateSync()
        // 渲染最新带对勾的菜单项
        rebuildMenu()
    }

    func rebuildMenu() {
        menu.removeAllItems()

        // 1. 预设方案 (Presets)
        if !presetManager.presets.isEmpty {
            let presetHeader = NSMenuItem(title: "预设方案", action: nil, keyEquivalent: "")
            presetHeader.isEnabled = false
            menu.addItem(presetHeader)

            for preset in presetManager.presets {
                let item = NSMenuItem(title: "   \(preset.name)", action: #selector(onPresetClicked(_:)), keyEquivalent: "")
                item.target = self
                item.representedObject = preset
                menu.addItem(item)
            }
            menu.addItem(NSMenuItem.separator())
        }

        // 2. 一键轮换 (Toggle 模式 - 参考 KVMSwitch)
        if let primary = monitorManager.monitors.first {
            let toggleItem = NSMenuItem(title: "轮流切换输入源 (Toggle)", action: #selector(onToggleClicked(_:)), keyEquivalent: "")
            toggleItem.target = self
            toggleItem.representedObject = primary
            setMenuItemImage(toggleItem, menuSymbol("arrow.triangle.2.circlepath"))
            menu.addItem(toggleItem)
            menu.addItem(NSMenuItem.separator())
        }

        // 3. 显示器与输入源列表
        if monitorManager.monitors.isEmpty {
            let noMonItem = NSMenuItem(title: "未发现外接显示器", action: nil, keyEquivalent: "")
            noMonItem.isEnabled = false
            menu.addItem(noMonItem)
        } else if monitorManager.monitors.count == 1, let monitor = monitorManager.monitors.first {
            let monHeader = NSMenuItem(title: "\(monitor.name)  [当前: \(monitor.currentInputSourceName)]", action: nil, keyEquivalent: "")
            monHeader.isEnabled = false
            setMenuItemImage(monHeader, menuSymbol("display"))
            menu.addItem(monHeader)

            for source in monitor.availableInputSources {
                let hexStr = String(format: "0x%02X", source.value)
                let item = NSMenuItem(title: "\(source.name)  (\(hexStr))", action: #selector(onInputSourceClicked(_:)), keyEquivalent: "")
                item.target = self
                item.tag = Int(source.value)
                item.representedObject = monitor
                item.state = (monitor.inputSource == source.value) ? .on : .off
                setMenuItemImage(item, PortIcon.icon(for: source.name))
                menu.addItem(item)
            }
        } else {
            for monitor in monitorManager.monitors {
                let subMenu = NSMenu()
                let monItem = NSMenuItem(title: "\(monitor.name)  [\(monitor.currentInputSourceName)]", action: nil, keyEquivalent: "")
                setMenuItemImage(monItem, menuSymbol("display"))
                monItem.submenu = subMenu

                for source in monitor.availableInputSources {
                    let hexStr = String(format: "0x%02X", source.value)
                    let item = NSMenuItem(title: "\(source.name)  (\(hexStr))", action: #selector(onInputSourceClicked(_:)), keyEquivalent: "")
                    item.target = self
                    item.tag = Int(source.value)
                    item.representedObject = monitor
                    item.state = (monitor.inputSource == source.value) ? .on : .off
                    setMenuItemImage(item, PortIcon.icon(for: source.name))
                    subMenu.addItem(item)
                }
                menu.addItem(monItem)
            }
        }

        menu.addItem(NSMenuItem.separator())

        // 4. 刷新
        let refreshItem = NSMenuItem(title: "刷新显示器状态", action: #selector(onRefreshClicked), keyEquivalent: "")
        refreshItem.target = self
        setMenuItemImage(refreshItem, menuSymbol("arrow.clockwise"))
        menu.addItem(refreshItem)

        // 5. 设置
        let settingsItem = NSMenuItem(title: "设置...", action: #selector(onSettingsClicked), keyEquivalent: ",")
        settingsItem.target = self
        setMenuItemImage(settingsItem, menuSymbol("gearshape"))
        menu.addItem(settingsItem)

        menu.addItem(NSMenuItem.separator())

        // 6. 退出
        let quitItem = NSMenuItem(title: "退出", action: #selector(onQuitClicked), keyEquivalent: "q")
        quitItem.target = self
        setMenuItemImage(quitItem, menuSymbol("power"))
        menu.addItem(quitItem)
    }

    // MARK: - Menu Actions

    @objc func onInputSourceClicked(_ sender: NSMenuItem) {
        let code = UInt16(sender.tag)
        if let monitor = sender.representedObject as? Monitor {
            appLog("点击输入源: 显示器 '\(monitor.name)', 目标端口代码: 0x\(String(format: "%02X", code)) (\(code))")
            monitor.setInputSource(code)
        }
    }

    @objc func onPresetClicked(_ sender: NSMenuItem) {
        if let preset = sender.representedObject as? Preset {
            appLog("点击预设: '\(preset.name)'")
            monitorManager.applyPreset(preset)
        }
    }

    @objc func onToggleClicked(_ sender: NSMenuItem) {
        let monitor = sender.representedObject as? Monitor
        appLog("点击一键轮换 (Toggle)")
        monitorManager.toggleInputSource(for: monitor)
    }

    @objc func onRefreshClicked() {
        appLog("手动点击刷新显示器状态")
        monitorManager.refreshAllMonitorsStateSync()
        rebuildMenu()
    }

    @objc func onSettingsClicked() {
        appLog("点击打开设置窗口")
        SettingsWindowManager.shared.show(presetManager: presetManager, monitorManager: monitorManager)
    }

    @objc func onQuitClicked() {
        appLog("用户退出应用")
        NSApplication.shared.terminate(nil)
    }
}

// MARK: - SwiftUI App Entry

@main
struct KVMSwitchApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate

    var body: some Scene {
        Settings {
            EmptyView()
        }
    }
}
