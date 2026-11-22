// webdav.h - 跨平台 WebDAV 客户端接口
// 各平台分别提供 HTTP 实现：Windows=WinHTTP, macOS/Linux=libcurl
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <functional>

namespace wddisk {

struct WebDavEntry {
    std::string path;      // 完整路径，以 / 结尾表示目录
    std::string name;      // 文件名或目录名
    bool isDir = false;
    uint64_t size = 0;
    int64_t modified = 0;  // Unix 秒
};

struct DavConfig {
    std::string name;      // 显示名称
    std::string url;       // 服务器根 URL，如 https://dav.example.com/dav/
    std::string username;
    std::string password;
    bool verifyTls = true;
    bool autoMount = false;  // 程序启动时自动挂载
    std::string mountPoint; // Windows: 盘符 "Z:"; Linux/macOS: 目录路径
};

// 平台需实现的 HTTP 层
struct IHttpClient {
    virtual ~IHttpClient() {}
    // 返回 HTTP 状态码，<0 表示网络错误（错误信息写入 errMsg）
    virtual int request(
        const std::string& method,
        const std::string& url,
        const std::vector<std::string>& headers,   // "Key: Value"
        const std::string& body,
        std::string& response,
        std::string& errMsg) = 0;
};

class WebDavClient {
public:
    explicit WebDavClient(const DavConfig& cfg, IHttpClient* http)
        : cfg_(cfg), http_(http) { normalizeUrl(); }

    // 列目录，dirPath 以 / 开头结尾（根为 "/"）
    bool listDir(const std::string& dirPath, std::vector<WebDavEntry>& out, std::string& err);

    // 判断路径存在与否及类型
    bool stat(const std::string& path, WebDavEntry& out, std::string& err);

    bool download(const std::string& path, std::string& data, std::string& err);
    // 分块下载：只取 [offset, offset+len)。若服务器不支持 Range（返回 200 全量），
    // 自动截取对应区间。返回实际长度，data 填充该区间。
    bool downloadRange(const std::string& path, uint64_t offset, size_t len,
        std::string& data, std::string& err);
    bool upload(const std::string& path, const std::string& data, std::string& err);

    bool mkdir(const std::string& path, std::string& err);
    bool remove(const std::string& path, std::string& err);
    bool move(const std::string& from, const std::string& to, std::string& err);
    bool copy(const std::string& from, const std::string& to, std::string& err);

    const DavConfig& config() const { return cfg_; }

private:
    std::string fullUrl(const std::string& path) const;
    void normalizeUrl();

    DavConfig cfg_;
    IHttpClient* http_;
};

// ---------- 极简 XML 提取（PROPFIND 响应解析） ----------
struct XmlExtract {
    // 找到所有 <tag ...> ... </tag> 的内容（不支持嵌套同名标签，够用于 WebDAV）
    static std::vector<std::string> findAll(const std::string& xml, const std::string& tag);
    // 提取第一个匹配的内容，找不到返回 ""
    static std::string first(const std::string& xml, const std::string& tag);
    static std::string decodeEntities(const std::string& s);
};

} // namespace wddisk
