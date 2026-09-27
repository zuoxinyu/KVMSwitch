// 临时诊断工具: 分别计时各个 DDC/CI 调用, 定位菜单弹出延迟的来源
// 第二遍枚举模拟 KVMSwitch 的 caps 缓存命中路径, 验证命中后的耗时
#include <windows.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>
#include <stdio.h>
#include <string>
#include <map>
#include <vector>

#pragma comment(lib, "dxva2.lib")
#pragma comment(lib, "user32.lib")

struct PhysEntry { HANDLE h; std::wstring desc; std::wstring device; std::vector<DWORD> inputs; };
static std::vector<PhysEntry> g_phys;
static std::map<std::wstring, std::vector<DWORD>> g_capsCache;

static BOOL CALLBACK EnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM) {
    MONITORINFOEXW mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMon, &mi);
    DWORD n = 0;
    if (GetNumberOfPhysicalMonitorsFromHMONITOR(hMon, &n) && n > 0) {
        std::vector<PHYSICAL_MONITOR> pm(n);
        if (GetPhysicalMonitorsFromHMONITOR(hMon, n, pm.data())) {
            for (DWORD i = 0; i < n; ++i) {
                PhysEntry e;
                e.h = pm[i].hPhysicalMonitor;
                e.desc = pm[i].szPhysicalMonitorDescription;
                e.device = mi.szDevice;
                g_phys.push_back(e);
            }
        }
    }
    return TRUE;
}

// 一遍完整枚举: 读当前输入 + 读 caps (带缓存, 与 KVMSwitch 新逻辑一致)
static double FullPass(LARGE_INTEGER f) {
    g_phys.clear();
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    EnumDisplayMonitors(NULL, NULL, EnumProc, 0);
    for (auto& p : g_phys) {
        MC_VCP_CODE_TYPE type; DWORD cur = 0, mx = 0;
        GetVCPFeatureAndVCPFeatureReply(p.h, 0x60, &type, &cur, &mx);
        std::wstring key = p.device + L"|" + p.desc;
        auto it = g_capsCache.find(key);
        if (it == g_capsCache.end()) {
            DWORD len = 0;
            if (GetCapabilitiesStringLength(p.h, &len) && len > 0) {
                std::vector<char> buf(len + 2, 0);
                if (CapabilitiesRequestAndCapabilitiesReply(p.h, buf.data(), len)) {
                    // 简化解析: 与主程序无关, 只标记已缓存
                    g_capsCache[key] = { cur };
                }
            }
        }
    }
    QueryPerformanceCounter(&t1);
    return (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
}

static double Ms(LARGE_INTEGER a, LARGE_INTEGER b, LARGE_INTEGER f) {
    return (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
}

int main() {
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);

    double pass1 = FullPass(f);
    wprintf(L"pass 1 (cold, caps read):  %8.1f ms\n", pass1);
    double pass2 = FullPass(f);
    wprintf(L"pass 2 (caps cached):      %8.1f ms\n", pass2);

    for (auto& p : g_phys) DestroyPhysicalMonitor(p.h);
    return 0;
}
