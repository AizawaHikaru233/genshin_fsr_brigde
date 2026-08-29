# Genshin FSR Bridge

面向原神 Windows DX11 客户端的图形插件。它独立 hook 游戏原生 FSR2 调用，并转接到 AMD FFX12 官方 SDK 实现超分：在支持的显卡上直接提供 FSR4/FSR3/FSR2，不依赖 OptiScaler 等外部插件；接入 OptiScaler 后可以进一步扩展 DLSS/XeSS/FSR4 INT8 等超分类型。

本仓库同时包含 `AntiPlayerMosaic/` 子项目。它是独立构建的原神马赛克修复与 UID 隐藏插件，具体用法见该目录的 README。

英文说明见 [README_EN.md](README_EN.md)。

## 支持范围与风险声明

- 目标支持原神中国服与全球服的 Windows DX11 客户端。
- 已更新支持原神 `7.0` 版本，后续如无意外不需要再更新也能支持新版本。
- 本项目与 `HoYoverse`、`miHoYo`、`《原神》`及 `Genshin Impact` 均无关联，也未获得其认可或授权；相关名称与商标归其各自权利人所有。
- 使用第三方 DLL、注入器、Mod 或图形插件可能违反游戏规则，并可能导致账号限制或封禁。使用者须自行评估风险并承担全部责任。

## 效果演示

![FSR4 激活](assets/FSR4激活.jpg)

![超分档位切换](assets/超分档位切换.jpg)

## 帧生成分支

`frame-generation` 分支中的帧生成功能基于 `OptiScaler 0.10.0-pre1` 构建。低于 `0.10.0-pre1` 的 OptiScaler 不支持该帧生成功能。

## 发布包

包由构建脚本直接组装：安装器脚本位于 `tools/FpsUnlockInstaller/`，反馈与组件清单位于 `assets/FpsUnlockPackage/`，运行资源和默认配置位于 `SharedResources/`。GitHub 发布包不会内置 NVIDIA DLSS 组件与 ReShade 二进制，安装时由脚本从各自官方上游获取（DLSS 来自 NVIDIA Streamline、ReShade 来自 reshade.me）；本地分发包需要自行补齐相应组件。

两个自有 DLL 是编译产物，不提交到仓库。要生成 GitHub 发布包，请在 Windows 上运行：

```powershell
powershell -ExecutionPolicy Bypass -File .\Build-OnlineInstaller.ps1 -Configuration Release -GithubOnly
```

构建结果位于 `dist\原神解帧FSR插件包_v*.7z`，GitHub 发布目录生成 `dist\github-release\GenshinFSRBridge_v*.zip`。GitHub Actions 只构建和发布这个 GitHub 发布包，不生成芙芙包。

## 芙芙启动器插件包源码与本地构建

