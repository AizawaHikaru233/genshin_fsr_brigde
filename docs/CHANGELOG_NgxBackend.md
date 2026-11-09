# NGX 后端接线变更日志（PR #12）

- **仓库**：本仓库，分支 `feat/ngx-backend`
- **覆盖范围**：NGX(DLSS) 后端接入渲染流程；HDR 状态自动探测（含 DisplayConfig 源与事件驱动缓存）；深度反转配置项对 NGX 生效；`NgxRenderPreset` / `Ffx12SingleInstance` 默认值调整；对 NGX 无效的 ini 键标注；family skip 统计（**未纳入本 PR**，见 §10）；以及 PR #12 审查报告 **10 个次要项**的逐项登记（次要 1/4/5/6/8/9/10 = 已改；次要 2 = 注释与事实不符已改；次要 3 = 核对后原始状态即合规、无需改动；次要 7 = 本文档本身）；以及审查报告 **必修 A / 必修 B-2 / 应修 C** 的补登（见文末 §23–§25：C 项判据已加但对目标游戏不可达、部署脚本 61 行改动、CI 编译门槛（**已于 2026-11-10 实跑通过**，见 §25））
- **编写时间**：2026-11-09（Asia/Hong_Kong）
- **规范依据**：本仓库开发规范 —— 区域 / 原因 / 目标 / 结果 四要素、失败必须留档、E0~E3 证据分级
- **基线 commit**：`a50020f`（分支 `feat/ngx-backend`）。本文档所有符号引用均以该 commit 的源码为准；后续改动请按符号重新定位，不要沿用本文的章节顺序当作代码位置。

> **读法**：每一节 = 一个改动条目。`区域` 给到文件 + 符号（函数名/变量名，不写行号）；`原因` 只写有证据的部分；
> `目标` 是可验证的预期；`结果` 是**实测**结论，没测的一律写「未验证」，不用「应该」代替。
>
> **证据等级**：E0 = 读代码推断 / E1 = 独立探针实测 / E2 = 游戏内实测 / E3 = 对照实测。
> **E0 不得冒充 E1/E2。**
>
> **引用约定**：源码一律按**符号**引用（`NgxBackend.cpp` 的 `srv_typed_format()`），不写 `file:line` ——
> 行号随每次改动腐烂，符号名相对稳定。仅 Windows SDK 头文件保留行号，因其版本固定（10.0.26100.0）。
>
> **章节编号约定**：本文件里的 `§N` **一律指本文档自己的章节**（如 `§11` = 上面的 `## 11.`），不指 `AGENTS.md` / `AGENTS_PROCESS.md`。引用那两份规范时一律写明文件名（如 `AGENTS_PROCESS.md` §8.4）。

---

## 0. 汇总表

| 条目 | 区域 | 类型 | 证据等级 |
|---|---|---|---|
| NGX 后端接线 | `Ffx12Backend.cpp` 的 `ffx12::dispatch()` 内 `g_backend == BackendKind::Ngx` 分流点 | 功能 | **E2**（实机出图）|
| SDR 下 NGX 不可用 | `NgxBackend.cpp` 的 `srv_typed_format()` | 修复 BLOCKER | **E2**（SDR 出图）|
| HDR 状态自动探测 | `NgxBackend.cpp` 的 `resolve_is_hdr()` | 功能 | **E2** |
| 深度反转对 NGX 生效 | `NgxBackend.cpp` 的 `build_create_flags()` / `flags_input_bits()`（`ffx12::depth_inverted()` 位）| 功能 | **E2** |
| DisplayConfig HDR 探测源 | `NgxBackend.cpp` 的 `probe_displayconfig_hdr()` | 功能 | **E2** |
| HDR 探测事件驱动缓存 | `NgxBackend.cpp` 的 `refresh_hdr_probe_cache()` | 性能/正确性 | **E2** |
| `NgxRenderPreset` 默认值 | `Dx11FsrBridge.cpp` 的 `load_config()` 键 5 读取点 | 默认值 | **E0** |
| 无效 ini 键标注 | `Dx11FsrBridge.package.ini` / 部署脚本 | 文档 | **E0** |
| `Ffx12SingleInstance` 默认值 | `Dx11FsrBridge.cpp` 的 `load_config()` 内 `Ffx12SingleInstance` 读取点 | 功能 | **E2**（实例 2→1）|
| family skip 统计（**未纳入本 PR**） | `Dx11FsrBridge.cpp` 的 `g_family_skip_count` + `family_skip_released=1` 对账行 | 统计 | **E0** |
| 审查次要 1：passthrough 每帧 `Error` | `NgxBackend.cpp` passthrough 闸门 | 可观测性 | **E0**（本次改动）|
| 审查次要 2：「CMakeLists 已冻结」注释 | `NgxBackend.cpp` 文件头 `#pragma comment` 段 | 文档 | **E0**（静态）|
| 审查次要 3：`.gitignore` 末尾换行 | 仓库根 `.gitignore` | 文档 | **E1**（`git show` 核对，原始即合规）|
| 审查次要 4：行内左大括号 | `NgxBackend.cpp` 9 处行内大括号 | 格式 | **E0**（静态）|
| 审查次要 5：超长行 | `NgxBackend.cpp` 的 `release_pool_resources()` 内超长注释行（改动前 142 列）| 格式 | **E0**（静态）|
| 审查次要 6：test 范围注释 | `CMakeLists.txt` 的 `add_executable(Ffx12BackendTest ...)` 之前 | 文档 | **E0** |
| 审查次要 7：变更日志 | 本文档 | 文档 | — |
| 审查次要 8：`ensure_init` 计时 | `NgxBackend.cpp` `ensure_init` 计时 | 可观测性 | **E0** |
| 审查次要 9：CS 置空决策 | `NgxBackend.cpp` CS 置空决策 | 决策记录 | **E0** |
| 审查次要 10：`load_config` 互斥 | `Dx11FsrBridge.cpp` `load_config` 互斥 | 正确性 | **E0** |
| 审查应修 C：scRGB / G10 判据 | `Dx11FsrBridge.cpp` 的 `is_hdr10_color_space()` | 判据（净效果=撤销）| **E0**（对目标游戏不可达，见 §23）|
| 审查必修 A：部署脚本 NGX 键 + 61 行改动 | `scripts/Deploy-Bridge.ps1` 的 `$configValues` / `$routes` / `$appliedCount` | 功能 + **契约变更** | **E1**（PS 解析 + 复刻探针）；实机部署 **NOT VERIFIED**（见 §24）|
| 审查必修 B-2：CI 编译门槛 | `.github/workflows/compile-check.yml` 的 `build-ngx-on` / `build-ngx-off` | 构建闭环 | **E1**（**两个 job 均已实跑 success**，见 §25）|

---

## 1. 把 NGX(DLSS) 后端接到渲染流程

**区域**：`Dx11FsrBridge/Ffx12Backend.cpp` 的 `ffx12::dispatch()` 内 `g_backend == BackendKind::Ngx` 分流点；`Dx11FsrBridge/NgxBackend.cpp` 全文；`Dx11FsrBridge/CMakeLists.txt` 的 `if(DX11FSRBRIDGE_ENABLE_NGX)` 接线块（三行：`target_sources` + include 目录 + 宏）。

**原因**：旧翻译层（`Fsr2TranslationLayer`）已移除，需要一条 DLSS 路径替代 FSR2 合成族劫持。分流点必须**早于** FFX 的任何初始化，否则 D3D12 设备与 FFX context 会白建。

