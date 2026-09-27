// KVMSwitch.cpp : 便携式显示器输入源快速切换程序 (DDC/CI)
// 支持通过快捷菜单、一键轮换、桌面快捷方式或命令行在 HDMI / DP / Type-C 间快速切换

#include "framework.h"
#include "KVMSwitch.h"
#include <lowlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <algorithm>
#include <cwctype>

#include <commctrl.h>

struct MonitorContext;
struct AppConfig;
void ShowQueryInfo(const MonitorContext& ctx, const AppConfig* pCfg);
void DoToggle(HINSTANCE hInstance, const AppConfig& cfg, MonitorContext& ctx);
bool ShowSettingsDialog(HINSTANCE hInstance, HWND hParent, AppConfig& cfg, MonitorContext& ctx, DWORD detectedTypeCCode);

#pragma comment(lib, "dxva2.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// Menu / Action IDs
#define ID_INPUT_BASE          2000
#define ID_ACTION_TOGGLE       3001
#define ID_ACTION_SHORTCUTS    3002
#define ID_ACTION_CONFIG       3003
#define ID_ACTION_QUERY        3004
#define ID_ACTION_EXIT         3005
#define ID_ACTION_AUTOSTART    3006
#define ID_ACTION_SAVE_PRESET  3007
#define ID_ACTION_SETTINGS     3008

#define ID_PRESET_BASE         4000
#define ID_MONITOR_INPUT_BASE  5000 // 用于多显示器独立子菜单 (monitorIdx * 100 + inputIdx)

// Settings Dialog Control IDs
#define IDC_SETTINGS_TAB           6001
#define IDC_SETTINGS_BTN_OPEN_INI  6002
#define IDC_PRESET_LIST            6101
#define IDC_PRESET_ADD             6102
#define IDC_PRESET_DEL             6103
#define IDC_PRESET_UP              6104
#define IDC_PRESET_DOWN            6105
#define IDC_PRESET_NAME_EDIT       6106
#define IDC_PRESET_MON_BASE        6200
#define IDC_GENERAL_NOTIFY         6301
#define IDC_GENERAL_AUTOSTART      6302
#define IDC_GENERAL_MODE           6303
#define IDC_GENERAL_TARGET         6304
#define IDC_CMD_DP                 6305
#define IDC_CMD_TYPEC              6306
#define IDC_CMD_HDMI1              6307
#define IDC_CMD_HDMI2              6308

#define TIMER_ID_REFRESH    1001
#define WM_TRAYNOTIFY       (WM_USER + 101)

// 全局托盘相关变量
static NOTIFYICONDATAW g_trayNid = { sizeof(NOTIFYICONDATAW) };
static HWND g_hTrayWnd = NULL;
static HWND g_hSettingsDlg = NULL;
static HINSTANCE g_hInstance = NULL;
static UINT g_wmTaskbarCreated = 0;
static UINT g_wmShowMenu = 0;
static UINT g_wmRefresh = 0;
static UINT g_wmShowSettings = 0;

// 数据结构
struct PhysicalMonEntry {
    HMONITOR hMonitor;
    HANDLE hPhysicalMonitor;
    std::wstring description;
    std::wstring deviceName;
    bool isPrimary;
    DWORD currentInput;
    std::vector<DWORD> supportedInputs;
};

struct MonitorContext {
    std::vector<PhysicalMonEntry> monitors;
    std::vector<PHYSICAL_MONITOR> rawPhysicalMonitors;
    std::vector<std::pair<HMONITOR, DWORD>> monitorPhysicalCounts;
};

struct InputItem {
    DWORD code;
    std::wstring name;
    std::wstring alias;
};

// 多显示器预设目标项 (对应单个显示器的目标输入源)
struct PresetTarget {
    std::wstring monitorId; // "1", "2", "primary", "secondary", "\\.\DISPLAY1", 或显示器型号/描述
    DWORD inputCode;        // 目标 VCP 60 代码 (0 表示保持不变)
};

// 多显示器预设方案 (与 macOS 端 Preset 对齐)
struct MonitorPreset {
    std::wstring name;
    std::vector<PresetTarget> targets;
};

struct AppConfig {
    std::wstring mode;          // "tray", "menu" or "toggle"
    bool notify;                // 是否显示桌面切换提示
    std::wstring targetMonitor; // "primary" or "all"
    std::vector<DWORD> toggleInputs;
    std::map<std::wstring, DWORD> customInputs;
    std::map<DWORD, std::wstring> commands; // 切换后触发的命令
    std::vector<MonitorPreset> presets;     // 多显示器预设方案列表
};

// 工具函数
std::wstring Trim(const std::wstring& s) {
    auto start = s.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos) return L"";
    auto end = s.find_last_not_of(L" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::wstring ToUpper(const std::wstring& s) {
    std::wstring res = s;
    std::transform(res.begin(), res.end(), res.begin(), ::towupper);
    return res;
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t delim) {
    std::vector<std::wstring> tokens;
    std::wstringstream ss(s);
    std::wstring item;
    while (std::getline(ss, item, delim)) {
        item = Trim(item);
        if (!item.empty()) {
            tokens.push_back(item);
        }
    }
    return tokens;
}

std::wstring GetExePath() {
    wchar_t szPath[MAX_PATH] = { 0 };
    GetModuleFileNameW(NULL, szPath, MAX_PATH);
    return szPath;
}

std::wstring GetExeDir() {
    std::wstring path = GetExePath();
    size_t pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        return path.substr(0, pos);
    }
    return L".";
}

std::wstring GetConfigPath() {
    return GetExeDir() + L"\\config.ini";
}

// Windows 开机自启动管理 (注册表 HKCU\...\Run)
const wchar_t* REG_RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* REG_APP_NAME = L"KVMSwitch";

bool IsAutoStartEnabled() {
    HKEY hKey = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t val[MAX_PATH * 2] = { 0 };
        DWORD size = sizeof(val);
        DWORD type = 0;
        LONG res = RegQueryValueExW(hKey, REG_APP_NAME, NULL, &type, reinterpret_cast<LPBYTE>(val), &size);
        RegCloseKey(hKey);
        return (res == ERROR_SUCCESS);
    }
    return false;
}

bool SetAutoStart(bool enable) {
    HKEY hKey = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        LONG res = 0;
        if (enable) {
            std::wstring exePath = GetExePath();
            std::wstring cmd = L"\"" + exePath + L"\" --tray";
            res = RegSetValueExW(hKey, REG_APP_NAME, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(cmd.c_str()),
                static_cast<DWORD>((cmd.length() + 1) * sizeof(wchar_t)));
        } else {
            res = RegDeleteValueW(hKey, REG_APP_NAME);
        }
        RegCloseKey(hKey);
        return (res == ERROR_SUCCESS || (!enable && res == ERROR_FILE_NOT_FOUND));
    }
    return false;
}

// 解析输入源代码与名称
std::wstring GetInputName(DWORD code, const AppConfig* pCfg = nullptr) {
    if (pCfg) {
        for (const auto& kv : pCfg->customInputs) {
            if (kv.second == code) {
                std::wstring upper = ToUpper(kv.first);
                if (upper == L"DP" || upper == L"DP1") return L"DisplayPort (DP)";
                if (upper == L"DP2") return L"DisplayPort 2";
                if (upper == L"TYPEC" || upper == L"TYPE-C" || upper == L"USBC" || upper == L"USB-C") return L"USB Type-C";
                if (upper == L"HDMI1" || upper == L"HDMI") return L"HDMI 1";
                if (upper == L"HDMI2") return L"HDMI 2";
                if (upper == L"HDMI3") return L"HDMI 3";
                return kv.first;
            }
        }
    }
    switch (code) {
    case 16: return L"DisplayPort (DP)"; // 泰坦军团 P275MV DP 代码为 16 (0x10)
    case 15: return L"USB Type-C";       // 泰坦军团 P275MV Type-C 代码为 15 (0x0F)
    case 17: return L"HDMI 1";
    case 18: return L"HDMI 2";
    case 19: return L"HDMI 3";
    case 20: return L"HDMI 4";
    case 27: return L"USB Type-C";       // VESA MCCS 0x1B 标准
    case 25: return L"USB Type-C";       // 部分厂商使用
    case 3:  return L"DVI";
    case 1:  return L"VGA";
    default: {
        wchar_t buf[32];
        swprintf_s(buf, L"输入源 (0x%02X)", code);
        return buf;
    }
    }
}

DWORD ParseInputAlias(const std::wstring& text, const AppConfig& cfg, DWORD detectedTypeCCode = 15) {
    std::wstring upper = ToUpper(Trim(text));
    if (upper.empty()) return 0;

    // 自定义映射优先
    for (const auto& kv : cfg.customInputs) {
        if (ToUpper(kv.first) == upper) {
            return kv.second;
        }
    }

    // 检查是否为十六进制 (0x...) 或纯数字
    try {
        if (upper.rfind(L"0X", 0) == 0) {
            return std::stoul(upper, nullptr, 16);
        }
        if (std::all_of(upper.begin(), upper.end(), ::iswdigit)) {
            return std::stoul(upper);
        }
    } catch (...) {}

    // 内置常见别名 (DP为16, TypeC为15)
    if (upper == L"DP" || upper == L"DP1" || upper == L"DISPLAYPORT" || upper == L"DISPLAYPORT1") return 16;
    if (upper == L"DP2" || upper == L"DISPLAYPORT2") return 15;
    if (upper == L"HDMI" || upper == L"HDMI1") return 17;
    if (upper == L"HDMI2") return 18;
    if (upper == L"HDMI3") return 19;
    if (upper == L"HDMI4") return 20;
    if (upper == L"TYPEC" || upper == L"TYPE-C" || upper == L"USBC" || upper == L"USB-C") return detectedTypeCCode;
    if (upper == L"DVI" || upper == L"DVI1") return 3;
    if (upper == L"VGA" || upper == L"VGA1") return 1;

    return 0;
}

// 配置文件初始化与读取
void EnsureConfigFile(const std::wstring& cfgPath) {
    DWORD attr = GetFileAttributesW(cfgPath.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) return;

    // 写入默认配置文件
    const wchar_t* defaultIni =
        L"; =======================================================\r\n"
        L"; KVMSwitch 显示器输入源快速切换配置文件\r\n"
        L"; =======================================================\r\n"
        L"\r\n"
        L"[General]\r\n"
        L"; 运行模式:\r\n"
        L"; tray   - 常驻系统托盘图标 (推荐，随时在任务栏右下角切换、监控状态)\r\n"
        L"; menu   - 单次运行弹出快捷选择菜单后退出\r\n"
        L"; toggle - 单次运行直接在 toggle_inputs 中轮流切换后退出\r\n"
        L"mode = tray\r\n"
        L"\r\n"
        L"; 切换成功后是否显示桌面气泡通知 (true / false)\r\n"
        L"notify = true\r\n"
        L"\r\n"
        L"; 目标显示器:\r\n"
        L"; primary - 仅切换主显示器 (推荐)\r\n"
        L"; all     - 同时切换所有连接的支持 DDC/CI 的显示器\r\n"
        L"target_monitor = primary\r\n"
        L"\r\n"
        L"; toggle 轮换模式下循环切换的输入源列表 (逗号分隔)\r\n"
        L"; 支持名称: DP, HDMI1, HDMI2, TypeC 或具体数值 15, 17, 18, 16\r\n"
        L"toggle_inputs = DP, HDMI1, TypeC\r\n"
        L"\r\n"
        L"[Inputs]\r\n"
        L"; 输入源名称与 VCP 0x60 数值映射 (泰坦军团 P275MV: DP 为 16, Type-C 为 15)\r\n"
        L"DP = 16\r\n"
        L"DP1 = 16\r\n"
        L"HDMI1 = 17\r\n"
        L"HDMI2 = 18\r\n"
        L"TypeC = 15\r\n"
        L"\r\n"
        L"[Presets]\r\n"
        L"; 多显示器预设方案 (与 macOS 端预设保持一致，一键联动切换所有显示器)\r\n"
        L"; 格式: 预设名称 = 显示器标识:输入源, 显示器标识:输入源 ...\r\n"
        L"; 显示器标识支持: 1, 2, primary, secondary, 设备名或显示器描述\r\n"
        L"; 输入源支持: DP, HDMI1, HDMI2, TypeC 或数值代码 (15, 16, 17 等)，0 表示保持不变\r\n"
        L"; 示例：\r\n"
        L"; 办公模式 = 1:DP, 2:TypeC\r\n"
        L"; 娱乐模式 = 1:HDMI1, 2:HDMI2\r\n"
        L"\r\n"
        L"[Commands]\r\n"
        L"; 切换到指定输入源后自动在后台执行的系统命令 (可选，留空则不执行)\r\n"
        L"; 例如切换到 Mac 时唤醒 Mac，切换回 PC 时联动等:\r\n"
        L"on_switch_to_typec = ssh mac caffeinate -u -t 2\r\n"
        L"on_switch_to_hdmi1 = \r\n"
        L"on_switch_to_hdmi2 = \r\n"
        L"on_switch_to_dp = \r\n";

    HANDLE hFile = CreateFileW(cfgPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        // 写入 UTF-8 BOM
        const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
        DWORD written = 0;
        WriteFile(hFile, bom, sizeof(bom), &written, NULL);

        // 转换为 UTF-8
        int len = WideCharToMultiByte(CP_UTF8, 0, defaultIni, -1, NULL, 0, NULL, NULL);
        if (len > 0) {
            std::vector<char> utf8(len);
            WideCharToMultiByte(CP_UTF8, 0, defaultIni, -1, utf8.data(), len, NULL, NULL);
            WriteFile(hFile, utf8.data(), len - 1, &written, NULL);
        }
        CloseHandle(hFile);
    }
}

// 以正确的编码 (UTF-8/UTF-16/ANSI) 读取整个文本文件为宽字符串
std::wstring ReadFileAsWideString(const std::wstring& filePath) {
    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return L"";

    DWORD size = GetFileSize(hFile, NULL);
    if (size == 0 || size == INVALID_FILE_SIZE) {
        CloseHandle(hFile);
        return L"";
    }

    std::vector<unsigned char> data(size);
    DWORD read = 0;
    ReadFile(hFile, data.data(), size, &read, NULL);
    CloseHandle(hFile);

    if (read == 0) return L"";

    // 检查 UTF-16 LE BOM (FF FE)
    if (read >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
        size_t wchars = (read - 2) / 2;
        return std::wstring(reinterpret_cast<const wchar_t*>(data.data() + 2), wchars);
    }

    const char* utf8Data = reinterpret_cast<const char*>(data.data());
    int utf8Len = static_cast<int>(read);
    // 检查 UTF-8 BOM (EF BB BF)
    if (read >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        utf8Data += 3;
        utf8Len -= 3;
    }

    // 优先尝试按 UTF-8 解码
    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Data, utf8Len, NULL, 0);
    if (wlen > 0) {
        std::wstring res(wlen, 0);
        MultiByteToWideChar(CP_UTF8, 0, utf8Data, utf8Len, &res[0], wlen);
        return res;
    }

    // 若不是合法 UTF-8，回退到系统默认 ANSI 代码页 (如 GBK)
    wlen = MultiByteToWideChar(CP_ACP, 0, utf8Data, utf8Len, NULL, 0);
    if (wlen > 0) {
        std::wstring res(wlen, 0);
        MultiByteToWideChar(CP_ACP, 0, utf8Data, utf8Len, &res[0], wlen);
        return res;
    }

    return L"";
}

struct IniSection {
    std::map<std::wstring, std::wstring> entries;
    std::vector<std::pair<std::wstring, std::wstring>> orderedEntries;
};

