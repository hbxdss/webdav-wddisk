// webdav.cpp - WebDAV 客户端公共实现（HTTP 层由平台注入）
#include "webdav.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>

namespace wddisk {

void WebDavClient::normalizeUrl() {
    // 统一为协议://host[:port]/path/ 形式（以 / 结尾）
    std::string u = cfg_.url;
    while (!u.empty() && (u.back() == '/' || u.back() == ' ')) u.pop_back();
    cfg_.url = u;
}

std::string WebDavClient::fullUrl(const std::string& path) const {
    std::string p = path;
    if (p.empty()) p = "/";
    if (p[0] != '/') p = "/" + p;
    // URL 编码路径中不安全字符（保留 /）
    std::string enc;
    char buf[8];
    for (unsigned char c : p) {
        if (isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') enc += (char)c;
        else { snprintf(buf, sizeof(buf), "%%%02X", c); enc += buf; }
    }
    return cfg_.url + enc;
}

bool WebDavClient::listDir(const std::string& dirPath, std::vector<WebDavEntry>& out, std::string& err) {
    const char* body =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<d:propfind xmlns:d=\"DAV:\"><d:prop>"
        "<d:displayname/><d:resourcetype/><d:getcontentlength/>"
        "<d:getlastmodified/></d:prop></d:propfind>";

    std::string resp;
    int code = http_->request("PROPFIND", fullUrl(dirPath),
        { "Depth: 1", "Content-Type: application/xml; charset=utf-8" },
        body, resp, err);
    if (code == 404) { err = "目录不存在 (404)"; return false; }
    if (code == 401) { err = "认证失败 (401)，请检查用户名密码"; return false; }
    if (code < 200 || code >= 300) {
        if (err.empty()) err = "PROPFIND 失败，HTTP " + std::to_string(code);
        return false;
    }

    // 拆出每个 <d:response> ... </d:response>
    std::vector<std::string> responses;
    {
        size_t pos = 0;
        while (true) {
            size_t s = resp.find("<d:response", pos);
            if (s == std::string::npos) s = resp.find("<D:response", pos);
            if (s == std::string::npos) s = resp.find("<response", pos);
            if (s == std::string::npos) break;
            size_t e = resp.find("</d:response>", s);
            size_t e2 = resp.find("</D:response>", s);
            size_t e3 = resp.find("</response>", s);
            size_t end = std::min(std::min(
                e == std::string::npos ? resp.size() : e,
                e2 == std::string::npos ? resp.size() : e2),
                e3 == std::string::npos ? resp.size() : e3);
            if (end >= resp.size()) break;
            responses.push_back(resp.substr(s, end - s));
            pos = end;
        }
    }

    auto tag = [&](const std::string& xml, const char* names[]) -> std::string {
        for (int i = 0; names[i]; ++i) {
            std::string v = XmlExtract::first(xml, names[i]);
            if (!v.empty()) return v;
        }
        return "";
    };
    const char* hrefTags[] = { "d:href", "D:href", "href", nullptr };
    const char* lenTags[] = { "d:getcontentlength", "D:getcontentlength", "getcontentlength", nullptr };
    const char* modTags[] = { "d:getlastmodified", "D:getlastmodified", "getlastmodified", nullptr };
    const char* collTags[] = { "d:collection", "D:collection", "collection", nullptr };
    const char* nameTags[] = { "d:displayname", "D:displayname", "displayname", nullptr };

    // URL 解码（%XX）
    auto urlDecode = [](const std::string& s) {
        std::string o;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size()) {
                char b[3] = { s[i + 1], s[i + 2], 0 };
                o += (char)strtoul(b, nullptr, 16);
                i += 2;
            } else o += s[i];
        }
        return o;
    };

    // HTTP 日期解析
    auto parseHttpDate = [](const std::string& d) -> int64_t {
        if (d.empty()) return 0;
        struct tm tmv = {};
        char mon[4] = {};
        if (sscanf(d.c_str(), "%*s %2d %3s %4d %2d:%2d:%2d",
            &tmv.tm_mday, mon, &tmv.tm_year, &tmv.tm_hour, &tmv.tm_min, &tmv.tm_sec) < 5) return 0;
        const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
        const char* m = strstr(months, mon);
        if (m) tmv.tm_mon = (int)(m - months) / 3;
        tmv.tm_year -= 1900;
        tmv.tm_isdst = -1;
        return (int64_t)mktime(&tmv);
    };

