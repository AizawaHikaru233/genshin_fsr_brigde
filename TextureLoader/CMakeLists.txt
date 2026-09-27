cmake_minimum_required(VERSION 3.20)

# 版本号**全仓库唯一事实源** = 仓库根 `../Version.cmake`（2026-09-28 用户要求：
# **所有自有组件的版本号同步迭代**，本组件本次无内容改动也照样跟号）。
# ⇒ 改版本只改 `Version.cmake` 一行；`.rc` 读生成的头，不手写。
# 历史：此前 `.rc` 手写 `0,1,0,0` / `"0.1.0"`；2026-09-19 起改为读生成的头，
#       2026-09-28 起并入统一版本线（原独立走 `0.1.0.0`）。
# 组件目录不能单独拷出构建（Detours 亦不在本目录）：事实源缺失时**显式失败**。
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../Version.cmake")
    message(FATAL_ERROR "缺少自有组件版本单一事实源：${CMAKE_CURRENT_SOURCE_DIR}/../Version.cmake（请使用完整仓库检出）")
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../Version.cmake")
project(TextureLoader VERSION ${FSR_SUITE_VERSION} LANGUAGES C CXX RC)

# 生成版本头供 .rc 使用（生成到构建目录，避免污染源码树）。
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/Version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/Version.h"
    @ONLY)

# GDDS（DirectStorage GPU 解压）依赖——SDK 已 vendor 到本目录 third_party/（MIT + MS 条款，见其 LICENSE*）
set(DSTORAGE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dstorage")

# Detours（hook 用）**不在本目录**，而是复用 Bridge 的副本（2026-09-19）。
#
# ⚠️ 这与 `build.ps1` 原先宣称的"自包含，不依赖仓库其他目录"**矛盾**：
# 单独拷出 `TextureLoader/` 目录构建会在这里失败。既然本目录内**没有** detours 副本，
# 就不能声称自包含。
#
# 为何不 vendor 一份进来：detours.lib 841 KB + detours.h 36 KB，两份副本会各自漂移
#（升级时容易只改一处）。当前选择**单一来源**（Bridge 的 third_party），
# 并在此显式校验 —— 缺失时给出可操作的报错，而不是让编译期报
# "无法打开包括文件: detours.h" 那种看不出根因的错误。
set(TEXTURELOADER_DETOURS_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../Dx11FsrBridge/third_party/detours"
    CACHE PATH "Path to the Detours headers/lib used for hooking")
if(NOT EXISTS "${TEXTURELOADER_DETOURS_ROOT}/detours.h" OR
   NOT EXISTS "${TEXTURELOADER_DETOURS_ROOT}/detours.lib")
    message(FATAL_ERROR
        "Detours not found under ${TEXTURELOADER_DETOURS_ROOT}.\n"
        "TextureLoader is NOT self-contained: it reuses the Bridge copy of Detours.\n"
        "Build from a full repository checkout, or pass "
        "-DTEXTURELOADER_DETOURS_ROOT=<path containing detours.h and detours.lib>.")
endif()

add_library(TextureLoader SHARED
    TextureLoader.cpp
    dds_loader.cpp
    gdds_interop.cpp
    texture_hash.cpp
    mod_ini.cpp
    crc32c/crc32c.cpp
    log.cpp
    TextureLoader.rc
)

target_compile_features(TextureLoader PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(TextureLoader PRIVATE /utf-8 /W3)
endif()
target_compile_definitions(TextureLoader PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    CRC32C_STATIC=1
)
# 2026-09-19：删除死定义 `TEXTURELOADER_RELEASE_RUNTIME=1`。
# 全仓库搜索确认**无任何代码引用**该宏（唯一命中就是它自己的定义行），
# 属死定义 —— 删掉后 Release 构建的 DEFINES 变化不影响任何行为。
target_include_directories(TextureLoader PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${TEXTURELOADER_DETOURS_ROOT}"
    "${DSTORAGE_ROOT}/include"
    # .rc 里 `#include "Version.h"` 需要能找到生成的头（RC 编译器也吃 include 目录）
    "${CMAKE_CURRENT_BINARY_DIR}"
)
# 生成头变化时重编 .rc，否则改版本号不会反映到产物。
set_source_files_properties(TextureLoader.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/Version.h")
target_link_libraries(TextureLoader PRIVATE
    d3d11
    d3d12
    dxgi
    shlwapi
    "${TEXTURELOADER_DETOURS_ROOT}/detours.lib"
)

# 注意：不链接 dstorage.lib——GDDS 运行时经 LoadLibraryW 动态加载（绝对路径），
# 避免注入场景下 DSTORAGE.dll 依赖解析失败导致整个 DLL 无法加载（err=126）。

# 作为普通 DLL 由宿主插件加载器（如 Starward DllList）加载，产物名 TextureLoader.dll。
set_target_properties(TextureLoader PROPERTIES OUTPUT_NAME "TextureLoader")

# GDDS 运行时（dstorage.dll/dstoragecore.dll——系统不自带，随 DLL 分发）。
# 注意：二进制不进入 git（**/x64/ 忽略）；源码构建不要求存在——
# 仅当 SDK 目录含运行时 DLL 时才复制，缺失时给出提示（运行时需自行获取，
# 见 README.md / NOTICE.md）。
if(EXISTS "${DSTORAGE_ROOT}/bin/x64/dstorage.dll")
    add_custom_command(TARGET TextureLoader POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DSTORAGE_ROOT}/bin/x64/dstorage.dll"
            "${DSTORAGE_ROOT}/bin/x64/dstoragecore.dll"
            $<TARGET_FILE_DIR:TextureLoader>
    )
else()
    message(WARNING "DirectStorage runtime DLLs not found in ${DSTORAGE_ROOT}/bin/x64 — \
TextureLoader.dll will build, but GDDS loading needs dstorage.dll/dstoragecore.dll at runtime")
endif()