// 解析 INI 文件内容
std::map<std::wstring, IniSection> ParseIniContent(const std::wstring& content) {
    std::map<std::wstring, IniSection> sections;
    std::wstringstream ss(content);
    std::wstring line;
    std::wstring curSec = L"";

    while (std::getline(ss, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;

        if (line.front() == L'[' && line.back() == L']') {
            curSec = ToUpper(Trim(line.substr(1, line.length() - 2)));
            continue;
        }

        size_t eq = line.find(L'=');
        if (eq != std::wstring::npos) {
            std::wstring key = Trim(line.substr(0, eq));
            std::wstring val = Trim(line.substr(eq + 1));
            sections[curSec].entries[ToUpper(key)] = val;
            sections[curSec].orderedEntries.push_back({ key, val });
        }
    }
    return sections;
}

AppConfig LoadConfig(const std::wstring& cfgPath, DWORD detectedTypeCCode = 15) {
    EnsureConfigFile(cfgPath);

    AppConfig cfg;
    std::wstring content = ReadFileAsWideString(cfgPath);
    if (content.empty()) {
        cfg.mode = L"TRAY";
        cfg.notify = true;
        cfg.targetMonitor = L"PRIMARY";
        cfg.toggleInputs = { 16, 17, detectedTypeCCode };
        return cfg;
    }

    auto sections = ParseIniContent(content);

    // General 节
    auto& gen = sections[L"GENERAL"].entries;
    if (gen.count(L"MODE")) {
        cfg.mode = ToUpper(gen[L"MODE"]);
        if (cfg.mode != L"TOGGLE" && cfg.mode != L"MENU" && cfg.mode != L"TRAY") {
            cfg.mode = L"TRAY";
        }
    } else {
        cfg.mode = L"TRAY";
    }

    if (gen.count(L"NOTIFY")) {
        std::wstring notifyStr = ToUpper(gen[L"NOTIFY"]);
        cfg.notify = (notifyStr == L"TRUE" || notifyStr == L"1" || notifyStr == L"YES");
    } else {
        cfg.notify = true;
    }

    if (gen.count(L"TARGET_MONITOR")) {
        cfg.targetMonitor = ToUpper(gen[L"TARGET_MONITOR"]);
    } else {
        cfg.targetMonitor = L"PRIMARY";
    }

    // Custom Inputs
    for (const auto& kv : sections[L"INPUTS"].orderedEntries) {
        try {
            DWORD code = (kv.second.rfind(L"0X", 0) == 0 || kv.second.rfind(L"0x", 0) == 0) ?
                std::stoul(kv.second, nullptr, 16) : std::stoul(kv.second);
            cfg.customInputs[kv.first] = code;
        } catch (...) {}
    }

    // Toggle Inputs
    std::wstring toggleStr = L"DP, HDMI1, TypeC";
    if (gen.count(L"TOGGLE_INPUTS")) {
        toggleStr = gen[L"TOGGLE_INPUTS"];
    }
    std::vector<std::wstring> parts = Split(toggleStr, L',');
    for (const auto& item : parts) {
        DWORD c = ParseInputAlias(item, cfg, detectedTypeCCode);
        if (c > 0 && std::find(cfg.toggleInputs.begin(), cfg.toggleInputs.end(), c) == cfg.toggleInputs.end()) {
            cfg.toggleInputs.push_back(c);
        }
    }
    if (cfg.toggleInputs.empty()) {
        cfg.toggleInputs = { 16, 17, detectedTypeCCode };
    }

    // Commands
    DWORD dpCode = 16;
    for (const auto& kv : cfg.customInputs) {
        if (ToUpper(kv.first) == L"DP" || ToUpper(kv.first) == L"DP1") {
            dpCode = kv.second;
            break;
        }
    }
    DWORD tcCode = detectedTypeCCode;
    for (const auto& kv : cfg.customInputs) {
        if (ToUpper(kv.first) == L"TYPEC" || ToUpper(kv.first) == L"TYPE-C" || ToUpper(kv.first) == L"USBC") {
            tcCode = kv.second;
            break;
        }
    }

    auto& cmdSec = sections[L"COMMANDS"].entries;
    std::vector<std::pair<std::wstring, DWORD>> aliasMap = {
        { L"ON_SWITCH_TO_DP", dpCode },
        { L"ON_SWITCH_TO_DP1", dpCode },
        { L"ON_SWITCH_TO_TYPEC", tcCode },
        { L"ON_SWITCH_TO_USBC", tcCode },
        { L"ON_SWITCH_TO_HDMI1", 17 },
        { L"ON_SWITCH_TO_HDMI2", 18 },
        { L"ON_SWITCH_TO_HDMI3", 19 },
    };
    for (const auto& item : aliasMap) {
        if (cmdSec.count(item.first) && !cmdSec[item.first].empty()) {
            cfg.commands[item.second] = cmdSec[item.first];
        }
    }

    // Presets 多显示器预设解析 (保留原始中文名称和顺序)
    for (const auto& kv : sections[L"PRESETS"].orderedEntries) {
        std::wstring presetName = Trim(kv.first);
        std::wstring valList = Trim(kv.second);
        if (!presetName.empty() && !valList.empty()) {
            MonitorPreset preset;
            preset.name = presetName;
            std::vector<std::wstring> entries = Split(valList, L',');
            int autoIdx = 1;
            for (const auto& entry : entries) {
                size_t colon = entry.find(L':');
                PresetTarget pt;
                if (colon != std::wstring::npos) {
                    pt.monitorId = Trim(entry.substr(0, colon));
                    std::wstring codeStr = Trim(entry.substr(colon + 1));
                    pt.inputCode = ParseInputAlias(codeStr, cfg, detectedTypeCCode);
                } else {
                    pt.monitorId = std::to_wstring(autoIdx++);
                    pt.inputCode = ParseInputAlias(entry, cfg, detectedTypeCCode);
                }
                preset.targets.push_back(pt);
            }
            if (!preset.targets.empty()) {
                cfg.presets.push_back(preset);
            }
        }
    }

    return cfg;
}

// 写入 UTF-8 带 BOM 编码文本文件
bool WriteFileAsUtf8WithBom(const std::wstring& filePath, const std::wstring& text) {
    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
    DWORD written = 0;
    WriteFile(hFile, bom, sizeof(bom), &written, NULL);

    int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.length(), NULL, 0, NULL, NULL);
    if (len > 0) {
        std::vector<char> utf8(len);
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.length(), utf8.data(), len, NULL, NULL);
        WriteFile(hFile, utf8.data(), len, &written, NULL);
    }
    CloseHandle(hFile);
    return true;
}

std::wstring GetInputAliasForCode(DWORD code, DWORD detectedTypeCCode = 15) {
    if (code == 16) return L"DP";
    if (code == detectedTypeCCode) return L"TypeC";
    if (code == 17) return L"HDMI1";
    if (code == 18) return L"HDMI2";
    if (code == 19) return L"HDMI3";
    if (code == 20) return L"HDMI4";
    return std::to_wstring(code);
}

// 将 AppConfig 保存至 INI 配置文件
bool SaveConfigToFile(const std::wstring& cfgPath, const AppConfig& cfg, DWORD detectedTypeCCode = 15) {
    std::wstringstream ss;
    ss << L"; =======================================================\r\n";
    ss << L"; KVMSwitch 显示器输入源快速切换配置文件\r\n";
    ss << L"; =======================================================\r\n\r\n";

    ss << L"[General]\r\n";
    ss << L"; 运行模式:\r\n";
    ss << L"; tray   - 常驻系统托盘 (推荐，随时在任务栏右下角切换、监控状态)\r\n";
    ss << L"; menu   - 单次运行弹出快捷选择菜单后退出\r\n";
    ss << L"; toggle - 单次运行直接在 toggle_inputs 中轮流切换后退出\r\n";
    std::wstring modeLower = cfg.mode;
    std::transform(modeLower.begin(), modeLower.end(), modeLower.begin(), ::towlower);
    ss << L"mode = " << modeLower << L"\r\n\r\n";

    ss << L"; 切换成功后是否显示桌面气泡通知 (true / false)\r\n";
    ss << L"notify = " << (cfg.notify ? L"true" : L"false") << L"\r\n\r\n";

    ss << L"; 目标显示器:\r\n";
    ss << L"; primary - 仅切换主显示器 (推荐)\r\n";
    ss << L"; all     - 同时切换所有连接的支持 DDC/CI 的显示器\r\n";
    std::wstring targetLower = cfg.targetMonitor;
    std::transform(targetLower.begin(), targetLower.end(), targetLower.begin(), ::towlower);
    ss << L"target_monitor = " << targetLower << L"\r\n\r\n";

    ss << L"; toggle 轮换模式下循环切换的输入源列表 (逗号分隔)\r\n";
    ss << L"; 支持名称: DP, HDMI1, HDMI2, TypeC 或具体数值 15, 17, 18, 16\r\n";
    ss << L"toggle_inputs = ";
    for (size_t i = 0; i < cfg.toggleInputs.size(); ++i) {
        if (i > 0) ss << L", ";
        ss << GetInputAliasForCode(cfg.toggleInputs[i], detectedTypeCCode);
    }
    ss << L"\r\n\r\n";

    ss << L"[Inputs]\r\n";
    ss << L"; 输入源名称与 VCP 0x60 数值映射 (泰坦军团 P275MV: DP 为 16, Type-C 为 15)\r\n";
    if (!cfg.customInputs.empty()) {
        for (const auto& kv : cfg.customInputs) {
            ss << kv.first << L" = " << kv.second << L"\r\n";
        }
    } else {
        ss << L"DP = 16\r\nDP1 = 16\r\nHDMI1 = 17\r\nHDMI2 = 18\r\nTypeC = " << detectedTypeCCode << L"\r\n";
    }
    ss << L"\r\n";

    ss << L"[Presets]\r\n";
    ss << L"; 多显示器预设方案 (与 macOS 端预设保持一致，一键联动切换所有显示器)\r\n";
    ss << L"; 格式: 预设名称 = 显示器标识:输入源, 显示器标识:输入源 ...\r\n";
    ss << L"; 显示器标识支持: 1, 2, primary, secondary, 设备名或显示器描述\r\n";
    ss << L"; 输入源支持: DP, HDMI1, HDMI2, TypeC 或数值代码 (15, 16, 17 等)，0 表示保持不变\r\n";
    for (const auto& pr : cfg.presets) {
        if (pr.name.empty()) continue;
        ss << pr.name << L" = ";
        for (size_t tIdx = 0; tIdx < pr.targets.size(); ++tIdx) {
            if (tIdx > 0) ss << L", ";
            ss << pr.targets[tIdx].monitorId << L":";
            if (pr.targets[tIdx].inputCode == 0) {
                ss << L"0";
            } else {
                ss << GetInputAliasForCode(pr.targets[tIdx].inputCode, detectedTypeCCode);
            }
        }
        ss << L"\r\n";
    }
    ss << L"\r\n";

    ss << L"[Commands]\r\n";
    ss << L"; 切换到指定输入源后自动在后台执行的系统命令 (可选，留空则不执行)\r\n";
    ss << L"; 例如切换到 Mac 时唤醒 Mac，切换回 PC 时联动等:\r\n";
    DWORD dpCode = 16;
    for (const auto& kv : cfg.customInputs) {
        if (ToUpper(kv.first) == L"DP" || ToUpper(kv.first) == L"DP1") { dpCode = kv.second; break; }
    }
    DWORD tcCode = detectedTypeCCode;
    for (const auto& kv : cfg.customInputs) {
        if (ToUpper(kv.first) == L"TYPEC" || ToUpper(kv.first) == L"TYPE-C" || ToUpper(kv.first) == L"USBC") { tcCode = kv.second; break; }
    }

    auto getCmd = [&](DWORD code) -> std::wstring {
        auto it = cfg.commands.find(code);
        return (it != cfg.commands.end()) ? it->second : L"";
    };

    ss << L"on_switch_to_typec = " << getCmd(tcCode) << L"\r\n";
    ss << L"on_switch_to_hdmi1 = " << getCmd(17) << L"\r\n";
    ss << L"on_switch_to_hdmi2 = " << getCmd(18) << L"\r\n";
    ss << L"on_switch_to_dp = " << getCmd(dpCode) << L"\r\n";

    return WriteFileAsUtf8WithBom(cfgPath, ss.str());
}

// 解析显示器 Capabilities 字符串中的 VCP 0x60 支持项
std::vector<DWORD> ParseCapabilitiesInputCodes(const std::string& caps) {
    std::vector<DWORD> codes;
    // 寻找 60( 11 12 0F 10) 类似结构
    size_t pos = caps.find("60(");
    if (pos == std::string::npos) {
        pos = caps.find("60 (");
    }
    if (pos != std::string::npos) {
        size_t start = caps.find('(', pos);
        size_t end = caps.find(')', start);
        if (start != std::string::npos && end != std::string::npos && end > start) {
            std::string sub = caps.substr(start + 1, end - start - 1);
            std::stringstream ss(sub);
            std::string token;
            while (ss >> token) {
                try {
                    DWORD val = std::stoul(token, nullptr, 16);
                    if (val > 0 && std::find(codes.begin(), codes.end(), val) == codes.end()) {
                        codes.push_back(val);
                    }
                } catch (...) {}
            }
        }
    }
    return codes;
}

// Capabilities (支持输入源列表) 的进程级缓存。
// GetCapabilitiesStringLength 是一次完整的 DDC/CI I2C 慢速事务, 实测约 1.5 秒,
// 而结果只取决于显示器型号, 进程内不会变化 —— 缓存后菜单/托盘刷新只需读取当前输入源 (~60ms)。
// 显示拓扑变化 (WM_DISPLAYCHANGE) 时清空。
static std::map<std::wstring, std::vector<DWORD>> g_capsCache;

// 显示器枚举回调
BOOL CALLBACK MonitorEnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM dwData) {
    auto* ctx = reinterpret_cast<MonitorContext*>(dwData);

    MONITORINFOEXW mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMon, &mi);
    bool isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;

    DWORD numPhys = 0;
    if (GetNumberOfPhysicalMonitorsFromHMONITOR(hMon, &numPhys) && numPhys > 0) {
        size_t startIndex = ctx->rawPhysicalMonitors.size();
        ctx->rawPhysicalMonitors.resize(startIndex + numPhys);

        if (GetPhysicalMonitorsFromHMONITOR(hMon, numPhys, &ctx->rawPhysicalMonitors[startIndex])) {
            ctx->monitorPhysicalCounts.push_back({ hMon, numPhys });

            for (DWORD i = 0; i < numPhys; ++i) {
                HANDLE hPhys = ctx->rawPhysicalMonitors[startIndex + i].hPhysicalMonitor;
                PhysicalMonEntry entry;
                entry.hMonitor = hMon;
                entry.hPhysicalMonitor = hPhys;
                entry.description = ctx->rawPhysicalMonitors[startIndex + i].szPhysicalMonitorDescription;
                entry.deviceName = mi.szDevice;
                entry.isPrimary = isPrimary;
                entry.currentInput = 0;

                // 读取当前输入源
                MC_VCP_CODE_TYPE vcpType;
                DWORD cur = 0, max = 0;
                if (GetVCPFeatureAndVCPFeatureReply(hPhys, 0x60, &vcpType, &cur, &max)) {
                    entry.currentInput = cur;
                }

                // 读取 Capabilities (带缓存, 命中时完全跳过 DDC/CI 慢速事务)
                std::wstring capsKey = entry.deviceName + L"|" + entry.description;
                auto cacheIt = g_capsCache.find(capsKey);
                if (cacheIt != g_capsCache.end()) {
                    entry.supportedInputs = cacheIt->second;
                } else {
                    DWORD capLen = 0;
                    if (GetCapabilitiesStringLength(hPhys, &capLen) && capLen > 0) {
                        std::vector<char> capBuf(capLen + 2, 0);
                        if (CapabilitiesRequestAndCapabilitiesReply(hPhys, capBuf.data(), capLen)) {
                            entry.supportedInputs = ParseCapabilitiesInputCodes(capBuf.data());
                            g_capsCache[capsKey] = entry.supportedInputs;
                        }
                    }
                }

                ctx->monitors.push_back(entry);
            }
        } else {
            ctx->rawPhysicalMonitors.resize(startIndex);
        }
    }
    return TRUE;
}

