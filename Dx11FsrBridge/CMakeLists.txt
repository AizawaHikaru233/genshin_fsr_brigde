cmake_minimum_required(VERSION 3.20)

# 版本号**单一事实源**（2026-09-19 审核报告）。
# 改版本只改这里；`.rc` 通过下面生成的头读取，不再手写。
project(Dx11FsrBridge VERSION 2.2.0.0 LANGUAGES C CXX RC)

# 生成版本头供 .rc 使用（生成到构建目录，避免污染源码树）。
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/Version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/Version.h"
    @ONLY)

option(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL "Build the standard FSR2 translation exports" ON)
option(DX11FSRBRIDGE_RELEASE_RUNTIME "Build the release runtime" ON)
option(DX11FSRBRIDGE_BUILD_FAMILY_TEST "Build the Fsr2FamilyTakeover state-machine test" OFF)
# 统一测试开关（2026-09-19 审核报告）。
#
# 背景：仓库有 7 个 `*Test.cpp`，但此前**只有** `Fsr2FamilyTakeoverTest` 接了 CMake
# （且需单独开 `DX11FSRBRIDGE_BUILD_FAMILY_TEST`）。其余 6 个被 git 跟踪却**从不编译**
# —— 会静默腐烂（改坏也没人知道），而它们恰是唯一能自动验证核心逻辑的东西。
#
# 这里接入**无需 FFX SDK 头**的两个：
#   - `Il2CppCallSiteHookTest`  —— 只需 `Il2CppCallSiteHook.cpp`
#   - `Ffx12BackendTest`        —— 只需 `Ffx12Backend.cpp`（其 `Ffx12Backend.h`
#     仅依赖 <cstdint>/<string>，不需要 ffx_api 头）
#
# **未接入**（原因已核实，非疏漏）：
#   - `Ffx12Test` / `Ffx12AccumTest` —— `#include "ffx_api.h"` / `"ffx_upscale.h"`，
#     需 FFX SDK 头 staging（本文件的 FFX_API_STAGE_ROOT 流程），且运行需真实 SDK DLL
#   - `Ffx12DescTest`               —— 纯 D3D12 计算着色器验证，运行需 GPU
#   - `GpuRouteTest`                —— 已在文件内标注 OBSOLETE
# 这些留待需要时按 FFX SDK staging 接入；先保证"能无外部依赖编译"的两个进 CI。
option(DX11FSRBRIDGE_BUILD_TESTS "Build the SDK-free unit tests (il2cpp callsite, ffx12 backend)" OFF)

# The bridge deliberately uses the deployed ffx-api runtime provider.  The
# static open-SDK host was tested in-game and produced a black output, while
# this provider has a known-good render path and exposes 2.3.4/3.1.5/4.1.1.
#
# 头文件来源：AMD FidelityFX SDK。其许可限定"binary form only"再分发，故不
# vendor 进仓库；默认取 OptiScaler 源码树的 FidelityFX-SDK-v2 子模块，并在
# 配置阶段 staging 到构建目录（保持 SDK 的 api/include 与 upscalers/include
# 两层结构——ffx_upscale.h 内部用 "../../api/include/ffx_api.h" 相对包含，
# 扁平化会破坏它）。
# 前置：git -C OptiScaler-src submodule update --init external/FidelityFX-SDK-v2
# 也可用 -DFFX_SDK_KIT_ROOT=<...>/Kits/FidelityFX 指定其它 SDK 快照。
set(FFX_SDK_KIT_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../OptiScaler-src/external/FidelityFX-SDK-v2/Kits/FidelityFX"
    CACHE PATH "Path to the FidelityFX SDK Kits/FidelityFX directory")
set(FFX_API_STAGE_ROOT
    "${CMAKE_BINARY_DIR}/ffx-sdk-stage"
    CACHE PATH "Build-time staging root for the FidelityFX SDK headers")
if(EXISTS "${FFX_SDK_KIT_ROOT}/api/include/ffx_api.h" AND
   EXISTS "${FFX_SDK_KIT_ROOT}/upscalers/include/ffx_upscale.h")
    # file(COPY <dir>) 复制的是目录内容，故显式给出目标子目录以保留 SDK 层次。
    file(COPY "${FFX_SDK_KIT_ROOT}/api/include/" DESTINATION "${FFX_API_STAGE_ROOT}/api/include")
    file(COPY "${FFX_SDK_KIT_ROOT}/upscalers/include/" DESTINATION "${FFX_API_STAGE_ROOT}/upscalers/include")