**目标**：`Ffx12Backend=ngx` 时，`ffx12::dispatch` 完全不碰 D3D12/FFX，只调 `ngx::dispatch`；日志出现 `ngx_core backend=ngx`，且实机能出 DLSS 画面。

**结果**：**E2 通过**。实机日志 `ngx_core backend=ngx` + `ngx_core init ok=1 available=1 rc=0x1`，实机确认 DLSS 指示器出现（运行 3，2026-11-08 21:37）。

**已知遗留**：SDR 条件下该路径完全不出图 —— 已由下一节修复。

---

## 2. 修复 SDR 下 NGX 完全不可用（BLOCKER）

**区域**：`Dx11FsrBridge/NgxBackend.cpp` 的 `srv_typed_format()`，消费点在 `create_feature()` 内的 `sd.Format = srv_typed_format(color_fmt)`。

**原因**：**D3D11 规定 TYPELESS 资源在 `CreateTexture2D` 合法，但在 `CreateShaderResourceView` 的 desc 里非法**（返回 `E_INVALIDARG 0x80070057`）。旧代码只映射了 `R10G10B10A2_TYPELESS` 一种；SDR 下游戏给的是 `R8G8B8A8_TYPELESS`(27)，落进 `default` 原样传入 ⇒ 每帧失败。
实测证据（运行 2，21:30）：
```
ngx_core init ok=1 available=1 rc=0x1
ngx_inst color_srv fail hr=0x80070057     <- 每帧失败，共 2053 次
ffx12_result rc=DISPATCH_FAILED ... action=invalidate_reset
```
**为什么开 HDR 能绕过**：HDR 下游戏直接给 `R8G8B8A8_UNORM`(28)（已是 typed），走 `default` 恰好合法 ⇒ **缺陷被掩盖，不是被修复**。

**目标**：SDR 下 `color_srv fail` 计数归零，DLSS 出图。

**结果**：**E2 通过**，`color_srv fail` 由 2053 → 0（实机日志计数）。

---

## 3. `NgxHdr` 自动探测 HDR 状态

**区域**：`Dx11FsrBridge/NgxBackend.cpp` 的 `resolve_is_hdr()`（IsHDR 的**唯一决策点**）；`build_create_flags()`；`flags_input_bits()`（位图判据）。

**原因**：原 `NgxHdr` 是固定 ini 默认值 ⇒ SDR 会话也会置 `IsHDR` 标志喂给 DLSS。四级探测（① `SetColorSpace1` hook > ② DisplayConfig > ③ swapchain 格式 > ④ 输入纹理格式）替代固定值。

**关键裁决**：回退层**宁可判 SDR** —— 只有 `R16G16B16A16_FLOAT` 判 HDR，`R10G10B10A2_*` 与 `R16G16B16A16_TYPELESS` 一律 SDR。理由：实测环境**存在 10-bit SDR 会话**，把 10-bit 判成 HDR 恰好复现本条要消除的缺陷。代价：真 10-bit HDR 会话若探测也失效会少置一个 IsHDR（画质问题，非正确性问题）。

**目标**：SDR 会话 `is_hdr=0`、HDR 会话 `is_hdr=1`；日志 `ngx_hdr_decision` 的 `src=` 能指明来源。

**结果**：**E2 通过**，日志 `ngx_hdr_decision` 的 `src=` 能指明来源。
**已实测否证的错误前提**：DXGI **没有** `GetColorSpace`（核查 Windows SDK `dxgi1_4.h`：`IDXGISwapChain3` 只有 `CheckColorSpaceSupport`(:121) 与 `SetColorSpace1`(:127)）。

---

## 4. `Ffx12DepthInverted` 对 NGX 生效

**区域**：`Dx11FsrBridge/NgxBackend.cpp` 的 `build_create_flags()`（`CreateFlags`）与 `flags_input_bits()` 的 bit1（`kFlagsInputDepthInvert`）。

**原因**：旧代码把 `DepthInverted` **硬编码为恒设**，完全不读 `Ffx12DepthInverted`（该键只传给 FFX 路径）。实测：把该键从 1 改成 0，画面**毫无变化** —— 因为对 NGX 无任何作用。

**目标**：改 `Ffx12DepthInverted` 后 NGX 的 `CreateFlags` 随之变化，且**触发 feature 重建**（否则只在首次 `CreateFeature` 生效）。

**结果**：**E2 通过**（改键后 `CreateFlags` 随之变化并触发 feature 重建，实机日志可见）。

---

## 5. HDR 探测新增 DisplayConfig 源

**区域**：`Dx11FsrBridge/NgxBackend.cpp` 的 `probe_displayconfig_hdr()`，调用点在 `ensure_init()` 第 0 步（只探一次）；`CMakeLists.txt` 链接 `user32`。

**原因**：实测「开着 HDR 但 `is_hdr=0`」，日志：
```
ngx_hdr_decision is_hdr=0 src=swapchain_format sw_fmt=24 color_fmt=23 depth_inv=1 flags=0xA
```
根因：原第 ② 级只把 `R16G16B16A16_FLOAT` 判 HDR，**10-bit HDR（`R10G10B10A2`）被判成 SDR**。新增系统级真实 HDR 状态源，不依赖游戏是否调 `SetColorSpace1`。

**技术依据（已核实）**：`DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO` = 9（`wingdi.h:3042`）；`DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO` 含 `advancedColorEnabled` 位域（`wingdi.h:3167-3186`）；`GetDisplayConfigBufferSizes`/`QueryDisplayConfig`/`DisplayConfigGetDeviceInfo` 在 `winuser.h:13310/13328/13341`。（以上三个头文件行号对应 Windows SDK **10.0.26100.0**；换 SDK 版本需重新核对。）

**目标**：10-bit HDR 会话 `is_hdr=1 src=displayconfig`。

**结果**：**E2 通过**，实机日志 `ngx_hdr_decision is_hdr=1 src=displayconfig`。

---

## 6. HDR 探测改为事件驱动缓存

**区域**：`Dx11FsrBridge/NgxBackend.cpp` 的 `refresh_hdr_probe_cache()`；写入点 4 个（`set_swapchain_hdr` / `set_swapchain_format` / `ensure_init` 的 DisplayConfig 探测 / `set_hdr_mode`）；读点 `resolve_is_hdr()`。

**原因**：对「每帧重复计算」的反对意见：

> 「我认为完全没有任何必要每帧调用。就算你说影响小，只看这个可能确实影响小，但就是这样想，以后每次都觉得影响小，就让它这样，累加起来不就大了」

采纳。①②③ 三级都是**事件驱动**的（hook / `ensure_init` 写入），没有理由每帧重算。

**目标**：热路径（`src == nullptr`）上只有 **1 次 atomic load**；冷路径（`src != nullptr`）额外 2 次 load 只为标注来源。

**结果**：**E2 通过**（A/B 对比完成，热路径只剩 1 次 atomic load）。

---

## 7. `NgxRenderPreset` 默认值改为 0

**区域**：`Dx11FsrBridge/Dx11FsrBridge.cpp` 的 `load_config()` 键 5（`NgxRenderPreset` 读取点）；`NgxBackend.cpp` 的 `g_render_preset = 0`。

**原因**：原默认 12 是凭空的预设值，会覆盖 NGX 自身按输入分辨率/画质推导的规则。默认值应当等于「不干预」。

**目标**：ini 缺键时 `NgxRenderPreset=0`，日志 `ngx_core ... preset=0`。

**结果**：**E0**（代码 + 日志格式已确认）。未做「preset=0 vs 12」的画面 A/B 实测 —— **未验证**该改动对画质的影响方向。
**风险（如实记录）**：这是**改变所有用户行为**的默认值变更，按规范应配一次实机对照；本次未做。

