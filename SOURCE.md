# 源码导航

## 运行模块

- `Dx11FsrBridge/`：原神 DX11 渲染链路的 FSR2 ABI 桥接层。
  - `Dx11FsrBridge.cpp`：D3D11 拦截、渲染状态跟踪、动态输入采集与运行日志。
  - `RenderScaleMenu.*`：渲染精度候选值、菜单标签和低于原生比例的应用逻辑；Release 只保留 3 个必要 Hook 与菜单事件触发的受限扫描，探针 Hook、硬件断点和手动热键不进入正式运行路径。
  - `Fsr2TranslationLayer.*`：标准 FSR2 导出、输入准备和调度实现。
  - `third_party/`：构建时所需的外部头文件与库。

- `AntiPlayerMosaic/`：反虚化、隐藏 UID 与水下马赛克修复插件。
  - `AntiPlayerMosaic.cpp`：DLL 生命周期、主线程回调、补丁写入和 UID 隐藏逻辑。
  - `PatternScanner.hpp`：按可执行代码段扫描唯一签名，避免使用固定 RVA。

## 构建与发布

- `Build-OnlineInstaller.ps1`：本地生成 FPS Unlock Lite、Full 与芙芙启动器完整包；GitHub Actions 只构建并发布 FPS Unlock Lite ZIP。
- `SharedResources/OptiScaler/runtime/`：与当前 OptiScaler runtime 配套的配置模板、运行库和超分运行文件清单。
- `SharedResources/ReShade/runtime/`：ReShade runtime、配置模板和授权资源。
- `tools/FpsUnlockInstaller/`：FPS Unlock 安装器、配置与自更新脚本。
- `assets/FpsUnlockPackage/`：FPS Unlock 包的反馈文案。
- `SharedResources/`：由构建脚本组装进发行包的运行资源与默认配置。
- `dist/`、各模块的 `build*` 目录和 `artifacts/`：生成产物，不进入仓库。

## 依赖边界

两个运行插件只依赖 Windows、Direct3D、Detours（FSR2 翻译层）与明确列出的 OptiScaler SDK/库。
