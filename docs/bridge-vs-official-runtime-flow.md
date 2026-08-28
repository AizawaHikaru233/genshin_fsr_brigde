# 当前 Bridge 与 GitHub 正式版运行流程对比

> 基线：正式版 = `v1.2.3` tag（cbdf5c1，https://github.com/AizawaHikaru233/genshin_fsr_brigde.git）
> 当前 = v1.2.3 + HEAD 8a3975d（Lite/CI 修复）+ 工作区改动（+450 行、新增 4 个源文件）。
> 日期：2026-08-23。

## 1. 改动清单（git diff v1.2.3）

| 文件 | 改动 |
|---|---|
| Dx11FsrBridge.cpp | +409 行：Config 4 键、load_config 解析、日志白名单 2 词、**家族跳过**（draw 钩子 + 翻译路径）、**il2cpp 钩子安装/收尾** |
| CMakeLists.txt | +20 行：Fsr2FamilyTakeover.cpp / Il2CppCallSiteHook.cpp 编入 DLL + Fsr2FamilyTakeoverTest 测试目标 |
| Dx11FsrBridge.package.ini | +21 行：Phase 1/1.5 新键注释 |
| **新增** Fsr2FamilyTakeover.{h,cpp} | 纯 C++ 状态机：5-pass 族识别（4 PRE + accumulate + SMAA）、跳过门控、统计 |
| **新增** Fsr2FamilyTakeoverTest.cpp | 状态机单测（ALL PASS） |
| **新增** Il2CppCallSiteHook.{h,cpp} | 7.0 FFX_FSR2::Render inline patch（observe/skip + 序言校验） |
| **新增** Il2CppCallSiteHookTest.cpp | 钩子单测（ALL PASS） |

## 2. 正式版 v1.2.3 运行流程

```
启动: DllMain → initialize
  ├─ load_config（Fsr2TranslationMode=2, Mode2OnDemand=1, ...）
  ├─ process_matches（目标进程过滤）
  ├─ 钩子安装：D3D11CreateDevice/AndSwapChain 导出 detour、
  │   GetProcAddress 双 detour（kernel32/kernelbase）、IAT scan、
  │   DXGI 颜色钩子（HDR spoof/force/probe）、render scale menu、
  │   OptiScaler shim 导出（ffxFsr2* → 桥转发）
  └─ 设备/上下文/交换链 vtable 克隆补丁（Draw/Dispatch/Map/Present...）

每帧:
  D3D11 draw/dispatch 拦截 → 状态跟踪（PS hash、cb0 快照链、SRV/UAV、资源信息）
  ├─ Mode 2 on-demand：识别游戏 FSR2 累积 pass（0x78057A29...）
  │   → jitter 读游戏 cb0（float 偏移 448，±0.6px 校验 + 模式翻转）
  │   → 替换为 OptiScaler FSR2 dispatch（motion/depth/color guard 伴随）
  ├─ 游戏 FSR2 其余 4 个预处理 pass（0x3CDF78FA/0xAC63A3AF/0x6018B8E9/0x590E69FE）
  │   ★ 照常记录、照常执行 —— GPU 上"双跑"：游戏跑一遍预处理 + OptiScaler 自己再处理
  └─ Present 钩子（帧边界、HDR、DLSSG workaround、tone map 等）
```

## 3. 当前版本运行流程（差异）

```
启动: 同 v1.2.3 + 家族状态机 reset + il2cpp 钩子（默认关，observe 时 patch Render）

每帧:
  D3D11 draw/draw_indexed 拦截
  ├─ ★ 家族跳过（Fsr2FamilySkip=1 时）：
  │    hash ∈ {4 个 PRE} 且 上次累积 pass 被桥成功替换（handled=1）
  │    且 未超过 Fsr2FamilyExpireMs(500ms)  → 该 draw 直接跳过（不透传 GPU）
  ├─ Mode 2 on-demand：识别累积 pass → jitter → 替换为 OptiScaler（同正式版）
  │    ★ 替换结果回填家族状态机 notify_accumulate_result(handled, now)
  │    （handled=1 → 下一帧起跳过 4 个 PRE；handled=0/超时 → 恢复正式版行为，自愈）
  └─ Present（同正式版）
```

### 差异本质（一句话）

**正式版**：桥只替换游戏 FSR2 的第 5 个 pass（累积/上采样），前 4 个预处理 pass 双跑。
**当前版**：上一帧累积替换成功后，4 个预处理 pass 在 D3D11 层被跳过 → **GPU 双跑消除**；
累积 pass 仍由 Mode 2 替换为 OptiScaler —— 拦截点不变，只是更早、更省。

## 4. 关键行为对照表

| 维度 | v1.2.3 正式版 | 当前版 |
|---|---|---|
| 游戏 FSR2 4 个预处理 pass | 全部执行 | 累积替换成功后跳过（500ms 门控） |
| 累积 pass | Mode 2 → OptiScaler | 同（唯一触发源不变） |
| jitter 来源 | 游戏 cb0（快照链） | 同（不受影响） |
| Mode 2 触发链 | 游戏 draw 流 | 同 |
| il2cpp 层 | 无 | observe 钩子可用（默认关，仅计数/验证） |
| 失败路径 | 翻译失败 → 回退原生 draw | 同 + 家族门控超时自愈（jitter 缺失帧不跳 PRE） |
| 新配置 | — | Fsr2FamilySkip / Fsr2FamilyExpireMs / Fsr2Il2Cpp* |
| 日志 | — | fsr2_family_skip_draw / fsr2_family_notify / fsr2_il2cpp_* |

## 5. 实测效果（用户观察 + 日志）

- 家族跳过会话：skip total 159,744 ≈ 4× 累积替换数（每帧 4 个 PRE 全跳）；
  本验证会话 skip total 91,136 持续增长、handled=1 正常。
- 用户观察：当前版运行流畅；对比 1.2.3 官方版边缘 shimmer 有所减弱
  （归因未完全确定：帧节奏改善 或 PRE 输出副作用消除，二者皆与家族跳过相关）。
- 启动期 jitter 自愈：累积替换建立前的数帧 PRE 不跳（跳过门控有 500ms 超时），无闪烁/冻结。
- 输入安全：t3/t4（motion/depth 等）为场景几何 pass 产出，与 FSR2 链解耦 —— 跳过族不伤桥的 OptiScaler 输入。

## 6. 风险与注意

- 家族哈希表（0x3CDF78FA 等）为 7.0 适配值：游戏更新需按探针流程复核（`D:\Dump\tools\` 分析链可复用）。
- il2cpp 钩子 RVA（0x06B59670）同理；序言校验失败时自动放弃、回退 draw 层。
- 当前部署：payload\Bridge\Dx11FsrBridge.ini = Fsr2FamilySkip=1, Fsr2Il2CppHook=0, Fsr2TranslationMode=2。
