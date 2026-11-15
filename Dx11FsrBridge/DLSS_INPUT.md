# DLSS 输入桥接（DlssInput）— 设计、根因与落地

> 代码基线：`gh/genshin_fsr_brigde` @ `d94e8c7`（2026-11-09）+ 本次新增 `Dx11FsrBridge/DlssInput.{h,cpp}`、`DlssInputTest.cpp`。
> OptiScaler 基线：`gh/OptiScaler`（`OptiScaler/resource.h` 报 v10.0.0）。
> 本文所有 `file:line` 均可直接核对；凡是**文档与源码冲突**处，一律以源码为准并在正文里标出。

---

## 0. 一句话结论

原桥在 N 卡上的问题**不是"没适配 N 卡"，而是"选错了输入契约"**。

它把自己伪装成一个 **FSR2 游戏**（`ffxFsr2ContextDispatch` / `ffxDispatchDescUpscale`），于是 OptiScaler 只能走 **FSR2/FFX 输入**那条**降级**链路，DLSS 拿到的是"按 FSR2 语义标定过"的 jitter / 运动矢量 / 色彩 / 蒙版。这条路线上有 6 处**架构级**语义错配，且其中至少 2 处（jitter 被钳成常量、PQ 码值被当作 HDR）**不可能靠 ini 配置修正**。

本改造新增**第二条输入契约**：桥反过来伪装成一个 **DLSS 游戏**（`nvngx` 客户端），直接按 `NVSDK_NGX_*` 的参数表逐帧填值。于是：

- 有 OptiScaler 时 → OptiScaler 走 `ApiUpscalerInput::DLSS_DX11`（**完整信息**），DLSS/XeSS/FSR 都能选；
- 没有 OptiScaler 时 → 直接接 NVIDIA 的 `nvngx.dll`，**N 卡像 A 卡直出 FSR4 一样直出 DLSS**。

两条路径共用**同一份**输入抓取（draw 识别 / il2cpp token / 资源槽位 / 归属裁决），差别只在"喂给谁"。

---

## 1. 先纠正三个错误前提

研究过程中发现三处**流传但已不成立**的说法，会直接误导改造方向：

### 1.1 当前 HEAD 已经没有 `ffxFsr2*` 导出，也没有 GetProcAddress shim

`Dx11FsrBridge/OPTISCALER_COMPATIBILITY.md` 说桥导出了六个标准 FSR2 符号、且要求日志里出现
`fsr2_get_proc_address_shim_queries mask=0x3F` 与 `fr2_translation_context_created ... detoured=1`。

**这两行日志在当前二进制里永远不会出现**：

| 事实 | 证据 |
|---|---|
| 全仓无 `.def`，无 `ffxFsr2*` 的 dllexport | `Dx11FsrBridge/CMakeLists.txt:117-127` 源列表；全仓 grep 只命中 `third_party/fsr2` 头与那份 md |
| `Fsr2TranslationLayer.cpp/.h/.def` 已于 2026-08-29 删除（`e2b812c`，-1015/-49/-7 行） | `git show --stat e2b812c` |
| 现存 `hooked_get_proc_address` 只转发 `D3D11CreateDevice*` 与 `DisplayConfig*` | `Dx11FsrBridge.cpp:14844-14888` |
| `:15138` 注释直言「FSR2 GetProcAddress shim（旧 OptiScaler 接入）已移除」 | — |
| 残留日志块被 `DX11FSRBRIDGE_SERVER_DEBUG_RUNTIME` 门控，而该宏**全仓无定义** | `Dx11FsrBridge.cpp:7019-7032` |
| 该块调用的 `fsr2_get_proc_address_shim_query_mask()` **无定义体** ⇒ 定义该宏会**链接失败** | 同上 |

⇒ `OPTISCALER_COMPATIBILITY.md` 是**过期文档**（最后修改 2026-07-21，早于删除）。本次已将其改写为"当前真实契约"。

### 1.2 v1.1.5 那六个导出**什么都不转发**（历史参考）

从 `git show 2116543:Dx11FsrBridge/Fsr2TranslationLayer.cpp` 可读到：六个符号是**空成功桩**
（`memset` + `return FFX_OK`；ratio/resolution/jitter 是硬编码算术）。它们既没转发游戏静态链接的
FSR2，也没转发 FFX12，**唯一目的**是让 OptiScaler 的"DX11 FSR2 可执行文件扫描 + 初始化"探测成功返回。
真正出图的是桥**自己另一条**静态链接 FSR2 2.3.4 的实例。

这条历史很重要：它证明"桥 + OptiScaler"从来不是"桥把 FSR2 转给 OptiScaler"，而是**两条完全独立的链路并行**。
所以"把 FSR2 输入改成 DLSS 输入"这件事，**不需要**改动 OptiScaler 的任何代码——只需要桥换一个身份。

### 1.3 "OptiScaler 的 FSR2 输入"和"DLSS 输入"最终落到同一处——差别全在参数完整度

OptiScaler 的 `FSR2_Dx11.cpp:107` 与 `NVNGX_DLSS_Dx11.cpp:493` 都调用
`NVSDK_NGX_D3D11_CreateFeature`，且后端选择只由 `[Upscalers] Dx11Upscaler` 决定
（`FeatureProvider_Dx11.cpp:59-83`），**与输入类型无关**。

⇒ 换输入类型**不会**改变 OptiScaler 用哪个后端，只会改变**喂给后端的参数是否完整**。
这正是本改造的全部意义所在：我们修的是"喂什么"，不是"用什么"。

---

## 2. 现状链路（改造前）

