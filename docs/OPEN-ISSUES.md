# 未解决问题与待办

> 本文件是仓库内**唯一**的待办载体（见 `AGENTS.md` §一.7）。
> 只记**尚未解决**的事项，每条按「现状 / 影响 / 推进需要什么 / 相关代码位置」写全，读者不看别处也能接手。
> **改动历史不在这里**：某段代码为什么是现在这样，用 `git log` / `git blame` 查。
> 条目关闭后直接删除；要留结论就移入文末「已解决」，并注明解决它的提交。

最后更新：2026-11-13

---

## 1. 长期未定案的路线问题

- **DX11 FSR2 导出层路线**：是否把 FSR2 以导出层形式暴露，至今未定案。
- **OptiScaler `Dx11wDx12` 开启帧生成会闪退**：现象稳定复现，根因未定位。

> **2026-11-12 补充（不修改上方原文）**
>
> 关于「`Dx11wDx12` 开启帧生成会闪退」这条：
> · 该结论对应 `Ffx12Backend=native`（FFX 后端）下、以 FFX 作为帧生成输入的情形。
> · 2026-11-12 实测 `Ffx12Backend=ngx` + `FGInput=Upscaler` + `FGOutput=DLSSG` + HDR10，
>   连续多轮累计数万帧无崩溃，说明该闪退不是帧生成整体的限制。
> · 两条后端路径的 FG 实现不同，需分别验证；上方描述的 FFX 路径闪退本次未复测。

---

## 2. 代码注释里残留的悬空编号与章节引用

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

**评估结论（2026-11-14）**

**一、成因判断需要更正**

§2 称这些编号「来自合并前的外部审查与整改记录」。这个说法只对一部分成立：

| 引用 | 真实来历 |
|---|---|
| `AGENTS §8.5`、`§15`、`§16.1-3` | 旧版 AGENTS.md 的真实章节 |
| `T-FIX-*`、`R-5 MAJOR`、`R-9 MINOR-*`、`D-1/D-2` | 确为外部审查记录，从未随代码入库 |

第一行指的是旧版 AGENTS.md 里真实存在的 `§8.5 部署与回滚纪律`、`§15 已知无效线索（不要重试）`、
`§16.1 写 C/C++ 源码`：这些引用写下时准确且可原地查证，是 AGENTS.md 改用 §零–§六 编号之后才失效。

这个区别决定修法不同：前者应**就地写出旧章节的结论**（现 AGENTS.md 已无这些节，内容整体删除，
无法换成现有章节号）；后者**删掉编号前缀、保留结论**即可。

**二、数量与范围**

- 原条目列的 20 个位置在写下时准确，但代码演进后行号已漂移（`NgxBackend.cpp` 从约 2,930 行增至 4,341 行）。
- 实际存量约 30 行，且原条目**漏列**了裸 `§15` 一处，以及若干纯审核过程叙述的段落。
- 原条目的方案二（保留编号并就地补图例）**不应采用**：那些审核过程叙述（谁在第几轮改了什么、
  用什么编译器验证）本身就是改动历史，写进注释违反 AGENTS.md 中「改动历史不进仓库」的约定。

**三、2026-11-14 的清理**

共 27 处（`NgxBackend.cpp` 19、`Dx11FsrBridge.cpp` 7、`NgxBackend.h` 1），只改注释、不动逻辑：

- 旧版 AGENTS.md 章节引用 → 就地写出结论。例如 `va_list` 那条写成「只能消费一次，
  第二次消费会被 CRT 以 `0xC0000409` 终止」。
- 外部审查编号 → 删前缀保留结论；纯审核叙述压缩成一句结论。

验证：剥掉全部注释后逐行比对，三个文件的非注释代码行与改动前完全一致（2268 / 13147 / 30 行），
DLL 大小 660,480 B 未变。

**四、未纳入 2026-11-14 清理范围**

`SDS §x.x` 的 27 处引用**刻意未动**。那份设计文档（`docs/SDS_NgxBackend.md`，3,253 行）存在于归档分支
`archive/feat-ngx-backend-20261110`，由 `c3bd66a` 引入、从未删除，文档内定义了代码里全部编号体系：
`S-n` 变更编号、`E0`–`E3` 证据等级、`M2`–`M4` 里程碑、`T-n` 任务编号、`D-n` 审核发现。

因此这些引用**不是悬空引用**，清理会破坏信息。真正的问题是这份文档不在主分支上 ——
从主分支视角读者无法原地自解，属文档归属问题而非注释问题。

另注：`B1xx` 编号（约 230 处）在 `Dx11FsrBridge.cpp` 文件头有就地图例
（「本项目历次变更批次的编号，每个编号的实质内容即紧随其后的注释段落」），符合就地定义要求，同样不应清理。

---

## 3. 上游 OptiScaler 的已知缺陷（非本仓库代码，本仓库无法修复）

### 帧生成开启 HDR 时画面发灰（2026-11-12）

**结论：已定位为上游 OptiScaler 缺陷，本仓库已用本地补丁绕过并实测通过。**

**现象**：HDR10 输出 + `FGInput=Upscaler` + `FGOutput=DLSSG`（XeFG 相同）时，画面整体发灰、
对比度塌陷；把 `FGOutput` 改成 `nofg` 立刻恢复正常。与是否真的开启插帧无关 ——
即使在菜单里不启用帧生成，只要 `FGOutput != nofg` 就发灰。

**根因**：`Dx11wDx12` 互操作路径下，真正上屏的是 FG 交换链：

```
Dx11wDx12SC::Present
  _CopyDx11BackBufferToShared(...)         D3D11 后缓冲 -> 共享纹理
  _CopyDx11SharedToDx12FGBackBuffer(...)   共享纹理 -> D3D12 FG 后缓冲
  _fgSwapChain->Present(...)               实际呈现
```

但 `_fgSwapChain` 的 HDR 色彩空间**从未被设置**：游戏调用的是 `WrappedIDXGISwapChain4::SetColorSpace1`，
它只转发给真实 D3D11 交换链；`Dx11wDx12SC::SetColorSpace1` 虽实现了转发到 `_fgSwapChain` 的逻辑，
但实测调用次数为 **0**。于是后缓冲是 PQ 编码、呈现链按 SDR 解释，画面发灰。

**修复**：在 `Dx11wDx12SC::Present` 首帧把 `State::outputColorSpace`（`WrappedIDXGISwapChain4`
记录的游戏设置值）同步给 `_fgSwapChain`，并加 `_fgColorSpaceSynced` 保证只做一次。
补丁 +34 行，改 `dx11_with_dx12_sc.cpp` / `.h` 两个文件。

**验证**：修复后日志出现 `FG swapchain colour space set to 12`（12 = `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020`），
仅在首帧一次；实机确认不再发灰。对照：官方 nightly-20261106（commit `d6b02459`）原版同样发灰，
加此补丁后正常。

**上游状态**：已检索 OptiScaler 全部 issue（`HDR+swapchain` / `HDR+framegen` / `HDR state:open` / `washed`），
无相关报告；上游 master 至今仍是原始实现。

**复现与维护要点**：

- 构建必须用与已部署依赖匹配的 commit。用 master HEAD 会因 59 个提交差异导致 Streamline
  初始化崩溃（日志 `Exception detected` + 迷你转储）。
- 项目默认 `PlatformToolset=v143`（VS 2022）；只有 VS 18 的环境需改为 `v145`。
- 上游若修复此处，本地补丁即可移除。

### 游戏退出后进程卡死需手动结束（2026-11-12）

**结论：上游 OptiScaler 缺陷，本仓库无法从桥侧修复。已归档，不再投入。**

**现象**：关闭游戏窗口后 `YuanShen.exe` 残留，占用 4–5 GB 内存，必须任务管理器
「结束任务」才能再次启动。每次退出都发生。