MonitorContext QueryAllMonitors() {
    MonitorContext ctx;
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx;
}

void FreeMonitorContext(MonitorContext& ctx) {
    // 按照每个 HMONITOR 释放其对应的物理显示器句柄
    size_t offset = 0;
    for (const auto& pair : ctx.monitorPhysicalCounts) {
        if (offset < ctx.rawPhysicalMonitors.size()) {
            DestroyPhysicalMonitors(pair.second, &ctx.rawPhysicalMonitors[offset]);
            offset += pair.second;
        }
    }
    ctx.rawPhysicalMonitors.clear();
    ctx.monitors.clear();
    ctx.monitorPhysicalCounts.clear();
}

// 托盘状态与气泡提示管理
// 在程序目录记录注册结果和通知区位置；受限进程可能无法写入普通临时目录。
static void LogTrayState(const char* operation, BOOL ok, DWORD error) {
    NOTIFYICONIDENTIFIER identifier = { sizeof(identifier) };
    identifier.hWnd = g_trayNid.hWnd;
    identifier.uID = g_trayNid.uID;
    RECT rect = {};
    HRESULT rectResult = Shell_NotifyIconGetRect(&identifier, &rect);
    wchar_t path[MAX_PATH] = {};
    DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    slash[1] = L'\0';
    if (wcscat_s(path, L"KVMSwitch_tray.log") != 0) return;
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME time;
    GetLocalTime(&time);
    char line[512];
    sprintf_s(line, "%04u-%02u-%02u %02u:%02u:%02u pid=%lu %s ok=%d error=%lu hwnd=%p icon=%p flags=0x%x rectHr=0x%08lx rect=(%ld,%ld,%ld,%ld)\r\n",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
        GetCurrentProcessId(), operation, ok, error, g_trayNid.hWnd, g_trayNid.hIcon,
        g_trayNid.uFlags, static_cast<unsigned long>(rectResult), rect.left, rect.top, rect.right, rect.bottom);
    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(strlen(line)), &written, NULL);
    CloseHandle(file);
}

// 托盘图标自愈: Explorer 偶发丢失图标 (进程强杀/任务栏重启/内测版任务栏 bug) 后,
// NIM_ADD 可能静默失败且不再恢复。用 no-op NIM_MODIFY 探测图标是否仍注册,
// 丢失则用完整 uFlags 重新 NIM_ADD; 结果记录到程序旁的 KVMSwitch_tray.log。
static bool EnsureTrayIconRegistered() {
    NOTIFYICONDATAW probe = { sizeof(probe) };
    probe.hWnd = g_trayNid.hWnd;
    probe.uID = g_trayNid.uID;
    probe.uFlags = NIF_STATE;
    probe.dwState = 0;
    probe.dwStateMask = 0;
    if (Shell_NotifyIconW(NIM_MODIFY, &probe)) {
        static bool loggedProbe = false;
        if (!loggedProbe) {
            LogTrayState("NIM_MODIFY probe", TRUE, 0);
            loggedProbe = true;
        }
        return true;
    }

    // 重新注册必须带全三个标志: uFlags 可能已被 UpdateTrayTooltip 改写成仅 NIF_TIP
    g_trayNid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    SetLastError(ERROR_SUCCESS);
    if (Shell_NotifyIconW(NIM_ADD, &g_trayNid)) {
        LogTrayState("NIM_ADD", TRUE, 0);
        return true;
    }

    DWORD err = GetLastError();
    LogTrayState("NIM_ADD", FALSE, err);
    return false;
}

void UpdateTrayTooltip(HWND hWnd, const AppConfig* pCfg = nullptr, DWORD forcedCurrentInput = 0) {
    if (!g_hTrayWnd || !IsWindow(g_hTrayWnd)) return;

    wchar_t tip[128] = { 0 };
    if (forcedCurrentInput != 0) {
        std::wstring iname = GetInputName(forcedCurrentInput, pCfg);
        swprintf_s(tip, L"KVMSwitch\n当前输入: %s (0x%02X)", iname.c_str(), forcedCurrentInput);
    } else {
        MonitorContext ctx = QueryAllMonitors();
        if (ctx.monitors.empty()) {
            swprintf_s(tip, L"KVMSwitch\n未检测到显示器");
        } else if (ctx.monitors.size() == 1) {
            DWORD cur = ctx.monitors[0].currentInput;
            std::wstring iname = cur ? GetInputName(cur, pCfg) : L"未知";
            swprintf_s(tip, L"KVMSwitch\n当前输入: %s (0x%02X)", iname.c_str(), cur);
        } else {
            // 多显示器展示: 例如 "KVMSwitch\n#1: DP\n#2: Type-C"
            std::wstring s = L"KVMSwitch";
            for (size_t i = 0; i < ctx.monitors.size() && i < 3; ++i) {
                DWORD cur = ctx.monitors[i].currentInput;
                std::wstring iname = cur ? GetInputName(cur, pCfg) : L"未知";
                if (iname.find(L"DisplayPort") != std::wstring::npos) iname = L"DP";
                else if (iname.find(L"USB Type-C") != std::wstring::npos) iname = L"Type-C";
                wchar_t line[32];
                swprintf_s(line, L"\n#%zu: %s", i + 1, iname.c_str());
                s += line;
            }
            wcsncpy_s(tip, s.c_str(), _TRUNCATE);
        }
        FreeMonitorContext(ctx);
    }

    wcscpy_s(g_trayNid.szTip, tip);
    g_trayNid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_trayNid);
}

void ShowTrayBalloon(const std::wstring& title, const std::wstring& msg) {
    if (g_hTrayWnd && IsWindow(g_hTrayWnd)) {
        g_trayNid.uFlags = NIF_INFO;
        wcscpy_s(g_trayNid.szInfoTitle, title.c_str());
        wcscpy_s(g_trayNid.szInfo, msg.c_str());
        g_trayNid.dwInfoFlags = NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &g_trayNid);
    }
}

// 执行输入源切换
bool SetMonitorInputSource(HANDLE hPhysMon, DWORD targetInput) {
    for (int retry = 0; retry < 3; ++retry) {
        if (SetVCPFeature(hPhysMon, 0x60, targetInput)) {
            return true;
        }
        Sleep(80);
    }
    return false;
}

// 桌面快捷方式生成
bool CreateDesktopShortcut(LPCWSTR szShortcutName, LPCWSTR szArgs, LPCWSTR szDesc) {
    wchar_t desktopPath[MAX_PATH] = { 0 };
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, desktopPath))) {
        return false;
    }

    std::wstring linkPath = std::wstring(desktopPath) + L"\\" + szShortcutName + L".lnk";
    std::wstring exePath = GetExePath();

    CoInitialize(NULL);
    IShellLinkW* psl = NULL;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (LPVOID*)&psl);
    if (SUCCEEDED(hr)) {
        psl->SetPath(exePath.c_str());
        if (szArgs && wcslen(szArgs) > 0) {
            psl->SetArguments(szArgs);
        }
        psl->SetIconLocation(exePath.c_str(), 0);
        if (szDesc) {
            psl->SetDescription(szDesc);
        }

        IPersistFile* ppf = NULL;
        hr = psl->QueryInterface(IID_IPersistFile, (LPVOID*)&ppf);
        if (SUCCEEDED(hr)) {
            hr = ppf->Save(linkPath.c_str(), TRUE);
            ppf->Release();
        }
        psl->Release();
    }
    CoUninitialize();
    return SUCCEEDED(hr);
}

void CreateAllShortcuts(const AppConfig* pCfg = nullptr) {
    CreateDesktopShortcut(L"切换显示器 - DP", L"dp", L"一键切换显示器至 DisplayPort 输入");
    CreateDesktopShortcut(L"切换显示器 - HDMI 1", L"hdmi1", L"一键切换显示器至 HDMI 1 输入");
    CreateDesktopShortcut(L"切换显示器 - HDMI 2", L"hdmi2", L"一键切换显示器至 HDMI 2 输入");
    CreateDesktopShortcut(L"切换显示器 - Type-C", L"typec", L"一键切换显示器至 USB Type-C 输入");
    CreateDesktopShortcut(L"切换显示器 - 轮流切换", L"--toggle", L"在常用输入源之间轮流快速切换");
    CreateDesktopShortcut(L"KVMSwitch (系统托盘常驻)", L"--tray", L"启动 KVMSwitch 并常驻系统托盘");

    std::wstring msg = L"已成功在桌面创建以下快捷方式：\n\n"
        L"1. 切换显示器 - DP (DisplayPort)\n"
        L"2. 切换显示器 - HDMI 1\n"
        L"3. 切换显示器 - HDMI 2\n"
        L"4. 切换显示器 - Type-C\n"
        L"5. 切换显示器 - 轮流切换 (Toggle)\n"
        L"6. KVMSwitch (系统托盘常驻)\n";

    if (pCfg && !pCfg->presets.empty()) {
        msg += L"\n已同时生成多显示器预设快捷方式：\n";
        for (const auto& pr : pCfg->presets) {
            std::wstring scName = L"预设 - " + pr.name;
            std::wstring scArgs = L"--preset \"" + pr.name + L"\"";
            std::wstring scDesc = L"一键应用多显示器预设方案: " + pr.name;
            CreateDesktopShortcut(scName.c_str(), scArgs.c_str(), scDesc.c_str());
            msg += L"• " + scName + L"\n";
        }
    }

    msg += L"\n您可以直接在桌面双击，或将其拖动到任务栏、为快捷方式设置全局热键！";

    MessageBoxW(NULL, msg.c_str(), L"快捷方式创建成功", MB_OK | MB_ICONINFORMATION);
}

// 桌面气泡提示
void ShowNotification(HINSTANCE hInstance, const std::wstring& title, const std::wstring& msg) {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"KVMSwitchNotifyWindowClass";
    RegisterClassExW(&wc);

    HWND hWnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (!hWnd) return;

    HICON hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(ID_KVMSWITCH));
    if (!hIcon) hIcon = LoadIconW(NULL, IDI_APPLICATION);

    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = hWnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_INFO;
    nid.hIcon = hIcon;
    wcscpy_s(nid.szTip, L"KVMSwitch");
    wcscpy_s(nid.szInfoTitle, title.c_str());
    wcscpy_s(nid.szInfo, msg.c_str());
    nid.dwInfoFlags = NIIF_INFO;

    Shell_NotifyIconW(NIM_ADD, &nid);

    // 运行简短消息循环让气泡弹出 1.5 秒
    SetTimer(hWnd, 1, 1500, NULL);
    MSG msgObj;
    while (GetMessageW(&msgObj, NULL, 0, 0)) {
        if (msgObj.message == WM_TIMER) {
            break;
        }
        TranslateMessage(&msgObj);
        DispatchMessageW(&msgObj);
    }

    Shell_NotifyIconW(NIM_DELETE, &nid);
    DestroyWindow(hWnd);
    UnregisterClassW(wc.lpszClassName, hInstance);
}

// 执行联动命令
void ExecuteCustomCommand(const std::wstring& cmd) {
    if (cmd.empty()) return;
    std::wstring cmdLine = L"/c " + cmd;
    ShellExecuteW(NULL, L"open", L"cmd.exe", cmdLine.c_str(), NULL, SW_HIDE);
}

// 统一切换入口
bool DoSwitch(HINSTANCE hInstance, DWORD targetCode, const AppConfig& cfg, MonitorContext& ctx) {
    bool anySuccess = false;
    bool switchAll = (cfg.targetMonitor == L"ALL");

    for (const auto& mon : ctx.monitors) {
        if (switchAll || mon.isPrimary || ctx.monitors.size() == 1) {
            if (SetMonitorInputSource(mon.hPhysicalMonitor, targetCode)) {
                anySuccess = true;
            }
        }
    }

    if (anySuccess) {
        // 执行关联命令
        auto it = cfg.commands.find(targetCode);
        if (it != cfg.commands.end()) {
            ExecuteCustomCommand(it->second);
        }

        // 显示通知
        if (cfg.notify) {
            std::wstring iname = GetInputName(targetCode, &cfg);
            std::wstring tip = L"显示器输入源已切换至: " + iname;
            if (g_hTrayWnd && IsWindow(g_hTrayWnd)) {
                ShowTrayBalloon(L"KVMSwitch 输入切换", tip);
            } else {
                ShowNotification(hInstance, L"KVMSwitch 输入切换", tip);
            }
        }

        if (g_hTrayWnd && IsWindow(g_hTrayWnd)) {
            UpdateTrayTooltip(g_hTrayWnd, &cfg, targetCode);
        }
    } else {
        MessageBoxW(NULL,
            L"切换输入源失败！\n\n请检查显示器 OSD 菜单是否已开启 DDC/CI 功能，以及线缆是否正常连接。",
            L"KVMSwitch 错误", MB_OK | MB_ICONERROR);
    }

    return anySuccess;
}


// 获取显示器支持的输入源代码列表 (带常见输入源补齐与权重排序)
std::vector<DWORD> GetSupportedInputCodes(const PhysicalMonEntry* mon, DWORD detectedTypeCCode = 15) {
    std::vector<DWORD> codes;
    if (mon && !mon->supportedInputs.empty()) {
        codes = mon->supportedInputs;
    } else {
        codes = { 16, 17, 18, detectedTypeCCode };
    }

    if (std::find(codes.begin(), codes.end(), 16) == codes.end()) codes.push_back(16);
    if (std::find(codes.begin(), codes.end(), 17) == codes.end()) codes.push_back(17);
    if (std::find(codes.begin(), codes.end(), detectedTypeCCode) == codes.end()) codes.push_back(detectedTypeCCode);

    std::sort(codes.begin(), codes.end(), [](DWORD a, DWORD b) {
        auto getWeight = [](DWORD c) {
            if (c == 16) return 1; // DP
            if (c == 17) return 2; // HDMI 1
            if (c == 18) return 3; // HDMI 2
            if (c == 15 || c == 27 || c == 25) return 4; // Type-C
            return (int)c + 10;
        };
        return getWeight(a) < getWeight(b);
    });
    return codes;
}

// 预设中的显示器匹配逻辑 (支持 1-based 序号、primary/secondary、设备名、型号/描述子串)
bool MatchMonitor(const PhysicalMonEntry& mon, size_t mon1BasedIndex, const std::wstring& targetId) {
    std::wstring id = ToUpper(Trim(targetId));
    if (id.empty()) return false;

    // 1. 编号匹配: "1", "#1"
    if (id == std::to_wstring(mon1BasedIndex) || id == (L"#" + std::to_wstring(mon1BasedIndex))) {
        return true;
    }

    // 2. 主副角色匹配: "PRIMARY", "MAIN", "SECONDARY"
    if ((id == L"PRIMARY" || id == L"MAIN") && mon.isPrimary) {
        return true;
    }
    if (id == L"SECONDARY" && !mon.isPrimary && mon1BasedIndex == 2) {
        return true;
    }

    // 3. 设备名匹配: "\\.\DISPLAY1" 或 "DISPLAY1"
    std::wstring devUpper = ToUpper(mon.deviceName);
    if (devUpper == id || devUpper.find(id) != std::wstring::npos) {
        return true;
    }

    // 4. 显示器型号/描述子串匹配: 例如 "P275MV"
    std::wstring descUpper = ToUpper(mon.description);
    if (descUpper.find(id) != std::wstring::npos) {
        return true;
    }

    return false;
}