```
游戏 DX11 ─┬─ il2cpp inline patch: FFX_FSR2::Render  → per-instance token（jitter/尺寸/帧号）
           ├─ 上下文 vtable 钩（12 方法）→ 状态镜像 DispatchState
           └─ DrawIndexed(3) → 双签名识别（固定槽位 + 动态评分）
                              → try_fsr2_translation_draw
                                   ├ draw→实例 归属（path 1/2/3）
                                   ├ PSGetShaderResources / OMGetRenderTargets 取 5 张纹理
                                   └ ffx12::FrameInput → ffx12::dispatch → AMD ffx-api (DX12, 共享纹理)
                                                                        └─ FSR2 / FSR3 / FSR4
```

关键点：`ffx12::FrameInput`（`Ffx12Backend.h:19-41`）是桥**真正的**输入契约——5 张 `ID3D11Texture2D`
+ 12 个标量。`third_party/fsr2/ffx_fsr2.h` 里的 `FfxFsr2DispatchDescription` **当前没有任何编译单元 include**。

与 OptiScaler 的共存方式：**完全并行，各自出图**，唯一耦合是读 `OptiScaler.ini` 的
`[Upscalers] Dx12Upscaler` 决定要不要做 jitter 钳制（`Dx11FsrBridge.cpp:596-630`，限频 500 ms）。
`passthrough` 机制被整体移除，理由是实测会让 OptiScaler 丢失 FFX 输入识别（`Ffx12Backend.h:116-119`）。

---

## 3. N 卡问题的六个根因（逐条对照代码）

> 这一节是本次改造的**立项依据**。每条都给出"代码事实 → 为什么对 NGX 是错的 → 症状"。

### 3.1 ★ jitter 被**饱和钳制**成常量（最严重）

**代码事实**（`Dx11FsrBridge.cpp:314-322`、`:12286-12355`）：

```cpp
// 默认 Ffx12JitterMode=3：
sdk_in.jitter_x = -(use_jx * render_w) + 0.5f;   // 量级 = 数百像素
...
if (xess_or_dlss) {                               // 只在 OptiScaler 选 XeSS/DLSS 时
    if (v > 0.5f) return 0.5f;                    // ← 饱和钳制
    if (v < -0.5f) return -0.5f;
    return v;
}
```

**为什么错**：NGX 的 `Jitter.Offset.X/Y` 契约是**亚像素、零中心、|v| ≤ 0.5**，且一个 phase 周期内均值≈0。
上面先乘了 `render_w`（把归一化相位放大到像素量级），再**饱和**到 ±0.5 ⇒ 绝大多数样本被压成**恰好 ±0.5 的二值常量**
⇒ 时域上**完全没有新的亚像素信息** ⇒ DLSS 的历史累积无法收敛 ⇒ **拖影 / 边缘抖动 / 画面偏软**。

**仓库自己记录了症状**（`Fsr2FamilyTakeover.h:292-302`）："每个实例的 jitter_px 恒定：分屏那一路 13/13 次采样同值、
另一路 108/108 次同值"，并归因到"重建只能停在单帧放大 + 不收敛的历史"。

**这是"配置改不好"的典型**：`Ffx12JitterMode` 的六个模式**全部**经过同一个钳制分支；换成哪个都不会产生多样性。

> 本次修法：`dlss_input::map_jitter` 用 **wrap（折返）** 而不是 clamp（饱和）。
> 二者在"值本来就在范围内"时**逐位相同**（⇒ 对现有 A 卡行为零影响），值超界时 wrap 保留相位序列的多样性。
> 单测 `DlssInputTest.cpp` 第 1 节直接把这件事钉成数字：**clamp 只保留 2 个不同值，wrap 保留 64 个**。

### 3.2 运动矢量的**符号被翻过两次**，对 DLSS 没有人做适配

**代码事实**：
- 解码自带负号：`mv = -sign(d) * 4d²`（`Ffx12Backend.cpp:498-499`）；
- scale 取 `+renderSize`（`Dx11FsrBridge.cpp:12359-12364`），注释明说为了抵消"游戏 `uv−mv_g`"与"FSR2 `uv+scale·mv`"的差异。

**为什么错**：NGX/DLSS 的约定是 `prev_uv = uv − mv`，**与 FSR2 反号**。把"已经为 FSR2 翻过一次向"的纹理配上
**正** scale 交给 DLSS ⇒ **反向运动** ⇒ 历史重投影方向全反 ⇒ 运动时历史每帧被拒 ⇒ AA 退化成单帧放大。

代码自己也承认这一点：`set_motion_flip()` / `set_depth_scale()`（`Ffx12Backend.cpp:3258-3266`）**零调用点**，
`g_motion_flip` 恒 1.0f，`Dx11FsrBridge.cpp:12340-12343` 注释说"旧实现每帧无条件调用是恒等于默认值的空转"
⇒ **对 XeSS/DLSS 从来没有 MV 符号适配**。

### 3.3 运动矢量是游戏**私有非线性编码**

`-sign(d)*4d²`，偏置 `0.498039`（`Ffx12Backend.cpp:498-499`）。这是小运动压缩、大运动放大的平方律。
DLSS 期望"像素位移的线性度量"。桥解出的 `R16G16_FLOAT` 是**按 FSR2 语义归一化过**的，
直接拿去给 NGX 只有配上**符合 NGX 约定的 scale 符号**才是对的（见 3.2）。

### 3.4 ★ 色彩：把 **PQ 码值** 当 **HDR** 喂给 NGX

**代码事实**：发布默认 `Ffx12PqChain=0`、`Ffx12Hdr=0`、`Ffx12NonLinear=0`（随包 ini:96-98；
注意**代码默认却是 1/1/1**，`:4020-4025`——这本身是个坑）。桥**不做任何色彩空间转换**，
把游戏 PQ 码值直接喂 FFX，靠 `HDR|NON_LINEAR_COLORSPACE|AUTO_EXPOSURE` 三个 flag 让 SDK 自己解释
（`:374-375`、`Ffx12Backend.cpp:1837-1841`，注释写"参照 OptiScaler 实测 initFlags 0x129"）。

