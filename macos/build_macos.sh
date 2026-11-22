#!/bin/bash
# Wddisk macOS 构建脚本
# 依赖：brew install --cask macfuse && brew install curl
set -e
cd "$(dirname "$0")/.."

CXX=${CXX:-clang++}
FLAGS="-std=c++17 -O2 -Wall -Wno-unknown-pragmas \
  -Icommon -Imacos \
  -framework Cocoa \
  $(pkg-config --cflags --libs libcurl 2>/dev/null || echo -lcurl) \
  $(pkg-config --cflags --libs osxfuse 2>/dev/null || echo '-I/usr/local/include/osxfuse -L/usr/local/lib -lfuse -DFUSE_USE_VERSION=26')"

mkdir -p build/macos
$CXX -x objective-c++ common/webdav.cpp macos/curl_client.cpp macos/webdav_fuse.cpp macos/gui.mm \
    -o build/macos/wddisk $FLAGS

echo "构建完成: build/macos/wddisk"
echo "运行前请确认已安装 macFUSE（系统设置中允许加载内核扩展）"
