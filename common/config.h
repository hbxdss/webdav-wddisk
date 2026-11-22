// config.h - 挂载配置存取（JSON 存储，存储路径由平台注入）
#pragma once
#include <string>
#include <vector>
#include <functional>
#include "json.h"
#include "webdav.h"

namespace wddisk {

class ConfigStore {
public:
    // filePath: 平台传入的配置文件完整路径
    // wPath: Windows 下的宽字符路径（可选）。当路径含中文等非 ANSI 字符时必须提供，
    //        否则 fopen 会按 ANSI 解释 UTF-8 字节而导致打开失败。
    explicit ConfigStore(const std::string& filePath, const std::wstring& wPath = std::wstring())
        : filePath_(filePath), wPath_(wPath) { load(); }

    std::vector<DavConfig>& servers() { return servers_; }

    // 密码加密钩子：平台注入（Windows=DPAPI，其他平台可留空=明文）
    // enc: 明文 -> 密文；dec: 密文 -> 明文（返回 false 视为解密失败，按原值使用）
    using EncFn = std::function<std::string(const std::string&)>;
    using DecFn = std::function<bool(const std::string&, std::string&)>;
    void setCrypto(EncFn enc, DecFn dec) { enc_ = std::move(enc); dec_ = std::move(dec); reload(); }
    // 重新读取配置（用于 setCrypto 之后解密已有密文密码）
    void reload() { decryptFailed_ = false; load(); }
    // 是否有部分密码解密失败（通常意味着换机器/换用户，需要重新输入）
    bool decryptFailed() const { return decryptFailed_; }

    bool save() {
        std::string arr = "[";
        for (size_t i = 0; i < servers_.size(); ++i) {
            auto& s = servers_[i];
            if (i) arr += ",";
            arr += "{";
            arr += "\"name\":" + Json::escape(s.name);
            arr += ",\"url\":" + Json::escape(s.url);
            arr += ",\"username\":" + Json::escape(s.username);
            // 密码：DPAPI 密文以 dpapi: 前缀存储；无加密钩子时按明文存
            std::string pw = s.password;
            if (enc_ && !pw.empty()) pw = "dpapi:" + enc_(pw);
            arr += ",\"password\":" + Json::escape(pw);
            arr += ",\"autoMount\":" + std::string(s.autoMount ? "true" : "false");
            arr += ",\"verifyTls\":" + std::string(s.verifyTls ? "true" : "false");
            arr += ",\"mountPoint\":" + Json::escape(s.mountPoint);
            arr += "}";
        }
        arr += "]";
        // 简单包装：整体作为一个转义字符串存 root.servers
        Json root;
        root.set("servers", arr);  // 存的是 JSON 字符串（二次解析）
        FILE* f = openFile("wb");
        if (!f) return false;
        std::string data = root.dump();
        fwrite(data.data(), 1, data.size(), f);
        fclose(f);
        return true;
    }

private:
    FILE* openFile(const char* mode) {
#ifdef _WIN32
        if (!wPath_.empty()) {
            // 注意：读取必须用 rb，否则会把配置文件截断为 0 字节
            const wchar_t* wmode = (mode && mode[0] == 'r') ? L"rb" : L"wb";
            return _wfopen(wPath_.c_str(), wmode);
        }
#endif
        return fopen(filePath_.c_str(), mode);
    }

    void load() {
        FILE* f = openFile("rb");
        if (!f) return;
        std::string data;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
        fclose(f);

        // 兼容两种格式：
        // 1) save() 写出的二次编码格式: {"servers":"[{...},...]"}
        // 2) 标准 JSON 数组格式: {"servers":[{...},...]}
        servers_.clear();
        std::string s;
        {
            size_t k = data.find("\"servers\"");
            if (k == std::string::npos) return;
            k = data.find(':', k);
            if (k == std::string::npos) return;
            ++k;
            while (k < data.size() && (data[k] == ' ' || data[k] == '\t' || data[k] == '\n' || data[k] == '\r')) ++k;
            if (k >= data.size()) return;
            if (data[k] == '"') {
                Json root;
                if (!Json::parse(data, root)) return;
                s = root.getStr("servers", "[]");
            } else if (data[k] == '[') {
                s = data.substr(k);
                size_t e = s.rfind(']');
                if (e != std::string::npos) s = s.substr(0, e + 1);
            } else {
                return;
            }
        }
        // 手动按对象拆分
        size_t pos = 0;
        while (true) {
            size_t b = s.find('{', pos);
            if (b == std::string::npos) break;
            size_t e = s.find('}', b);
            if (e == std::string::npos) break;
            Json one;
            if (Json::parse(s.substr(b, e - b + 1), one)) {
                DavConfig c;
                c.name = one.getStr("name");
                c.url = one.getStr("url");
                c.username = one.getStr("username");
                c.password = one.getStr("password");
                c.verifyTls = one.getBool("verifyTls", true);
                c.autoMount = one.getBool("autoMount", false);
                c.mountPoint = one.getStr("mountPoint");
                // 密文解密；旧版本明文配置直接沿用
                if (c.password.compare(0, 6, "dpapi:") == 0) {
                    std::string plain;
                    if (dec_ && dec_(c.password.substr(6), plain)) c.password = plain;
                    else {
                        c.password.clear();
                        decryptFailed_ = true;
                    }
                }
                servers_.push_back(c);
            }
            pos = e + 1;
        }
    }

    std::string filePath_;
    std::wstring wPath_;
    std::vector<DavConfig> servers_;
    EncFn enc_;
    DecFn dec_;
    bool decryptFailed_ = false;
};

} // namespace wddisk
