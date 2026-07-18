# Genshin FSR Bridge

面向原神 Windows DX11 客户端的 FSR2 ABI 桥接项目。Bridge 在游戏进程中提供标准 FSR2 导出，并把原神的上采样调用转接给 OptiScaler 等兼容实现。

本项目不是 HoYoverse、miHoYo、AMD、OptiScaler 或芙芙启动器的官方项目。GitHub 仓库与 Lite 发布包不分发 OptiScaler、FPS Unlocker 或 NVIDIA 运行库；安装脚本只从对应项目的官方来源下载，或要求用户手动选择本地文件。

Bridge 的详细技术说明见 [Dx11FsrBridge/README.md](Dx11FsrBridge/README.md)，英文技术说明见 [Dx11FsrBridge/README_EN.md](Dx11FsrBridge/README_EN.md)。

## 支持范围与风险声明

- 目标支持原神中国服与全球服的 Windows DX11 客户端。
- 当前版本仅针对已验证的游戏版本和渲染路径；游戏更新后不保证继续兼容。
- 本项目与 HoYoverse、miHoYo、《原神》及 Genshin Impact 均无关联，也未获得其认可或授权；相关名称与商标归其各自权利人所有。
- 使用第三方 DLL、注入器、Mod 或图形插件可能违反游戏规则，并可能导致账号限制或封禁。使用者须自行评估风险并承担全部责任。

## 发布包

GitHub 只发布两种联网 Lite 包：

- FPS Unlock 方案：`GenshinFSRBridge.Lite_v版本号.zip`
- 芙芙启动器插件方案：`FuFuLauncherPlugin.Lite_v版本号.zip`

两种包共用 Bridge、OptiScaler 默认配置和 ReShade 默认配置，但使用不同的注入方式：

- FPS Unlock 方案通过 Genshin FPS Unlock 按顺序加载 Bridge、OptiScaler、反虚化插件和 ReShade。
- 芙芙方案安装为 `FSR-Bridge-Plugin`，由芙芙启动器加载 Bootstrap，再由 Bootstrap 加载同一插件目录中的 Bridge、OptiScaler 和 ReShade。芙芙启动器已提供反虚化相关功能，因此该包不包含 `AntiPlayerMosaic`。

Lite 包不内置 FPS Unlocker、OptiScaler 或 NVIDIA DLSS 组件。安装器会固定获取非帧生成版使用的 OptiScaler 0.9.3，并在 NVIDIA 显卡需要 DLSS 时从 NVIDIA 官方 Streamline 发布包补全对应组件。

`frame-generation` 分支保留基于 OptiScaler 0.10.0-pre1 的帧生成实现，不进入 `main` 的 Lite 发布流程。

## 使用方法

从 [Releases](https://github.com/AizawaHikaru233/genshin_fsr_brigde/releases) 下载对应 Lite 压缩包并完整解压：

- FPS Unlock 包运行 `一键配置.bat`，也可运行 `GenshinFSRBridgeTools.bat` 使用英文界面。
- 芙芙包运行 `安装到芙芙启动器.bat`，并手动选择 `FufuLauncher.exe` 所在目录。

两套管理脚本都支持安装、卸载、组件更新和脚本自身更新，并保存上次由用户确认的安装位置。

Bridge 本身不执行 FSR、DLSS、XeSS 或其他超分算法。它只暴露标准 FSR2 接口并转接原神的 DX11 上采样输入，实际后端由 OptiScaler 负责。

## 功能

- 接管原神原生 TAAU/FSR 上采样路径并提供标准 FSR2 ABI。
- 向外部超分后端传递颜色、深度、运动矢量、抖动和历史资源。
- 动态读取输入与输出分辨率，不硬编码 0.6、0.8 或 0.9 渲染精度。
- 正式版只保留基础诊断日志，每次重新运行覆盖上一轮日志；单次运行不限制日志大小。
- 正式版不集成逐帧探针、资源导出或实验渲染路径。

## 仓库结构

- `Dx11FsrBridge/`：Bridge 源码、配置、技术文档和构建依赖。
- `FufuGraphicsPlugin/`：芙芙启动器 Bootstrap 插件与安装脚本。
- `GenshinOneClick/`：FPS Unlock 交互安装器、组件清单和 Lite 资源模板。
- `AntiPlayerMosaic/`：反虚化、隐藏 UID 与水下马赛克修复插件。
- `SharedResources/`：两种安装方案共用的全新 OptiScaler 与 ReShade 默认配置模板。
- `RenoDX-Genshin/`：经授权归档的 RenoDX Add-on。
- `Build-OnlineInstaller.ps1`：一次构建两种 Lite 包并生成 GitHub Release 英文别名。

## 构建

需要 Visual Studio 2022（含 C++ 桌面开发组件）、Windows SDK、CMake 3.20 或更新版本，以及仓库中声明的第三方构建依赖。

构建 Bridge：

```powershell
cmake -S .\Dx11FsrBridge -B .\build-package-bridge -A x64 `
  -DDX11FSRBRIDGE_RELEASE_RUNTIME=ON `
  -DDX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL=ON
cmake --build .\build-package-bridge --config Release
```

在 `GenshinOneClick/payload` 中准备好自有 DLL 与允许分发的 Lite 资源后，一次生成两种在线包：

```powershell
powershell -ExecutionPolicy Bypass -File .\Build-OnlineInstaller.ps1 -Configuration Release
```

中文本地包输出到 `dist/`，GitHub Release 英文别名输出到 `dist/github-release/`。GitHub Actions 需要从 Actions 页面手动触发；选择发布并填写标签后，会将两个英文别名上传到对应 Release。

## 日志与反馈

正式发行版默认只记录基础错误。复现问题后请不要再次启动游戏，并根据所用方案保留：

- `Dx11FsrBridge.log`
- `OptiScaler.log` 与 `OptiScaler.ini`
- 安装器生成的最后一次错误日志（若存在）
- 显卡型号、游戏版本、异常发生阶段和选择的超分模式

不要把游戏账号、登录信息或包含个人信息的截图提交到公开 Issue。

## 第三方组件与许可证

- OptiScaler：<https://github.com/optiscaler/OptiScaler>
- Genshin FPS Unlock：<https://github.com/34736384/genshin-fps-unlock>
- NVIDIA Streamline：<https://github.com/NVIDIA-RTX/Streamline>
- ReShade：<https://reshade.me/>
- ReShade HDR shaders：<https://github.com/EndlesslyFlowering/ReShade_HDR_shaders>

各可分发资源的许可证和来源声明随对应目录或发布包保留。本项目 Bridge 源码采用 [GPL-3.0-or-later](Dx11FsrBridge/LICENSE)。
