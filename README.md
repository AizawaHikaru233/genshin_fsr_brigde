# Genshin FSR Bridge

面向原神 Windows DX11 客户端的 FSR2 ABI 桥接 DLL。它在游戏进程中提供标准 FSR2 导出，并把游戏的上采样调用转接给外部兼容实现，例如 OptiScaler。

本项目不是 HoYoverse、AMD 或 OptiScaler 的官方项目；不包含 OptiScaler、FSR SDK 运行时、显卡驱动组件或其他超分 DLL。

本仓库同时包含 `AntiPlayerMosaic/` 子项目。它是独立构建的原神马赛克修复与 UID 隐藏插件，具体用法见该目录的 README。

## 功能

- 通过 DX11 设备与上下文拦截获取原神的 FSR2 调用时机。
- 提供标准 FSR2 导出，使外部超分工具可以识别 FSR2 接口。
- 为外部处理器准备颜色、深度、运动向量、抖动和历史资源。
- 运行时日志默认写入 DLL 同目录的 `Dx11FsrBridge.log`，用于排查加载与 Hook 状态。
- 正式配置关闭资源导出、逐帧追踪和调试探针，避免额外开销。

## 仓库结构

- 仓库根目录：FSR Bridge 源码、配置与构建文件。
- `AntiPlayerMosaic/`：反虚化、隐藏 UID 与水下马赛克修复插件。
- `third_party/`：Bridge 的构建依赖及其原始声明。

## 运行要求

- 64 位 Windows、Direct3D 11 原神客户端。
- 能加载本 DLL 的外部启动器或注入器。
- 需要替换超分时，用户自行安装兼容的 OptiScaler 版本及其依赖。

推荐加载顺序：

1. `Dx11FsrBridge.dll`
2. `OptiScaler.dll`

Bridge 不直接加载、修改或捆绑 OptiScaler；后端选择、FSR3/FSR4 模型和其他 OptiScaler 配置均由用户自己的 OptiScaler 安装负责。

## 构建

需要 Visual Studio 2022（含 C++ 桌面开发组件）、Windows SDK 和 CMake 3.20 或更新版本。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DDX11FSRBRIDGE_RELEASE_RUNTIME=ON `
  -DDX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL=ON
cmake --build build --config Release
```

生成的 DLL 与 `Dx11FsrBridge.release.ini` 会位于 `build\Release`。发布配置需要仓库内的 `third_party` 目录，其中包含 FSR2 兼容 ABI 头文件和 Microsoft Detours 构建依赖。

`DX11FSRBRIDGE_ENABLE_FSR31_EXPERIMENTAL`、`DX11FSRBRIDGE_ENABLE_OPTISCALER_NGX_EXPERIMENTAL` 等 CMake 选项仅用于实验，不属于正式运行链路。

## 配置与反馈

DLL 从自身目录读取 `Dx11FsrBridge.ini`。正式发行版默认启用基础日志；请在复现问题后保留并提交：

- `Dx11FsrBridge.log`
- `Dx11FsrBridge.ini`
- OptiScaler 的日志与配置（若使用）
- 显卡型号、游戏版本、异常阶段和选择的超分模式

不要把游戏账号、登录信息或包含个人信息的截图提交到公开 Issue。

## 第三方组件

- FSR2 ABI 头文件与 Microsoft Detours 仅作为构建依赖，保留各自原始许可证与声明。
- OptiScaler 是独立项目：<https://github.com/optiscaler/OptiScaler>。
- 本项目不包含 ReShade、Snap Hutao、NVIDIA DLSS 或 AMD FSR SDK 运行时二进制文件。

## 许可证

本项目采用 [GPL-3.0-or-later](LICENSE)。你可以使用、修改和再分发代码；分发修改版本时必须同时提供对应完整源码，并以 GPL-3.0-or-later 发布。
