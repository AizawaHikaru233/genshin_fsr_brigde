cmake_minimum_required(VERSION 3.20)

project(AntiPlayerMosaic LANGUAGES CXX RC)

add_library(AntiPlayerMosaic SHARED AntiPlayerMosaic.cpp AntiPlayerMosaic.rc)
target_compile_features(AntiPlayerMosaic PRIVATE cxx_std_20)
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
