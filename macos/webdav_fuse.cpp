// webdav_fuse.cpp - macFUSE 实现
#include "webdav_fuse.h"
#define FUSE_USE_VERSION 26
#include <fuse.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>

using namespace wddisk;

static DavFuseFs* g_fs = nullptr;

std::string DavFuseFs::cachePathFor(const std::string& remotePath) const {
    unsigned long long h = 1469598103934665603ULL;
    for (unsigned char c : remotePath) { h ^= c; h *= 1099511628211ULL; }
    char buf[64];
    snprintf(buf, sizeof(buf), "%016llx.bin", h);
    return cacheDir_ + "/" + buf;
}

static std::string remotePath(const char* p) {
    std::string s = p ? p : "/";
    if (s.empty()) s = "/";
    return s;
}

static void fillStat(struct stat& st, bool isDir, off_t size, time_t mtime, mode_t extra = 0) {
    memset(&st, 0, sizeof(st));
    st.st_blksize = 4096;
    st.st_nlink = 1;
    if (isDir) {
        st.st_mode = S_IFDIR | 0755;
        st.st_nlink = 2;
        st.st_size = 512;
    } else {
        st.st_mode = S_IFREG | 0644 | extra;
        st.st_size = size;
    }
    st.st_uid = getuid();
    st.st_gid = getgid();
    st.st_atime = st.st_mtime = st.st_ctime = mtime;
}

// 将本地缓存文件 stat 转换
static void statCache(const std::string& cachePath, struct stat& st, bool isDir) {
    struct stat c;
    if (::stat(cachePath.c_str(), &c) == 0) {
        st.st_size = c.st_size;
        st.st_mtime = st.st_atime = st.st_ctime = c.st_mtime;
    }
}

// 打开（或获取）缓存文件，确保内容已下载
static bool ensureCached(const std::string& remote, const std::string& cachePath, std::string& err) {
    struct stat c;
    if (::stat(cachePath.c_str(), &c) == 0) return true; // 已缓存
    std::string data;
    if (!g_fs->dav_.download(remote, data, err)) return false;
    FILE* f = fopen(cachePath.c_str(), "wb");
    if (!f) { err = "cache write failed"; return false; }
    if (!data.empty()) fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return true;
}

static int fs_getattr(const char* path, struct stat* st) {
    std::string remote = remotePath(path);
    WebDavEntry e;
    std::string err;
    if (remote == "/") {
        fillStat(*st, true, 0, time(nullptr));
        return 0;
    }
    if (!g_fs->dav_.stat(remote, e, err)) return -ENOENT;
    fillStat(*st, e.isDir, (off_t)e.size, (time_t)e.modified);
    return 0;
}

static int fs_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
                      off_t offset, struct fuse_file_info* fi) {
    (void)offset; (void)fi;
    std::string remote = remotePath(path);
    std::vector<WebDavEntry> entries;
    std::string err;
    if (!g_fs->dav_.listDir(remote, entries, err)) return -EIO;
    filler(buf, ".", nullptr, 0);
    filler(buf, "..", nullptr, 0);
    for (auto& e : entries) {
        struct stat st;
        fillStat(st, e.isDir, (off_t)e.size, (time_t)e.modified);
        if (filler(buf, e.name.c_str(), &st, 0)) break;
    }
    return 0;
}

struct FileHandle {
    std::string remote;
    std::string cachePath;
    int fd;
    bool dirty;
    bool writeMode;
};

static int fs_open(const char* path, struct fuse_file_info* fi) {
    std::string remote = remotePath(path);
    WebDavEntry e;
    std::string err;
    if (!g_fs->dav_.stat(remote, e, err) || e.isDir) return -ENOENT;

    std::lock_guard<std::mutex> lock(g_fs->mtx_);
    std::string cachePath = g_fs->cachePathFor(remote);
    if (!ensureCached(remote, cachePath, err)) return -EIO;

    FileHandle* h = new FileHandle();
    h->remote = remote;
    h->cachePath = cachePath;
    h->fd = ::open(cachePath.c_str(), O_RDWR);
    h->dirty = false;
    h->writeMode = (fi->flags & (O_WRONLY | O_RDWR)) != 0;
    fi->fh = (uint64_t)h;
    return 0;
}

static int fs_read(const char* path, char* buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    (void)path;
    FileHandle* h = (FileHandle*)fi->fh;
    ssize_t n = pread(h->fd, buf, size, offset);
    return n < 0 ? -EIO : (int)n;
}

static int fs_write(const char* path, const char* buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    (void)path;
    FileHandle* h = (FileHandle*)fi->fh;
    ssize_t n = pwrite(h->fd, buf, size, offset);
    if (n < 0) return -EIO;
    h->dirty = true;
    return (int)n;
}

