import Foundation
import SwiftUI
import AppKit
import IOKit

// MARK: - App Logging

public func appLog(_ msg: String) {
    let line = "[\(Date())] \(msg)\n"
    NSLog("%@", msg)
    if let data = line.data(using: .utf8) {
        if let handle = FileHandle(forWritingAtPath: "/tmp/monitorsuit.log") {
            handle.seekToEndOfFile()
            handle.write(data)
            handle.closeFile()
        } else {
            try? line.write(toFile: "/tmp/monitorsuit.log", atomically: true, encoding: .utf8)
        }
    }
}

// MARK: - Private Apple Silicon IOAVService APIs

@_silgen_name("IOAVServiceCreateWithService")
private func IOAVServiceCreateWithService(_ allocator: CFAllocator?, _ service: io_service_t) -> Unmanaged<AnyObject>?

@_silgen_name("IOAVServiceReadI2C")
private func IOAVServiceReadI2C(_ service: AnyObject, _ chipAddress: UInt32, _ dataAddress: UInt32, _ data: UnsafeMutablePointer<UInt8>, _ dataLength: UInt32) -> kern_return_t

@_silgen_name("IOAVServiceWriteI2C")
private func IOAVServiceWriteI2C(_ service: AnyObject, _ chipAddress: UInt32, _ dataAddress: UInt32, _ data: UnsafePointer<UInt8>, _ dataLength: UInt32) -> kern_return_t

// MARK: - Input Source Definition

public struct InputSourceOption: Identifiable, Hashable, Codable {
    public var id: UInt16 { value }
    public var name: String
    public var value: UInt16

    public init(name: String, value: UInt16) {
        self.name = name
        self.value = value
    }
}

// 默认输入源映射 (参考 Windows KVMSwitch: 泰坦军团 P275MV 及主流显示器)
public let defaultInputSources: [InputSourceOption] = [
    InputSourceOption(name: "USB Type-C",       value: 15),   // 0x0F
    InputSourceOption(name: "DisplayPort (DP)", value: 16),   // 0x10
    InputSourceOption(name: "HDMI 1",          value: 17),   // 0x11
    InputSourceOption(name: "HDMI 2",          value: 18),   // 0x12
    InputSourceOption(name: "USB-C (VESA)",    value: 27),   // 0x1B
    InputSourceOption(name: "HDMI 3",          value: 19),   // 0x13
]

// MARK: - Apple Silicon DDC Helper

enum AppleSiliconDDC {
    static func checksum(chk: UInt8, data: [UInt8], start: Int, end: Int) -> UInt8 {
        var c = chk
        for i in start...end {
            c ^= data[i]
        }
        return c
    }

    static func readVCP(service: AnyObject, command: UInt8) -> UInt16? {
        let chipAddress: UInt8 = 0x37
        let dataAddress: UInt8 = 0x51
        var packet: [UInt8] = [0x82, 0x01, command, 0]
        packet[3] = checksum(chk: chipAddress << 1, data: packet, start: 0, end: 2)

        var reply = [UInt8](repeating: 0, count: 11)
        for _ in 1...4 {
            let kr = IOAVServiceWriteI2C(service, UInt32(chipAddress), UInt32(dataAddress), &packet, UInt32(packet.count))
            if kr != 0 {
                // 句柄失效 (如 268435459)，立刻退出返回 nil 以便触发上层自愈
                return nil
            }
            usleep(45000)
            let rkr = IOAVServiceReadI2C(service, UInt32(chipAddress), 0, &reply, UInt32(reply.count))
            if rkr == 0 {
                let current = UInt16(reply[8]) * 256 + UInt16(reply[9])
                // 验证显示器回复报文：
                // 1) reply[0] 为 0x6E 或 0x50
                // 2) reply[4] 为所请求的 VCP 编号 (如 0x60)
                // 3) 校验和验证：支持直接校验与标准头 (0x88 0x02) 归一化校验
                if (reply[0] == 0x6E || reply[0] == 0x50) && reply[4] == command && current > 0 {
                    let chkDirect = checksum(chk: 0x50, data: reply, start: 0, end: reply.count - 2)
                    var norm = reply
                    norm[1] = 0x88
                    norm[2] = 0x02
                    let chkNorm = checksum(chk: 0x50, data: norm, start: 0, end: reply.count - 2)
                    if chkDirect == reply[reply.count - 1] || chkNorm == reply[reply.count - 1] || reply[reply.count - 1] != 0 {
                        return current
                    }
                }
            }
            usleep(25000)
        }
        return nil
    }

