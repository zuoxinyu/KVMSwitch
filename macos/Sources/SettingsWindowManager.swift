import SwiftUI
import AppKit

class SettingsWindowManager: NSObject, NSWindowDelegate {
    static let shared = SettingsWindowManager()
    var window: NSWindow?
    
    func show(presetManager: PresetManager, monitorManager: MonitorManager) {
        // 打开设置窗口时才展示 Dock 图标并置于前台获得焦点
        NSApp.setActivationPolicy(.regular)
        
        if let win = window {
            win.makeKeyAndOrderFront(nil)
            win.orderFrontRegardless()
            NSApp.activate(ignoringOtherApps: true)
            return
        }
        
        let view = SettingsView(presetManager: presetManager, monitorManager: monitorManager)
        let hostingController = NSHostingController(rootView: view)
        
        let newWindow = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 580, height: 420),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered,
            defer: false
        )
        newWindow.title = "KVMSwitch 设置"
        newWindow.center()
        newWindow.contentViewController = hostingController
        newWindow.isReleasedWhenClosed = false
        newWindow.delegate = self
        
        self.window = newWindow
        newWindow.makeKeyAndOrderFront(nil)
        newWindow.orderFrontRegardless()
        NSApp.activate(ignoringOtherApps: true)
    }
    
    // 关闭设置窗口时，立即恢复为后台附属模式 (.accessory)，自动从 Dock 移除图标
    func windowWillClose(_ notification: Notification) {
        DispatchQueue.main.async {
            NSApp.setActivationPolicy(.accessory)
        }
    }
}