**为什么错**：
- OptiScaler 的 FSR2 输入路径**逐位翻译** `FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE → NVSDK_NGX_DLSS_Feature_Flags_IsHDR`
  （`FSR2_Dx11.cpp:68-69`）。所以只要桥把 HDR 位打开，DLSS 就会被告知"这是线性 HDR"。
- 但 **NGX 没有 `NON_LINEAR_COLORSPACE`，也没有 `AUTO_EXPOSURE` 的等价物**。DLSS 拿到 PQ 码值当线性值，
  会走完全不同的色调曲线 ⇒ **暗部发灰 / 高光被压 / 整体偏色**。

**这是真实的架构缺口，不是配置问题**：唯一的补救路径（`Ffx12PqChain=1` 走 `PqToLinear`）在当前后端**不可用**——
`Ffx12Backend.cpp:1697-1701` 在 `ensure_pool` 里直接 `return false`（注释："gpu interop path rejects legacy PQ conversion"），
⇒ `dispatch()` 每帧 `return false` ⇒ 全回落原生。ini 注释也直言"1=白屏，勿改"。

> 本次修法：`dlss_input::map_create_flags` 把"色彩**真实编码**"与"FFX 的 HDR flag"拆成两个概念。
> PQ 码值 **绝不**置 `IsHDR`；只有"线性 + 值域确实 >1"才置。
> 同时 `reasons` 里显式记一条 `pq_input_requires_linearize_before_ngx` —— 让这个缺口**在日志里可见**，而不是静默错色。

### 3.5 `transparencyAndComposition` 在 NGX 侧**根本没有槽位**

**代码事实**：NGX 只有一个 `DLSS.Input.Bias.Current.Color.Mask`（reactive）。
桥的 transparency 抓到但**从未送进 dispatch**（`Dx11FsrBridge.cpp:12799-12803` 自己标注 `transparency_sent=0`），
reactive 则借用 `motion.b` 通道（`Ffx12Backend.cpp:509`）且**从未为 NGX 标定过值域**。

OptiScaler 侧的证据：FSR2 输入把 transparency 塞进**私有键** `"FSR.transparencyAndComposition"`
（`FSR2_Dx11.cpp:303`），而**只有 FSR/FFX/XeSS-Dx11 后端读它**（`FFXFeature_Dx12.cpp:232`、`FSR2Feature_Dx11.cpp:366`）
⇒ **DLSS 输出时完全丢失**。更糟的是 `IFeature_Dx11wDx12`（`DLSS_on12`）会**显式清空**它（`IFeature_Dx11wDx12.cpp:433-434`）。

⇒ 半透明/粒子区域会累积错误的长期历史 ⇒ **鬼影/拖影**。

> 本次修法：`dlss_input::plan_masks` **承认**这个缺失（`note="transparency_dropped_ngx_has_no_slot"`），
> 并提供 `FoldIntoReactive` 选项（取 max 折进唯一的槽位），而不是假装能送。

### 3.6 其余摩擦（都在本次的纯映射层里显式建模）

| # | 事实 | NGX 侧后果 |
|---|---|---|
| a | 深度反向只用 FFX init flag 表达（`Ffx12Backend.cpp:1838`），且**不归一化**（`g_depth_scale` 恒 1.0，`set_depth_scale` 零调用）；游戏逆深度实际值域约 `[0,~0.03]` | NGX 用独立 `depthInverted` 参数，且依赖 `[0,1]` 线性分布的 disocclusion 判断会失真 |
| b | 只有 `renderSize`，**没有 rect**（`:2219`）；尺寸取自纹理 desc 而非游戏上报值（`:12122-12140` 不一致只打日志） | DLSS 需要 render/output **矩形**（超宽、分屏、动态分辨率）；分屏时两路视图共用同一套尺寸判据 |
| c | `preExposure` **桥从不赋值**（恒 1.0f） | NGX 的 `DLSS.Pre.Exposure` 是**唯一的曝光来源**（没有 auto-exposure 兜底） |
| d | reset 是启发式："距上次 dispatch > 500 ms"（`:12105-12121`） | 真实相机跳变不会被捕获（残留历史），一次卡顿又会凭空丢历史 |
| e | 有些"帧"其实是上一帧的**拷贝**（repair `:11902-11960` / reuse `:11968-12025`），不推进 FSR2 历史但**吞掉原生 draw** | 落到 DLSS 会**推进一次历史**（用上一帧的 MV/depth）⇒ 方向性残影 |
| f | cb0 的 jitter **落后一个 token**（代码自陈 `:12143-12147`） | 任何经 cb0 取 jitter 的消费者都继承一帧相位误差 |

---

## 4. 改造方案：换成 **DLSS 输入契约**

### 4.1 为什么是"换身份"而不是"改参数"

OptiScaler 的 DLSS 输入判定**只看模块名**（`DllNames.h:125` + `CompareFileNameW:179-198`）：

```cpp
DEFINE_NAME_VECTORS(nvngx, "nvngx", "_nvngx");   // nvngx.dll / _nvngx.dll（要求前一个字符是 \ 或 /）
```

命中后 `LoadLibraryCheckW` **直接返回 OptiScaler 自己的 `dllModule`**（`LibraryLoad_Hooks.cpp:66-75`）。
于是"桥调用 `LoadLibraryW(L"nvngx.dll")` + `GetProcAddress("NVSDK_NGX_D3D11_*")`"这一串，
拿到的就是 OptiScaler 的完整 NGX 实现 —— 桥**不需要链接任何 NVIDIA 库、不需要 OptiScaler 改一行代码**。

