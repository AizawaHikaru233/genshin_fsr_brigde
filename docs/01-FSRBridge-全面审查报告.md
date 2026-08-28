# FSRBridge 全面审查报告

> 生成日期：2026-08-21 ｜ 审查对象：`D:\FSR`（v1.2.3，目标原神 7.0 DX11 客户端）
> 方法：Agency 专家团队 8 人并行审查 + 交叉复核（onboarding / code-reviewer / software-architect / appsec / senior-developer / reality-checker）
> 证据规则：所有发现均标注 `file:line`；标注「已证实」= 至少 2 位专家独立读码确认，「未确认」= 单方推断。

## 0. 结论摘要

- **当前主干不可直接发布 v1.3**（reality-checker 判定 NEEDS WORK）。
- **1 个 Critical 内存安全 bug（C1）**：vtable 槽位 off-by-one，DLSSG 路径可致崩溃；**2 个 High**：vtable 克隆越界读（H1）、自更新供应链信任链断裂（H2/H3）。
- 建议路线：**hotfix v1.2.4 修 C1+H1**（内存安全止血）→ **v1.3 收 Should 清单**。
- 正面结论：防御性编程意识强（`__try/__except`、指纹缓存、写前可写性校验、白名单值、DLL pin 均到位）；FSR2 Mode 2 用**结构化内省**识别 TAAU（SRV/RTV 布局 + weights 16×16 + cb0≥464）而非字节签名，版本韧性最好。

## 1. Critical

### C1 — SetHDRMetaData vtable 槽位 off-by-one（已证实 ×3）
- `Dx11FsrBridge.cpp:94` `k_idx_set_hdr_metadata = 39`；按 `dxgi1_5.h`，IDXGISwapChain4 槽 **39=ResizeBuffers1、40=SetHDRMetaData**。
- 连带 `:10591` swapchain3 分支 `hook_method_count=39`，但 IDXGISwapChain3 实为 40 槽 → clone 漏 39，越界调即空指针。
- 触发：DLSSG（DX11-on-DX12）路径下游戏调 `ResizeBuffers1`（8 参）会误入 `hooked_set_hdr_metadata`（4 参签名，`:6212`）→ 垃圾参数 → 崩溃/设备移除；真正的 SetHDRMetaData 永远没被钩，HDR spoof 静默失效。
- **修复**：`:94` 改 `= 40`（加 `k_idx_resize_buffers1 = 39` 自文档）；`:10591` 改 `hook_method_count = 40`。风险极低，需回归 DLSSG 路径。

## 2. High

### H1 — vtable 克隆越界读（已证实 ×3）
- `Dx11FsrBridge.cpp:4622` `memcpy(clone, source, sizeof(void*)*method_count)`，method_count 为硬编码：context 128 / context4 149 / device 80 / swapchain 41 / factory 32/40。
- 实际槽数：ID3D11Device=**42**、ID3D11DeviceContext≈112、Context4≈143、IDXGIFactory≈26、swapchain4=41（恰好对）。device 越界读 38 槽（304B），context 越界 16 槽。
- vtable 在 .rdata 页通常不崩，但属 UB；若恰在页尾 → AV。
- **修复**：仿 `:4632` 的 QI 判接口模式，按接口精确槽数克隆；memcpy 前 `VirtualQuery` 校验可读长度 ≥ method_count×8。

### H2/H3 — 自更新供应链信任链断裂（已证实）
- `tools\FpsUnlockInstaller\Installer.ps1:636-700,761`：`Get-GitHubFallbackUrls` 把 `ghfast.top/gh-proxy.com/ghproxy.net` 前置（含 api.github.com）；`Start-PackageSelfUpdate` 用 `$asset.digest` 校验下载包，而 digest 来自**同一批不受信代理**返回的 JSON → 恶意代理可伪造 digest+包体 → RCE。
- `Installer.ps1:675`：`ExpectedSha256` 为空即**跳过校验**。
- **修复**：自更新包固定硬编码 SHA-256（`known-releases.json`，CI 回填）；`api.github.com` 永远直连、代理仅下载字节；无 digest 时拒绝代理下载。

### 其他 High（appsec 视角）
- **vendored 上游 DLL 无打包前 SHA256 清单**：`Build-OnlineInstaller.ps1:115,402` 只对最终 zip/7z 打哈希，`Assert-OptiConfigMatchesRuntime`（`:75-92`）只比对版本字符串。被植马的 ReShade64/OptiScaler/nvngx_dlss 会随包分发并在游戏进程内加载。
- **修复**：`third-party.lock.json`（path→sha256+version+来源）+ `Assert-VendoredManifest`，CI 打包前逐文件校验。

## 3. Medium

