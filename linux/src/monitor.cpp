// monitor.cpp : 显示器枚举、状态读取、切换、预设与轮换
#include "kvmswitch.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace kvm {

namespace {

std::string readSysFile(const std::string& path, size_t maxBytes = 4096) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.size() > maxBytes) data.resize(maxBytes);
    return data;
}

std::string runCapture(const std::string& command) {
    FILE* pipe = ::popen(command.c_str(), "r");
    if (!pipe) return {};
    std::string output;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe)) output += buffer;
    ::pclose(pipe);
    return output;
}

bool isInternalConnector(const std::string& connector) {
    const std::string upper = toUpper(connector);
    return startsWith(upper, "EDP") || startsWith(upper, "LVDS") ||
           startsWith(upper, "DSI") || startsWith(upper, "WRITEBACK") ||
           startsWith(upper, "VIRTUAL");
}

std::string connectorFromDrm(const std::string& drmName) {
    const size_t dash = drmName.find('-');
    return dash == std::string::npos ? drmName : drmName.substr(dash + 1);
}

std::string findI2cBus(const fs::path& connectorPath) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(connectorPath, ec)) {
        const std::string name = entry.path().filename().string();
        if (startsWith(name, "i2c-")) return "/dev/" + name;
    }
    // 某些驱动只提供 ddc 符号链接
    const fs::path ddc = connectorPath / "ddc";
    if (fs::exists(ddc, ec)) {
        std::error_code linkEc;
        const fs::path target = fs::read_symlink(ddc, linkEc);
        if (!linkEc) {
            const std::string name = target.filename().string();
            if (startsWith(name, "i2c-")) return "/dev/" + name;
        }
    }
    return {};
}

std::string capsCachePath(const std::string& edidHash) {
    if (edidHash.empty()) return {};
    return (fs::path(getCacheDir()) / "caps" / (edidHash + ".txt")).string();
}

std::string loadCapsFromCache(const std::string& edidHash) {
    const std::string path = capsCachePath(edidHash);
    if (path.empty()) return {};
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.size() > 4096) data.resize(4096);
    return trim(data);
}

void saveCapsToCache(const std::string& edidHash, const std::string& caps) {
    const std::string path = capsCachePath(edidHash);
    if (path.empty() || caps.empty()) return;
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    if (ec) return;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return;
    file << caps;
}

std::vector<uint16_t> parseCapabilitiesInputCodes(const std::string& caps) {
    std::vector<uint16_t> codes;
    size_t pos = caps.find("60(");
    if (pos == std::string::npos) pos = caps.find("60 (");
    if (pos == std::string::npos) return codes;

    const size_t start = caps.find('(', pos);
    const size_t end = caps.find(')', start);
    if (start == std::string::npos || end == std::string::npos || end <= start) return codes;

    std::istringstream stream(caps.substr(start + 1, end - start - 1));
    std::string token;
    while (stream >> token) {
        try {
            const unsigned long value = std::stoul(token, nullptr, 16);
            if (value > 0 && value <= 0xFF) {
                const uint16_t code = static_cast<uint16_t>(value);
                if (std::find(codes.begin(), codes.end(), code) == codes.end()) codes.push_back(code);
            }
        } catch (...) {
        }
    }
    return codes;
}

int inputWeight(uint16_t code, uint16_t detectedTypeCCode) {
    if (code == 16) return 1;
    if (code == 17) return 2;
    if (code == 18) return 3;
    if (code == detectedTypeCCode || code == 15 || code == 27 || code == 25) return 4;
    return static_cast<int>(code) + 10;
}

} // namespace

std::string detectPrimaryConnector() {
    if (const char* env = std::getenv("KVMSWITCH_PRIMARY"); env && *env) return env;

    // X11 / XWayland：xrandr 输出中带 primary 标记
    const std::string xrandr = runCapture("xrandr --query 2>/dev/null");
    if (!xrandr.empty()) {
        std::istringstream stream(xrandr);
        std::string line;
        while (std::getline(stream, line)) {
            std::istringstream lineStream(line);
            std::string name;
            if (!(lineStream >> name)) continue;
            if (line.find(" connected primary") != std::string::npos) return name;
        }
    }

    // KDE Wayland：kscreen-doctor 的 priority 1
    const std::string kscreen = runCapture("kscreen-doctor -o 2>/dev/null");
    if (!kscreen.empty()) {
        std::istringstream stream(kscreen);
        std::string line;
        std::string current;
        while (std::getline(stream, line)) {
            const size_t outputPos = line.find("Output:");
            if (outputPos != std::string::npos) {
                std::istringstream lineStream(line.substr(outputPos + 7));
                std::string index;
                lineStream >> index >> current;
            } else if (line.find("priority 1") != std::string::npos && !current.empty()) {
                return current;
            }
        }
    }
    return {};
}

