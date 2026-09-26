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

struct MonitorContext;
struct AppConfig;
void ShowQueryInfo(const MonitorContext& ctx, const AppConfig* pCfg);

#pragma comment(lib, "dxva2.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

// Menu / Action IDs
#define ID_INPUT_BASE       2000
#define ID_ACTION_TOGGLE    3001
#define ID_ACTION_SHORTCUTS 3002
#define ID_ACTION_CONFIG    3003
#define ID_ACTION_QUERY     3004
#define ID_ACTION_EXIT      3005
#define ID_ACTION_AUTOSTART 3006

#define TIMER_ID_REFRESH    1001
#define WM_TRAYNOTIFY       (WM_USER + 101)

// 全局托盘相关变量
static NOTIFYICONDATAW g_trayNid = { sizeof(NOTIFYICONDATAW) };
static HWND g_hTrayWnd = NULL;
static HINSTANCE g_hInstance = NULL;
static UINT g_wmTaskbarCreated = 0;
static UINT g_wmShowMenu = 0;
static UINT g_wmRefresh = 0;

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

struct AppConfig {
    std::wstring mode;          // "menu" or "toggle"
    bool notify;                // 是否显示桌面切换提示
    std::wstring targetMonitor; // "primary" or "all"
    std::vector<DWORD> toggleInputs;
    std::map<std::wstring, DWORD> customInputs;
    std::map<DWORD, std::wstring> commands; // 切换后触发的命令
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
        L"[Commands]\r\n"
        L"; 切换到指定输入源后自动在后台执行的系统命令 (可选，留空则不执行)\r\n"
        L"; 例如切换到 Mac 时唤醒 Mac，切换回 PC 时联动等:\r\n"
        L"on_switch_to_typec = ssh mac caffeinate -u -t 2\r\n"
        L"on_switch_to_hdmi1 = ssh mac caffeinate -u -t 2\r\n"
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

AppConfig LoadConfig(const std::wstring& cfgPath, DWORD detectedTypeCCode = 15) {
    EnsureConfigFile(cfgPath);

    AppConfig cfg;
    wchar_t buf[512] = { 0 };

    GetPrivateProfileStringW(L"General", L"mode", L"tray", buf, 512, cfgPath.c_str());
    cfg.mode = ToUpper(Trim(buf));
    if (cfg.mode != L"TOGGLE" && cfg.mode != L"MENU" && cfg.mode != L"TRAY") {
        cfg.mode = L"TRAY";
    }

    GetPrivateProfileStringW(L"General", L"notify", L"true", buf, 512, cfgPath.c_str());
    std::wstring notifyStr = ToUpper(Trim(buf));
    cfg.notify = (notifyStr == L"TRUE" || notifyStr == L"1" || notifyStr == L"YES");

    GetPrivateProfileStringW(L"General", L"target_monitor", L"primary", buf, 512, cfgPath.c_str());
    cfg.targetMonitor = ToUpper(Trim(buf));

    // Custom Inputs
    wchar_t secBuf[2048] = { 0 };
    if (GetPrivateProfileSectionW(L"Inputs", secBuf, 2048, cfgPath.c_str()) > 0) {
        wchar_t* p = secBuf;
        while (*p) {
            std::wstring line = p;
            size_t eq = line.find(L'=');
            if (eq != std::wstring::npos) {
                std::wstring key = Trim(line.substr(0, eq));
                std::wstring val = Trim(line.substr(eq + 1));
                try {
                    DWORD code = (val.rfind(L"0X", 0) == 0 || val.rfind(L"0x", 0) == 0) ?
                        std::stoul(val, nullptr, 16) : std::stoul(val);
                    cfg.customInputs[key] = code;
                } catch (...) {}
            }
            p += wcslen(p) + 1;
        }
    }

    // Toggle Inputs
    GetPrivateProfileStringW(L"General", L"toggle_inputs", L"DP, HDMI1, TypeC", buf, 512, cfgPath.c_str());
    std::vector<std::wstring> parts = Split(buf, L',');
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

    std::vector<std::pair<std::wstring, DWORD>> aliasMap = {
        { L"on_switch_to_dp", dpCode },
        { L"on_switch_to_dp1", dpCode },
        { L"on_switch_to_typec", tcCode },
        { L"on_switch_to_usbc", tcCode },
        { L"on_switch_to_hdmi1", 17 },
        { L"on_switch_to_hdmi2", 18 },
        { L"on_switch_to_hdmi3", 19 },
    };

    for (const auto& item : aliasMap) {
        if (GetPrivateProfileStringW(L"Commands", item.first.c_str(), L"", buf, 512, cfgPath.c_str()) > 0) {
            std::wstring cmd = Trim(buf);
            if (!cmd.empty()) {
                cfg.commands[item.second] = cmd;
            }
        }
    }

    return cfg;
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

                // 读取 Capabilities
                DWORD capLen = 0;
                if (GetCapabilitiesStringLength(hPhys, &capLen) && capLen > 0) {
                    std::vector<char> capBuf(capLen + 2, 0);
                    if (CapabilitiesRequestAndCapabilitiesReply(hPhys, capBuf.data(), capLen)) {
                        entry.supportedInputs = ParseCapabilitiesInputCodes(capBuf.data());
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
void UpdateTrayTooltip(HWND hWnd, const AppConfig* pCfg = nullptr, DWORD forcedCurrentInput = 0) {
    if (!g_hTrayWnd || !IsWindow(g_hTrayWnd)) return;

    DWORD curInput = forcedCurrentInput;
    if (curInput == 0) {
        MonitorContext ctx = QueryAllMonitors();
        if (!ctx.monitors.empty()) {
            const PhysicalMonEntry* pMon = nullptr;
            for (const auto& m : ctx.monitors) {
                if (m.isPrimary) { pMon = &m; break; }
            }
            if (!pMon) pMon = &ctx.monitors[0];
            curInput = pMon->currentInput;
        }
        FreeMonitorContext(ctx);
    }

    std::wstring iname = curInput ? GetInputName(curInput, pCfg) : L"未知";
    wchar_t tip[128] = { 0 };
    swprintf_s(tip, L"KVMSwitch\n当前输入: %s (0x%02X)", iname.c_str(), curInput);
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

void CreateAllShortcuts() {
    CreateDesktopShortcut(L"切换显示器 - DP", L"dp", L"一键切换显示器至 DisplayPort 输入");
    CreateDesktopShortcut(L"切换显示器 - HDMI 1", L"hdmi1", L"一键切换显示器至 HDMI 1 输入");
    CreateDesktopShortcut(L"切换显示器 - HDMI 2", L"hdmi2", L"一键切换显示器至 HDMI 2 输入");
    CreateDesktopShortcut(L"切换显示器 - Type-C", L"typec", L"一键切换显示器至 USB Type-C 输入");
    CreateDesktopShortcut(L"切换显示器 - 轮流切换", L"--toggle", L"在常用输入源之间轮流快速切换");
    CreateDesktopShortcut(L"KVMSwitch (系统托盘常驻)", L"--tray", L"启动 KVMSwitch 并常驻系统托盘");

    MessageBoxW(NULL,
        L"已成功在桌面创建以下 6 个快捷方式：\n\n"
        L"1. 切换显示器 - DP (DisplayPort)\n"
        L"2. 切换显示器 - HDMI 1\n"
        L"3. 切换显示器 - HDMI 2\n"
        L"4. 切换显示器 - Type-C\n"
        L"5. 切换显示器 - 轮流切换 (Toggle)\n"
        L"6. KVMSwitch (系统托盘常驻)\n\n"
        L"您可以直接在桌面双击，或将其拖动到任务栏、为快捷方式设置全局热键！",
        L"快捷方式创建成功", MB_OK | MB_ICONINFORMATION);
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


// 弹出快捷菜单
void ShowQuickMenu(HINSTANCE hInstance, const AppConfig& cfg, MonitorContext& ctx, DWORD detectedTypeCCode, HWND hOwnerWnd = NULL) {
    UNREFERENCED_PARAMETER(hOwnerWnd);

    // 确定主显示器或首个显示器
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
    std::wstring curName = curInput ? GetInputName(curInput, &cfg) : L"未知";

    // 组织菜单项列表
    std::vector<DWORD> inputCodes;
    if (targetMon && !targetMon->supportedInputs.empty()) {
        inputCodes = targetMon->supportedInputs;
    } else {
        inputCodes = { 16, 17, 18, detectedTypeCCode };
    }

    // 保证 16 (DP), 17 (HDMI 1), detectedTypeCCode (Type-C) 在列表中
    if (std::find(inputCodes.begin(), inputCodes.end(), 16) == inputCodes.end()) inputCodes.push_back(16);
    if (std::find(inputCodes.begin(), inputCodes.end(), 17) == inputCodes.end()) inputCodes.push_back(17);
    if (std::find(inputCodes.begin(), inputCodes.end(), detectedTypeCCode) == inputCodes.end()) inputCodes.push_back(detectedTypeCCode);

    // 排序保证顺序友好: DP -> HDMI1 -> HDMI2 -> Type-C
    std::sort(inputCodes.begin(), inputCodes.end(), [](DWORD a, DWORD b) {
        auto getWeight = [](DWORD c) {
            if (c == 16) return 1; // DP
            if (c == 17) return 2; // HDMI 1
            if (c == 18) return 3; // HDMI 2
            if (c == 15 || c == 27 || c == 25) return 4; // Type-C
            return (int)c + 10;
        };
        return getWeight(a) < getWeight(b);
    });

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

    // 头部信息
    std::wstring monDesc = targetMon ? targetMon->description : L"显示器";
    std::wstring headerStr = L"🖥️ " + monDesc + L" [当前: " + curName + L"]";
    AppendMenuW(hMenu, MF_STRING | MF_DISABLED, 0, headerStr.c_str());
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    // 输入源项
    std::map<UINT, DWORD> menuIdToCode;
    UINT currentMenuId = ID_INPUT_BASE;
    for (DWORD code : inputCodes) {
        std::wstring iname = GetInputName(code, &cfg);
        wchar_t itemText[128];
        swprintf_s(itemText, L"   %s  (0x%02X)", iname.c_str(), code);

        UINT flags = MF_STRING;
        if (code == curInput) {
            flags |= MF_CHECKED;
        }
        AppendMenuW(hMenu, flags, currentMenuId, itemText);
        menuIdToCode[currentMenuId] = code;
        currentMenuId++;
    }

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_ACTION_TOGGLE, L"🔄 轮流切换到下一个输入 (Toggle)");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_ACTION_SHORTCUTS, L"🔗 创建桌面快捷方式 (一键切换)");
    AppendMenuW(hMenu, MF_STRING, ID_ACTION_CONFIG, L"⚙️ 打开配置文件 (config.ini)");
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
    if (cmd >= ID_INPUT_BASE && cmd < currentMenuId) {
        DWORD chosen = menuIdToCode[cmd];
        DoSwitch(hInstance, chosen, cfg, ctx);
    } else if (cmd == ID_ACTION_TOGGLE) {
        // 轮换切换
        DWORD nextCode = cfg.toggleInputs.empty() ? 15 : cfg.toggleInputs[0];
        auto it = std::find(cfg.toggleInputs.begin(), cfg.toggleInputs.end(), curInput);
        if (it != cfg.toggleInputs.end()) {
            size_t idx = std::distance(cfg.toggleInputs.begin(), it);
            nextCode = cfg.toggleInputs[(idx + 1) % cfg.toggleInputs.size()];
        }
        DoSwitch(hInstance, nextCode, cfg, ctx);
    } else if (cmd == ID_ACTION_SHORTCUTS) {
        CreateAllShortcuts();
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
        Shell_NotifyIconW(NIM_ADD, &g_trayNid);
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
        UpdateTrayTooltip(hWnd, &g_trayCfg);
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
            UpdateTrayTooltip(hWnd, &g_trayCfg);
        }
        return 0;

    case WM_POWERBROADCAST:
        UpdateTrayTooltip(hWnd, &g_trayCfg);
        return TRUE;

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
    Shell_NotifyIconW(NIM_ADD, &g_trayNid);

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

    // 注册进程间通信自定义消息
    g_wmShowMenu = RegisterWindowMessageW(L"KVMSwitch_ShowMenu");
    g_wmRefresh = RegisterWindowMessageW(L"KVMSwitch_Refresh");

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
    std::wstring cfgPath = GetConfigPath();
    AppConfig cfg = LoadConfig(cfgPath, detectedTypeCCode);

    int exitCode = 0;

    if (trimmedCmd == L"--TRAY" || trimmedCmd == L"-TRAY" || trimmedCmd == L"TRAY") {
        if (alreadyRunning) {
            HWND hTray = FindWindowW(L"KVMSwitchTrayWindowClass", NULL);
            if (hTray) {
                PostMessageW(hTray, g_wmShowMenu, 0, 0);
            }
        } else {
            FreeMonitorContext(ctx);
            exitCode = RunTrayApp(hInstance, cfg, detectedTypeCCode);
            if (hMutex) CloseHandle(hMutex);
            return exitCode;
        }
    } else if (trimmedCmd.empty()) {
        // 无命令行参数运行
        if (cfg.mode == L"TRAY") {
            if (alreadyRunning) {
                HWND hTray = FindWindowW(L"KVMSwitchTrayWindowClass", NULL);
                if (hTray) {
                    PostMessageW(hTray, g_wmShowMenu, 0, 0);
                }
            } else {
                FreeMonitorContext(ctx);
                exitCode = RunTrayApp(hInstance, cfg, detectedTypeCCode);
                if (hMutex) CloseHandle(hMutex);
                return exitCode;
            }
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
        CreateAllShortcuts();
    } else if (trimmedCmd == L"--QUERY" || trimmedCmd == L"-Q" || trimmedCmd == L"QUERY") {
        ShowQueryInfo(ctx, &cfg);
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
            std::wstring err = L"无法识别的输入源参数: " + cmdLine + L"\n\n支持的参数示例: dp, hdmi1, hdmi2, typec 或数值 15, 17, 18, 16";
            MessageBoxW(NULL, err.c_str(), L"KVMSwitch 错误", MB_OK | MB_ICONERROR);
            exitCode = 1;
        }
    }

    // 释放资源
    FreeMonitorContext(ctx);
    if (hMutex) CloseHandle(hMutex);
    return exitCode;
}

