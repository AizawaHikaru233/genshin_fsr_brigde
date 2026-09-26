cmake_minimum_required(VERSION 3.20)

# 版本号**单一事实源**（2026-09-19 审核报告）。
# 改版本只改这里；`.rc` 通过下面生成的头读取，不再手写。
project(AntiPlayerMosaic VERSION 1.1.0.0 LANGUAGES CXX RC)

# 生成版本头供 .rc 使用（生成到构建目录，避免污染源码树）。
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/Version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/Version.h"
    @ONLY)

add_library(AntiPlayerMosaic SHARED AntiPlayerMosaic.cpp AntiPlayerMosaic.rc)
target_compile_features(AntiPlayerMosaic PRIVATE cxx_std_20)
# .rc 里 `#include "Version.h"` 需要能找到生成的头（RC 编译器也吃 include 目录）。
target_include_directories(AntiPlayerMosaic PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
# 生成头变化时重编 .rc，否则改版本号不会反映到产物。
set_source_files_properties(AntiPlayerMosaic.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/Version.h")
if(MSVC)
    target_compile_options(AntiPlayerMosaic PRIVATE /utf-8)
endif()
target_compile_definitions(AntiPlayerMosaic PRIVATE
    $<$<CONFIG:Release>:ANTIPLAYER_RELEASE_RUNTIME=1>
)
# Psapi 函数自 Windows 7 起位于 kernel32（Psapi.lib 仅为兼容转发库，VS18/新 SDK 已移除
# 该 .lib）—— 直接链 kernel32。
#
# ⚠️ 2026-09-19（审核报告）修正注释：原文写 "EnumProcessModules 等"，但源码实际**只用**
# `K32GetModuleInformation`（见 AntiPlayerMosaic.cpp 中 `K32GetModuleInformation(
# GetCurrentProcess(), main_module, &module_info, ...)`）；本文件并未调用
# `EnumProcessModules` / `K32EnumProcessModules`。
target_link_libraries(AntiPlayerMosaic PRIVATE kernel32)

# 模式解析回归测试（2026-09-19）。
#
# `parse_pattern` 现在会在**输入被截断**时置 `valid=false`（原先静默 break）。
# 该测试同时覆盖：5 个真实签名必须完整解析、两类坏输入必须被拒绝、
# 合法输入不得误判。默认 OFF（不影响正常构建）。
option(ANTIPLAYER_BUILD_PATTERN_TEST "Build the pattern-parse regression test" OFF)
if(ANTIPLAYER_BUILD_PATTERN_TEST)
    add_executable(PatternParseTest PatternParseTest.cpp)
    target_compile_features(PatternParseTest PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(PatternParseTest PRIVATE /utf-8 /W3)
    endif()
    target_include_directories(PatternParseTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    enable_testing()
    add_test(NAME PatternParseTest COMMAND PatternParseTest)
endif()

# `SetActive` 动态解析回归测试。
#
# 覆盖：国服（YuanShen.exe）与国际服（GenshinImpact.exe）两份客户端都能唯一解出
# 原生实现及**尾调用它**的包装器；判据被破坏时必须失败（fail-closed，不许猜函数）。
# 该测试直接编入产品源码（测的是真实现，不是复刻）；二进制不在本机时用例记 SKIP。
# 默认 OFF（不影响正常构建）。
option(ANTIPLAYER_BUILD_RESOLVE_TEST "Build the SetActive dynamic-resolve test" OFF)
if(ANTIPLAYER_BUILD_RESOLVE_TEST)
    add_executable(ObjectActiveResolveTest ObjectActiveResolveTest.cpp)
    target_compile_features(ObjectActiveResolveTest PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(ObjectActiveResolveTest PRIVATE /utf-8 /W3)
    endif()
    target_include_directories(ObjectActiveResolveTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(ObjectActiveResolveTest PRIVATE kernel32)
    enable_testing()
    add_test(NAME ObjectActiveResolveTest COMMAND ObjectActiveResolveTest)
endif()
