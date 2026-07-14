# 原神 FSR Bridge 交互安装器

正式版只提供联网精简版 `GenshinOneClick-Online`：不包含 FPS Unlocker 和 OptiScaler，安装时从官方 GitHub 下载或由用户手动选择。

发布包包含：

1. `Dx11FsrBridge.dll`
2. `AntiPlayerMosaic.dll` 反虚化与隐藏 UID
3. 官方 ReShade Add-on 版、RenoDX Add-on 与 HDR 着色器运行文件
4. 交互安装与配置脚本

双击 `一键配置.bat` 后会进入两层命令行管理界面；运行 `GenshinFSRBridgeTools.bat` 可直接进入 English 界面。主菜单也提供语言切换，选择会自动保存。本地不存在 FPS Unlocker 或 OptiScaler 时，安装器才会询问联网下载或手动选择。

首次运行先选择游戏目录：可以直接输入游戏目录或游戏 EXE 路径，也可以不输入直接按回车打开目录选择窗口。有效路径保存在 `.installer-state.json`，下次运行自动读取；缓存失效时会重新进入选择页面。

官方下载来源：

- FPS Unlocker：`https://github.com/34736384/genshin-fps-unlock/releases`
- OptiScaler：`https://github.com/optiscaler/OptiScaler/releases`
- NVIDIA DLSS 超分组件：`https://github.com/NVIDIA-RTX/Streamline/releases`

自动下载会通过 GitHub API 获取最新正式发行版，压缩包只保存在系统临时目录，安装完成后自动删除。运行所需文件会放到安装器自身目录下。若 GitHub 无法访问，可在浏览器中手动下载，然后在安装器中选择本地 `exe`、`zip`、`7z` 或完整解压目录。

一键脚本会在已安装 OptiScaler、检测到 NVIDIA 显卡且缺少 `nvngx_dlss.dll` 时，自动从 NVIDIA 官方 Streamline 最新正式包提取生产版 `bin/x64/nvngx_dlss.dll`，验证 NVIDIA 数字签名后安装并保留随包许可证。已有有效文件时不会覆盖。`nvngx_dlssg.dll` 和 `nvngx_dlssd.dll` 不在自动下载范围内，如有需要请自行从合法来源提供。

ReShade 使用官方 Add-on 版 `6.7.3`，随发布包提供并保留 BSD-3-Clause 许可证。HDR 着色器来自 `EndlesslyFlowering/ReShade_HDR_shaders`，其 GPL-3.0 许可证会随包保留。

`renodx-genshin.addon64` 的作者为 [剪刀妹丽丽](https://www.bilibili.com/video/av116861345793770/)。作者已明确允许本项目在 GitHub Release 安装包中再分发未修改的 Add-on 二进制文件；详细范围见 `payload\ReShade\reshade-shaders\NOTICE-RenoDX-genshin.txt`。

注入器和部分插件不可靠支持相对路径，因此配置器会从自身 `Configure.ps1` 所在目录动态计算完整绝对路径，再写入 `fps_config.json`、`OptiScaler.ini` 和游戏目录中的 `ReShade.ini`。发布包本身没有写死安装位置；移动整个目录后重新运行一次 `一键配置.bat` 即可刷新全部路径。旧版创建的 `GIUnifiedRuntime` 目录联接会在确认其确实为联接后自动删除。

DLL 加载顺序固定为：

1. `Dx11FsrBridge.dll`
2. `OptiScaler.dll`
3. `AntiPlayerMosaic.dll`
4. `ReShade64.dll`

安装器不会读取、修改或清理游戏登录信息，也不会写入原神注册表。已有 `fps_config.json` 时，只更新游戏路径、帧率上限和 DLL 列表，其他手动设置保持不变。桌面快捷方式固定命名为 `原神`。

安装完成后可直接运行 `unlockfps_nc.exe`、桌面上的 `原神` 快捷方式，或使用管理器主菜单的“启动游戏”。发布目录不再包含额外的启动批处理和路径修复脚本；移动目录后重新运行一次 `一键配置.bat` 即可刷新插件绝对路径。

无人值守示例：

```powershell
powershell -ExecutionPolicy Bypass -File .\Configure.ps1 `
  -GamePath "<游戏目录>\YuanShen.exe" `
  -FpsTarget 300 `
  -UnlockerSource Auto `
  -OptiScalerSource Auto `
  -NonInteractive
```

可选开关：`-DisableOptiScaler`、`-DisableAntiBlur`、`-DisableHDR`、`-NoShortcut`。手动导入可使用 `-UnlockerSource Manual -UnlockerPackagePath <路径>` 和 `-OptiScalerSource Manual -OptiScalerPackagePath <路径>`。

## 日志与问题反馈

正式版默认记录基础运行日志。遇到游戏无法启动、FSR 无法激活、切换超分后闪退或其他异常时，请在复现后不要再次启动游戏，并提供以下文件：

1. `payload\Bridge\Dx11FsrBridge.log`
2. `payload\OptiScaler\OptiScaler.log`
3. `payload\Bridge\Dx11FsrBridge.ini`
4. `payload\OptiScaler\OptiScaler.ini`
5. 简要说明显卡型号、异常发生阶段和当时选择的超分/帧生成模式

Bridge 默认记录加载、Hook、FSR2 转译和失败代码；OptiScaler 默认使用异步 `Info` 级文件日志，以保留初始化、依赖选择、超分和帧生成错误，同时避免 Trace/Debug 日志带来的明显额外开销。需要进一步排查时，可临时把 `OptiScaler.ini` 中 `[Log]` 的 `LogLevel` 改为 `1`（Debug）或 `0`（Trace）。
