# 原神 FSR Bridge 交互安装器

正式版只提供联网精简版 `GenshinOneClick-Online`：不包含 FPS Unlocker 和 OptiScaler，安装时从官方 GitHub 下载或由用户手动选择。

发布包包含：

1. `Dx11FsrBridge.dll`
2. `AntiPlayerMosaic.dll` 反虚化与隐藏 UID
3. 官方 ReShade Add-on 版与 HDR 着色器运行文件
4. 交互安装与配置脚本

双击 `一键配置.bat` 后会进入两层命令行管理界面。本地不存在 FPS Unlocker 或 OptiScaler 时，安装器才会询问联网下载或手动选择。

首次运行先选择游戏目录：可以直接输入游戏目录或游戏 EXE 路径，也可以不输入直接按回车打开目录选择窗口。有效路径保存在 `.installer-state.json`，下次运行自动读取；缓存失效时会重新进入选择页面。

主界面会显示当前游戏目录、当前插件目录、强制基础组件 FPS Unlocker，以及三个可选插件的完整路径和安装状态，并提供安装、卸载、更换游戏目录、修改帧率和启动游戏等选项。

- 首次安装 FPS Unlocker 后会要求设置帧率上限，直接按回车默认使用 60
- 安装时输入 `A` 或直接按回车：安装全部三个可选插件
- 卸载时输入 `A` 或直接按回车：卸载全部三个可选插件，保留 FPS Unlocker
- 输入顶部表格中的模块 ID `2`、`3` 或 `4`：处理指定插件
- 输入 `2`：FSR Bridge + OptiScaler
- 输入 `3`：反虚化 / 隐藏 UID
- 输入 `4`：ReShade HDR
- 输入 `0`：返回上一层

需要下载的组件提供三种方式：联网安装、检查更新和手动安装。手动安装可以输入本地文件或解压目录；直接按回车会打开文件选择窗口。地址无效或关闭选择窗口时会提示未找到目录或文件，并返回安装方式菜单，不会退出管理器。

FPS Unlocker 是强制基础组件。选择游戏目录后若检测到它尚未就绪，管理器会先要求自动下载或手动导入，成功后才进入主界面；卸载向导不会卸载 FPS Unlocker。安装或卸载结束后会自动回到主界面，OptiScaler 尚未就绪时安装向导会继续询问使用官方下载还是本地文件。

安装和卸载页面使用相同的横向表格，显示模块 ID、插件名、作者、当前版本和安装状态。当前版本与安装状态分别显示，过长版本会在版本列的起始位置继续显示，不会自然换行到窗口最左侧。表格下方不重复显示模块列表，只需输入顶部对应的模块 ID。

主菜单提供“关于 / 作者主页”。同一作者的插件和脚本会合并显示，选择编号后由默认浏览器打开对应页面。`シリアCelia` 项包含 FSR Bridge、AntiPlayerMosaic 和安装管理脚本。

官方下载来源：

- FPS Unlocker：`https://github.com/34736384/genshin-fps-unlock/releases`
- OptiScaler：`https://github.com/optiscaler/OptiScaler/releases`

自动下载会通过 GitHub API 获取最新正式发行版，压缩包只保存在系统临时目录，安装完成后自动删除。运行所需文件会放到安装器自身目录下。若 GitHub 无法访问，可在浏览器中手动下载，然后在安装器中选择本地 `exe`、`zip`、`7z` 或完整解压目录。

ReShade 使用官方 Add-on 版 `6.7.3`，随发布包提供并保留 BSD-3-Clause 许可证。HDR 着色器来自 `EndlesslyFlowering/ReShade_HDR_shaders`，其 GPL-3.0 许可证会随包保留。

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
