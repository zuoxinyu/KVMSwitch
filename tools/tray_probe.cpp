// 诊断: 跨进程探测托盘图标是否真的注册在 Explorer 通知区
// NIM_MODIFY (NIF_STATE no-op) 对不存在的图标会失败, 对存在的图标成功且无副作用
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

int wmain() {
    HWND hTray = FindWindowW(L"KVMSwitchTrayWindowClass", NULL);
    if (!hTray) {
        wprintf(L"tray window not found\n");
        return 1;
    }
    wprintf(L"tray window: hwnd=0x%p\n", (void*)hTray);

    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = hTray;
    nid.uID = 1;  // RunTrayApp 使用的 uID
    nid.uFlags = NIF_STATE;
    nid.dwState = 0;
    nid.dwStateMask = 0;

    BOOL ok = Shell_NotifyIconW(NIM_MODIFY, &nid);
    wprintf(L"NIM_MODIFY probe: %s (GetLastError=%lu)\n", ok ? L"EXISTS" : L"NOT REGISTERED", GetLastError());

    // 顺便列出通知区窗口句柄状态
    HWND hTaskbar = FindWindowW(L"Shell_TrayWnd", NULL);
    wprintf(L"Shell_TrayWnd: 0x%p\n", (void*)hTaskbar);
    return 0;
}
