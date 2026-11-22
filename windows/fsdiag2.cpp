// fsdiag2.cpp - 模拟 --mount 驻留模式（不读 stdin）
#include "webdav_fs.h"
#include <cstdio>
#include <string>
#include <thread>
#include <chrono>

using namespace wddisk;

int wmain(int argc, wchar_t** argv) {
    DavConfig cfg;
    cfg.name = "test";
    cfg.url = "http://127.0.0.1:8080";
    cfg.mountPoint = "Z:";
    std::wstring mp = L"Z:";
    if (argc > 1) mp = argv[1];

    std::wstring cache = L"build\\cache";
    DavFileSystem fs(cfg, cache);
    std::string err;
    bool ok = fs.mount(mp, err);
    printf("mount(%ls) => %s err=%s\n", mp.c_str(), ok ? "OK" : "FAIL", err.c_str());
    if (!ok) return 1;
    for (int i = 0; i < 120; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        printf("alive %d\n", i);
        fflush(stdout);
    }
    fs.unmount();
    return 0;
}