> ⚠️ 一个反直觉但关键的事实：**`nvngx_dlss.dll` 不是 DLSS 输入**。
> `CompareFileNameW` 要求匹配串前面是 `\` 或 `/`，`nvngx_dlss` 不匹配 `nvngx` 这个后缀名。
> `nvngx_dlss.dll` 走的是另一分支（`LibraryLoad_Hooks.cpp:93-102`），返回**真实**的 DLSS 运行时，
> 供 OptiScaler 自己的 DLSS **输出**后端使用。

### 4.2 分层（关键：让 N 卡问题可离线单测）

```
DlssInput.h  ──┬─ 1. 纯映射层   零 D3D / 零 NGX / 零 Windows  → DlssInputTest.cpp 直接断言
               ├─ 2. NGX ABI 层 只声明 ABI 形状（vtable 顺序与 nvsdk_ngx_params.h 逐项一致）
               └─ 3. 驱动层     解析导出 → Init → CreateFeature → 每帧 Evaluate
```

**为什么这样切**：第 3 节列的六条根因**全是语义**（单位、符号、值域、槽位有无）。语义是可以用纯函数钉住的。
把它放在需要 GPU + N 卡 + 游戏才能跑的路径里，就等于"永远只能在别人机器上验证"——这正是本项目
TextureLoader 在 N 卡上的问题至今没定位的原因（README 自己写的："唯一无法在本地复现的环节是驱动的纹理创建/上载时机"）。

### 4.3 纯映射层的九个决策点

| 函数 | 把什么翻成什么 | 对应根因 |
|---|---|---|
| `map_jitter` | 归一化相位 `[0,1)` → NGX 像素抖动（wrap 而非 clamp） | 3.1 |
| `map_motion_scale` | FSR2 语义纹理 → NGX 的 scale 符号（两种修法） | 3.2 / 3.3 |
| `map_create_flags` | 色彩真实编码 → NGX create flags（PQ ≠ HDR） | 3.4 |
| `map_perf_quality` | 连续 ratio → 6 档离散（并规避 OptiScaler 的 UltraQuality 改写） | 附加 |
| `decide_reset` | 多源事实 → reset + **可读原因** | 3.6d |
| `plan_masks` | reactive/transparency → NGX 唯一槽位（缺就承认） | 3.5 |
| `validate_subrect` | 矩形越界校验 | 3.6b |
| `route_output` | 厂商/运行时/用户偏好 → FFX 还是 DLSS（绝不盲目切） | 全局 |
| `build_frame` | 上面全部 → 一帧的 `FrameParams` | 全部 |

### 4.4 三条**绝不**做的事

1. **绝不因为"是 N 卡"就切换**。`route_output` 要求 `dlss_runtime_present == true` 才切；
   解析不到 NGX 就保持现状（`reason="no_dlss_runtime"`）。
2. **绝不静默失败**。任何失败都带可读原因：`create_feature_failed(0x%08X)` / `ngx_init_failed(0x%08X)` /
   `params_null` / `bad_sizes` / `missing=...`。初始化失败后**永久退回 ffx**（不每帧重试），并打一条 WARN。
3. **绝不把桥已有的行为改成"只在 N 卡上对"**。wrap 与 clamp 在值本来合法时**逐位相同**；
   `DlssInput` 默认 **0=关**。A 卡用户不显式打开就完全不受影响。

---

## 5. 落地改动清单

### 5.1 新增文件

| 文件 | 作用 |
|---|---|
| `Dx11FsrBridge/DlssInput.h` | 契约声明：纯映射层 + NGX ABI 形状 + 驱动层 |
| `Dx11FsrBridge/DlssInput.cpp` | 实现（纯映射层零平台依赖；驱动层用 `#if defined(_WIN32)` 隔离） |
| `Dx11FsrBridge/DlssInputTest.cpp` | 237 条断言的离线单测 |

### 5.2 修改文件

| 文件 | 改动 |
|---|---|
| `Dx11FsrBridge/Dx11FsrBridge.cpp` | ① 新增 `dlss_gate` 命名空间（路由 + 首次初始化 + 一次派发）；② `Config` 增 5 个 `dlss_input_*` 字段；③ `load_config()` 读同名 ini 键；④ **唯一派发点**改为"路由决定喂 ffx 还是 NGX" |
| `Dx11FsrBridge/Dx11FsrBridge.cpp`（`record_adapter_info`） | 新增 `g_route_vendor_id`（数值 vendor id）；原先只有 `"0x10DE"` 字符串标签，路由要按厂商决策 |
| `Dx11FsrBridge/CMakeLists.txt` | 主 target 加 `DlssInput.cpp`；新增 `DlssInputTest` target + `add_test` |
| `Dx11FsrBridge/OPTISCALER_COMPATIBILITY.md` | 改写为**当前真实契约**（原文描述的六个导出已不存在） |

### 5.3 派发点的改动（最小侵入）

改造前：

```cpp
if (ffx12::dispatch(sdk_in, context, call_params.instance)) { /* 成功路径 */ }
```

改造后：

```cpp
bool dispatch_ok = false;
if (dlss_gate::resolve_path() == 1)
    dispatch_ok = dlss_gate::dispatch(context, sdk_in, ...);   // 失败 ⇒ 永久退回 ffx
else
    dispatch_ok = ffx12::dispatch(sdk_in, context, call_params.instance);
if (dispatch_ok) { /* 原有成功路径，一行未改 */ }
```

**为什么两条链互斥**：同一帧同时喂两个后端会让两份时域历史互相污染，且两边的 reset/尺寸重建节奏不同。
路由结果**进程内只决定一次**（显卡不会中途变）。

### 5.4 新增 ini 键（`[Dx11FsrBridge]`）

