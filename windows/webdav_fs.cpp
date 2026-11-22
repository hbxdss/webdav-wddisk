// webdav_fs.cpp - WinFsp 文件系统实现（对齐 WinFsp v2.0 C 接口）
// 性能架构：
//  - 分块按需下载：1MB 块，读时按需拉取（Range GET），顺序读自动预取至 4MB
//  - 写走本地缓存，Flush/Cleanup 时补齐空洞后整体 PUT（兼容所有 WebDAV 服务器）
//  - 目录列表 / stat 元数据 TTL 缓存，写操作后自动失效
#include "webdav_fs.h"
#include "../common/webdav.h"
#include <winfsp.h>
#include <sddl.h>
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "winfsp-x64.lib")
#pragma comment(lib, "sddl.lib")

using namespace wddisk;

static DavFileSystem* g_fs = nullptr;

static const uint64_t CHUNK_SIZE = 1 << 20;  // 1MB
static const uint64_t PREFETCH_MAX = 4 << 20; // 单次预取上限 4MB
static const int DIR_TTL = 30;
static const int STAT_TTL = 10;

void fslog(const char* fmt, ...) {
    static FILE* lf = nullptr;
    if (!lf) {
        char p[MAX_PATH];
        GetTempPathA(MAX_PATH, p);
        strcat(p, "wddisk.log");
        lf = fopen(p, "a");
    }
    if (!lf) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(lf, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(lf, fmt, ap);
    va_end(ap);
    fprintf(lf, "\n");
    fflush(lf);
}

// UTF-8 <-> UTF-16
static std::wstring u8w(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::string wu8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static PSECURITY_DESCRIPTOR g_defaultSd = nullptr;

static PSECURITY_DESCRIPTOR defaultSecurityDescriptor() {
    if (!g_defaultSd) {
        ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:BAG:SYD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;WD)",
            SDDL_REVISION_1, &g_defaultSd, nullptr);
    }
    return g_defaultSd;
}

// ---------- 文件节点 ----------
struct FileNode {
    struct Chunk { bool filled = false; };

    std::string remotePath;
    bool isDir = false;
    std::wstring cachePath;
    HANDLE hFile = INVALID_HANDLE_VALUE;
    std::mutex fmu;
    uint64_t remoteSize = 0;  // 打开时服务器端大小
    uint64_t curSize = 0;     // 逻辑大小（写可扩展）
    bool dirty = false;
    bool deleted = false;
    bool cachePending = false; // 缓存文件已交由后台上传队列，Close 不得删除
    std::map<uint64_t, Chunk> chunks; // key = 块索引

    ~FileNode() { if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile); }
};

// FileInfo 辅助
static void setFileInfo2(FSP_FSCTL_FILE_INFO* fi, bool isDir, UINT32 attrs,
    UINT64 size, UINT64 mtimeWin) {
    memset(fi, 0, sizeof(*fi));
    fi->FileAttributes = attrs;
    fi->CreationTime = mtimeWin;
    fi->LastAccessTime = mtimeWin;
    fi->LastWriteTime = mtimeWin;
    fi->ChangeTime = mtimeWin;
    fi->FileSize = size;
    fi->AllocationSize = size;
    fi->IndexNumber = 0;
    fi->HardLinks = 0;
    fi->ReparseTag = 0;
    fi->EaSize = 0;
}

// ---------- 路径工具 ----------
static std::string toRemote(PWSTR fileName) {
    return wu8(fileName ? fileName : L"");
}
static std::string normRemote(const std::string& p) {
    std::string r = p;
    if (!r.empty() && r[0] == '\\') r.erase(0, 1);
    for (auto& c : r) if (c == '\\') c = '/';
    return "/" + r;
}
static std::string parentOf(const std::string& p) {
    size_t s = p.rfind('/');
    if (s == std::string::npos || s == 0) return "/";
    return p.substr(0, s);
}

DavFileSystem::DavFileSystem(const DavConfig& cfg, const std::wstring& cacheDir)
    : cfg_(cfg), http_(cfg), dav_(cfg, &http_), cacheDir_(cacheDir) {
    g_fs = this;
}

DavFileSystem::~DavFileSystem() {
    unmount();
    if (g_fs == this) g_fs = nullptr;
}

bool DavFileSystem::ensureCacheDir() {
    std::wstring dir = cacheDir_;
    for (size_t i = 1; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == L'\\') {
            std::wstring sub = dir.substr(0, i);
            CreateDirectoryW(sub.c_str(), nullptr);
        }
    }
    return true;
}

std::wstring DavFileSystem::cachePathFor(const std::string& remotePath) {
    unsigned long long h = 1469598103934665603ULL;
    for (unsigned char c : remotePath) { h ^= c; h *= 1099511628211ULL; }
    wchar_t buf[64];
    swprintf(buf, 64, L"%016llx.bin", h);
    return cacheDir_ + L"\\" + buf;
}

// ---------- 元数据缓存 ----------
bool DavFileSystem::cachedStat(const std::string& path, WebDavEntry& e, std::string& err) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = statCache_.find(path);
        if (it != statCache_.end() && time(nullptr) < it->second.second) {
            e = it->second.first;
            return true;
        }
    }
    bool ok = dav_.stat(path, e, err);
    if (ok) {
        std::lock_guard<std::mutex> lock(mtx_);
        statCache_[path] = { e, time(nullptr) + STAT_TTL };
    }
    return ok;
}

bool DavFileSystem::cachedListDir(const std::string& path, std::vector<WebDavEntry>& out, std::string& err) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = dirCache_.find(path);
        if (it != dirCache_.end() && time(nullptr) < it->second.expiry) {
            out = it->second.entries;
            return true;
        }
    }
    std::vector<WebDavEntry> entries;
    if (!dav_.listDir(path, entries, err)) return false;
    std::lock_guard<std::mutex> lock(mtx_);
    dirCache_[path] = { entries, time(nullptr) + DIR_TTL };
    out = std::move(entries);
    return true;
}

