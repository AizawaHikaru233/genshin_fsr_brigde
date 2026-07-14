# Genshin FSR Bridge

面向原神 Windows DX11 客户端的 FSR2 ABI 桥接 DLL。它在游戏进程中提供标准 FSR2 导出，并把游戏的上采样调用转接给外部兼容实现，例如 OptiScaler。

本项目不是 HoYoverse、AMD 或 OptiScaler 的官方项目；不包含 OptiScaler、FSR SDK 运行时、显卡驱动组件或其他超分 DLL。

本仓库同时包含 `AntiPlayerMosaic/` 子项目。它是独立构建的原神马赛克修复与 UID 隐藏插件，具体用法见该目录的 README。

## Lite 发布包

`GenshinOneClick/` 包含 Lite 安装器的完整脚本、默认配置、官方 ReShade Add-on DLL 和 HDR 着色器。FPS Unlocker 与 OptiScaler 不随 Lite 包或本仓库分发，安装器会从其官方来源下载或要求用户手动选择。

两个自有 DLL 是编译产物，不提交到仓库。要生成与 GitHub Actions 相同的 Lite ZIP，请在 Windows 上运行：

```powershell
powershell -ExecutionPolicy Bypass -File .\Build-Package.ps1
```

构建结果位于 `dist\原神解帧FSR插件包Lite_v*.zip`。GitHub Actions 在 `main` 分支推送后也会自动编译，并将 ZIP 上传为 `GenshinOneClick-Lite` Artifact。

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
- FPS Unlocker 或其他支持按顺序加载 DLL 的第三方启动器/注入器。
- 需要替换超分时，用户自行安装兼容的 OptiScaler 或其他可识别标准 FSR2 导出的转换工具及其依赖。

## 加载与转换链路

`Dx11FsrBridge.dll` 本身不执行 FSR、DLSS、XeSS 或其他超分算法。它只向外部工具暴露标准 FSR2 接口，并将游戏的 DX11 上采样调用转接到该接口。

使用 FPS Unlocker 时，将 Bridge 与外部超分工具加入其 DLL 注入列表。也可以使用其他 DLL 注入工具，但必须保证它支持稳定的按序加载，且不会重复加载同一 DLL。

推荐加载顺序：

1. `Dx11FsrBridge.dll`
2. `OptiScaler.dll`
3. `AntiPlayerMosaic.dll`（可选）
4. `ReShade64.dll`（可选）

仅使用超分桥接时，前两项必须保持该顺序：Bridge 先加载，随后由 OptiScaler（或同类工具）在启动时扫描标准 FSR2 导出并接管。Bridge 不直接加载、修改或捆绑 OptiScaler；后端选择、FSR3/FSR4 模型和其他 OptiScaler 配置均由用户自己的工具安装负责。

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
- Lite 资源包含官方 ReShade Add-on DLL（BSD-3-Clause）与 Lilium HDR 着色器（GPL-3.0），许可证位于各自资源目录。
- 本项目不包含 Snap Hutao、NVIDIA DLSS、AMD FSR SDK 或 OptiScaler 运行时二进制文件。

## 许可证

本项目采用 [GPL-3.0-or-later](LICENSE)。你可以使用、修改和再分发代码；分发修改版本时必须同时提供对应完整源码，并以 GPL-3.0-or-later 发布。