| 键 | 默认 | 含义 |
|---|---|---|
| `DlssInput` | **0** | 0=关（全部走现有 ffx12 链路）1=auto（N 卡 + 能解析到 NGX 才切）2=force（不管显卡，能解析到就切，用于 A 卡 + OptiScaler 的 A/B） |
| `DlssInputJitterReduce` | 0 | 0=**wrap**（保多样性，推荐）1=clamp（复刻旧行为，A/B）2=none（诊断：观察原始量级） |
| `DlssInputMotionFixup` | 0 | 0=负 scale 抵消（零额外 GPU 成本）1=要求解码纹理改为原生符号（语义最干净，需改后端解码符号） |
| `DlssInputModule` | `nvngx.dll` | 被 OptiScaler 拦下的模块名；填绝对路径可直接指定某个 NGX 实现 |
| `DlssInputAllowRealNgx` | 1 | 没有 OptiScaler 时是否直接用 NVIDIA 原生 NGX（= N 卡直出 DLSS） |

**默认关的理由**：切换输入契约等于换掉所有已调好的语义（jitter 符号、MV 符号、色彩空间、auto-exposure）。
在 N 卡实机验证前默认打开，等于让 A 卡用户也承担"被当 N 卡处理"的风险。

### 5.5 日志取证点（新增）

| 日志行 | 何时 | 用途 |
|---|---|---|
| `dlss_input_route path=... reason=... vendor=0x... optiscaler=... ngx=... mode=...` | 首次派发决定路由时 | **一行判定**"为什么没切/为什么切了" |
| `dlss_input_init_ok impl=<nvidia-ngx|optiscaler> module=...` | 初始化成功 | 确认身份 |
| `dlss_input_init_failed error=... missing=... impl=...` | 初始化失败 | 缺哪个导出 |
| `dlss_input_first_dispatch render=..x.. display=..x..` | 首次派发 | 尺寸对账 |
| `dlss_input_frame n=.. jitter_px=..,.. mv_scale=..,.. pre_exp=.. flags=0x.. reset=..(reason) mask=...` | 前 4 帧 + 每 600 帧 | **jitter 真值**（证伪"被钳死"）、MV 符号、reset 原因、mask 策略 |
| `dlss_input_eval_failed count=.. error=..` | 前 8 次 + 每 256 次 | NGX 返回码 |
| `dlss_input_feature_failed error=..` | 建 feature 失败 | 尺寸/flags 问题 |
| `dlss_input_fallback_to_ffx (permanent for this process)` | 派发失败后 | 明确"已退回现状" |

---

## 6. 验证

### 6.1 离线单测（已跑通）

```
=== DlssInputTest ===
[1] jitter: clamp vs wrap diversity
    clamp distinct=2  wrap distinct=64
...
=== 237 checks, 0 failures ===
ALL PASS
```

构建方式（本机用 LLVM-MinGW 的 clang，无需 MSVC/GPU/N 卡）：

```powershell
clang++ -std=c++20 -O1 -Wall -Wextra -static `
  DlssInputTest.cpp DlssInput.cpp -o DlssInputTest.exe
./DlssInputTest.exe
```

CMake 侧也已接入（`DX11FSRBRIDGE_BUILD_TESTS=ON` 时自动 `add_test`）。

覆盖：jitter 的**多样性差异**（本项目最核心的 N 卡缺陷）、wrap 边界与负值对称性、cb0 一帧相位补偿、
MV 符号两种修法、PQ 不得标 HDR、AutoExposure 需真绑曝光纹理、perf quality 量化与改写规避、
reset 优先级、mask 缺失必须承认、subrect 越界、路由的三类"绝不切"、`build_frame` 端到端一致性。

### 6.2 编译验证

**已用真实 MSVC 19.44.35229 完成完整构建**（见 §10）：

```
=== BUILD OK ===
  Dx11FsrBridge\build\Dx11FsrBridge.dll
  size   : 917,504 bytes
  sha256 : 00524F540BF41D2EFD112141F46D8D88708F083F2B4EE1DD846D9ABE9DA52472

            8664 machine (x64)
    d3d12.dll
    D3DCOMPILER_47.dll
    KERNEL32.dll

SEH self-check:
           DD000 [    5910] RVA [size] of Exception Directory
        6000 .pdata
