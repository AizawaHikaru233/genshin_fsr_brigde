# Genshin FSR Bridge 安装器

原神解帧 / FSR 插件链的图形安装器（Tauri 2 + Rust + WebView2）。

安装器本身只做三件事：

1. 找到原神安装目录；
2. 把组件（FPS Unlocker、Dx11FsrBridge、OptiScaler、ReShade + RenoDX、TextureLoader、
   AntiPlayerMosaic）的绝对路径写进 `payload\FpsUnlocker\fps_config.json` 的 `DllList`；
3. 按需改写各组件的 ini。

组件文件始终留在 `payload\`，游戏目录里不会出现任何 DLL。

## 目录

```
Installer\
  app\
    build-app.ps1                构建脚本（cargo build --release + 复制 exe）
    README.md                    构建、协议、功能说明（详细文档在这里）
    src-tauri\                   Rust 后端 + Tauri 配置
  ui\                            界面（HTML/CSS/JS，编译期嵌进 exe）
    index.html
    fonts\misans\                MiSans 可变字体（Apache-2.0）
  tools\
    wv.mjs                       CDP 调试脚本（eval / shot / press / targets）
    shoot-window.ps1             抓原生窗口截图（DPI 感知）
```

`payload\` 不在仓库里：本地需要时从发布包（7z/zip）解出来放到 `Installer\payload\`，
或用 `GFSR_PAYLOAD` 指向任意一份 payload 目录。

## 构建

```powershell
cd Installer\app
.\build-app.ps1
```

产物 `app\GenshinFSRBridgeInstaller.exe`。仓库根的打包装脚本
（`Build-OnlineInstaller.ps1`）默认就用这个安装器出包，不需要额外开关；
要打旧的 PS1 流程包才需要 `-ScriptInstaller`。

## 详细文档

构建细节、前后端协议、各功能（版本切换 / 解帧工具选项 / 缺件联网补齐 / 插件包自更新 /
组件日志 / 桌面快捷方式 / 语言与主题）见 [`app/README.md`](app/README.md)。

## 第三方组件

`payload\` 里的组件各自遵循上游许可，随包的 `license\` 目录与各组件目录内的 LICENSE 文件为准。
