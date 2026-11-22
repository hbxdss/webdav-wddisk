// cracktest.c - WinHttpCrackUrl 隔离测试
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>

int wmain(void) {
    const wchar_t* url = L"http://127.0.0.1:8080/subdir/note.txt";
    WCHAR scheme[16], host[256];
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    uc.lpszScheme = scheme; uc.dwSchemeLength = 16;
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    BOOL ok = WinHttpCrackUrl(url, (DWORD)wcslen(url), 0, &uc);
    wprintf(L"ok=%d err=%lu scheme=%.*ls(%u) host=%.*ls(%u) path=%.*ls(%u)\n",
        ok, GetLastError(),
        uc.dwSchemeLength, uc.lpszScheme ? uc.lpszScheme : L"(null)", uc.dwSchemeLength,
        uc.dwHostNameLength, uc.lpszHostName ? uc.lpszHostName : L"(null)", uc.dwHostNameLength,
        uc.dwUrlPathLength, uc.lpszUrlPath ? uc.lpszUrlPath : L"(null)", uc.dwUrlPathLength);
    return 0;
}
