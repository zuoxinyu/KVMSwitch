import SwiftUI

struct SettingsView: View {
    @ObservedObject var presetManager: PresetManager
    @ObservedObject var monitorManager: MonitorManager
    
    @State private var selectedTab: Int = 0
    @State private var selectedPresetId: UUID?
    
    @State private var notifyEnabled: Bool = UserDefaults.standard.object(forKey: "notify_enabled") as? Bool ?? true
    @State private var cmdDP: String = UserDefaults.standard.string(forKey: "command_16") ?? ""
    @State private var cmdTypeC: String = UserDefaults.standard.string(forKey: "command_15") ?? ""
    @State private var cmdHDMI1: String = UserDefaults.standard.string(forKey: "command_17") ?? ""
    @State private var cmdHDMI2: String = UserDefaults.standard.string(forKey: "command_18") ?? ""
    
    var body: some View {
        VStack(spacing: 0) {
            Picker("", selection: $selectedTab) {
                Text("预设方案").tag(0)
                Text("快捷与联动").tag(1)
            }
            .pickerStyle(.segmented)
            .padding(.horizontal, 20)
            .padding(.vertical, 12)
            
            Divider()
            
            if selectedTab == 0 {
                presetTab
            } else {
                commandsTab
            }
        }
        .frame(width: 580, height: 420)
    }
    
    // MARK: - Presets Tab
    
