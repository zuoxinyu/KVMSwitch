// config.cpp : INI 配置读写、路径与字符串工具
#include "kvmswitch.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace kvm {

namespace {

const char* kDefaultConfig = R"INI(; =======================================================
; KVMSwitch 显示器输入源快速切换配置文件 (Linux)
; =======================================================

[General]
; 运行模式:
; tray   - 启动后常驻系统托盘，单击托盘图标弹出切换菜单 (推荐)
; menu   - 点击时弹出快捷切换窗口 (单次运行)
; toggle - 点击直接在 toggle_inputs 中轮流切换 (适合作为快捷键/桌面快捷方式)
mode = tray

; 切换成功后是否显示桌面通知 (true / false)
notify = true

; 目标显示器:
; primary - 仅切换主显示器 (推荐)
; all     - 同时切换所有连接的支持 DDC/CI 的显示器
target_monitor = primary

; toggle 轮换模式下循环切换的输入源列表 (逗号分隔)
; 支持名称: DP, HDMI1, HDMI2, TypeC 或具体数值 15, 17, 18, 16
toggle_inputs = DP, HDMI1, TypeC

[Inputs]
; 输入源名称与 VCP 0x60 数值映射 (泰坦军团 P275MV: DP 为 16, Type-C 为 15)
DP = 16
DP1 = 16
HDMI1 = 17
HDMI2 = 18
TypeC = 15

[Commands]
; 切换到指定输入源后自动在后台执行的系统命令 (可选，留空则不执行)
; Linux 下通过 /bin/sh -c 执行，例如:
; on_switch_to_typec = ssh mac caffeinate -u -t 2
on_switch_to_typec =
on_switch_to_hdmi1 =
on_switch_to_hdmi2 =
on_switch_to_dp =

[Presets]
; 多显示器预设方案 (与 Windows / macOS 端保持一致，一键联动切换所有显示器)
; 格式: 预设名称 = 显示器标识:输入源, 显示器标识:输入源 ...
; 显示器标识支持: 1, 2, primary, secondary, 设备名或显示器描述 (如 P275MV)
; 输入源支持: DP, HDMI1, HDMI2, TypeC 或数值代码 (15, 16, 17 等)，0 表示保持不变
; 示例：
; 双屏办公 = 1:DP, 2:TypeC
; 娱乐影音 = 1:HDMI1, 2:HDMI2
)INI";

struct IniSection {
    std::map<std::string, std::string> entries;
    std::vector<std::pair<std::string, std::string>> orderedEntries;
};

std::string readTextFile(const std::string& path, size_t maxBytes = 1024 * 1024) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.size() > maxBytes) data.resize(maxBytes);
    // 去除 UTF-8 BOM (Windows 端配置文件带 BOM)
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF) {
        data.erase(0, 3);
    }
    return data;
}

std::map<std::string, IniSection> parseIni(const std::string& content) {
    std::map<std::string, IniSection> sections;
    std::istringstream stream(content);
    std::string line;
    std::string current;

    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            current = toUpper(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        if (key.empty()) continue;
        sections[current].entries[toUpper(key)] = value;
        sections[current].orderedEntries.emplace_back(key, value);
    }
    return sections;
}

uint16_t parseCodeValue(const std::string& text) {
    try {
        std::string value = trim(text);
        if (startsWith(toUpper(value), "0X")) {
            return static_cast<uint16_t>(std::stoul(value, nullptr, 16));
        }
        return static_cast<uint16_t>(std::stoul(value));
    } catch (...) {
        return 0;
    }
}

std::string ensureParentDirectory(const std::string& path) {
    std::error_code ec;
    fs::path parent = fs::path(path).parent_path();
    if (!parent.empty()) fs::create_directories(parent, ec);
    return ec ? ec.message() : std::string();
}

} // namespace

// ------------------------------------------------------------
// 字符串工具
// ------------------------------------------------------------

