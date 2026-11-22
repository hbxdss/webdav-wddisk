// webdav_fuse3.h - libfuse3 文件系统后端（Linux）
#pragma once
#include "../common/webdav.h"
#include "../common/curl_client.h"
#include <string>
#include <map>
#include <mutex>

namespace wddisk {

class DavFuseFs {
public:
    DavFuseFs(const DavConfig& cfg, const std::string& cacheDir)
        : cfg_(cfg), http_(cfg), dav_(cfg, &http_), cacheDir_(cacheDir) {}

    // 在当前线程运行 fuse 主循环（阻塞），mountPoint 为绝对路径
    int run(const std::string& mountPoint);

    DavConfig cfg_;
    CurlClient http_;
    WebDavClient dav_;
    std::string cacheDir_;
    std::mutex mtx_;
};

} // namespace wddisk