    for (auto& r : responses) {
        std::string href = urlDecode(tag(r, hrefTags));
        if (href.empty()) continue;

        // 去掉 query
        size_t q = href.find('?');
        if (q != std::string::npos) href = href.substr(0, q);

        WebDavEntry e;
        // href 通常是 URL 路径；取 last segment
        std::string clean = href;
        if (!clean.empty() && clean.back() == '/') { clean.pop_back(); e.isDir = true; }
        if (r.find("<d:collection") != std::string::npos ||
            r.find("<D:collection") != std::string::npos ||
            r.find("<collection") != std::string::npos) e.isDir = true;

        // 相对 dirPath 判断是否为直接子项；跳过目录自身的 response
        std::string hrefNoSlash = href;
        while (!hrefNoSlash.empty() && hrefNoSlash.back() == '/') hrefNoSlash.pop_back();
        std::string dirNoSlash = dirPath;
        while (dirNoSlash.size() > 1 && dirNoSlash.back() == '/') dirNoSlash.pop_back();
        // 服务器 href 通常带 URL 基础路径前缀（如 /webdav），需先拼接再比较
        std::string basePath;
        {
            std::string u = cfg_.url;
            size_t sp = u.find("://");
            if (sp != std::string::npos) {
                std::string after = u.substr(sp + 3);
                size_t sl = after.find('/');
                basePath = (sl == std::string::npos) ? "/" : after.substr(sl);
                while (basePath.size() > 1 && basePath.back() == '/') basePath.pop_back();
            }
        }
        std::string selfPath = (dirNoSlash == "/" || dirNoSlash.empty())
            ? basePath : (basePath + dirNoSlash);
        if (hrefNoSlash == selfPath || hrefNoSlash == dirNoSlash || hrefNoSlash.empty()) continue;

        size_t slash = hrefNoSlash.rfind('/');
        std::string base = (slash == std::string::npos) ? hrefNoSlash : hrefNoSlash.substr(slash + 1);

        e.path = href;
        if (e.isDir && !e.path.empty() && e.path.back() != '/') e.path += "/";
        e.name = tag(r, nameTags);
        if (e.name.empty()) e.name = base;
        std::string len = tag(r, lenTags);
        if (!len.empty() && !e.isDir) e.size = strtoull(len.c_str(), nullptr, 10);
        e.modified = parseHttpDate(tag(r, modTags));

        out.push_back(e);
    }
    return true;
}

bool WebDavClient::stat(const std::string& path, WebDavEntry& out, std::string& err) {
    std::string dir = path;
    bool trailing = !dir.empty() && dir.back() == '/';
    // 对文件本身做 Depth:0 PROPFIND
    std::string resp;
    int code = http_->request("PROPFIND", fullUrl(path),
        { "Depth: 0", "Content-Type: application/xml; charset=utf-8" },
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<d:propfind xmlns:d=\"DAV:\"><d:prop>"
        "<d:resourcetype/><d:getcontentlength/><d:getlastmodified/>"
        "</d:prop></d:propfind>", resp, err);
    if (code == 404) { err = "not found"; return false; }
    if (code < 200 || code >= 300) {
        if (err.empty()) err = "PROPFIND HTTP " + std::to_string(code);
        return false;
    }
    out.path = path;
    out.isDir = trailing
        || resp.find("<d:collection") != std::string::npos
        || resp.find("<D:collection") != std::string::npos
        || resp.find("<collection") != std::string::npos;
    std::string len = XmlExtract::first(resp, "d:getcontentlength");
    if (len.empty()) len = XmlExtract::first(resp, "D:getcontentlength");
    if (!len.empty()) out.size = strtoull(len.c_str(), nullptr, 10);
    return true;
}

bool WebDavClient::download(const std::string& path, std::string& data, std::string& err) {
    std::string resp;
    int code = http_->request("GET", fullUrl(path), {}, "", resp, err);
    if (code == 200) { data = std::move(resp); return true; }
    if (err.empty()) err = "下载失败 HTTP " + std::to_string(code);
    return false;
}