**对照实验**：关闭 OptiScaler 后进程 3 秒内正常退出；开启时残留进程表现为
线程数 1、模块数 0、`Stop-Process -Force` 被拒绝（内核已标记 terminating）。

**根因定位**：OptiScaler.log 最后一行是 `DLSSG_Common::Deactivate`，之后无输出。
该函数（`framegen/dlssg/DLSSG_Common.cpp:32`）以
`queueParallelismMode = eBlockPresentingClientQueue` 调用 `sl::DLSSGSetOptions()`，
上游在此处自带注释 `// Potential crash point on exit`。进程退出期间 present 队列
已不可用，于是永久阻塞。

配套的三个结构性问题（均在 `dllmain.cpp`）：`DLL_PROCESS_DETACH` 里所有 hook 卸载
被注释（注释说明"causing issues during shutdown"）；`CreateThread(getGpuInfo)` 句柄
丢弃无法等待；`isShuttingDown` 只是标志，没有真正的线程 join。

**上游状态**：issue #512（2025-06-09，已关闭）报告了同一现象，维护者回复
"known issue ... Will be eventually fixed"，此后 5 个月无修复。

**为何不修**：阻塞发生在 OptiScaler 自身的 DLL 卸载路径，桥无法介入。
完整证据链见本地留痕 `VERDICT_game_exit_hang_20261112.md`（不随仓库分发）。

## 4. DLSS 选档的**实机验收**尚未完成（机制已实现）

**现状**

- 选档判据已改为向 NGX 查询合法 render 尺寸区间：`Dx11FsrBridge/NgxBackend.cpp` 的
  `query_dlss_optimal_range()`（照 `NGX_DLSS_GET_OPTIMAL_SETTINGS` 的逻辑重写）、
  `refresh_optimal_ranges()`（六档各查一次，按 display 尺寸缓存）、
  `resolve_create_quality()`（挑区间包含当前 render 尺寸的那一档；都不包含或查询失败则
  回退 `quality_from_ratio()` + `perf_quality_for_create()`）。纯计算部分在
  `Dx11FsrBridge/NgxParamMap.h` 的 `render_range_contains()` / `pick_quality_from_ranges()`。
- 三个调用点同源：`create_feature` 写 `PerfQualityValue`、`diag_check_create_params` 的回读期望值、
  以及 `make_feature_input_key_from` 的 `perf_quality` 字段 —— 都调 `resolve_create_quality()`。
- 已在本机做过全档位实机核对（见下方「实机验证」）：9 个非默认渲染精度全部
  `create_feature ok=1`。但**仍只覆盖一台机器、一个 DLSS 版本、一种显示分辨率**；
  `NgxParamMapTest` 能证明选档器的行为（区间包含、容差、重叠、回退），
  仍证明不了「真实驱动返回的区间在本机之外的显卡 / DLSS 版本上也成立」。

**影响**

- 若驱动返回的区间与假设不符，可能出现三种情况，日志里都能区分：
  ① `pq_select mode=ratio reason=query_unavailable` ⇒ 查询本身不可用（可能是
  `shared_singleton`，即 provider 返回了同一个参数对象），选档退回旧比例表 ⇒ 0.8~0.999 仍会
  走 `perf_quality_for_create` 兜底；
  ② `pq_select mode=ratio reason=no_interval ranges_ok=6` ⇒ 区间查到了但都不包含当前尺寸
  ⇒ 也退回旧比例表；③ `pq_select mode=interval` ⇒ 查询路径生效。

**推进需要**

- 实机验收（需有游戏 + N 卡环境）：渲染精度依次取 0.999 / 0.9 / 0.8 / 0.7，每一个都确认
  `create_feature ok=1`，并逐条对账：`pq_range` 六行给出的区间 → `pq_select mode=interval picked=N`
  → `pq=N` → `create_feature ok=1`。
- 反向对照：把 `NgxProvider` 切到 `external`（OptiScaler），确认若对方返回单例参数对象，
  日志出现 `pq_query_params ok=0 reason=shared_singleton` 且选档退回比例表 —— 即
  「宁可不查，也不污染共享对象」这条防线确实生效。
- 记录至少一台机器、一个 DLSS 版本的 `pq_range` 六行原文：这是判断「区间是不是比例」的
  唯一一手数据，也是本机制日后回归的基线。

**注意**

- 只改「选哪个档」，不改 create flags、不碰资源与实例生命周期。
- 三个调用点必须走同一条选档链，否则 `set_mismatch` 会误报。
- 判据：渲染精度 0.8~0.999 时 `create_feature ok=1`，且日志里的 `pq=` 与区间查询结果一致；
  对照 0.999 旧行为是 `ok=0 rc=0xBAD00010`。
- 容差按 `upper/64` 计算（与原文「约 2 像素」不符，见 `range_upper_tolerance` 的注释）；
  `pq_range` 行会打出实际容差，实机可据此校核。
- **NGX 的 optimal settings 与 `CreateFeature` 的校验可能不是同一套规则**（推断，未验证）。
  `NGX_DLSS_GET_OPTIMAL_SETTINGS` 返回的是「该档能接受哪些 render 尺寸」，而 `CreateFeature`
  另有自己的 `PerfQualityValue` × 分辨率校验；两者是否逐条一致，本轮**没有**一手证据。
  - 已守住的一个方向：区间选出的档**仍要过一遍** `perf_quality_for_create`（UltraQuality → MaxQuality），
    所以即使驱动报「UltraQuality 区间包含你」，也不会把 UltraQuality 送进 NGX。
  - **反向风险仍在**：若区间查询比 `CreateFeature` 宽松，就可能选到更激进的档；
    `perf_quality_for_create` **拦不住**这种情况（它只拦 UltraQuality 一个值）。
    推断的后果与今天同形：`create_feature ok=0 rc=0xBAD00010`（§5 的重试路径会兜住一部分）。
  - 缓解：选档规则是「**最贴近 optimal** 的那一档」（`pick_quality_from_ranges` 按 optimal 尺寸
    与实际 render 尺寸的距离取最近），**不是**「最接近区间上限的那档」⇒ 结果倾向保守而不是激进。
  - 目前唯一能证实或证伪它的手段仍是**实机**：看 `pq_select mode=interval picked=N` 之后是否
    `create_feature ok=1`（见上文「推进需要」）。

**相关代码**

- `Dx11FsrBridge/NgxBackend.cpp`：`query_dlss_optimal_range` / `refresh_optimal_ranges` /
  `resolve_create_quality` / `ensure_query_params` / `release_query_params`；
  `Dx11FsrBridge/NgxParamMap.h`：`render_range_contains` / `pick_quality_from_ranges`；
  `Dx11FsrBridge/NgxParamMapTest.cpp` 第 [10] 节；`Dx11FsrBridge/third_party/ngx/nvsdk_ngx_defs.h`

**实机验证（2026-11-13 21:51，N 卡 + 驱动自带 NGX）**

只跑了一次有效会话（渲染精度 0.999），一次创建成功。日志原文：

```
pq_range pq=0 name=MaxPerf          dw=3840 dh=2160 opt=1920x1080 min=1920x1080 max=3840x2160 tol=60x33
pq_range pq=1 name=Balanced         dw=3840 dh=2160 opt=2227x1253 min=1920x1080 max=3840x2160 tol=60x33
pq_range pq=2 name=MaxQuality       dw=3840 dh=2160 opt=2560x1440 min=1920x1080 max=3840x2160 tol=60x33
pq_range pq=3 name=UltraPerformance dw=3840 dh=2160 opt=1280x720  min=1280x720  max=1280x720  tol=20x11
pq_range pq=5 name=DLAA             dw=3840 dh=2160 opt=3840x2160 min=3802x2138 max=3840x2160 tol=60x33

pq_select mode=interval picked=5 picked_name=DLAA pq=5 pq_name=DLAA r=3836x2156 d=3840x2160 tol=60x33 ranges_ok=5
create_feature ok=1 preset=0 rc=0x1
```

