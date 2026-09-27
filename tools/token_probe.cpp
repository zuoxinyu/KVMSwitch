// 只读诊断：比较应用与 Explorer 的令牌完整性及限制状态。
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <initializer_list>
#pragma comment(lib, "advapi32.lib")
int wmain(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; ++i) {
        DWORD pid = wcstoul(argv[i], nullptr, 10);
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid), token = nullptr;
        if (!process || !OpenProcessToken(process, TOKEN_QUERY, &token)) {
            printf("pid=%lu openError=%lu\n", pid, GetLastError());
            if (process) CloseHandle(process);
            continue;
        }
        BYTE buffer[4096]; DWORD size = 0;
        printf("pid=%lu restricted=%d ", pid, IsTokenRestricted(token));
        if (GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &size)) {
            PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer)->Label.Sid;
            printf("integrity=0x%lx ", *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid)-1));
        }
        for (auto kind : { TokenElevation, TokenUIAccess, TokenIsAppContainer, TokenSessionId }) {
            DWORD value = 0;
            if (GetTokenInformation(token, kind, &value, sizeof(value), &size))
                printf("token[%u]=%lu ", kind, value);
        }
        puts(""); CloseHandle(token); CloseHandle(process);
    }
}
