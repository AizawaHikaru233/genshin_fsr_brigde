cmake_minimum_required(VERSION 3.20)

project(Dx11FsrBridge LANGUAGES C CXX RC)

option(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL "Build the standard FSR2 translation exports" OFF)
option(DX11FSRBRIDGE_RELEASE_RUNTIME "Build the release runtime" ON)
option(DX11FSRBRIDGE_BUILD_FAMILY_TEST "Build the Fsr2FamilyTakeover state-machine test" OFF)

# The bridge deliberately uses the deployed ffx-api runtime provider.  The
# static open-SDK host was tested in-game and produced a black output, while
# this provider has a known-good render path and exposes 2.3.4/3.1.5/4.1.1.
set(FFX_API_INCLUDE_ROOT
    "D:/DLSSG2FSR4/dependencies/FidelityFX-SDK/ffx-api/include/ffx_api"
    CACHE PATH "Path to the FidelityFX ffx-api include directory")
if(NOT EXISTS "${FFX_API_INCLUDE_ROOT}/ffx_api.h")
    message(FATAL_ERROR "FidelityFX ffx-api headers were not found: ${FFX_API_INCLUDE_ROOT}")
endif()

add_library(Dx11FsrBridge SHARED
    Dx11FsrBridge.cpp
    Dx11FsrBridge.rc
    RenderScaleMenu.cpp
    Fsr2FamilyTakeover.cpp
    Il2CppCallSiteHook.cpp
    Ffx12Backend.cpp
)

# 旧方案已彻底移除：Fsr2TranslationLayer（旧翻译层）与 Dx11On12Swapchain（On12 引导）
# 不再编译进 DLL；OptiScaler 以新 ffx12 链路为基底接入。
if(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
    target_compile_definitions(Dx11FsrBridge PRIVATE DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL=1)
endif()

if(DX11FSRBRIDGE_RELEASE_RUNTIME)
    # Debug 配置（--config Debug）不定义正式版过滤宏：log_line 全量输出（排查日志等级提升）。
    # Release 保持白名单精简（error_terms + basic_terms）。
    target_compile_definitions(Dx11FsrBridge PRIVATE
        $<$<NOT:$<CONFIG:Debug>>:DX11FSRBRIDGE_RELEASE_RUNTIME=1>
        $<$<CONFIG:Debug>:DX11FSRBRIDGE_DEBUG_RUNTIME=1>
    )
endif()

target_compile_features(Dx11FsrBridge PRIVATE cxx_std_20)
if(MSVC)
    # /EHsc 仅 Debug 配置开（异常展开——排查用，消除 C4530）；
    # Release 不开（异常展开表使 DLL +184KB；桥自身不抛 C++ 异常，catch 为防御，
    # 无 /EHsc 时异常直接 terminate（fail-fast），与旧版 762KB 行为一致）。
    target_compile_options(Dx11FsrBridge PRIVATE /utf-8 $<$<CONFIG:Debug>:/EHsc>)
    if(DX11FSRBRIDGE_RELEASE_RUNTIME)
        
        
        # 链接映射文件（模块大小分析用；仅构建产物，不部署）
        target_compile_options(Dx11FsrBridge PRIVATE $<$<CONFIG:Release>:/GL>)
        target_link_options(Dx11FsrBridge PRIVATE $<$<CONFIG:Release>:/LTCG>)
        target_link_options(Dx11FsrBridge PRIVATE $<$<CONFIG:Release>:/MAP:$<TARGET_FILE_DIR:Dx11FsrBridge>/Dx11FsrBridge.map>)
    endif()
endif()
target_compile_definitions(Dx11FsrBridge PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)
target_include_directories(Dx11FsrBridge PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/detours"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
    "${FFX_API_INCLUDE_ROOT}"
)
target_link_libraries(Dx11FsrBridge PRIVATE
    d3d11
    d3d12
    d3dcompiler
    dxgi
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/detours/detours.lib"
)

if(DX11FSRBRIDGE_BUILD_FAMILY_TEST)
    add_executable(Fsr2FamilyTakeoverTest
        Fsr2FamilyTakeoverTest.cpp
        Fsr2FamilyTakeover.cpp
    )
    target_compile_features(Fsr2FamilyTakeoverTest PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(Fsr2FamilyTakeoverTest PRIVATE /utf-8 /W3)
    endif()
    target_compile_definitions(Fsr2FamilyTakeoverTest PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )
endif()