// 应用多显示器预设方案 (与 macOS 端 applyPreset 保持一致)
bool ApplyPreset(HINSTANCE hInstance, const MonitorPreset& preset, const AppConfig& cfg, MonitorContext& ctx) {
    int appliedCount = 0;
    std::vector<std::wstring> switchLogs;

    for (size_t i = 0; i < ctx.monitors.size(); ++i) {
        const auto& mon = ctx.monitors[i];
        size_t mon1Based = i + 1;

        // 查找该显示器对应的预设目标输入源
        DWORD targetCode = 0;
        for (const auto& target : preset.targets) {
            if (MatchMonitor(mon, mon1Based, target.monitorId)) {
                targetCode = target.inputCode;
                break;
            }
        }

        // targetCode == 0 表示保持不变或未配置
        if (targetCode > 0 && targetCode != mon.currentInput) {
            if (SetMonitorInputSource(mon.hPhysicalMonitor, targetCode)) {
                appliedCount++;
                std::wstring iname = GetInputName(targetCode, &cfg);
                switchLogs.push_back(L"#" + std::to_wstring(mon1Based) + L": " + iname);

                // 执行关联联动命令
                auto itCmd = cfg.commands.find(targetCode);
                if (itCmd != cfg.commands.end()) {
                    ExecuteCustomCommand(itCmd->second);
                }
            }
        }
    }

    if (appliedCount > 0) {
        if (cfg.notify) {
            std::wstring msg = L"预设「" + preset.name + L"」已生效";
            if (!switchLogs.empty()) {
                msg += L" (";
                for (size_t idx = 0; idx < switchLogs.size(); ++idx) {
                    if (idx > 0) msg += L", ";
                    msg += switchLogs[idx];
                }
                msg += L")";
            }
            if (g_hTrayWnd && IsWindow(g_hTrayWnd)) {
                ShowTrayBalloon(L"KVMSwitch 预设切换", msg);
            } else {
                ShowNotification(hInstance, L"KVMSwitch 预设切换", msg);
            }
        }

        if (g_hTrayWnd && IsWindow(g_hTrayWnd)) {
            UpdateTrayTooltip(g_hTrayWnd, &cfg);
        }
        return true;
    } else {
        if (cfg.notify) {
            std::wstring msg = L"预设「" + preset.name + L"」：所有显示器已处于目标状态或无变更。";
            if (g_hTrayWnd && IsWindow(g_hTrayWnd)) {
                ShowTrayBalloon(L"KVMSwitch 预设", msg);
            }
        }
        return false;
    }
}

// 保存预设对话框内部状态
struct SavePresetDialogState {
    std::wstring currentSummary;
    std::wstring resultName;
    bool confirmed = false;
    HWND hEdit = NULL;
};

