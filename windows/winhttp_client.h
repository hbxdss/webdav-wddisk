// winhttp_client.h - Windows HTTP 实现（WinHTTP），供 WebDavClient 使用
// 内置：5xx/超时/断网自动重试（指数退避）、401 重新协商认证、307/308 重定向跟随
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include "../common/webdav.h"
#include <functional>
#include <atomic>
#include <mutex>
#include <string>

namespace wddisk {

class WinHttpClient : public IHttpClient {
public:
    explicit WinHttpClient(const DavConfig& cfg, std::string* lastUrl = nullptr) : cfg_(cfg) {}
    ~WinHttpClient() override;

    int request(const std::string& method, const std::string& url,
        const std::vector<std::string>& headers,
        const std::string& body, std::string& response, std::string& errMsg) override;

    // 全局传输进度回调（所有实例共享，用于 UI 显示"正在上传 xxx 45%"）
    // phase: 0=空闲 1=上传中 2=下载中
    struct Progress {
        std::atomic<int> phase{ 0 };
        std::atomic<long long> done{ 0 };
        std::atomic<long long> total{ 0 };
        std::mutex nameMtx;
        std::string name;
    };
    static Progress& progress();
    // 连接状态（连续失败后置 false，UI 显示"网络异常"）
    static std::atomic<bool>& online();

    // 关闭自动重试（供 UI 线程的轻量请求使用）
    void setRetries(int n) { retries_ = n; }

private:
    DavConfig cfg_;
    void* hSession_ = nullptr; // HINTERNET
    std::wstring hostW_, pathW_;
    INTERNET_PORT port_ = 80;
    bool https_ = false;
    int retries_ = 3;
    bool ensureSession();
    // 单次请求（不重试）；返回状态码，<0 网络错误（netErr 带 WinHTTP 错误码）
    int requestOnce(const std::string& method, const std::string& url,
        const std::vector<std::string>& headers,
        const std::string& body, std::string& response, std::string& errMsg,
        int& winErr, std::string& redirectTo);
};

} // namespace wddisk