    static func writeVCP(service: AnyObject, command: UInt8, value: UInt16) -> Bool {
        let chipAddress: UInt8 = 0x37
        let dataAddress: UInt8 = 0x51
        let send: [UInt8] = [command, UInt8(value >> 8), UInt8(value & 0xFF)]
        var packet: [UInt8] = [UInt8(0x80 | (send.count + 1)), UInt8(send.count)] + send + [0]
        packet[packet.count - 1] = checksum(chk: (chipAddress << 1) ^ dataAddress, data: packet, start: 0, end: packet.count - 2)

        var lastKr: kern_return_t = -1
        // Apple Silicon DCP: 发送多轮突发写入以确保显示器固件可靠接收并触发物理端口切换
        for _ in 1...4 {
            usleep(25000)
            for _ in 1...2 {
                usleep(10000)
                lastKr = IOAVServiceWriteI2C(service, UInt32(chipAddress), UInt32(dataAddress), &packet, UInt32(packet.count))
            }
        }
        appLog("AppleSiliconDDC.writeVCP: cmd=0x\(String(format: "%02X", command)), val=0x\(String(format: "%02X", value)) (\(value)), lastKr=\(lastKr)")
        return lastKr == 0
    }

    static func readEDIDName(service: AnyObject) -> String? {
        var edid = [UInt8](repeating: 0, count: 128)
        let kr = IOAVServiceReadI2C(service, 0x50, 0x00, &edid, 128)
        if kr == 0 {
            for i in stride(from: 54, to: 126, by: 18) {
                if edid[i] == 0 && edid[i+1] == 0 && edid[i+2] == 0 && edid[i+3] == 0xFC {
                    var name = ""
                    for j in 0..<13 {
                        let c = edid[i + 5 + j]
                        if c == 0x0A { break }
                        if c >= 0x20 && c <= 0x7E { name.append(Character(UnicodeScalar(c))) }
                    }
                    let trimmed = name.trimmingCharacters(in: .whitespaces)
                    if !trimmed.isEmpty { return trimmed }
                }
            }
        }
        return nil
    }
}

// MARK: - Post-Switch Notification & Command Execution (KVMSwitch Parity)

public enum PostSwitchHandler {
    public static func handleSwitch(to sourceValue: UInt16, sourceName: String, monitorName: String) {
        // 1. Notification
        let notify = UserDefaults.standard.object(forKey: "notify_enabled") as? Bool ?? true
        if notify {
            let msg = "已切换至: \(sourceName)"
            NotificationManager.show(title: "KVMSwitch", subtitle: monitorName, message: msg)
        }

        // 2. Custom Command (KVMSwitch on_switch_to_* command execution)
        let key = "command_\(sourceValue)"
        if let cmd = UserDefaults.standard.string(forKey: key), !cmd.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            appLog("执行联动命令: '\(cmd)'")
            CommandExecutor.run(cmd)
        }
    }
}

public enum NotificationManager {
    public static func show(title: String, subtitle: String, message: String) {
        let escapedTitle = title.replacingOccurrences(of: "\"", with: "\\\"")
        let escapedSub = subtitle.replacingOccurrences(of: "\"", with: "\\\"")
        let escapedMsg = message.replacingOccurrences(of: "\"", with: "\\\"")
        let script = "display notification \"\(escapedMsg)\" with title \"\(escapedTitle)\" subtitle \"\(escapedSub)\""

        DispatchQueue.global(qos: .utility).async {
            let process = Process()
            process.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
            process.arguments = ["-e", script]
            try? process.run()
        }
    }
}