static LRESULT CALLBACK SavePresetDlgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SavePresetDialogState* pState = reinterpret_cast<SavePresetDialogState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pState = reinterpret_cast<SavePresetDialogState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pState));

        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        HWND hLabel1 = CreateWindowExW(0, L"STATIC", L"当前显示器输入状态将被保存为新预设：",
            WS_CHILD | WS_VISIBLE, 20, 15, 390, 20, hWnd, NULL, cs->hInstance, NULL);
        SendMessageW(hLabel1, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hLabelSummary = CreateWindowExW(0, L"STATIC", pState->currentSummary.c_str(),
            WS_CHILD | WS_VISIBLE, 20, 38, 390, 48, hWnd, NULL, cs->hInstance, NULL);
        SendMessageW(hLabelSummary, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hLabel2 = CreateWindowExW(0, L"STATIC", L"预设名称 (例如: 双屏办公、游戏娱乐):",
            WS_CHILD | WS_VISIBLE, 20, 94, 390, 20, hWnd, NULL, cs->hInstance, NULL);
        SendMessageW(hLabel2, WM_SETFONT, (WPARAM)hFont, TRUE);

        pState->hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"我的预设",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 20, 118, 390, 24, hWnd, (HMENU)101, cs->hInstance, NULL);
        SendMessageW(pState->hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(pState->hEdit, EM_SETSEL, 0, -1);

        HWND hBtnOk = CreateWindowExW(0, L"BUTTON", L"保存",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 220, 156, 90, 28, hWnd, (HMENU)IDOK, cs->hInstance, NULL);
        SendMessageW(hBtnOk, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hBtnCancel = CreateWindowExW(0, L"BUTTON", L"取消",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 320, 156, 90, 28, hWnd, (HMENU)IDCANCEL, cs->hInstance, NULL);
        SendMessageW(hBtnCancel, WM_SETFONT, (WPARAM)hFont, TRUE);

        SetFocus(pState->hEdit);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdcStatic = (HDC)wParam;
        SetBkColor(hdcStatic, GetSysColor(COLOR_BTNFACE));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDOK) {
            wchar_t buf[256] = { 0 };
            GetWindowTextW(pState->hEdit, buf, 256);
            std::wstring name = Trim(buf);
            if (name.empty()) {
                MessageBoxW(hWnd, L"预设名称不能为空！", L"提示", MB_OK | MB_ICONWARNING);
                SetFocus(pState->hEdit);
                return 0;
            }
            pState->resultName = name;
            pState->confirmed = true;
            DestroyWindow(hWnd);
            return 0;
        } else if (id == IDCANCEL) {
            pState->confirmed = false;
            DestroyWindow(hWnd);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        pState->confirmed = false;
        DestroyWindow(hWnd);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// 弹出纯 Win32 模态预设保存对话框
std::wstring PromptSavePresetDialog(HINSTANCE hInstance, HWND hParent, const std::wstring& summary) {
    WNDCLASSEXW wc = { sizeof(wc) };
    if (!GetClassInfoExW(hInstance, L"KVMSwitchSavePresetDlgClass", &wc)) {
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = SavePresetDlgProc;
        wc.hInstance = hInstance;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"KVMSwitchSavePresetDlgClass";
        RegisterClassExW(&wc);
    }

    SavePresetDialogState state;
    state.currentSummary = summary;

    int dlgW = 450;
    int dlgH = 240;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenW - dlgW) / 2;
    int posY = (screenH - dlgH) / 2;

    HWND hDlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"KVMSwitchSavePresetDlgClass",
        L"保存多显示器预设方案",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        posX, posY, dlgW, dlgH, hParent, NULL, hInstance, &state);

    if (!hDlg) return L"";

    if (hParent) EnableWindow(hParent, FALSE);
    SetForegroundWindow(hDlg);

    MSG msg;
    while (IsWindow(hDlg) && GetMessageW(&msg, NULL, 0, 0)) {
        if (msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_RETURN) {
                SendMessageW(hDlg, WM_COMMAND, IDOK, 0);
                continue;
            } else if (msg.wParam == VK_ESCAPE) {
                SendMessageW(hDlg, WM_COMMAND, IDCANCEL, 0);
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hParent) {
        EnableWindow(hParent, TRUE);
        SetForegroundWindow(hParent);
    }

    return state.confirmed ? state.resultName : L"";
}

// 保存当前所有物理显示器状态为新预设
void SaveCurrentStateAsPreset(HINSTANCE hInstance, const MonitorContext& ctx, const AppConfig* pCfg, DWORD detectedTypeCCode) {
    if (ctx.monitors.empty()) {
        MessageBoxW(NULL, L"未检测到外接显示器，无法保存预设。", L"KVMSwitch", MB_OK | MB_ICONWARNING);
        return;
    }

    std::wstring summary;
    std::wstring presetValStr;

    for (size_t i = 0; i < ctx.monitors.size(); ++i) {
        const auto& m = ctx.monitors[i];
        size_t monIndex = i + 1;
        std::wstring iname = m.currentInput ? GetInputName(m.currentInput, pCfg) : L"未知";

        if (i > 0) summary += L"\n";
        summary += L"• 显示器 #" + std::to_wstring(monIndex) + L" (" + m.description + L"): " + iname;

        if (i > 0) presetValStr += L", ";
        std::wstring aliasStr = GetInputAliasForCode(m.currentInput, detectedTypeCCode);
        presetValStr += std::to_wstring(monIndex) + L":" + aliasStr;
    }

    std::wstring presetName = PromptSavePresetDialog(hInstance, NULL, summary);
    if (presetName.empty()) {
        return;
    }

    std::wstring cfgPath = GetConfigPath();
    AppConfig updatedCfg = LoadConfig(cfgPath, detectedTypeCCode);

    // 检查并更新现有同名预设或追加新预设
    MonitorPreset newPreset;
    newPreset.name = presetName;
    for (size_t i = 0; i < ctx.monitors.size(); ++i) {
        newPreset.targets.push_back({ std::to_wstring(i + 1), ctx.monitors[i].currentInput });
    }

    bool found = false;
    for (auto& pr : updatedCfg.presets) {
        if (pr.name == presetName) {
            pr = newPreset;
            found = true;
            break;
        }
    }
    if (!found) {
        updatedCfg.presets.push_back(newPreset);
    }

    SaveConfigToFile(cfgPath, updatedCfg, detectedTypeCCode);

    std::wstring succMsg = L"预设「" + presetName + L"」已成功保存！\n\n配置项: " + presetValStr + L"\n您可以随时在托盘菜单中一键应用该方案。";
    if (g_hTrayWnd && IsWindow(g_hTrayWnd)) {
        ShowTrayBalloon(L"KVMSwitch 预设已保存", L"预设「" + presetName + L"」保存成功。");
    } else {
        MessageBoxW(NULL, succMsg.c_str(), L"预设已保存", MB_OK | MB_ICONINFORMATION);
    }
}

// -------------------------------------------------------------
// 图形化设置窗口 (Settings Dialog) - 与 macOS 端 SettingsView 完全对齐
// -------------------------------------------------------------

struct MonComboInfo {
    HWND hLabel = NULL;
    HWND hCombo = NULL;
    std::vector<DWORD> inputCodes; // 下标 0 为 0 (不改变)，后续为支持的代码
};

struct SettingsDialogState {
    HINSTANCE hInstance = NULL;
    HWND hDlg = NULL;
    HWND hTab = NULL;
    HWND hBtnOpenIni = NULL;
    HWND hBtnSave = NULL;
    HWND hBtnCancel = NULL;

    // Tab 0 (预设方案) 控件
    HWND hPresetList = NULL;
    HWND hPresetBtnAdd = NULL;
    HWND hPresetBtnDel = NULL;
    HWND hPresetBtnUp = NULL;
    HWND hPresetBtnDown = NULL;
    HWND hPresetNameLabel = NULL;
    HWND hPresetNameEdit = NULL;
    HWND hPresetMonHeader = NULL;
    HWND hPresetPlaceholder = NULL;
    std::vector<MonComboInfo> monCombos;

    // Tab 1 (通用偏好与联动) 控件
    HWND hGenNotifyCheck = NULL;
    HWND hGenAutoStartCheck = NULL;
    HWND hGenModeLabel = NULL;
    HWND hGenModeCombo = NULL;
    HWND hGenTargetLabel = NULL;
    HWND hGenTargetCombo = NULL;
    HWND hGenCmdHeader = NULL;
    HWND hCmdDpLabel = NULL;
    HWND hCmdDpEdit = NULL;
    HWND hCmdTypecLabel = NULL;
    HWND hCmdTypecEdit = NULL;
    HWND hCmdHdmi1Label = NULL;
    HWND hCmdHdmi1Edit = NULL;
    HWND hCmdHdmi2Label = NULL;
    HWND hCmdHdmi2Edit = NULL;
    HWND hGenCmdHint = NULL;

    // 临时数据与运行时状态
    AppConfig tempConfig;
    const MonitorContext* pCtx = nullptr;
    DWORD detectedTypeCCode = 15;
    int currentPresetIndex = -1;
    bool loadingPreset = false;
    bool saved = false;
    HFONT hFont = NULL;
    HFONT hBoldFont = NULL;
};

// 切换 Tab 页面的显示/隐藏
static void UpdateSettingsTabVisibility(SettingsDialogState* pState) {
    if (!pState || !pState->hTab) return;
    int curTab = TabCtrl_GetCurSel(pState->hTab);

    if (curTab == 0) {
        // Tab 0: 预设方案
        ShowWindow(pState->hPresetList, SW_SHOW);
        ShowWindow(pState->hPresetBtnAdd, SW_SHOW);
        ShowWindow(pState->hPresetBtnDel, SW_SHOW);
        ShowWindow(pState->hPresetBtnUp, SW_SHOW);
        ShowWindow(pState->hPresetBtnDown, SW_SHOW);

        bool hasSelection = (pState->currentPresetIndex >= 0 && pState->currentPresetIndex < (int)pState->tempConfig.presets.size());
        int showRight = hasSelection ? SW_SHOW : SW_HIDE;
        int showPlaceholder = hasSelection ? SW_HIDE : SW_SHOW;

        ShowWindow(pState->hPresetNameLabel, showRight);
        ShowWindow(pState->hPresetNameEdit, showRight);
        ShowWindow(pState->hPresetMonHeader, showRight);

        for (auto& row : pState->monCombos) {
            ShowWindow(row.hLabel, showRight);
            ShowWindow(row.hCombo, showRight);
        }
        ShowWindow(pState->hPresetPlaceholder, showPlaceholder);

        // 隐藏 Tab 1 控件
        ShowWindow(pState->hGenNotifyCheck, SW_HIDE);
        ShowWindow(pState->hGenAutoStartCheck, SW_HIDE);
        ShowWindow(pState->hGenModeLabel, SW_HIDE);
        ShowWindow(pState->hGenModeCombo, SW_HIDE);
        ShowWindow(pState->hGenTargetLabel, SW_HIDE);
        ShowWindow(pState->hGenTargetCombo, SW_HIDE);
        ShowWindow(pState->hGenCmdHeader, SW_HIDE);
        ShowWindow(pState->hCmdDpLabel, SW_HIDE);
        ShowWindow(pState->hCmdDpEdit, SW_HIDE);
        ShowWindow(pState->hCmdTypecLabel, SW_HIDE);
        ShowWindow(pState->hCmdTypecEdit, SW_HIDE);
        ShowWindow(pState->hCmdHdmi1Label, SW_HIDE);
        ShowWindow(pState->hCmdHdmi1Edit, SW_HIDE);
        ShowWindow(pState->hCmdHdmi2Label, SW_HIDE);
        ShowWindow(pState->hCmdHdmi2Edit, SW_HIDE);
        ShowWindow(pState->hGenCmdHint, SW_HIDE);
    } else {
        // Tab 1: 通用偏好与联动
        // 隐藏 Tab 0 控件
        ShowWindow(pState->hPresetList, SW_HIDE);
        ShowWindow(pState->hPresetBtnAdd, SW_HIDE);
        ShowWindow(pState->hPresetBtnDel, SW_HIDE);
        ShowWindow(pState->hPresetBtnUp, SW_HIDE);
        ShowWindow(pState->hPresetBtnDown, SW_HIDE);
        ShowWindow(pState->hPresetNameLabel, SW_HIDE);
        ShowWindow(pState->hPresetNameEdit, SW_HIDE);
        ShowWindow(pState->hPresetMonHeader, SW_HIDE);
        for (auto& row : pState->monCombos) {
            ShowWindow(row.hLabel, SW_HIDE);
            ShowWindow(row.hCombo, SW_HIDE);
        }
        ShowWindow(pState->hPresetPlaceholder, SW_HIDE);

        // 显示 Tab 1 控件
        ShowWindow(pState->hGenNotifyCheck, SW_SHOW);
        ShowWindow(pState->hGenAutoStartCheck, SW_SHOW);
        ShowWindow(pState->hGenModeLabel, SW_SHOW);
        ShowWindow(pState->hGenModeCombo, SW_SHOW);
        ShowWindow(pState->hGenTargetLabel, SW_SHOW);
        ShowWindow(pState->hGenTargetCombo, SW_SHOW);
        ShowWindow(pState->hGenCmdHeader, SW_SHOW);
        ShowWindow(pState->hCmdDpLabel, SW_SHOW);
        ShowWindow(pState->hCmdDpEdit, SW_SHOW);
        ShowWindow(pState->hCmdTypecLabel, SW_SHOW);
        ShowWindow(pState->hCmdTypecEdit, SW_SHOW);
        ShowWindow(pState->hCmdHdmi1Label, SW_SHOW);
        ShowWindow(pState->hCmdHdmi1Edit, SW_SHOW);
        ShowWindow(pState->hCmdHdmi2Label, SW_SHOW);
        ShowWindow(pState->hCmdHdmi2Edit, SW_SHOW);
        ShowWindow(pState->hGenCmdHint, SW_SHOW);
    }
}

// 将右侧输入源与名称配置存回当前选中的预设方案中
static void SaveRightPanelToPreset(SettingsDialogState* pState) {
    if (pState->currentPresetIndex < 0 || pState->currentPresetIndex >= (int)pState->tempConfig.presets.size()) {
        return;
    }
    if (pState->loadingPreset) return;

    auto& preset = pState->tempConfig.presets[pState->currentPresetIndex];
    wchar_t nameBuf[256] = { 0 };
    GetWindowTextW(pState->hPresetNameEdit, nameBuf, 256);
    std::wstring newName = Trim(nameBuf);
    if (!newName.empty()) {
        preset.name = newName;
    }

    preset.targets.clear();
    for (size_t i = 0; i < pState->monCombos.size(); ++i) {
        int sel = (int)SendMessageW(pState->monCombos[i].hCombo, CB_GETCURSEL, 0, 0);
        DWORD code = 0;
        if (sel >= 0 && sel < (int)pState->monCombos[i].inputCodes.size()) {
            code = pState->monCombos[i].inputCodes[sel];
        }
        preset.targets.push_back({ std::to_wstring(i + 1), code });
    }
}

// 将指定预设方案加载至右侧编辑面板
static void LoadPresetToRightPanel(SettingsDialogState* pState, int index) {
    if (index < 0 || index >= (int)pState->tempConfig.presets.size()) {
        pState->currentPresetIndex = -1;
        UpdateSettingsTabVisibility(pState);
        return;
    }

    pState->loadingPreset = true;
    pState->currentPresetIndex = index;
    const auto& preset = pState->tempConfig.presets[index];

    SetWindowTextW(pState->hPresetNameEdit, preset.name.c_str());

    for (size_t i = 0; i < pState->monCombos.size(); ++i) {
        DWORD targetCode = 0;
        if (pState->pCtx && i < pState->pCtx->monitors.size()) {
            const auto& mon = pState->pCtx->monitors[i];
            size_t mon1Based = i + 1;
            for (const auto& t : preset.targets) {
                if (MatchMonitor(mon, mon1Based, t.monitorId)) {
                    targetCode = t.inputCode;
                    break;
                }
            }
        } else if (i < preset.targets.size()) {
            targetCode = preset.targets[i].inputCode;
        }

        int selIdx = 0;
        for (size_t cIdx = 0; cIdx < pState->monCombos[i].inputCodes.size(); ++cIdx) {
            if (pState->monCombos[i].inputCodes[cIdx] == targetCode) {
                selIdx = static_cast<int>(cIdx);
                break;
            }
        }
        SendMessageW(pState->monCombos[i].hCombo, CB_SETCURSEL, selIdx, 0);
    }

    pState->loadingPreset = false;
    UpdateSettingsTabVisibility(pState);
}

// 刷新左侧预设列表
static void RefreshPresetListbox(SettingsDialogState* pState, int selectIndex = -1) {
    SendMessageW(pState->hPresetList, LB_RESETCONTENT, 0, 0);
    for (const auto& pr : pState->tempConfig.presets) {
        SendMessageW(pState->hPresetList, LB_ADDSTRING, 0, (LPARAM)pr.name.c_str());
    }
    if (selectIndex >= 0 && selectIndex < (int)pState->tempConfig.presets.size()) {
        SendMessageW(pState->hPresetList, LB_SETCURSEL, selectIndex, 0);
        LoadPresetToRightPanel(pState, selectIndex);
    } else if (!pState->tempConfig.presets.empty()) {
        SendMessageW(pState->hPresetList, LB_SETCURSEL, 0, 0);
        LoadPresetToRightPanel(pState, 0);
    } else {
        LoadPresetToRightPanel(pState, -1);
    }
}

static BOOL CALLBACK SetChildFontProc(HWND hChild, LPARAM lParam) {
    SendMessageW(hChild, WM_SETFONT, (WPARAM)lParam, TRUE);
    return TRUE;
}

// 设置窗口过程函数
static LRESULT CALLBACK SettingsDlgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SettingsDialogState* pState = reinterpret_cast<SettingsDialogState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pState = reinterpret_cast<SettingsDialogState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pState));
        pState->hDlg = hWnd;
        g_hSettingsDlg = hWnd;

        pState->hFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        pState->hBoldFont = CreateFontW(-13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

        // 1. Tab Control
        pState->hTab = CreateWindowExW(0, WC_TABCONTROLW, L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP,
            12, 10, 620, 420, hWnd, (HMENU)IDC_SETTINGS_TAB, cs->hInstance, NULL);

        TCITEMW ti = { 0 };
        ti.mask = TCIF_TEXT;
        ti.pszText = const_cast<LPWSTR>(L"📐 多显示器预设方案 (Presets)");
        TabCtrl_InsertItem(pState->hTab, 0, &ti);
        ti.pszText = const_cast<LPWSTR>(L"⚙️ 通用偏好与联动 (General & Commands)");
        TabCtrl_InsertItem(pState->hTab, 1, &ti);

        // 2. Tab 0 控件
        pState->hPresetList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY,
            24, 46, 175, 330, hWnd, (HMENU)IDC_PRESET_LIST, cs->hInstance, NULL);

        pState->hPresetBtnAdd = CreateWindowExW(0, L"BUTTON", L"➕",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            24, 384, 40, 26, hWnd, (HMENU)IDC_PRESET_ADD, cs->hInstance, NULL);

        pState->hPresetBtnDel = CreateWindowExW(0, L"BUTTON", L"➖",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            69, 384, 40, 26, hWnd, (HMENU)IDC_PRESET_DEL, cs->hInstance, NULL);

        pState->hPresetBtnUp = CreateWindowExW(0, L"BUTTON", L"▲",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            114, 384, 40, 26, hWnd, (HMENU)IDC_PRESET_UP, cs->hInstance, NULL);

        pState->hPresetBtnDown = CreateWindowExW(0, L"BUTTON", L"▼",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            159, 384, 40, 26, hWnd, (HMENU)IDC_PRESET_DOWN, cs->hInstance, NULL);

        // Tab 0 右侧
        pState->hPresetNameLabel = CreateWindowExW(0, L"STATIC", L"预设名称:",
            WS_CHILD | WS_VISIBLE, 220, 50, 70, 20, hWnd, NULL, cs->hInstance, NULL);

        pState->hPresetNameEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
            295, 48, 325, 24, hWnd, (HMENU)IDC_PRESET_NAME_EDIT, cs->hInstance, NULL);

        pState->hPresetMonHeader = CreateWindowExW(0, L"STATIC", L"显示器目标输入源配置:",
            WS_CHILD | WS_VISIBLE, 220, 84, 400, 20, hWnd, NULL, cs->hInstance, NULL);

        pState->hPresetPlaceholder = CreateWindowExW(0, L"STATIC", L"从左侧列表选择或新建预设方案进行配置",
            WS_CHILD | SS_CENTER, 220, 180, 400, 40, hWnd, NULL, cs->hInstance, NULL);

        // 为连接的各台外接显示器创建目标源下拉选择器
        if (pState->pCtx && !pState->pCtx->monitors.empty()) {
            for (size_t i = 0; i < pState->pCtx->monitors.size(); ++i) {
                const auto& mon = pState->pCtx->monitors[i];
                int yRow = 114 + static_cast<int>(i) * 36;

                std::wstring desc = mon.description;
                if (desc.length() > 22) desc = desc.substr(0, 20) + L"...";
                std::wstring monLabelText = L"🖥️ #" + std::to_wstring(i + 1) + L" " + desc + (mon.isPrimary ? L" (主):" : L":");

                HWND hMonLabel = CreateWindowExW(0, L"STATIC", monLabelText.c_str(),
                    WS_CHILD | WS_VISIBLE, 220, yRow + 3, 210, 20, hWnd, NULL, cs->hInstance, NULL);

                HWND hMonCombo = CreateWindowExW(0, L"COMBOBOX", L"",
                    WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                    435, yRow, 185, 220, hWnd, (HMENU)(IDC_PRESET_MON_BASE + i), cs->hInstance, NULL);

                MonComboInfo info;
                info.hLabel = hMonLabel;
                info.hCombo = hMonCombo;

                SendMessageW(hMonCombo, CB_ADDSTRING, 0, (LPARAM)L"不改变 (Don't Change)");
                info.inputCodes.push_back(0);

                std::vector<DWORD> supported = GetSupportedInputCodes(&mon, pState->detectedTypeCCode);
                for (DWORD code : supported) {
                    std::wstring iname = GetInputName(code, &pState->tempConfig);
                    wchar_t itemBuf[128];
                    swprintf_s(itemBuf, L"%s (0x%02X)", iname.c_str(), code);
                    SendMessageW(hMonCombo, CB_ADDSTRING, 0, (LPARAM)itemBuf);
                    info.inputCodes.push_back(code);
                }
                SendMessageW(hMonCombo, CB_SETCURSEL, 0, 0);

                pState->monCombos.push_back(info);
            }
        } else {
            HWND hNoMon = CreateWindowExW(0, L"STATIC", L"未检测到支持 DDC/CI 的外接显示器（请确认线缆连接并开启 DDC/CI）",
                WS_CHILD | WS_VISIBLE, 220, 114, 400, 40, hWnd, NULL, cs->hInstance, NULL);
            MonComboInfo dummy;
            dummy.hLabel = hNoMon;
            pState->monCombos.push_back(dummy);
        }

        // 3. Tab 1 控件
        pState->hGenNotifyCheck = CreateWindowExW(0, L"BUTTON", L"切换输入源成功后弹出系统气泡通知",
            WS_CHILD | BS_AUTOCHECKBOX | WS_TABSTOP,
            28, 48, 400, 22, hWnd, (HMENU)IDC_GENERAL_NOTIFY, cs->hInstance, NULL);

        pState->hGenAutoStartCheck = CreateWindowExW(0, L"BUTTON", L"开机自启动 (登录系统后常驻系统托盘)",
            WS_CHILD | BS_AUTOCHECKBOX | WS_TABSTOP,
            28, 74, 400, 22, hWnd, (HMENU)IDC_GENERAL_AUTOSTART, cs->hInstance, NULL);

        pState->hGenModeLabel = CreateWindowExW(0, L"STATIC", L"托盘运行模式:",
            WS_CHILD, 28, 104, 130, 20, hWnd, NULL, cs->hInstance, NULL);

        pState->hGenModeCombo = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
            160, 102, 260, 120, hWnd, (HMENU)IDC_GENERAL_MODE, cs->hInstance, NULL);
        SendMessageW(pState->hGenModeCombo, CB_ADDSTRING, 0, (LPARAM)L"常驻系统托盘 (tray) - 推荐");
        SendMessageW(pState->hGenModeCombo, CB_ADDSTRING, 0, (LPARAM)L"单次运行弹出菜单 (menu)");
        SendMessageW(pState->hGenModeCombo, CB_ADDSTRING, 0, (LPARAM)L"单次运行轮流切换 (toggle)");

        if (pState->tempConfig.mode == L"MENU") SendMessageW(pState->hGenModeCombo, CB_SETCURSEL, 1, 0);
        else if (pState->tempConfig.mode == L"TOGGLE") SendMessageW(pState->hGenModeCombo, CB_SETCURSEL, 2, 0);
        else SendMessageW(pState->hGenModeCombo, CB_SETCURSEL, 0, 0);

        pState->hGenTargetLabel = CreateWindowExW(0, L"STATIC", L"默认切换目标:",
            WS_CHILD, 28, 134, 130, 20, hWnd, NULL, cs->hInstance, NULL);

        pState->hGenTargetCombo = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
            160, 132, 260, 120, hWnd, (HMENU)IDC_GENERAL_TARGET, cs->hInstance, NULL);
        SendMessageW(pState->hGenTargetCombo, CB_ADDSTRING, 0, (LPARAM)L"仅切换主显示器 (primary) - 推荐");
        SendMessageW(pState->hGenTargetCombo, CB_ADDSTRING, 0, (LPARAM)L"同时切换所有外接显示器 (all)");

        if (pState->tempConfig.targetMonitor == L"ALL") SendMessageW(pState->hGenTargetCombo, CB_SETCURSEL, 1, 0);
        else SendMessageW(pState->hGenTargetCombo, CB_SETCURSEL, 0, 0);

        pState->hGenCmdHeader = CreateWindowExW(0, L"STATIC", L"端口联动命令 (切换至对应输入源后在后台静默执行系统命令，留空则不执行):",
            WS_CHILD, 28, 168, 580, 20, hWnd, NULL, cs->hInstance, NULL);

        pState->hCmdDpLabel = CreateWindowExW(0, L"STATIC", L"DisplayPort (0x10):",
            WS_CHILD, 28, 198, 150, 20, hWnd, NULL, cs->hInstance, NULL);
        pState->hCmdDpEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | ES_AUTOHSCROLL | WS_TABSTOP,
            180, 196, 440, 24, hWnd, (HMENU)IDC_CMD_DP, cs->hInstance, NULL);

        pState->hCmdTypecLabel = CreateWindowExW(0, L"STATIC", L"USB Type-C (0x0F):",
            WS_CHILD, 28, 230, 150, 20, hWnd, NULL, cs->hInstance, NULL);
        pState->hCmdTypecEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | ES_AUTOHSCROLL | WS_TABSTOP,
            180, 228, 440, 24, hWnd, (HMENU)IDC_CMD_TYPEC, cs->hInstance, NULL);

        pState->hCmdHdmi1Label = CreateWindowExW(0, L"STATIC", L"HDMI 1 (0x11):",
            WS_CHILD, 28, 262, 150, 20, hWnd, NULL, cs->hInstance, NULL);
        pState->hCmdHdmi1Edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | ES_AUTOHSCROLL | WS_TABSTOP,
            180, 260, 440, 24, hWnd, (HMENU)IDC_CMD_HDMI1, cs->hInstance, NULL);

        pState->hCmdHdmi2Label = CreateWindowExW(0, L"STATIC", L"HDMI 2 (0x12):",
            WS_CHILD, 28, 294, 150, 20, hWnd, NULL, cs->hInstance, NULL);
        pState->hCmdHdmi2Edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | ES_AUTOHSCROLL | WS_TABSTOP,
            180, 292, 440, 24, hWnd, (HMENU)IDC_CMD_HDMI2, cs->hInstance, NULL);

        pState->hGenCmdHint = CreateWindowExW(0, L"STATIC", L"提示：与 macOS 端快捷与联动功能对齐，支持系统命令、脚本或快捷调用（例如唤醒从机、执行 ssh/curl 等），通过后台静默异步执行。",
            WS_CHILD, 28, 330, 590, 38, hWnd, NULL, cs->hInstance, NULL);

        // 初始化 Tab 1 勾选项与联动内容
        SendMessageW(pState->hGenNotifyCheck, BM_SETCHECK, pState->tempConfig.notify ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(pState->hGenAutoStartCheck, BM_SETCHECK, IsAutoStartEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);

        DWORD dpCode = 16;
        for (const auto& kv : pState->tempConfig.customInputs) {
            if (ToUpper(kv.first) == L"DP" || ToUpper(kv.first) == L"DP1") { dpCode = kv.second; break; }
        }
        DWORD tcCode = pState->detectedTypeCCode;
        for (const auto& kv : pState->tempConfig.customInputs) {
            if (ToUpper(kv.first) == L"TYPEC" || ToUpper(kv.first) == L"TYPE-C" || ToUpper(kv.first) == L"USBC") { tcCode = kv.second; break; }
        }

        auto getCmd = [&](DWORD code) -> std::wstring {
            auto it = pState->tempConfig.commands.find(code);
            return (it != pState->tempConfig.commands.end()) ? it->second : L"";
        };
        SetWindowTextW(pState->hCmdDpEdit, getCmd(dpCode).c_str());
        SetWindowTextW(pState->hCmdTypecEdit, getCmd(tcCode).c_str());
        SetWindowTextW(pState->hCmdHdmi1Edit, getCmd(17).c_str());
        SetWindowTextW(pState->hCmdHdmi2Edit, getCmd(18).c_str());

        // 4. 底部常驻按钮
        pState->hBtnOpenIni = CreateWindowExW(0, L"BUTTON", L"📂 打开配置文件 (config.ini)",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            15, 442, 195, 28, hWnd, (HMENU)IDC_SETTINGS_BTN_OPEN_INI, cs->hInstance, NULL);

        pState->hBtnSave = CreateWindowExW(0, L"BUTTON", L"保存",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
            440, 442, 90, 28, hWnd, (HMENU)IDOK, cs->hInstance, NULL);

        pState->hBtnCancel = CreateWindowExW(0, L"BUTTON", L"取消",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            542, 442, 90, 28, hWnd, (HMENU)IDCANCEL, cs->hInstance, NULL);

        // 统一设置现代化字体
        EnumChildWindows(hWnd, SetChildFontProc, (LPARAM)pState->hFont);
        if (pState->hPresetMonHeader && pState->hBoldFont) SendMessageW(pState->hPresetMonHeader, WM_SETFONT, (WPARAM)pState->hBoldFont, TRUE);
        if (pState->hGenCmdHeader && pState->hBoldFont) SendMessageW(pState->hGenCmdHeader, WM_SETFONT, (WPARAM)pState->hBoldFont, TRUE);

        // 填充并选中预设
        RefreshPresetListbox(pState, 0);
        UpdateSettingsTabVisibility(pState);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdcStatic = (HDC)wParam;
        SetBkColor(hdcStatic, GetSysColor(COLOR_BTNFACE));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }

    case WM_NOTIFY: {
        NMHDR* pNmhdr = reinterpret_cast<NMHDR*>(lParam);
        if (pNmhdr->idFrom == IDC_SETTINGS_TAB && pNmhdr->code == TCN_SELCHANGE) {
            SaveRightPanelToPreset(pState);
            UpdateSettingsTabVisibility(pState);
            return 0;
        }
        break;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        if (id == IDC_PRESET_LIST && code == LBN_SELCHANGE) {
            SaveRightPanelToPreset(pState);
            int sel = (int)SendMessageW(pState->hPresetList, LB_GETCURSEL, 0, 0);
            LoadPresetToRightPanel(pState, sel);
            return 0;
        }

        if (id == IDC_PRESET_NAME_EDIT && code == EN_CHANGE && !pState->loadingPreset) {
            if (pState->currentPresetIndex >= 0 && pState->currentPresetIndex < (int)pState->tempConfig.presets.size()) {
                wchar_t buf[256] = { 0 };
                GetWindowTextW(pState->hPresetNameEdit, buf, 256);
                pState->tempConfig.presets[pState->currentPresetIndex].name = buf;

                int curSel = pState->currentPresetIndex;
                SendMessageW(pState->hPresetList, LB_DELETESTRING, curSel, 0);
                SendMessageW(pState->hPresetList, LB_INSERTSTRING, curSel, (LPARAM)buf);
                SendMessageW(pState->hPresetList, LB_SETCURSEL, curSel, 0);
            }
            return 0;
        }

        if (id >= IDC_PRESET_MON_BASE && id < IDC_PRESET_MON_BASE + 32 && code == CBN_SELCHANGE) {
            SaveRightPanelToPreset(pState);
            return 0;
        }

        if (id == IDC_PRESET_ADD) {
            SaveRightPanelToPreset(pState);
            MonitorPreset newPr;
            std::wstring baseName = L"新建预设";
            int suffix = 1;
            bool nameExists = true;
            while (nameExists) {
                nameExists = false;
                std::wstring checkName = (suffix == 1) ? baseName : (baseName + L" " + std::to_wstring(suffix));
                for (const auto& p : pState->tempConfig.presets) {
                    if (p.name == checkName) {
                        nameExists = true;
                        suffix++;
                        break;
                    }
                }
                if (!nameExists) {
                    newPr.name = checkName;
                }
            }

            if (pState->pCtx) {
                for (size_t i = 0; i < pState->pCtx->monitors.size(); ++i) {
                    newPr.targets.push_back({ std::to_wstring(i + 1), pState->pCtx->monitors[i].currentInput });
                }
            }
            pState->tempConfig.presets.push_back(newPr);
            int newIdx = static_cast<int>(pState->tempConfig.presets.size()) - 1;
            RefreshPresetListbox(pState, newIdx);
            SetFocus(pState->hPresetNameEdit);
            SendMessageW(pState->hPresetNameEdit, EM_SETSEL, 0, -1);
            return 0;
        }

        if (id == IDC_PRESET_DEL) {
            if (pState->currentPresetIndex >= 0 && pState->currentPresetIndex < (int)pState->tempConfig.presets.size()) {
                int idx = pState->currentPresetIndex;
                pState->tempConfig.presets.erase(pState->tempConfig.presets.begin() + idx);
                int nextSel = idx;
                if (nextSel >= (int)pState->tempConfig.presets.size()) {
                    nextSel = (int)pState->tempConfig.presets.size() - 1;
                }
                RefreshPresetListbox(pState, nextSel);
            }
            return 0;
        }

        if (id == IDC_PRESET_UP) {
            if (pState->currentPresetIndex > 0 && pState->currentPresetIndex < (int)pState->tempConfig.presets.size()) {
                SaveRightPanelToPreset(pState);
                int idx = pState->currentPresetIndex;
                std::swap(pState->tempConfig.presets[idx], pState->tempConfig.presets[idx - 1]);
                RefreshPresetListbox(pState, idx - 1);
            }
            return 0;
        }

        if (id == IDC_PRESET_DOWN) {
            if (pState->currentPresetIndex >= 0 && pState->currentPresetIndex + 1 < (int)pState->tempConfig.presets.size()) {
                SaveRightPanelToPreset(pState);
                int idx = pState->currentPresetIndex;
                std::swap(pState->tempConfig.presets[idx], pState->tempConfig.presets[idx + 1]);
                RefreshPresetListbox(pState, idx + 1);
            }
            return 0;
        }

        if (id == IDC_SETTINGS_BTN_OPEN_INI) {
            std::wstring cfgPath = GetConfigPath();
            EnsureConfigFile(cfgPath);
            ShellExecuteW(hWnd, L"open", cfgPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }

        if (id == IDOK) {
            SaveRightPanelToPreset(pState);

            pState->tempConfig.notify = (SendMessageW(pState->hGenNotifyCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);

            bool autostartDesired = (SendMessageW(pState->hGenAutoStartCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
            if (autostartDesired != IsAutoStartEnabled()) {
                SetAutoStart(autostartDesired);
            }

            int modeSel = (int)SendMessageW(pState->hGenModeCombo, CB_GETCURSEL, 0, 0);
            if (modeSel == 0) pState->tempConfig.mode = L"TRAY";
            else if (modeSel == 1) pState->tempConfig.mode = L"MENU";
            else if (modeSel == 2) pState->tempConfig.mode = L"TOGGLE";

            int targetSel = (int)SendMessageW(pState->hGenTargetCombo, CB_GETCURSEL, 0, 0);
            if (targetSel == 0) pState->tempConfig.targetMonitor = L"PRIMARY";
            else if (targetSel == 1) pState->tempConfig.targetMonitor = L"ALL";

            auto getEditText = [](HWND hEdit) -> std::wstring {
                wchar_t buf[1024] = { 0 };
                GetWindowTextW(hEdit, buf, 1024);
                return Trim(buf);
            };

            DWORD dpCode = 16;
            for (const auto& kv : pState->tempConfig.customInputs) {
                if (ToUpper(kv.first) == L"DP" || ToUpper(kv.first) == L"DP1") { dpCode = kv.second; break; }
            }
            DWORD tcCode = pState->detectedTypeCCode;
            for (const auto& kv : pState->tempConfig.customInputs) {
                if (ToUpper(kv.first) == L"TYPEC" || ToUpper(kv.first) == L"TYPE-C" || ToUpper(kv.first) == L"USBC") { tcCode = kv.second; break; }
            }

            pState->tempConfig.commands[dpCode] = getEditText(pState->hCmdDpEdit);
            pState->tempConfig.commands[tcCode] = getEditText(pState->hCmdTypecEdit);
            pState->tempConfig.commands[17] = getEditText(pState->hCmdHdmi1Edit);
            pState->tempConfig.commands[18] = getEditText(pState->hCmdHdmi2Edit);

            std::wstring cfgPath = GetConfigPath();
            SaveConfigToFile(cfgPath, pState->tempConfig, pState->detectedTypeCCode);

            pState->saved = true;
            DestroyWindow(hWnd);
            return 0;
        }

        if (id == IDCANCEL) {
            pState->saved = false;
            DestroyWindow(hWnd);
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        pState->saved = false;
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        g_hSettingsDlg = NULL;
        if (pState->hFont) {
            DeleteObject(pState->hFont);
            pState->hFont = NULL;
        }
        if (pState->hBoldFont) {
            DeleteObject(pState->hBoldFont);
            pState->hBoldFont = NULL;
        }
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// 唤起图形化设置窗口
bool ShowSettingsDialog(HINSTANCE hInstance, HWND hParent, AppConfig& cfg, MonitorContext& ctx, DWORD detectedTypeCCode) {
    if (g_hSettingsDlg && IsWindow(g_hSettingsDlg)) {
        ShowWindow(g_hSettingsDlg, SW_RESTORE);
        SetForegroundWindow(g_hSettingsDlg);
        return false;
    }

    INITCOMMONCONTROLSEX icex = { sizeof(INITCOMMONCONTROLSEX) };
    icex.dwICC = ICC_TAB_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    WNDCLASSEXW wc = { sizeof(wc) };
    if (!GetClassInfoExW(hInstance, L"KVMSwitchSettingsDlgClass", &wc)) {
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = SettingsDlgProc;
        wc.hInstance = hInstance;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"KVMSwitchSettingsDlgClass";
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        RegisterClassExW(&wc);
    }

    SettingsDialogState state;
    state.hInstance = hInstance;
    state.tempConfig = cfg;
    state.pCtx = &ctx;
    state.detectedTypeCCode = detectedTypeCCode;

    int dlgW = 660;
    int dlgH = 525;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenW - dlgW) / 2;
    int posY = (screenH - dlgH) / 2;

    HWND hWndParent = (hParent && IsWindow(hParent)) ? hParent : NULL;

    HWND hDlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"KVMSwitchSettingsDlgClass",
        L"KVMSwitch 设置",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        posX, posY, dlgW, dlgH, hWndParent, NULL, hInstance, &state);

    if (!hDlg) return false;

    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);

    if (hWndParent) EnableWindow(hWndParent, FALSE);
    SetForegroundWindow(hDlg);

    MSG msg;
    while (IsWindow(hDlg) && GetMessageW(&msg, NULL, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            SendMessageW(hDlg, WM_COMMAND, IDCANCEL, 0);
            continue;
        }
        if (IsDialogMessageW(hDlg, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hWndParent && IsWindow(hWndParent)) {
        EnableWindow(hWndParent, TRUE);
        SetForegroundWindow(hWndParent);
    }

    if (state.saved) {
        cfg = state.tempConfig;
        if (g_hTrayWnd && IsWindow(g_hTrayWnd)) {
            UpdateTrayTooltip(g_hTrayWnd, &cfg);
            ShowTrayBalloon(L"KVMSwitch 设置", L"设置已成功保存并生效。");
        }
    }
    return state.saved;
}

// 弹出快捷菜单
void ShowQuickMenu(HINSTANCE hInstance, const AppConfig& cfg, MonitorContext& ctx, DWORD detectedTypeCCode, HWND hOwnerWnd = NULL) {
    UNREFERENCED_PARAMETER(hOwnerWnd);

    // 创建菜单接收窗口与菜单
    WNDCLASSEXW wc = { sizeof(wc) };
    if (!GetClassInfoExW(hInstance, L"KVMSwitchMenuDummyClass", &wc)) {
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = hInstance;
        wc.lpszClassName = L"KVMSwitchMenuDummyClass";
        RegisterClassExW(&wc);
    }

    HWND hWnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"KVMSwitchMenuDummyClass", L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (!hWnd) return;

    HMENU hMenu = CreatePopupMenu();

    // 1. 预设方案 (Presets - 与 macOS 端对齐，顶部快捷入口)
    std::map<UINT, size_t> presetMenuIdToIdx;
    if (!cfg.presets.empty()) {
        AppendMenuW(hMenu, MF_STRING | MF_DISABLED, 0, L"📐 多显示器预设方案");
        for (size_t i = 0; i < cfg.presets.size(); ++i) {
            UINT pid = ID_PRESET_BASE + static_cast<UINT>(i);
            presetMenuIdToIdx[pid] = i;
            std::wstring itemText = L"   ▶ " + cfg.presets[i].name;
            AppendMenuW(hMenu, MF_STRING, pid, itemText.c_str());
        }
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    }

    // 2. 轮流切换 (Toggle)
    AppendMenuW(hMenu, MF_STRING, ID_ACTION_TOGGLE, L"🔄 轮流切换输入源 (Toggle)");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    // 3. 显示器与输入源列表
    std::map<UINT, DWORD> singleMenuIdToCode;
    UINT singleInputBase = ID_INPUT_BASE;

    struct MonInputTarget {
        size_t monIndex;
        DWORD code;
    };
    std::map<UINT, MonInputTarget> multiMenuIdToTarget;
    UINT multiInputBase = ID_MONITOR_INPUT_BASE;

    if (ctx.monitors.empty()) {
        AppendMenuW(hMenu, MF_STRING | MF_DISABLED, 0, L"未检测到外接显示器");
    } else if (ctx.monitors.size() == 1) {
        // 单显示器: 直接展开输入源
        const auto& mon = ctx.monitors[0];
        DWORD curInput = mon.currentInput;
        std::wstring curName = curInput ? GetInputName(curInput, &cfg) : L"未知";

        std::wstring headerStr = L"🖥️ " + mon.description + L" [当前: " + curName + L"]";
        AppendMenuW(hMenu, MF_STRING | MF_DISABLED, 0, headerStr.c_str());

        std::vector<DWORD> inputCodes = GetSupportedInputCodes(&mon, detectedTypeCCode);
        for (DWORD code : inputCodes) {
            std::wstring iname = GetInputName(code, &cfg);
            wchar_t itemText[128];
            swprintf_s(itemText, L"   %s  (0x%02X)", iname.c_str(), code);

            UINT flags = MF_STRING;
            if (code == curInput) {
                flags |= MF_CHECKED;
            }
            AppendMenuW(hMenu, flags, singleInputBase, itemText);
            singleMenuIdToCode[singleInputBase] = code;
            singleInputBase++;
        }
    } else {
        // 多显示器: 每台显示器拥有独立的子菜单 (与 macOS 保持一致)
        for (size_t mIdx = 0; mIdx < ctx.monitors.size(); ++mIdx) {
            const auto& mon = ctx.monitors[mIdx];
            DWORD curInput = mon.currentInput;
            std::wstring curName = curInput ? GetInputName(curInput, &cfg) : L"未知";

            HMENU hSubMenu = CreatePopupMenu();
            std::vector<DWORD> inputCodes = GetSupportedInputCodes(&mon, detectedTypeCCode);

            for (DWORD code : inputCodes) {
                std::wstring iname = GetInputName(code, &cfg);
                wchar_t itemText[128];
                swprintf_s(itemText, L"   %s  (0x%02X)", iname.c_str(), code);

                UINT flags = MF_STRING;
                if (code == curInput) {
                    flags |= MF_CHECKED;
                }
                AppendMenuW(hSubMenu, flags, multiInputBase, itemText);
                multiMenuIdToTarget[multiInputBase] = { mIdx, code };
                multiInputBase++;
            }

            std::wstring monItemTitle = L"🖥️ #" + std::to_wstring(mIdx + 1) + L" " + mon.description +
                (mon.isPrimary ? L" (主)" : L"") + L"  [" + curName + L"]";
            AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hSubMenu, monItemTitle.c_str());
        }
    }

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_ACTION_SETTINGS, L"⚙️ 设置... (Settings)");
    AppendMenuW(hMenu, MF_STRING, ID_ACTION_SAVE_PRESET, L"💾 保存当前状态为多显示器预设...");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_ACTION_SHORTCUTS, L"🔗 创建桌面快捷方式 (一键切换)");
    AppendMenuW(hMenu, MF_STRING, ID_ACTION_CONFIG, L"📄 打开配置文件 (config.ini)");
    AppendMenuW(hMenu, MF_STRING, ID_ACTION_QUERY, L"🔍 检测显示器信息与输入状态");

    // 开机自启动选项
    UINT autoStartFlags = MF_STRING;
    if (IsAutoStartEnabled()) {
        autoStartFlags |= MF_CHECKED;
    }
    AppendMenuW(hMenu, autoStartFlags, ID_ACTION_AUTOSTART, L"🚀 开机自启动 (常驻系统托盘)");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_ACTION_EXIT, L"✕ 退出");

    // 弹出菜单并保证失焦自动隐藏 (遵循 Microsoft 托盘菜单准则)
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hWnd);

    UINT cmd = TrackPopupMenuEx(hMenu, TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_RETURNCMD, pt.x, pt.y, hWnd, NULL);
    PostMessageW(hWnd, WM_NULL, 0, 0);

    DestroyMenu(hMenu);
    DestroyWindow(hWnd);

    // 处理菜单动作
    if (cmd == ID_ACTION_SETTINGS) {
        AppConfig latestCfg = LoadConfig(GetConfigPath(), detectedTypeCCode);
        if (ShowSettingsDialog(hInstance, (hOwnerWnd && IsWindow(hOwnerWnd)) ? hOwnerWnd : NULL, latestCfg, ctx, detectedTypeCCode)) {
            if (g_hTrayWnd && IsWindow(g_hTrayWnd)) {
                PostMessageW(g_hTrayWnd, g_wmRefresh, 0, 0);
            }
        }
    } else if (presetMenuIdToIdx.count(cmd)) {
        size_t pIdx = presetMenuIdToIdx[cmd];
        if (pIdx < cfg.presets.size()) {
            ApplyPreset(hInstance, cfg.presets[pIdx], cfg, ctx);
        }
    } else if (singleMenuIdToCode.count(cmd)) {
        DWORD chosen = singleMenuIdToCode[cmd];
        DoSwitch(hInstance, chosen, cfg, ctx);
    } else if (multiMenuIdToTarget.count(cmd)) {
        auto target = multiMenuIdToTarget[cmd];
        if (target.monIndex < ctx.monitors.size()) {
            const auto& mon = ctx.monitors[target.monIndex];
            if (SetMonitorInputSource(mon.hPhysicalMonitor, target.code)) {
                if (cfg.notify) {
                    std::wstring iname = GetInputName(target.code, &cfg);
                    std::wstring tip = L"显示器 #" + std::to_wstring(target.monIndex + 1) + L" 输入源已切换至: " + iname;
                    if (g_hTrayWnd && IsWindow(g_hTrayWnd)) {
                        ShowTrayBalloon(L"KVMSwitch 输入切换", tip);
                    } else {
                        ShowNotification(hInstance, L"KVMSwitch 输入切换", tip);
                    }
                }
                if (g_hTrayWnd && IsWindow(g_hTrayWnd)) {
                    UpdateTrayTooltip(g_hTrayWnd, &cfg);
                }
            } else {
                MessageBoxW(NULL, L"切换指定显示器输入源失败！请检查 DDC/CI 设置与线缆连接。", L"KVMSwitch 错误", MB_OK | MB_ICONERROR);
            }
        }
    } else if (cmd == ID_ACTION_SAVE_PRESET) {
        SaveCurrentStateAsPreset(hInstance, ctx, &cfg, detectedTypeCCode);
    } else if (cmd == ID_ACTION_TOGGLE) {
        DoToggle(hInstance, cfg, ctx);
    } else if (cmd == ID_ACTION_SHORTCUTS) {
        CreateAllShortcuts(&cfg);
    } else if (cmd == ID_ACTION_CONFIG) {
        std::wstring cfgPath = GetConfigPath();
        EnsureConfigFile(cfgPath);
        ShellExecuteW(NULL, L"open", cfgPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
    } else if (cmd == ID_ACTION_QUERY) {
        ShowQueryInfo(ctx, &cfg);
    } else if (cmd == ID_ACTION_AUTOSTART) {
        bool enabled = IsAutoStartEnabled();
        SetAutoStart(!enabled);
        if (g_hTrayWnd && IsWindow(g_hTrayWnd)) {
            ShowTrayBalloon(L"KVMSwitch 开机自启动", !enabled ? L"已开启开机自启动 (常驻系统托盘)。" : L"已关闭开机自启动。");
        } else {
            MessageBoxW(NULL, !enabled ? L"已开启开机自启动 (常驻系统托盘)。" : L"已关闭开机自启动。", L"开机自启动设置", MB_OK | MB_ICONINFORMATION);
        }
    } else if (cmd == ID_ACTION_EXIT) {
        if (g_hTrayWnd && IsWindow(g_hTrayWnd)) {
            DestroyWindow(g_hTrayWnd);
        }
    }
}

// 轮换切换动作
void DoToggle(HINSTANCE hInstance, const AppConfig& cfg, MonitorContext& ctx) {
    const PhysicalMonEntry* targetMon = nullptr;
    for (const auto& m : ctx.monitors) {
        if (m.isPrimary) {
            targetMon = &m;
            break;
        }
    }
    if (!targetMon && !ctx.monitors.empty()) {
        targetMon = &ctx.monitors[0];
    }

    DWORD curInput = targetMon ? targetMon->currentInput : 0;
    DWORD nextCode = cfg.toggleInputs.empty() ? 16 : cfg.toggleInputs[0];

    auto it = std::find(cfg.toggleInputs.begin(), cfg.toggleInputs.end(), curInput);
    if (it != cfg.toggleInputs.end()) {
        size_t idx = std::distance(cfg.toggleInputs.begin(), it);
        nextCode = cfg.toggleInputs[(idx + 1) % cfg.toggleInputs.size()];
    }

    DoSwitch(hInstance, nextCode, cfg, ctx);
}

// 托盘窗口过程与常驻主循环
static AppConfig g_trayCfg;
static DWORD g_detectedTypeCCode = 15;

LRESULT CALLBACK TrayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_wmTaskbarCreated) {
        // Explorer 重启时重新添加托盘图标
        EnsureTrayIconRegistered();
        UpdateTrayTooltip(hWnd, &g_trayCfg);
        return 0;
    }
    if (msg == g_wmShowMenu) {
        g_trayCfg = LoadConfig(GetConfigPath(), g_detectedTypeCCode);
        MonitorContext ctx = QueryAllMonitors();
        ShowQuickMenu(g_hInstance, g_trayCfg, ctx, g_detectedTypeCCode, hWnd);
        FreeMonitorContext(ctx);
        return 0;
    }
    if (msg == g_wmRefresh) {
        g_trayCfg = LoadConfig(GetConfigPath(), g_detectedTypeCCode);
        UpdateTrayTooltip(hWnd, &g_trayCfg);
        return 0;
    }
    if (msg == g_wmShowSettings) {
        g_trayCfg = LoadConfig(GetConfigPath(), g_detectedTypeCCode);
        MonitorContext ctx = QueryAllMonitors();
        ShowSettingsDialog(g_hInstance, hWnd, g_trayCfg, ctx, g_detectedTypeCCode);
        FreeMonitorContext(ctx);
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        return 0;

    case WM_TRAYNOTIFY:
        if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
            g_trayCfg = LoadConfig(GetConfigPath(), g_detectedTypeCCode);
            MonitorContext ctx = QueryAllMonitors();
            ShowQuickMenu(g_hInstance, g_trayCfg, ctx, g_detectedTypeCCode, hWnd);
            FreeMonitorContext(ctx);
        }
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_ID_REFRESH) {
            // 每次刷新前自检图标是否还在 (Explorer 偶发吞图标)
            EnsureTrayIconRegistered();
            UpdateTrayTooltip(hWnd, &g_trayCfg);
        }
        return 0;

    case WM_POWERBROADCAST:
        UpdateTrayTooltip(hWnd, &g_trayCfg);
        return TRUE;

    case WM_DISPLAYCHANGE:
        // 分辨率/显示器拓扑变化, capabilities 缓存可能不再对应实际接管的显示器
        g_capsCache.clear();
        break;

    case WM_DESTROY:
        KillTimer(hWnd, TIMER_ID_REFRESH);
        Shell_NotifyIconW(NIM_DELETE, &g_trayNid);
        g_hTrayWnd = NULL;
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int RunTrayApp(HINSTANCE hInstance, const AppConfig& cfg, DWORD detectedTypeCCode) {
    g_hInstance = hInstance;
    g_trayCfg = cfg;
    g_detectedTypeCCode = detectedTypeCCode;

    // 注册任务栏与进程间通信自定义消息
    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_wmShowMenu = RegisterWindowMessageW(L"KVMSwitch_ShowMenu");
    g_wmRefresh = RegisterWindowMessageW(L"KVMSwitch_Refresh");
    g_wmShowSettings = RegisterWindowMessageW(L"KVMSwitch_ShowSettings");

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"KVMSwitchTrayWindowClass";
    RegisterClassExW(&wc);

    g_hTrayWnd = CreateWindowExW(0, wc.lpszClassName, L"KVMSwitchTrayWindow", WS_OVERLAPPED, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (!g_hTrayWnd) return 1;

    memset(&g_trayNid, 0, sizeof(g_trayNid));
    g_trayNid.cbSize = sizeof(NOTIFYICONDATAW);
    g_trayNid.hWnd = g_hTrayWnd;
    g_trayNid.uID = 1;
    g_trayNid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_trayNid.uCallbackMessage = WM_TRAYNOTIFY;
    g_trayNid.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(ID_KVMSWITCH));
    if (!g_trayNid.hIcon) {
        g_trayNid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    }
    wcscpy_s(g_trayNid.szTip, L"KVMSwitch - 显示器输入切换");
    if (!EnsureTrayIconRegistered()) {
        // 首次注册失败不放弃: 定时器会持续重试
        OutputDebugStringW(L"KVMSwitch: initial tray icon registration failed\n");
    }

    UpdateTrayTooltip(g_hTrayWnd, &g_trayCfg);

    // 开启定时器，每 15 秒更新一次托盘提示 (当前输入状态)
    SetTimer(g_hTrayWnd, TIMER_ID_REFRESH, 15000, NULL);

    // 启动通知气泡
    ShowTrayBalloon(L"KVMSwitch", L"已常驻系统托盘，单击或右击托盘图标可快速切换输入源。");

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnregisterClassW(wc.lpszClassName, hInstance);
    return 0;
}