```

构建是**可复现的**（`/Brepro`）：连续两次 `Build-DlssBridge.ps1` 产出逐字节相同的
SHA-256。没有它链接器会把当前时间写进 PE 头，每次重建哈希都变，"这两个二进制是不是
同一份"就变得无法回答。

- 9 个编译单元（8 个 .cpp + 1 个 .rc）全部通过，**零编译警告**。
- 依赖只有系统 DLL（静态 CRT ⇒ 不需要 VC 运行库）。
- **Exception Directory 存在**（0x5910 字节）⇒ 19 处 `__try` 是真正的 SEH 保护。
- 版本资源正确：`FileVersion 2.4.0.0` / `FileDescription "Genshin Impact DX11 FSR Bridge"`。
- 无导出（注入型 DLL），有 `DllMain` 入口点。
- 新代码确实编入：二进制内含 `dlss_input_route` / `dlss_input_init_ok` / `dlss_input_frame` /
  `dlss_input_fallback_to_ffx` 等字符串，以及 5 个 `DlssInput*` ini 键名（宽字符）。

### 6.3 尚未验证（诚实边界）

| 项 | 为什么没验 | 怎么验 |
|---|---|---|
| 真机出图 | 本机无 N 卡、无游戏、无 nvngx.dll | 见 §7 的验证清单 |
| `nvngx.dll` 冒充是否被 NVIDIA 驱动接受 | 需要 RTX + 驱动 | OptiScaler 已用同样手法（`LibraryLoad_Hooks.cpp:74` 返回自身），成功案例充分 |
| `Feature::create` 的 `nullptr` context | DX11 的 NGX 只在 Init 时用 device；DX12 才要求 command list | 若不接受，改成把 immediate context 传进去（一行改动） |
| 多实例（分屏/UI）各自一套 feature | 现在只有一个 `Runtime` 单例 | 扩展 `Runtime` 为 per-instance 表（桥已有 `sdk234_insts[8]` 的骨架） |
| 与现有 ffx-api 路径并存的资源压力 | 需要 GPU | 两条链互斥（同帧只走一条），但 NGX 会额外持有一份历史缓冲 |

---

## 7. 使用与验证清单

### 7.1 最小验证（N 卡 + OptiScaler）

1. 装最新桥 + OptiScaler（顺序：**Bridge 先、OptiScaler 后**，现有脚本已保证）。
2. `Dx11FsrBridge.ini` 里加 `DlssInput=1`。
3. `OptiScaler.ini` 里 `[Upscalers] Dx11Upscaler=dlss`（或 `dlss_on12`）、`Dx12Upscaler=dlss`。
4. 进游戏，开 FSR2 抗锯齿 + 渲染精度 < 1。
5. 看 `Dx11FsrBridge.log`：
   - `dlss_input_route path=dlss reason=nvidia_with_ngx_runtime` → 路由成功；
   - `dlss_input_init_ok impl=optiscaler` → 身份正确；
   - `dlss_input_frame ... jitter_px=<每帧变化的小数>` → **关键**：值必须在 ±0.5 内且**逐帧不同**。
6. 看 `OptiScaler.log`：应出现 `DLSS_DX11` 作为输入 API，而不是 `FSR2X_DX11`。

### 7.2 三条"失败如何定位"的判据

| 现象 | 看哪一行 | 说明 |
|---|---|---|
| 没切过去 | `dlss_input_route ... reason=no_dlss_runtime` | OptiScaler 没加载 / `nvngx.dll` 名字不对 |
| 切了但没图 | `dlss_input_eval_failed ... error=0x...` | NGX 返回码直接可查（`0xBAD0000A` 系列） |
| 有图但拖影 | `dlss_input_frame ... jitter_px` 是否恒定 | 恒定 ⇒ 上游相位本身冻结（见 3.1 的仓库记录），不是本模块的锅 |

### 7.3 A/B 建议（一次只动一个变量）

| 实验 | ini | 预期 |
|---|---|---|
| 确认 jitter 多样性是主因 | `DlssInput=1` + `DlssInputJitterReduce=0` vs `=1` | `=1` 复刻旧症状（拖影/网格纹）；`=0` 应显著改善 |
| 确认 MV 符号 | `DlssInputMotionFixup=0` vs 手改 `Fsr2MotionVectorScaleMode=0` | 反号时运动物体出现**方向性**残影 |
| 确认色彩 | 对比 `Ffx12Hdr=1` vs `=0`（配合 `DlssInput=1`） | 前者偏灰/高光被压 |

---

## 8. 后续路线（按性价比排序）

1. **把 `PqToLinear` 从 FFX 专用链里解耦**。现在 `Ffx12PqChain=1` 会让 `ensure_pool` 直接失败（§3.4），
   而"线性 FP16 + `IsHDR` 视值域而定"才是 DLSS 的正确输入。这是剩余最大的一处画质缺口。
2. **per-instance feature**。分屏/UI 两路视图需要各自的历史（桥的 `sdk234_insts[8]` 已有骨架）。
3. **真实相机跳变信号**。现在 reset 靠"距上次派发 >500ms"这种间接代理；
   `Il2CppCallSiteHook` 已有 `ConfigureJitteredProjectionMatrix` 的观察点，可以升级成真实 reset 触发。
4. **transparency 折进 reactive 的着色器**。现在只有 `FoldIntoReactive` 的**策略开关**，没有实际的 max 合并 pass。
5. **subrect**。分屏/超宽需要真实 rect；现在恒为全尺寸。

---

## 9. 风险与合规

- 本改造**不引入任何 NVIDIA 私有头/库**：`DlssInput.h` 只声明 ABI 形状（vtable 顺序与签名逐一对应公开的
  `nvsdk_ngx_params.h`），运行期通过 `GetProcAddress` 取符号 —— 与桥现有对 `ffx-api` 的做法一致。
- 桥仍是 **GPL-3.0-or-later**；本改造文件同许可。
- `DlssInput` 默认关闭 ⇒ 不改变任何现有用户的默认行为。
- 注入第三方 DLL 的**账号风险**不因本改造增加或减少（`README.md` 已有的警告继续适用）。

---

## 10. 构建（本机实测可复现）

### 10.1 为什么不能用 MinGW

桥有 **19 处 `__try`**，全部用于安全探测：

| 位置 | 保护的操作 |
|---|---|
| `Dx11FsrBridge.cpp:4531` | 读游戏 D3D 资源描述 |
| `Dx11FsrBridge.cpp:5325` | `WriteProcessMemory` 改 IAT |
| `Dx11FsrBridge.cpp:5371` | `hook_iat` 整体 |
| `Dx11FsrBridge.cpp:10166` | `memcpy` 读游戏内存 |
| `Il2CppCallSiteHook.cpp:593/606` | 读相机/投影矩阵 |
| `RenderScaleMenu.cpp:353/406/507/522/593` | 读游戏 UI 结构体、写字符串 |
| `TransparentJitterHook.cpp:102` | 校验序言字节 |

MinGW clang 在 `windows-gnu` ABI 下**能编译 `__try` 但不生成 SEH 表**——实测：

```
$ clang++ -fms-extensions -fdeclspec sehav.cpp && ./a.exe
进程直接终止 (0xC0000005)，__except 未被调用
```

这是一个**静默**失效：编译无警告、运行看起来正常，但任何一次游戏更新导致的签名错位或越界读
都会**直接带崩游戏进程**，而不是按设计优雅回退。所以必须用 MSVC 构建。

### 10.2 免管理员准备工具链

本机没有 Visual Studio、没有管理员权限。用 VS BuildTools 的 **`--layout` 模式**（只下载、不安装）
取出 `cl.exe`，再从 NuGet 取 Windows SDK：

```powershell
# 1) VS BuildTools bootstrapper
curl -L -o vs_buildtools.exe https://aka.ms/vs/17/release/vs_buildtools.exe

