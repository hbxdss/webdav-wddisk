// winhttp_client.cpp - WinHTTP 实现（重试 / 401 / 重定向 / 进度）
#include "winhttp_client.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <cstdio>
#pragma comment(lib, "winhttp.lib")

namespace wddisk {

static std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::string wideToUtf8(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string o(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &o[0], n, nullptr, nullptr);
    return o;
}

WinHttpClient::Progress& WinHttpClient::progress() {
    static Progress p;
    return p;
}
std::atomic<bool>& WinHttpClient::online() {
    static std::atomic<bool> o{ true };
    return o;
}

WinHttpClient::~WinHttpClient() {
    if (hSession_) WinHttpCloseHandle((HINTERNET)hSession_);
}

bool WinHttpClient::ensureSession() {
    if (hSession_) return true;
    hSession_ = (void*)WinHttpOpen(L"Wddisk/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession_) return false;
    // 超时：解析 10s / 连接 10s / 发送 10s / 接收 60s（防止服务器无响应时操作挂死）
    WinHttpSetTimeouts((HINTERNET)hSession_, 10000, 10000, 10000, 60000);
    return true;
}

// 单次请求：不重试、不跟随重定向（返回 3xx 时通过 redirectTo 带出 Location）
int WinHttpClient::requestOnce(const std::string& method, const std::string& url,
    const std::vector<std::string>& headers,
    const std::string& body, std::string& response, std::string& errMsg,
    int& winErr, std::string& redirectTo) {

    winErr = 0;
    redirectTo.clear();
    if (!ensureSession()) { errMsg = "WinHttpOpen failed"; return -1; }

    // 手动解析 URL（不依赖 WinHttpCrackUrl，MinGW 头文件下其 path 输出异常）
    // 格式: http(s)://host[:port]/path
    std::string u = url;
    bool https = false;
    std::string hostA, pathA;
    {
        size_t p = u.find("://");
        if (p == std::string::npos) { errMsg = "URL 缺少协议: " + url; return -1; }
        https = (u.compare(0, 5, "https") == 0);
        std::string rest = u.substr(p + 3);
        size_t slash = rest.find('/');
        std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
        pathA = (slash == std::string::npos) ? "/" : rest.substr(slash);
        std::string host = hostport;
        INTERNET_PORT port = https ? 443 : 80;
        size_t colon = hostport.rfind(':');
        if (colon != std::string::npos) {
            host = hostport.substr(0, colon);
            port = (INTERNET_PORT)atoi(hostport.substr(colon + 1).c_str());
            if (port == 0) port = https ? 443 : 80;
        }
        https_ = https;
        port_ = port;
        hostW_ = utf8ToWide(host);
        pathW_ = utf8ToWide(pathA);
    }
    const std::wstring& host = hostW_;
    const std::wstring& path = pathW_;

    HINTERNET hConnect = WinHttpConnect((HINTERNET)hSession_, host.c_str(), port_, 0);
    if (!hConnect) { winErr = (int)GetLastError(); errMsg = "WinHttpConnect failed"; return -1; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, utf8ToWide(method).c_str(),
        path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, https_ ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) {
        winErr = (int)GetLastError();
        WinHttpCloseHandle(hConnect); errMsg = "WinHttpOpenRequest failed"; return -1;
    }

    // 手动跟随重定向（禁用自动跟随，便于保留方法/头/Range）
    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));

    if (https_ && !cfg_.verifyTls) {
        DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
    }

    // 认证（Basic）
    std::wstring allHeaders;
    if (!cfg_.username.empty() || !cfg_.password.empty()) {
        std::string auth = cfg_.username + ":" + cfg_.password;
        // Base64
        static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string b64;
        for (size_t i = 0; i < auth.size(); i += 3) {
            unsigned v = (unsigned char)auth[i] << 16;
            if (i + 1 < auth.size()) v |= (unsigned char)auth[i + 1] << 8;
            if (i + 2 < auth.size()) v |= (unsigned char)auth[i + 2];
            b64 += tbl[(v >> 18) & 63]; b64 += tbl[(v >> 12) & 63];
            b64 += i + 1 < auth.size() ? tbl[(v >> 6) & 63] : '=';
            b64 += i + 2 < auth.size() ? tbl[v & 63] : '=';
        }
        allHeaders += L"Authorization: Basic " + utf8ToWide(b64) + L"\r\n";
    }
    for (auto& h : headers) allHeaders += utf8ToWide(h) + L"\r\n";

    BOOL ok = WinHttpSendRequest(hRequest, allHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : allHeaders.c_str(),
        (DWORD)allHeaders.size(), (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);
    if (!ok) {
        DWORD e = GetLastError();
        winErr = (int)e;
        errMsg = "WinHttpSendRequest 失败, 错误码 " + std::to_string(e);
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect);
        return -1;
    }