    var presetTab: some View {
        HStack(spacing: 0) {
            VStack(spacing: 0) {
                List(selection: $selectedPresetId) {
                    ForEach(presetManager.presets) { preset in
                        Text(preset.name).tag(preset.id)
                    }
                }
                .listStyle(.sidebar)
                
                Divider()
                
                HStack(spacing: 12) {
                    Button(action: {
                        let newPreset = Preset(name: "新建预设", settings: [:])
                        presetManager.presets.append(newPreset)
                        selectedPresetId = newPreset.id
                    }) {
                        Image(systemName: "plus")
                    }
                    .buttonStyle(.plain)
                    
                    Button(action: {
                        if let id = selectedPresetId {
                            presetManager.presets.removeAll { $0.id == id }
                            selectedPresetId = nil
                        }
                    }) {
                        Image(systemName: "minus")
                    }
                    .buttonStyle(.plain)
                    .disabled(selectedPresetId == nil)
                    
                    Spacer()
                }
                .padding(.horizontal, 10)
                .padding(.vertical, 8)
                .background(Color(NSColor.controlBackgroundColor))
            }
            .frame(width: 170)
            
            Divider()
            
            if let id = selectedPresetId, let index = presetManager.presets.firstIndex(where: { $0.id == id }) {
                VStack(alignment: .leading, spacing: 14) {
                    HStack {
                        Text("预设名称:")
                            .font(.subheadline)
                            .foregroundColor(.secondary)
                            .frame(width: 80, alignment: .trailing)
                        TextField("预设名称", text: Binding(
                            get: { presetManager.presets[index].name },
                            set: { presetManager.presets[index].name = $0; presetManager.save() }
                        ))
                        .textFieldStyle(.roundedBorder)
                    }
                    .padding(.top, 4)
                    
                    Divider().padding(.vertical, 2)
                    
                    Text("显示器目标输入源配置:")
                        .font(.headline)
                    
                    if monitorManager.monitors.isEmpty {
                        Text("未检测到外接显示器（请先连接显示器并刷新）")
                            .font(.subheadline)
                            .foregroundColor(.secondary)
                            .padding(.top, 4)
                    } else {
                        VStack(spacing: 10) {
                            ForEach(monitorManager.monitors) { monitor in
                                HStack {
                                    HStack(spacing: 4) {
                                        Image(systemName: "display")
                                        Text("\(monitor.name):")
                                    }
                                    .font(.subheadline)
                                    .frame(width: 140, alignment: .trailing)

                                    Picker("", selection: Binding(
                                        get: { presetManager.presets[index].settings[monitor.name] ?? 0 },
                                        set: { val in
                                            if val == 0 {
                                                presetManager.presets[index].settings.removeValue(forKey: monitor.name)
                                            } else {
                                                presetManager.presets[index].settings[monitor.name] = val
                                            }
                                            presetManager.save()
                                        }
                                    )) {
                                        Text("不改变 (Don't Change)").tag(UInt16(0))
                                        ForEach(monitor.availableInputSources) { source in
                                            Text("\(source.name) (0x\(String(format: "%02X", source.value)))").tag(source.value)
                                        }
                                    }
                                    .labelsHidden()
                                    Spacer()
                                }
                            }
                        }
                    }
                    
                    Spacer()
                }
                .padding(20)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            } else {
                VStack(spacing: 8) {
                    Image(systemName: "slider.horizontal.3")
                        .font(.system(size: 28))
                        .foregroundColor(.secondary)
                    Text("从左侧选择或新建预设方案")
                        .foregroundColor(.secondary)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
    }
    
    // MARK: - Commands Tab (KVMSwitch Parity)
    
    var commandsTab: some View {
        VStack(alignment: .leading, spacing: 14) {
            Toggle("切换输入源成功后弹出系统气泡通知", isOn: Binding(
                get: { notifyEnabled },
                set: {
                    notifyEnabled = $0
                    UserDefaults.standard.set($0, forKey: "notify_enabled")
                }
            ))
            .toggleStyle(.checkbox)
            
            Divider().padding(.vertical, 2)
            
            VStack(alignment: .leading, spacing: 4) {
                Text("联动命令 (切换至对应输入源后在后台自动执行，可选):")
                    .font(.headline)
                Text("支持配置终端命令或脚本（留空则不执行联动）：")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
            
            VStack(spacing: 10) {
                HStack {
                    Text("DisplayPort (0x10):")
                        .font(.subheadline)
                        .frame(width: 150, alignment: .trailing)
                    TextField("例如: caffeinate -u -t 2", text: Binding(
                        get: { cmdDP },
                        set: {
                            cmdDP = $0
                            UserDefaults.standard.set($0, forKey: "command_16")
                        }
                    ))
                    .textFieldStyle(.roundedBorder)
                }
                
                HStack {
                    Text("USB Type-C (0x0F):")
                        .font(.subheadline)
                        .frame(width: 150, alignment: .trailing)
                    TextField("例如: ssh mac caffeinate -u -t 2", text: Binding(
                        get: { cmdTypeC },
                        set: {
                            cmdTypeC = $0
                            UserDefaults.standard.set($0, forKey: "command_15")
                        }
                    ))
                    .textFieldStyle(.roundedBorder)
                }
                
                HStack {
                    Text("HDMI 1 (0x11):")
                        .font(.subheadline)
                        .frame(width: 150, alignment: .trailing)
                    TextField("例如: 自定义脚本或命令", text: Binding(
                        get: { cmdHDMI1 },
                        set: {
                            cmdHDMI1 = $0
                            UserDefaults.standard.set($0, forKey: "command_17")
                        }
                    ))
                    .textFieldStyle(.roundedBorder)
                }
                
                HStack {
                    Text("HDMI 2 (0x12):")
                        .font(.subheadline)
                        .frame(width: 150, alignment: .trailing)
                    TextField("例如: 自定义脚本或命令", text: Binding(
                        get: { cmdHDMI2 },
                        set: {
                            cmdHDMI2 = $0
                            UserDefaults.standard.set($0, forKey: "command_18")
                        }
                    ))
                    .textFieldStyle(.roundedBorder)
                }
            }
            
            Text("提示：与 Windows 端的 config.ini [Commands] 功能一致，切换到对应端口后会在后台静默通过 /bin/zsh 执行。")
                .font(.caption)
                .foregroundColor(.secondary)
                .padding(.top, 4)
            
            Spacer()
        }
        .padding(20)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    }
}