# 2) 只下载 layout（不安装，无需管理员）
.\vs_buildtools.exe --layout D:\...\gh\_vs\layout --lang en-US --quiet --wait `
  --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64

# 3) 从 layout 里取 payload.vsix 并展开，组装成
#    _vs\msvcroot\VC\Tools\MSVC\14.44.35207\{include, lib\x64, bin\Hostx64\x64}
#    需要的包：Tools.HostX64.TargetX64.base（cl.exe）
#             Tools.HostX64.TargetX64.Res.base（clui.dll）
#             CRT.Headers.base（vcruntime.h 等 269 个头）
#             CRT.x64.Desktop.base（libcmt.lib / libvcruntime.lib）

# 4) Windows SDK 头与库（NuGet）
#    头：Microsoft.Windows.SDK.CPP        -> c\Include\10.0.22621.0
#    库：Microsoft.Windows.SDK.CPP.x64    -> c\Lib\10.0.22621.0\{ucrt,um}\x64
```

**两个必须知道的坑**（都踩过）：

1. **`libvcruntime.lib` 必须用桌面版**（2,087,406 字节），不能用 enclave/onecore 版
   （2,063,722 字节）。只有桌面版定义 `__crt_debugger_hook`，否则链接报
   `LNK2019: unresolved external symbol __crt_debugger_hook`。
2. **`rc.exe` 在 Windows SDK 里**，不在 MSVC 里：
   `c\bin\10.0.22621.0\x64\rc.exe`。

### 10.3 一条命令构建

```powershell
cd Dx11FsrBridge
powershell -ExecutionPolicy Bypass -File .\Build-DlssBridge.ps1 -Clean
```

