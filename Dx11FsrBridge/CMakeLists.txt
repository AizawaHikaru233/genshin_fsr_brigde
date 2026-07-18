cmake_minimum_required(VERSION 3.20)

project(Dx11FsrBridge LANGUAGES CXX)

option(DX11FSRBRIDGE_ENABLE_FSR31_EXPERIMENTAL "Build the experimental AMD FSR3.1 backend" OFF)
option(DX11FSRBRIDGE_ENABLE_OPTISCALER_NGX_EXPERIMENTAL "Build the passive OptiScaler NGX export probe" OFF)
option(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL "Build the standard FSR2 translation exports" OFF)
option(DX11FSRBRIDGE_RELEASE_RUNTIME "Build without runtime diagnostics and capture hooks" OFF)
option(DX11FSRBRIDGE_GPU_PROFILE_RUNTIME "Use the GPU profiling runtime configuration" OFF)
option(DX11FSRBRIDGE_SERVER_DEBUG_RUNTIME "Build server-agnostic diagnostics for FSR hook discovery" OFF)
option(DX11FSRBRIDGE_FG_DXGI_DIAGNOSTICS "Log DXGI swapchain creation inputs for the frame-generation test build" OFF)
option(DX11FSRBRIDGE_ASI "Build the bridge as an OptiScaler ASI plugin (.asi)" OFF)

add_library(Dx11FsrBridge SHARED
    Dx11FsrBridge.cpp
)

if(DX11FSRBRIDGE_ASI)
    # ASI files are ordinary PE DLLs with a different filename extension.
    # Keep the target name stable so the generated import/export files remain
    # compatible with the normal bridge build.
    set_target_properties(Dx11FsrBridge PROPERTIES SUFFIX ".asi")
    target_compile_definitions(Dx11FsrBridge PRIVATE DX11FSRBRIDGE_ASI=1)
endif()

set(DX11FSRBRIDGE_OPTISCALER_SOURCE_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/OptiScaler-master/OptiScaler"
    CACHE PATH "Optional OptiScaler source tree used only by legacy experimental backends")

if(DX11FSRBRIDGE_ENABLE_FSR31_EXPERIMENTAL)
    target_sources(Dx11FsrBridge PRIVATE Fsr31Bridge.cpp)
    target_include_directories(Dx11FsrBridge PRIVATE
        "${DX11FSRBRIDGE_OPTISCALER_SOURCE_ROOT}/include"
    )
else()
    target_sources(Dx11FsrBridge PRIVATE Fsr31BridgeStub.cpp)
endif()

if(DX11FSRBRIDGE_ENABLE_OPTISCALER_NGX_EXPERIMENTAL)
    target_sources(Dx11FsrBridge PRIVATE OptiScalerNgxBridge.cpp)
    target_include_directories(Dx11FsrBridge PRIVATE
        "${DX11FSRBRIDGE_OPTISCALER_SOURCE_ROOT}/../external/nvngx_dlss_sdk"
        "${DX11FSRBRIDGE_OPTISCALER_SOURCE_ROOT}/include"
    )
    target_compile_definitions(Dx11FsrBridge PRIVATE DX11FSRBRIDGE_ENABLE_OPTISCALER_NGX_EXPERIMENTAL=1)
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

if(DX11FSRBRIDGE_ENABLE_FSR31_EXPERIMENTAL)
    target_link_libraries(Dx11FsrBridge PRIVATE
        "${DX11FSRBRIDGE_OPTISCALER_SOURCE_ROOT}/library/fsr31/ffx_backend_dx11_x64.lib"
        "${DX11FSRBRIDGE_OPTISCALER_SOURCE_ROOT}/library/fsr31/ffx_fsr3upscaler_x64.lib"
    )
endif()

set(DX11FSRBRIDGE_INI_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/Dx11FsrBridge.ini")
if(DX11FSRBRIDGE_GPU_PROFILE_RUNTIME)
    set(DX11FSRBRIDGE_INI_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/Dx11FsrBridge.profile.ini")
elseif(DX11FSRBRIDGE_SERVER_DEBUG_RUNTIME)
    set(DX11FSRBRIDGE_INI_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/Dx11FsrBridge.server-debug.ini")
elseif(DX11FSRBRIDGE_RELEASE_RUNTIME)
    set(DX11FSRBRIDGE_INI_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/Dx11FsrBridge.release.ini")
elseif(DX11FSRBRIDGE_ENABLE_OPTISCALER_NGX_EXPERIMENTAL)
    set(DX11FSRBRIDGE_INI_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/Dx11FsrBridge.ngx-experimental.ini")
endif()

add_custom_command(TARGET Dx11FsrBridge POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${DX11FSRBRIDGE_INI_SOURCE}
        $<TARGET_FILE_DIR:Dx11FsrBridge>/Dx11FsrBridge.ini
)