static HANDLE g_hStdOut = NULL;

void InitConsoleOutput() {
    // 优先检查标准输出是否已经被重定向 (管道或文件)
    HANDLE hStd = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hStd != NULL && hStd != INVALID_HANDLE_VALUE) {
        g_hStdOut = hStd;
        return;
    }
    // 否则尝试附加到父进程控制台
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        g_hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (g_hStdOut == INVALID_HANDLE_VALUE || g_hStdOut == NULL) {
            g_hStdOut = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            SetStdHandle(STD_OUTPUT_HANDLE, g_hStdOut);
        }
    }
}

void PrintOutput(const std::wstring& str, const wchar_t* title) {
    if (g_hStdOut != NULL && g_hStdOut != INVALID_HANDLE_VALUE) {
        DWORD dwMode = 0;
        if (GetConsoleMode(g_hStdOut, &dwMode)) {
            // 控制台输出
            DWORD written = 0;
            WriteConsoleW(g_hStdOut, str.c_str(), (DWORD)str.length(), &written, NULL);
            WriteConsoleW(g_hStdOut, L"\r\n", 2, &written, NULL);
            return;
        } else {
            // 管道或文件重定向输出 (UTF-8)
            int utf8Len = WideCharToMultiByte(CP_UTF8, 0, str.c_str(), (int)str.length(), NULL, 0, NULL, NULL);
            if (utf8Len > 0) {
                std::vector<char> utf8(utf8Len);
                WideCharToMultiByte(CP_UTF8, 0, str.c_str(), (int)str.length(), utf8.data(), utf8Len, NULL, NULL);
                DWORD written = 0;
                WriteFile(g_hStdOut, utf8.data(), (DWORD)utf8.size(), &written, NULL);
                WriteFile(g_hStdOut, "\r\n", 2, &written, NULL);
                return;
            }
        }
    }

    MessageBoxW(NULL, str.c_str(), title ? title : L"KVMSwitch", MB_OK | MB_ICONINFORMATION);
}

