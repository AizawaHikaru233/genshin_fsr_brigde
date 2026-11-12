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

## 4. DLSS 质量档位由手写比例表决定，未向 NGX 查询合法尺寸区间

**现状**

- `Dx11FsrBridge/NgxBackend.cpp:1750-1760` 的 `quality_from_ratio()` 用手写比例表选档：
  `ratio 1.0–1.3 → UltraQuality`、`1.3–1.5 → MaxQuality`、`1.5–1.7 → Balanced`、
  `1.7–2.0 → MaxPerf`、`2.0–3.0 → UltraPerformance`、其余 → DLAA。
- 两个调用点：`:1867` 传给 `create_feature`；`:2701` 算 `set_mismatch` 的期望值（同一处改，必须同源）。
- `:1781-1784` 的 `perf_quality_for_create()` 把 `UltraQuality` 改成 `MaxQuality`，是 `b632cc5`
  为渲染精度 0.8~0.999 建的兜底。

**影响**

- 每个 DLSS 档位能接受的 render 尺寸区间由驱动与模型决定，**不是固定比例**；手写表假设它是比例。
  渲染精度 0.999（ratio≈1.001）落进 `UltraQuality` 分支 ⇒ `create_feature ok=0 rc=0xBAD00010`
  （`UnsupportedParameter`），而当前失败锁存不重试（见下一条）⇒ 必须重启游戏才能恢复。
- 兜底只改结果、不改判据：不同显卡、不同 DLSS 版本的真实区间可能不同，兜底挡不住下一个档位错配。

**推进需要**

- 改用 NGX 自带的合法区间查询：`NVSDK_NGX_DLSS_GET_OPTIMAL_SETTINGS` 是
  `nvsdk_ngx_helpers.h:65` 的 `static inline`，本仓库**故意没有** vendor 这个头
  （`tools/Fetch-NgxHeaders.ps1:28`），但可照它的实现重写约 40 行：在 capability params 上
  `Set(Width/Height/PerfQualityValue/RTXValue)` → 取 `NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback`
  调一次 → 读 `NVSDK_NGX_Parameter_OutWidth/OutHeight`、
  `NVSDK_NGX_Parameter_DLSS_Get_Dynamic_{Max,Min}_Render_{Width,Height}`、`Sharpness`。
  这些参数名字符串已全部在 `Dx11FsrBridge/third_party/ngx/nvsdk_ngx_defs.h` 里。
- 五个档位各查一次，取各自合法的 render 宽高区间，挑**包含当前 render 尺寸**的那档；
  允许每轴超出区间上限不超过最大尺寸的 1/64（约 2 像素）。都不包含时回退现有比例表。
- 查询失败（callback 缺失 / 返回非 `NVSDK_NGX_Result_Success`）一律回退现有行为，不改原有可用性。

**注意**

- 只改「选哪个档」，不改 create flags、不碰资源与实例生命周期。
- `:1867` 与 `:2701` 必须走同一条选档链，否则 `set_mismatch` 会误报。
- 判据：渲染精度 0.8~0.999 时 `create_feature ok=1`，且日志里的 `pq=` 与区间查询结果一致；
  对照 0.999 旧行为是 `ok=0 rc=0xBAD00010`。

**相关代码**

- `Dx11FsrBridge/NgxBackend.cpp:1750-1760`、`:1781-1784`、`:1867`、`:2701`；
  `Dx11FsrBridge/third_party/ngx/nvsdk_ngx_defs.h`；`tools/Fetch-NgxHeaders.ps1`

---

## 5. `create_feature` 失败后永久不重试，参数变化本可自愈的场景必须重启游戏

**现状**

- `Dx11FsrBridge/NgxBackend.cpp:3106` 写明「永久不重试（§3.9.7 / S-81）：CreateFeature 失败的
  根因（PathListInfo 配错）不会自愈」。`S-81` 是注释里的旧审查编号，含义在本文件可原地读到：
  同一对象生命周期内锁存 `feature_create_failed`，尺寸/格式变化只释放资源、不重试
  （`:205`、`:1848`、`:1893`）。
- 锁存只在实例对象销毁时随对象消失（`:2743`）。

**影响**

- 「PathListInfo 配错」确实不会自愈，但失败原因不止这一类：`0xBAD00010` 是**参数性**失败，
  玩家把渲染精度、分辨率或 HDR 改一下，本该能建起来，现在却必须重启游戏。
- 失败后每帧 fail-open 回游戏原生 FSR2，玩家只看到「DLSS 不生效」，日志里才有一行 rc。

**推进需要**

- 把「永久」改成「输入 key 变化即允许重试」：记录失败时的 key = render 宽高 + display 宽高 +
  档位 + NgxRenderPreset + HDR 标志（+ create flags）；下一次创建前 key 不同就清
  `feature_create_failed` 重试一次，key 相同仍不重试。
- 日志记录 key 变化与重试结果（成功/同一个 rc），便于区分「参数问题」与「环境问题」。

**注意**

- S-81 的原始理由仍然成立，**不能**改成无条件解锁：否则尺寸抖动或无关状态变化会反复触发
  `0xBAD0000B` 这类每帧重试（`:2085` 记过这个教训）。触发条件只认上面那几个输入。
- 与上一条配合看：上一条去掉参数错配的成因，本条是剩余失败类型的兜底；两条可分别验收。

**相关代码**

- `Dx11FsrBridge/NgxBackend.cpp:205`、`:1848`、`:1893`、`:2085`、`:3049-3069`、`:3106`、`:2743`

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
