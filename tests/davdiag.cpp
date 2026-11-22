// davdiag.cpp - 直接测试 WebDavClient.stat
#include "../common/webdav.h"
#include "../windows/winhttp_client.h"
#include <windows.h>
#include <cstdio>
using namespace wddisk;

int wmain(int argc, wchar_t** argv) {
    DavConfig cfg;
    if (argc > 1) {
        int n = WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, nullptr, 0, nullptr, nullptr);
        std::string u(n - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, &u[0], n, nullptr, nullptr);
        cfg.url = u;
    } else {
        cfg.url = "http://127.0.0.1:8080";
    }
    if (argc > 3) {
        int n = WideCharToMultiByte(CP_UTF8, 0, argv[3], -1, nullptr, 0, nullptr, nullptr);
        std::string s(n - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, argv[3], -1, &s[0], n, nullptr, nullptr);
        cfg.username = s;
    }
    if (argc > 4) {
        int n = WideCharToMultiByte(CP_UTF8, 0, argv[4], -1, nullptr, 0, nullptr, nullptr);
        std::string s(n - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, argv[4], -1, &s[0], n, nullptr, nullptr);
        cfg.password = s;
    }
    WinHttpClient http(cfg);
    WebDavClient dav(cfg, &http);

    // 原始请求诊断
    std::string resp, err2;
    int code = http.request("PROPFIND", cfg.url + "/subdir/note.txt",
        { "Depth: 0", "Content-Type: application/xml; charset=utf-8" },
        "<?xml version=\"1.0\" encoding=\"utf-8\"?><d:propfind xmlns:d=\"DAV:\"><d:prop>"
        "<d:resourcetype/><d:getcontentlength/><d:getlastmodified/></d:prop></d:propfind>",
        resp, err2);
    printf("RAW PROPFIND note.txt => %d err=%s resp[0..120]=%.120s\n", code, err2.c_str(), resp.c_str());
    code = http.request("PROPFIND", cfg.url + "/subdir",
        { "Depth: 0", "Content-Type: application/xml; charset=utf-8" },
        "<?xml version=\"1.0\" encoding=\"utf-8\"?><d:propfind xmlns:d=\"DAV:\"><d:prop>"
        "<d:resourcetype/></d:prop></d:propfind>", resp, err2);
    printf("RAW PROPFIND subdir => %d err=%s resp[0..200]=%.200s\n", code, err2.c_str(), resp.c_str());


    WebDavEntry e;
    std::string err;
    bool ok = dav.stat("/subdir/note.txt", e, err);
    printf("stat(/subdir/note.txt) => %d err=%s size=%llu isDir=%d\n", ok, err.c_str(), e.size, e.isDir);

    std::vector<WebDavEntry> list;
    err.clear();
    ok = dav.listDir("/", list, err);
    printf("listDir(/) => %d err=%s count=%d\n", ok, err.c_str(), (int)list.size());
    for (auto& x : list) printf("  [%s] %s size=%llu\n", x.isDir ? "D" : "F", x.name.c_str(), x.size);

    err.clear();
    ok = dav.stat("/subdir", e, err);
    printf("stat(/subdir) => %d err=%s isDir=%d\n", ok, err.c_str(), e.isDir);

    err.clear();
    std::string data;
    ok = dav.download("/hello.txt", data, err);
    printf("download(/hello.txt) => %d err=%s len=%d\n", ok, err.c_str(), (int)data.size());
    return 0;
}