`FufuGraphicsPlugin/` 提供芙芙启动器插件的源码、配置模板以及商城/本地测试 Lua 安装脚本；仓库不提交芙芙启动器插件二进制包。本地构建前先运行 `tools/Update-UpstreamComponents.ps1` 获取上游组件基线（OptiScaler 固定 v0.9.4，DLSS、ReShade、FPS Unlocker 获取各自最新正式版并做 SHA-256 校验），再运行：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\Update-UpstreamComponents.ps1 -WorkspaceRoot .
powershell -ExecutionPolicy Bypass -File .\Build-OnlineInstaller.ps1 -Configuration Release
```

也可以让构建脚本先自动更新上游组件再打包（`-FetchUpstream`）。芙芙启动器插件包仅写入本地 `dist\FSR-Bridge-Plugin.v*.zip`，不会进入 GitHub Actions 或 GitHub Release。

## 功能

- 通过 DX11 设备与上下文拦截获取原神的 FSR2 调用时机。
- 为 FFX12 SDK 准备颜色、深度、运动向量、抖动和历史资源，并转接超分 dispatch。
- 按显卡能力自动匹配 FSR 系列（FSR4/FSR3/FSR2），支持显卡上无需外部插件即可超分。
- 将游戏渲染精度菜单扩展为 `0.2–0.9 + 原生`；`原生` 档位为游戏原本的 `1.0` 渲染精度。
- 运行时日志默认写入 DLL 同目录的 `Dx11FsrBridge.log`，用于排查加载与 Hook 状态。

## 仓库结构

- 仓库根目录：FSR Bridge 源码、配置与构建文件。
- `AntiPlayerMosaic/`：反虚化、隐藏 UID 与水下马赛克修复插件。
- `third_party/`：Bridge 的构建依赖及其原始声明。

## 使用方法

从 [Releases](https://github.com/AizawaHikaru233/genshin_fsr_brigde/releases) 下载压缩包，解压后运行 `一键配置.bat` 并根据提示安装。英语界面可运行 `GenshinFSRBridgeTools.bat`；也可在安装器主菜单中随时切换中文或 English，选择会自动保存。GitHub 发布包内置 FPS Unlocker 与 OptiScaler，安装脚本会在运行时从官方上游获取 [NVIDIA DLSS 超分组件（`nvngx_dlss.dll`）](https://github.com/NVIDIA-RTX/Streamline/releases) 与 ReShade；本地分发包需要自行补齐相应组件。
游戏内必须启用 `FSR2` 抗锯齿，渲染精度需低于 `1`。

`Dx11FsrBridge.dll` 独立 hook 原神的 FSR2 调用并转接到 AMD FFX12 SDK，在支持的显卡上直接实现 FSR4/FSR3/FSR2 超分，无需 OptiScaler 等外部插件。可选接入 [OptiScaler](https://github.com/optiscaler/OptiScaler) 扩展超分类型（DLSS、XeSS、FSR4 INT8 等）。安装包已按默认顺序配置好组件加载，通常无需手动指定；`AntiPlayerMosaic.dll` 为可选的反虚化/UID 隐藏插件。

若接入 OptiScaler 和 ReShade，它们的运行配置位于各自组件目录。OptiScaler 的 DLL 与日志路径、ReShade 的着色器、纹理、Preset 和截图路径均使用相对路径，避免安装目录含中文时被第三方配置保存逻辑错误转码。只有游戏目录中用于定位外置 ReShade 目录的 `[INSTALL] BasePath` 在跨目录或跨盘安装时必须使用动态生成的绝对路径。

## 构建

需要 Visual Studio（含 C++ 桌面开发组件）、Windows SDK 和 CMake 3.20 或更新版本（Ninja 生成器）。发布配置需要仓库内的 `Dx11FsrBridge\third_party` 目录，其中包含 FFX12 ffx-api 头文件和 Microsoft Detours 构建依赖。

先更新上游组件基线（仅需在组件版本变化时执行；脚本固定 OptiScaler v0.9.4，DLSS、ReShade、FPS Unlocker 获取各自最新正式版）：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\Update-UpstreamComponents.ps1 -WorkspaceRoot .
```

然后构建全部发行包：

```powershell
powershell -ExecutionPolicy Bypass -File .\Build-OnlineInstaller.ps1 -Configuration Release
```

需要先更新组件再打包时可合并为 `-FetchUpstream`。三个自有 DLL（Bridge / AntiPlayerMosaic / FufuGraphicsPlugin）由构建脚本以 Ninja 生成器自动编译，无需手动执行 cmake。

## 日志与问题反馈

Bridge 和反虚化组件默认会保留错误日志（接入 OptiScaler/ReShade 时它们也会保留各自日志）。每次重新运行会覆盖上一轮日志。
遇到游戏无法启动、FSR 无法激活、切换超分后闪退或其他异常时，请在复现后不要再次启动游戏，并提供：

1. `payload/Bridge/Dx11FsrBridge.log` (必须)
2. `payload/OptiScaler/OptiScaler.log` 与 `payload/OptiScaler/OptiScaler.ini`（使用 OptiScaler 时）
3. `payload/ReShade/ReShade.log`（涉及 ReShade 时）
4. `payload/AntiPlayerMosaic/AntiPlayerMosaic.log`（涉及反虚化、UID 或水下马赛克时）
5. 芙芙插件目录下的 `FSR-Bridge-Plugin.log`（使用芙芙启动器插件时）
6. 显卡型号、游戏版本、异常发生阶段和所选超分模式

需要进一步排查时，可临时将 `OptiScaler.ini`（接入 OptiScaler 时）中 `Log` 下的 `LogLevel` 改为 `1（Debug）`或 `0（Trace）`，但诊断结束后应恢复正式配置以避免额外开销。
不要把游戏账号、登录信息或包含个人信息的截图提交到公开 Issue。

## 第三方组件

- FFX12 ffx-api 头文件与 Microsoft Detours 仅作为构建依赖，保留各自原始许可证与声明。
- OptiScaler 是独立项目：<https://github.com/optiscaler/OptiScaler>。
- GitHub 发布包不会内置 NVIDIA DLSS 组件与 ReShade 二进制，安装时由脚本从各自官方上游获取。
- 本地分发包需要自行补齐相应组件，并遵守各组件授权要求（例如随附 GPL 全文与源码链接、ReShade 官方"不得分享二进制"、DLSS 仅限 NVIDIA GPU 使用等）。
- 本项目不包含 NVIDIA DLSS 与 AMD FSR SDK 运行时二进制。

## 许可证

本项目采用 [GPL-3.0-or-later](Dx11FsrBridge/LICENSE)。你可以使用、修改和再分发代码；分发修改版本时必须同时提供对应完整源码，并以 GPL-3.0-or-later 发布。
