cmake_minimum_required(VERSION 3.20)

# 版本号**单一事实源**（2026-09-19 审核报告）。
# 改版本只改这里；`.rc` 通过下面生成的头读取，不再手写。
project(FufuGraphicsPlugin VERSION 2.2.0.0 LANGUAGES CXX RC)

# 生成版本头供 .rc 使用（生成到构建目录，避免污染源码树）。
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/Version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/Version.h"
    @ONLY)

add_library(FufuGraphicsPlugin SHARED
    FufuGraphicsPlugin.cpp
    FufuGraphicsPlugin.rc
)

target_compile_features(FufuGraphicsPlugin PRIVATE cxx_std_20)
# .rc 里 `#include "Version.h"` 需要能找到生成的头（RC 编译器也吃 include 目录）。
target_include_directories(FufuGraphicsPlugin PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
# 生成头变化时重编 .rc，否则改版本号不会反映到产物。
set_source_files_properties(FufuGraphicsPlugin.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/Version.h")
target_compile_definitions(FufuGraphicsPlugin PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_compile_options(FufuGraphicsPlugin PRIVATE /utf-8)
# version：日志用 GetFileVersionInfoW/VerQueryValueW 读**自身 .rc 的版本资源**
# （2026-09-19 审核报告：原硬编码 version=2.0.0，而 rc/config.ini/Lua 均为 2.2.0）。
# 加它使 `.rc` 成为版本号的唯一事实源。
target_link_libraries(FufuGraphicsPlugin PRIVATE dxgi version)
set_property(TARGET FufuGraphicsPlugin PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_target_properties(FufuGraphicsPlugin PROPERTIES OUTPUT_NAME "FSR-Bridge-Plugin")