public enum CommandExecutor {
    public static func run(_ cmd: String) {
        let trimmed = cmd.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return }
        DispatchQueue.global(qos: .utility).async {
            let process = Process()
            process.executableURL = URL(fileURLWithPath: "/bin/zsh")
            process.arguments = ["-c", trimmed]
            try? process.run()
        }
    }
}

// MARK: - Monitor Model

public class Monitor: ObservableObject, Identifiable {
    public let id = UUID()
    public let displayIndex: Int
    @Published public var name: String
    @Published public var inputSource: UInt16 = 0
    @Published public var availableInputSources: [InputSourceOption] = defaultInputSources

    private var avService: AnyObject?
    private let ddcQueue = DispatchQueue(label: "com.kvmswitch.ddc", qos: .userInitiated)

    public init(displayIndex: Int, name: String, avService: AnyObject?) {
        self.displayIndex = displayIndex
        self.name = name
        self.avService = avService
        refreshState()
    }

    public func updateAVService(_ newService: AnyObject?) {
        self.avService = newService
    }

    /// 重新扫描 I/O Registry 重新获取匹配当前显示器的有效 IOAVService 句柄
    public func refreshAVService() -> Bool {
        let matching = IOServiceMatching("DCPAVServiceProxy")
        var iter: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iter) == KERN_SUCCESS else { return false }
        defer { IOObjectRelease(iter) }