---

## 8. 标注对 ngx 后端无效的 ini 键

**区域**：`Dx11FsrBridge/Dx11FsrBridge.package.ini`、`scripts/Deploy-Bridge.ps1` 的键清单注释。

**原因**：一次独立的配置键审计确认 19 个键对 NGX 后端**无任何消费者**（如 `Ffx12Hdr`、`Ffx12VelocityFactor`、`Ffx12CameraNear/Far/FovScale`、`Ffx12AsyncUpscale`、`Ffx12DllPath` 等）。使用者在 ini 里调它们会「设了没反应」且无告警。

**目标**：ini 与部署脚本里这些键旁有「对 NGX 无效」标注；键仍被读取（FFX 路径还需要）。

**结果**：**E0**（文本已就位）。未验证「使用者是否真的因此少踩坑」—— 该类指标本身不可测，**N/A**。

---

## 9. `Ffx12SingleInstance` 默认值改为 1

**区域**：`Dx11FsrBridge/Dx11FsrBridge.cpp` 的 `load_config()` 内 `Ffx12SingleInstance` 读取点；同文件 `g_config` 结构里 `ffx12_single_instance = true` 的默认值。

**原因**：FFX12 的 GPU 互操作共享层是**全局单份**（共享纹理/fence/单槽 pending/尺寸池），只支持一条流水线；而现场实测有**两个长期并存的 FSR2 上下文**（各有一份 `ffxContext`、历史各归各的）⇒ 两路交替复用同一份共享资源，**交付帧混进另一视角的角色**。

**目标**：实例数由 2 降到 1，混帧消失。

**结果**：**E2 通过**（实例数 2 → 1，实机日志；对应提交 `bac7456`）。

---

## 10. family skip 统计

> **本项未纳入本 PR。** 本节记录的是开发分支上的既有演进；代码侧已确认
> `g_family_skip_count` / `g_family_notify_*` / `fsr2_family_stats` 在本 PR 的
> 改动范围内 **0 命中**（该功能与 PR #12 审查报告的 15 项无关）。保留本节仅用于
> 说明该项在当前代码库中的状态，**请勿据此认为本 PR 包含该功能**。

**区域**：`Dx11FsrBridge/Dx11FsrBridge.cpp` 的 `g_family_skip_count`；对账行 `family_skip_released=1`。

**原因**：`fsr2_family_skip` 跳过合成族预处理后，若不同时记录「跳过了多少次」，无法判断该优化是否真的命中，也无法在出问题时分清「跳过导致」与「别的原因」。

**目标**：对账行出现 `family_skip_released=` 且计数随帧递增。

**结果**：**E0**。计数点已就位，**未做**「关/开 family skip」的帧数与画面 A/B —— **未验证**。

---

## 11. 审查次要项 1（重新评估）：passthrough 每帧 `Error`

**区域**：`Dx11FsrBridge/NgxBackend.cpp` 的 `ngx::dispatch()` 第 0 步 `if (g_passthrough)` 闸门；新增计数器 `g_ngx_passthrough_count`。

**原因**：审查意见「passthrough 每帧写一条 `Level::Error`；`log_ngx_core` 只有级别闸门、没有计数闸门 ⇒ 诊断模式刷屏」。
**原判断**：不加闸门，因为 SDS 判据 14 要求这一行与 `ngx_in` 同帧对应。
**复核后的判断**：判据 14 是**验收时**的需求，不是常态，两者可以共存。

**目标**：
- `NgxLogLevel <= 2`（error/info，验收与常规档位）：**不限频** ⇒ 判据 14 的 `grep -c` 取证成立；
- `NgxLogLevel == 3`（debug，诊断档位）：前 8 条全打 + 之后每 256 条一条。

**为什么保留「前 8 条全打」而不是纯 `%256`**：纯 `%256` 会让**全新一次运行的首条永不出现**（`count=1` 时 `1%256≠0`），而那正是判据 14 的 grep 对象 ⇒ 在 debug 档跑验收会得到**假阴性**。

**结果**：**E0**（代码已改，未编译、未实机）。**未验证**：
- 未测「debug 档下刷屏是否真的被抑制」；
- 未测「error 档下 `grep -c` 计数是否仍与帧数一一对应」。

---

## 12. 审查次要项 2：「CMakeLists.txt 已冻结」注释与事实不符

**区域**：`Dx11FsrBridge/NgxBackend.cpp` 文件头注释第 ⑤ 组（系统库 / `#pragma comment` 段）。

**原因**：该注释原文写「不改 `CMakeLists.txt`，它已冻结」，据此选择了 `#pragma comment(lib, ...)`
而不是改链接行。但本 PR **确实改了** `Dx11FsrBridge/CMakeLists.txt`（加 `DX11FSRBRIDGE_ENABLE_NGX`
开关、加 `user32` 链接）—— 注释与事实相反，属于「理由陈述已经被现实推翻但没改」。

**目标**：注释不再声称 CMakeLists 不可改；保留 `#pragma comment` 的理由改为**真实理由**
（三行只在 `DX11FSRBRIDGE_ENABLE_NGX` 打开时生效，不需要污染 CMake 的全局链接列表）。

**结果**：**E0（静态）通过**。注释已改为「原注释写『已冻结』，但本 PR 确实改了 CMakeLists；
保留 `#pragma comment` 的理由**不是**『CMakeLists 不能动』，而是……」。
**未验证**：无需运行期验证（纯注释改动）。

---

## 13. 审查次要项 3：`.gitignore` 末尾换行

**区域**：仓库根 `.gitignore`。

**原因**：审查报告怀疑该文件末尾缺少换行（`\ No newline at end of file`），
这会让后续追加的规则与最后一行黏在一起。

**目标**：确认文件是否以 LF 结尾；缺则补。

**结果**：**核对后无需改动 —— 原始状态已合规**。实测（`git show HEAD:.gitignore`）
文件以 `\n` 结尾，`git diff` 也不报 `No newline at end of file`，
即**这一项本来就不存在缺陷**，不登记任何「已修复」。
本 PR 对 `.gitignore` 的改动只有新增忽略规则（`!docs/CHANGELOG_NgxBackend.md` 等），与行尾无关。
**证据等级：E1**（`git show` 与 `git diff` 的可复现输出，非推断）。

> **核对范围（如实标注）**：以上结论只对**本分支 `feat/ngx-backend` 的历史**成立 —— 该分支上所有改动过 `.gitignore` 的提交（最早 `c3bd66a`）末尾均带换行。
> 仓库里另有一条 `ngx-backend` 分支（独立 worktree），其 `474ebe4` 版本的 `.gitignore` 末尾**确实没有换行**；该分支不在本 PR 范围，**本次未对其做任何改动**。若日后合并该分支，需重新核对行尾。

---

## 14. 审查次要项 4（补完）：行内左大括号

**区域**：`Dx11FsrBridge/NgxBackend.cpp` 共 **9 处**（`refresh_hdr_probe_cache` ×2、`resolve_is_hdr` ×2、`ensure_shaders` ×4、`sample_1x1_zero` ×1）。改动后行内大括号计数 = 0。

**原因**：代码规范要求左大括号**独立成行**。先前的改动只改了 5 处，剩 4 处（改动要求列出的 7 处含两个多语句行内块，实际共 9 个 `if`）。

**目标**：`grep -cE "if \(.*\) \{ [A-Za-z]"` = **0**。

**结果**：**E0（静态）通过**，实测 0。`resolve_is_hdr` 的两处保持了**语义完全不变**（三个语句：`if (src) *src = ...`、`g_hdr = ...`、`return ...`）。

