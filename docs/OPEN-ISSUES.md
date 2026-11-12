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
