# 源码导航

## 运行模块

- `Dx11FsrBridge/`：原神 DX11 渲染链路插件（独立 hook 游戏原生 FSR2 并转接 AMD FFX12 SDK 超分）。
  - `Dx11FsrBridge.cpp`：D3D11 拦截、渲染状态跟踪、动态输入采集、FFX12 接管调度与运行日志。
  - `Ffx12Backend.*`：FFX12 SDK 后端（自建 D3D12 设备 + 共享纹理 GPU 互操作、版本前缀匹配与 dispatch）。
  - `RenderScaleMenu.*`：渲染精度候选值、菜单标签和低于原生比例的应用逻辑；Release 只保留 3 个必要 Hook 与菜单事件触发的受限扫描（RVA 与特征签名硬编码于源码，不读 INI）。
  - `Fsr2FamilyTakeover.cpp` / `Il2CppCallSiteHook.cpp`：合成族接管状态机与 il2cpp 调用点挂钩。
  - `third_party/`：构建时所需的外部头文件与库。

- `AntiPlayerMosaic/`：反虚化、隐藏 UID 与水下马赛克修复插件。
  - `AntiPlayerMosaic.cpp`：DLL 生命周期、主线程回调、补丁写入和 UID 隐藏逻辑。
  - `PatternScanner.hpp`：按可执行代码段扫描唯一签名，避免使用固定 RVA。

- `FufuGraphicsPlugin/`：芙芙启动器插件（bootstrap：启动时初始化 OptiScaler 配置并按 DXGI Device-ID 分类 FSR4 策略、补齐 DLSS 依赖）与商城/本地测试 Lua 安装脚本。

## 构建与发布

- `tools/Update-UpstreamComponents.ps1`：更新上游组件到 `SharedResources/` 基线（OptiScaler 固定 v0.9.4；DLSS、ReShade、FPS Unlocker 获取各自最新正式版并做 SHA-256 校验），生成 `upstream-versions.json` 随包分发。
- `Build-OnlineInstaller.ps1`：以 Ninja 生成器自动编译三个自有 DLL 并组装发行包：本地完整包（`dist\原神解帧FSR插件包_v*.7z`）、GitHub 发布包（`dist\github-release\GenshinFSRBridge_v*.zip`，`-GithubOnly`）与芙芙启动器插件包（`dist\FSR-Bridge-Plugin.v*.zip`）；`-FetchUpstream` 在打包前先自动更新上游组件。
- `SharedResources/OptiScaler/runtime/`：OptiScaler 官方第一手运行库与配置模板（`OptiScaler.ini` 为上游原版模板，关键键由安装脚本的托管设置覆写）及 curated 运行文件清单。
- `tools/FpsUnlockInstaller/`：FPS Unlock 安装器、配置与自更新脚本（含 `Set-OptiScalerManagedSettings` 托管配置写入）。
- `assets/FpsUnlockPackage/`：FPS Unlock 包的反馈文案。
- `dist/`、各模块的 `build*` 目录：生成产物，不进入仓库。

## 依赖边界

运行插件只依赖 Windows、Direct3D、Detours、FFX12 ffx-api 头文件与明确列出的 OptiScaler SDK/库。