---

## 15. 审查次要项 5（补完）：超长行

**区域**：`Dx11FsrBridge/NgxBackend.cpp` 的 `release_pool_resources()` 内 cb 缓存判据注释（改动前 142 列）。

**原因**：代码规范目标 ≤120 列。先前的改动拆了 2 处，剩这 1 处。

**目标**：`awk 'length>120'` = **0**。

**结果**：**E0（静态）通过**，实测 0。拆成两行，内容未删。

---

## 16. 审查次要项 6：`Ffx12BackendTest` 未定义 NGX 宏

**区域**：`Dx11FsrBridge/CMakeLists.txt` 的 `add_executable(Ffx12BackendTest ...)` 之前。

**原因**：`Ffx12Backend.cpp` 有 `#if defined(DX11FSRBRIDGE_ENABLE_NGX)` 分支（调 `ngx::*`），但 test target 不定义该宏 ⇒ 只覆盖 native 路径，而 PR 中**未说明测试范围**。

**目标**：CMakeLists 里有一行注释写明「本 test 只覆盖 native 路径」及理由。

**结果**：**E0**。采用**加注释**方案（改动要求推荐）：该 test 已知在隔离 harness 下运行崩溃（`ffxCreateContext` 抛未捕获异常，退出码 `0xE06D7363`），扩展其覆盖面没有价值。

> ⚠️ **原文此处写「NGX 路径的编译覆盖由 `build-ngx-on` job 保证（已核对 job 存在）」—— 该措辞已在第 5 轮复审中被判为夸大，已于 2026-11-10 改写。**
> **「job 存在」不等于「job 跑过」。** 实测（2026-11-10）：`git ls-tree -r origin/ngx-backend --name-only .github/` 只列出
> `.github/workflows/build-release.yml`（PR 分支上没有 `compile-check.yml`，PR 上不可能触发）；`gh workflow list --all` 只注册了一条
> `Build GitHub Release Package`；`gh run list --limit 10` 的历史运行记录里没有任何 `Compile Check` 条目。
> ⇒ 该 workflow 后来已实跑通过（见 §25）；原文的「保证」二字在当时是夸大，已撤销。

---

## 17. 审查次要项 8：`ensure_init` 在渲染线程 `LoadLibrary` + `Sleep(500)`

**区域**：`Dx11FsrBridge/NgxBackend.cpp` 的 `ensure_init()` 入口计时 + 其第 10 步日志。

**原因（调用链已核实）**：
```
hooked_draw / hooked_draw_indexed            (Dx11FsrBridge.cpp 的 hooked_draw / hooked_draw_indexed)  ← 渲染线程
  -> try_fsr2_translation_draw               (Dx11FsrBridge.cpp 的 try_fsr2_translation_draw)
     -> ffx12::dispatch                       (Dx11FsrBridge.cpp 内对 ffx12::dispatch 的调用)
        -> ngx::dispatch                      (Ffx12Backend.cpp 的 ffx12::dispatch 内)
           -> ensure_init                     (NgxBackend.cpp 的 ensure_init)  ← LoadLibraryW + Sleep(500)
```
**确认**：`ensure_init` 确实在**渲染线程的 draw 钩子内**首次被调用。`LoadLibraryW` 与 `Sleep(500)` 都在该路径上 ⇒ 首次进游戏有一次 ≥0.5 秒的卡顿。

**三个方案的评估**：

| 方案 | 可行性 | 结论 |
|---|---|---|
| **A**：移到 `DllMain` | ❌ `DllMain` 里不能做重活（loader lock）。`LoadLibraryW` 在 `DllMain` 内会死锁；`Sleep` 更不可接受。且 NGX 需要 `ID3D11Device`，`DllMain` 时设备还不存在。 | **否决** |
| **B**：后台线程预热 | ⚠️ `ngx::` 状态是**全局单例**（`g_params` / `g_init_ext` / `g_inited` / `g_device`），且 `ensure_init` 的契约是「唯一调用点在 `ngx::dispatch` 内、在 `ffx12::g_mutex` 保护下」。加线程需要给整个 NGX 状态层加锁，而 NGX 的 D3D11 入口本身不保证线程安全 ⇒ 引入的**竞态风险高于它省下的 0.5 秒**（一次性）。另：`Sleep(500)` 在后台线程上依然要等，只是把卡顿从"进游戏那一刻"挪到"某个不确定时刻"，可能变成**随机卡顿**，更难排查。 | **否决（风险 > 收益）** |
| **C**：接受现状 + 记录耗时 | ✅ 最省事、最安全。卡顿是**一次性**的（`g_inited` 短路后不再付），而 `Sleep(500)` 的 500ms 是 **E1 实测结论**（`Sleep(500)` 后 `CreateFeature` 才有效），删掉会让 DLSS 直接失效。 | **采纳** |

**目标**：`ensure_init` 成功时固定打一行 `ngx_core init_slow first_init_us=%.1f`，把这次卡顿变成**数字**。

**实现细节**：
- 计时起点取在**失败退避窗口之后**（窗口内的早退不做任何工作，计它会把「没初始化」算成「初始化很慢」）；
- 计时**只在首次初始化路径上**（`g_inited` 短路 + 5 秒退避窗口之外），**不是每帧探针** ⇒ 不触犯热路径红线（禁止在每帧路径上加计时探针）；
- `Sleep(500)` **原样保留**，未改动。

**结果**：**E0**（代码已改，未编译、未实机）。**未验证**：`first_init_us` 的实际数值 —— 需要一次实机启动才能拿到。预期量级 **≥500,000 us**（因为含 `Sleep(500)`）；若实测明显更大，说明 `LoadLibraryW` 与 `Init_Ext` 占主要部分。

---

## 18. 审查次要项 9：CS 槽位置空而非还原原值

**区域**：`Dx11FsrBridge/NgxBackend.cpp` 的 `dispatch_depth_cs()` / `dispatch_motion_cs()` 的解绑段。

**原因**：审查意见「用完置空（null）而非还原游戏原本绑定的 SRV，多组件共存（桥 + OptiScaler + ReShade）时可能互相干扰」。

**量化依据（这是本条决策的核心产出，不是估计）**：

| 项 | 数量 | 说明 |
|---|---|---|
| 两个 CS 各绑定的槽位类别 | 4 | CS shader / SRV0 / CB0 / UAV0..N |
| 还原需要的 `Get*` | **8 / 帧** | `CSGetShader` + `CSGetShaderResources` + `CSGetConstantBuffers` + `CSGetUnorderedAccessViews` × 2 个 CS |
| 还原附带的 `Release` | **8 / 帧** | 每个 Get 返回的接口都 AddRef 一次，漏一个即永久泄漏 |
| 当前解绑侧的 `Set*` | **8 / 帧** | 0 个 Get |

⇒ 还原方案的代价是 **+8 Get* +8 Release / 帧**，**不是**改动要求设想的 2-3 个 Get*。

**两条决定性理由（均已在代码/头文件核实）**：

1. **不能改读桥自己的影子状态**：`g_state.cs_srvs` / `cs_uavs` / `cs_shader` / `cs_cbs` 只由 `hooked_cs_set_*` 维护，而 **CS 系列钩子在 Release 构建里根本不装** —— `Dx11FsrBridge.cpp` 的 patches 表里 `k_idx_cs_set_*` 整组在 `install_context_hooks()` 的 `#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)` 块内。生产构建读到的会是**从不更新**的旧值 ⇒ 还原成任意别的资源，**比置空更危险**。

