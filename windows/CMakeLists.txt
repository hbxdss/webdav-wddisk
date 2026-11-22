# Wddisk Windows 端构建
# 依赖：WinFsp（https://winfsp.dev/，安装时勾选 Developer files，默认位于 C:\Program Files (x86)\WinFsp）
cmake_minimum_required(VERSION 3.15)
project(wddisk-win CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WINFSP_ROOT)
    set(WINFSP_ROOT "C:/Program Files (x86)/WinFsp")
endif()

find_path(WINFSP_INC_DIR winfsp.h HINTS "${WINFSP_ROOT}/inc")
if(NOT WINSP_INC_DIR AND EXISTS "${WINFSP_ROOT}/inc")
    set(WINFSP_INC_DIR "${WINFSP_ROOT}/inc")
endif()
find_library(WINFSP_LIB winfsp-x64 HINTS "${WINFSP_ROOT}/lib")

add_executable(wddisk
    ../common/webdav.cpp
    winhttp_client.cpp
    webdav_fs.cpp
    app.cpp
)

target_include_directories(wddisk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${WINFSP_INC_DIR}
)

target_link_libraries(wddisk PRIVATE
    ${WINFSP_LIB}
    winhttp comctl32 shell32 ole32 uuid advapi32 shlwapi user32 gdi32
)

if(MSVC)
    target_compile_options(wddisk PRIVATE /utf-8 /W3)
else()
    # MinGW
    target_compile_options(wddisk PRIVATE -Wall -Wno-unknown-pragmas -Wno-attributes -Wno-write-strings)
    target_link_options(wddisk PRIVATE -municode -mwindows -static-libgcc -static-libstdc++)
endif()
