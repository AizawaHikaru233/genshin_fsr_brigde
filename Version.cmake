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
#  ⇒ 发版时**只需改两处**：本文件下面那一行 + `FufuGraphicsPlugin/config.ini` 的
#    `Version = x.y.z`（启动器清单格式，只能写 3 段式；第 4 段固定 0）。
#    `Build-OnlineInstaller.ps1` 打包时会按本文件改写 stage 里 `config.ini` 的那一行，
#    并断言 `config.ini` 与包版本一致（`Assert-FufuInstallerVersionSynced`）。
#    改完用 `git grep -n FSR_SUITE_VERSION` 复核。
#
#  ⇒ 两个 Lua 安装脚本**不再写版本号**（2026-11-27 用户要求，取代 2026-10-16 的
#    "两个 Lua 必须跟号"规则）：`install.write_config(plugin_dir, 表)` 会把表渲染进
#    插件清单，表里带 `Version` 就会**覆盖**包内 `config.ini` 的值 —— 2.3.1 那次事故
#    正是 Lua 里的旧版本号盖掉了清单里的新版本号。
#    ⇒ 现在版本只有 `config.ini` 一个来源；打包脚本**反向断言**"两个 Lua 里不许出现
#      `Version = "x.y.z"`"，规则被改回去会当场失败。
#    ⚠️ 未验证：宿主在表里没有 `Version` 键时，是保留清单现有那一行，还是整份重渲染后
#      丢掉该键 —— 装包后要实地看一眼插件版本号是否仍显示正确（见 docs/OPEN-ISSUES.md）。
# ============================================================================
set(FSR_SUITE_VERSION 2.5.1.0)