2. **还原在 API 层面就是不完整的**：`CSGetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView **ppUAVs)` **没有 `pUAVInitialCounts` 输出参数**（已核 `d3d11.h:8523-8529`，Windows SDK 10.0.26100.0），而绑定侧 `CSSetUnorderedAccessViews(..., const UINT *pUAVInitialCounts)` 是有的。⇒ 走「保存/还原」时，游戏原本设的 **UAV 初始计数会被静默丢弃**（append/consume 型 UAV 的语义来源）。**「还原原值」做不到真的还原原值，只能还原一半** —— 比置空更难解释的形态。

**决策**：**保持置空，加注释说明为什么**，并记录风险。

**风险（如实记录，未验证）**：多组件共存时，若 OptiScaler / ReShade 依赖「桥退出后 CS 槽位仍是它们绑的那一份」，本实现会把它们看到的槽位清成 null。当前**无证据**表明存在这种依赖（三组件共存下未做对照实测）。若现场出现「开桥后 ReShade 的某个 CS 后处理失效」，本条是**首要排查点**。

**结果**：**E0**（决策记录 + 代码注释）。**未验证**：
- 未做「关/开还原」的帧数 A/B 实测（热路径红线要求先验证探针本身无扰动，本次不具备条件）；
- 未做三组件共存的对照实测。
⇒ 本决策的依据是**可数的事实**（8 Get* + 8 Release vs 0）与**Release 构建无影子状态可用**，**不是性能实测**。这一点已在代码注释里显式标注。

---

## 19. 审查次要项 10：`NgxPassthrough=1` + `Ffx12FailClosed=1` 组合黑屏

**区域**：`Dx11FsrBridge/Dx11FsrBridge.cpp` 的 `load_config()`，在 `Ffx12FailClosed` 读取点之后新增的互斥块。

**原因**（配置键规范要求评估「两个『关闭/接管』语义的开关同时开启是否互相吞掉」）：
1. `NgxPassthrough=1` ⇒ `ngx::dispatch` 在第 0 步就 `return false`（不超分）；
2. 该 `false` 传回 `ffx12::dispatch` ⇒ 走到 draw 钩子的派发失败分支；
3. `Ffx12FailClosed=1` ⇒ 该分支 `return true` = **吞掉这次 draw 且不产出任何图**。

⇒ 不超分 + 不回退 + draw 被吞 = **每帧黑屏**（不崩不冻，最难排查的一类）。

**决策**：**代码级互斥** —— 检测到两者同时开时，**强制关掉 `Ffx12FailClosed`**，并记一条 `config_mutex` 告警。

**为什么关 `Ffx12FailClosed` 而不是关 `NgxPassthrough`**：`passthrough` 是使用者显式要的**观测手段**（SDS 判据 14 依赖它），关掉它等于让使用者「设了没反应」；而 `fail_closed` 是**故障注入式诊断**（源码注释即「禁止回退原生（测试/故障显式暴露）」），它的正确语义本就是「暴露失败」，不是「制造黑屏」。两者冲突时保留使用者的观测意图、放弃故障注入，方向更安全。

**实现要点**：
- 用 `pending_warnings` 而非 `LOG_WARN`：`load_config()` 跑在 `blog::init()` 之前，直接写日志会被丢弃；
- 判据用 `ngx::passthrough()` 而非局部 `ngx_pt`：前者返回的正是下游消费者读到的**同一个值**，避免「互斥按 ini 判、消费者按全局判」的两套口径；也避免二次解析 ini。

**⚠️ 适用范围（如实记录，这一条很重要）**：`Ffx12FailClosed` 的**读取点本身**在 `#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)` 内 ⇒ **发布构建里 `g_config.ffx12_fail_closed` 恒为 false，本互斥在发布构建中永不触发**。这对组合只在**诊断构建**里能凑出来 —— 但它恰恰是诊断构建最容易踩的组合（排查黑屏时同时开「直通」与「失败关闭」来定位到底哪一段吞了图），所以仍然必须挡住。

**结果**：**E0**（代码已改，未编译、未实机）。**未验证**：
- 未在诊断构建里实跑「两者同开」验证告警出现且不黑屏；
- 未验证 `config_mutex` 行在日志里的实际形状。

---

## 20. 未做 / 无法验证的项（如实登记）

| 项 | 状态 | 缺什么条件 |
|---|---|---|
| 所有代码改动的**编译**验证 | **NOT VERIFIED** | 本次按要求**不跑构建**。后续必须实跑 `cmake --build`，且构建输出必须含 `Building CXX object`（构建输出只有 Linking 说明没有重编）|
| 构建命令文档（`AGENTS.md` / `AGENTS_PROCESS.md`）与既有 CMakeCache 生成器的一致性 | **NOT VERIFIED**（未实跑 configure）| 本次按要求**不跑构建**。依据是三个既有目录 `build` / `build-off` / `build-diag` 的 `CMakeCache.txt` 均记录 `NMake Makefiles`（读取缓存文件，非推断）；`build-fresh` / `build-diag-fresh` 的 Ninja 路径未实测 |
| `init_slow first_init_us` 的实际数值 | **NOT VERIFIED** | 需一次实机启动 |
| passthrough 闸门的刷屏抑制效果 | **NOT VERIFIED** | 需诊断构建 + `NgxLogLevel=3` 实跑 |
| CS 置空对 ReShade/OptiScaler 的实际影响 | **NOT VERIFIED** | 需三组件共存对照实测 |
| `config_mutex` 互斥的实际触发 | **NOT VERIFIED** | 需诊断构建 + 两个键同开 |
| `NgxRenderPreset` 默认值 0 的画质影响 | **NOT VERIFIED** | 需 `preset=0` vs `12` 的 A/B 实测 |
| family skip 的收益 | **NOT VERIFIED** | 需关/开的帧数 A/B |
| 应修 C：G10 + FP16 后缓冲组合的真实行为 | **NOT VERIFIED** | 需能在目标游戏上构造「G10 色空间 + FP16 后缓冲」组合，或换一个真的走 scRGB 的程序。当前判据对目标游戏**恒 false**（见 §23）|
| 必修 A：部署脚本在真实路线上的实际部署 | **NOT VERIFIED** | 需一次真实部署（会改动实机 payload 目录）。本轮只做了 PS 5.1 语法解析 + 复刻逻辑的退出码探针 + 三条路线的 `Test-Path`（见 §24）|
| 必修 A：仓库外调用方是否依赖旧的 `exit 0` | **NOT VERIFIED** | 需知道有哪些外部调用方；仓库内已核为零调用方，外部无从枚举（见 §24）|
| 必修 B-2：`compile-check.yml` 的两个 job 能否跑通 | ✅ **已解除（E1）** | 2026-11-10 实跑：NGX OFF 1m07s success / NGX ON 1m20s success，DLL 640000 B（见 §25）|

---

## 21. 失败与被证伪的记录（失败不删除，标注留档）