核实到的事实：

- 查询路径生效：`mode=interval`，无 `mode=ratio` / `query_unavailable` / `shared_singleton`
  ⇒ 独立参数对象可用，「宁可不查也不污染 `g_params`」这条防线未被触发。
- **0.999 精度（ratio 1.001）由区间查询选中 `DLAA` 并 `ok=1`**，而不是靠
  `perf_quality_for_create` 把 UltraQuality 硬改成 MaxQuality ⇒ 治本路径在本例有效。
  对照旧行为：同一精度在 `b632cc5` 之前的失败**据该提交的说明**是 `ok=0 rc=0xBAD00010`，
  但本次会话日志里没有该精度的失败记录可供复现（`b632cc5` 的提交说明记载它来自
  另一份未随仓库分发的环境日志）。
- **`ranges_ok=5` 而非 6**：本机驱动没有返回 `pq=4`（UltraQuality）的区间行，
  即该档查询无结果。`pick_quality_from_ranges` 只在已查到的档里挑，不影响正确性，
  但说明「六档全查到」不是普遍情况，回退路径要能容忍部分档缺失。
- **容差是分档的，不是统一值**：`pq=0/1/2/5` 是 `60x33`（`3840/64`、`2160/64`），
  而 `pq=3` 是 `20x11`（它的上限只有 1280x720）。与 `range_upper_tolerance(upper)=upper/64`
  的实现一致，也再次说明原文「约 2 像素」不成立。

**全档位实机核对（本机 3840x2160 + 驱动自带 NGX，两次会话）**

9 个非默认渲染精度全部走过，`create_feature ok=1` 9/9，零失败。
其中 0.2~0.9 在 2026-11-13 23:12–23:21 两次会话内测得；0.999 来自同日 21:51 的
另一次会话（该会话只有这一个精度）。

| 精度 | render | ratio | mode | 选中 | 结果 | 会话 |
|---|---|---|---|---|---|---|
| 0.2 | 768x432 | 5.00 | `ratio` | UltraPerformance | `ok=1` | 23:13 |
| 0.3 | 1152x648 | 3.33 | `ratio` | UltraPerformance | `ok=1` | 23:13 |
| 0.4 | 1536x864 | 2.50 | `ratio` | UltraPerformance | `ok=1` | 23:13 |
| 0.5 | 1920x1080 | 2.00 | `interval` | MaxPerf | `ok=1` | 23:20 |
| 0.6 | 2304x1296 | 1.67 | `interval` | Balanced | `ok=1` | 23:20 |
| 0.7 | 2688x1512 | 1.43 | `interval` | MaxQuality | `ok=1` | 23:20 |
| 0.8 | 3072x1728 | 1.25 | `interval` | MaxQuality | `ok=1` | 23:21 |
| 0.9 | 3456x1944 | 1.11 | `interval` | MaxQuality | `ok=1` | 23:21 |
| 0.999 | 3836x2156 | 1.001 | `interval` | DLAA | `ok=1` | 21:51 |

- **分界线在 ratio 2.0**：`≤ 2.0` 走 `interval`（render 落在某档区间内），
  `> 2.0` 走 `ratio` 回退（`reason=no_interval`）。
  回退路径已实测可用，不是纸上设计。
  注：`> 2.0` 的两个具体成因不同 —— 0.2/0.3 低于所有档位的 `min`；
  0.4（1536x864）则是**高于** `pq=3` 的 `max+tol`（1300x731）、
  又低于其余档位的 `min`（1920x1080），同样落空。
- **0.2 / 0.3 的档位归属（2026-11-13 改）**：两者 `ratio > 3.0`，原本落
  `quality_from_ratio` 的 `else` ⇒ DLAA（不放大）。改成 UltraPerformance 的理由是
  **语义一致**：3.3x / 5.0x 属于极端降采样，对应最高档的 UltraPerformance，
  而 DLAA 是「不放大」的档位。这是一次判据对齐，**不是画质改善**。
  实测这两个精度声明 UltraPerformance 后 `create_feature` 均 `ok=1`；
  与改前声明 DLAA 相比画面观感差异很小，未做 A/B 量化对比 ⇒ **画质优劣未验证**。
  `ratio ≤ 1.0`（原分辨率 / 超采样）仍返回 DLAA，两个分支刻意分开。
- ★ **一条被证伪的推断**：此前认为「实际倍率超出档位名义能力时 NGX 会拒绝」
  （0.2 需 5x、0.3 需 3.3x，而 UltraPerformance 名义 3x）。实测两者均 `ok=1`
  ⇒ NGX 未拒绝。证据只有这两个尺寸，不要据此断言 NGX 对所有尺寸都放宽。

**未覆盖**（不得据此认为已充分验证）：

- 全部数据来自**一台机器、一个 DLSS 版本、3840x2160 一种显示分辨率**。
- 反向对照（`NgxProvider=external` 时对方返回单例 ⇒ `shared_singleton` 回退）**未做**。
- §4「注意」里那条「区间可能比 CreateFeature 宽松」的反向风险，本轮反而得到**间接佐证**：
  `UltraPerformance` 的区间是 `min=max=1280x720`（一个点），而 0.3/0.4 声明该档仍
  `ok=1` ⇒ `CreateFeature` 的校验确实比 optimal settings 宽松。

---

## 5. `create_feature` 参数性失败的自愈路径**未经实机验证**（机制已实现）

**现状**

- 机制已实现（见文末「已解决」的《`create_feature` 参数性失败按输入 key 重试》）：`0xBAD00010`
  （`UnsupportedParameter`）失败不再置 `g_release_all`，改为记录一份输入 key；key 变化时重试一次。
- 已在本机做过一次实机验证（见下方「实机验证」）：失败 → 记录 key → 改精度 → 重试成功，
  全程无需重启游戏。但那次失败是**用临时诊断开关构造的**——正常路径下 §4 的区间查询
  已能选到合法档位，自然状态下构造不出 `0xBAD00010`，因此该路径在真实玩家的
  「参数配错」场景中是否同样表现良好，仍缺观测。

**影响**

- 若判据有误，可能的表现是：① 自愈不生效（日志有 `key_fail retry_eligible=1` 但**没有**
  `key_retry` 行 ⇒ 说明 key 判据恒假）；② 自愈了但不出图（有 `key_retry_result ok=1` 而
  `DISPATCH_OK` 仍为 0）；③ 参数持续失败时反复重试造成卡顿（`key_retry` 行的出现频次
  高于每 2 秒一次）。

**推进需要**

- 实机验收（需有游戏 + N 卡的环境）：
  1. 把渲染精度调到会造成 `create_feature ok=0 rc=0xBAD00010` 的档位（历史实测 0.8 / 0.9 / 0.999），
     确认日志出现 `create_feature key_fail … cfmt=<n> ofmt=<n> rc=0xBAD00010 retry_eligible=1`；
     ★ `retry_eligible=1` 这个字段是**代码侧**补上的验收判据（原代码没打它 ⇒ 本条 grep 恒 0 命中）。
  2. 改回安全精度（如 0.5 / 0.7），确认出现 `key_retry` 与 `key_retry_result ok=1`，
     且随后 `DISPATCH_OK` 递增、画面出 DLSS；
     ★ 同时确认**不再**每帧刷 `ngx_hdr_decision`（该行在失败判据路径上已静默，见 §5 的 `log_decision`）。
  3. 反向对照：不改任何设置，确认**不会**出现 `key_retry`（key 相同必须不重试）。
  4. 冻帧对照（第 1 条修复）：确认失败发生后**不出现**「画面冻在最后一帧最多 500ms」。
     机制：白名单 rc 不置 `g_release_all`，故 `fsr2_family_should_skip_draw` 那道 gate
     不会关闭；现在由 `create_feature` 失败分支当场调
     `fsr2_family_takeover::notify_accumulate_result(false, …)` 解除「跳过预处理 pass」许可。
