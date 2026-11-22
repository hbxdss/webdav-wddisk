// cfgdiag.cpp - 配置加载诊断
#include "../common/config.h"
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>
using namespace wddisk;
int wmain(int argc, wchar_t** argv) {
    std::wstring wp = argc > 1 ? argv[1] : L"";
    int n = WideCharToMultiByte(CP_UTF8, 0, wp.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string path(n - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wp.c_str(), -1, &path[0], n, nullptr, nullptr);
    ConfigStore store(path);
    printf("servers=%d\n", (int)store.servers().size());
    for (auto& s : store.servers())
        printf("  name=%s url=%s mount=%s\n", s.name.c_str(), s.url.c_str(), s.mountPoint.c_str());
    return 0;
}
