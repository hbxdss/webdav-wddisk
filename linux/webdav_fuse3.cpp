// webdav_fuse3.cpp - libfuse3 实现（Linux）
// 与 Windows/macOS 端相同策略：打开时下载缓存，flush/release 时上传
#include "webdav_fuse3.h"
#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>

using namespace wddisk;

static DavFuseFs* g_fs = nullptr;

static std::string cachePathFor(const std::string& remotePath) {
    unsigned long long h = 1469598103934665603ULL;
    for (unsigned char c : remotePath) { h ^= c; h *= 1099511628211ULL; }
    char buf[64];
    snprintf(buf, sizeof(buf), "%016llx.bin", h);
    return g_fs->cacheDir_ + "/" + buf;
}

static std::string remotePath(const char* p) {
    std::string s = p ? p : "/";
    if (s.empty()) s = "/";
    return s;
}

static void fillStat(struct stat& st, bool isDir, off_t size, time_t mtime) {
    memset(&st, 0, sizeof(st));
    st.st_blksize = 4096;
    if (isDir) { st.st_mode = S_IFDIR | 0755; st.st_nlink = 2; st.st_size = 512; }
    else { st.st_mode = S_IFREG | 0644; st.st_nlink = 1; st.st_size = size; }
    st.st_uid = getuid();
    st.st_gid = getgid();
    st.st_atime = st.st_mtime = st.st_ctime = mtime;
}

static bool ensureCached(const std::string& remote, const std::string& cachePath, std::string& err) {
    struct stat c;
    if (::stat(cachePath.c_str(), &c) == 0) return true;
    std::string data;
    if (!g_fs->dav_.download(remote, data, err)) return false;
    FILE* f = fopen(cachePath.c_str(), "wb");
    if (!f) { err = "cache write failed"; return false; }
    if (!data.empty()) fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return true;
}

static int fs_getattr(const char* path, struct stat* st, fuse_file_info*) {
    std::string remote = remotePath(path);
    if (remote == "/") { fillStat(*st, true, 0, time(nullptr)); return 0; }
    WebDavEntry e;
    std::string err;
    if (!g_fs->dav_.stat(remote, e, err)) return -ENOENT;
    fillStat(*st, e.isDir, (off_t)e.size, (time_t)e.modified);
    return 0;
}

static int fs_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
                      off_t offset, fuse_file_info*, fuse_readdir_flags) {
    (void)offset;
    std::string remote = remotePath(path);
    std::vector<WebDavEntry> entries;
    std::string err;
    if (!g_fs->dav_.listDir(remote, entries, err)) return -EIO;
    filler(buf, ".", nullptr, 0, FUSE_FILL_DIR_PLUS);
    filler(buf, "..", nullptr, 0, FUSE_FILL_DIR_PLUS);
    for (auto& e : entries) {
        struct stat st;
        fillStat(st, e.isDir, (off_t)e.size, (time_t)e.modified);
        if (filler(buf, e.name.c_str(), &st, 0, FUSE_FILL_DIR_PLUS)) break;
    }
    return 0;
}

struct FileHandle {
    std::string remote;
    std::string cachePath;
    int fd;
    bool dirty;
};

static int fs_open(const char* path, fuse_file_info* fi) {
    std::string remote = remotePath(path);
    WebDavEntry e;
    std::string err;
    if (!g_fs->dav_.stat(remote, e, err) || e.isDir) return -ENOENT;
    std::lock_guard<std::mutex> lock(g_fs->mtx_);
    std::string cachePath = cachePathFor(remote);
    if (!ensureCached(remote, cachePath, err)) return -EIO;
    FileHandle* h = new FileHandle();
    h->remote = remote;
    h->cachePath = cachePath;
    h->fd = ::open(cachePath.c_str(), O_RDWR);
    h->dirty = false;
    fi->fh = (uint64_t)h;
    return 0;
}

static int fs_read(const char* path, char* buf, size_t size, off_t offset, fuse_file_info* fi) {
    (void)path;
    FileHandle* h = (FileHandle*)fi->fh;
    ssize_t n = pread(h->fd, buf, size, offset);
    return n < 0 ? -EIO : (int)n;
}