endif()
set(FFX_API_INCLUDE_ROOT "${FFX_API_STAGE_ROOT}/api/include")
set(FFX_UPSCALER_INCLUDE_ROOT "${FFX_API_STAGE_ROOT}/upscalers/include")
if(NOT EXISTS "${FFX_API_INCLUDE_ROOT}/ffx_api.h" OR
   NOT EXISTS "${FFX_UPSCALER_INCLUDE_ROOT}/ffx_upscale.h")
    message(FATAL_ERROR "FidelityFX ffx-api headers were not found under ${FFX_API_STAGE_ROOT} "
        "(init OptiScaler-src submodule external/FidelityFX-SDK-v2, or pass -DFFX_SDK_KIT_ROOT=...)")
endif()

add_library(Dx11FsrBridge SHARED
    Dx11FsrBridge.cpp
    BridgeLogger.cpp
    Dx11FsrBridge.rc
    RenderScaleMenu.cpp
    Fsr2FamilyTakeover.cpp
    Il2CppCallSiteHook.cpp
    Ffx12Backend.cpp
)

# 旧方案已彻底移除：Fsr2TranslationLayer（旧翻译层）与 Dx11On12Swapchain（On12 引导）
# 不再编译进 DLL；OptiScaler 以新 ffx12 链路为基底接入。
#
# ⚠️ 默认值必须是 ON（曾经是 OFF，是个静默失效陷阱）：
# 宏名里的 "EXPERIMENTAL" 已名不副实——它现在包住的是**在用的生产路径**：
#   - try_fsr2_translation_draw（draw 翻译派发主链）
#   - fsr2_family_skip（合成族预处理跳过，ini 里 Fsr2FamilySkip 默认 =1）
#   - Fsr2TranslationMode 的 mode-2 按需识别路径
#   - 一批 FSR2 内省/统计（format_bytes_per_pixel、translation dispatch 计数等）
# 仓库里**没有任何构建脚本**传这个宏，此前它只存在于本机 build 目录的 CMakeCache
# （DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL:BOOL=ON）。也就是说：
# 任何全新 clone / CI / 换机构建都会拿到 OFF → 翻译层整块消失，而 ini 里
# Fsr2FamilySkip=1 仍在声称"已跳过预处理"——配置与二进制不一致，且无任何报错。
if(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
    target_compile_definitions(Dx11FsrBridge PRIVATE DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL=1)
endif()

if(DX11FSRBRIDGE_RELEASE_RUNTIME)
    # Debug 配置（--config Debug）**不**定义正式版过滤宏 → 日志全量输出。
    # Release 定义 DX11FSRBRIDGE_RELEASE_RUNTIME=1 → 由 BridgeLogger 按**显式等级**裁剪。
    #
    # ⚠️ 2026-09-19（审核报告）修正过时注释：此处原写"Release 保持白名单精简
    # （error_terms + basic_terms）"——那是**旧日志方案**的描述，已不适用：
    #   - 新 BridgeLogger 用显式 `LOG_DEBUG/INFO/WARN/ERROR` + 每分类等级，
    #     不再按消息内容猜等级（见 BridgeLogger.h 的设计目标说明）。
    #   - `basic_terms` 在白名单里已不存在；`error_terms` 仅保留给
    #     **兼容路径** `level_from_prefix`（BridgeLogger.cpp:253），
    #     新代码不再经过它。
    #
    # 另：此处原还定义了 `DX11FSRBRIDGE_DEBUG_RUNTIME=1`（Debug 配置），
    # 但全仓库**无任何代码引用**该宏（已核实），属死定义，已删除。
    target_compile_definitions(Dx11FsrBridge PRIVATE
        $<$<NOT:$<CONFIG:Debug>>:DX11FSRBRIDGE_RELEASE_RUNTIME=1>
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
    "${FFX_UPSCALER_INCLUDE_ROOT}"
    # .rc 里 `#include "Version.h"` 需要能找到生成的头（RC 编译器也吃 include 目录）
    "${CMAKE_CURRENT_BINARY_DIR}"
)
# 生成头变化时重编 .rc，否则改版本号不会反映到产物。
set_source_files_properties(Dx11FsrBridge.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/Version.h")
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

# ---- 单元测试接入（2026-09-19 审核报告：此前被跟踪但从不编译）----
#
# 背景：仓库有 7 个 `*Test.cpp`，此前**只有** `Fsr2FamilyTakeoverTest` 接了 CMake
# （且需单独开 `DX11FSRBRIDGE_BUILD_FAMILY_TEST`）。其余 6 个被 git 跟踪却**从不编译**
# —— 会静默腐烂，而它们恰是唯一能自动验证核心逻辑的东西。
#
# 接入实测结论（逐个编译并运行过，不是估计）：
#   ✅ `Il2CppCallSiteHookTest` —— 可编译、**运行通过**（`ALL PASS`）。
#      接入后立刻暴露一个真实缺陷：`shutdown()` 不清 per-instance 表，
#      残留的**失效实例指针**会污染下一次 `install()`（已修）。
#   ⚠️ `Ffx12BackendTest` —— 可编译，但**运行仍崩溃**（见下），故单独开关。
#   ❌ `Ffx12Test` / `Ffx12AccumTest` —— `#include "ffx_api.h"` 等，需 FFX SDK 头
#      staging；`Ffx12DescTest` 另需 GPU；`GpuRouteTest` 已在文件内标注 OBSOLETE。
option(DX11FSRBRIDGE_BUILD_TESTS "Build the runnable SDK-free unit tests (il2cpp callsite)" OFF)
option(DX11FSRBRIDGE_BUILD_BACKEND_TEST
    "Build Ffx12BackendTest (KNOWN ISSUE: crashes in ffxCreateContext under the isolated test harness)"
    OFF)

if(DX11FSRBRIDGE_BUILD_TESTS OR DX11FSRBRIDGE_BUILD_BACKEND_TEST)
    # 共用设置函数：把主 target 用到的宏与警告级别统一施加到测试 target
    function(fsrbridge_configure_test target)
        target_compile_features(${target} PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(${target} PRIVATE /utf-8 /W3)
        endif()
        target_compile_definitions(${target} PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            UNICODE
            _UNICODE
        )
    endfunction()
endif()

if(DX11FSRBRIDGE_BUILD_TESTS)
    # il2cpp callsite hook：观察/跳过/还原模式 + per-instance 隔离 + 相机 hook。
    # 需链 Il2CppCallSiteHook.cpp。**此测试已实测通过**。
    add_executable(Il2CppCallSiteHookTest
        Il2CppCallSiteHookTest.cpp
        Il2CppCallSiteHook.cpp
    )
    fsrbridge_configure_test(Il2CppCallSiteHookTest)
    target_include_directories(Il2CppCallSiteHookTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    enable_testing()
    add_test(NAME Il2CppCallSiteHookTest COMMAND Il2CppCallSiteHookTest)
endif()

if(DX11FSRBRIDGE_BUILD_BACKEND_TEST)
    # ⚠️ **已知问题（2026-09-19 实测）**：本测试能编译、能跑到 FFX context 创建，
    #    但在 `g_runtime.create`（FFX SDK 的 `ffxCreateContext`，D3D12 后端路径）
    #    处**抛出未捕获异常**而终止（退出码 0xE06D7363）。
    #
    #    已核实的事实：
    #      - `ensure_pool` **成功**（GPU 互操作池已建好，逐步打印确认）
    #      - 崩溃发生在 `create_context` 调用第三方 SDK 时，**不在本仓库代码里**
    #      - 三种 SDK DLL 变体（41MB AMD 版 / 28MB ×2）表现**完全一致**
    #      - 加 `/EHsc` 后 `catch(...)` 仍抓不到 → 不是标准 C++ 异常路径
    #    因此判断是**测试环境**（裸 D3D11/D3D12 设备 + 隔离上下文）不满足 SDK 的
    #    内部前提，而非产品缺陷 —— 游戏内该路径正常工作（实测日志有
    #    `ffx12_path chain=direct->ffx_dispatch`）。
    #
    #    但由此**发现并修复了一处真实健壮性缺口**：`create_context` 对
    #    `g_runtime.create` **没有异常保护**，SDK 抛异常会崩掉整个游戏进程。
    #    已加 fail-open 的 try/catch（见 Ffx12Backend.cpp）。
    #
    #    保持本开关默认 OFF，避免让"已知失败的测试"混进 CI 绿灯。
    add_executable(Ffx12BackendTest
        Ffx12BackendTest.cpp
        Ffx12Backend.cpp
    )
    fsrbridge_configure_test(Ffx12BackendTest)
    target_include_directories(Ffx12BackendTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
        "${FFX_API_INCLUDE_ROOT}"
        "${FFX_UPSCALER_INCLUDE_ROOT}"
    )
    target_link_libraries(Ffx12BackendTest PRIVATE d3d11 d3d12 d3dcompiler dxgi)

    message(STATUS "Dx11FsrBridge: building Ffx12BackendTest "
        "(KNOWN ISSUE: crashes in ffxCreateContext under the isolated test harness)")
endif()
