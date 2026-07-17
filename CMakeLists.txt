cmake_minimum_required(VERSION 3.20)

project(Dx11FsrBridge LANGUAGES CXX RC)

option(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL "Build the standard FSR2 translation exports" OFF)
option(DX11FSRBRIDGE_RELEASE_RUNTIME "Build without runtime diagnostics and capture hooks" OFF)
option(DX11FSRBRIDGE_SERVER_DEBUG_RUNTIME "Build server-agnostic diagnostics for FSR hook discovery" OFF)
option(DX11FSRBRIDGE_FG_DXGI_DIAGNOSTICS "Log DXGI swapchain creation inputs for the frame-generation test build" OFF)
option(DX11FSRBRIDGE_ASI "Build the bridge as an OptiScaler ASI plugin (.asi)" OFF)

add_library(Dx11FsrBridge SHARED
    Dx11FsrBridge.cpp
    Dx11FsrBridge.rc
    Fsr31BridgeStub.cpp
)

if(DX11FSRBRIDGE_ASI)
    # ASI files are ordinary PE DLLs with a different filename extension.
    # Keep the target name stable so the generated import/export files remain
    # compatible with the normal bridge build.
    set_target_properties(Dx11FsrBridge PROPERTIES SUFFIX ".asi")
    target_compile_definitions(Dx11FsrBridge PRIVATE DX11FSRBRIDGE_ASI=1)
endif()

if(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
    target_sources(Dx11FsrBridge PRIVATE
        Fsr2TranslationLayer.cpp
        Fsr2TranslationLayer.def
    )
    target_compile_definitions(Dx11FsrBridge PRIVATE DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL=1)
    target_include_directories(Dx11FsrBridge PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
    )
    target_link_libraries(Dx11FsrBridge PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/detours/detours.lib"
    )
endif()

if(DX11FSRBRIDGE_RELEASE_RUNTIME)
    target_compile_definitions(Dx11FsrBridge PRIVATE DX11FSRBRIDGE_RELEASE_RUNTIME=1)
endif()

if(DX11FSRBRIDGE_SERVER_DEBUG_RUNTIME)
    target_compile_definitions(Dx11FsrBridge PRIVATE DX11FSRBRIDGE_SERVER_DEBUG_RUNTIME=1)
endif()

if(DX11FSRBRIDGE_FG_DXGI_DIAGNOSTICS)
    target_compile_definitions(Dx11FsrBridge PRIVATE DX11FSRBRIDGE_FG_DXGI_DIAGNOSTICS=1)
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
target_link_libraries(Dx11FsrBridge PRIVATE
    d3d11
    d3dcompiler
    dxgi
)

# The production mode2 build is self-contained and intentionally has no deployment
# INI.  Runtime behavior is fixed in source so an obsolete Bridge INI cannot
# silently select a former mode or color path.
