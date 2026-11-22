// fsdiag.cpp - 诊断挂载失败原因的控制台小工具
#include "webdav_fs.h"
#include <cstdio>
#include <string>

using namespace wddisk;

int wmain(int argc, wchar_t** argv) {
    DavConfig cfg;
    cfg.name = "test";
    cfg.url = "http://127.0.0.1:8080";
    cfg.mountPoint = "Z:";

    // 支持命令行覆盖
    if (argc > 1) {
        int n = WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, nullptr, 0, nullptr, nullptr);
        std::string s(n - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, &s[0], n, nullptr, nullptr);
        cfg.mountPoint = s;
    }
    if (argc > 2) {
        int n = WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, nullptr, 0, nullptr, nullptr);
        std::string s(n - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, &s[0], n, nullptr, nullptr);
        cfg.url = s;
    }

    std::wstring cache = L"build\\cache";
    DavFileSystem fs(cfg, cache);
    std::string err;
    std::wstring mp;
    mp.assign(cfg.mountPoint.begin(), cfg.mountPoint.end()); // ASCII 盘符，直接转
    bool ok = fs.mount(mp, err);
    printf("mount(%s) => %s, err=%s\n", cfg.mountPoint.c_str(), ok ? "OK" : "FAIL", err.c_str());
    if (!ok) {
        printf("LastError=%lu\n", GetLastError());
        return 1;
    }
    printf("挂载成功，回车退出...\n");
    getchar();
    fs.unmount();
    return 0;
}
