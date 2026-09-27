import SwiftUI

struct ContentView: View {
    @ObservedObject var monitorManager: MonitorManager
    @ObservedObject var presetManager: PresetManager

    var body: some View {
        // 1. 预设方案 (Presets)
        if !presetManager.presets.isEmpty {
            Section("预设方案") {
                ForEach(presetManager.presets) { preset in
                    Button(action: {
                        monitorManager.applyPreset(preset)
                    }) {
                        Text("  \(preset.name)")
                    }
                }
            }
            Divider()
        }

        // 2. 轮流切换输入源 (Toggle 模式，参考 KVMSwitch)
        if let primary = monitorManager.monitors.first {
            Button(action: {
                monitorManager.toggleInputSource(for: primary)
            }) {
                Label("轮流切换输入源 (Toggle)", systemImage: "arrow.triangle.2.circlepath")
            }
            Divider()
        }

        // 3. 显示器与输入源控制
        if monitorManager.monitors.isEmpty {
            Text("未发现外接显示器")
        } else if monitorManager.monitors.count == 1, let monitor = monitorManager.monitors.first {
            Label("\(monitor.name)  [当前: \(monitor.currentInputSourceName)]", systemImage: "display")
            
            ForEach(monitor.availableInputSources) { source in
                Button(action: {
                    monitor.setInputSource(source.value)
                }) {
                    if monitor.inputSource == source.value {
                        Text("✓  \(source.name)  (0x\(String(format: "%02X", source.value)))")
                    } else {
                        Text("    \(source.name)  (0x\(String(format: "%02X", source.value)))")
                    }
                }
            }
        } else {
            ForEach(monitorManager.monitors) { monitor in
                Menu {
                    ForEach(monitor.availableInputSources) { source in
                        Button(action: {
                            monitor.setInputSource(source.value)
                        }) {
                            if monitor.inputSource == source.value {
                                Text("✓  \(source.name)  (0x\(String(format: "%02X", source.value)))")
                            } else {
                                Text("    \(source.name)  (0x\(String(format: "%02X", source.value)))")
                            }
                        }
                    }
                } label: {
                    Label("\(monitor.name)  [\(monitor.currentInputSourceName)]", systemImage: "display")
                }
            }
        }

        Divider()

        Button(action: {
            monitorManager.discoverMonitors()
        }) {
            Label("刷新显示器状态", systemImage: "arrow.clockwise")
        }

        Button(action: {
            SettingsWindowManager.shared.show(presetManager: presetManager, monitorManager: monitorManager)
        }) {
            Label("设置...", systemImage: "gearshape")
        }

        Divider()

        Button("退出") {
            NSApplication.shared.terminate(nil)
        }
    }
}
