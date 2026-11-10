# 未解决问题与待办

> 本文件是仓库内**唯一**的待办载体（见 `AGENTS.md` §一.7）。
> 只记**尚未解决**的事项，每条按「现状 / 影响 / 推进需要什么 / 相关代码位置」写全，读者不看别处也能接手。
> **改动历史不在这里**：某段代码为什么是现在这样，用 `git log` / `git blame` 查。
> 条目关闭后直接删除；要留结论就移入文末「已解决」，并注明解决它的提交。

最后更新：2026-11-10

---

## 1. NGX 后端只做过编译验证，没有画面验证

**现状**

- 合并时只有：两个构型的编译通过（含导出符号检查）+ 作者侧一次 130–131 fps 的 A/B（无可见帧数差）。
- **没有**鬼影 / 闪烁 / 锐度的画质证据，没有 HDR 与 SDR 两种会话的对比，A 卡与 Intel 平台未测，多实例行为未测。

**影响**

- `Ffx12Backend=ngx` 目前只能算「能起来」，不能算「能交付」。

**推进需要**

- RTX 20 系及以上显卡 + 对应版本驱动；
- 确认 NGX 能从驱动侧拿到 DLSS 模型：发布包只在「勾选 OptiScaler 且检测到 RTX」时才下载 `nvngx_dlss.dll`，只开桥的 NGX 后端时该文件可能不存在（本机无 N 卡，无法判断纯驱动是否够用）。
- 判据：日志 `ngx_core init ok=1 available=1`；`ngx_hdr_decision is_hdr=… src=…` 与当前会话是否一致；同一场景分别用 native 与 ngx 截图对比。

**相关代码**

- `Dx11FsrBridge/NgxBackend.cpp`（`ensure_init` / `resolve_is_hdr` / `build_create_flags`）

---

## 2. 非 NVIDIA 平台借用外部 NGX 实现（机制已落地，画质契约与实机未验证）

**现状**

- `NgxProvider` 决定 NGX 实现从哪里来（默认 `auto`）：
  - `nvidia`：注册表 `HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore` 的 `FullPath` → `_nvngx.dll`，失败即确定性失败（与改动前逐条相同）；
  - `external`：先 `LoadLibraryW(L"_nvngx.dll")`（OptiScaler 的 LoadLibrary 钩子会返回它自己），再枚举已加载模块，找导出 `NVSDK_NGX_D3D11_Init_Ext` 的那个；
  - `auto`：先 `nvidia`，失败再 `external`。
- 外部实现失败是**可重试**的（5 s 退避 × 12 次 ≈ 60 s），用尽才记 `reason=provider_missing` 并回落 native；不再一次判死整轮会话。
- 取证行：`ngx_core provider_mode=…`（配置要什么）与 `ngx_core provider resolved=… module=…`（实际用了谁；kind ∈ `nvidia_registry` / `nvngx_load` / `loaded_module`）。

**影响 / 仍未验证**

- 只针对「A 卡 + OptiScaler（v0.9.4-final）」这一种组合设计，**没有实机跑过**；`auto` 在纯 N 卡机器上的行为应当与改动前一致，但也只是代码级推断。
- 外部实现是 OptiScaler 自己的 FSR 2.2.1（D3D11 静态 FFX），与真 DLSS 不是同一个实现 ⇒ 画面结论不能照搬 DLSS 的。

**必须先验的画质契约**

- 桥恒设 `MVLowRes`，`MV_Scale_X/Y` 只是透传；而对方的 FSR2 D3D11 实现不读该组参数（按 UV 单位处理运动矢量）⇒ 若游戏给的是像素单位会出现全屏拖影。
- `IsHDR` 直接决定对方的 FSR2 是否走 HDR 分支，误判即过曝或发灰。

**推进需要**

- 本机（AMD RX 9070 XT + OptiScaler）实跑：`Ffx12Backend=ngx` + `NgxProvider=auto`，确认日志 `init ok=1` 与 `provider resolved=…`；
- 画面：先在 SDR 会话看是否拖影（验 `MV_Scale` 契约），再切 HDR 会话看亮度（验 `IsHDR`）；测试期把 OptiScaler 的 `SkipFirstFrames` / `Dx11DelayedInit` / `Hudfix` 显式钉死。

**相关代码**

- `Dx11FsrBridge/NgxBackend.cpp`（`try_nvidia_registry` / `try_external` / `scan_loaded_modules` / `bind_entries_from` / `soft_fail_init` / `build_create_flags`）

---

## 3. `Ffx12SingleInstance` 缺省值改为 1，需要回归确认

**现状**

- 随 NGX 后端一并把代码缺省从 0 改为 1，`Dx11FsrBridge.package.ini` 也写 1；理由是「ini 与代码缺省不一致」。

**影响**

- 这是**全局**行为变更，不只影响 NGX：多实例同时存在时，非首选实例会走单实例分支。

**推进需要**

- 确认多实例场景（多个 FSR2 实例 / 多次 device 创建）的实际表现，再决定保留缺省 1 还是回退 0。

---

## 4. 长期未定案的路线问题

