# ============================================================================
#  自有组件版本号 —— 全仓库**唯一事实源**（2026-09-28 用户要求，硬约束）
#
#  规则：**每次发版 ⇒ 所有【自有组件】的版本号一起同步迭代**，
#        某个组件本次一行未改，版本号也照样跟着走。
#  理由：任意两个组件搭配时，版本号一致即代表**同一批发版**。
#
#  自有组件（4 个，全部经本文件取版本）：
#      Dx11FsrBridge       -> Dx11FsrBridge.dll
#      FufuGraphicsPlugin  -> FSR-Bridge-Plugin.dll（芙芙启动器/商城插件，产物改名）
#      AntiPlayerMosaic    -> AntiPlayerMosaic.dll
#      TextureLoader       -> TextureLoader.dll
#  非自有（上游分发，**不参与**本规则）：OptiScaler / ReShade / 3dmigoto /
#      DirectStorage / DLSS / FPS Unlocker —— 版本由
#      `SharedResources/upstream-versions.json` 记录并由
#      `tools/Update-UpstreamComponents.ps1` 校验。
#
#  用法（各组件 CMakeLists.txt 顶部）：
#      include("${CMAKE_CURRENT_SOURCE_DIR}/../Version.cmake")
#      project(<Name> VERSION ${FSR_SUITE_VERSION} LANGUAGES ... RC)
#
#  ⇒ 升版只改**下面这一行**：各组件 `.rc` 经 `configure_file` 生成的头读取，
#    不手写版本号。
#
#  ⇒ 发版时**只需改下面一行**；另有 3 处必须手工跟着走（不适合自动化，
#    因为它们是启动器清单格式，只能写 3 段式 `x.y.z`）：
#      `FufuGraphicsPlugin/config.ini`
#      `FufuGraphicsPlugin/Install-FufuPlugin.lua`
#      `FufuGraphicsPlugin/Install-FufuPlugin-Marketplace.lua`
#    → 三处的 `x.y.z` 应等于本文件的 `x.y.z`（第 4 段固定 0）。
#    `Build-OnlineInstaller.ps1` 打包时会按本文件改写 `config.ini` 的那一行，
#    并对两个 Lua 做同步断言（`Assert-FufuInstallerVersionSynced`）。
#    改完用 `git grep -n FSR_SUITE_VERSION` 复核。
#
#  ⇒ 两个 Lua 安装脚本**必须跟版本号**（2026-10-16 修正：此前这里写的是
#    "不参与打包 ⇒ 不跟版本号，其内的 Version 只是清单副本"，**那是错的**）：
#    `install.write_config(plugin_dir, 表)` 是拿**整张表重渲染**插件清单
#    `config.ini`，会把包内随附的那份**整份覆盖**⇒ 包内写得再对也没用，
#    宿主最终显示的是 **Lua 里那个 `Version`**。
#    漏改的后果：2.3.1 升版只改了 `config.ini`，两个 Lua 仍写 2.3.0 ⇒
#    安装/启动后插件版本号被写回 2.3.0（用户实机复现，本日修复）。
# ============================================================================
set(FSR_SUITE_VERSION 2.4.0.0)