std::vector<MonitorInfo> enumerateMonitors() {
    std::vector<MonitorInfo> monitors;
    const fs::path drmRoot("/sys/class/drm");
    std::error_code ec;
    if (!fs::exists(drmRoot, ec)) return monitors;

    for (const auto& entry : fs::directory_iterator(drmRoot, ec)) {
        const std::string drmName = entry.path().filename().string();
        // 只处理 cardN-<connector> 形式的连接器目录
        if (!startsWith(drmName, "card")) continue;
        const size_t dash = drmName.find('-');
        if (dash == std::string::npos) continue;

        const std::string connector = connectorFromDrm(drmName);
        if (connector.empty() || isInternalConnector(connector)) continue;

        const std::string status = trim(readSysFile((entry.path() / "status").string(), 64));
        if (status != "connected") continue;

        const std::string bus = findI2cBus(entry.path());
        if (bus.empty()) continue;

        MonitorInfo monitor;
        monitor.drmPath = drmName;
        monitor.connector = connector;
        monitor.i2cBus = bus;
        monitor.isExternal = true;
        monitor.edidName = readEdidName((entry.path() / "edid").string());
        monitor.edidHash = hashEdid((entry.path() / "edid").string());
        monitor.description = monitor.edidName.empty() ? connector : monitor.edidName;
        monitors.push_back(std::move(monitor));
    }

    std::sort(monitors.begin(), monitors.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
        return a.drmPath < b.drmPath;
    });

    const std::string primary = detectPrimaryConnector();
    bool primaryFound = false;
    if (!primary.empty()) {
        for (auto& monitor : monitors) {
            if (toUpper(monitor.connector) == toUpper(primary) ||
                toUpper(monitor.drmPath) == toUpper(primary)) {
                monitor.isPrimary = true;
                primaryFound = true;
            }
        }
    }
    if (!primaryFound && !monitors.empty()) {
        // 无法获取桌面主屏信息时，单屏默认主屏；多屏按连接器排序取第一台
        monitors.front().isPrimary = true;
    }

    for (size_t i = 0; i < monitors.size(); ++i) monitors[i].index1 = static_cast<int>(i + 1);
    return monitors;
}

uint16_t detectTypeCCode(const MonitorContext& ctx) {
    for (const auto& monitor : ctx.monitors) {
        if (monitor.supportedInputs.empty()) continue;
        const bool has15 = std::find(monitor.supportedInputs.begin(), monitor.supportedInputs.end(), 15) !=
                           monitor.supportedInputs.end();
        const bool has27 = std::find(monitor.supportedInputs.begin(), monitor.supportedInputs.end(), 27) !=
                           monitor.supportedInputs.end();
        if (has15 && !has27) return 15;
        if (has27) return 27;
    }
    return 15;
}

std::vector<uint16_t> getSupportedInputCodes(const MonitorInfo& mon, uint16_t detectedTypeCCode) {
    std::vector<uint16_t> codes;
    if (!mon.supportedInputs.empty()) codes = mon.supportedInputs;
    else codes = {15, 16, 17, 18};

    auto add = [&](uint16_t code) {
        if (code > 0 && std::find(codes.begin(), codes.end(), code) == codes.end()) codes.push_back(code);
    };
    add(16);
    add(17);
    add(detectedTypeCCode);

    std::sort(codes.begin(), codes.end(), [&](uint16_t a, uint16_t b) {
        return inputWeight(a, detectedTypeCCode) < inputWeight(b, detectedTypeCCode);
    });
    return codes;
}

std::vector<std::string> getSupportedInputNames(const MonitorInfo& mon, const AppConfig& cfg,
                                                uint16_t detectedTypeCCode) {
    std::vector<std::string> names;
    for (uint16_t code : getSupportedInputCodes(mon, detectedTypeCCode)) {
        names.push_back(getInputName(code, &cfg));
    }
    return names;
}

MonitorContext queryMonitors(const AppConfig& cfg, bool readCapabilities,
                             uint16_t detectedTypeCCode, bool useCapsCache) {
    (void)cfg;
    MonitorContext ctx;
    ctx.detectedTypeCCode = detectedTypeCCode;
    ctx.monitors = enumerateMonitors();

    const bool noCache = std::getenv("KVMSWITCH_NO_CAPS_CACHE") != nullptr;
    for (auto& monitor : ctx.monitors) {
        if (readCapabilities) {
            std::string capabilities;
            if (useCapsCache && !noCache) capabilities = loadCapsFromCache(monitor.edidHash);
            if (capabilities.empty()) {
                DdcBus bus(monitor.i2cBus);
                std::string error;
                if (bus.open(&error)) {
                    capabilities = bus.getCapabilities(&error);
                    if (!capabilities.empty()) saveCapsToCache(monitor.edidHash, capabilities);
                }
                if (capabilities.empty()) {
                    logMsg("读取 " + monitor.connector + " Capabilities 失败: " + error);
                }
            }
            monitor.supportedInputs = parseCapabilitiesInputCodes(capabilities);
        } else {
            const std::string cached = loadCapsFromCache(monitor.edidHash);
            monitor.supportedInputs = parseCapabilitiesInputCodes(cached);
        }

        DdcBus bus(monitor.i2cBus);
        std::string error;
        if (bus.open(&error)) {
            if (auto value = bus.getVcp(0x60, &error)) monitor.currentInput = *value;
            else logMsg("读取 " + monitor.connector + " 当前输入源失败: " + error);
        } else {
            logMsg("打开 " + monitor.i2cBus + " 失败: " + error);
        }
    }

    if (readCapabilities) ctx.detectedTypeCCode = detectTypeCCode(ctx);
    return ctx;
}

