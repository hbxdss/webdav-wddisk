// authdiag.c - 隔离测试 WinHTTP 发送 Basic Authorization
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>

int wmain(void) {
    const wchar_t* hdr =
        L"Authorization: Basic MTg5MTU2MTYxOTk6a2t3eW44d3g=\r\n"
        L"Depth: 0\r\n"
        L"Content-Type: application/xml; charset=utf-8\r\n";
    const char* body = "<?xml version=\"1.0\" encoding=\"utf-8\"?><d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/></d:prop></d:propfind>";

    HINTERNET s = WinHttpOpen(L"authdiag", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = WinHttpConnect(s, L"webdav.123pan.cn", 443, 0);
    HINTERNET r = WinHttpOpenRequest(c, L"PROPFIND", L"/webdav/", nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                  SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(r, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
    BOOL ok = WinHttpSendRequest(r, hdr, (DWORD)wcslen(hdr), (LPVOID)body, (DWORD)strlen(body), (DWORD)strlen(body), 0);
    printf("send=%d err=%lu\n", ok, GetLastError());
    if (WinHttpReceiveResponse(r, nullptr)) {
        DWORD code = 0, sz = sizeof(code);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
        printf("status=%lu\n", code);
        // 打印响应头里的 WWW-Authenticate / Server
        wchar_t w[2048]; sz = sizeof(w);
        if (WinHttpQueryHeaders(r, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, w, &sz, WINHTTP_NO_HEADER_INDEX))
            wprintf(L"%.*ls\n", sz / 2, w);
    }
    return 0;
}
