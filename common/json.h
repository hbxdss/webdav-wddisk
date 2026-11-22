// json.h - 极简 JSON 读写（够用即可，不支持嵌套数组对象之外的怪异语法）
#pragma once
#include <string>
#include <map>
#include <vector>
#include <sstream>

namespace wddisk {

// 仅支持 string / bool / int64，顶层为对象
class Json {
public:
    std::map<std::string, std::string> strs;
    std::map<std::string, long long> nums;
    std::map<std::string, bool> bools;

    void set(const std::string& k, const std::string& v) { strs[k] = v; }
    void set(const std::string& k, long long v) { nums[k] = v; }
    void set(const std::string& k, bool v) { bools[k] = v; }

    std::string getStr(const std::string& k, const std::string& def = "") const {
        auto it = strs.find(k); return it == strs.end() ? def : it->second;
    }
    long long getInt(const std::string& k, long long def = 0) const {
        auto it = nums.find(k); return it == nums.end() ? def : it->second;
    }
    bool getBool(const std::string& k, bool def = false) const {
        auto it = bools.find(k); return it == bools.end() ? def : it->second;
    }

    std::string dump() const {
        std::ostringstream o;
        o << "{";
        bool first = true;
        auto emit = [&](const std::string& k, const std::string& encodedVal) {
            if (!first) o << ",";
            first = false;
            o << escape(k) << ":" << encodedVal;
        };
        for (auto& kv : strs) emit(kv.first, escape(kv.second));
        for (auto& kv : nums) emit(kv.first, std::to_string(kv.second));
        for (auto& kv : bools) emit(kv.first, kv.second ? "true" : "false");
        o << "}";
        return o.str();
    }

    static std::string escape(const std::string& s) {
        std::string o = "\"";
        for (unsigned char c : s) {
            switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
                else o += (char)c;
            }
        }
        o += "\"";
        return o;
    }

    static bool parse(const std::string& text, Json& out) {
        size_t i = 0;
        skipWs(text, i);
        if (i >= text.size() || text[i] != '{') return false;
        ++i;
        while (true) {
            skipWs(text, i);
            if (i < text.size() && text[i] == '}') return true;
            if (i >= text.size() || text[i] != '"') return false;
            std::string key;
            if (!parseString(text, i, key)) return false;
            skipWs(text, i);
            if (i >= text.size() || text[i] != ':') return false;
            ++i;
            skipWs(text, i);
            if (i < text.size() && text[i] == '"') {
                std::string v;
                if (!parseString(text, i, v)) return false;
                out.strs[key] = v;
            } else if (text.compare(i, 4, "true") == 0) {
                out.bools[key] = true; i += 4;
            } else if (text.compare(i, 5, "false") == 0) {
                out.bools[key] = false; i += 5;
            } else {
                size_t start = i;
                while (i < text.size() && (isdigit((unsigned char)text[i]) || text[i] == '-' || text[i] == '+' || text[i] == '.' || text[i] == 'e' || text[i] == 'E')) ++i;
                if (i == start) return false;
                out.nums[key] = strtoll(text.substr(start, i - start).c_str(), nullptr, 10);
            }
            skipWs(text, i);
            if (i < text.size() && text[i] == ',') { ++i; continue; }
            if (i < text.size() && text[i] == '}') return true;
            return false;
        }
    }

private:
    static void skipWs(const std::string& t, size_t& i) {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) ++i;
    }
    static bool parseString(const std::string& t, size_t& i, std::string& out) {
        if (i >= t.size() || t[i] != '"') return false;
        ++i;
        while (i < t.size()) {
            char c = t[i++];
            if (c == '"') return true;
            if (c == '\\') {
                if (i >= t.size()) return false;
                char e = t[i++];
                switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    if (i + 4 > t.size()) return false;
                    unsigned code = (unsigned)strtoul(t.substr(i, 4).c_str(), nullptr, 16);
                    i += 4;
                    // 简化处理：BMP 直接转 UTF-8，代理对按原样拼接
                    if (code < 0x80) out += (char)code;
                    else if (code < 0x800) { out += (char)(0xC0 | (code >> 6)); out += (char)(0x80 | (code & 0x3F)); }
                    else { out += (char)(0xE0 | (code >> 12)); out += (char)(0x80 | ((code >> 6) & 0x3F)); out += (char)(0x80 | (code & 0x3F)); }
                    break;
                }
                default: return false;
                }
            } else out += c;
        }
        return false;
    }
};

} // namespace wddisk
