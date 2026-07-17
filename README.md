# Genshin FSR Bridge

面向原神 Windows DX11 客户端的 FSR2 ABI 桥接 DLL。它在游戏进程中提供标准 FSR2 导出，并把游戏的上采样调用转接给外部兼容实现，例如 OptiScaler。

本项目不是 HoYoverse、AMD 或 OptiScaler 的官方项目；仓库和 Lite 包不分发 OptiScaler、FPS Unlocker 或 NVIDIA 运行库，安装器仅在用户运行时从官方来源下载或要求手动选择。

本仓库同时包含 `AntiPlayerMosaic/` 子项目。它是独立构建的原神马赛克修复与 UID 隐藏插件，具体用法见该目录的 README。

英文说明见 [README_EN.md](README_EN.md)。

## 支持范围与风险声明

- 目标支持原神中国服与全球服的 Windows DX11 客户端。
- 仅在 6.7（「月之八」，两者是同一版本的不同称呼）测试；不保证与其他游戏版本或客户端环境兼容。
- 本项目与 HoYoverse、miHoYo、《原神》及 Genshin Impact 均无关联，也未获得其认可或授权；相关名称与商标归其各自权利人所有。
- 使用第三方 DLL、注入器、Mod 或图形插件可能违反游戏规则，并可能导致账号限制或封禁。使用者须自行评估风险并承担全部责任。

## 帧生成分支

`frame-generation` 分支中的帧生成功能基于 `OptiScaler 0.10.0-pre1` 构建。低于 `0.10.0-pre1` 的 OptiScaler 不支持该帧生成功能。

## 非帧生成 Lite 发布包

`GenshinOneClick/` 包含 Lite 安装器、官方 ReShade Add-on DLL 和 HDR 着色器。OptiScaler、FPS Unlocker 与 NVIDIA DLSS 超分组件不进入仓库或打包产物，仅由安装器在运行时从各自官方来源下载。非帧生成版会选择 OptiScaler 0.9.3 正式版并锁定关闭帧生成。

两个自有 DLL 是编译产物，不提交到仓库。要生成与 GitHub Actions 相同的 Lite ZIP，请在 Windows 上运行：

```powershell
powershell -ExecutionPolicy Bypass -File .\Build-Package.ps1
```

构建结果位于 `dist\原神解帧FSR插件包Lite_v*.zip`。GitHub Actions 可从 Actions 页面手动触发，始终上传 ZIP 为 `GenshinOneClick-Lite` Artifact；勾选发布选项并填写版本标签时，会额外创建或更新对应 GitHub Release。

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

## 使用方法

从 [Releases](https://github.com/AizawaHikaru233/genshin_fsr_brigde/releases) 下载压缩包，解压后运行 `一键配置.bat` 并根据提示安装。脚本会按环境自动获取 [Genshin FPS Unlock](https://github.com/34736384/genshin-fps-unlock/releases)、[NVIDIA DLSS 超分组件（`nvngx_dlss.dll`）](https://github.com/NVIDIA-RTX/Streamline/releases) 和 [OptiScaler](https://github.com/optiscaler/OptiScaler/releases)，补全运行环境。

`Dx11FsrBridge.dll` 本身不执行 FSR、DLSS、XeSS 或其他超分算法。它只向外部工具暴露标准 FSR2 接口，并将游戏的 DX11 上采样调用转接到该接口。

也可以使用其他 DLL 注入工具，但必须保证它支持稳定的按序加载。

推荐加载顺序：

1. `Dx11FsrBridge.dll`
2. `OptiScaler.dll`
3. `AntiPlayerMosaic.dll`（可选）
4. `ReShade64.dll`（可选）

仅使用超分桥接时，前两项必须保持该顺序：Bridge 先加载，随后由 [OptiScaler](https://github.com/optiscaler/OptiScaler)（或同类工具）在启动时扫描标准 FSR2 导出并接管。Bridge 不直接加载、修改或捆绑 OptiScaler；后端选择、FSR3/FSR4 模型和其他 OptiScaler 配置均由用户自己的工具安装负责。

## 构建

需要 Visual Studio 2022（含 C++ 桌面开发组件）、Windows SDK 和 CMake 3.20 或更新版本。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DDX11FSRBRIDGE_RELEASE_RUNTIME=ON `
  -DDX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL=ON
cmake --build build --config Release
```

生成的 DLL 位于 `build\Release`。Bridge 的正式 mode2 配置固定在 DLL 内，不再读取 Bridge INI。发布配置需要仓库内的 `third_party` 目录，其中包含 FSR2 兼容 ABI 头文件和 Microsoft Detours 构建依赖。

`DX11FSRBRIDGE_ENABLE_FSR31_EXPERIMENTAL`、`DX11FSRBRIDGE_ENABLE_OPTISCALER_NGX_EXPERIMENTAL` 等 CMake 选项仅用于实验，不属于正式运行链路。

## 配置与反馈

正式发行版仅保留基础错误日志，并在每次启动时覆盖上一轮日志；请在复现问题后保留并提交：

- `Dx11FsrBridge.log`
- OptiScaler 的日志与配置（若使用）
- 显卡型号、游戏版本、异常阶段和选择的超分模式

不要把游戏账号、登录信息或包含个人信息的截图提交到公开 Issue。

## 第三方组件

- FSR2 ABI 头文件与 Microsoft Detours 仅作为构建依赖，保留各自原始许可证与声明。
- OptiScaler 是独立项目：<https://github.com/optiscaler/OptiScaler>。
- Lite 资源包含官方 ReShade Add-on DLL（BSD-3-Clause）、[剪刀妹丽丽](https://www.bilibili.com/video/av116861345793770/) 授权再分发的 RenoDX Add-on，以及 Lilium HDR 着色器（GPL-3.0）。各自许可证与授权记录位于资源目录。
- 仓库和 Lite 包不包含 OptiScaler、FPS Unlocker、NVIDIA 运行库或帧生成后端；安装器仅提供官方来源下载流程。

## 许可证

本项目采用 [GPL-3.0-or-later](LICENSE)。你可以使用、修改和再分发代码；分发修改版本时必须同时提供对应完整源码，并以 GPL-3.0-or-later 发布。