std::string trim(const std::string& s) {
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string toUpper(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return result;
}

std::string toLower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::vector<std::string> split(const std::string& s, char delimiter) {
    std::vector<std::string> parts;
    std::string item;
    std::istringstream stream(s);
    while (std::getline(stream, item, delimiter)) {
        item = trim(item);
        if (!item.empty()) parts.push_back(item);
    }
    return parts;
}

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool parseBool(const std::string& s, bool fallback) {
    const std::string value = toUpper(trim(s));
    if (value == "TRUE" || value == "1" || value == "YES" || value == "ON") return true;
    if (value == "FALSE" || value == "0" || value == "NO" || value == "OFF") return false;
    return fallback;
}

// ------------------------------------------------------------
// 路径
// ------------------------------------------------------------

std::string getExePath() {
    char buffer[4096];
    const ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len > 0) {
        buffer[len] = '\0';
        return buffer;
    }
    return "./KVMSwitch";
}

std::string getExeDir() {
    std::error_code ec;
    const fs::path path = fs::path(getExePath()).parent_path();
    return path.empty() ? "." : path.string();
}

std::string getConfigPath() {
    // 测试 / 特殊部署可用环境变量覆盖
    if (const char* env = std::getenv("KVMSWITCH_CONFIG"); env && *env) {
        std::error_code ec;
        const fs::path p = fs::absolute(fs::path(env), ec);
        return ec ? std::string(env) : p.string();
    }

    std::error_code ec;
    const fs::path exeDir = fs::path(getExeDir());
    const fs::path portable = exeDir / "config.ini";
    if (fs::exists(portable, ec)) return portable.string();

    // 便携优先：exe 所在目录可写时把配置放在程序旁
    if (::access(exeDir.c_str(), W_OK) == 0) return portable.string();

    // 否则回退到用户配置目录 (安装到 /usr/bin 等只读位置时)
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    fs::path base;
    if (xdg && *xdg) base = xdg;
    else if (home && *home) base = fs::path(home) / ".config";
    else base = "/tmp";
    return (base / "kvmswitch" / "config.ini").string();
}

std::string getRuntimeDir() {
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    fs::path base;
    if (xdg && *xdg) {
        base = xdg;
    } else {
        base = fs::path("/tmp") / ("kvmswitch-" + std::to_string(::getuid()));
    }
    std::error_code ec;
    fs::create_directories(base, ec);
    if (!ec) ::chmod(base.c_str(), 0700);
    return base.string();
}

std::string getCacheDir() {
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    fs::path base;
    if (xdg && *xdg) base = xdg;
    else if (home && *home) base = fs::path(home) / ".cache";
    else base = fs::path("/tmp") / ("kvmswitch-cache-" + std::to_string(::getuid()));
    const fs::path path = base / "kvmswitch";
    std::error_code ec;
    fs::create_directories(path, ec);
    return path.string();
}

std::string getIconPath() {
    if (const char* env = std::getenv("KVMSWITCH_ICON"); env && *env) {
        if (::access(env, R_OK) == 0) return env;
    }
    std::error_code ec;
    const fs::path exeDir = fs::path(getExeDir());
    const std::vector<fs::path> candidates = {
        exeDir / "KVMSwitch.png",
        exeDir / "kvmswitch.png",
        exeDir.parent_path() / "KVMSwitch.png",
        "/usr/share/icons/hicolor/256x256/apps/kvmswitch.png",
        "/usr/share/pixmaps/kvmswitch.png",
    };
    for (const auto& path : candidates) {
        if (fs::exists(path, ec)) return path.string();
    }
    return {};
}

// ------------------------------------------------------------
// 配置文件
// ------------------------------------------------------------

void ensureConfigFile(const std::string& cfgPath) {
    std::error_code ec;
    if (fs::exists(cfgPath, ec)) return;
    ensureParentDirectory(cfgPath);

    const int fd = ::open(cfgPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        if (errno == EEXIST) return;
        // 目录只读等情况静默失败，调用方会使用内存默认值
        return;
    }
    const std::string content(kDefaultConfig);
    size_t written = 0;
    while (written < content.size()) {
        const ssize_t n = ::write(fd, content.data() + written, content.size() - written);
        if (n <= 0) break;
        written += static_cast<size_t>(n);
    }
    ::close(fd);
}