void DavFileSystem::invalidate(const std::string& path) {
    std::lock_guard<std::mutex> lock(mtx_);
    statCache_.erase(path);
    dirCache_.erase(path);
    dirCache_.erase(parentOf(path));
}

// ---------- 分块读写 ----------
// 确保 [off, off+len) 覆盖的块已填充（顺序读自动预取）
static bool ensureRange(FileNode* node, uint64_t off, uint64_t len, std::string& err) {
    if (len == 0) return true;
    uint64_t beginChunk = off / CHUNK_SIZE;
    uint64_t endChunk = (off + len - 1) / CHUNK_SIZE;

    // 锁内收集第一个 missing 连续段（可扩展至预取上限）
    uint64_t fetchOff, fetchLen;
    {
        std::lock_guard<std::mutex> lock(node->fmu);
        if (node->chunks.count(beginChunk) && node->chunks[beginChunk].filled) {
            // 找到第一个未填充块
            while (beginChunk <= endChunk && node->chunks.count(beginChunk) &&
                   node->chunks[beginChunk].filled)
                ++beginChunk;
            if (beginChunk > endChunk) return true;
        }
        uint64_t c = beginChunk;
        while (c <= endChunk &&
               !(node->chunks.count(c) && node->chunks[c].filled)) {
            ++c;
            if ((c * CHUNK_SIZE - beginChunk * CHUNK_SIZE) >= PREFETCH_MAX) break;
        }
        fetchOff = beginChunk * CHUNK_SIZE;
        fetchLen = std::min((c - beginChunk) * CHUNK_SIZE, node->remoteSize - fetchOff);
    }
    if (fetchLen == 0) return true; // 超出远端大小的部分保持零

    std::string data;
    if (!g_fs->dav_.downloadRange(node->remotePath, fetchOff, (size_t)fetchLen, data, err)) {
        fslog("ensureRange: %s @%llu FAILED %s", node->remotePath.c_str(), fetchOff, err.c_str());
        return false;
    }
    if (data.size() < fetchLen) fetchLen = data.size(); // 服务器提前截断

    // 写入缓存并标记已填充
    {
        std::lock_guard<std::mutex> lock(node->fmu);
        OVERLAPPED ov = {};
        ov.Offset = (DWORD)(fetchOff & 0xFFFFFFFF);
        ov.OffsetHigh = (DWORD)(fetchOff >> 32);
        DWORD written = 0;
        if (!data.empty() &&
            !WriteFile(node->hFile, data.data(), (DWORD)data.size(), &written, &ov)) {
            err = "缓存写入失败";
            return false;
        }
        uint64_t lastChunk = (fetchOff + fetchLen - 1) / CHUNK_SIZE;
        for (uint64_t c = fetchOff / CHUNK_SIZE; c <= lastChunk; ++c)
            node->chunks[c].filled = true;
    }
    return true;
}

// Flush/Cleanup 前：补齐 [0, curSize) 内所有空洞（只对远端存在的部分做下载）
static bool fillAllChunks(FileNode* node, std::string& err) {
    uint64_t size = node->curSize;
    if (size == 0) return true;
    uint64_t idx = 0;
    while (idx * CHUNK_SIZE < size) {
        bool need = false;
        uint64_t off = idx * CHUNK_SIZE;
        uint64_t dlLen = 0; // 需要从服务器下载的长度（0 = 无需下载）
        {
            std::lock_guard<std::mutex> lock(node->fmu);
            need = !(node->chunks.count(idx) && node->chunks[idx].filled);
            if (need) {
                // 只有远端存在该区间的数据才需要下载
                uint64_t dlEnd = std::min(off + CHUNK_SIZE, node->remoteSize);
                dlLen = dlEnd > off ? dlEnd - off : 0;
            }
        }
        if (need && dlLen > 0) {
            std::string data;
            if (!g_fs->dav_.downloadRange(node->remotePath, off, (size_t)dlLen, data, err))
                return false;
            std::lock_guard<std::mutex> lock(node->fmu);
            OVERLAPPED ov = {};
            ov.Offset = (DWORD)(off & 0xFFFFFFFF);
            ov.OffsetHigh = (DWORD)(off >> 32);
            DWORD written = 0;
            if (!data.empty() &&
                !WriteFile(node->hFile, data.data(), (DWORD)data.size(), &written, &ov)) {
                err = "缓存写入失败";
                return false;
            }
            node->chunks[idx].filled = true;
        } else if (need) {
            // 远端没有该区间（新建文件/追加区域），缓存中已是实际写入或零
            std::lock_guard<std::mutex> lock(node->fmu);
            node->chunks[idx].filled = true;
        }
        ++idx;
    }
    return true;
}

// 确保缓存文件内容完整（补齐远端存在的空洞），随后交由后台上传队列
static void submitUpload(DavFileSystem* self, FileNode* node, std::string& err) {
    if (!fillAllChunks(node, err)) {
        fslog("submitUpload: fill FAILED %s (%s) 缓存保留", node->remotePath.c_str(), err.c_str());
        return;
    }
    UINT64 size;
    { std::lock_guard<std::mutex> lock(node->fmu); size = node->curSize; }
    node->dirty = false;
    node->cachePending = true;
    fslog("submitUpload: enqueue %s (%llu bytes)", node->remotePath.c_str(), (unsigned long long)size);
    self->enqueueUpload(node->remotePath, node->cachePath, size);
}

