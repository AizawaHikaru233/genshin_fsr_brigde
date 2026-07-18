# 源码导航

## 运行模块

- `Dx11FsrBridge/`：原神 DX11 渲染链路的 FSR2 ABI 桥接层。
  - `Dx11FsrBridge.cpp`：D3D11 拦截、渲染状态跟踪、动态输入采集与运行日志。
  - `Fsr2TranslationLayer.*`：标准 FSR2 导出、输入准备和调度实现。
  - `Fsr31Bridge.*`：可选 FSR 3.1 实验后端；默认构建使用 `Fsr31BridgeStub.cpp`。
  - `OptiScalerNgxBridge.*`：可选 NGX 探测功能，不参与正式桥接链路。
  - `third_party/`：构建时所需的外部头文件与库。

- `AntiPlayerMosaic/`：反虚化、隐藏 UID 与水下马赛克修复插件。
  - `AntiPlayerMosaic.cpp`：DLL 生命周期、主线程回调、补丁写入和 UID 隐藏逻辑。
  - `PatternScanner.hpp`：按可执行代码段扫描唯一签名，避免使用固定 RVA。
  - `scan_test.cpp`：离线验证签名扫描结果的辅助工具。

## 构建与发布

- `Build-OnlineInstaller.ps1`：一次生成 FPS Unlock 与芙芙启动器两种 GitHub Lite ZIP。
- `SharedResources/OptiScaler/default_config/`：两种注入方案共用的官方 OptiScaler 默认配置与超分运行文件清单。
- `SharedResources/ReShade/default_config/`：两种注入方案共用的全新 ReShade 配置和空白 preset 模板。
- `GenshinOneClick/`：安装脚本与发布包源树。
- `dist/`、各模块的 `build*` 目录和 `artifacts/`：生成产物，不作为运行源码维护。

## 依赖边界

两个运行插件只依赖 Windows、Direct3D、Detours（FSR2 翻译层）与明确列出的 OptiScaler SDK/库。反虚化插件不加载、不调用也不分发 Snap Hutao 的 DLL、共享内存接口或固定偏移表。
