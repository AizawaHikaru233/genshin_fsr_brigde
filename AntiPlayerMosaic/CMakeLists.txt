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
# Psapi 函数（EnumProcessModules 等）自 Windows 7 起位于 kernel32（Psapi.lib 仅为
# 兼容转发库，VS18/新 SDK 已移除该 .lib）——直接链 kernel32。
target_link_libraries(AntiPlayerMosaic PRIVATE kernel32)
