// webdav_fuse.h - macFUSE 文件系统后端（macOS）
// 与 Windows 端相同策略：打开时下载到本地缓存，release/flush 时上传
#pragma once
#include "../common/webdav.h"
#include "../common/curl_client.h"
#include <string>
#include <map>
#include <mutex>
#include <sys/stat.h>

namespace wddisk {

class DavFuseFs {
public:
    DavFuseFs(const DavConfig& cfg, const std::string& cacheDir)
        : cfg_(cfg), http_(cfg), dav_(cfg, &http_), cacheDir_(cacheDir) {}

    // 运行 fuse_main；阻塞直至卸载。返回 0 成功
    int run(const std::string& mountPoint);
    void stop();

    DavConfig cfg_;
    CurlClient http_;
    WebDavClient dav_;
    std::string cacheDir_;
    std::mutex mtx_;
    std::map<std::string, int> openCounts_; // remotePath -> 打开数

    static uint64_t unixToTimespecSec(int64_t s) { return (uint64_t)s; }
    std::string cachePathFor(const std::string& remotePath) const;
};

} // namespace wddisk
