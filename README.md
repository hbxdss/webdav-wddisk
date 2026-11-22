# Wddisk —— WebDAV 网络磁盘管理器

用 C++ 编写的跨平台 WebDAV 挂载工具，每个平台一套独立的挂载后端与原生 GUI（非浏览器管理）。

## 功能
- 多 WebDAV 服务器配置管理（添加/编辑/删除）
- 一键挂载为本地磁盘 / 一键卸载
- 自动选择可用挂载点（Windows 自动选盘符；macOS/Linux 挂载到 `~/Wddisk/<名称>`）
- 文件管理器中直接打开
- 跳过 TLS 验证选项（自签名证书）
- 状态实时显示

## 挂载原理
- **Windows**：WinFsp 用户态文件系统 → 挂载为真实盘符（如 `Z:`）
- **macOS**：macFUSE (osxfuse) → 挂载到 `~/Wddisk/<名称>`
- **Linux**：libfuse3 → 挂载到 `~/Wddisk/<名称>`

文件打开时整体下载到本地缓存（哈希命名），读写直接走缓存；关闭/Flush 时若修改自动上传。目录列表实时 PROPFIND。

> 限制：单文件整体缓存策略适合常规文档、图片、代码等；超大文件（GB 级）首次打开会等待完整下载。

## 各平台构建

### Windows（需要 WinFsp + 编译工具）
1. 安装 WinFsp：https://winfsp.dev/rel/ （安装时勾选 "Developer files"）
2. MSVC：`cmake -S windows -B build/win && cmake --build build/win --config Release`
3. MinGW（gcc）：
   ```
   g++ -std=c++17 -municode -c common/webdav.cpp windows/winhttp_client.cpp \
       windows/webdav_fs.cpp windows/app.cpp -Icommon -Iwindows -I"<WinFsp inc>" 
   g++ *.o -o wddisk.exe <WinFsp lib>\winfsp-x64.lib -lwinhttp -lcomctl32 -lshell32 \
       -lole32 -ladvapi32 -lshlwapi -luser32 -lgdi32 -luuid -mwindows -static-libgcc -static-libstdc++
   ```
   注意：MinGW 头文件下需删除 WinFsp `winfsp/fsctl.h` 中两处 `void _ReadWriteBarrier(void);`
   声明（MSVC 无此问题）；另外本项目不使用 WinHttpCrackUrl（MinGW 下 path 输出异常）。
4. 制作安装包：`dist\wddisk.iss`（Inno Setup 7 编译，内嵌 WinFsp 驱动自动安装）

### macOS（需要 macFUSE）
```bash
brew install --cask macfuse && brew install curl
chmod +x macos/build_macos.sh && ./macos/build_macos.sh
./build/macos/wddisk
```
首次运行需在 系统设置 → 隐私与安全性 中允许加载 macFUSE 扩展。

### Linux（需要 libfuse3）
```bash
# Debian/Ubuntu
sudo apt install g++ libfuse3-dev libcurl4-openssl-dev libgtk-3-dev
# Fedora
sudo dnf install gcc-c++ fuse3-devel libcurl-devel gtk3-devel

chmod +x linux/build_linux.sh && ./linux/build_linux.sh
./build/linux/wddisk
```

## 目录结构
```
common/           共享核心：WebDAV 客户端协议层、libcurl HTTP、JSON 配置存储
windows/          WinFsp 后端 + Win32 深色原生 GUI + CMake（含命令行挂载模式 --mount N）
macos/            macFUSE 后端 + AppKit 原生 GUI (Objective-C++) + 构建脚本
linux/            libfuse3 后端 + GTK3 原生 GUI + 构建脚本
tests/            testdav.py（最小 WebDAV 测试服务器）与诊断小工具
dist/             安装包脚本（Inno Setup）与构建产物
```

## Windows 端实测结果
使用 `tests/testdav.py` 本地 WebDAV 服务器 + WinFsp 完整回归测试，全部通过：
创建/写入文件、创建目录、子目录写入、根目录与子目录枚举、读取回显、重命名（含大小写不敏感回退）、删除文件、删除目录，服务器端状态一致。

## 配置存储
- Windows: `wddisk.exe` 所在目录的 `config.json`（便携式）；若安装目录不可写则回退 `%LOCALAPPDATA%\Wddisk\config.json`
- macOS:   `~/.wddisk/config.json`
- Linux:   `~/.wddisk/config.json`

Windows 端密码使用 DPAPI 按当前用户加密（`dpapi:` 前缀）后写入配置；安装版会给安装目录授予 `users-modify` 权限，账号配置因此可以直接放在安装目录。

## 开源协议

本项目基于 [MIT License](LICENSE) 开源，可自由使用、修改、分发与商用，请保留版权声明。