AppConfig loadConfig(const std::string& cfgPath, uint16_t detectedTypeCCode) {
    ensureConfigFile(cfgPath);

    AppConfig cfg;
    cfg.mode = "tray";
    cfg.notify = true;
    cfg.targetMonitor = "primary";
    cfg.toggleInputs = {16, 17, detectedTypeCCode};

    const std::string content = readTextFile(cfgPath);
    if (content.empty()) return cfg;

    const auto sections = parseIni(content);

    auto getSection = [&](const char* name) -> const IniSection& {
        static const IniSection empty;
        auto it = sections.find(name);
        return it == sections.end() ? empty : it->second;
    };

    // [General]
    const auto& general = getSection("GENERAL").entries;
    if (auto it = general.find("MODE"); it != general.end()) {
        const std::string mode = toUpper(it->second);
        if (mode == "TRAY" || mode == "MENU" || mode == "TOGGLE") cfg.mode = toLower(mode);
        else cfg.mode = "tray";
    }
    if (auto it = general.find("NOTIFY"); it != general.end()) cfg.notify = parseBool(it->second, true);
    if (auto it = general.find("TARGET_MONITOR"); it != general.end()) {
        const std::string target = toUpper(it->second);
        cfg.targetMonitor = (target == "ALL") ? "all" : "primary";
    }

    // [Inputs] 自定义输入源映射（保留键的原始大小写用于展示）
    for (const auto& kv : getSection("INPUTS").orderedEntries) {
        const uint16_t code = parseCodeValue(kv.second);
        if (code > 0) cfg.customInputs[kv.first] = code;
    }

    // [General] toggle_inputs
    std::string toggleStr = "DP, HDMI1, TypeC";
    if (auto it = general.find("TOGGLE_INPUTS"); it != general.end()) toggleStr = it->second;
    cfg.toggleInputs.clear();
    for (const auto& item : split(toggleStr, ',')) {
        const uint16_t code = parseInputAlias(item, cfg, detectedTypeCCode);
        if (code > 0 && std::find(cfg.toggleInputs.begin(), cfg.toggleInputs.end(), code) == cfg.toggleInputs.end()) {
            cfg.toggleInputs.push_back(code);
        }
    }
    if (cfg.toggleInputs.empty()) cfg.toggleInputs = {16, 17, detectedTypeCCode};

    // [Commands]
    uint16_t dpCode = 16;
    for (const auto& kv : cfg.customInputs) {
        const std::string key = toUpper(kv.first);
        if (key == "DP" || key == "DP1") {
            dpCode = kv.second;
            break;
        }
    }
    uint16_t tcCode = detectedTypeCCode;
    for (const auto& kv : cfg.customInputs) {
        const std::string key = toUpper(kv.first);
        if (key == "TYPEC" || key == "TYPE-C" || key == "USBC" || key == "USB-C") {
            tcCode = kv.second;
            break;
        }
    }

    const auto& cmdSec = getSection("COMMANDS").entries;
    const std::vector<std::pair<std::string, uint16_t>> aliasMap = {
        {"ON_SWITCH_TO_DP", dpCode},
        {"ON_SWITCH_TO_DP1", dpCode},
        {"ON_SWITCH_TO_TYPEC", tcCode},
        {"ON_SWITCH_TO_USBC", tcCode},
        {"ON_SWITCH_TO_HDMI1", 17},
        {"ON_SWITCH_TO_HDMI2", 18},
        {"ON_SWITCH_TO_HDMI3", 19},
    };
    for (const auto& item : aliasMap) {
        if (auto it = cmdSec.find(item.first); it != cmdSec.end() && !trim(it->second).empty()) {
            cfg.commands[item.second] = it->second;
        }
    }

    // [Presets]
    for (const auto& kv : getSection("PRESETS").orderedEntries) {
        const std::string presetName = trim(kv.first);
        const std::string valueList = trim(kv.second);
        if (presetName.empty() || valueList.empty()) continue;

        MonitorPreset preset;
        preset.name = presetName;
        int autoIndex = 1;
        for (const auto& entry : split(valueList, ',')) {
            PresetTarget target;
            const size_t colon = entry.find(':');
            if (colon != std::string::npos) {
                target.monitorId = trim(entry.substr(0, colon));
                target.inputCode = parseInputAlias(trim(entry.substr(colon + 1)), cfg, detectedTypeCCode);
            } else {
                target.monitorId = std::to_string(autoIndex++);
                target.inputCode = parseInputAlias(entry, cfg, detectedTypeCCode);
            }
            preset.targets.push_back(target);
        }
        if (!preset.targets.empty()) cfg.presets.push_back(preset);
    }

    return cfg;
}

