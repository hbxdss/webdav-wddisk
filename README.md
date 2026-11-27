# Wddisk —— WebDAV 挂载为 Windows 本地磁盘

把 WebDAV 云盘（123pan、阿里云盘、坚果云、Nextcloud、群晖等）一键挂载为
Windows 真实本地盘符（如 `Z:`），通过 WinFsp 用户态文件系统实现，
不依赖浏览器，不占虚拟机资源。

## 功能
- 多 WebDAV 服务器配置管理（添加 / 编辑 / 删除）
- 一键挂载 / 一键卸载，自动挑选可用盘符
- 文件管理器里直接双击打开，就像本地硬盘一样用
- 跳过 TLS 验证（自签名证书、内网服务）
- 账号配置存在安装目录（便携式，重装/换机器直接搬过去）
- 密码用 DPAPI 按当前用户加密
- 系统托盘、开机自启、后台静默运行

## 原理
```
  Wddisk GUI  ── WinHTTP ──►  WebDAV 服务器
        │
        ▼
  WinFsp (FUSE for Windows)
        │
        ▼
  本地盘符 Z:  ← 资源管理器 / 任何应用直接读写
```
打开文件时整体下载到本地缓存（哈希命名），读写直接走缓存；
关闭 / Flush 时若有修改自动上传回服务器。目录列表走实时 PROPFIND。

> 说明：整体缓存策略对常规文档、图片、代码等体验很好；GB 级超大文件
> 首次打开会等待完整下载。

## 下载 / 安装
去 [GitHub Releases](https://github.com/hbxdss/webdav-wddisk/releases)
或 [Gitee Releases](https://gitee.com/hbxdss/webdav-wddisk/releases) 拿：

| 文件 | 说明 |
|------|------|
| `wddisk-setup-x64.exe` | 安装包（内嵌 WinFsp 驱动，自动安装） |
| `wddisk-windows-x64-portable.zip` | 便携绿色版（需自行先装 WinFsp） |
| `wddisk-source.zip` | 源码压缩包 |

首次启动如果 WinFsp 没装，会提示下载：https://winfsp.dev/rel/

## 源码构建（Windows）
依赖：WinFsp SDK + MinGW-w64 / LLVM-MinGW

```bash
# 1. 安装 WinFsp（https://winfsp.dev/rel/），选 Developer files，记下 include/lib 路径

# 2. 编译源码
g++ -DUNICODE -D_UNICODE -std=c++17 -O2 -ffunction-sections -fdata-sections \
    -Icommon -Iwindows -I<WinFsp 头文件路径> \
    -c windows/app.cpp common/webdav.cpp windows/webdav_fs.cpp windows/winhttp_client.cpp \
       windows/app.rc \
    -o build/sz-o2s/

# 3. 链接
g++ -static -mwindows -municode -Wl,--gc-sections -s \
    -Lbuild/win -o build/win/wddisk.exe \
    build/sz-o2s/*.o \
    -lwinfsp-x64 -lwinhttp -lcrypt32 -ladvapi32 -lole32 -lshell32 \
    -luuid -lgdi32 -lmsimg32 -ldwmapi

# 4. 制作安装包：dist/wddisk.iss（用 Inno Setup 6 编译，内嵌 WinFsp MSI）
```

## 目录结构
```
common/       WebDAV 协议层（PROPFIND/GET Range/PUT/MOVE/DELETE）、JSON 配置
windows/      WinFsp 后端 + Win32 自绘深色 UI（核心） + Inno Setup 脚本
tests/        PowerShell 回归脚本 + 诊断小工具（非必需）
dist/         安装包脚本（wddisk.iss）
```

## 配置存储
优先存在 `wddisk.exe` 同目录（可移植），装到只读目录时
回退到 `%LOCALAPPDATA%\Wddisk\config.json`。密码用 DPAPI 加密后写入。

## 开源协议
[MIT License](LICENSE)
