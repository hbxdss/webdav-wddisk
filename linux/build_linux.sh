#!/bin/bash
# Wddisk Linux 构建脚本
# 依赖（Debian/Ubuntu）: sudo apt install g++ libfuse3-dev libcurl4-openssl-dev libgtk-3-dev
# 依赖（Fedora）: sudo dnf install gcc-c++ fuse3-devel libcurl-devel gtk3-devel
set -e
cd "$(dirname "$0")/.."

CXX=${CXX:-g++}
FLAGS="-std=c++17 -O2 -Wall -Wno-unknown-pragmas \
  -Icommon -Ilinux \
  $(pkg-config --cflags --libs fuse3 libcurl gtk+-3.0 2>/dev/null || \
    { pkg-config --cflags --libs fuse libcurl gtk+-3.0; })"

mkdir -p build/linux
$CXX common/webdav.cpp common/curl_client.cpp linux/webdav_fuse3.cpp linux/gui.cpp \
    -o build/linux/wddisk $FLAGS -lpthread

echo "构建完成: build/linux/wddisk"