| 结论 | 状态 | 新证据 |
|---|---|---|
| 「passthrough 行不能加计数闸门，会破坏判据 14」 | ~~已证伪~~ | 判据 14 只在**验收档位**（`NgxLogLevel<=2`）需要；诊断档位（=3）可以限频。两者共存，见本文第 11 节「审查次要项 1（重新评估）：passthrough 每帧 `Error`」。 |
| 「`NgxPassthrough` 的互斥可以直接用局部变量 `ngx_pt`」 | ~~已证伪~~（编写时写错过一次，已在提交前改正） | `ngx_pt` 虽然在同一函数作用域可见（`#endif` 不关闭 C++ 作用域），但它与消费者读的全局是**两套口径**。改用 `ngx::passthrough()` 保证同源。**注**：编写时曾误写「跨 `#endif` 引用是 C2065」，那是**错的**，已在代码注释中改正为正确理由。 |
| 「`Ffx12FailClosed` 在发布构建里也会被读」 | ~~已证伪~~ | 它的读取点在 `#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)` 内，发布构建恒 false。见本文第 19 节「审查次要项 10」末的「⚠️ 适用范围」段（诊断构建专属，发布构建恒 false）。 |
| 「插入互斥块时用『`Ffx12FailClosed` 注释 + `#endif`』当锚点」 | ~~已证伪~~（**编写时真实踩中，已改正**） | 该锚点跨越了 `g_config.ffx12_fail_closed = GetPrivateProfileIntW(...)` **两行真正的读取代码** ⇒ 替换把这两行**删掉了**。若不复查 `git diff`，后果是「`Ffx12FailClosed` 设了也永远读不到」——正是「配置键只在非 RELEASE 分支读 ⇒ 生产静默失效」的同族形态，且编译零错误。**已从 diff 里发现并逐字恢复**（现 diff 中该两行为 context 行，不再是删除行）。教训：锚点必须包含**被保留的代码本身**，不能只用它前后的注释。 |

---

## 22. 后续建议（不属于本 PR 范围）

1. **实机验证优先序**：`init_slow first_init_us` 的数值 → 诊断构建下 passthrough 闸门 → 三组件共存的 CS 槽位影响。
2. **若要真正解决 `ensure_init` 的卡顿**（而非只记录），正确方向是**在游戏创建 D3D11 设备后、第一次 draw 之前**找个时机预初始化（例如 swapchain 首次 `Present` 时起一个**单次**后台任务，并让它与 `ngx::dispatch` 通过一个 `std::once_flag` 串行化）。这需要单独一轮 L2 设计与审核，不能顺手做。
3. **`NgxRenderPreset` 的默认值变更**应当补一次实机 A/B（这是「改变所有用户行为」的改动，按规范应配一次实机对照）。

---

## 23. 审查应修项 C（scRGB / G10）：**判据已加，但对目标游戏不可达 —— 净效果 = 撤销 C 项**

> **本节是对「应修 C 已修复」的纠正。** 代码注释是诚实的（明写「净效果 = 撤销 C 项」「维护者不得据此认为该通路已可用」），
> 但变更日志此前**一处都没写**，读者只看本文件会以为 C 项已修好。违反「不隐瞒」，故补本节。

**区域**：`Dx11FsrBridge/Dx11FsrBridge.cpp` 的 `is_hdr10_color_space()`（检索：`grep -n "bool is_hdr10_color_space" Dx11FsrBridge/Dx11FsrBridge.cpp`）。
两个**消费点**必须一起看，否则会误判影响面：
- `hooked_set_color_space1()`（检索：`grep -n "HRESULT STDMETHODCALLTYPE hooked_set_color_space1" Dx11FsrBridge/Dx11FsrBridge.cpp`）
  把返回值交给 `ngx::set_swapchain_hdr(hdr10)` —— 这是 ①级探测源；
- `hooked_check_color_space_support()`（同文件，检索 `hooked_check_color_space_support`）
  把它当作 `hdr_swapchain_spoof_active()` 的判据（交换链颜色空间伪装）。

**原因**：审查报告「应修 C」指出原判据**只认 PQ（`DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020` 一族）**，
漏掉 scRGB（`DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709` / `..._G10_NONE_P2020`，即 G10）⇒ 走 scRGB 输出的 HDR 会话会被上报为 SDR。

**目标**：把 G10 系列纳入 HDR 判定，使 scRGB 会话 `is_hdr=1`。

**结果（分两层，不要只看第一层）**：

1. **判据确实加了**：G10 分支存在。但完整判据不是「是 G10 就算 HDR」，而是
   **「G10 色空间 **且** 后缓冲格式为 `R16G16B16A16_FLOAT`(10) 或 `..._TYPELESS`(9)」**
   （判据来源：`ngx::swapchain_format()`；未开 `DX11FSRBRIDGE_ENABLE_NGX` 时**恒 false**）。
   不收 `_UNORM`(11) / `_SNORM`(13)，理由见代码注释「真正撑住本判据的是取值范围而不是位深」。
2. **但对本项目唯一目标游戏，这个分支恒为 false、不可达**。实机日志（两套独立会话，逐字一致）：

```
HDR 会话：ngx_hdr_decision is_hdr=1 src=swapchain mode=0 sw_known=1 sw_hdr=1 sw_fmt=24 color_fmt=23 depth_inv=1 flags=0xB
SDR 会话：ngx_hdr_decision is_hdr=0 src=swapchain_format mode=0 sw_known=0 sw_hdr=0 sw_fmt=29 color_fmt=27 depth_inv=1 flags=0xA
```

   `sw_fmt=24` = `R10G10B10A2_UNORM`，`sw_fmt=29` = `R8G8B8A8_UNORM_SRGB`（Windows SDK 10.0.26100.0 的 `dxgiformat.h`：
   检索 `grep -n "R10G10B10A2_UNORM\|R8G8B8A8_UNORM_SRGB\|R16G16B16A16_FLOAT" <SDK>/Include/10.0.26100.0/shared/dxgiformat.h`）。
   **两者均非 FP16** ⇒ G10 分支的条件永远不成立 ⇒ 对目标游戏 G10 一律判 SDR。
   （出处：`build-t6verify/t8-run1.log`、`perf-latest.log` 的 `ngx_hdr_decision` HDR 行；
   `build-t6verify/t8-sdr.log`、`perf-sdr.log` 的 SDR 行。这些文件被 `.gitignore` 的 `**/build-*/` 命中，**不入库**，
   外部无法直接复核 —— 这是已知缺陷，见 §26 的 m-4 条。）
3. **净效果 = 撤销 C 项**：目标游戏的 HDR 实际走 **PQ(G2084)** 分支（`is_hdr=1` 那一行是它给出的，不是 G10 给出的），
   G10 在目标游戏上一律判 SDR。**不得把本条读成「scRGB 已支持」。**
4. **逃生舱**：`NgxHdr=1` 强制 HDR（`resolve_is_hdr()` 的 `g_hdr_mode == 1` 分支直接 `return true`，不看色空间）。
   注意它会**覆盖整个探测链**（对任何色空间都强制 HDR），所以它是「人工兜底」，不是「scRGB 判据」。

**副作用（必须记，否则会误判影响面）**：`hooked_set_color_space1()` 里 `hdr10` **一次求值、两处复用**（spoof 判据 + `set_swapchain_hdr` 实参）。
目标游戏走 G10 时两者都会是 false，但那是「G10 一律判 SDR」的**直接后果**，不是第二个 bug。
即：在 G10 会话上，交换链颜色空间伪装与 NGX 的 HDR 判定会**同时**失效。

**证据等级**：**E0**（读代码 + 逐字比对实机日志字段 + 核对 SDK 头文件常量）。
- **未做**「`_UNORM`/`_SNORM` 后缓冲 + G10 色空间」的实机组合实测；
- **未做**任何构建与实机运行。
⇒ 该判据是**未经 E2 证实的假设**，不是已验证结论。**状态：NOT VERIFIED（对目标游戏）**。

**缺什么条件才能验证**：需要能构造出「G10 色空间 + FP16 后缓冲」组合的程序（目标游戏不是），
或找到另一个真的走 scRGB 输出的应用，在其上跑 `ngx_hdr_decision` 观察 `src=swapchain` 且 `is_hdr=1`。
在此之前，与本条相关的任何「已修复」结论都不成立。