// ---------- 后台上传队列 ----------
// Cleanup/Flush 只把脏文件入队即返回，独立线程负责 PUT，避免阻塞文件系统回调。
// 队列项只持字符串（remotePath + cachePath），与 FileNode 生命周期解耦：
// Close 时若 uploadPending 则不删除缓存文件，由工作线程上传后删除。
struct UploadItem {
    std::string remotePath;
    std::wstring cachePath;
};
static std::queue<UploadItem> g_upQ;
static std::mutex g_upMtx;
static std::condition_variable g_upCv;
static bool g_upStop = false;
static std::thread g_upThread;
static DavFileSystem* g_upFs = nullptr; // 工作线程使用的 dav 客户端宿主

// 待上传集合：上传完成前，GetSecurityByName 视为存在（避免文件"消失"）
static std::map<std::string, std::pair<UINT64, time_t>> g_pending; // path -> (size, expiry)
static std::mutex g_pendingMtx;
// 已删除文件的取消集合：worker 跳过其上传，PUT 后再补偿删除
static std::set<std::string> g_cancel;
static std::mutex g_cancelMtx;

static void uploadWorker() {
    fslog("uploadWorker: thread started");
    while (true) {
        UploadItem it;
        {
            std::unique_lock<std::mutex> lock(g_upMtx);
            g_upCv.wait(lock, [] { return g_upStop || !g_upQ.empty(); });
            if (g_upQ.empty()) return; // stop 且已排空
            it = std::move(g_upQ.front());
            g_upQ.pop();
        }
        // 读取缓存文件
        // 已被删除的文件：跳过上传，删除缓存
        {
            std::lock_guard<std::mutex> cl(g_cancelMtx);
            if (g_cancel.count(it.remotePath)) {
                g_cancel.erase(it.remotePath);
                DeleteFileW(it.cachePath.c_str());
                fslog("uploadWorker: canceled %s", it.remotePath.c_str());
                continue;
            }
        }
        fslog("uploadWorker: processing %s", it.remotePath.c_str());
        HANDLE h = CreateFileW(it.cachePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            fslog("uploadWorker: cache open FAILED %ls err=%lu", it.cachePath.c_str(), GetLastError());
            continue;
        }
        LARGE_INTEGER sz;
        std::string data;
        if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0) {
            data.resize((size_t)sz.QuadPart);
            DWORD got = 0;
            if (!ReadFile(h, &data[0], (DWORD)data.size(), &got, nullptr)) {
                CloseHandle(h);
                continue;
            }
            data.resize(got);
        }
        CloseHandle(h);

        std::string err;
        if (g_upFs && g_upFs->dav_.upload(it.remotePath, data, err)) {
            // 上传期间被删除：补偿删除远端
            bool wasCanceled = false;
            {
                std::lock_guard<std::mutex> cl(g_cancelMtx);
                wasCanceled = g_cancel.count(it.remotePath) > 0;
                g_cancel.erase(it.remotePath);
            }
            if (wasCanceled) {
                std::string derr;
                g_upFs->dav_.remove(it.remotePath, derr);
                fslog("uploadWorker: put-then-delete %s", it.remotePath.c_str());
            } else {
                fslog("uploadWorker: OK %s (%llu bytes)", it.remotePath.c_str(), (unsigned long long)data.size());
                g_upFs->invalidate(it.remotePath);
                g_upFs->invalidate(parentOf(it.remotePath));
            }
            DeleteFileW(it.cachePath.c_str());
            std::lock_guard<std::mutex> pl(g_pendingMtx);
            g_pending.erase(it.remotePath);
        } else {
            fslog("uploadWorker: FAILED %s (%s) 缓存保留", it.remotePath.c_str(), err.c_str());
        }
    }
}

void DavFileSystem::startUploadWorker() {
    g_upStop = false;
    g_upFs = this;
    g_upThread = std::thread(uploadWorker);
}

void DavFileSystem::stopUploadWorker() {
    {
        std::lock_guard<std::mutex> lock(g_upMtx);
        g_upStop = true;
    }
    g_upCv.notify_all();
    if (g_upThread.joinable()) g_upThread.join();
}

void DavFileSystem::enqueueUpload(const std::string& remotePath, const std::wstring& cachePath, UINT64 size) {
    {
        std::lock_guard<std::mutex> lock(g_upMtx);
        g_upQ.push({ remotePath, cachePath });
    }
    {
        std::lock_guard<std::mutex> pl(g_pendingMtx);
        g_pending[remotePath] = { size, time(nullptr) + 3600 };
    }
    g_upCv.notify_one();
}

// ---------- OVERLAPPED 随机读写 ----------
static bool pwrite(HANDLE h, const void* buf, size_t len, uint64_t off) {
    OVERLAPPED ov = {};
    ov.Offset = (DWORD)(off & 0xFFFFFFFF);
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD put = 0;
    return WriteFile(h, buf, (DWORD)len, &put, &ov) && put == len;
}
static bool pread(HANDLE h, void* buf, size_t len, uint64_t off, DWORD& got) {
    OVERLAPPED ov = {};
    ov.Offset = (DWORD)(off & 0xFFFFFFFF);
    ov.OffsetHigh = (DWORD)(off >> 32);
    return ReadFile(h, buf, (DWORD)len, &got, &ov) ? true : false;
}