        var foundService: AnyObject? = nil
        var index = 1
        while true {
            let entry = IOIteratorNext(iter)
            if entry == 0 { break }
            defer { IOObjectRelease(entry) }

            var locationStr = "External"
            if let unmanagedLoc = IORegistryEntryCreateCFProperty(entry, "Location" as CFString, kCFAllocatorDefault, 0),
               let loc = unmanagedLoc.takeRetainedValue() as? String {
                locationStr = loc
            }
            if locationStr == "External" {
                if let ioav = IOAVServiceCreateWithService(kCFAllocatorDefault, entry)?.takeRetainedValue() {
                    let edidName = AppleSiliconDDC.readEDIDName(service: ioav) ?? "Display \(index)"
                    if (edidName == self.name || index == self.displayIndex) && foundService == nil {
                        foundService = ioav
                        appLog("显示器 '\(self.name)' 成功更新物理 IOAVService 句柄 (Index: \(index))")
                    }
                    index += 1
                }
            }
        }
        if let service = foundService {
            self.avService = service
            return true
        }
        return false
    }

    public var currentInputSourceName: String {
        if inputSource == 0 { return "检测中..." }
        for src in availableInputSources {
            if src.value == inputSource {
                return src.name
            }
        }
        return String(format: "0x%02X", inputSource)
    }

    public func nameForSource(_ value: UInt16) -> String {
        for src in availableInputSources {
            if src.value == value {
                return src.name
            }
        }
        return String(format: "0x%02X", value)
    }

    // 异步刷新
    public func refreshState() {
        ddcQueue.async {
            _ = self.refreshStateSync()
        }
    }

    // 同步从硬件读取并立即刷新当前状态 (耗时约 40-70ms)
    @discardableResult
    public func refreshStateSync() -> UInt16? {
        if let current = self.readInputSource() {
            DispatchQueue.main.async {
                self.inputSource = current
            }
            self.inputSource = current
            appLog("自动刷新显示器 '\(self.name)' 状态成功: 当前源为 \(self.nameForSource(current)) (0x\(String(format: "%02X", current)))")
            return current
        } else {
            appLog("自动刷新显示器 '\(self.name)' 状态: 未能获取到 VCP 0x60")
            return nil
        }
    }

    public func setInputSource(_ value: UInt16) {
        let previous = self.inputSource
        appLog("即将切换输入源: 显示器 '\(self.name)' -> \(self.nameForSource(value)) (0x\(String(format: "%02X", value)))")
        self.inputSource = value
        ddcQueue.async {
            let success = self.writeInputSource(value)
            DispatchQueue.main.async {
                if success {
                    self.inputSource = value
                    PostSwitchHandler.handleSwitch(to: value, sourceName: self.nameForSource(value), monitorName: self.name)
                } else {
                    appLog("切换输入源指令失败 (write returned false)，恢复先前状态")
                    self.inputSource = previous
                    self.refreshState()
                }
            }
        }
    }

    private func readInputSource() -> UInt16? {
        if let service = avService {
            if let val = AppleSiliconDDC.readVCP(service: service, command: 0x60) {
                return val
            }
            // 读取失败，可能发生过休眠唤醒或物理端口重连导致句柄失效，尝试自愈刷新句柄重试
            appLog("显示器 '\(self.name)' 原句柄读取失败，尝试重新获取物理 IOAVService...")
            if refreshAVService(), let fresh = avService {
                return AppleSiliconDDC.readVCP(service: fresh, command: 0x60)
            }
            return nil
        } else {
            if refreshAVService(), let fresh = avService {
                return AppleSiliconDDC.readVCP(service: fresh, command: 0x60)
            }
            return readInputSourceViaDdcctl()
        }
    }

    private func writeInputSource(_ value: UInt16) -> Bool {
        if let service = avService {
            if AppleSiliconDDC.writeVCP(service: service, command: 0x60, value: value) {
                return true
            }
            // 写入失败，刷新句柄后重试一次
            appLog("显示器 '\(self.name)' 写入失败，尝试重新获取物理 IOAVService...")
            if refreshAVService(), let fresh = avService {
                return AppleSiliconDDC.writeVCP(service: fresh, command: 0x60, value: value)
            }
            return false
        } else {
            if refreshAVService(), let fresh = avService {
                return AppleSiliconDDC.writeVCP(service: fresh, command: 0x60, value: value)
            }
            return writeInputSourceViaDdcctl(value)
        }
    }

    // Fallback for Intel Macs
    private func readInputSourceViaDdcctl() -> UInt16? {
        let output = runDdcctl(["-d", "\(displayIndex)", "-i", "?"])
        for line in output.components(separatedBy: "\n") {
            if let range = line.range(of: #"V\(\s*(\d+)\|"#, options: .regularExpression) {
                let sub = line[range]
                let digits = sub.drop(while: { !$0.isNumber })
                let numStr = String(digits.prefix(while: { $0.isNumber }))
                if let v = UInt16(numStr) { return v }
            }
        }
        return nil
    }

    private func writeInputSourceViaDdcctl(_ value: UInt16) -> Bool {
        let output = runDdcctl(["-d", "\(displayIndex)", "-i", "\(value)"])
        return !output.contains("E: Failed")
    }

    private func runDdcctl(_ args: [String]) -> String {
        for path in ["/opt/homebrew/bin/ddcctl", "/usr/local/bin/ddcctl"] {
            if FileManager.default.isExecutableFile(atPath: path) {
                let process = Process()
                process.executableURL = URL(fileURLWithPath: path)
                process.arguments = args
                let pipe = Pipe()
                process.standardOutput = pipe
                process.standardError = pipe
                try? process.run()
                process.waitUntilExit()
                return String(data: pipe.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
            }
        }
        return ""
    }
}

// MARK: - Monitor Manager

public class MonitorManager: ObservableObject {
    @Published public var monitors: [Monitor] = []

    public init() {
        discoverMonitors()
    }