bool saveConfig(const std::string& cfgPath, const AppConfig& cfg, uint16_t detectedTypeCCode) {
    std::ostringstream out;
    out << "; =======================================================\n"
        << "; KVMSwitch 显示器输入源快速切换配置文件 (Linux)\n"
        << "; =======================================================\n\n";

    out << "[General]\n"
        << "; 运行模式: tray (常驻托盘) / menu (快捷窗口) / toggle (一键轮换)\n"
        << "mode = " << toLower(cfg.mode) << "\n\n"
        << "; 切换成功后是否显示桌面通知 (true / false)\n"
        << "notify = " << (cfg.notify ? "true" : "false") << "\n\n"
        << "; 目标显示器: primary (仅主显示器) / all (所有显示器)\n"
        << "target_monitor = " << toLower(cfg.targetMonitor) << "\n\n"
        << "; toggle 轮换模式下循环切换的输入源列表 (逗号分隔)\n"
        << "toggle_inputs = ";
    for (size_t i = 0; i < cfg.toggleInputs.size(); ++i) {
        if (i) out << ", ";
        out << getInputAliasForCode(cfg.toggleInputs[i], detectedTypeCCode);
    }
    out << "\n\n";

    out << "[Inputs]\n"
        << "; 输入源名称与 VCP 0x60 数值映射 (泰坦军团 P275MV: DP 为 16, Type-C 为 15)\n";
    if (!cfg.customInputs.empty()) {
        for (const auto& kv : cfg.customInputs) out << kv.first << " = " << kv.second << "\n";
    } else {
        out << "DP = 16\nDP1 = 16\nHDMI1 = 17\nHDMI2 = 18\nTypeC = " << detectedTypeCCode << "\n";
    }
    out << "\n";

    out << "[Presets]\n"
        << "; 多显示器预设方案 (与 Windows / macOS 端保持一致，一键联动切换所有显示器)\n"
        << "; 格式: 预设名称 = 显示器标识:输入源, 显示器标识:输入源 ...\n"
        << "; 显示器标识支持: 1, 2, primary, secondary, 设备名或显示器描述\n"
        << "; 输入源支持: DP, HDMI1, HDMI2, TypeC 或数值代码 (15, 16, 17 等)，0 表示保持不变\n";
    for (const auto& preset : cfg.presets) {
        if (trim(preset.name).empty()) continue;
        out << preset.name << " = ";
        for (size_t i = 0; i < preset.targets.size(); ++i) {
            if (i) out << ", ";
            out << preset.targets[i].monitorId << ":";
            if (preset.targets[i].inputCode == 0) out << "0";
            else out << getInputAliasForCode(preset.targets[i].inputCode, detectedTypeCCode);
        }
        out << "\n";
    }
    out << "\n";

    uint16_t dpCode = 16;
    for (const auto& kv : cfg.customInputs) {
        const std::string key = toUpper(kv.first);
        if (key == "DP" || key == "DP1") {
            dpCode = kv.second;
            break;
        }
    }
    uint16_t tcCode = detectedTypeCCode;
    for (const auto& kv : cfg.customInputs) {
        const std::string key = toUpper(kv.first);
        if (key == "TYPEC" || key == "TYPE-C" || key == "USBC" || key == "USB-C") {
            tcCode = kv.second;
            break;
        }
    }
    auto commandFor = [&](uint16_t code) -> std::string {
        auto it = cfg.commands.find(code);
        return it == cfg.commands.end() ? std::string() : it->second;
    };

    out << "[Commands]\n"
        << "; 切换到指定输入源后自动在后台执行的系统命令 (可选，留空则不执行)\n"
        << "; Linux 下通过 /bin/sh -c 执行\n"
        << "on_switch_to_typec = " << commandFor(tcCode) << "\n"
        << "on_switch_to_hdmi1 = " << commandFor(17) << "\n"
        << "on_switch_to_hdmi2 = " << commandFor(18) << "\n"
        << "on_switch_to_dp = " << commandFor(dpCode) << "\n";

    ensureParentDirectory(cfgPath);
    const std::string tempPath = cfgPath + ".tmp." + std::to_string(::getpid());
    {
        std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file << out.str();
        if (!file.good()) {
            file.close();
            ::unlink(tempPath.c_str());
            return false;
        }
    }
    ::chmod(tempPath.c_str(), 0644);
    if (::rename(tempPath.c_str(), cfgPath.c_str()) != 0) {
        ::unlink(tempPath.c_str());
        return false;
    }
    return true;
}