- 若第 3 步不成立（即不做任何操作也反复重试），说明某个每帧变化的量混进了 key，
  必须查 `CreateFeatureInputKey` 的 10 个字段是否都在运行期稳定
  （`NgxParamMapTest` 第 [11] 节对每个字段各有一条敏感断言，加字段必须同步加断言）。

**相关代码**

- `Dx11FsrBridge/NgxBackend.cpp`：`CreateFeatureInputKey` / `same_feature_input_key` /
  `kFeatureCreateRetryMinMs`（结构定义与比较）、`make_feature_input_key[_from]`、
  `record_feature_create_failure`（记录 key）、`create_feature` 的 `rr != Success` 分支
  （rc 白名单）、`dispatch` 第 5 步之后的 `feature_create_failed` 分支（重试判据与三行日志）。

**实机验证（2026-11-13 22:02–22:03）**

四条验收项逐条通过，日志时序与结论见文末「已解决」的
《`create_feature` 参数性失败按输入 key 重试》。

- 失败被复现：`key_fail … retry_eligible=1`（`rc=0xBAD00010`）。
- 改精度后出现 `key_retry` 与 `key_retry_result ok=1`，随后出图。
- 反向对照：精度不变时**没有** `key_retry`（22:02:48 那次失败未触发重试）。
- 冻帧对照：失败瞬间出现 `family_release … cleared=1`，未观察到冻帧；该行只打一次。

复现方式：正常路径下 §4 的区间查询已能选到合法档位（实测 0.999 精度
`picked=DLAA` → `create_feature ok=1`），自然构造不出 `0xBAD00010`；
2026-11-13 验证时用临时诊断开关强制走旧选档造失败，该开关验证后已删除、未进入提交。

---

## 6. 焦点日志块只描述 FFX 直连路径，看不出后端与真正上屏的超分模型

**现状**

- `Dx11FsrBridge/Dx11FsrBridge.cpp:3397-3411` 的 `log_focus_block()` 固定输出三行
  `GPU :` / `SDK :` / `FSR : FSRx.y.z [OK|FAILED]`，取 `g_route_*`（`:594-596`，由
  `record_adapter_info()` `:15379-15396` 写入）与 `ffx12::matched_version_name()`。
- 只有两个调用点，都在 FFX 直连派发上：`:13028`（首次 dispatch 成功）与 `:13305`
  （首次失败，`sdk234_fail_count==1`）。`Ffx12Backend=ngx` 时不走该路径 ⇒ **整块不出现**，
  只有 `ngx_core …` 的零散行（provider / featuredirs / init / create_feature）。
- 桥能确定的事实只有四类：自身后端（`ffx12::backend()`）、自己派发的 FSR 版本
  （`matched_version_name()`，含 4.x→3.x→2.x 降级）、NGX provider（`NgxBackend.cpp:1471`
  的 `hit.kind` / `hit.path`，目前**只打日志、未持久化**）、NGX 特征目录
  （`resolve_feature_dirs()` `:947-976`，`PathListInfo` 首项 = `g_feature_dir_storage.front()`）。
- `nvngx_dlss.dll` 的模型版本目前没有任何一行：仓库不链 `version.lib`，全库也没有
  `GetFileVersionInfo` / `VerQueryValue` / `VS_FIXEDFILEINFO`。

**影响**

- 「这局到底谁在超分」在日志里答不出来：FFX 模式写 `FSR`，但外接 OptiScaler 时最终上屏
  实现可能不是桥；NGX 模式则完全没有摘要块，只能翻 5~6 行分散日志。
- 装/换 `nvngx_dlss.dll` 后想确认「新的 DLSS 到底生效没有」，现在没有任何一手判据。

**推进需要**

P1（只报事实，可独立验收）

- 焦点块加 `BACKEND :` 行（`native` / `ngx`；`Ffx12Backend=auto` 时带 `g_backend_auto_note`
  `:15416` 的 `auto=… vendor=…`），现有 `SDK :` 行改名为 `FFX SDK :` 以区分后端。
- NGX 侧新增焦点块，触发点必须用**一次性闸门**（`create_feature` 每次 key 变化都会重跑，
  `:1884` 不能直接当触发点）：块内 `BACKEND : ngx` + `UPSCALER : NVIDIA DLSS <FileVersion> [OK]`。
- `ngx::` 增加两个只读 getter：provider kind/path（把 `:1471` 的 `hit.*` 存全局，与现有
  `active()` / `available()` 同级），以及 DLSS 运行时版本。
- DLSS 版本取「传给 NGX 的首个特征目录里 `nvngx_dlss.dll` 的 FileVersion」：实测
  `payload\NVIDIA\DLSS\nvngx_dlss.dll` 的 FileVersion 为 `310,9,16,0`，打印成 `DLSS 310.9.16`。
  初始化路径上读一次缓存即可（非热路径）。
- 读版本需要 `version.dll`：`GetFileVersionInfoSizeW` → `GetFileVersionInfoW` → `VerQueryValueW`。
  新增 import 还是 `GetModuleHandleW` + `GetProcAddress`（不加 import）在 `CMakeLists.txt`
  里写明选择；两种都只能读**文件**版本，不代表已加载模块在内存里的版本。
- provider 为 `external`（OptiScaler 之类别家实现）时打
  `UPSCALER : <模块文件名> 外部 NGX 实现（实际模型由该实现决定）`——不猜模型名。
- 该块用 `log_focus_line()`（`cat::core` INFO），不要用 `log_ngx_error()`：
  `NgxLogLevel=0` 会把后者一起关掉，摘要不该受单后端日志等级影响。

P2（可选，只报配置意图，与 P1 的测量行分开）

- 已有现成读取器 `unsafe_dx11_on12_backend_selected()` `:9318-9357`：读
  `<exe 目录>\OptiScaler.ini` 的 `[Upscalers] Dx11Upscaler`（带 250 ms 限流缓存）。
  把「读值」抽成独立函数，块内加
  `COEXIST : OptiScaler Dx11Upscaler=<值> source=optiscaler_ini(未核实)`。
- 该行只能说明 OptiScaler 被**配置**成什么，不能说明它这局真的用了；不得与 P1 的测量行
  混排（AGENTS.md §五.1：判据与动作分离，观测源必须是事实源头）。

P3（可选）

- 桥未接管时也打一次块（`state=SKIP` + 跳过原因），避免「日志里没有块 = 一切正常」的误读。

**注意**

- 所有取值一次性缓存，块走一次性闸门。禁止在每帧 / 每 draw 读 ini 或文件版本。
- `matched_version_name()` 是**桥自己派发**的版本，不是游戏原本请求的版本
  （另有 `selected_version_name()` `Ffx12Backend.cpp:3164`，两者别混用）。
- 块排版沿用现状（分隔线上下各一个空行、`  KEY : ` 两空格 + 键 + 分隔符）；
  行数变化若被文档或测试引用，需同步。
- README 的验收判据里已有 `ngx_core provider resolved=… module=…` 行；新块引用同一事实时
  复用该行的字段名，不要再造第二套口径。

**相关代码**

- `Dx11FsrBridge/Dx11FsrBridge.cpp:594-596`、`:3397-3411`、`:9318-9357`、`:13028`、`:13305`、
  `:15379-15396`、`:15407-15432`