    func applyPreset(_ preset: Preset) {
        // 关键点：应用预设前先同步刷新所有硬件状态
        refreshAllMonitorsStateSync()
        var appliedCount = 0
        for monitor in monitors {
            if let targetSource = preset.settings[monitor.name], targetSource != 0 {
                appLog("应用预设 '\(preset.name)': 显示器 '\(monitor.name)' -> \(monitor.nameForSource(targetSource))")
                monitor.setInputSource(targetSource)
                appliedCount += 1
            }
        }
        if appliedCount == 0 {
            appLog("预设 '\(preset.name)' 未配置任何匹配显示器的目标输入源")
            NotificationManager.show(
                title: "KVMSwitch",
                subtitle: "预设 '\(preset.name)'",
                message: "该预设尚未配置当前显示器的目标端口，请在设置中配置。"
            )
        }
    }

    // Toggle (一键轮换) - 参考 KVMSwitch
    func toggleInputSource(for monitor: Monitor? = nil) {
        guard let target = monitor ?? monitors.first else {
            appLog("轮换输入源失败: 未发现任何显示器")
            return
        }
        // 关键点：轮换切换前，立即同步从硬件读取最新实际状态！
        let currentHardware = target.refreshStateSync() ?? target.inputSource

        // 轮换序列: DP (16) -> USB Type-C (15) -> HDMI 1 (17) -> HDMI 2 (18) -> DP (16)
        let toggleSequence: [UInt16] = [16, 15, 17, 18]
        var nextCode = toggleSequence[0]
        if let idx = toggleSequence.firstIndex(of: currentHardware) {
            nextCode = toggleSequence[(idx + 1) % toggleSequence.count]
        }
        appLog("一键轮换: 硬件实测当前: 0x\(String(format: "%02X", currentHardware)), 目标切换为下一个: 0x\(String(format: "%02X", nextCode))")
        target.setInputSource(nextCode)
    }

    // 同步刷新所有已连接显示器的实时硬件状态
    public func refreshAllMonitorsStateSync() {
        if monitors.isEmpty {
            discoverMonitorsSync()
        }
        for monitor in monitors {
            if monitor.refreshStateSync() == nil {
                // 若读取失败（可能睡眠唤醒或端口重连导致句柄失效），自愈更新句柄并重试
                appLog("显示器 '\(monitor.name)' 状态读取失败，重新获取物理 IOAVService...")
                if monitor.refreshAVService() {
                    monitor.refreshStateSync()
                } else {
                    discoverMonitorsSync()
                    monitor.refreshStateSync()
                }
            }
        }
    }

    // 同步探测显示器并更新现有句柄
    public func discoverMonitorsSync() {
        var discovered: [(index: Int, name: String, service: AnyObject)] = []

        let matching = IOServiceMatching("DCPAVServiceProxy")
        var iter: io_iterator_t = 0
        if IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iter) == KERN_SUCCESS {
            defer { IOObjectRelease(iter) }
            var index = 1
            while true {
                let entry = IOIteratorNext(iter)
                if entry == 0 { break }
                defer { IOObjectRelease(entry) }

                var locationStr = "External"
                if let unmanagedLoc = IORegistryEntryCreateCFProperty(entry, "Location" as CFString, kCFAllocatorDefault, 0),
                   let loc = unmanagedLoc.takeRetainedValue() as? String {
                    locationStr = loc
                }

                if locationStr == "External" {
                    if let ioav = IOAVServiceCreateWithService(kCFAllocatorDefault, entry)?.takeRetainedValue() {
                        let edidName = AppleSiliconDDC.readEDIDName(service: ioav) ?? "Display \(index)"
                        discovered.append((index: index, name: edidName, service: ioav))
                        index += 1
                    }
                }
            }
        }

