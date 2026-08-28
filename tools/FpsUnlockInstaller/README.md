# 原神 FSR Bridge 交互安装器

这是 FPS Unlock 注入方案的完整组件安装器。GitHub 发布包名为 `GenshinFSRBridge_v版本号.zip`，内置全部组件（桥、反虚化、ReShade 框架目录与 RenoDX Add-on、OptiScaler 全家桶、FPS Unlocker、默认配置模板），但**不内置 NVIDIA DLSS 与 ReShade64.dll**：两者由 Configure.ps1 首次配置时分别从 NVIDIA 官方 Streamline 发行版与 reshade.me 官方下载（GitHub 公开渠道保持合规；ReShade 官方明确"Do NOT share the binaries"）。本地/国内完整包 `原神解帧FSR插件包_v版本号.7z` 则连 DLSS 与 ReShade64.dll 一并内置。

完整包内置：

1. `Dx11FsrBridge.dll`
2. `AntiPlayerMosaic.dll`（反虚化、隐藏 UID 与水下马赛克修复）
3. 官方 ReShade Add-on 版与获得明确再分发授权的 RenoDX Add-on（闭源，授权凭据随包归档）
4. OptiScaler 0.9.4 及随包 FFX 2.3 SDK（FSR 4.1.1）/ XeSS / XeLL / D3D12Core 运行时与各自许可文件
5. OptiScaler 与 ReShade 的全新默认配置模板
6. 交互安装、卸载、组件更新和脚本自更新功能

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
- ReShade：<https://reshade.me/downloads>

上游组件由 `tools/Update-UpstreamComponents.ps1` 在构建时从上述官方来源获取并锁定版本基线（`upstream-versions.json` 随包内置）：OptiScaler 固定 0.9.4 正式版，DLSS / ReShade / FPS Unlocker 获取最新正式版。安装脚本按同一基线补齐缺失组件（不自由追 latest），旧包无基线文件时回退内置默认值。

本地/国内完整包内置 DLSS 与 ReShade64.dll（含各自许可文件）。GitHub 合规包不内置：首次配置检测到 RTX 显卡时从 NVIDIA 官方 Streamline 发行版下载 DLSS（验证 NVIDIA 数字签名后安装并保留许可证）；ReShade 二进制与效果库由安装器从 reshade.me 与各原作者官方仓库下载（SHA-256 校验、许可文件保留）。已有有效文件时不会覆盖；`nvngx_dlssg.dll` 和 `nvngx_dlssd.dll` 不在自动下载范围内。

## 配置与目录

OptiScaler 和 ReShade 各自从组件目录读取运行配置。不会随组件目录替换而删除的发布模板统一位于 `payload/default_config/`。

模板不使用维护者本机配置，不包含旧 preset、绝对用户路径或运行状态。首次安装时优先使用包内对应版本已有的官方配置作为模板；仅当资源中不存在所需模板时，安装器才创建最小默认文件。

配置器从自身目录动态计算完整路径并写入 FPS Unlock 的 DLL 列表；组件内部配置尽量只使用相对路径。OptiScaler 使用 `OptiDllPath=.` 与 `LogFileName=OptiScaler.log`，ReShade 使用相对于组件目录的着色器、纹理、Preset 和截图路径。只有游戏目录中的 ReShade `[INSTALL] BasePath` 重定向在跨目录或跨盘时使用动态绝对路径。发布包没有写死安装位置；移动整个目录后重新运行配置即可刷新重定向和注入路径。

## 渲染精度菜单

Bridge 将原神渲染精度菜单扩展为 `0.2 / 0.3 / 0.4 / 0.5 / 0.6 / 0.7 / 0.8 / 0.9 / 原生`。比例根据当前输出分辨率动态生效；例如 4K 输出下 `0.5` 为 `1920×1080`，`0.6` 为 `2304×1296`。

`原生` 对应游戏原本的 `1.0` 渲染精度。菜单打开事件只会启动一段有次数上限的标签扫描窗口，不会被连续 UI 调用反复刷新。

DLL 加载顺序固定为：

1. `ReShade64.dll`
2. `Dx11FsrBridge.dll`
3. `OptiScaler.dll`
4. `AntiPlayerMosaic.dll`

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

Bridge 的发行配置只保留正式超分路径、输入约定、兼容开关和渲染精度菜单所需设置。相似度采样、OSD、资源导出、热键探针和高数据量诊断会在 Release 构建中直接排除，而不只是写成关闭状态。

需要进一步排查时，可临时提高 OptiScaler 日志等级，但诊断结束后应恢复正式配置以避免额外开销。
不要把游戏账号、登录信息或包含个人信息的截图提交到公开 Issue。