---

## 24. 审查必修 A：部署脚本补 NGX 键 **+ 61 行其它改动**（含**退出码契约变更**）

> **为什么单独一节**：审查报告「必修 A」只要求补 9 个 NGX 键，但该脚本本轮实际是 **+61 / −8**（`git diff --numstat --ignore-cr-at-eol -- scripts/Deploy-Bridge.ps1`）。
> 变更日志此前对它的唯一提及（§8 的「键清单注释」）**不足以覆盖**后面第 2 条起的四处改动。本节按四要素补全。

**区域**：`scripts/Deploy-Bridge.ps1` 的 5 处：
`$configValues` 键表 / `$routes` 路线表 / `$stamp` / `$sha` 与成功行 / `$appliedCount` + 文件末尾的 0-路线分支。

**原因**：
1. **必修 A 原文**：`$configValues` 里一个 NGX 键都没有，而 `Dx11FsrBridge.package.ini` 新增了相关内容
   ⇒ 经部署脚本安装的环境里用户手写的 NGX 键会被写回缺失，功能「设了也没用」。
   这违反仓库 §二.7「新增键四处同步：`load_config` + 部署脚本 + `package.ini` + **变更日志条目**，
   缺任一处即视为未完成」——**第四处就是本文**，此前缺的正是它。
2. **路线表**：实机加载路线已从全机安装目录迁到用户级目录，原两条路线在本机 `Test-Path` 均为 False
   ⇒ 旧脚本「一条都铺不到，然后 exit 0」。
3. **备份名**：规范 §8.5 要求 `YYYYMMDD-HHMM`（时分两位），原格式带秒。
4. **摘要位数**：SHA256 前 8 位只有 32 bit，多份构建撞车概率不可忽略。
5. **退出码**：0 条路线成功时原脚本只打印一串「跳过」然后 `exit 0`，使用者极易误判成「已部署」。

**目标 / 实现**：

| # | 改了什么 | 改前 | 改后 |
|---|---|---|---|
| 1 | `$configValues` 补 NGX 键 | 0 个 | **8 个** + `Ffx12Backend` **故意不写**（见下）|
| 2 | `$routes` 条数 | 2 条 | **3 条**：新增 `FufuLauncherUser`（`Legacy=$false`，路径由 `$env:LOCALAPPDATA` 拼接）；`Starward` / `FufuLauncher` 保留并标 `Legacy=$true` |
| 3 | `$stamp` 格式 | `yyyyMMdd-HHmmss` | `yyyyMMdd-HHmm` |
| 4 | SHA256 摘要前缀 | 8 位 | **16 位**（部署行与回滚行都改）|
| 5 | 成功行 / 回滚行 | 只有 `路线名 + 大小 + sha` | 加**完整 DLL 路径** + `合计：N / M 条路线已部署` |
| 6 | **0 条路线成功** | 打印「跳过」+ **`exit 0`** | `Write-Host` 逐条回显 `Test-Path` 结果 + `Write-Error` + **`exit 1`** |

**键表对照（E1，可复现）**：8 个键的默认值逐键与 `Dx11FsrBridge.package.ini` 的 `[Dx11FsrBridge]` 段一致 ——
`NgxApplicationId=1337` / `NgxFeatureDirs=`（空）/ `NgxHdr=auto` / `NgxRenderPreset=0` / `NgxSetFrameTimeDelta=1` /
`NgxUseReactiveMask=0` / `NgxLogLevel=1` / `NgxPassthrough=0`。
段位：`Set-IniValue` 只把 9 个 `[Log]` 系键名送进 `[Log]`，其余一律 `[Dx11FsrBridge]` ——
8 个 NGX 键**都不在** [Log] 名单里 ⇒ 全部进 `[Dx11FsrBridge]`，与 ini 一致。

**`Ffx12Backend` 为什么故意不写（这是刻意设计，不是遗漏）**：本脚本每次部署都跑，
若在这里写死 `native`，每次部署都会把用户已选的 `ngx` 清掉；而 NGX 失效是**静默**的（回落 native 只是性能/画质变化，不崩不报错）。
缺键时代码内建缺省就是 `native` ⇒ 不写等于同样的默认行为，但不会覆盖用户选择。
⇒ 9 个键里 **8 个写入 + 1 个刻意略过**，与「9 个键四处同步」的**形式**不同，**语义**一致。这一点必须在审核时认下来。

**⚠️ 必须如实记的两项（不要粉饰）**：

1. **`exit 1` 是对外契约的破坏性变更**。改前：0 条路线成功也返回 **0**，任何把它当成功判据的调用方都拿到「成功」；
   改后返回 **1**。⇒ 依赖 `$LASTEXITCODE` / `$?` 判断部署结果的调用方**会因此开始失败**。
   仓库内的调用方已核：`scripts/` 与 `.github/workflows/` 里**没有任何**文件调用本脚本并检查退出码
   （检索：`grep -rn "Deploy-Bridge" --include="*.ps1" --include="*.bat" --include="*.yml" .`）；
   仓库外用 `.cmd`/`&&` 串联本脚本的调用方**无法在仓库内枚举** ⇒ 这部分**未核**，见下方 NOT VERIFIED。
   实现细节：`Write-Error` 必须显式带 `-ErrorAction Continue`，否则（`$ErrorActionPreference='Stop'`）会先抛终止错误、
   跳过下一行的 `exit 1`，退出码就落到调用方手里。
2. **`FufuLauncherUser` 的 `Dir` 由环境变量拼接**：
   `Join-Path $env:LOCALAPPDATA 'Programs\FufuLauncher\...'`，**不写死任何本机绝对路径**，
   与规范 §四.I「提交信息/面向 GitHub 的文案不要出现本地路径」一致。
   该变量为空（非 Windows 或被清空）时自动跳过这条路线，不抛错。
   用户级安装目录本就随用户而异，环境变量拼接是该目录的正确表达形式。

**结果**：

- **PS 5.1 语法解析 PASS**：`[System.Management.Automation.Language.Parser]::ParseFile()` 返回 `PARSE_OK`（未执行部署，只解析）。
- **编码 PASS**：文件为 **UTF-8 带 BOM + LF**（`head -c 3` = `efbbbf`，`bareLF` 计数 = 全文行数）。BOM 缺失会被 PS 5.1 按 GBK 解码 ——
  本次实测复现过：一个**无 BOM** 的中文探针脚本被 PS 5.1 报 `The string is missing the terminator: '.`，补 BOM 后立即通过。
- **退出码契约 E1**：用**复刻同一段逻辑**的独立探针（`%TEMP%` 下，未改被审文件）实测两条路径 ——
  有路线可达 ⇒ `EXITCODE=0`；0 条可达 ⇒ `EXITCODE=1`，且 `Write-Error` 确实先打印再退出。
- **三条路线的 `Test-Path` 现状（E1，2026-11-10 本机）**：`FufuLauncherUser` = **True**；
  `Starward` = **False**；`FufuLauncher` = **False**。
- **未验证**：**没有对真实 `FufuLauncherUser` 目录跑过一次部署**（不铺 DLL、不改 ini）⇒
  「写回 8 个键后 ini 的实际形态」「备份名 / 摘要位数的实际输出」**全部 NOT VERIFIED**。
  缺什么条件：需一次真实部署（会改动实机 payload 目录），本轮为只读 + 文档任务，未执行。
- **未验证**：仓库外调用方是否依赖旧的 `exit 0`。缺什么条件：需知道有哪些外部调用方，仓库内无从枚举。

