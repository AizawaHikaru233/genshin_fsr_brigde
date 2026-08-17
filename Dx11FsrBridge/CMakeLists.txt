cmake_minimum_required(VERSION 3.20)

project(Dx11FsrBridge LANGUAGES CXX RC)

option(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL "Build the standard FSR2 translation exports" OFF)
option(DX11FSRBRIDGE_RELEASE_RUNTIME "Build the release runtime" ON)

add_library(Dx11FsrBridge SHARED
    Dx11FsrBridge.cpp
    Dx11FsrBridge.rc
    RenderScaleMenu.cpp
)

if(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
    target_sources(Dx11FsrBridge PRIVATE
        Fsr2TranslationLayer.cpp
        Fsr2TranslationLayer.def
    )
    target_compile_definitions(Dx11FsrBridge PRIVATE DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL=1)
endif()

if(DX11FSRBRIDGE_RELEASE_RUNTIME)
    target_compile_definitions(Dx11FsrBridge PRIVATE DX11FSRBRIDGE_RELEASE_RUNTIME=1)
endif()

target_compile_features(Dx11FsrBridge PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(Dx11FsrBridge PRIVATE /utf-8)
    if(DX11FSRBRIDGE_RELEASE_RUNTIME)
        target_compile_options(Dx11FsrBridge PRIVATE $<$<CONFIG:Release>:/GL>)
        target_link_options(Dx11FsrBridge PRIVATE $<$<CONFIG:Release>:/LTCG>)
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
)
target_link_libraries(Dx11FsrBridge PRIVATE
    d3d11
    d3dcompiler
    dxgi
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/detours/detours.lib"
)
