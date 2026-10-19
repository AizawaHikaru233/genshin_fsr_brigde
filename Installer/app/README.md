# Genshin FSR Bridge 安装器（Tauri 版）

用 FPS Unlocker、Dx11FsrBridge、OptiScaler、ReShade + RenoDX、TextureLoader、AntiPlayerMosaic
给原神配一套图形外挂链的本地安装器。组件本体全部留在 `payload\`，安装 = 把选中组件的绝对路径
写进 `payload\FpsUnlocker\fps_config.json` 的 `DllList`（注入链），游戏目录里一个 DLL 都不落。
界面是 `..\ui\index.html`（HTML/CSS/JS），外壳与后端是 Tauri + Rust。

## 构建

```powershell
.\build-app.ps1
```

产物：`src-tauri\target\release\gfsr-installer.exe`，并复制到 `app\GenshinFSRBridgeInstaller.exe`。
exe 必须能找到 `payload\`（同级，或上溯 1–3 级）——注入时按 `DllList` 里的绝对路径加载。

> 注意：`ui\` 的内容是**编译期嵌进 exe** 的。改完界面必须重新 `cargo build`，
> `build-app.ps1` 会顺手 touch 一下 `src/main.rs` 防止 cargo 跳过重编。

## 开发时直接跑

```powershell
$env:GFSR_PAYLOAD='<payload 目录>'          # 调试时 exe 在 target\debug 里，指回 payload
$env:WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS='--remote-debugging-port=9222'
.\src-tauri\target\debug\gfsr-installer.exe
```

CDP 驱动脚本在 `..\tools\wv.mjs`（`eval` / `shot` / `press` / `targets`）。

## 打成发布包（仓库流程）

仓库根目录的 `Build-OnlineInstaller.ps1` 默认就打这个 GUI 安装器（旧的 PS1 流程要显式加 `-ScriptInstaller`）：

```powershell
.\Build-OnlineInstaller.ps1
```

- 包根没有 `Installer.ps1` / `一键配置.bat` / `scripts\*.ps1`，换成 `GenshinFSRBridgeInstaller.exe`
  （默认取 `app\GenshinFSRBridgeInstaller.exe`，可用 `-GuiInstallerExe` 指定）；
  包根 `README.md` 用仓库主 README，不打包本文件；`assets\readme\` 的演示图会一起复制进包。
- 解帧工具从包根挪到 `payload\FpsUnlocker\unlockfps_nc.exe`（安装器原地启动它，`fps_config.json` 也在那儿）。
- 其余照旧：`payload\{AMD,Bridge,AntiPlayerMosaic,OptiScaler,ReShade,TextureLoader,default_config}`、
  `license\`、`Package-Version.txt`、`upstream-versions.json`；
  本地包与 GitHub 包只差打包范围，具体包含哪些组件按实际构建结果处理，缺的由安装器联网补齐。
- 产物：`dist\原神解帧FSR插件包_v<ver>.7z`（本地包，≤100 MB）与
  `dist\github-release\GenshinFSRBridge_v<ver>.7z`（GitHub，同样是 7z）。
- 本机跑打包时若 `vcvars64.bat` 起不来（cmd 报 “\Windows was unexpected at this time”），
  先在当前 shell 里补 `LIB` / `INCLUDE`（MSVC lib\x64 + Windows Kits `Lib\<ver>\{ucrt,um}\x64`），
  脚本检测到 `LIB` 已存在就会跳过自己的 vcvars 导入。

## 目录

| 路径 | 作用 |
| --- | --- |
| `src-tauri\src\main.rs` | 前后端协议：`init/scan/pickFolder/save/install/launch/stop/window/openPath/appearance/switchVersion/modsDir/pickMods/pickCustomDll/removeCustomDll/restoreDefaults/fetchComponent/openLog/bundleLogs/checkUpdate/applyUpdate/openUrl/setShortcut` |
| `src-tauri\src\pkg.rs` | 组件清单、payload 内路径、必装项补全、注入链顺序、版本目录扫描 |
| `src-tauri\src\install.rs` | 各组件 ini 写入（Bridge / OptiScaler / ReShade）+ 刷新 `fps_config.json` |
| `src-tauri\src\launch.rs` | 在 `payload\FpsUnlocker` 原地拉起 `unlockfps_nc.exe`、读写 `fps_config.json` |
| `src-tauri\src\ini.rs` | ini 读改写（保留原有键、注释与换行风格） |
| `src-tauri\src\gpu.rs` | 显卡判定（FSR4 档位：FP8 / INT8 / 不支持） |
| `src-tauri\src\gamepath.rs` | 注册表 + 常见目录扫描、目录校验 |
| `src-tauri\src\backdrop.rs` | Mica / 亚克力 / 纯色窗口材质 |
| `src-tauri\src\fetch.rs` | 系统代理、镜像测速、下载、7z/zip 解压、从 Releases 补缺件 |
| `src-tauri\src\update.rs` | 插件包自更新：查版本、下整包、备份/回填 ini、切版本目录 |
| `src-tauri\src\logs.rs` | 组件日志清单与打包 |
| `src-tauri\src\shortcut.rs` | 桌面快捷方式：读 User Shell Folders、经 WScript.Shell 建/删/读目标，只动自己建的那个 |
| `src-tauri\capabilities\default.json` | 主窗口权限（缺了它前端收不到事件） |

## 打包结构

与旧脚本安装包一致：exe 在包根目录，组件全在 `payload\`，`Package-Version.txt` 是包版本。

```
GenshinFSRBridge\
  GenshinFSRBridgeInstaller.exe
  Package-Version.txt
  payload\
    FpsUnlocker\unlockfps_nc.exe        ← 解帧工具原地运行，fps_config.json 也在这里
    Bridge_v2.2.0\Dx11FsrBridge.dll
    OptiScaler_v0.9.4\OptiScaler.dll
    ReShade\ReShade64.dll
    TextureLoader\…
    AntiPlayerMosaic\…
    AMD\amd_fidelityfx_upscaler_dx12.dll ← Bridge 的 FSR2/3 回退后端
    default_config\…                     ← ini 模板