        if !discovered.isEmpty {
            var updatedMonitors: [Monitor] = []
            for item in discovered {
                if let existing = self.monitors.first(where: { $0.name == item.name || $0.displayIndex == item.index }) {
                    existing.updateAVService(item.service)
                    updatedMonitors.append(existing)
                } else {
                    let mon = Monitor(displayIndex: item.index, name: item.name, avService: item.service)
                    updatedMonitors.append(mon)
                }
            }
            if Thread.isMainThread {
                self.monitors = updatedMonitors
            } else {
                DispatchQueue.main.sync {
                    self.monitors = updatedMonitors
                }
            }
            appLog("同步探测显示器完成，共检测到 \(updatedMonitors.count) 台外接显示器")
        }
    }

    public func discoverMonitors() {
        appLog("正在探测外接显示器...")
        DispatchQueue.global(qos: .userInitiated).async {
            var newMonitors: [Monitor] = []

            // 1. 优先通过 Apple Silicon 原生 DCPAVServiceProxy 发现
            let matching = IOServiceMatching("DCPAVServiceProxy")
            var iter: io_iterator_t = 0
            if IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iter) == KERN_SUCCESS {
                var entry = IOIteratorNext(iter)
                var index = 1
                while entry != 0 {
                    var locationStr = "External"
                    if let unmanagedLoc = IORegistryEntryCreateCFProperty(entry, "Location" as CFString, kCFAllocatorDefault, 0),
                       let loc = unmanagedLoc.takeRetainedValue() as? String {
                        locationStr = loc
                    }

                    if locationStr == "External" {
                        if let ioav = IOAVServiceCreateWithService(kCFAllocatorDefault, entry)?.takeRetainedValue() {
                            let edidName = AppleSiliconDDC.readEDIDName(service: ioav) ?? "Display \(index)"
                            let mon = Monitor(displayIndex: index, name: edidName, avService: ioav)
                            newMonitors.append(mon)
                            appLog("发现外接显示器 #\(index): '\(edidName)' (Apple Silicon DCP)")
                            index += 1
                        }
                    }
                    IOObjectRelease(entry)
                    entry = IOIteratorNext(iter)
                }
                IOObjectRelease(iter)
            }

            // 2. 若未检测到 DCPAVServiceProxy (例如 Intel 架构)，回退至 ddcctl
            if newMonitors.isEmpty {
                var screenNames: [UInt32: String] = [:]
                DispatchQueue.main.sync {
                    for screen in NSScreen.screens {
                        if let num = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber {
                            screenNames[num.uint32Value] = screen.localizedName
                        }
                    }
                }

                let ddcctlExecutable: String? = {
                    for path in ["/opt/homebrew/bin/ddcctl", "/usr/local/bin/ddcctl"] {
                        if FileManager.default.isExecutableFile(atPath: path) { return path }
                    }
                    return nil
                }()

                if let exe = ddcctlExecutable {
                    let process = Process()
                    process.executableURL = URL(fileURLWithPath: exe)
                    process.arguments = []
                    let pipe = Pipe()
                    process.standardOutput = pipe
                    process.standardError = pipe
                    try? process.run()
                    process.waitUntilExit()
                    let output = String(data: pipe.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""

                    var ddcIndex = 1
                    for line in output.components(separatedBy: "\n") {
                        guard line.contains("CGDisplay") && line.contains("dispID") else { continue }
                        var displayName = "Display \(ddcIndex)"
                        if let range = line.range(of: #"dispID\(#(\d+)\)"#, options: .regularExpression) {
                            let token = line[range]
                            let digits = token.drop(while: { !$0.isNumber })
                            let numStr = String(digits.prefix(while: { $0.isNumber }))
                            if let cgID = UInt32(numStr), let name = screenNames[cgID] {
                                displayName = name
                            }
                        }
                        newMonitors.append(Monitor(displayIndex: ddcIndex, name: displayName, avService: nil))
                        appLog("发现外接显示器 #\(ddcIndex): '\(displayName)' (Intel ddcctl)")
                        ddcIndex += 1
                    }
                }
            }

            DispatchQueue.main.async {
                self.monitors = newMonitors
                appLog("显示器发现完成，共检测到 \(newMonitors.count) 台外接显示器")
            }
        }
    }
}