static int fs_write(const char* path, const char* buf, size_t size, off_t offset, fuse_file_info* fi) {
    (void)path;
    FileHandle* h = (FileHandle*)fi->fh;
    ssize_t n = pwrite(h->fd, buf, size, offset);
    if (n < 0) return -EIO;
    h->dirty = true;
    return (int)n;
}

static int uploadHandle(FileHandle* h) {
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
    return 0;
}

static int fs_flush(const char* path, fuse_file_info* fi) {
    (void)path;
    FileHandle* h = (FileHandle*)fi->fh;
    if (h->dirty) return uploadHandle(h);
    return 0;
}

static int fs_release(const char* path, fuse_file_info* fi) {
    (void)path;
    FileHandle* h = (FileHandle*)fi->fh;
    if (h->dirty) uploadHandle(h);
    ::close(h->fd);
    std::lock_guard<std::mutex> lock(g_fs->mtx_);
    ::unlink(h->cachePath.c_str());
    delete h;
    return 0;
}

static int fs_create(const char* path, mode_t mode, fuse_file_info* fi) {
    (void)mode;
    std::string remote = remotePath(path);
    std::string cachePath = cachePathFor(remote);
    FILE* f = fopen(cachePath.c_str(), "wb");
    if (!f) return -EACCES;
    fclose(f);
    FileHandle* h = new FileHandle();
    h->remote = remote;
    h->cachePath = cachePath;
    h->fd = ::open(cachePath.c_str(), O_RDWR);
    h->dirty = true;
    fi->fh = (uint64_t)h;
    return 0;
}

static int fs_truncate(const char* path, off_t size, fuse_file_info* fi) {
    if (fi && fi->fh) {
        FileHandle* h = (FileHandle*)fi->fh;
        if (ftruncate(h->fd, size) != 0) return -EIO;
        h->dirty = true;
        return 0;
    }
    std::string remote = remotePath(path);
    WebDavEntry e;
    std::string err;
    if (!g_fs->dav_.stat(remote, e, err) || e.isDir) return -ENOENT;
    std::lock_guard<std::mutex> lock(g_fs->mtx_);
    std::string cachePath = cachePathFor(remote);
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

static int fs_mkdir(const char* path, mode_t) {
    std::string err;
    if (!g_fs->dav_.mkdir(remotePath(path), err)) return -EIO;
    return 0;
}

static int fs_rmdir(const char* path) {
    std::string err;
    if (!g_fs->dav_.remove(remotePath(path), err)) return -EIO;
    return 0;
}

static int fs_rename(const char* from, const char* to, unsigned int flags) {
    if (flags) return -EINVAL; // 不支持 renameat2 语义
    std::string err;
    if (!g_fs->dav_.move(remotePath(from), remotePath(to), err)) return -EIO;
    return 0;
}

static int fs_utimens(const char* path, const struct timespec tv[2], fuse_file_info*) {
    (void)path; (void)tv; // WebDAV 需 PROPPATCH，暂忽略
    return 0;
}

static int fs_statfs(const char* path, struct statvfs* st) {
    (void)path;
    memset(st, 0, sizeof(*st));
    st->f_bsize = st->f_frsize = 4096;
    st->f_blocks = 0x7FFFFFFF;
    st->f_bfree = st->f_bavail = 0x3FFFFFFF;
    st->f_namemax = 255;
    return 0;
}

int DavFuseFs::run(const std::string& mountPoint) {
    g_fs = this;
    fuse_operations ops = {};
    ops.getattr = fs_getattr;
    ops.readdir = fs_readdir;
    ops.open = fs_open;
    ops.read = fs_read;
    ops.write = fs_write;
    ops.flush = fs_flush;
    ops.release = fs_release;
    ops.create = fs_create;
    ops.truncate = fs_truncate;
    ops.unlink = fs_unlink;
    ops.mkdir = fs_mkdir;
    ops.rmdir = fs_rmdir;
    ops.rename = fs_rename;
    ops.utimens = fs_utimens;
    ops.statfs = fs_statfs;

    system(("mkdir -p '" + cacheDir_ + "'").c_str());

    char arg0[] = "wddisk";
    char* argv[] = { arg0, (char*)mountPoint.c_str(), (char*)"-f", nullptr };
    return fuse_main(3, argv, &ops, nullptr);
}
