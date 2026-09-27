// 诊断: 列出指定 PID 的所有顶层窗口 (类名+标题), 判断卡死进程的状态
#include <windows.h>
#include <stdio.h>

static DWORD g_targetPid;

static BOOL CALLBACK EnumProc(HWND h, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != g_targetPid) return TRUE;
    wchar_t cls[256] = {0}, title[256] = {0};
    GetClassNameW(h, cls, 256);
    GetWindowTextW(h, title, 256);
    BOOL visible = IsWindowVisible(h);
    wprintf(L"hwnd=0x%p visible=%d class=[%s] title=[%s]\n", (void*)h, visible, cls, title);
    return TRUE;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 1;
    g_targetPid = (DWORD)_wtoi(argv[1]);
    EnumWindows(EnumProc, 0);

    // 探测托盘窗口消息循环是否存活: WM_NULL 需要消息循环取出处理, 卡死则超时
    HWND hTray = FindWindowW(L"KVMSwitchTrayWindowClass", NULL);
    if (hTray) {
        DWORD trayPid = 0;
        GetWindowThreadProcessId(hTray, &trayPid);
        if (argc > 2 && wcscmp(argv[2], L"--close") == 0 && trayPid == g_targetPid) {
            // 正常关闭指定进程的窗口，让应用执行 NIM_DELETE 清理图标。
            return PostMessageW(hTray, WM_CLOSE, 0, 0) ? 0 : 2;
        }
        wprintf(L"FindWindowW: hwnd=0x%p\n", (void*)hTray);
        ULONG_PTR res = 0;
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        DWORD ok = SendMessageTimeoutW(hTray, WM_NULL, 0, 0, SMTO_BLOCK | SMTO_ABORTIFHUNG, 3000, &res);
        QueryPerformanceCounter(&t1);
        wprintf(L"WM_NULL probe: ok=%u elapsed=%.0f ms (timeout=frozen loop)\n", ok,
            (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
    } else {
        wprintf(L"FindWindowW: tray window NOT found\n");
    }
    return 0;
}