| # | 问题 | 位置 | 修复 |
|---|---|---|---|
| M1 | vtable 全局切换与渲染线程无锁读竞态（ScopedContextVtableBypass 期间并发调用绕开 hook） | `Dx11FsrBridge.cpp:4663-4683` | 内部操作直调 `g_original_*` 替代全局切表；或改 `std::atomic<void*>` release/acquire |
| M2 | `g_buffer_info`/`g_buffer_snapshots` 以裸指针为键、从不 erase → 无界增长 + 地址复用读脏 jitter | `:604-612,10293-10298` | 钩 Buffer Release 清理 + 容量上限；CreateBuffer 无初值时无条件 erase |
| M3 | 5 处 ini 负整数未 clamp（INT 回绕成巨大 uint32） | `:3356,3424,3438,3442,3496` | 统一 std::clamp / 范围校验 |
| S1 | **Mode2/3 下 exposure 恒 null → FSR2 自动曝光恒开**，`Fsr2UseNativeExposure` 失效 | `:8903-9010`、`Fsr2TranslationLayer.cpp:434-447` | Mode2/3 恢复 exposure 采集，或显式关闭并在日志告警 |
| S2 | 历史重置/同步缺陷：`matches` 判定缺维度（不比 hdr10/direct-color/jittered/masks）；原生历史（views[5]）冻结 | `Fsr2TranslationLayer.cpp:417-421`、`:8274`、`:9186` | matches 补全维度；历史由 FSR2 侧接管更新。注：reality-checker 已修正——**发布版默认 reset=1**（`#if DX11FSRBRIDGE_RELEASE_RUNTIME` `:3347-3348`），非发布版默认 0 |
| S3 | motion 矢量自带 jitter 时未启用 jitter cancellation → 双影风险 | `:3449` 无启用路径 | 按游戏实际 motion 是否含 jitter 配置 `FFX_FSR2_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION` |
| S4 | cb0 jitter 快照经 CopyResource 时不刷新 → 陈旧错帧；首帧零值 jitter=(-0.5,-0.5) 通过 ±0.6px 守卫 | `:10121,10064,10157,7045` | Copy 路径补刷新；首帧 jitter 判定修正 |
| S5 | `copy_fsr2_history_metadata` 失败重放原生 TAAU 后仍返回 skip（双写并存） | `:9353-9399` | skip 判定与重放路径解耦 |
| M4 | AntiPlayerMosaic 缓存 il2cpp 对象指针跨帧复用 → UAF | `AntiPlayerMosaic.cpp:41-42,187-226` | 每次现场 find_object 或加 klass 校验；去掉跨帧缓存 |
| M5 | 特征缓存被篡改可崩溃：`std::stoul` 无保护（异常终止进程）；build_rva 未校验 < image_size | `AntiPlayerMosaic.cpp:111`、`RenderScaleMenu.cpp:149-172` | from_chars/try + `rva < SizeOfImage` 校验 + memcmp 包 SEH |

## 4. Low / Nit（摘选）

- L1 `RenderScaleMenu.cpp:351-375` 伪造 Il2CppString 的 klass 指针置 null，引擎做虚调用/GC 即崩（现菜单场景侥幸可用）。
- L2 `AntiPlayerMosaic.cpp:307-325` CAS 门非严格互斥，>1200ms 阻塞可并发进入。
- L3 `Dx11FsrBridge.cpp:11203-11216` 卸载路径不回滚 Detours/克隆、不释放 COM；宿主卸载 DLL 而进程继续则崩。
- L4 RWX 跳板 stub（`AntiPlayerMosaic.cpp:328-361`），可改 PAGE_EXECUTE_READ。
- Nit：热路径 `g_state_mutex` 争用（可读写锁）；`PatternScanner.hpp` 不支持 `?5` 半字节通配（当前未用）。

## 5. 架构评估（software-architect）

- 边界问题：`Dx11FsrBridge.cpp` 单文件 ~11k 行，状态镜像/诊断/HDR/DLSSG/OSD/日志全堆一个 TU → 建议拆 TU + 抽「DX11 vtable 拦截器」「签名+指纹缓存」公共库（AntiPlayerMosaic 已重复实现指纹缓存）。
- 版本兼容：**结构化内省（Mode 2 TAAU 识别）韧性最好**，应作为主识别手段；字节签名降级为校验/兜底；ini 死 RVA 应删除或仅作扫描种子（签名+RVA 双重校验，错值自动回退扫描）。
- ABI 契约：与 OptiScaler 仅 6 个导出名/签名耦合，且依赖「Bridge 先于 OptiScaler 加载」的 GetProcAddress 扫描时序；建议额外导出版本握手符号，`query_mask` 不全时告警而非静默。
- 线程：无渲染线程断言，建议「仅 immediate context」断言 + 文档化。

## 6. 修复优先级（Top 5，senior-developer + reality-checker 共识）

1. **C1** vtable 槽位 39→40（可致 DLSSG 崩溃，一行常量 + 一处 count）→ v1.2.4 hotfix
2. **H1** vtable 克隆越界读（跨驱动/游戏更新的潜在崩溃）→ v1.2.4 hotfix
3. **H2/H3** 自更新信任链 + vendored SHA256 清单（供应链 RCE）
4. **S1** Mode2/3 自动曝光恒开（核心功能正确性）
5. **M2** 常量缓冲 map 无界泄漏 + 脏 jitter

## 7. 发布验收（reality-checker）

- **Blocking（禁发）**：B1=C1、B2=H1、B3=供应链信任链。
- **Should**：M1 竞态、M2 无界增长、M3 clamp、S1 曝光、S2 修正后重验。
- **Could**：S3 jitter cancellation、ini RVA 死配置清理。
- README「后续无需更新也能支持新版本」**过度承诺，应撤回**为「已适配 7.0，新版本需重新验证」。

## 8. 审查范围外声明

- 未覆盖：`vendor/`、`tools/FpsUnlockInstaller/` 各脚本全文、Lua 安装脚本、RenoDX-Genshin 二进制、游戏二进制反汇编层面的签名正确性。
- `Dx11FsrBridge.cpp` 中 FSR2 候选识别启发式（~7000-9500 行）由 senior-developer 深度复核（S1-S5），其余行号按安全关键路径定向审查。
- HANDOFF-mask-fix.md 位于仓库根目录 `D:\FSR\HANDOFF-mask-fix.md`（多位专家初查误寻于 Dx11FsrBridge 子目录）。