- `Dx11FsrBridge/NgxBackend.cpp:947-976`、`:1104-1106`、`:1422-1472`、`:1884`；`Dx11FsrBridge/NgxBackend.h`
- `Dx11FsrBridge/Ffx12Backend.cpp:2633-2721`、`:3164`、`:3169-3172`

**评估结论（2026-11-14）**

**一、事实核验：6 条断言与代码不符**

逐条复核「现状」段，有 6 处描述与代码不符。**下列行号均指修复前的代码位置**
（`f945231` 之前；修复后 `log_focus_block` 在 `:3413`，两个调用点在 `:13063` / `:13340`，
`record_adapter_info` 在 `:15414`）：

1. 「`Ffx12Backend=ngx` 时整块不出现」——**实际会出现**，且报的是未被使用的 AMD SDK 与
   `FSR?`（详见下节，这是要修的真实缺陷）。`ffx12::dispatch()` 在 ngx 模式下同样返回 true
   （`Ffx12Backend.cpp:2934-2956` 分流后走 `ngx::dispatch`），`Dx11FsrBridge.cpp:13028` 的成功分支
   照常执行 ⇒ 块照常打印。
2. 「全库没有 `GetFileVersionInfo`/`VerQueryValue`」——`FufuGraphicsPlugin/FufuGraphicsPlugin.cpp:252-269`
   已有完整实现，`FufuGraphicsPlugin/CMakeLists.txt:41` 已链 `version`；只有 `Dx11FsrBridge` 没链。
3. `resolve_feature_dirs()` 位置——实际在 `NgxBackend.cpp:1068-1098`；`:947-976` 是三个日志闸门。
4. `NgxBackend.cpp:1471` 的 `hit.kind` / `hit.path`——该行是 `g_init_retry_after_us`；真正赋值在
   `:1378-1379` / `:1415-1416` / `:1439-1440`，唯一消费点 `:1597-1598`。「只打日志、未持久化」
   属实：`ProviderHit` 是 `ensure_init` 内的局部量（`:1551`）。
5. `:1884` 不能当触发点——该行是 `build_create_flags` 里的 `cf.depth_inverted`；`create_feature`
   定义在 `:2500`，调用点 `:4041`。
6. 「DLSS 版本没有任何一行」——日志里已有
   `[NGXSecureLoadFeature:1621] ... nvngx_dlss.dll version: 310.9.16`，由 `ngx_log_cb`
   （`NgxBackend.cpp:984-998`）原样透传。但它受 `NgxLogLevel>0` 与回调内 200 条硬限流约束，
   `NgxLogLevel=0` 时整条消失。

**属实的断言（已核验）**

- `log_focus_block` 的位置与三行内容（`Dx11FsrBridge.cpp:3397-3411`）、两个调用点（`:13028`、`:13305`）、
  `g_route_*`（`:596` 起）、`record_adapter_info`（`:15379-15396`）。
- `unsafe_dx11_on12_backend_selected()`（`:9318-9357`）读的 ini 键 `[Upscalers] Dx11Upscaler`
  与 250 ms 限流缓存。
- `log_focus_line`（恒 `core` INFO）与 `log_ngx_error`（恒 Error、无计数闸门）的等级差异。
- `matched_version_name()`（`Ffx12Backend.cpp:3169`）与 `selected_version_name()`（`:3164`）并存，
  且前者只在 native 侧匹配成功时被赋值（`:2710`）——ngx 模式下它保持初值 `"?"`。
- 实测 `nvngx_dlss.dll` 的 FileVersion = `310,9,16,0`，条目里的版本号准确。

**二、真实问题比条目描述的更严重**

不是「缺摘要」，是「有摘要，但内容是错的」。`backend=ngx` 的会话实测：

```
  GPU : NVIDIA GeForce RTX 4070 SUPER (vendor=0x10DE)
  SDK : ...\payload\AMD\amd_fidelityfx_upscaler_dx12.dll
  FSR : FSR?  [OK]
```

同一次会话里 `ngx_core provider resolved=nvidia_registry module=...\_nvngx.dll`——真正上屏的是
DLSS，块里报的却是 AMD 的 FFX SDK 与 `FSR?`。读日志的人会据此认定「这局在跑 FSR」，方向正好相反。

原因：`ffx12::dispatch()` 在 ngx 模式下同样返回 true（`Ffx12Backend.cpp:2934-2956` 分流后走
`ngx::dispatch`），所以焦点块的触发条件成立；而块里取的 `g_route_sdk_path`（由
`record_adapter_info` 从 `g_config.ffx12_dll_path` 抄来）与 `matched_version_name()` 在 ngx 下
从未被赋值 ⇒ 两行都是「默认值」而非「事实」。

**三、已做的修复**

四处改动均已落到工作区，构建与单测通过（证据见节末）：

- 焦点块加 `BACKEND :` 行（`native` / `ngx`；`Ffx12Backend=auto` 时带 `g_backend_auto_note`
  的 `auto=… vendor=…`），排在 `GPU :` 之后。后端取 `ffx12::backend()` 的真值而非 ini 字面量
  ——`Ffx12Backend=auto` 时 ini 里写的是 `auto`，实际落位由型号决定。
- `SDK :` → `FFX SDK :`、`FSR :` → `FFX :`；ngx 模式下这两行明确标注 `(unused, backend=ngx)`，
  不再把未使用的 AMD SDK 与 `FSR?` 当成「这局在用什么」；native 模式下保持原三行语义不变。
- `ngx_core init ok=1` 行追加 `dlss=<版本>` 与 `provider=<kind>`：`ProviderHit` 的 `kind`
  提升为全局供该行读取；DLSS 版本取**已加载模块**的 FileVersion（`GetModuleHandleW` +
  `GetModuleFileNameW` + 版本 API），模块未加载时才退回特征目录里的文件。取不到打 `dlss=?`，
  不省略字段，保持行格式稳定便于脚本比对。版本只在首次初始化成功时读一次（锁存），
  不在每帧路径上。
- `Dx11FsrBridge/CMakeLists.txt` 链 `version` 库。用直接链接而非 `GetProcAddress`：
  少一条运行期失败路径，且依赖在加载期就暴露，不必等第一次读版本时才发现
  （与 `FufuGraphicsPlugin/CMakeLists.txt` 同写法）。
  ⚠️ 风险面如实记录：`version.dll` **不是** KnownDLL（实测 KnownDLLs 共 38 项，其中无 VERSION），
  这条 import 走标准 DLL 搜索顺序，**游戏 exe 目录优先于 System32**。
  这与仓库既有的 `user32` / `d3d11` / `dxgi` 等 import 属同一类风险面，不是本改动新引入的类别。

改后 `backend=ngx` 的会话里，焦点块输出四行：`GPU : …` / `BACKEND : ngx` /
`FFX SDK : (unused, backend=ngx)` / `FFX : (unused, backend=ngx)`；
`ngx_core init ok=1 … dlss=310.9.16 source=loaded provider=nvidia_registry`
给出真正上屏的实现与版本。

**构建核验（2026-11-14）**

- `cmake --build <构建目录> --clean-first`：exit 0，0 error / 0 warning，`Building CXX object`
  11 条（含 `NgxBackend.cpp.obj` 与 `Dx11FsrBridge.cpp.obj`，非只有 Linking），末行 `Linking`。
- `NgxParamMapTest`：3657 checks / 0 failures，ALL PASS。
- DLL 660,480 B（改前 656,896 B，+3,584 B）。产物内确有 ASCII `BACKEND : ` ×1、
  `FFX SDK : ` ×2、`(unused, backend=ngx)` ×2，以及 UTF-16 `dlss=%s provider=%s` ×1；
  `dumpbin /imports` 有 `GetFileVersionInfoSizeW` / `GetFileVersionInfoW` / `VerQueryValueW`，
  `/dependents` 有 `VERSION.dll`。
