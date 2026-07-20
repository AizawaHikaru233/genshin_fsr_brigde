# 原神 FSR Bridge 交互安装器

这是 FPS Unlock 注入方案的联网 Lite 安装器。GitHub 发布包名为 `GenshinFSRBridge.Lite_v版本号.zip`，不内置 FPS Unlocker、OptiScaler、NVIDIA DLSS 或 RenoDX 以外的 ReShade 效果库；安装时从官方来源下载，或在支持的组件上由用户选择本地压缩包、可执行文件或已解压目录。

Lite 包内置：

1. `Dx11FsrBridge.dll`
2. `AntiPlayerMosaic.dll`（反虚化、隐藏 UID 与水下马赛克修复）
3. 官方 ReShade Add-on 版与获得明确再分发授权的 RenoDX Add-on；HDR 着色器与依赖由安装器从官方上游下载
4. OptiScaler 与 ReShade 的全新默认配置模板
5. 交互安装、卸载、组件更新和脚本自更新功能

## 使用方法

完整解压后运行：

- `一键配置.bat`：中文界面
- `GenshinFSRBridgeTools.bat`：English interface

首次使用必须由用户选择游戏目录或 `YuanShen.exe`/`GenshinImpact.exe`。有效路径保存在 `.installer-state.json`，以后启动会自动读取；路径失效时会重新要求选择。

管理器的安装页面可分别选择安装或更新所需组件。主菜单还提供卸载、启动游戏、语言切换和安装器自身更新；语言与有效路径都会保存。

## 组件来源

- FPS Unlocker：<https://github.com/34736384/genshin-fps-unlock/releases>
- OptiScaler 0.9.4：<https://github.com/optiscaler/OptiScaler/releases/tag/v0.9.4>
- NVIDIA DLSS 超分组件：<https://github.com/NVIDIA-RTX/Streamline/releases>

自动下载通过 GitHub API 获取官方发布包。FPS Unlocker 与 NVIDIA DLSS 获取对应官方正式版，非帧生成包固定使用 OptiScaler 0.9.4。下载文件只保存在系统临时目录，安装完成后删除。

如果无法访问 GitHub，可以先在浏览器中下载，再在安装器里选择本地 `exe`、`zip`、`7z` 或完整解压目录。手动来源必须是独立资源，不能选择当前安装目录里的 `payload/OptiScaler` 作为来源。

检测到 NVIDIA 显卡且用户启用相应后端时，安装器可从 NVIDIA 官方 Streamline 包提取生产版 `nvngx_dlss.dll`，验证 NVIDIA 数字签名后安装并保留许可证。已有有效文件时不会覆盖；`nvngx_dlssg.dll` 和 `nvngx_dlssd.dll` 不在自动下载范围内。

## 配置与目录

OptiScaler 和 ReShade 各自从组件目录读取运行配置。不会随组件目录替换而删除的发布模板统一位于 `payload/default_config/`。

模板不使用维护者本机配置，不包含旧 preset、绝对用户路径或运行状态。首次安装时优先使用包内对应版本已有的官方配置作为模板；仅当资源中不存在所需模板时，安装器才创建最小默认文件。

配置器从自身目录动态计算完整路径并写入 FPS Unlock 的 DLL 列表；组件内部配置尽量只使用相对路径。OptiScaler 使用 `OptiDllPath=.` 与 `LogFileName=OptiScaler.log`，ReShade 使用相对于组件目录的着色器、纹理、Preset 和截图路径。只有游戏目录中的 ReShade `[INSTALL] BasePath` 重定向在跨目录或跨盘时使用动态绝对路径。发布包没有写死安装位置；移动整个目录后重新运行配置即可刷新重定向和注入路径。

## 渲染精度菜单

Bridge 将原神渲染精度菜单扩展为 `0.2 / 0.3 / 0.4 / 0.5 / 0.6 / 0.7 / 0.8 / 0.9 / 原生`。比例根据当前输出分辨率动态生效；例如 4K 输出下 `0.5` 为 `1920×1080`，`0.6` 为 `2304×1296`。

`原生` 对应游戏原本的 `1.0` 渲染精度。菜单打开事件只会启动一段有次数上限的标签扫描窗口，不会被连续 UI 调用反复刷新。

DLL 加载顺序固定为：

1. `Dx11FsrBridge.dll`
2. `OptiScaler.dll`
3. `AntiPlayerMosaic.dll`
4. `ReShade64.dll`

安装器不会读取、修改或清理游戏登录信息，也不会写入原神登录注册表。已有 `fps_config.json` 时，只更新游戏路径、帧率上限和 DLL 列表，其他兼容设置会尽量保留。

## 无人值守安装

```powershell
powershell -ExecutionPolicy Bypass -File .\Configure.ps1 `
  -GamePath "<游戏目录>\YuanShen.exe" `
  -FpsTarget 300 `
  -UnlockerSource Auto `
  -OptiScalerSource Auto `
  -NonInteractive
```

可选开关包括 `-DisableOptiScaler`、`-DisableAntiBlur`、`-DisableHDR` 和 `-NoShortcut`。手动导入可使用 `-UnlockerSource Manual -UnlockerPackagePath <路径>` 与 `-OptiScalerSource Manual -OptiScalerPackagePath <路径>`。

## ## 日志与问题反馈

Bridge、OptiScaler 和反虚化组件默认会保留错误日志。每次重新运行会覆盖上一轮日志。
遇到游戏无法启动、FSR 无法激活、切换超分后闪退或其他异常时，请在复现后不要再次启动游戏，并提供：

1. `payload/Bridge/Dx11FsrBridge.log` (必须)
2. `payload/OptiScaler/OptiScaler.log` (必须)
3. `payload/OptiScaler/OptiScaler.ini`
4. `payload/ReShade/ReShade.log`（涉及 ReShade 时）
5. `payload/AntiPlayerMosaic/AntiPlayerMosaic.log`（涉及反虚化、UID 或水下马赛克时）
6. 芙芙插件目录下的 `FSR-Bridge-Plugin.log`（使用芙芙启动器插件时）
7. 显卡型号、游戏版本、异常发生阶段和所选超分模式

Bridge 的发行配置只保留正式 Mode 4、输入约定、兼容开关和渲染精度菜单所需设置。相似度采样、OSD、资源导出、热键探针和高数据量诊断会在 Release 构建中直接排除，而不只是写成关闭状态。

需要进一步排查时，可临时提高 OptiScaler 日志等级，但诊断结束后应恢复正式配置以避免额外开销。
不要把游戏账号、登录信息或包含个人信息的截图提交到公开 Issue。
