# Genshin AntiPlayerMosaic

面向原神 Windows 客户端的运行时修复 DLL，用于处理角色相关马赛克、部分水下马赛克与 UID 隐藏。

本项目不是 HoYoverse 官方项目。它通过主模块代码签名动态定位目标位置，不依赖特定版本的固定 RVA；当签名无法唯一匹配时，对应功能会自动跳过并在日志中记录原因。

## 功能

- 动态扫描主模块的唯一代码签名，适配客户端地址变化。
- 修复已定位的角色透视与水下马赛克调用点。
- 在主线程定期隐藏已知 UI 路径中的 UID。
- 将初始化、扫描、补丁与降级结果写入 `AntiPlayerMosaic.log`。

## 使用与日志

将 DLL 通过兼容的外部加载器加载到游戏进程。加载后会在 DLL 同目录生成 `AntiPlayerMosaic.log`。

如某项功能失效，请在复现后提供日志、游戏版本和客户端区域。日志显示签名未解析或匹配数量不为 1 时，表示该版本需要更新签名；插件会保持未修补状态，而非写入不确定地址。

## 构建

需要 Visual Studio 2022（含 C++ 桌面开发组件）、Windows SDK 和 CMake 3.20 或更新版本。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

输出 DLL 位于 `build\Release`。`scan_test` 是离线签名扫描辅助程序，不是运行时依赖。

## 边界

本项目不包含、加载或调用 Snap Hutao 二进制文件、共享内存接口或固定 RVA 表。它也不包含游戏文件、ReShade、OptiScaler 或其他第三方 Mod。

## 许可证

本项目采用 [GPL-3.0-or-later](LICENSE)。你可以使用、修改和再分发代码；分发修改版本时必须同时提供对应完整源码，并以 GPL-3.0-or-later 发布。