    // 上传进度：body 已整体交给 WinHTTP，标记完成（大文件 PUT 期间即"已发送"）
    {
        Progress& pr = progress();
        if (!body.empty()) {
            pr.done.store((long long)body.size());
            pr.total.store((long long)body.size());
        }
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD e = GetLastError();
        winErr = (int)e;
        errMsg = "WinHttpReceiveResponse 失败, 错误码 " + std::to_string(e);
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect);
        return -1;
    }

    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);

    // 重定向：取 Location
    if (status == 301 || status == 302 || status == 307 || status == 308) {
        wchar_t loc[2048] = {};
        DWORD lsz = sizeof(loc) - sizeof(wchar_t);
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
            loc, &lsz, WINHTTP_NO_HEADER_INDEX)) {
            std::string l = wideToUtf8(loc);
            if (!l.empty()) {
                if (l[0] == '/') {
                    // 相对路径，补全 scheme://host[:port]
                    std::string base = url.substr(0, url.find("://") + 3);
                    size_t slash = url.find('/', base.size());
                    std::string hostport = (slash == std::string::npos)
                        ? url.substr(base.size()) : url.substr(base.size(), slash - base.size());
                    redirectTo = base + hostport + l;
                } else if (l.compare(0, 7, "http://") == 0 || l.compare(0, 8, "https://") == 0) {
                    redirectTo = l;
                }
            }
        }
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        response.clear();
        return (int)status;
    }

    response.clear();
    char buf[65536];
    DWORD read = 0;
    long long got = 0;
    Progress& pr = progress();
    bool isGet = (method == "GET");
    if (isGet) pr.done.store(0);
    while (WinHttpReadData(hRequest, buf, sizeof(buf), &read) && read > 0) {
        response.append(buf, read);
        if (isGet && pr.phase.load() == 2) {
            got += read;
            pr.done.store(got);
            pr.total.store((long long)response.size()); // 无 Content-Length 时占位
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    return (int)status;
}

int WinHttpClient::request(const std::string& method, const std::string& url,
    const std::vector<std::string>& headers,
    const std::string& body, std::string& response, std::string& errMsg) {

    std::string curUrl = url;
    bool isGet = (method == "GET");
    bool isPut = (method == "PUT");

    // 设置进度阶段
    Progress& pr = progress();
    {
        std::lock_guard<std::mutex> l(pr.nameMtx);
        if (isPut) { pr.name = url; pr.phase.store(1); pr.done.store(0); pr.total.store((long long)body.size()); }
        else if (isGet) { pr.name = url; pr.phase.store(2); pr.done.store(0); pr.total.store(0); }
    }
    auto clearPhase = [&]() {
        if (isGet || isPut) {
            std::lock_guard<std::mutex> l(pr.nameMtx);
            pr.phase.store(0);
            pr.name.clear();
        }
    };

    for (int attempt = 0; ; ++attempt) {
        std::string redirectTo;
        int winErr = 0;
        errMsg.clear();
        int code = requestOnce(method, curUrl, headers, body, response, errMsg, winErr, redirectTo);

        // 重定向（最多 5 跳）
        if (code == 301 || code == 302 || code == 307 || code == 308) {
            if (!redirectTo.empty() && attempt < 5) {
                curUrl = redirectTo;
                --attempt; // 重定向不计入重试次数
                continue;
            }
            errMsg = "重定向失败 HTTP " + std::to_string(code);
            clearPhase();
            return code;
        }

        // 401：服务器可能要求先匿名请求再挑战；清空后重试一次（已带 Basic 仍 401 则交给上层报错）
        if (code == 401 && attempt == 0 && (!cfg_.username.empty() || !cfg_.password.empty())) {
            continue;
        }

        // 网络错误 / 5xx / 408 / 429：退避重试
        bool retriable = (code < 0) ||
            code == 500 || code == 502 || code == 503 || code == 504 ||
            code == 408 || code == 429;
        if (retriable && attempt < retries_) {
            online().store(false);
            int delay = 300 * (1 << attempt); // 300 / 600 / 1200 ms
            // 连接类错误（服务器拒绝/无路由）稍长等待
            if (code < 0 && (winErr == ERROR_WINHTTP_CANNOT_CONNECT ||
                             winErr == ERROR_WINHTTP_TIMEOUT ||
                             winErr == ERROR_WINHTTP_NAME_NOT_RESOLVED))
                delay = 800 * (1 << attempt);
            if (delay > 5000) delay = 5000;
            // fslog 位于全局 namespace，避免跨命名空间 extern 歧义
            SleepEx((DWORD)delay, FALSE);
            continue;
        }

        if (code >= 200 && code < 400) online().store(true);
        else if (code < 0) online().store(false);

        if (code < 0 && errMsg.empty()) errMsg = "网络错误";
        clearPhase();
        return code;
    }
}

} // namespace wddisk