- 版本解析独立探针：`payload\NVIDIA\DLSS\nvngx_dlss.dll` → `310.9.16`；
  另有一份副本解出 `310.9.1`——这正是必须按**已加载模块**路径取版本的原因，按特征目录
  取会报错。

**实机验证（2026-11-14，两次真实 ngx 会话）**

- 焦点块输出 `GPU : …` / `BACKEND : ngx` / `FFX SDK : (unused, backend=ngx)` /
  `FFX : (unused, backend=ngx)` 四行，两次会话一致。
- `init ok=1 available=1 rc=0x1 dlss=310.9.16 source=loaded provider=nvidia_registry`。
- 零回归：两次会话均为 `create_feature ok=1` 一次、`DISPATCH_OK` 七次、`DISPATCH_FAILED` 零次。
- **`source=loaded` 有独立价值**：它证明版本确实取自**本进程已装载的模块**，而不是特征目录里的
  候选文件。两条路可能给出同一个版本号，只看 `dlss=` 的数值无法区分 —— 这正是加这个字段的原因。
  实测同时说明 NGX 在 `ensure_init` 返回前就已装载 `nvngx_dlss.dll`，故「已加载模块优先」
  这条路径真的会命中。

**尚未验证**

- **native 模式的实际输出**未实机确认。注意 native 下的块**并非**与改前逐字一致：
  `BACKEND` 行无条件打印、`SDK :`→`FFX SDK :`、`FSR :`→`FFX :` 同样作用于 native，
  故 native 从三行变四行、两行改名。
- `hit.path` **未持久化**：只保留了有消费者的 `kind`，不引入没有读者的状态。

**四、未做与理由**