- **DX11 FSR2 导出层路线**：是否把 FSR2 以导出层形式暴露，至今未定案。
- **OptiScaler `Dx11wDx12` 开启帧生成会闪退**：现象稳定复现，根因未定位。

---

## 5. 代码注释里残留的悬空编号与章节引用

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

## 6. `Ffx12Backend=auto` 只按型号判据，缺 DLSS 运行时不会自动回落

**现状**

- 2026-11-10 起 `Ffx12Backend` 缺省为 `auto`（代码 `lpDefault` 与发布模板一致）：`GpuBackendPick.h` 按 vendor / device id / 描述判 `RTX` ⇒ `ngx`，其余 ⇒ `native`。
- 判据**只看型号**，不看 NGX 运行时是否真的可用（避免与 `ngx::ensure_init` 形成两套口径）。
- 落位点在 `record_adapter_info`（设备创建 hook 内，早于 `swapchain_hook_needed()` 的求值），每个会话只落位一次。

**影响**

- RTX 机器但缺 `nvngx_dlss.dll`（发布包只在「勾选 OptiScaler 且检测到 RTX」时才下载该文件）⇒ auto 选中 ngx 后 `ensure_init` 失败，dispatch 逐帧返回 false 走 fail-open，表现为**没有超分**（回落到游戏原生路径），日志只有 `ngx_core init ok=0`。
- 非 RTX + OptiScaler 的组合（借外部实现呈现 DLSS 接口那条路线）在**全新安装**下默认得到 native，必须显式写 `Ffx12Backend=ngx` 才会让 OptiScaler 接管；`Deploy-Bridge.ps1` 不写该键，所以既有安装保留用户原值。

**推进需要**

- RTX 实机：验 `auto` ⇒ `ngx` 的正向路径，以及缺 `nvngx_dlss.dll` 时的实际表现。
- 若「缺运行时即无超分」不可接受，可在 NGX 确定性失败后把后端切回 native；注意别为此新增第二套可用性判据（`AGENTS.md` §五.1 判据单一来源）。
- 可选的收紧：auto 除型号外再要求「特征目录里存在 `nvngx_dlss.dll`」或「注册表 NGXCore 存在」。

**相关代码**

- `Dx11FsrBridge/GpuBackendPick.h`、`Dx11FsrBridge/Dx11FsrBridge.cpp`（`resolve_backend_auto` / `record_adapter_info` / `load_config` 的 Ffx12Backend 段）、`Dx11FsrBridge/Ffx12Backend.cpp`（dispatch 的 ngx 分支）

---

## 已解决

### 发布管线不启用 NGX 构型（2026-11-10）

**解决提交**：《chore(ngx): NVIDIA NGX 头随仓库分发，发布构型启用 NGX 后端》（`git log --oneline --grep=chore(ngx)` 可定位）。

**结论**：采用与 OptiScaler 相同的做法 —— 把编译所需的 NVIDIA NGX 头随仓库分发，发布包因此带上 NGX 后端。

- `Dx11FsrBridge/third_party/ngx/` 现有固定 commit（`a291cc7` / DLSS 310.7.0）的 `nvsdk_ngx_defs.h`、`nvsdk_ngx_params.h`、许可原文 `NVIDIA-SDK-LICENSE.txt` 与 `NOTICE.md`（含字节数与 SHA-256 表）。
- `tools/Fetch-NgxHeaders.ps1` 由「下载器」变成「下载/校验/升级三用」：`-Verify` 只读本地按固定哈希校验（CI 用它，手改头文件会直接失败），`-Update` 只打印新哈希。
- `build-release.yml` 新增正向断言：打包后按 UTF-16 查 `payload\Bridge\Dx11FsrBridge.dll` 里的 `Ffx12Backend` / `NgxRenderPreset` / `NgxHdr` / `ngx_hdr_decision`，缺任一即让构建失败 —— 以后不可能再静默降级成 NGX OFF。
- 许可文本随包分发：`payload\Bridge\NVIDIA-NGX-SDK-LICENSE.txt`（打包时从 `third_party/ngx/` 复制，缺失即打包失败）。

**残余风险（务必知悉）**：这不是许可合规，只是「先例多、执行风险低」。NVIDIA RTX SDKs License §4.e 禁止使 SDK 受开源许可约束，§2.c 要求以同等保护条款分发，RTX Supplement §1 把授权限定在 NVIDIA GPU 系统；本仓库以 GPL-3.0-or-later 分发含 NGX 代码的二进制因此仍与之冲突，风险落点是编译并分发该二进制的人。

### `third_party/ngx/README.md` 与实际脚本不一致（2026-11-10）

**解决提交**：《chore(ngx): NVIDIA NGX 头随仓库分发，发布构型启用 NGX 后端》（`git log --oneline --grep=chore(ngx)` 可定位）。

按现状重写：只列已入库的 2 个头、说明为何不能 include 第三个（会触发 `__imp_NVSDK_NGX_D3D11_*` 链接失败）、写明 CMake 默认 ON 且**不存在** `DX11FSRBRIDGE_NGX_INCLUDE_DIR` 变量、给出升级 5 步（含刷新 NOTICE 表与许可原文）。