void refreshMonitorStates(MonitorContext& ctx) {
    for (auto& monitor : ctx.monitors) {
        DdcBus bus(monitor.i2cBus);
        std::string error;
        if (bus.open(&error)) {
            if (auto value = bus.getVcp(0x60, &error)) {
                monitor.currentInput = *value;
                continue;
            }
        }
        logMsg("刷新 " + monitor.connector + " 状态失败: " + error);
    }
}

bool matchMonitor(const MonitorInfo& mon, const std::string& targetId) {
    const std::string id = toUpper(trim(targetId));
    if (id.empty()) return false;

    if (id == std::to_string(mon.index1) || id == "#" + std::to_string(mon.index1)) return true;

    if ((id == "PRIMARY" || id == "MAIN") && mon.isPrimary) return true;
    if (id == "SECONDARY" && !mon.isPrimary && mon.index1 == 2) return true;

    const std::string connector = toUpper(mon.connector);
    const std::string drmPath = toUpper(mon.drmPath);
    if (connector == id || connector.find(id) != std::string::npos) return true;
    if (drmPath == id || drmPath.find(id) != std::string::npos) return true;

    const std::string name = toUpper(mon.edidName);
    if (!name.empty() && name.find(id) != std::string::npos) return true;
    const std::string description = toUpper(mon.description);
    if (!description.empty() && description.find(id) != std::string::npos) return true;

    return false;
}

bool switchMonitor(const MonitorInfo& mon, uint16_t code, std::string* error) {
    DdcBus bus(mon.i2cBus);
    std::string openError;
    if (!bus.open(&openError)) {
        if (error) *error = openError;
        return false;
    }
    return bus.setVcp(0x60, code, error);
}

int switchTargetMonitors(const AppConfig& cfg, MonitorContext& ctx, uint16_t code, std::string* error) {
    std::vector<size_t> targets;
    if (cfg.targetMonitor == "all") {
        for (size_t i = 0; i < ctx.monitors.size(); ++i) targets.push_back(i);
    } else {
        for (size_t i = 0; i < ctx.monitors.size(); ++i) {
            if (ctx.monitors[i].isPrimary) {
                targets.push_back(i);
                break;
            }
        }
        if (targets.empty() && !ctx.monitors.empty()) targets.push_back(0);
    }

    if (targets.empty()) {
        if (error) *error = "未检测到支持 DDC/CI 的显示器";
        return 0;
    }

    int successCount = 0;
    std::string lastError;
    for (size_t index : targets) {
        std::string switchError;
        if (switchMonitor(ctx.monitors[index], code, &switchError)) {
            ctx.monitors[index].currentInput = code;
            ++successCount;
        } else {
            lastError = switchError;
            logMsg("切换 " + ctx.monitors[index].connector + " 到 " + formatHexCode(code) +
                   " 失败: " + switchError);
        }
    }

    if (successCount > 0) {
        if (auto it = cfg.commands.find(code); it != cfg.commands.end()) {
            executeShellCommand(it->second);
        }
        if (cfg.notify) {
            notifyDesktop("KVMSwitch 输入切换",
                          "显示器输入源已切换至: " + getInputName(code, &cfg));
        }
    } else if (error) {
        *error = lastError.empty() ? "切换输入源失败，请检查显示器 DDC/CI 设置与线缆连接" : lastError;
    }
    return successCount;
}