bool WebDavClient::downloadRange(const std::string& path, uint64_t offset, size_t len,
    std::string& data, std::string& err) {
    if (len == 0) { data.clear(); return true; }
    std::string resp;
    std::string range = "Range: bytes=" + std::to_string(offset) + "-" +
        std::to_string(offset + len - 1);
    int code = http_->request("GET", fullUrl(path), { range }, "", resp, err);
    if (code != 200 && code != 206) {
        if (err.empty()) err = "Range 下载失败 HTTP " + std::to_string(code);
        return false;
    }
    if (code == 206 && resp.size() <= len) {
        data = std::move(resp);
    } else {
        // 服务器返回了全量（不支持 Range）或超出区间，截取
        if (offset >= resp.size()) { data.clear(); return true; }
        size_t avail = resp.size() - offset;
        data = resp.substr(offset, avail < len ? avail : len);
    }
    return true;
}

bool WebDavClient::upload(const std::string& path, const std::string& data, std::string& err) {
    std::string resp;
    int code = http_->request("PUT", fullUrl(path),
        { "Content-Type: application/octet-stream" }, data, resp, err);
    if (code >= 200 && code < 300) return true;
    if (err.empty()) err = "上传失败 HTTP " + std::to_string(code);
    return false;
}

bool WebDavClient::mkdir(const std::string& path, std::string& err) {
    std::string resp;
    int code = http_->request("MKCOL", fullUrl(path), {}, "", resp, err);
    if (code >= 200 && code < 300) return true;
    if (code == 405) return true; // 已存在
    if (err.empty()) err = "MKCOL HTTP " + std::to_string(code);
    return false;
}

bool WebDavClient::remove(const std::string& path, std::string& err) {
    std::string resp;
    int code = http_->request("DELETE", fullUrl(path), {}, "", resp, err);
    if (code >= 200 && code < 300) return true;
    if (err.empty()) err = "DELETE HTTP " + std::to_string(code);
    return false;
}

bool WebDavClient::move(const std::string& from, const std::string& to, std::string& err) {
    std::string resp;
    int code = http_->request("MOVE", fullUrl(from),
        { "Destination: " + fullUrl(to), "Overwrite: T" }, "", resp, err);
    if (code >= 200 && code < 300) return true;
    if (err.empty()) err = "MOVE HTTP " + std::to_string(code);
    return false;
}

bool WebDavClient::copy(const std::string& from, const std::string& to, std::string& err) {
    std::string resp;
    int code = http_->request("COPY", fullUrl(from),
        { "Destination: " + fullUrl(to), "Overwrite: T" }, "", resp, err);
    if (code >= 200 && code < 300) return true;
    if (err.empty()) err = "COPY HTTP " + std::to_string(code);
    return false;
}

// ---------- XML ----------

std::vector<std::string> XmlExtract::findAll(const std::string& xml, const std::string& tag) {
    std::vector<std::string> out;
    std::string open = "<" + tag;
    size_t pos = 0;
    while (true) {
        size_t s = xml.find(open, pos);
        if (s == std::string::npos) break;
        // 确保是完整标签名（下一个字符是 > 或空格或 /）
        size_t after = s + open.size();
        if (after < xml.size() && xml[after] != '>' && xml[after] != ' ' && xml[after] != '/') {
            pos = after; continue;
        }
        size_t contentStart = xml.find('>', s);
        if (contentStart == std::string::npos) break;
        ++contentStart;
        // 自闭合？
        if (contentStart >= 2 && xml[contentStart - 2] == '/') { pos = contentStart; continue; }
        std::string close = "</" + tag + ">";
        size_t e = xml.find(close, contentStart);
        if (e == std::string::npos) break;
        out.push_back(xml.substr(contentStart, e - contentStart));
        pos = e + close.size();
    }
    return out;
}

std::string XmlExtract::first(const std::string& xml, const std::string& tag) {
    auto v = findAll(xml, tag);
    return v.empty() ? "" : v[0];
}

std::string XmlExtract::decodeEntities(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            if (s.compare(i, 5, "&amp;") == 0) { o += '&'; i += 4; }
            else if (s.compare(i, 4, "&lt;") == 0) { o += '<'; i += 3; }
            else if (s.compare(i, 4, "&gt;") == 0) { o += '>'; i += 3; }
            else if (s.compare(i, 6, "&quot;") == 0) { o += '"'; i += 5; }
            else if (s.compare(i, 6, "&apos;") == 0) { o += '\''; i += 5; }
            else o += s[i];
        } else o += s[i];
    }
    return o;
}

} // namespace wddisk