void ShowQueryInfo(const MonitorContext& ctx, const AppConfig* pCfg) {
    std::wstringstream ss;
    ss << L"检测到 " << ctx.monitors.size() << L" 台物理显示器：\n\n";
    for (size_t i = 0; i < ctx.monitors.size(); ++i) {
        const auto& m = ctx.monitors[i];
        ss << L"[" << (i + 1) << L"] " << m.description << (m.isPrimary ? L" (主显示器)\n" : L"\n");
        ss << L"   设备名: " << m.deviceName << L"\n";
        ss << L"   当前输入: " << GetInputName(m.currentInput, pCfg) << L" (代码: " << m.currentInput << L", 0x" << std::hex << m.currentInput << std::dec << L")\n";
        if (!m.supportedInputs.empty()) {
            ss << L"   支持的输入源: ";
            for (DWORD sc : m.supportedInputs) {
                ss << GetInputName(sc, pCfg) << L"(0x" << std::hex << sc << std::dec << L") ";
            }
            ss << L"\n";
        }
        ss << L"\n";
    }

    PrintOutput(ss.str(), L"显示器 DDC/CI 信息");
}

// 显示帮助
void ShowHelp() {
    std::wstring helpText =
        L"KVMSwitch - 便携式显示器输入源快速切换工具\n\n"
        L"命令行用法：\n"
        L"  KVMSwitch.exe                 按 config.ini 配置运行 (默认常驻系统托盘)\n"
        L"  KVMSwitch.exe --tray          启动并常驻系统托盘\n"
        L"  KVMSwitch.exe dp              直接切换到 DisplayPort (DP)\n"
        L"  KVMSwitch.exe hdmi1           直接切换到 HDMI 1\n"
        L"  KVMSwitch.exe hdmi2           直接切换到 HDMI 2\n"
        L"  KVMSwitch.exe typec           直接切换到 USB Type-C\n"
        L"  KVMSwitch.exe <数值>          直接切换到指定 VCP 60 数值 (如 15, 16, 17, 18)\n"
        L"  KVMSwitch.exe --preset <名称> (-p)  一键应用指定的多显示器预设方案\n"
        L"  KVMSwitch.exe --presets             列出所有已配置的多显示器预设方案\n"
        L"  KVMSwitch.exe --settings (-s)       打开图形化设置窗口 (预设、偏好与联动配置)\n"
        L"  KVMSwitch.exe --toggle (-t)   在常用输入源之间轮换切换\n"
        L"  KVMSwitch.exe --menu (-m)     强制弹出快速选择菜单 (单次模式)\n"
        L"  KVMSwitch.exe --query (-q)    检测并显示当前所有显示器及输入源状态\n"
        L"  KVMSwitch.exe --autostart-enable   开启开机自启动 (常驻系统托盘)\n"
        L"  KVMSwitch.exe --autostart-disable  关闭开机自启动\n"
        L"  KVMSwitch.exe --autostart-status   查询开机自启动状态\n"
        L"  KVMSwitch.exe --create-shortcuts   在桌面生成一键切换快捷方式\n"
        L"  KVMSwitch.exe --help (-h)     查看本帮助信息\n";

    PrintOutput(helpText, L"KVMSwitch 帮助");
}

