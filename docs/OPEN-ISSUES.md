# 未解决问题与待办

> 本文件是仓库内**唯一**的待办载体（见 `AGENTS.md` §一.7）。
> 只记**尚未解决**的事项，每条按「现状 / 影响 / 推进需要什么 / 相关代码位置」写全，读者不看别处也能接手。
> **改动历史不在这里**：某段代码为什么是现在这样，用 `git log` / `git blame` 查。
> 条目关闭后直接删除；要留结论就移入文末「已解决」，并注明解决它的提交。

最后更新：2026-11-10

---

## 1. 发布管线不启用 NGX 构型（最高优先级）

**现状**

- `.github/workflows/compile-check.yml` 对 NGX 做双构型编译验证（NGX OFF / NGX ON，含拉取固定 commit 的头文件与 `Verify NGX symbols present`）。
- `.github/workflows/build-release.yml` **完全不涉及 NGX** —— 发布构建里 `Dx11FsrBridge/CMakeLists.txt` 因缺头自动降级为 NGX OFF，只打一行 STATUS，不报错。

**影响**

- `Dx11FsrBridge/NgxBackend.cpp` 与桥侧所有 `#if DX11FSRBRIDGE_ENABLE_NGX` 分支在发布包里是死代码。
- 用户在 ini 里设 `Ffx12Backend=ngx` 会静默回落 native：日志只有 `ngx_core init ok=0 …`，画面与不开该键一致。

**推进需要（二选一，先做决策）**

1. 发布管线加 NGX 开关 —— 前提是先确认「本仓库以 GPL-3.0 分发、用 NVIDIA 专有 NGX 头编译出的二进制」是否合规；
2. 维持现状，并在 README 写明「要 NGX 后端需自行拉头重编」（`tools/Fetch-NgxHeaders.ps1`，固定 commit + 逐文件 SHA-256）。

**连带问题**

- `nvngx_dlss.dll` 只在安装器「勾选 OptiScaler 且检测到 RTX」时才下载（`Build-OnlineInstaller.ps1`、`Installer/app/README.md`）。只开桥的 NGX 后端、不勾 OptiScaler 时，即使机器是 N 卡也会因缺该 DLL 起不来。

**相关代码**

- `Dx11FsrBridge/NgxBackend.cpp`（`resolve_feature_dirs` / `ensure_init`）
- `Dx11FsrBridge/CMakeLists.txt`（NGX 头存在性判据与降级分支）
- `tools/Fetch-NgxHeaders.ps1`、`.github/workflows/build-release.yml`、`.github/workflows/compile-check.yml`

---

## 2. NGX 后端只做过编译验证，没有画面验证

**现状**

- 合并时只有：两个构型的编译通过（含导出符号检查）+ 作者侧一次 130–131 fps 的 A/B（无可见帧数差）。
- **没有**鬼影 / 闪烁 / 锐度的画质证据，没有 HDR 与 SDR 两种会话的对比，A 卡与 Intel 平台未测，多实例行为未测。

**影响**

- `Ffx12Backend=ngx` 目前只能算「能起来」，不能算「能交付」。

**推进需要**

- RTX 20 系及以上显卡 + 对应版本驱动；
- 判据：日志 `ngx_core init ok=1 available=1`；`ngx_hdr_decision is_hdr=… src=…` 与当前会话是否一致；同一场景分别用 native 与 ngx 截图对比。

**相关代码**

- `Dx11FsrBridge/NgxBackend.cpp`（`ensure_init` / `resolve_is_hdr` / `build_create_flags`）

---

## 3. 非 NVIDIA 平台借用外部 NGX 实现（未实现，方案已评估）

**现状**

- `ensure_init` 第一步读注册表 `HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore` 的 `FullPath`；非 N 卡上该键不存在 ⇒ 记为确定性失败，进入 5 秒退避，本轮会话不再尝试，整体回落 FFX。

**可行性**

- OptiScaler 拦截对 `nvngx` 的 `LoadLibraryW` 并把自身模块返回给调用方，且导出了 D3D11 全套 `NVSDK_NGX_D3D11_*` 入口；其 `Init_Ext` 在 DLSS 能力不可用时也会返回成功，`CreateFeature` 默认给出 FSR 2.2.1 的 D3D11 实现（静态链接 FFX，不需要额外 DLL、不需要 N 卡）。
- 方案 A：无注册表时直接 `LoadLibraryW(L"_nvngx.dll")`，依赖对方的 kernel32 钩子（约 3~5 行）。
- 方案 B（推荐）：枚举已加载模块，逐个 `GetProcAddress("NVSDK_NGX_D3D11_Init_Ext")`，命中即用（不依赖钩子与文件名，约 40~60 行）。

**推进需要**

- 改掉 `fail_init` 的「确定性失败」语义（当前一次失败即整会话放弃）；
- 增加显式开关（如 `NgxProvider=auto|nvidia|external`），避免默认行为在 N 卡上被改掉。

**必须先验的画质契约**