int applyPreset(const AppConfig& cfg, MonitorContext& ctx, const MonitorPreset& preset,
                uint16_t detectedTypeCCode, std::string* summary) {
    (void)detectedTypeCCode;
    int applied = 0;
    std::vector<std::string> logs;

    for (auto& monitor : ctx.monitors) {
        uint16_t targetCode = 0;
        for (const auto& target : preset.targets) {
            if (matchMonitor(monitor, target.monitorId)) {
                targetCode = target.inputCode;
                break;
            }
        }
        if (targetCode == 0 || targetCode == monitor.currentInput) continue;

        std::string error;
        if (switchMonitor(monitor, targetCode, &error)) {
            monitor.currentInput = targetCode;
            ++applied;
            logs.push_back("#" + std::to_string(monitor.index1) + " " + monitor.description + ": " +
                           getInputName(targetCode, &cfg));
            if (auto it = cfg.commands.find(targetCode); it != cfg.commands.end()) {
                executeShellCommand(it->second);
            }
        } else {
            logMsg("预设「" + preset.name + "」切换 " + monitor.connector + " 失败: " + error);
        }
    }

    std::string message;
    if (applied > 0) {
        message = "预设「" + preset.name + "」已生效";
        if (!logs.empty()) {
            message += " (";
            for (size_t i = 0; i < logs.size(); ++i) {
                if (i) message += ", ";
                message += logs[i];
            }
            message += ")";
        }
    } else {
        message = "预设「" + preset.name + "」：所有显示器已处于目标状态或无变更。";
    }
    if (summary) *summary = message;
    if (cfg.notify) notifyDesktop("KVMSwitch 预设切换", message);
    return applied;
}

bool toggleInput(const AppConfig& cfg, MonitorContext& ctx, uint16_t detectedTypeCCode,
                 std::string* error) {
    (void)detectedTypeCCode;
    const MonitorInfo* target = nullptr;
    for (const auto& monitor : ctx.monitors) {
        if (monitor.isPrimary) {
            target = &monitor;
            break;
        }
    }
    if (!target && !ctx.monitors.empty()) target = &ctx.monitors.front();
    if (!target) {
        if (error) *error = "未检测到支持 DDC/CI 的显示器";
        return false;
    }

    uint16_t current = target->currentInput;
    DdcBus bus(target->i2cBus);
    std::string openError;
    if (bus.open(&openError)) {
        if (auto value = bus.getVcp(0x60, &openError)) current = *value;
    }
    if (current == 0) current = target->currentInput;

    const std::vector<uint16_t>& sequence = cfg.toggleInputs;
    if (sequence.empty()) {
        if (error) *error = "toggle_inputs 未配置";
        return false;
    }

    uint16_t nextCode = sequence.front();
    auto it = std::find(sequence.begin(), sequence.end(), current);
    if (it != sequence.end()) nextCode = sequence[(static_cast<size_t>(it - sequence.begin()) + 1) % sequence.size()];

    for (auto& monitor : ctx.monitors) {
        if (&monitor == target || monitor.i2cBus == target->i2cBus) {
            std::string switchError;
            if (!switchMonitor(monitor, nextCode, &switchError)) {
                if (error) *error = switchError;
                return false;
            }
            monitor.currentInput = nextCode;
            if (auto cmd = cfg.commands.find(nextCode); cmd != cfg.commands.end()) {
                executeShellCommand(cmd->second);
            }
            if (cfg.notify) {
                notifyDesktop("KVMSwitch 输入轮换",
                              "已切换至: " + getInputName(nextCode, &cfg) + " (" +
                                  formatHexCode(nextCode) + ")");
            }
            return true;
        }
    }

    if (error) *error = "未找到目标显示器";
    return false;
}

bool saveCurrentStateAsPreset(const std::string& cfgPath, MonitorContext& ctx,
                              const std::string& presetName, uint16_t detectedTypeCCode,
                              std::string* error) {
    const std::string name = trim(presetName);
    if (name.empty()) {
        if (error) *error = "预设名称不能为空";
        return false;
    }
    if (ctx.monitors.empty()) {
        if (error) *error = "未检测到外接显示器，无法保存预设";
        return false;
    }

    refreshMonitorStates(ctx);
    MonitorPreset preset;
    preset.name = name;
    for (const auto& monitor : ctx.monitors) {
        preset.targets.push_back({std::to_string(monitor.index1), monitor.currentInput});
    }

    AppConfig cfg = loadConfig(cfgPath, detectedTypeCCode);
    bool replaced = false;
    for (auto& existing : cfg.presets) {
        if (toUpper(existing.name) == toUpper(name)) {
            existing = preset;
            replaced = true;
            break;
        }
    }
    if (!replaced) cfg.presets.push_back(preset);
    if (!saveConfig(cfgPath, cfg, detectedTypeCCode)) {
        if (error) *error = "写入配置文件失败: " + cfgPath;
        return false;
    }
    return true;
}

std::string presetSummary(const MonitorPreset& preset, const AppConfig& cfg) {
    std::ostringstream out;
    out << preset.name << ": ";
    for (size_t i = 0; i < preset.targets.size(); ++i) {
        if (i) out << ", ";
        out << preset.targets[i].monitorId << " -> ";
        out << (preset.targets[i].inputCode == 0 ? "保持不变"
                                                 : getInputName(preset.targets[i].inputCode, &cfg));
    }
    return out.str();
}

} // namespace kvm