// 主入口
int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(nCmdShow);

    // 优先尝试附加到调用者的控制台（如果在终端中运行）
    InitConsoleOutput();

    // 启用 Per-Monitor 高 DPI 感知，保证菜单在高分辨率屏下清晰
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 初始化通用控件库
    INITCOMMONCONTROLSEX icex = { sizeof(INITCOMMONCONTROLSEX) };
    icex.dwICC = ICC_TAB_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    // 注册进程间通信自定义消息
    g_wmShowMenu = RegisterWindowMessageW(L"KVMSwitch_ShowMenu");
    g_wmRefresh = RegisterWindowMessageW(L"KVMSwitch_Refresh");
    g_wmShowSettings = RegisterWindowMessageW(L"KVMSwitch_ShowSettings");

    // 单实例互斥量检测
    HANDLE hMutex = CreateMutexW(NULL, FALSE, L"Local\\KVMSwitch_SingleInstance_Mutex_zuoxinyu");
    bool alreadyRunning = (GetLastError() == ERROR_ALREADY_EXISTS);

    // 处理自启动控制命令行参数 (不依赖硬件查询，快速执行)
    std::wstring cmdLine = lpCmdLine ? lpCmdLine : L"";
    std::wstring trimmedCmd = ToUpper(Trim(cmdLine));

    if (trimmedCmd == L"--AUTOSTART-ENABLE") {
        SetAutoStart(true);
        PrintOutput(L"开机自启动已启用 (将在登录系统后自动常驻系统托盘)。", L"KVMSwitch");
        if (hMutex) CloseHandle(hMutex);
        return 0;
    } else if (trimmedCmd == L"--AUTOSTART-DISABLE") {
        SetAutoStart(false);
        PrintOutput(L"开机自启动已禁用。", L"KVMSwitch");
        if (hMutex) CloseHandle(hMutex);
        return 0;
    } else if (trimmedCmd == L"--AUTOSTART-STATUS") {
        bool enabled = IsAutoStartEnabled();
        PrintOutput(enabled ? L"开机自启动状态: 已启用 (Enabled)" : L"开机自启动状态: 已禁用 (Disabled)", L"KVMSwitch");
        if (hMutex) CloseHandle(hMutex);
        return 0;
    } else if (trimmedCmd == L"--HELP" || trimmedCmd == L"-H" || trimmedCmd == L"/?" || trimmedCmd == L"HELP") {
        ShowHelp();
        if (hMutex) CloseHandle(hMutex);
        return 0;
    } else if (trimmedCmd == L"--PRESETS") {
        std::wstring cfgPath = GetConfigPath();
        AppConfig cfg = LoadConfig(cfgPath, 15);
        if (cfg.presets.empty()) {
            PrintOutput(L"未配置任何预设方案。您可以在 config.ini 的 [Presets] 节添加，或在托盘菜单中保存当前状态为预设。", L"KVMSwitch 预设");
        } else {
            std::wstringstream ss;
            ss << L"当前共配置了 " << cfg.presets.size() << L" 个多显示器预设方案：\n\n";
            for (size_t i = 0; i < cfg.presets.size(); ++i) {
                const auto& pr = cfg.presets[i];
                ss << L"[" << (i + 1) << L"] " << pr.name << L":\n";
                for (const auto& t : pr.targets) {
                    std::wstring iname = (t.inputCode == 0) ? L"保持不变" : GetInputName(t.inputCode, &cfg);
                    ss << L"    显示器 " << t.monitorId << L" -> " << iname << L" (0x" << std::hex << t.inputCode << std::dec << L")\n";
                }
                ss << L"\n";
            }
            PrintOutput(ss.str(), L"KVMSwitch 预设方案列表");
        }
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    // 快速路径: 托盘已常驻且本次运行只是要唤起它的菜单或设置窗口时, 跳过耗时的显示器枚举直接转发
    // (完整枚举首次约 1.7 秒, 由常驻进程用它自己的缓存完成, 新进程立即退出)
    std::wstring cfgPath = GetConfigPath();
    {
        wchar_t modeBuf[16] = { 0 };
        GetPrivateProfileStringW(L"General", L"mode", L"tray", modeBuf, 16, cfgPath.c_str());
        std::wstring iniMode = ToUpper(Trim(modeBuf));

        bool settingsRequest = (trimmedCmd == L"--SETTINGS" || trimmedCmd == L"-S" || trimmedCmd == L"SETTINGS");
        if (alreadyRunning && settingsRequest) {
            HWND hTray = FindWindowW(L"KVMSwitchTrayWindowClass", NULL);
            if (hTray) {
                PostMessageW(hTray, g_wmShowSettings, 0, 0);
            }
            if (hMutex) CloseHandle(hMutex);
            return 0;
        }

        bool wakeRequest = (trimmedCmd == L"--TRAY" || trimmedCmd == L"-TRAY" || trimmedCmd == L"TRAY" ||
            (trimmedCmd.empty() && iniMode != L"TOGGLE" && iniMode != L"MENU"));
        if (alreadyRunning && wakeRequest) {
            HWND hTray = FindWindowW(L"KVMSwitchTrayWindowClass", NULL);
            if (hTray) {
                PostMessageW(hTray, g_wmShowMenu, 0, 0);
            }
            if (hMutex) CloseHandle(hMutex);
            return 0;
        }
    }

    // 枚举显示器
    MonitorContext ctx = QueryAllMonitors();
    if (ctx.monitors.empty()) {
        MessageBoxW(NULL,
            L"未检测到支持 DDC/CI 的显示器！\n\n请确认：\n1. 显示器 OSD 菜单中已开启 DDC/CI\n2. 显卡驱动已正常安装",
            L"KVMSwitch 错误", MB_OK | MB_ICONERROR);
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    // 检测 Type-C VCP 代码 (泰坦军团 P275MV 汇报 15 为 Type-C，16 为 DP；标准 VESA 为 27)
    DWORD detectedTypeCCode = 15;
    for (const auto& m : ctx.monitors) {
        if (!m.supportedInputs.empty()) {
            bool has15 = std::find(m.supportedInputs.begin(), m.supportedInputs.end(), 15) != m.supportedInputs.end();
            bool has27 = std::find(m.supportedInputs.begin(), m.supportedInputs.end(), 27) != m.supportedInputs.end();
            if (has15 && !has27) {
                detectedTypeCCode = 15;
            } else if (has27) {
                detectedTypeCCode = 27;
            }
            break;
        }
    }

    // 加载配置
    AppConfig cfg = LoadConfig(cfgPath, detectedTypeCCode);

    int exitCode = 0;

    if (trimmedCmd == L"--TRAY" || trimmedCmd == L"-TRAY" || trimmedCmd == L"TRAY") {
        // alreadyRunning 的情况已在前面快速路径中转发并返回, 走到这里说明本进程就是首个实例
        FreeMonitorContext(ctx);
        exitCode = RunTrayApp(hInstance, cfg, detectedTypeCCode);
        if (hMutex) CloseHandle(hMutex);
        return exitCode;
    } else if (trimmedCmd.empty()) {
        // 无命令行参数运行
        if (cfg.mode == L"TRAY") {
            FreeMonitorContext(ctx);
            exitCode = RunTrayApp(hInstance, cfg, detectedTypeCCode);
            if (hMutex) CloseHandle(hMutex);
            return exitCode;
        } else if (cfg.mode == L"TOGGLE") {
            DoToggle(hInstance, cfg, ctx);
            if (alreadyRunning) {
                HWND hTray = FindWindowW(L"KVMSwitchTrayWindowClass", NULL);
                if (hTray) PostMessageW(hTray, g_wmRefresh, 0, 0);
            }
        } else {
            ShowQuickMenu(hInstance, cfg, ctx, detectedTypeCCode);
        }
    } else if (trimmedCmd == L"--MENU" || trimmedCmd == L"-M" || trimmedCmd == L"MENU") {
        ShowQuickMenu(hInstance, cfg, ctx, detectedTypeCCode);
    } else if (trimmedCmd == L"--TOGGLE" || trimmedCmd == L"-T" || trimmedCmd == L"TOGGLE") {
        DoToggle(hInstance, cfg, ctx);
        if (alreadyRunning) {
            HWND hTray = FindWindowW(L"KVMSwitchTrayWindowClass", NULL);
            if (hTray) PostMessageW(hTray, g_wmRefresh, 0, 0);
        }
    } else if (trimmedCmd == L"--CREATE-SHORTCUTS" || trimmedCmd == L"SHORTCUTS") {
        CreateAllShortcuts(&cfg);
    } else if (trimmedCmd == L"--SETTINGS" || trimmedCmd == L"-S" || trimmedCmd == L"SETTINGS") {
        if (ShowSettingsDialog(hInstance, NULL, cfg, ctx, detectedTypeCCode)) {
            if (alreadyRunning) {
                HWND hTray = FindWindowW(L"KVMSwitchTrayWindowClass", NULL);
                if (hTray) PostMessageW(hTray, g_wmRefresh, 0, 0);
            }
        }
    } else if (trimmedCmd == L"--QUERY" || trimmedCmd == L"-Q" || trimmedCmd == L"QUERY") {
        ShowQueryInfo(ctx, &cfg);
    } else {
        // 检查是否为 --preset <name> 或 -p <name>
        bool isPresetCmd = false;
        std::wstring presetArg;
        if (trimmedCmd.rfind(L"--PRESET", 0) == 0 || trimmedCmd.rfind(L"-P", 0) == 0) {
            std::wstring raw = Trim(cmdLine);
            size_t spacePos = raw.find_first_of(L" =:\t");
            if (spacePos != std::wstring::npos) {
                presetArg = Trim(raw.substr(spacePos + 1));
                if (presetArg.length() >= 2 && presetArg.front() == L'\"' && presetArg.back() == L'\"') {
                    presetArg = presetArg.substr(1, presetArg.length() - 2);
                }
                isPresetCmd = true;
            }
        }

        const MonitorPreset* pPresetFound = nullptr;
        if (isPresetCmd) {
            std::wstring targetNameUpper = ToUpper(presetArg);
            for (const auto& pr : cfg.presets) {
                if (ToUpper(pr.name) == targetNameUpper) {
                    pPresetFound = &pr;
                    break;
                }
            }
            if (!pPresetFound) {
                for (const auto& pr : cfg.presets) {
                    if (ToUpper(pr.name).find(targetNameUpper) != std::wstring::npos) {
                        pPresetFound = &pr;
                        break;
                    }
                }
            }
        } else {
            // 尝试直接匹配预设名称 (例如直接运行 KVMSwitch.exe 办公模式)
            for (const auto& pr : cfg.presets) {
                if (ToUpper(pr.name) == trimmedCmd) {
                    pPresetFound = &pr;
                    isPresetCmd = true;
                    presetArg = pr.name;
                    break;
                }
            }
        }

        if (isPresetCmd) {
            if (pPresetFound) {
                ApplyPreset(hInstance, *pPresetFound, cfg, ctx);
                if (alreadyRunning) {
                    HWND hTray = FindWindowW(L"KVMSwitchTrayWindowClass", NULL);
                    if (hTray) PostMessageW(hTray, g_wmRefresh, 0, 0);
                }
            } else {
                std::wstring err = L"未找到名为「" + presetArg + L"」的多显示器预设方案！\n\n请检查 config.ini 中的 [Presets] 配置，或运行 KVMSwitch.exe --presets 查看列表。";
                PrintOutput(err, L"KVMSwitch 错误");
                exitCode = 1;
            }
        } else {
            // 直接指定目标输入源 (例如 dp, hdmi1, hdmi2, typec, 15, 17 等)
            std::wstring targetStr = trimmedCmd;
            if (targetStr.rfind(L"--", 0) == 0) targetStr = targetStr.substr(2);
            else if (targetStr.rfind(L"-", 0) == 0) targetStr = targetStr.substr(1);
            else if (targetStr.rfind(L"/", 0) == 0) targetStr = targetStr.substr(1);

            DWORD targetCode = ParseInputAlias(targetStr, cfg, detectedTypeCCode);
            if (targetCode > 0) {
                DoSwitch(hInstance, targetCode, cfg, ctx);
                if (alreadyRunning) {
                    HWND hTray = FindWindowW(L"KVMSwitchTrayWindowClass", NULL);
                    if (hTray) PostMessageW(hTray, g_wmRefresh, 0, 0);
                }
            } else {
                std::wstring err = L"无法识别的输入源或预设参数: " + cmdLine + L"\n\n支持的参数示例:\n• 输入源: dp, hdmi1, hdmi2, typec, 15, 16, 17\n• 预设方案: --preset 办公模式, --presets";
                PrintOutput(err, L"KVMSwitch 错误");
                exitCode = 1;
            }
        }
    }

    // 释放资源
    FreeMonitorContext(ctx);
    if (hMutex) CloseHandle(hMutex);
    return exitCode;
}