// ------------------------------------------------------------
// 输入源名称 / 别名
// ------------------------------------------------------------

uint16_t parseInputAlias(const std::string& text, const AppConfig& cfg, uint16_t detectedTypeCCode) {
    const std::string upper = toUpper(trim(text));
    if (upper.empty()) return 0;

    for (const auto& kv : cfg.customInputs) {
        if (toUpper(kv.first) == upper) return kv.second;
    }

    try {
        if (startsWith(upper, "0X")) return static_cast<uint16_t>(std::stoul(upper, nullptr, 16));
        if (std::all_of(upper.begin(), upper.end(), [](unsigned char c) { return std::isdigit(c); })) {
            return static_cast<uint16_t>(std::stoul(upper));
        }
    } catch (...) {
    }

    if (upper == "DP" || upper == "DP1" || upper == "DISPLAYPORT" || upper == "DISPLAYPORT1") return 16;
    if (upper == "DP2" || upper == "DISPLAYPORT2") return 15;
    if (upper == "HDMI" || upper == "HDMI1") return 17;
    if (upper == "HDMI2") return 18;
    if (upper == "HDMI3") return 19;
    if (upper == "HDMI4") return 20;
    if (upper == "TYPEC" || upper == "TYPE-C" || upper == "USBC" || upper == "USB-C") return detectedTypeCCode;
    if (upper == "DVI" || upper == "DVI1") return 3;
    if (upper == "VGA" || upper == "VGA1") return 1;
    return 0;
}

std::string getInputName(uint16_t code, const AppConfig* cfg) {
    if (cfg) {
        for (const auto& kv : cfg->customInputs) {
            if (kv.second != code) continue;
            const std::string upper = toUpper(kv.first);
            if (upper == "DP" || upper == "DP1") return "DisplayPort (DP)";
            if (upper == "DP2") return "DisplayPort 2";
            if (upper == "TYPEC" || upper == "TYPE-C" || upper == "USBC" || upper == "USB-C") return "USB Type-C";
            if (upper == "HDMI1" || upper == "HDMI") return "HDMI 1";
            if (upper == "HDMI2") return "HDMI 2";
            if (upper == "HDMI3") return "HDMI 3";
            return kv.first;
        }
    }
    switch (code) {
    case 16: return "DisplayPort (DP)";
    case 15: return "USB Type-C";
    case 17: return "HDMI 1";
    case 18: return "HDMI 2";
    case 19: return "HDMI 3";
    case 20: return "HDMI 4";
    case 27: return "USB Type-C";
    case 25: return "USB Type-C";
    case 3: return "DVI";
    case 1: return "VGA";
    default: return getInputHex(code);
    }
}

std::string getInputAliasForCode(uint16_t code, uint16_t detectedTypeCCode) {
    if (code == 16) return "DP";
    if (code == detectedTypeCCode) return "TypeC";
    if (code == 17) return "HDMI1";
    if (code == 18) return "HDMI2";
    if (code == 19) return "HDMI3";
    if (code == 20) return "HDMI4";
    return std::to_string(code);
}

std::string getInputHex(uint16_t code) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "输入源 (0x%02X)", code);
    return buffer;
}

std::string formatHexCode(uint16_t code) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "0x%02X", code);
    return buffer;
}

} // namespace kvm
