cmake_minimum_required(VERSION 3.20)

project(TextureLoader LANGUAGES C CXX RC)

# GDDS（DirectStorage GPU 解压）依赖——SDK 已 vendor 到本目录 third_party/（MIT + MS 条款，见其 LICENSE*）
set(DSTORAGE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dstorage")

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
    $<$<CONFIG:Release>:TEXTURELOADER_RELEASE_RUNTIME=1>
)
target_include_directories(TextureLoader PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Dx11FsrBridge/third_party/detours"
    "${DSTORAGE_ROOT}/include"
)
target_link_libraries(TextureLoader PRIVATE
    d3d11
    d3d12
    dxgi
    shlwapi
    "${CMAKE_CURRENT_SOURCE_DIR}/../Dx11FsrBridge/third_party/detours/detours.lib"
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
