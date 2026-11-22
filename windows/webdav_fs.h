// webdav_fs.h - WinFsp 文件系统实现（挂载 WebDAV 为 Windows 盘符）
// 策略：文件打开时整体下载到本地缓存，读写走缓存；Cleanup/Flush 时若修改则上传。
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../common/webdav.h"
#include "winhttp_client.h"
#include <string>
#include <mutex>
#include <map>
#include <vector>
#include <ctime>

struct _FSP_FILE_SYSTEM;
typedef struct _FSP_FILE_SYSTEM FSP_FILE_SYSTEM;

// 调试日志（写 %TEMP%\wddisk.log）
void fslog(const char* fmt, ...);

namespace wddisk {

// FileNode 定义在 webdav_fs.cpp（含分块缓存状态，仅文件系统内部使用）

class DavFileSystem {
public:
    DavFileSystem(const DavConfig& cfg, const std::wstring& cacheDir);
    ~DavFileSystem();

    bool mount(const std::wstring& mountPoint, std::string& err); // mountPoint 形如 L"Z:"
    void unmount();

    DavConfig cfg_;
    WinHttpClient http_;
    WebDavClient dav_;
    std::wstring cacheDir_;
    std::mutex mtx_;
    // 元数据 TTL 缓存
    struct DirCache { std::vector<wddisk::WebDavEntry> entries; time_t expiry; };
    std::map<std::string, DirCache> dirCache_;
    std::map<std::string, std::pair<wddisk::WebDavEntry, time_t>> statCache_;
    FSP_FILE_SYSTEM* fs_ = nullptr;

    // 元数据缓存接口（fsops 回调使用）
    bool cachedStat(const std::string& path, wddisk::WebDavEntry& e, std::string& err);
    bool cachedListDir(const std::string& path, std::vector<wddisk::WebDavEntry>& out, std::string& err);
    void invalidate(const std::string& path);
    void startUploadWorker();
    void stopUploadWorker();
    void enqueueUpload(const std::string& remotePath, const std::wstring& cachePath, UINT64 size);

    static UINT64 unixToWinTime(int64_t unixSec) {
        return (UINT64)(unixSec + 11644473600LL) * 10000000ULL;
    }

    bool ensureCacheDir();
public:
    std::wstring cachePathFor(const std::string& remotePath);
};

} // namespace wddisk