// ============================ 回调实现 ============================
namespace fsops {

NTSTATUS GetVolumeInfo(FSP_FILE_SYSTEM*, FSP_FSCTL_VOLUME_INFO* VolumeInfo) {
    memset(VolumeInfo, 0, sizeof(*VolumeInfo));
    VolumeInfo->TotalSize = 0x7FFF00000000ULL;
    VolumeInfo->FreeSize = 0x3FFF00000000ULL;
    wcscpy_s(VolumeInfo->VolumeLabel, L"Wddisk");
    return STATUS_SUCCESS;
}

NTSTATUS GetSecurityByName(FSP_FILE_SYSTEM*, PWSTR FileName,
    PUINT32 PFileAttributes, PSECURITY_DESCRIPTOR SecurityDescriptor, SIZE_T* PSecurityDescriptorSize) {
    DavFileSystem* self = g_fs;
    std::string remote = normRemote(toRemote(FileName));
    WebDavEntry e;
    std::string err;
    bool found;
    if (remote == "/") { found = true; e.isDir = true; }
    else found = self->cachedStat(remote, e, err);
    if (!found) {
        // 上传队列中的文件视为存在（大小为待上传值）
        std::lock_guard<std::mutex> pl(g_pendingMtx);
        auto it = g_pending.find(remote);
        if (it != g_pending.end()) {
            if (time(nullptr) < it->second.second) {
                found = true;
                e.isDir = false;
                e.size = it->second.first;
                e.modified = time(nullptr);
            } else {
                g_pending.erase(it);
            }
        }
    }
    if (!found) {
        fslog("GetSecurityByName: %s not found (%s)", remote.c_str(), err.c_str());
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    if (PFileAttributes)
        *PFileAttributes = e.isDir ? (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY) : FILE_ATTRIBUTE_NORMAL;
    if (SecurityDescriptor && PSecurityDescriptorSize) {
        PSECURITY_DESCRIPTOR sd = defaultSecurityDescriptor();
        DWORD sz = GetSecurityDescriptorLength(sd);
        if (*PSecurityDescriptorSize < sz) { *PSecurityDescriptorSize = sz; return STATUS_BUFFER_OVERFLOW; }
        memcpy(SecurityDescriptor, sd, sz);
        *PSecurityDescriptorSize = sz;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS openOrCreate(PWSTR fileName, bool create, UINT32 CreateOptions,
    PVOID* ctx, FSP_FSCTL_FILE_INFO* fi) {
    DavFileSystem* self = g_fs;
    std::string remote = normRemote(toRemote(fileName));
    // 注意：不可持有 self->mtx_（cachedStat/cachedListDir 内部会加锁）
    bool wantDir = (CreateOptions & FILE_DIRECTORY_FILE) != 0;
    fslog("openOrCreate: %s create=%d wantDir=%d", remote.c_str(), (int)create, (int)wantDir);

    WebDavEntry e;
    std::string err;
    if (create) {
        // 目标已存在则冲突（目录/文件类型不匹配也算）
        if (remote != "/" && self->cachedStat(remote, e, err)) {
            if (e.isDir != wantDir || !wantDir) return STATUS_OBJECT_NAME_COLLISION;
        }
        // 创建文件时确保父目录在服务器端存在（新建目录可能有短暂延迟或创建失败）
        if (!wantDir && remote != "/") {
            std::string parent = parentOf(remote);
            WebDavEntry pe;
            if (parent != "/" && !self->cachedStat(parent, pe, err)) {
                std::string perr;
                self->dav_.mkdir(parent, perr); // 已存在(405)也会返回 true
                self->invalidate(parent);
                self->invalidate(parentOf(parent));
            }
        }
    } else {
        if (remote != "/" && !self->cachedStat(remote, e, err)) {
            // 待上传队列中的文件视为存在
            bool pendingHit = false;
            {
                std::lock_guard<std::mutex> pl(g_pendingMtx);
                auto pit = g_pending.find(remote);
                if (pit != g_pending.end() && time(nullptr) < pit->second.second) {
                    e.isDir = false;
                    e.size = pit->second.first;
                    e.modified = time(nullptr);
                    pendingHit = true;
                }
            }
            if (!pendingHit) {
                fslog("openOrCreate: stat failed %s (%s)", remote.c_str(), err.c_str());
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }
        }
        if (remote == "/") e.isDir = true;
    }

    FileNode* node = new FileNode();
    node->remotePath = remote;
    node->isDir = create ? wantDir : e.isDir;

    if (node->isDir) {
        if (create && !self->dav_.mkdir(remote, err)) {
            fslog("openOrCreate: MKCOL failed %s", err.c_str());
            delete node;
            return STATUS_ACCESS_DENIED;
        }
        if (create) self->invalidate(parentOf(remote));
        setFileInfo2(fi, true, FILE_ATTRIBUTE_DIRECTORY, 0, DavFileSystem::unixToWinTime(e.modified));
        *ctx = node;
        return STATUS_SUCCESS;
    }

    node->cachePath = self->cachePathFor(remote);
    if (create) {
        node->curSize = 0;
        node->remoteSize = 0;
        node->dirty = true;
        self->invalidate(parentOf(remote));
    } else {
        node->curSize = e.size;
        node->remoteSize = e.size;
    }

    node->hFile = CreateFileW(node->cachePath.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (node->hFile == INVALID_HANDLE_VALUE) {
        // 缓存目录可能被清理/首次使用还没创建：重建目录后重试一次
        self->ensureCacheDir();
        node->hFile = CreateFileW(node->cachePath.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (node->hFile == INVALID_HANDLE_VALUE) {
        fslog("openOrCreate: cache open failed err=%lu path=%ls", GetLastError(), node->cachePath.c_str());
        delete node;
        return STATUS_ACCESS_DENIED;
    }

    setFileInfo2(fi, false, FILE_ATTRIBUTE_NORMAL, node->curSize,
        DavFileSystem::unixToWinTime(e.modified));
    *ctx = node;
    return STATUS_SUCCESS;
}

NTSTATUS Create(FSP_FILE_SYSTEM*, PWSTR FileName, UINT32 CreateOptions,
    UINT32 GrantedAccess, UINT32 FileAttributes, PSECURITY_DESCRIPTOR SecurityDescriptor,
    UINT64 AllocationSize, PVOID* PFileContext, FSP_FSCTL_FILE_INFO* FileInfo) {
    (void)GrantedAccess; (void)FileAttributes;
    (void)SecurityDescriptor; (void)AllocationSize;
    return openOrCreate(FileName, true, CreateOptions, PFileContext, FileInfo);
}

NTSTATUS Open(FSP_FILE_SYSTEM*, PWSTR FileName, UINT32 CreateOptions,
    UINT32 GrantedAccess, PVOID* PFileContext, FSP_FSCTL_FILE_INFO* FileInfo) {
    (void)GrantedAccess;
    return openOrCreate(FileName, false, CreateOptions, PFileContext, FileInfo);
}

NTSTATUS Overwrite(FSP_FILE_SYSTEM*, PVOID FileContext,
    UINT32 FileAttributes, BOOLEAN ReplaceFileAttributes, UINT64 AllocationSize, FSP_FSCTL_FILE_INFO* FileInfo) {
    (void)FileAttributes; (void)ReplaceFileAttributes; (void)AllocationSize;
    FileNode* node = (FileNode*)FileContext;
    if (node->hFile == INVALID_HANDLE_VALUE) return STATUS_ACCESS_DENIED;
    std::lock_guard<std::mutex> lock(node->fmu);
    LARGE_INTEGER zero = {};
    SetFilePointerEx(node->hFile, zero, nullptr, FILE_BEGIN);
    SetEndOfFile(node->hFile);
    node->chunks.clear();
    node->curSize = 0;
    node->dirty = true;
    setFileInfo2(FileInfo, false, FILE_ATTRIBUTE_NORMAL, 0, 0);
    return STATUS_SUCCESS;
}

VOID Cleanup(FSP_FILE_SYSTEM*, PVOID FileContext, PWSTR FileName, ULONG Flags) {
    (void)FileName;
    DavFileSystem* self = g_fs;
    FileNode* node = (FileNode*)FileContext;
    if (!node) return;
    bool doDelete = (Flags & FspCleanupDelete) != 0;
    if (doDelete) node->deleted = true;
    if (doDelete) fslog("Cleanup: delete flag for %s", node->remotePath.c_str());
    if (node->dirty && !node->deleted && node->hFile != INVALID_HANDLE_VALUE) {
        std::string err;
        submitUpload(self, node, err);
    }
}

VOID Close(FSP_FILE_SYSTEM*, PVOID FileContext) {
    FileNode* node = (FileNode*)FileContext;
    if (!node) return;
    DavFileSystem* self = g_fs;
    if (node->hFile != INVALID_HANDLE_VALUE) CloseHandle(node->hFile);
    if (node->deleted) {
        // 取消未完成的上传，避免"删除后幽灵文件"
        {
            std::lock_guard<std::mutex> cl(g_cancelMtx);
            g_cancel.insert(node->remotePath);
        }
        {
            std::lock_guard<std::mutex> pl(g_pendingMtx);
            g_pending.erase(node->remotePath);
        }
        std::string err;
        if (!self->dav_.remove(node->remotePath, err)) {
            // 404 可能因上传尚未完成，由 worker 补偿删除；其余错误记日志
            fslog("Close: remove %s => %s", node->remotePath.c_str(), err.c_str());
        }
        self->invalidate(node->remotePath);
        self->invalidate(parentOf(node->remotePath));
    }
    // uploadPending：缓存文件已入后台上传队列，由工作线程上传后删除
    if (!node->cachePending && !node->cachePath.empty())
        DeleteFileW(node->cachePath.c_str());
    delete node;
}

NTSTATUS Read(FSP_FILE_SYSTEM*, PVOID FileContext, PVOID Buffer,
    UINT64 Offset, ULONG Length, PULONG PBytesTransferred) {
    FileNode* node = (FileNode*)FileContext;
    if (node->hFile == INVALID_HANDLE_VALUE) return STATUS_ACCESS_VIOLATION;

    std::string err;
    {
        std::lock_guard<std::mutex> lock(node->fmu);
        if (Offset >= node->curSize) { *PBytesTransferred = 0; return STATUS_END_OF_FILE; }
    }
    UINT64 len = std::min<UINT64>(Length, node->curSize - Offset);
    if (len == 0) { *PBytesTransferred = 0; return STATUS_SUCCESS; }

    if (!ensureRange(node, Offset, len, err)) return STATUS_DATA_ERROR;

    DWORD got = 0;
    if (!pread(node->hFile, Buffer, (size_t)len, Offset, got)) return STATUS_ACCESS_VIOLATION;
    *PBytesTransferred = got;
    return STATUS_SUCCESS;
}

NTSTATUS Write(FSP_FILE_SYSTEM*, PVOID FileContext, PVOID Buffer,
    UINT64 Offset, ULONG Length, BOOLEAN WriteToEndOfFile, BOOLEAN ConstrainedIo,
    PULONG PBytesTransferred, FSP_FSCTL_FILE_INFO* FileInfo) {
    FileNode* node = (FileNode*)FileContext;
    if (node->hFile == INVALID_HANDLE_VALUE) return STATUS_ACCESS_VIOLATION;

    UINT64 writeOffset, sizeSnap;
    {
        std::lock_guard<std::mutex> lock(node->fmu);
        sizeSnap = node->curSize;
    }
    writeOffset = WriteToEndOfFile ? sizeSnap : Offset;
    if (ConstrainedIo) {
        if (writeOffset > sizeSnap) { *PBytesTransferred = 0; return STATUS_SUCCESS; }
        Length = (ULONG)std::min<UINT64>(Length, sizeSnap - writeOffset);
    }

    std::string err;
    // 覆盖已有数据区域且未写满整块的，需要先填充块
    if (writeOffset < node->remoteSize) {
        UINT64 end = writeOffset + Length;
        uint64_t b = writeOffset / CHUNK_SIZE, en = (end - 1) / CHUNK_SIZE;
        bool partial = false;
        {
            std::lock_guard<std::mutex> lock(node->fmu);
            for (uint64_t c = b; c <= en; ++c) {
                uint64_t cs = c * CHUNK_SIZE, ce = cs + CHUNK_SIZE;
                if (!(node->chunks.count(c) && node->chunks[c].filled) &&
                    !(writeOffset <= cs && end >= ce))
                    partial = true;
            }
        }
        if (partial && !ensureRange(node, writeOffset, Length, err))
            return STATUS_DATA_ERROR;
    }

    if (!pwrite(node->hFile, Buffer, Length, writeOffset)) return STATUS_ACCESS_VIOLATION;
    {
        std::lock_guard<std::mutex> lock(node->fmu);
        node->dirty = true;
        if (writeOffset + Length > node->curSize) node->curSize = writeOffset + Length;
        // 标记完全被本次写入覆盖的块为已填充；部分覆盖的块，
        // 其未写部分若在远端不存在也视为已填充（无内容可下载）
        UINT64 end = writeOffset + Length;
        for (uint64_t c = writeOffset / CHUNK_SIZE; c <= (end - 1) / CHUNK_SIZE; ++c) {
            uint64_t cs = c * CHUNK_SIZE, ce = cs + CHUNK_SIZE;
            bool fullyCovered = writeOffset <= cs && end >= ce;
            bool restBeyondRemote = std::max(ce, end) > node->remoteSize && writeOffset >= node->remoteSize;
            if (fullyCovered || restBeyondRemote)
                node->chunks[c].filled = true;
        }
    }
    *PBytesTransferred = Length;
    setFileInfo2(FileInfo, false, FILE_ATTRIBUTE_NORMAL, node->curSize, 0);
    return STATUS_SUCCESS;
}

NTSTATUS Flush(FSP_FILE_SYSTEM*, PVOID FileContext, FSP_FSCTL_FILE_INFO* FileInfo) {
    FileNode* node = (FileNode*)FileContext;
    if (node && !node->isDir && node->hFile != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(node->hFile);
        if (node->dirty) {
            std::string err;
            submitUpload(g_fs, node, err);
        }
    }
    if (FileInfo && node) {
        std::lock_guard<std::mutex> lock(node->fmu);
        setFileInfo2(FileInfo, node->isDir,
            node->isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
            node->curSize, 0);
    }
    return STATUS_SUCCESS;
}

NTSTATUS GetFileInfo(FSP_FILE_SYSTEM*, PVOID FileContext, FSP_FSCTL_FILE_INFO* FileInfo) {
    FileNode* node = (FileNode*)FileContext;
    std::lock_guard<std::mutex> lock(node->fmu);
    setFileInfo2(FileInfo, node->isDir,
        node->isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
        node->curSize, 0);
    return STATUS_SUCCESS;
}

NTSTATUS SetFileSize(FSP_FILE_SYSTEM*, PVOID FileContext,
    UINT64 NewSize, BOOLEAN SetAllocationSize, FSP_FSCTL_FILE_INFO* FileInfo) {
    (void)SetAllocationSize;
    FileNode* node = (FileNode*)FileContext;
    if (node->hFile == INVALID_HANDLE_VALUE) return STATUS_ACCESS_DENIED;
    std::lock_guard<std::mutex> lock(node->fmu);
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)NewSize;
    if (!SetFilePointerEx(node->hFile, li, nullptr, FILE_BEGIN)) return STATUS_ACCESS_VIOLATION;
    if (!SetEndOfFile(node->hFile)) return STATUS_ACCESS_VIOLATION;
    // 截断：丢弃超出的块状态
    uint64_t lastChunk = (NewSize + CHUNK_SIZE - 1) / CHUNK_SIZE;
    for (auto it = node->chunks.begin(); it != node->chunks.end();) {
        if (it->first >= lastChunk) it = node->chunks.erase(it);
        else ++it;
    }
    node->curSize = NewSize;
    node->dirty = true;
    setFileInfo2(FileInfo, false, FILE_ATTRIBUTE_NORMAL, NewSize, 0);
    return STATUS_SUCCESS;
}

NTSTATUS SetBasicInfo(FSP_FILE_SYSTEM*, PVOID FileContext, UINT32 FileAttributes,
    UINT64 CreationTime, UINT64 LastAccessTime, UINT64 LastWriteTime, UINT64 ChangeTime,
    FSP_FSCTL_FILE_INFO* FileInfo) {
    (void)FileAttributes; (void)CreationTime; (void)ChangeTime; (void)LastAccessTime;
    FileNode* node = (FileNode*)FileContext;
    UINT64 size;
    {
        std::lock_guard<std::mutex> lock(node->fmu);
        size = node->curSize;
    }
    setFileInfo2(FileInfo, node->isDir,
        node->isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, size, LastWriteTime);
    return STATUS_SUCCESS;
}

NTSTATUS CanDelete(FSP_FILE_SYSTEM*, PVOID, PWSTR FileName) {
    (void)FileName;
    return STATUS_SUCCESS;
}

NTSTATUS Rename(FSP_FILE_SYSTEM*, PVOID FileContext,
    PWSTR FileName, PWSTR NewFileName, BOOLEAN ReplaceIfExists) {
    DavFileSystem* self = g_fs;
    FileNode* node = (FileNode*)FileContext;
    std::string from = normRemote(toRemote(FileName));
    std::string to = normRemote(toRemote(NewFileName));
    fslog("Rename: %s -> %s (replace=%d)", from.c_str(), to.c_str(), (int)ReplaceIfExists);

    if (!ReplaceIfExists) {
        WebDavEntry e;
        std::string err;
        if (self->cachedStat(to, e, err)) return STATUS_OBJECT_NAME_COLLISION;
    }
    std::string err;
    if (!self->dav_.move(from, to, err)) {
        // 大小写不敏感回退：列出父目录，找到实际大小写的远程路径
        std::string parent = parentOf(from);
        std::string fname = from.substr(from.rfind('/') + 1);
        std::vector<WebDavEntry> ents;
        std::string lerr;
        bool movedOk = false;
        if (self->dav_.listDir(parent, ents, lerr)) {
            auto ieq = [](const std::string& a, const std::string& b) {
                if (a.size() != b.size()) return false;
                for (size_t i = 0; i < a.size(); ++i)
                    if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
                return true;
            };
            for (auto& e : ents) {
                if (ieq(e.name, fname)) {
                    std::string real = (parent == "/" ? "" : parent) + "/" + e.name;
                    if (self->dav_.move(real, to, err)) { movedOk = true; break; }
                }
            }
        }
        if (!movedOk) {
            fslog("Rename: MOVE FAILED %s", err.c_str());
            return STATUS_ACCESS_DENIED;
        }
    }

    std::wstring newCache = self->cachePathFor(to);
    if (node->hFile != INVALID_HANDLE_VALUE) CloseHandle(node->hFile);
    if (!node->cachePath.empty())
        MoveFileW(node->cachePath.c_str(), newCache.c_str());
    node->remotePath = to;
    node->cachePath = newCache;
    if (!node->isDir)
        node->hFile = CreateFileW(newCache.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    self->invalidate(from);
    self->invalidate(to);
    self->invalidate(parentOf(from));
    self->invalidate(parentOf(to));
    return STATUS_SUCCESS;
}

NTSTATUS ReadDirectory(FSP_FILE_SYSTEM*, PVOID FileContext, PWSTR Pattern,
    PWSTR Marker, PVOID Buffer, ULONG Length, PULONG PBytesTransferred) {
    (void)Pattern;
    DavFileSystem* self = g_fs;
    FileNode* node = (FileNode*)FileContext;
    std::string dir = node ? node->remotePath : "/";
    std::vector<WebDavEntry> entries;
    std::string err;
    if (!self->cachedListDir(dir, entries, err)) {
        fslog("ReadDirectory: listDir FAILED %s", err.c_str());
        return STATUS_ACCESS_DENIED;
    }

    *PBytesTransferred = 0;
    fslog("ReadDirectory: dir=%s entries=%d marker=%s", dir.c_str(), (int)entries.size(), (Marker && *Marker) ? "yes" : "no");
    // WinFsp 用 Marker（上次最后返回的文件名）续传，必须保证条目按名字升序输出
    std::sort(entries.begin(), entries.end(), [](const WebDavEntry& a, const WebDavEntry& b) {
        return u8w(a.name) < u8w(b.name);
    });
    std::wstring wmarker = (Marker && *Marker) ? std::wstring(Marker) : L"";
    for (auto& e : entries) {
        std::wstring wname = u8w(e.name);
        if (!wmarker.empty()) {
            if (wname <= wmarker) continue;
        }
        size_t need = offsetof(FSP_FSCTL_DIR_INFO, FileNameBuf) + (wname.size() + 1) * sizeof(WCHAR);
        std::vector<char> dbuf(need);
        FSP_FSCTL_DIR_INFO* pdi = (FSP_FSCTL_DIR_INFO*)dbuf.data();
        memset(pdi, 0, need);
        pdi->Size = (UINT16)need;
        if (e.isDir) pdi->FileInfo.FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
        else {
            pdi->FileInfo.FileAttributes = FILE_ATTRIBUTE_NORMAL;
            pdi->FileInfo.FileSize = e.size;
            pdi->FileInfo.AllocationSize = e.size;
        }
        pdi->FileInfo.CreationTime = DavFileSystem::unixToWinTime(e.modified);
        pdi->FileInfo.LastWriteTime = DavFileSystem::unixToWinTime(e.modified);
        pdi->FileInfo.LastAccessTime = pdi->FileInfo.LastWriteTime;
        pdi->FileInfo.ChangeTime = pdi->FileInfo.LastWriteTime;
        memcpy(pdi->FileNameBuf, wname.c_str(), (wname.size() + 1) * sizeof(WCHAR));
        if (!FspFileSystemAddDirInfo(pdi, Buffer, Length, PBytesTransferred))
            return STATUS_SUCCESS;
    }
    return STATUS_SUCCESS;
}

NTSTATUS GetDirInfoByName(FSP_FILE_SYSTEM*, PVOID, PWSTR FileName, FSP_FSCTL_DIR_INFO* DirInfo) {
    DavFileSystem* self = g_fs;
    std::wstring wfile(FileName);
    size_t s = wfile.find_last_of(L'\\');
    std::wstring wparent = (s == std::wstring::npos) ? L"" : wfile.substr(0, s);
    std::wstring wname = (s == std::wstring::npos) ? wfile : wfile.substr(s + 1);

    std::string parent = wparent.empty() ? "/" : normRemote(wu8(wparent));
    std::string remote = parent + (parent == "/" ? "" : "/") + wu8(wname);
    WebDavEntry e;
    std::string err;
    if (!self->cachedStat(remote, e, err)) return STATUS_OBJECT_NAME_NOT_FOUND;
    memset(DirInfo, 0, sizeof(*DirInfo));
    DirInfo->Size = (UINT16)(offsetof(FSP_FSCTL_DIR_INFO, FileNameBuf) + (wname.size() + 1) * sizeof(WCHAR));
    DirInfo->FileInfo.FileAttributes = e.isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    DirInfo->FileInfo.FileSize = e.size;
    DirInfo->FileInfo.AllocationSize = e.size;
    DirInfo->FileInfo.CreationTime = DavFileSystem::unixToWinTime(e.modified);
    DirInfo->FileInfo.LastWriteTime = DavFileSystem::unixToWinTime(e.modified);
    DirInfo->FileInfo.LastAccessTime = DirInfo->FileInfo.LastWriteTime;
    DirInfo->FileInfo.ChangeTime = DirInfo->FileInfo.LastWriteTime;
    memcpy(DirInfo->FileNameBuf, wname.c_str(), (wname.size() + 1) * sizeof(WCHAR));
    return STATUS_SUCCESS;
}

NTSTATUS GetSecurity(FSP_FILE_SYSTEM*, PVOID FileContext,
    PSECURITY_DESCRIPTOR SecurityDescriptor, SIZE_T* PSecurityDescriptorSize) {
    (void)FileContext;
    PSECURITY_DESCRIPTOR sd = defaultSecurityDescriptor();
    DWORD sz = GetSecurityDescriptorLength(sd);
    if (*PSecurityDescriptorSize < sz) { *PSecurityDescriptorSize = sz; return STATUS_BUFFER_OVERFLOW; }
    memcpy(SecurityDescriptor, sd, sz);
    *PSecurityDescriptorSize = sz;
    return STATUS_SUCCESS;
}

NTSTATUS SetSecurity(FSP_FILE_SYSTEM*, PVOID, SECURITY_INFORMATION, PSECURITY_DESCRIPTOR) {
    return STATUS_SUCCESS;
}

} // namespace fsops

// ---------- 挂载 ----------
static void buildInterface(FSP_FILE_SYSTEM_INTERFACE* itf) {
    memset(itf, 0, sizeof(*itf));
    itf->GetVolumeInfo = fsops::GetVolumeInfo;
    itf->GetSecurityByName = fsops::GetSecurityByName;
    itf->Create = fsops::Create;
    itf->Open = fsops::Open;
    itf->Overwrite = fsops::Overwrite;
    itf->Cleanup = fsops::Cleanup;
    itf->Close = fsops::Close;
    itf->Read = fsops::Read;
    itf->Write = fsops::Write;
    itf->Flush = fsops::Flush;
    itf->GetFileInfo = fsops::GetFileInfo;
    itf->SetBasicInfo = fsops::SetBasicInfo;
    itf->SetFileSize = fsops::SetFileSize;
    itf->CanDelete = fsops::CanDelete;
    itf->Rename = fsops::Rename;
    itf->GetSecurity = fsops::GetSecurity;
    itf->SetSecurity = fsops::SetSecurity;
    itf->ReadDirectory = fsops::ReadDirectory;
    itf->GetDirInfoByName = fsops::GetDirInfoByName;
}

bool DavFileSystem::mount(const std::wstring& mountPoint, std::string& err) {
    ensureCacheDir();
    defaultSecurityDescriptor();

    FSP_FSCTL_VOLUME_PARAMS vp = {};
    vp.Version = sizeof(FSP_FSCTL_VOLUME_PARAMS);
    vp.SectorSize = 4096;
    vp.SectorsPerAllocationUnit = 1;
    vp.VolumeCreationTime = (UINT64)time(nullptr) * 10000000ULL + 116444736000000000ULL;
    vp.VolumeSerialNumber = (UINT32)(GetTickCount() | 0x80000000);
    vp.MaxComponentLength = 255;
    vp.ReadOnlyVolume = FALSE;
    vp.CaseSensitiveSearch = FALSE;
    vp.CasePreservedNames = TRUE;
    vp.UnicodeOnDisk = TRUE;
    vp.PersistentAcls = FALSE;
    vp.PostCleanupWhenModifiedOnly = 1;
    vp.FlushAndPurgeOnCleanup = 1;
    vp.UmFileContextIsUserContext2 = TRUE;
    wcscpy_s(vp.FileSystemName, L"Wddisk");

    static FSP_FILE_SYSTEM_INTERFACE itf;
    buildInterface(&itf);

    NTSTATUS st = FspFileSystemCreate((PWSTR)L"" FSP_FSCTL_DISK_DEVICE_NAME, &vp, &itf, &fs_);
    if (!NT_SUCCESS(st)) { err = "FspFileSystemCreate 失败 (WinFSP 驱动未安装？)"; return false; }
    FspFileSystemSetOperationGuardStrategy(fs_, FSP_FILE_SYSTEM_OPERATION_GUARD_STRATEGY_FINE);
    startUploadWorker();

    wchar_t mp[8];
    wcscpy_s(mp, mountPoint.c_str());
    st = FspFileSystemSetMountPoint(fs_, mp);
    if (!NT_SUCCESS(st)) {
        err = "设置挂载点失败（盘符可能被占用）";
        FspFileSystemDelete(fs_); fs_ = nullptr;
        return false;
    }
    st = FspFileSystemStartDispatcher(fs_, 0);
    if (!NT_SUCCESS(st)) {
        err = "启动文件系统调度失败";
        FspFileSystemDelete(fs_); fs_ = nullptr;
        return false;
    }
    return true;
}

void DavFileSystem::unmount() {
    if (fs_) {
        FspFileSystemStopDispatcher(fs_);
        FspFileSystemDelete(fs_);
        fs_ = nullptr;
    }
    stopUploadWorker(); // 排空后台上传队列
}