脚本会做：preflight 检查（缺什么直接报，不静默降级）→ 生成 `Version.h` →
编译 8 个 .cpp + 1 个 .rc → `link.exe` 链接 → dumpbin 自检（架构/依赖/**SEH 表**）。

产物落在 `Dx11FsrBridge\build\Dx11FsrBridge.dll`。

> ⚠️ 脚本刻意写成**纯 ASCII**：Windows PowerShell 5.1 会按 ANSI 读 `.ps1`，
> 中文注释会破坏语法解析。

---

## 11. 娈嬬暀闂锛氫笁娓蹭簩绾挎潯鎶栧姩锛? 甯х浉浣嶅惊鐜紝宸插埌涓婇檺锛?

> 鏈妭璁板綍涓€娆?*瀹屾暣鐨勬帓闅滆繃绋?*锛屽寘鎷笁涓瀹炴祴璇佷吉鐨勫亣璇淬€?
> 缁撹鏄細杩欐槸**娓告垙渚х殑纭檺鍒?*锛屾ˉ鍐呮棤娉曠粫杩囥€?

### 11.1 鐥囩姸

涓夋覆浜岃鑹诧紙鍙搷浣滆鑹蹭笌 NPC锛夌殑**鎻忚竟銆佸ご鍙戙€佽。鎽嗚竟缂?*鎸佺画鎶栧姩銆?
鍏抽敭鐗瑰緛锛?*闈欐绔欑潃涓嶅姩涔熷湪鎶?*锛屼笖 M/L 妯″瀷姣?K 鏄庢樉锛堟椂鍩熸潈閲嶆洿楂橈級銆?

### 11.2 涓変釜琚瘉浼殑鍋囪锛堟寜灏濊瘯椤哄簭锛?

| # | 鍋囪 | 瀹炴祴缁撴灉 | 璇佷吉渚濇嵁 |
|---|---|---|---|
| 1 | alpha 娣峰悎杈圭紭鏈璇嗗埆 鈫?寮€ `AlphaUpscaling`(1<<7) | **瀹屽叏娌″彉鍖?* | alpha 鍙湪**杩愬姩**鏃堕€氳繃鍘嗗彶绱Н璧蜂綔鐢紝鑰岀棁鐘舵槸闈欐涔熸姈 |
| 2 | jitter 瓒婄晫琚姌杩斿鑷存柟鍚戠炕杞?| **娌″彉鍖?* | 闃舵鍒掑垎鏁板绛変环锛歚wrap(0.5-pix)` 鈮?`0.5-fract(pix)`锛屽瀹炴祴鍊?6/6 鐩稿悓 鈬?閭ｆ"淇"鏄?no-op |
| 3 | 娓告垙鐩镐綅鍐荤粨 鈫?鏀圭敤鑷敓鎴?Halton | **鐢婚潰鍏ㄩ潰鎶栧姩锛屾洿绯?* | DLSS 鐨?`Jitter.Offset` 蹇呴』绛変簬娓告垙**瀹為檯鏂藉姞**鐨勭浉浣嶏紝鍑┖鐢熸垚浼氳瀹冩瘡甯у幓閿欒浣嶇疆鎵惧巻鍙?|

> 鈿狅笍 杩欎笁鏉￠兘婧愪簬"娌℃祴閲忓氨鏀逛唬鐮?銆傜 2 鏉″挨鍏跺吀鍨嬶細鎴戞妸**涓棿鍊?*瓒婄晫
> 褰撴垚浜?bug锛岃€?`wrap_to_half_pixel` 鏃╁凡鎶婂畠澶勭悊鎺変簡銆?

### 11.3 瀹炴祴鍒扮殑鐪熷疄鎯呭喌

鐢?F11 鐑敭瑙﹀彂杩炵画 200 甯ч噰鏍凤紙`DlssInputJitterProbe=1`锛夛紝寰楀埌 866 甯ц繛缁暟鎹細

```
jitter 鎹㈢畻**瀹屽叏姝ｇ‘**锛歯orm*W=0.501 鈫?璁＄畻 -0.001 鈫?瀹炴祴 +0.000  (8/8 鍖归厤)
鐩镐綅瑕嗙洊 81%锛氳寖鍥?-0.375 ~ +0.438锛圢GX 濂戠害 卤0.5锛?
```

鍞竴鐨勫紓甯告槸**鍛ㄦ湡**锛?

```
瀹炴祴搴忓垪锛? 甯у悗瀹屽叏閲嶅锛夛細
  +0.000  +0.250  -0.250  +0.375  -0.125  +0.125  -0.375  +0.438
  鈫?绗?9 甯у洖鍒?+0.000

866 甯ц繛缁噰鏍凤細鍛ㄦ湡 8 甯х殑鍖归厤鐜?**96%**
锛?6/24/32 甯т篃 96%锛屽洜涓洪兘鏄?8 鐨勫€嶆暟锛?
```

杩?8 涓浉浣嶅搴?`norm*W` = `0.501 0.250 0.749 0.125 0.626 0.376 0.874 0.061`锛?
姝ｆ槸**鏍囧噯 Halton(2) 鐨勫墠 8 椤?*銆備絾鏍囧噯 Halton 浼氭棤闄愬欢浼革紝娓告垙鍗存埅鏂洖浜嗗紑澶淬€?

### 11.4 cb0 绌蜂妇缁撴灉锛堟帓闄?瀛樺湪鏇撮暱鐩镐綅鏉ユ簮"锛?

鎵弿鏁翠釜 cb0锛?..127 float 瀵癸級锛屽彧鎵惧埌 4 涓€欓€夋Ы锛?

| 妲戒綅 | 瀹炴祴琛屼负 |
|---|---|
| `[112]/[113]` | **涓?il2cpp 瀛楁瀹屽叏鍚屾**锛屽悓涓€缁?8 鐩镐綅銆佸悓鏍?8 甯у懆鏈?|
| `[110]/[111]` | 鎭掑畾 `0.000391, 0.000694` |
| `[106]/[107]` | 鎭掑畾 `0.000391, 0.000695` |
| `[124]` | 鎭?`0,0`锛堟棩蹇楅噷鐨?0鈫? 鏄亣鍙樺寲锛墊

`cb0[112]` 鐨勫疄娴嬪簭鍒椾笌 Halton(2) 閫愰」鍚诲悎锛堝樊寮備粎 10-bit 閲忓寲锛夛細

```
瀹炴祴: 0.6262 0.3757 0.8742 0.0613 0.5010 0.2505 0.7489 0.1252
鐞嗚: 0.625  0.375  0.875  0.0625 0.5    0.25   0.75   0.125
```

鈬?**cb0 閲屾病鏈変换浣曟Ы浣嶆彁渚涜秴杩?8 甯х殑鍛ㄦ湡銆?*

### 11.5 涓轰粈涔堜笉鑳借嚜宸辫ˉ闀垮簭鍒?

DLSS 鍐呴儴锛?

```
prev_uv = uv - mv*scale + jitter
```

瀹冪敤 `Jitter.Offset` **鍙嶆帹鏈抚浠庡摢涓簹鍍忕礌浣嶇疆閲囨牱**銆傝嫢鎶ヤ竴涓父鎴忔病鏂藉姞鐨勭浉浣嶏紝
瀹冧細鍘婚敊璇綅缃壘鍘嗗彶 鈥斺€?瀹炴祴缁撴灉鏄?*鏁翠釜鐢婚潰鎶栧緱鐩稿綋鍘夊**锛堜笉浠呯嚎鏉★級銆?

**鐪熷疄浣嗙煭锛? 鐩镐綅锛夎儨杩囪櫄鍋囦絾闀裤€?*

### 11.6 缁撹涓庡彲閫夐」

| 鏂规 | 鍙鎬?|
|---|---|
| 鎵炬父鎴忛噷鏇撮暱鐨勭浉浣嶆潵婧?| 鉂?宸茬┓涓?cb0锛屽彧鏈?8 鐩镐綅 |
| 璁╂父鎴忚緭鍑烘洿闀垮簭鍒?| 鉂?闇€瑕佹敼娓告垙 |
| 妗ュ唴琛ラ暱搴忓垪 | 鉂?瀹炴祴鏇寸碂锛堢浉浣嶄笌娓告垙涓嶅悓姝ワ級|

杩欐槸**鎵€鏈?DLSS 杈撳叆鏂规鐨勫叡鍚岄檺鍒?*锛堜笂娓稿畼鏂?NGX 鍚庣鐨勫悓绫婚棶棰樻洿涓ラ噸锛?
K 鎷栧奖 + M/L 鎶栧姩锛夛紝涓嶆槸鏈疄鐜扮殑缂洪櫡銆?

### 11.7 淇濈暀鐨勮瘖鏂伐鍏?

`DlssInputJitterProbe=1` 寮€鍚悗锛屾寜 **F11** 鍙Е鍙?200 甯ц繛缁噰鏍凤細

```
dlss_jitter_cadence_armed from_n=8173 frames=200 (press F11 again to re-arm)
dlss_input_frame n=8174 ... norm=... jitter_px=...
```

榛樿鍏抽棴锛岄浂寮€閿€锛坄GetAsyncKeyState` 璋冪敤琚厤缃煭璺級銆傜敤浜庡皢鏉ュ鏍哥浉浣嶅簭鍒椼€?