**状态**：语法/编码/退出码契约 **PASS（E1）**；实际部署 **NOT VERIFIED**；本机绝对路径已移除（改用 `$env:LOCALAPPDATA` 拼接）。

---

## 25. 审查必修 B-2：CI 编译门槛（`compile-check.yml`）—— **已实跑通过**

> **本节是 §16 那条「保证」措辞的正式纠正与补登。** 变更日志此前**完全没有**必修 B-2（CI）的条目，
> 只在 §16 里用一句「由 `build-ngx-on` job 保证」带过 —— 那句在当时既是夸大、又是唯一的痕迹。
> **2026-11-10 更新：该 workflow 已实跑通过，本节据实改写。**

**区域**：`.github/workflows/compile-check.yml`（两个 job：`build-ngx-off` / `build-ngx-on`，`runs-on: windows-latest`）；
以及 `Dx11FsrBridge/CMakeLists.txt` 里引用它的注释。

**原因**：审查报告「必修 B」指出 `.github/workflows/build-release.yml` 完全没有提及 NGX 构型，
`DX11FSRBRIDGE_ENABLE_NGX` 在 CI 中既不开启也不构建 ⇒ 新增的 `NgxBackend.cpp` 与桥侧 `#if` 分支**从未被任何自动化编译过**。

**目标**：给 NGX ON / OFF 两个构型各一个 CI 编译门槛。

**结果（分三层看）**：

### 25.1 文件内容（E0，静态）

`compile-check.yml` 存在且结构完整 —— 触发条件 `on: pull_request` / `push`（`main`、`feat/**`）/ `workflow_dispatch`；
`build-ngx-off` 与 `build-ngx-on` 两个 job 各含 checkout → 拉 FidelityFX 头 → 导入 vcvars → configure → build；
ON 构型额外有「拉 NGX 头（固定 commit + 逐文件 SHA-256）」与「Verify NGX symbols present」两步。

### 25.2 实际执行（**E1，已跑通**）

2026-11-10 在维护者 fork 上以 `workflow_dispatch` 实跑**同一份 workflow 文件**（同一 commit `39f1d4e`）：

| Job | 结果 | 耗时 | 关键输出 |
|---|---|---|---|
| **NGX OFF** | ✅ success | 1m07s | `[100%] Built target Dx11FsrBridge` |
| **NGX ON** | ✅ success | 1m20s | `[100%] Built target Dx11FsrBridge`，`DLL: 640000 bytes` |

NGX ON 构型的完整步骤链全部通过：
`Fetch OptiScaler source tree (FidelityFX SDK headers)` → `Fetch NGX headers (SHA-256 pinned)`
→ `Import MSVC environment (vcvars64)` → `Configure (NGX ON)` → `Build (NGX ON)`
→ `Verify NGX symbols present`。

Run: https://github.com/dnjcgqz/genshin_fsr_brigde/actions/runs/42690967139

### 25.3 结论

⇒ **「2529 行 NGX 代码有编译门槛」这一原始缺陷已解除**（E1，有可复核的 CI 运行记录）。

⇒ **§16 里那句「由 `build-ngx-on` job 保证」在写下时是夸大**（当时该 job 从未运行过），
现已撤销并改为据实描述；撤销的不是「写错了文件」，而是「把存在的文件说成已生效的门槛」。

**状态：PASS（E1）** —— 两个 job 均已跑通。

### 25.4 一个必须留档的事实：上游 PR 上的 CI 仍显示 `action_required`

把分支推到上游后，`pull_request` 事件确实触发了 run（`42688663899`），但结论是 **`action_required`（0 秒，未执行）**。
原因不是代码或 workflow 有问题，而是：**上游仓库对首次贡献者的策略要求维护者点「Approve and run」**。

⇒ 打开 PR #12 的 Actions 页面时，看到的会是「等待批准」而不是绿色对勾。
这是**仓库策略**，不是本项缺陷的残留。上面的 success 是在 fork 上以 `workflow_dispatch` 实跑的同一份文件。

**证据等级**：**E1**（可复核的 CI 运行记录与 job 日志）。

**同族措辞（只报告、不在本次授权内修改）**：`Dx11FsrBridge/CMakeLists.txt` 的
`add_executable(Ffx12BackendTest ...)` 之前新增的注释里同样写着「NGX 路径的编译覆盖由 … 的 `build-ngx-on` job 保证」
（检索：`grep -n "build-ngx-on" Dx11FsrBridge/CMakeLists.txt`）。就 CI 现已跑通这一点而言，该措辞**已不再是夸大**，
但它属于静态断言式的表述，建议后续改为「由 CI 的 `build-ngx-on` job 覆盖（见变更日志 §25）」。

---

## 26. 本次文档修订的自我审计（T-FIX-28）

> 本节的唯一目的：把「这次改文档本身」也按同一套标准审一遍，而不是只宣称「已补齐」。
> **本节不含任何代码改动，也没有提升任何一条结论的证据等级。**

**改了什么（4 项，对应第 5 轮复审的条目）**：

| 复审编号 | 缺陷 | 写入位置 | 判定 |
|---|---|---|---|
| B-2 | 应修 C（scRGB）实质撤销，变更日志 0 处披露 | §23 + §0 汇总表 + §20 + 文首覆盖范围 | 已补**留痕**（**不是**已修复该缺陷）|
| B-3 | 必修 A 的部署脚本 61 行改动无条目 | §24 + §0 汇总表 + §20 | 已补**留痕** |
| B-4 | 「由 `build-ngx-on` job 保证」措辞过强 | §16 改写 + §25 + §0 汇总表 + §20 | 措辞已撤销，状态改为 NOT VERIFIED |
| M-2 | T-FIX-25 等 162 行 cpp 改动无条目 | **本次未做**（不在本任务授权范围）| **NOT VERIFIED** |

**仍然存在的缺陷（主动登记，不粉饰）**：

1. **m-4 未修**：§23 引用的 4 份实机日志在 `build-t6verify/` 下，被 `.gitignore` 的 `**/build-*/` 命中 ⇒ **不入库，外部无法复核**。
   建议把它们的关键行贴进 `docs/agent/evidence/`（该目录有白名单可入库）。本次未做。
2. **M-2 未修**：本轮 162 行 cpp 改动（`swapchain_control_hook_needed()` 并入 Ngx / `hooked_resize_buffers()` 的 NGX 刷新块 /
   `hooked_set_color_space1()` 的单次求值 / `load_config()` 的 fail_closed 互斥）**仍无变更日志条目**。
   本任务书只授权修 B-2 / B-3 / B-4 / MAJOR-1 四项，M-2 属另一轮。
3. **`CMakeLists.txt` 的同族过度措辞未修**：该文件不在本次授权范围内，只在 §25 里**报告**，未改动。
4. **没有新增任何实测数据**：§24 的退出码证据是「复刻同一段逻辑的独立探针」，**不是**在真实 `Deploy-Bridge.ps1` 上跑部署；
   §23 的 scRGB 行为全部是**既有日志的重新比对（E0）**，没有任何新的运行。
   ⇒ **不得把本次改动读成「缺陷已解决」**：它补的是留痕，不是验证。
5. **§23 的副作用分析是 E0**：「G10 会话上颜色空间伪装与 NGX HDR 判定同时失效」这一条是从代码路径推的，
   没有实机复现（目标游戏根本不走 G10，也无从复现）。

**证据等级**：本节所有「已补 / 已改」的结论为 **E0**（读被改文件复核）；
两份文档的行尾与编码为 **E1**（按字节读文件统计 `BOM` / `CRLF` / `bareLF`）。