```

### 版本目录（Bridge / OptiScaler）

`payload\` 里以 `Bridge` / `OptiScaler` 开头的文件夹都算一个可选版本，
在「插件配置」页可切换，切换后立刻改写 `fps_config.json` 的 `DllList` 与该版本的 ini。

- OptiScaler 0.9.x 是**平铺**布局：`OptiScaler_v0.9.4\OptiScaler.dll` 与
  `amd_fidelityfx_upscaler_dx12.dll` 同级，`OptiDllPath` 写版本目录本身。
- OptiScaler 0.10 起是**嵌套**布局：DLL 在 `OptiScaler_v0.10.0\OptiScaler\` 下，
  ini 仍在版本目录根，`OptiDllPath` 写 `…\OptiScaler_v0.10.0\OptiScaler`。
  写错这一项会出现「注入成功但找不到超分后端」，所以由安装器按标记文件
  `amd_fidelityfx_upscaler_dx12.dll` 的所在位置自动判定。

## 解帧工具选项

`payload\FpsUnlocker\fps_config.json` 里除 `GamePath` / `FPSTarget` / `DllList` / `UseHDR` 之外的
可调项，在「插件配置」页的「解帧工具选项」卡里改，改完立刻写盘（消息 `unlockerConfig`）。

- 后端只改被点的键（白名单：`AutoStart` `AutoClose` `StartMinimized` `UsePowerSave` `PopupWindow`
  `Fullscreen` `IsExclusiveFullscreen` `CustomResX` `CustomResY` `UseMobileUI` `SuspendLoad`
  `MonitorNum` `Priority`），其余键（含 `GamePath`、`DllList` 与上游预留字段）原样保留。
- 互斥关系与上游一致：开无边框就关全屏，开全屏就关无边框，独占全屏只在全屏开启时可用。
- `UseHDR` 是只读的——它等于注入链里有没有 `ReShade64.dll`。
- 没有 `fps_config.json`（还没完成过一次安装）时整卡置灰。

## TextureLoader 的 Mods 目录

`payload\TextureLoader\TextureLoader.ini` 的 `mods_dir` 在「插件配置」页选目录改（消息 `pickMods` /
`modsDir`），选完立刻写盘，重启游戏生效：

- 留空 = 用 DLL 同目录的 `Mods`（TextureLoader 自己会建空目录），文件名是 `payload\TextureLoader\Mods`。
- 写的是绝对路径，`TextureLoader` 递归扫描该目录下的 `ini`。
- 只动 `mods_dir` 这一行，注释与其他键原样保留（空值写回 `mods_dir =`，文件字节与出厂一致）。

## 自定义注入（任意 DLL）

「主页 → 安装内容」最下方一行：选一个 `.dll` 就把它**追加到注入链末尾**（消息 `pickCustomDll`），
已加入的行和自带插件一样有一个勾选框，右上角是状态标记与「删除」（消息 `removeCustomDll`）。

- 路径存在 `settings.json` 的 `customDlls`（绝对路径，去重按小写比较），勾选状态存在 `customOn`；
  `customOn` 缺省（老配置）视为全开，`pkg::dll_list()` 在固定五件套之后按列表顺序追加，未勾选或文件不存在就跳过。
- 勾选/取消勾选发 `{type:'setCustomOn', path, on}`：只改注入链，**文件不动**（行上标「未启用」而不是「已删除」）。
- 加/删/勾选之后立刻重写 `fps_config.json`（`DllList`）并回推 state；加进来的项默认是勾上的。
- 自定义项不参与「安装内容」的已安装计数，只在表头附一句「N 个自定义注入」（只数勾上的）。
- FPS Unlocker 不出现在清单里：它本身不是被注入的插件，只在底层保证就位。

## 缺件联网补齐

组件文件缺失时（例如只拿走了一份 `GenshinFSRBridgeInstaller.exe`），插件配置里那一行会显示
「缺少文件」并在右侧出现「下载」按钮，点它当场补齐，不需要重下整包。

> **启动时自愈**：每次打开安装器，`setup()` 里会跑一遍 `reapply()` —— 按当前设置重写
> OptiScaler 的 `OptiDllPath`、ReShade 的 `AddonPath` / `EffectSearchPaths` / `TextureSearchPaths`、
> 以及解帧工具 `fps_config.json` 的注入链 `DllList`，顺手把已经不存在的条目清掉。
> 组件文件本身不复制；没有游戏目录或 payload 时只写一行日志跳过。

- **只补缺的**：目标文件已存在就跳过，用户自己换过版本不会被覆盖。
- **ReShade**：从 reshade.me 下官方安装器（PA 里挂了 zip），按 512 字节对齐扫 `PK\x03\x04`
  定位内嵌压缩包，取出 `ReShade64.dll`。
- **其它组件**：取本项目 GitHub Releases 的最新整包（优先 `.7z`，其次 `.zip`），解压后按目录前缀
  （`Bridge*` / `OptiScaler*` / `ReShade` / `TextureLoader` / `AntiPlayerMosaic` / `FpsUnlocker`）
  与 `default_config` 把缺的文件搬进 `payload`。
- **跟随系统代理**：读 `HKCU\Software\Microsoft\Windows\CurrentVersion\Internet Settings` 的
  `ProxyEnable` / `ProxyServer` / `ProxyOverride`，与浏览器、.NET 用的是同一份设置；
  `ProxyOverride` 命中的主机直连。注意 WinHTTP 的设置是另一套（本机就是直连），不能用它。
- **镜像测速**：GitHub 走旧脚本那套线路（直连 + `ghfast.top` / `gh-proxy.com` / `ghproxy.net`），
  下载前先各拉 1 MB 量速度，用最快的那条，失败再按速度降级。底栏会显示「正在测速选择下载线路…」
  和实时进度。

解压用 `sevenz-rust`（7z）与 `zip`（zip），不依赖系统里的 `curl` / `7z` ——
Windows 的 `curl.exe` 并不保证存在（本机 `System32` 里就没有）。`fetch.rs` 里的单测可用
`GFSR_TEST_URL`（下载）与 `GFSR_TEST_7Z`（真实 7z 包）验证链路。

## 插件恢复默认设置

设置页有一个「插件恢复默认设置」（按钮 `恢复…`，点两次才执行），消息 `restoreDefaults`。

基线来自 `payload\default_config\`：

| 文件 | 来源 | 恢复时覆盖到 |
| --- | --- | --- |
| `Dx11FsrBridge.ini` / `Bridge.ini` | 打包时复制自 `payload\Bridge\` | 当前启用版本目录里的 `Dx11FsrBridge.ini` |
| `OptiScaler.ini` / `<版本目录>.ini` | 打包时复制自 `payload\OptiScaler\` | 当前启用版本目录里的 `OptiScaler.ini` |
| `ReShade.ini` / `ReShadePreset.ini` | 打包时从 ReShade 运行时复制 | 游戏目录里的同名文件 |
| `TextureLoader.ini` | 打包时复制自 `payload\TextureLoader\` | `payload\TextureLoader\TextureLoader.ini` |

- 版本目录名跟模板名不一致时（`Bridge` 目录 vs `Dx11FsrBridge.ini`）打包脚本会另存一份
  `default_config\Bridge.ini`；用户自己加的版本目录（如 `OptiScaler_v0.10.0`）在**第一次切到它时**
  自动留一份 `default_config\<目录名>.ini`，已存在就不动，避免把用户改过的内容当成默认值。
- 恢复只覆盖 ini，随后立刻重跑一次写入（托管键 `OptiDllPath` / `Ffx12AsyncUpscale` /
  ReShade 的三个搜索路径按当前环境重算）并重写注入链——基线里的路径是打包时的相对布局，
  不能直接留在用户机器上。
- `settings.json` 的 `modsDir` 一并清空（它对应 `TextureLoader.ini` 的 `mods_dir`，文件已还原）。

## 组件日志

设置页的「组件日志」列出四个来源，每行点「打开」直接用默认程序打开日志；右上角「打包全部…」
弹保存对话框，把所有日志打成一个 zip。协议：`openLog`（字段 `id`）、
`bundleLogs`（可选字段 `path`，给了就跳过对话框，便于脚本/测试）；日志清单随 `state` 的 `logs` 下发。

| id | 组件 | 路径 | 文件 |
| --- | --- | --- | --- |
| `installer` | 安装器 | `%LOCALAPPDATA%\GenshinFsrBridge\` | `installer.log`（超 2 MB 轮转成 `installer.old.log`） |
| `bridge` | Dx11FsrBridge | 当前启用版本目录 | `Dx11FsrBridge.log`（轮转 `.1`/`.2`） |
| `opti` | OptiScaler | 当前启用版本目录 | `OptiScaler.log`（`SingleFile` 关闭时是 `OptiScaler_<ticks>.log`） |
| `texture` | TextureLoader | `payload\TextureLoader\` | `TextureLoader.log`、`TextureLoader.crash.log` |

- 三个插件都把日志写在自己 DLL 旁边（Bridge 传 `g_module_dir`、TextureLoader 传 `dll_dir`、
  OptiScaler 相对 LogFileName 落在 `DllPath().parent_path()`），所以不需要进游戏目录找。
- 一行里代表该来源的是**主文件** `<前缀>.log`；轮转件与 crash 日志只计入数量并一起打包。
  一个都没有时按钮置灰，提示「还没有生成日志，启动一次游戏再看」。
- 打包按 `来源id/文件名` 分层放进 zip；四个来源全都没有日志时回报错并删掉空包。

## 检查更新（插件包自更新）

关于页「插件包」一行显示 `Package-Version.txt` 的版本，按钮「检查更新」弹窗：

- 第一行：查 GitHub Releases 的最新 tag，比本地版本（数字段逐位比较，`v2.3.1` 与 `2.3.1` 视为相同）。
  有新版才启用「立即更新」。
- 「立即更新」：下载整包（`.7z` 优先于 `.zip`）→ 解压 → 找到 `payload` → **先把正在用的 ini
  备份到临时目录**（游戏目录 `ReShade.ini`/`ReShadePreset.ini`、当前 Bridge 版本目录
  `Dx11FsrBridge.ini`、当前 OptiScaler 目录 `OptiScaler.ini`、`payload\TextureLoader\TextureLoader.ini`）
  → 覆盖 `payload\` 与根文件（`Package-Version.txt`/`upstream-versions.json`/`README.md`/`Feedback.txt`/`license\`，
  不覆盖正在运行的安装器 exe）→ 把 `bridgeDir`/`optiDir` 切到新包里版本号最大的目录 → 回填备份的 ini
  → 再跑一次 `install::apply` 重算托管键与注入链。
- 第二行「手动更新」：GitHub Releases、蓝奏云【CN】、夸克网盘【CN】三个按钮用默认浏览器打开
  （协议 `openUrl`，只放行 http/https）。

协议：`checkUpdate`（回 `{type:"update", phase:"checking"|"checked", info}`）、
`applyUpdate`（回 `phase:"working"` 带 `message` 进度，结束 `phase:"done"|"error"`）、`openUrl`（字段 `url`）。
查询与下载都走系统代理和加速线路，见上一节。更新不可中途取消，界面在写入期间不允许关闭弹窗。

## 语言与主题

- 界面语言（`uiLang`）与明暗主题（`theme`）都存进 `settings.json`，取值 `system` / `zh` / `en` 与 `system` / `light` / `dark`，默认 `system`。
- `system` 时：界面语言看 `navigator.language`（中文系显示中文，其它显示英文）；明暗看 `prefers-color-scheme`，一有变化就通过 `prefs` 消息告诉后端，后端用 `DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE)` 让窗口边框和阴影一起变深。
- 英文文案由前端字典（`ui/index.html` 里的 `EN_EXACT` / `EN_PAT`）在渲染后改写文本节点，`MutationObserver` 盯着新插入的节点继续翻；切回中文时按记录的原文还原。后端日志、`fps_config.json`、组件 ini 一律保持原样，不进翻译。
- 设置页「界面语言」「明暗主题」两组分段控件写的就是这两个字段，改完立刻生效并落盘。
- 协议新增 `prefs`（`{type:"prefs", uiLang, theme, dark}`），后端据此保存并应用深色边框。

## 桌面快捷方式

- 安装组件成功后，如果 `desktopShortcut`（默认 `true`）开着，就在桌面建一个快捷方式：
  - 目标 = `payload\FpsUnlocker\unlockfps_nc.exe`，工作目录 = `payload\FpsUnlocker`，图标借用游戏本体的 `YuanShen.exe`。
  - 名字跟随界面语言：中文环境 `原神.lnk`，英文环境 `Genshin Impact.lnk`；切换语言时已存在的会自动改名。
- 桌面目录优先读 `HKCU\...\Explorer\User Shell Folders` 的 `Desktop`（OneDrive 重定向也能拿到），读不到回落 `%USERPROFILE%\Desktop`。
- 创建走系统 `WScript.Shell` COM（PowerShell 拉起，`CREATE_NO_WINDOW` 不闪黑窗），不引入额外依赖。
- **只动自己建的快捷方式**：删除或改名之前先用 `WScript.Shell` 读目标，目标不是 `unlockfps_nc.exe` 的一律不碰（用户自己放的「原神.lnk」不会被覆盖）。
- 设置页「桌面快捷方式」开关发 `{type:'setShortcut', enabled}`：开=立刻建，关=删掉我们建的那些；协议 `setShortcut`。

## 与其他启动器联动

关于页有一张卡：上面是解帧工具目录（带「复制路径」），下面每个启动器一行，行内直接写怎么填，不再重复路径、也不再给第二第三个复制按钮。

- Starward：原神 - 游戏设置 - 启动参数 - 自定义启动程序，选中上面目录里的 `unlockfps_nc.exe`（英文用官方译法 Genshin - Game Setting - Startup Arguments - Custom Startup Executable）。
- BetterGI：启动选项卡 - 同时启动原神 - 原神安装位置，同样选中 `unlockfps_nc.exe`（英文用本项目的译法）。
- 芙芙启动器（FuFuLauncher）：插件已上架插件商城，在商城装「FSR-Bridge-Plugin（原神FSR2桥接插件）」后用芙芙启动即可，不需要填路径。
- `state` 字段：`desktopShortcut` / `unlockerDir` / `shortcutName`。

## NVIDIA DLSS 运行时（RTX 显卡 + OptiScaler）

- **触发条件**：勾了 `OptiScaler` 且检测到 RTX 架构 N 卡（`gpu::has_rtx()`：设备 ID 属 RTX 世代且不是 GTX 16，或型号名含 `RTX`）。GTX 16 只吃 FSR4 INT8，不部署 DLSS。
- **放在哪**：`nvngx_dlss.dll` 写进 OptiScaler 的**组件目录**（嵌套布局下是 `OptiScaler_v0.10.0\OptiScaler\`），因为 ini 的 `NvngxDlssPath = auto` 相对 `OptiDllPath` 解析，放根层 N 卡上会找不到 DLSS。同目录带一份 `nvngx_dlss.license.txt`。
- **从哪来**：先看 `payload\NVIDIA\DLSS\` 里有没有现成的；没有就从 NVIDIA 官方 `NVIDIA-RTX/Streamline` 最新 release 的 `streamline-sdk-v*.zip` 取，挑 `bin\x64\nvngx_dlss.dll` 生产版（跳过 `development` 目录）。下载复用系统代理 + 加速线路 + 测速。
- **时机**：安装/保存写入链之后、切换 OptiScaler 版本之后，后台线程跑，不挡界面；结果写 `installer.log`（`[dlss] …`），失败只提示不阻断安装。
- 自测：`GFSR_TEST_DLSS=1 cargo test --bins dlss_probe -- --nocapture`（会真的下 ~257 MB 的 Streamline 包）。

## 初始勾选与显卡兼容性提示

- **初始不勾选任何选装组件**：只有必装项（解帧工具、`Dx11FsrBridge`）进注入链，`OptiScaler` /
  `ReShade` / `TextureLoader` / `AntiPlayerMosaic` 都要用户自己勾。
- **没选游戏目录就不显示已安装**：行尾的「已安装 / 未写入」需要真实存在 `YuanShen.exe` 才算数，
  包内残留的 `fps_config.json` 不会让组件看起来装过（`post_state` 里的 `have_game` 判定）。
- **N 卡提示**（勾选时弹窗，两个按钮：`确认` 直接勾上、`不再提醒` 勾上且记住）：
  - 勾 `TextureLoader`：NVIDIA 显卡加载纹理贴图极大概率导致贴图渲染丢失（也有小概率正常）。
  - 勾 `ReShade` 且 `OptiScaler` 也选着：两者同时启用可能不稳定。
  - 关掉弹窗（❌ / Esc / 点遮罩）＝保持不勾选。
  - 「不再提醒」记在 `settings.json` 的 `suppressWarnings`，按条目（`nvidia-texture` /
    `nvidia-reshade-opti`）分别记住。
- 显卡厂商与 FSR4 策略来自同一个 `gpu::info()`（开机算一次并缓存）：`install::apply` 写
  `OptiScaler.ini` 的 `Fsr4Update` / `UpscalerIndex` / `Fsr4ForceEnableInt8` 用它，界面提示也用它的
  `vendor`，不会出现两边判断不一致。
