cmake_minimum_required(VERSION 3.20)

project(FufuGraphicsPlugin LANGUAGES CXX RC)

add_library(FufuGraphicsPlugin SHARED
    FufuGraphicsPlugin.cpp
    FufuGraphicsPlugin.rc
)

target_compile_features(FufuGraphicsPlugin PRIVATE cxx_std_20)
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
