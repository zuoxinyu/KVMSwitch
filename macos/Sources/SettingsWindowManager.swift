import SwiftUI
import AppKit

class SettingsWindowManager {
    static let shared = SettingsWindowManager()
    var window: NSWindow?
    
    func show(presetManager: PresetManager, monitorManager: MonitorManager) {
        // 允许后台应用拥有独立活动窗口
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
        
        self.window = newWindow
        newWindow.makeKeyAndOrderFront(nil)
        newWindow.orderFrontRegardless()
        NSApp.activate(ignoringOtherApps: true)
    }
}