- **P2（读 OptiScaler.ini 的 `Dx11Upscaler`）不做**：`unsafe_dx11_on12_backend_selected()`
  （`:9318-9357`）返回的是 bool 不是值，要改签名；且它读的是**游戏 exe 目录**的 ini，仓库里
  只有 `SharedResources/OptiScaler/runtime/OptiScaler.ini` 这份模板（打包进
  `payload\default_config\`），真实运行目录里的那份由谁写入、何时写入未核实。它报的也只是
  「配置意图」不是事实，价值低。
- **P3（未接管时打 `state=SKIP`）不做**：`ngx_core backend=`、`fsr2_translation_blocked` 等已有
  信号，新增块的收益不抵噪声。
- **独立 NGX 摘要块不做**：把 `BACKEND :` 与 DLSS 版本并入现有行已覆盖主要痛点，独立块与
  焦点块内容重复。

---

## 已解决

### NGX 后端的画面与平台验证（2026-11-13）

**解决提交**：`40a6d8e`《fix(ngx): NgxHdr 默认改为 0，避免 IsHDR 导致的拖影》、`d76a437`《feat(ngx): 新增 D3D11 NGX (DLSS) 超分后端》、`9e1d301`《chore(release): 版本号迭代到 2.5.0》。

**结论**：画面验证已完成，随 2.5.0 发版。

- 画质问题的根因：桥交给 NGX 的是 PQ 码值、不是线性光，`NgxHdr=auto` 在 HDR 会话解析成 HDR（`create_flags=0xB`，含 `IsHDR`），DLSS 按 HDR 管线解释这些码值 ⇒ 角色走动 / 转视角时大面积拖影。默认改为 `0`（`create_flags=0xA`，不置 `IsHDR`）后拖影大幅改善，`auto` 三态保留作逃生舱。三处同步：`load_config` 的兜底、`Dx11FsrBridge.package.ini:203`、`scripts/Deploy-Bridge.ps1:123`。
- 维护者 2026-11-13 确认：鬼影 / 闪烁 / 锐度、HDR 与 SDR 两种会话、非 N 卡平台、多实例均已实测通过，并随 2.5.0 发布。
- 现场自查判据：`ngx_core init ok=1 available=1`、`ngx_core create_feature ok=1`、`ngx_hdr_decision is_hdr=… src=…`。

### 非 NVIDIA 平台借用外部 NGX 实现（2026-11-13）

**解决提交**：`c67daf5`《feat(ngx): 支持从外部实现解析 NGX 入口，非 NVIDIA 平台经 OptiScaler 呈现 DLSS 接口》。

**结论**：机制落地并经实机验证，随 2.5.0 发版。

- `NgxProvider` 三态：`nvidia`（注册表 `HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore` 的 `FullPath` → `_nvngx.dll`，失败即确定性失败）/ `external`（先 `LoadLibraryW(L"_nvngx.dll")`，OptiScaler 的加载钩子会返回它自己；再枚举已加载模块，找导出 `NVSDK_NGX_D3D11_Init_Ext` 的那个）/ `auto`（先 nvidia，失败再 external）。
- 外部实现失败可重试：5 s 退避 × 12 次 ≈ 60 s，用尽才记 `reason=provider_missing` 并回落 native。
- 画质契约：对方是 OptiScaler 自带的 FSR 2.2.1（D3D11 静态 FFX），**不读** `MV_Scale_X/Y`（按 UV 单位处理运动矢量），`IsHDR` 直接决定它走不走 HDR 分支。注意 `40a6d8e` 的提交说明写过「OptiScaler 会把 IsHDR 改回 1」⇒ 桥把 `NgxHdr` 默认改成 `0` 只覆盖**绕过** OptiScaler 的路径，外部实现路径上的 HDR 判定仍归 OptiScaler 的 ini。
- 取证行：`ngx_core provider_mode=…`（配置要什么）与 `ngx_core provider resolved=… module=…`（实际用了谁，kind ∈ `nvidia_registry` / `nvngx_load` / `loaded_module`）。

### `Ffx12SingleInstance` 缺省 0 + NGX 后端强制单实例（2026-11-13 复核）

**~~旧结论（已证伪）~~**：曾记为「`d76a437` 把缺省由 0 改为 1 是有意修正，保留缺省 1，多实例场景无回归」。

**证伪依据**：`379f343`（2026-09-28）引入时缺省 1，是为绕开当时**全局单份**的 FFX12 GPU 互操作共享层；同日 `f9e52b3` 起该层已改为按 `instance_key` 独占（纹理/UAV/SRV/尺寸池/context/挂起槽全部按实例隔离，只剩 fence 全局），`68ed170`（2026-10-02）因此把缺省改为 0 全量接管，并实测左右 1:1 分屏两路各自 100% 接管（0 拒绝 / 0 吞掉 / 无覆盖层 / 无冻结）。`d76a437` 在合并 NGX 后端时把代码与模板双双改回 1，提交说明写「缺省值与发布 ini 对齐」——改动前两处**本来都是 0**，所谓对齐不成立，属回归。

**现结论**：缺省恢复 0（全量接管），两处同步：`Dx11FsrBridge/Dx11FsrBridge.cpp` 的读取点与 `Dx11FsrBridge.package.ini`；`scripts/Deploy-Bridge.ps1` 不写该键（新增为第四个「故意不写」的例外），缺键时走代码内建缺省 0。FFX 后端多机位场景回到 `68ed170` 的已验证行为。

**NGX 后端不同**：NGX 的 feature 与资源池是**真全局单实例**（`NgxBackend.cpp` 的 `g_inst`，v8 起不再是 map + LRU），只支持一条流水线。全量接管下两路视图交替派发时：尺寸/格式不同 ⇒ 每次 `instance_key` 变化都 `destroy_instance()` + `create_instance()`；尺寸格式全等 ⇒ 走跨 key 复用但每帧复位时域（原注释：否则两个视图的时域累积互相污染）。两种都不可能两路同时正常出图 ⇒ NGX 后端**无视该键**、一律只接管一个实例（`single_instance_takeover_effective()`），日志行 `ngx_core backend=ngx … single_instance=forced` 记录该强制。

**未验证**：NGX 后端多机位场景未实机复测（结论来自代码审计：全局唯一实例 + 逐 key 重建/复位两条路径都不可并发）；FFX 全量接管沿用 `68ed170` 的分屏实测，本次只改缺省值，未重跑。

### `Ffx12Backend=auto` 与 DLSS 运行时（2026-11-13）

**解决提交**：`af3f9ad`《feat(ngx): Ffx12Backend 支持 auto，按显卡型号选择后端》、`42020f2`《feat(installer): 新增「超分输入类型」，只装桥时也补 DLSS 运行时》。

**结论**：安装路径上「auto 在 RTX 上选中 ngx 却没有运行时」已不存在，auto 正向路径实测通过，随 2.5.0 发版。

- 安装器与桥共用一条判据（`srinput::uses_ngx()`：`ngx`，或 `auto` 且判为 RTX），补文件与选后端不再两套口径；落点两处（桥的特征目录 `payload\Bridge\ngx\` 与 OptiScaler 的组件目录），只下一次网。
- 主页新增「超分输入类型」行（界面 Auto / FFX / NVNGX ↔ ini `auto` / `native` / `ngx`），选完立刻写回**已安装**的 `Dx11FsrBridge.ini`；`scripts/Deploy-Bridge.ps1` 仍故意不写该键，保留用户原值。
- 非 RTX 想借外部实现呈现 DLSS 接口时，仍须显式选 NVNGX（写 `Ffx12Backend=ngx`）。
- 仍然成立的一条（**刻意保留，不算缺陷**）：运行时被手工删掉时，ngx 会 `init ok=0` 并逐帧 fail-open 到游戏原生 FSR2，不会自动改走 native —— 避免出现第二套可用性判据（`AGENTS.md` §五.1）。
- 已核对 2.5.0 两个归档（`dist\原神解帧FSR插件包_v2.5.0.7z` 与 `dist\github-release\GenshinFSRBridge_v2.5.0.7z`）里的 `payload/Bridge/Dx11FsrBridge.ini`：`Ffx12Backend=auto`、`NgxHdr=0`、`Ffx12SingleInstance=1`。末项是 2.5.0 自带的缺省回归（见本文《`Ffx12SingleInstance` 缺省 0 + NGX 后端强制单实例》一节）：两处当时确实同为 1，但值本身是错的，本轮已恢复 0。

### `create_feature` 参数性失败按输入 key 重试（2026-11-13）

**解决提交**：`5f37dd6`

**结论**：机制已实现，构建 / 单测 / **实机验证均通过**。

- **实机验证（2026-11-13 22:02–22:03，同一次游戏运行内完成，未重启）**：
  为复现失败，临时加入诊断开关强制走"§4 之前"的旧选档（绕过区间查询与
  `UltraQuality`→`MaxQuality` 兜底；该开关验证后已删除，未进入任何提交）。日志时序：
  ```
  22:02:29  pq_select mode=legacy_diag pq=4 UltraQuality r=3072x1728
            create_feature ok=0 rc=0xBAD00010
            key_fail … retry_eligible=1
            family_release reason=create_feature_rc reason_rc=0xBAD00010 cleared=1
  22:02:48  create_feature ok=0 rc=0xBAD00010        ← 精度未变 ⇒ key 相同 ⇒ 未重试
            key_fail … retry_eligible=1              ← 闸门已打，family_release 不再重复
  22:03:13  key_retry … prev_rc=0xBAD00010           ← 精度改为 0.7 ⇒ key 变了
            create_feature ok=1 rc=0x1
            key_retry_result ok=1 prev_rc=0xBAD00010
  ```
  验证到的事实：① 失败被复现；② `retry_eligible=1` 字段确实存在（验收 grep 可用）；
  ③ `family_release` 在失败瞬间解除 family skip 且**只打一次**（无日志洪水）；
  ④ **key 相同不重试**（22:02:48）；⑤ **key 变化才重试并成功**（22:03:13）；
  ⑥ **全程无需重启游戏**。
- 注意：本次是通过诊断开关构造的失败。正常路径下 §4 的区间查询已能选到合法档位
  （同日 21:51 实测 0.999 精度 `picked=DLAA` → `create_feature ok=1`），
  因此该重试路径在未注入失败的会话中通常不会触发 —— 这是预期行为，不是未生效。

- 缺陷原状（用户提供的事故日志）：`create_feature ok=0 rc=0xBAD00010` 后，用户把渲染精度
  由 0.999 改回 0.5，日志仍 `DISPATCH_FAILED` 一路失败到结束 —— 只能重启游戏。
- 根因有两层，缺一不可：
  1. `NgxInstance::feature_create_failed` 在对象生命周期内永不清除（`S-81` 的"永久不重试"）。
     ★ `S-81` 是 `NgxBackend.cpp` 注释里的**旧审查编号**，含义就地说明（读者不必去别处找）：
     同一对象生命周期内锁存 `feature_create_failed`，尺寸/格式变化只释放资源、**不重试**；
     锁存只在实例对象销毁时随对象消失。最初的理由是 `CreateFeature` 失败的根因
     （`PathListInfo` 配错，rc=`0xBAD0000B`）**不会自愈**，相同输入重试只会每帧阻塞 + 每帧重复日志。
     ★ 本轮为何放宽：该理由只覆盖"根因不会自愈"这一类，而实机出事的是**参数性**失败
     （rc=`0xBAD00010` `UnsupportedParameter`）—— 玩家把渲染精度改回去，输入就变了，
     本该能建起来。⇒ 保留 S-81 的保守默认（非白名单 rc 仍然不重试、`g_release_all` 照旧置位），
     只对"白名单 rc + 输入 key 确实变化 + 间隔足够"这三个条件同时成立的情形放行一次；
     任何一条不满足都退回 S-81 的原始行为。
  2. 三个置位点都同时置 `g_release_all`，而它是 **0→1 不可逆**的全局放手信号，被
     `Dx11FsrBridge.cpp` 的两道每 draw gate 与 `NgxBackend.cpp` 的一处 gate 读取
     ⇒ 一旦置位，`dispatch` 在**到达**锁存判据之前就返回了。**只清锁存不会生效**，
     这是原待办配方里漏掉的一环。
- 修复（三重窄门，任何一条不满足都回到"不重试"）：
  1. **rc 白名单**：只有 `NVSDK_NGX_Result_FAIL_UnsupportedParameter`（`0xBAD00010`，
     `third_party/ngx/nvsdk_ngx_defs.h:169`）不置 `g_release_all`；其余一切 rc（含未知 rc、
     含 `0xBAD0000B`）维持原样置位 ⇒ 未知新 rc 默认仍是保守放手。
  2. **输入 key 变化**：`ngx_param_map::CreateFeatureInputKey` 记录 render/display 宽高、
     选档结果、`g_render_preset`、create flags、`color_fmt`/`output_fmt`、
     `resolve_is_hdr` 的决策（**不是** `g_hdr`，后者只是回写镜像）。key 相同 ⇒ 仍然不重试。
     比较实现已搬进纯函数层 `ngx_param_map::same_feature_input_key`，由 `NgxParamMapTest`
     第 [11] 节逐字段钉住（每加一个字段必须同时加断言，否则该段会红）。
     ★ `color_fmt`/`output_fmt` 是后补的：原实现漏了它们，反例是后缓冲由 `R10G10B10A2`
     换成 `R16G16B16A16_FLOAT` 而 create flags 不变 ⇒ key 判「相同」⇒ 自愈被漏判。
  3. **最小重试间隔 2000 ms**（`kFeatureCreateRetryMinMs`）：给「连续失败」一个频率上限。
     真实理由是 **DLSS `CreateFeature` 失败时会阻塞**（不是「尺寸逐帧抖动」——key 的 10 个
     字段全是低频事件驱动量，不随帧变化，该说法已证伪并就地标注）。该值**未实机标定**，
     是保守猜测值，不是测量值。
- 另两个置位点（`no_params` / `null_resource`）**不记录 key、不参与重试**：它们是环境性 /
  接线缺陷，与"换一组输入参数"无关；纳入重试会让每帧的建池失败反复重跑 `CreateFeature`。
- 取证行（默认 `NgxLogLevel=1` 即可见，均为 Error 级）：`create_feature key_fail … retry_eligible=1`
  （失败并记下 key）、`create_feature key_retry …`（key 变化、即将重试）、
  `create_feature key_retry_result ok=1 …` 或 `ok=0 … same=1|0 rc_known=1|0`（重试结果；
  `same=1` 说明本次 rc 与上次相同、更像环境性问题）。
  ★ `new_rc` 取的是**本次调用**真实的 rc（`create_feature` 的出参），**不是**
  `feature_fail_rc` —— 后者只在白名单 rc 时才被写，读它会得到上一次的旧值。
  ★ `rc_known=0` + `new_rc=0xFFFFFFFF` 表示本次**没走到 NGX 调用**（`no_params` /
  `null_resource` 两条早退路径），此时 `same=` 无意义、固定为 0。
- 冻帧对照的取证行：`ngx_core family_release reason=create_feature_rc reason_rc=0x… cleared=N`
  —— 白名单失败分支里**只打一次**（进程级 `std::atomic` 闸门）。它出现即证明那行
  `notify_accumulate_result(false, …)` 真的执行过；`cleared=` 是该调用的累计次数（≥1）。
- 热路径影响：key 比较**只在 `feature_create_failed` 为真时**执行 —— 那时 `dispatch`
  本来就要 `return false`；成功路径上仍是一次 bool 判断，与改动前相同。
- 未改动的边界（刻意）：`release_pool_resources` / `create_pool` 仍然不碰锁存
  （否则 `0xBAD0000B` 会每帧重试）；`quality_from_ratio` / `perf_quality_for_create`
  行为未变。

### DLSS 选档改由 NGX 区间查询决定（2026-11-13）

**解决提交**：`5f37dd6`

**结论**：判据已治本（不再假设「档位 = 固定比例」）；构建 / 单测 / **实机验证均通过**。

- 缺陷原状：`quality_from_ratio()` 用手写比例表选档（1.0–1.3 → UltraQuality 等六条），
  而每个档位能接受的 render 尺寸区间由驱动与模型决定、**不是比例**。渲染精度 0.999
  （ratio≈1.0016）因此落进 UltraQuality ⇒ `create_feature ok=0 rc=0xBAD00010`
  （`NVSDK_NGX_Result_FAIL_UnsupportedParameter`，`third_party/ngx/nvsdk_ngx_defs.h:169`）。
  `perf_quality_for_create()`（`b632cc5`）只改结果、不改判据，挡不住下一个档位错配。
- 新判据：`query_dlss_optimal_range()` 照 `nvsdk_ngx_helpers.h:65` 的
  `NGX_DLSS_GET_OPTIMAL_SETTINGS` 重写（不 vendor 那个头 —— 它末尾 include 了仓库里没有的
  `nvsdk_ngx_helpers_d3d.h` / `_cuda.h`）；六档各查一次合法 render 区间，挑包含当前
  render 尺寸的那档，每轴上界留 `max/64` 容差。都不包含或查询失败 ⇒ 回退
  `quality_from_ratio` + `perf_quality_for_create`（= 改动前行为，逐字不变）。
- **参数对象污染的处理（本次最大的风险点）**：区间查询要往参数对象上 `Set` 四项
  （Width/Height/PerfQualityValue/RTXValue）。做法是**另调一次**
  `NVSDK_NGX_D3D11_GetCapabilityParameters()` 拿**独立**对象（`g_query_params`），
  绝不在全局 `g_params` 上查询。为什么不用 `AllocateParameters`：SDK 头文件里
  `NVSDK_NGX_D3D11_AllocateParameters` 的注释说明它分配的表**不预置能力项**
  （该头未入库，随驱动 / SDK 分发），而
  `NGX_DLSS_GET_OPTIMAL_SETTINGS` 自己的注释（`nvsdk_ngx_helpers.h:83-84`）就写着
  「callback 取不到时可能是用了 AllocateParameters，改用 GetCapabilityParameters」。
  若实现返回**同一个指针**（单例 provider），判为查询不可用并回退 —— 宁可不查也不污染。
  释放：`shutdown_all()` 里对本对象调 `NVSDK_NGX_D3D11_DestroyParameters`；
  全局 `g_params` **不释放**（这是既有的设计约定：桥不销毁 NGX 的参数表，
  与 `NgxBackend.cpp` 里「不调 `g_params->Reset()`」是同一条约束）。
- 缓存：区间只依赖 display 尺寸，按 `(dw, dh)` 缓存六档结果；`create_feature` /
  `diag_check_create_params` 本来就不是每帧路径，缓存是额外保证。
- 调用点三处同源（否则 `set_mismatch` 误报）：`create_feature` 的 `Set`、
  `diag_check_create_params` 的回读期望值（该函数整体在 `DX11FSRBRIDGE_ENABLE_DIAGNOSTICS`
  内，生产构型不编译）、`make_feature_input_key_from` 的 key 字段。
- 取证行（Error 级，默认 `NgxLogLevel=1` 可见）：`pq_query_params ok=0|1 reason=…`、
  `pq_range pq=N name=… opt=… min=… max=… tol=…`（每档一条，实际可能少于六条——
  本机就没有返回 UltraQuality 那一档）、
  `pq_select mode=interval picked=… pq=…` 或 `pq_select mode=ratio pq=… reason=…`。
- 与规格的一处出入（如实记录）：容差原文写「1/64（约 2 像素）」，而 `upper/64` 在 1280 上
  是 20 像素、2560 上是 40 像素 ——「约 2 像素」应是估算。实现按**公式**（1/64），
  并把实际 tol 打进日志，单测 [10] 有一条断言专门防止后人按文字改成常量 2。
- **实机验证（2026-11-13 21:51 单精度 + 23:12–23:21 全档位）**：9 个非默认渲染精度
  全部 `create_feature ok=1`。ratio ≤ 2.0（0.5~0.999）走 `interval`，ratio > 2.0
  （0.2~0.4）走 `ratio` 回退（`reason=no_interval`），两条路径都实测可用。
  附带结论：① 本机驱动不返回 UltraQuality 那一档的区间（`ranges_ok=5` 而非 6），
  「六档全查到」不是普遍情况；② 容差是**分档**的（`60x33` vs `pq=3` 的 `20x11`），
  说明原文「约 2 像素」不成立；③ `CreateFeature` 的校验确实比 optimal settings 宽松
  —— `UltraPerformance` 区间是 `min=max=1280x720` 一个点，而 0.3/0.4 声明它仍 `ok=1`。
- `ratio > 3.0`（渲染精度 0.2/0.3）的档位归属同日改过：原落 `else` ⇒ DLAA，
  改为 UltraPerformance 属于判据对齐（倍率语义），**不是画质改善**；
  实测两个精度均 `ok=1`，与改前的画面观感差异很小，未做 A/B 量化对比
  ⇒ 画质优劣未验证。`ratio ≤ 1.0` 仍返回 DLAA，两分支刻意分开。
- **覆盖范围有限**：一台机器、一个 DLSS 版本、3840x2160 一种分辨率；
  external provider 的单例回退对照未做。

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
