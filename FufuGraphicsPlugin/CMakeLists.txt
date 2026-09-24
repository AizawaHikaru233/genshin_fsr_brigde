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

# ---------------------------------------------------------------------------
# 单元测试（默认关）：`FufuReshadeConfigTest`
#
# 验证 `prepare_reshade_game_configuration` 的路径写入策略 ——
# 该函数**每次启动游戏都会执行**，若无条件覆写会把用户在 ReShade 界面里改过的
# 设置（自定义截图目录等）**每次启动都重置**（2026-09-23 修正）。
#
# 为什么测试要 `#include` 被测 .cpp：其实现全在匿名命名空间内（内部链接），
# 外部无法链接；同一 TU 内则可见。exe 所在目录即"游戏目录"
# （`module_path(nullptr)` 取自身路径），故测试在自身目录造布局。
# ---------------------------------------------------------------------------
option(FUFUGRAPHICS_BUILD_TESTS "Build the Fufu bootstrap unit tests" OFF)
if(FUFUGRAPHICS_BUILD_TESTS)
    add_executable(FufuReshadeConfigTest FufuReshadeConfigTest.cpp)
    target_compile_features(FufuReshadeConfigTest PRIVATE cxx_std_20)
    target_compile_definitions(FufuReshadeConfigTest PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(FufuReshadeConfigTest PRIVATE /utf-8 /W3)
    target_link_libraries(FufuReshadeConfigTest PRIVATE dxgi version)
    # 测试从 exe 同目录读模板
    add_custom_command(TARGET FufuReshadeConfigTest POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../SharedResources/ReShade/runtime/ReShade.ini"
            "$<TARGET_FILE_DIR:FufuReshadeConfigTest>/ReShade.template.ini")
    enable_testing()
    add_test(NAME FufuReshadeConfigTest COMMAND FufuReshadeConfigTest)

    # 组件路径解析顺序：验证旧 Lite 包布局 `..\..\FSRGraphicsPayload\...`
    # **排在候选末位** —— 既不让残留的过期目录遮蔽当前路径，又保留对旧版用户的回退。
    add_executable(FufuPathOrderTest FufuPathOrderTest.cpp)
    target_compile_features(FufuPathOrderTest PRIVATE cxx_std_20)
    target_compile_definitions(FufuPathOrderTest PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(FufuPathOrderTest PRIVATE /utf-8 /W3)
    target_link_libraries(FufuPathOrderTest PRIVATE dxgi version)
    add_test(NAME FufuPathOrderTest COMMAND FufuPathOrderTest)
endif()