static int fs_flush(const char* path, struct fuse_file_info* fi) {
    (void)path;
    FileHandle* h = (FileHandle*)fi->fh;
    if (h->dirty) {
        // 读取整个缓存文件上传
        FILE* f = fopen(h->cachePath.c_str(), "rb");
        if (!f) return -EIO;
        std::string data;
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
        fclose(f);
        std::string err;
        if (!g_fs->dav_.upload(h->remote, data, err)) return -EIO;
        h->dirty = false;
    }
    return 0;
}

static int fs_release(const char* path, struct fuse_file_info* fi) {
    (void)path;
    FileHandle* h = (FileHandle*)fi->fh;
    if (h->dirty) fs_flush(path, fi);
    ::close(h->fd);
    std::lock_guard<std::mutex> lock(g_fs->mtx_);
    ::unlink(h->cachePath.c_str());
    delete h;
    return 0;
}

static int fs_create(const char* path, mode_t mode, struct fuse_file_info* fi) {
    (void)mode;
    std::string remote = remotePath(path);
    std::string cachePath = g_fs->cachePathFor(remote);
    FILE* f = fopen(cachePath.c_str(), "wb");
    if (!f) return -EACCES;
    fclose(f);
    FileHandle* h = new FileHandle();
    h->remote = remote;
    h->cachePath = cachePath;
    h->fd = ::open(cachePath.c_str(), O_RDWR);
    h->dirty = true;
    h->writeMode = true;
    fi->fh = (uint64_t)h;
    return 0;
}

static int fs_truncate(const char* path, off_t size) {
    std::string remote = remotePath(path);
    WebDavEntry e;
    std::string err;
    if (!g_fs->dav_.stat(remote, e, err) || e.isDir) return -ENOENT;
    std::lock_guard<std::mutex> lock(g_fs->mtx_);
    std::string cachePath = g_fs->cachePathFor(remote);
    if (!ensureCached(remote, cachePath, err)) return -EIO;
    if (::truncate(cachePath.c_str(), size) != 0) return -EIO;
    FILE* f = fopen(cachePath.c_str(), "rb");
    if (!f) return -EIO;
    std::string data;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    if (!g_fs->dav_.upload(remote, data, err)) return -EIO;
    ::unlink(cachePath.c_str());
    return 0;
}

static int fs_unlink(const char* path) {
    std::string err;
    if (!g_fs->dav_.remove(remotePath(path), err)) return -EIO;
    return 0;
}

static int fs_mkdir(const char* path, mode_t mode) {
    (void)mode;
    std::string err;
    if (!g_fs->dav_.mkdir(remotePath(path), err)) return -EIO;
    return 0;
}

static int fs_rmdir(const char* path) {
    std::string err;
    if (!g_fs->dav_.remove(remotePath(path), err)) return -EIO;
    return 0;
}

static int fs_rename(const char* from, const char* to) {
    std::string err;
    if (!g_fs->dav_.move(remotePath(from), remotePath(to), err)) return -EIO;
    return 0;
}

static int fs_utimens(const char* path, const struct timespec tv[2]) {
    // WebDAV 标准不直接支持改时间（PROPPATCH 可），此处忽略
    (void)path; (void)tv;
    return 0;
}

static int fs_statfs(const char* path, struct statvfs* st) {
    (void)path;
    memset(st, 0, sizeof(*st));
    st->f_bsize = 4096;
    st->f_frsize = 4096;
    st->f_blocks = 0x7FFFFFFF;
    st->f_bfree = 0x3FFFFFFF;
    st->f_bavail = 0x3FFFFFFF;
    st->f_namemax = 255;
    return 0;
}

static void* fs_init(struct fuse_conn_info* conn) {
    (void)conn;
    return nullptr;
}

static struct fuse_operations g_ops;

int DavFuseFs::run(const std::string& mountPoint) {
    g_fs = this;
    memset(&g_ops, 0, sizeof(g_ops));
    g_ops.getattr = fs_getattr;
    g_ops.readdir = fs_readdir;
    g_ops.open = fs_open;
    g_ops.read = fs_read;
    g_ops.write = fs_write;
    g_ops.flush = fs_flush;
    g_ops.release = fs_release;
    g_ops.create = fs_create;
    g_ops.truncate = fs_truncate;
    g_ops.unlink = fs_unlink;
    g_ops.mkdir = fs_mkdir;
    g_ops.rmdir = fs_rmdir;
    g_ops.rename = fs_rename;
    g_ops.utimens = fs_utimens;
    g_ops.statfs = fs_statfs;
    g_ops.init = fs_init;

    // 确保缓存目录存在
    system(("mkdir -p '" + cacheDir_ + "'").c_str());

    char arg0[] = "wddisk";
    char* argv[] = { arg0, (char*)mountPoint.c_str(), (char*)"-f", nullptr };
    int argc = 3;
    // 前台运行（GUI 在专用线程中调用）
    return fuse_main(argc, argv, &g_ops, nullptr);
}

void DavFuseFs::stop() {
    // fuse_unmount 需要挂载点；GUI 卸载时调用
}