- 桥恒设 `MVLowRes`，且 `MV_Scale_X/Y` 只是透传；而对方的 FSR2 D3D11 实现不读该组参数（按 UV 单位处理运动矢量）⇒ 若游戏给的是像素单位会出现全屏拖影。
- `IsHDR` 直接决定对方的 FSR2 是否走 HDR 分支，误判即过曝或发灰。

**相关代码**

- `Dx11FsrBridge/NgxBackend.cpp`（注册表探测、导出解析、`build_create_flags`、每帧参数设置）

---

## 4. `third_party/ngx/README.md` 与实际脚本不一致

**现状**

- README 仍写「从 `main` 分支取 6 个头文件」「可用 `DX11FSRBRIDGE_NGX_INCLUDE_DIR` 指定目录」；实际脚本是「固定 commit + 只取 2 个文件 + 逐文件 SHA-256」，且仓库内不存在该 CMake 变量。

**影响**

- 照 README 拉头会多拉、版本不可复现；改 CMake 的人会去搜一个不存在的变量。

**推进需要**

- 按 `tools/Fetch-NgxHeaders.ps1` 与 `Dx11FsrBridge/CMakeLists.txt` 的现状重写该 README。

---

## 5. `Ffx12SingleInstance` 缺省值改为 1，需要回归确认

**现状**

- 随 NGX 后端一并把代码缺省从 0 改为 1，`Dx11FsrBridge.package.ini` 也写 1；理由是「ini 与代码缺省不一致」。

**影响**

- 这是**全局**行为变更，不只影响 NGX：多实例同时存在时，非首选实例会走单实例分支。

**推进需要**

- 确认多实例场景（多个 FSR2 实例 / 多次 device 创建）的实际表现，再决定保留缺省 1 还是回退 0。

---

## 6. 长期未定案的路线问题

- **DX11 FSR2 导出层路线**：是否把 FSR2 以导出层形式暴露，至今未定案。
- **OptiScaler `Dx11wDx12` 开启帧生成会闪退**：现象稳定复现，根因未定位。

---

## 7. 代码注释里残留的悬空编号与章节引用

**现状**

- 与 NGX 后端、HDR 判定相关的注释引用了**本仓库里查不到定义**的编号和章节。这些编号来自合并前的外部审查与整改记录，没有随代码入库，读者无法原地自解。
  - 章节引用指向不存在的章节：`Dx11FsrBridge/NgxBackend.cpp:300` 的「AGENTS §8.5」、`:580` 与 `:679` 的「AGENTS.md §16.1-3」、`:1244` 与 `:2365` 的「AGENTS.md §15」（`AGENTS.md` 现只有 §零、§一、§二、§三、§四 A–I、§五、§六）。
  - 编号未定义：`T-FIX-11`（MINOR-1 / MAJOR-1 / MAJOR-2 / MINOR-2）、`T-FIX-25`、`T-FIX-18`、`T-FIX-2`、`R-5 MAJOR`、`R-9 MINOR-1`、`R-9 MINOR-3`、`R-5 审核 MINOR-A`，以及 `Dx11FsrBridge/NgxBackend.cpp:349` 的「见下方 D-1 注」。
  - 具体位置：`Dx11FsrBridge/Dx11FsrBridge.cpp:7380`、`:7384`、`:7386`、`:7486`、`:7503`、`:7585`；`Dx11FsrBridge/NgxBackend.cpp:349`、`:367`、`:378`、`:381`、`:607`、`:661`、`:2421`、`:2810`；`Dx11FsrBridge/NgxBackend.h:40`。

**影响**

- 违反 `AGENTS.md` §四.I「内部编号必须就地定义」：接手者读到「（AGENTS §8.5）」「T-FIX-25」时会去仓库里找一个不存在的章节或记录，只能跳过，注释因此失去价值。
- 这些注释大多在解释**为什么这么写**（可达性、只求值一次、槽位模板参数、读回后缓冲格式），信息本身是对的，只是引用了不存在的出处。

**推进需要**

二选一，改注释、不改逻辑：

1. **就地展开**（推荐）：把编号替换成它所指的结论本身，例如把「va_list 只消费一次（AGENTS.md §16.1-3）」写成「`va_list` 只能消费一次，第二次 `va_start`/`va_end` 配对会读到已耗尽的位置」；把 `§8.5`、`§15`、`§16.x` 改成 `AGENTS.md` 现有章节号（如 §二.4 线程与并发、§三 #12 资源释放），若原意已无对应条文则删掉引用、只留结论。
2. 暂时保留编号时，在首次出现处**就地补一句图例**（说明编号含义），满足「读者能在原地自解」。

**注意**

- 只改注释；改完按 `AGENTS.md` §四.A 重新编译并核对 DLL 大小与 `DllMain` 归属（注释不影响逻辑，但构建完整性仍要过一遍）。

**相关代码**

- `Dx11FsrBridge/NgxBackend.cpp`、`Dx11FsrBridge/NgxBackend.h`、`Dx11FsrBridge/Dx11FsrBridge.cpp`

---

## 已解决

（暂无）
