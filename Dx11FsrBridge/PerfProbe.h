#pragma once
// PerfProbe.h — **分段性能探针**（诊断用，默认关）。
//
// 【它回答什么问题】
//   症状：帧率上不去 / 不稳，但 GPU 占用很低（⇒ 每帧在 CPU 侧多花 0.4~0.6 ms）。
//   本探针把"每帧的时间"拆到**具体步骤**上，用 QueryPerformanceCounter 实测，
//   不靠推理。用户实测读数即可判定元凶在哪一段。
//
// 【为什么是 header-only】
//   主干（`Dx11FsrBridge.cpp` / `Ffx12Backend.cpp` / `Il2CppCallSiteHook.cpp` /
//   `TransparentJitterHook.cpp`）都要埋点，而 `Ffx12Backend.cpp` 与
//   `TransparentJitterHook.cpp` **各自还被单测 target 单独编译**
//   （见 `CMakeLists.txt`：`Ffx12BackendTest` / `TransparentJitterHookTest`）。
//   做成 header-only（`inline` 变量 + `inline` 函数）后，任何 TU 直接 include 即可，
//   **不必改任何现有 test target 的源文件列表**，也就不会出现"加了 .cpp 但某个
//   target 忘了加 ⇒ 链接失败 / 单测腐烂"这一类静默问题。
//
// 【分段与"重合"纪律（重要 ✓）】
//   段之间**有嵌套关系**，直接求和会重复计数。本文件把段分成两类：
//     - **可累加（top-level）**：彼此不重合，只有它们进 `acct`：
//         present / il2cpp_observer / draw_hook / dispatch_hook / jitter_observer /
//         gpu_query / config_io
//     - **嵌套（nested）**：一定发生在上面某段**内部**，只单独报告、不进 `acct`：
//         upscale_dispatch（⊂ draw_hook 或 present）
//         interop_* / finish_*（⊂ upscale_dispatch）
//   ⇒ 日志行里同时给 `acct_us/frame` 与 `unacc_us/frame`：
//     `unacc` = 帧预算 − acct ⇒ **包含游戏自身耗时**，所以它的用途是"当 acct 很小
//     而帧预算很大时，说明开销不在桥的这些段里"（这本身就是一条结论 ✓）。
//
// 【开销自证（错误清单第 10 条要的反面：加探针必须先证明探针便宜）】
//   - 每帧级段的 `Scope` = 2×QPC + 3 次 relaxed 原子加（实测量级 **~50 ns/次**）；
//     每帧此类段 ≤ 10 个 ⇒ **< 1 µs/帧**。
//   - **draw 钩子绝不逐次计时**（错误清单第 10 条：每 draw 2×QPC + 4 原子在
//     80k draw/s 时吃掉 7~8% 帧数 ⇒ 那测的是探针不是代码）。
//     改为**按 `PerfProbeDrawSample` 抽样**（默认每 32 次测 1 次），并用
//     `抽到的总时长 × (实际次数 / 抽样次数)` 外推 ⇒ 开销降到 1/32，且 max 仍保留。
//   - `upscale_dispatch` 与 `interop_*` **不额外取时间**：直接复用 Ffx12Backend
//     已有的 `qpc_us()` 分段测量（`add_span_us`）⇒ 这些段零附加开销。
//   - 日志：**每 `PerfProbeIntervalMs`（默认 1000 ms）两行**，不刷屏：
//     ① `perf_probe …` 汇总行；② `perf_probe_hist_draw …` **单次调用成本直方图**
//     （见 `detail::DrawCostHistogram`）。②回答的是①**结构上**回答不了的问题：
//     "均值是不是被少数巨贵调用带偏的"（B104 实测：逐 draw 的 COM 调用都是纳秒级，
//     与"每 draw 2.26 µs"的均值矛盾 ⇒ 必须先看**分布**，再决定优化方向）。
//     ③ `perf_probe_funnel_draw …`（B106）**内省漏斗** —— 逐 draw 内省函数里
//     **每一道判定之后还剩多少**（见 `FunnelStage`）+ 工作量计数 `views_per_call`。
//     只在真的抽到过内省调用的区间才输出这一行。
//     ④ `perf_probe_draw_shape_draw …`（B107/B108，**每次 flush 四行**）——
//     **调用入口**的实参分布（形状 Top-N 的 `calls/hits` + 被识别形状清单 +
//     **元素数直方图**）。B108 起：①形状表改成"每区间按调用数保留 Top-N"（B107 的
//     128 槽**键永不清**在真机上立刻饱和 ⇒ `identified` 全落 `other` ⇒ 形状统计失效）；
//     ②`identified` 改为**精确口径**（含淘汰/溢出的识别数）并与**漏斗层**的
//     `identified_total` 交叉核对（不相等就打 `perf_probe_draw_shape_mismatch`）；
//     ③新增元素数直方图（**带 hits**）⇒ 可直接读出"`count==3` 是不是必要条件"。
//   - 探针自身开销估算也打进那一行（`self_est_us/frame`），让读数可以自我核对。
//
// 【入口级过滤器（B109，**正式功能、默认开**；不属于诊断）】
//   真机（B108 的元素数直方图）证明 `count == 3` 是**唯一有 hits 的桶**
//   （`elem_ix 3` / `elem_dr 3`，其余区间 hits 全 0，`outside_hits=0`）
//   ⇒ 在**调用入口**用调用实参淘汰 `element_count != 3` 的绘制，可拒掉约 92.2%
//   的内省调用（≈203 µs/帧）。判据装在 `inspect_target_upscaler_draw_on_demand`
//   的入口（`perf_funnel.mark(entry)` 之后、任何 COM 调用之前）。
//   ⚠️ 代码层面两套签名**都不检查 `element_count`** ⇒ 判据只是"样本内的必要条件"，
//   **不是代码级等价** ⇒ 因此带两条安全机制（见下两节）：
//     ① **自我证伪保险**：每一次识别成功处都检查本次绘制的 `element_count`，
//        一旦出现任何一次非 3 的识别成功 ⇒ **一次性单向**停用过滤器并打一行
//        `draw_entry_filter_disabled reason=non3_identified element_count=N entry=ix|dr`，
//        之后一律走原路径（**fail-open：只会退回慢路径，不会漏目标**）；
//     ② **canary（抽样放行）**：过滤器生效时按 `DrawEntryFilterCanary` 抽样放行
//        非 3 绘制（默认每 64 个放 1 个）⇒ 保险**可观测**（否则它永远不会被触发，
//        因为没有非 3 绘制能走到识别成功处 —— 那是"假的安全感"）。
//   - 状态与区间计数打进两行：汇总行末尾 `entry_filter=on|off|disabled`，
//     漏斗行 `| entry_filter=… stride=… skip=… canary=… non3_id=… disable_ec=… disable_entry=…`；
//   - `skip=`（本区间被入口淘汰的调用数）与形状行第四行的 `count3 reject=` 可对账：
//     `skip + canary ≈ reject`（差额 = 走了钩子但没进内省的绘制：family gate /
//     翻译接管 / HDR 直通）。
//
// 【直方图两个槽的口径（B105 修正，读这两列前必看）】
//   `draw_all`     = **整个 draw / draw_indexed 钩子体**（含 `g_original_draw_*`
//                    —— **游戏自己的绘制提交**，桥不装也照样要花）。落桶在
//                    `SampledScope` 的**析构**里 ⇒ 任何 return 路径都盖得到。
//   `draw_inspect` = **只包住 `inspect_target_upscaler_draw*` 调用本身**；本次 draw
//                    没进入内省（family gate 命中 / 翻译接管 / HDR 直通）时记 0。
//   两槽样本集合都由 `perf_scope.sampled()` 决定 ⇒ **恒等**，因此
//     `mean(draw_all) − mean(draw_inspect)` = 每次调用的**非内省部分**。
//   ⚠️ `entered=` 是 slot 1 里"非 0 样本"的个数；它远小于 `draws_sampled` 时，
//      slot 1 的均值被"未内省记 0"稀释过，别按"内省只要这么点时间"去读。
//   ⚠️ `draw_all` **不等于"桥的开销"**：它把游戏自己的 `DrawIndexed` 提交也算进去了。
//      判"桥净增多少"要看 `draw_inspect` 与"同一秒 `per_frame_us drawhook=` ÷
//      `n draw_pf=`"的对比，不能直接拿 `drawhook` 当桥的成本。
//
// 【ini 键（段位 `[Dx11FsrBridge]`）】
//   PerfProbe=0              总开关，**默认 0**（关）。非 0 = 开。
//   PerfProbeIntervalMs=1000 汇总行间隔（毫秒）。0 视为默认。
//   PerfProbeDrawSample=32   draw/dispatch 钩子的抽样步长（1 = 每次都测）。
//   ---- 以下两个键属于**正式功能**（`DrawEntryFilter`），与 `PerfProbe` **无关**：
//        即使 `PerfProbe=0`，入口过滤器照样生效（保险也照样会打告警行）----
//   DrawEntryFilter=1        入口级过滤开关：1 = 启用（默认）；0 = 关闭，走原路径。
//   DrawEntryFilterCanary=64 canary 抽样步长：N = 每 N 个非 3 绘制放行 1 个进内省
//                            （保险要能看到非 3 的识别才可能触发）；
//                            0 = 不放行（完全信任判据）；1 = 全部放行（等价于不过滤）。
//
// 【与日志的关系】本模块刻意不依赖 BridgeLogger：日志行通过 `set_log_sink` 注入
//   （与 TransparentJitterHook 同一纪律），这样单测只需本头文件 + Windows API。

#include <Windows.h>

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace perf_probe
{

// 日志出口（由调用方注入；为空则只统计不输出）。line 以 '\0' 结尾。
using LogSink = void (*)(const char *line);

// ===========================================================================
// 【发布构建开关（2026-09-27）】
//
//   `DX11FSRBRIDGE_ENABLE_DIAGNOSTICS` 已定义 ⇒ 诊断核心参与编译（开发/排查构建）；
//   未定义（发布构建的默认）            ⇒ 诊断核心**一行都不参与编译**。
//
// 范围（发布构建里被排除的）：
//   分段计时（Segment / Scope / SampledScope）、抽样与直方图（DrawCostHistogram /
//   DrawElementHistogram）、形状分桶表（DrawShapeTable，B107/B108）、内省漏斗
//   （FunnelStage / FunnelScope）、计数器（Counter）、汇总与格式化
//   （flush_now / format_*_report / *_name 表）——**全部汇总/格式化字符串都在这一侧**。
//
// 范围（**两个分支都编译**，因为它们是**正式功能**）：
//   - B109 入口级过滤器（`draw_entry_filter_admit` 等，见下面 B109 小节）；
//   - B109 的**自我证伪告警**（`note_draw_identified_element_count` + 经 `set_log_sink`
//     注入的出口）——它要写清是哪个钩子入口（`ix` / `dr`）⇒ 依赖形状槽打包。
//
// ⚠️ 本文件**不删任何代码**：排除靠这一个开关。将来排查问题打开开关即可恢复全部工具。
// ===========================================================================
#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)

// ---- 段定义（顺序即输出顺序；新增段必须同步 k_segment_names 与单测）----
enum class Segment : std::uint32_t
{
    present = 0,          // hooked_present 整体（⚠️ 只有装了 Present 钩子才有数）
    il2cpp_observer,      // il2cpp render 入口观察者（桥侧回调整体）
    draw_hook,            // draw / draw_indexed 钩子整体（**抽样**测量）
    dispatch_hook,        // compute dispatch 钩子整体（**抽样**测量）
    jitter_observer,      // TransparentJitterHook 的 setter observer 整体（每次调用）
    gpu_query,            // GPU timestamp query 的 begin/end/采集
    config_io,            // 帧路径上的配置/文件 IO（OptiScaler ini 探测等）
    render_scale,         // RenderScaleMenu 的两个游戏钩子（写渲染精度）
    // ---- 以下为**嵌套**段：只单独报告，不计入 acct ----
    upscale_dispatch,     // ffx12::dispatch 整体
    interop_prep,         // 互操作输入准备（motion 解码 CS + CopyResource）
    interop_signal,       // ctx4->Signal（D3D11 侧提交段）
    interop_queue_wait,   // g_queue->Wait（D3D12 队列插入等待，CPU 侧应≈0）
    interop_submit,       // D3D12 侧提交（含 ring 槽位阻塞等待）
    finish_wait,          // finish_pending：等共享 fence（ctx4->Wait）
    finish_copy,          // finish_pending：输出 CopyResource
    count
};

// ---- 计数事件（重量级系统调用：只看**次数**就够判断"是否在热路径上"）----
enum class Counter : std::uint32_t
{
    virtual_protect = 0,        // VirtualProtect（重量级；在热路径上就是元凶级）
    flush_instruction_cache,    // FlushInstructionCache（同上）
    sleep,                      // Sleep / SleepEx
    wait_single_object,         // WaitForSingleObject（显式阻塞等待）
    config_read,                // GetPrivateProfile*（文件/注册表 IO）
    file_probe,                 // GetFileAttributes / std::filesystem::exists 之类
    count
};

// ---- 内省漏斗的阶段（B106）----
//
// 【它回答什么问题】
//   `inspect_target_upscaler_draw_on_demand` 是一串判定（OM 查询 → 双 RTV 预筛 →
//   PS 资源查询 → 正缓存快路径 → cb0 判据 → 视口/输出候选判据 → 读 9 个视图 → 两套签名）。
//   真机只知道"总量 = 每次绘制 0.33~0.53 µs × 490 次/帧"，**不知道每一道各拦下多少**
//   ⇒ 也就回答不了"哪一道最便宜且最有区分度"。
//   这里给每一道判定**之后还剩多少**记一个数（抽样）。
//
// 【漏斗形状与判定顺序无关】
//   每个阶段计的是"满足前 k 条**纯谓词**的 draw 数"（谓词无副作用、彼此无依赖）
//   ⇒ 重排判定不改变任何一个阶段的集合 ⇒ 新顺序的漏斗同时就是旧顺序的漏斗。
//
// 【口径】纯诊断：不参与 acct、不参与 draw_all/draw_inspect 直方图；跟 `PerfProbe`
//   总开关（默认关），抽样步长复用 `PerfProbeDrawSample`（**不新增 ini 键**）。
enum class FunnelStage : std::uint32_t
{
    entry = 0,        // 进入内省函数（= 被抽样到的调用数）
    after_om,         // OMGetRenderTargets(2) 返回后
    after_prescreen,  // 双 RTV 预筛通过
    after_ps_query,   // PSGetShaderResources(0,7) + PSGetConstantBuffers(0,1) 返回后
    after_fast_path,  // 正缓存快路径**未命中**（继续往下）
    fast_hit,         // 正缓存快路径命中（与 after_fast_path 互斥，和 = after_ps_query）
    after_cb,         // 便宜判据①通过：cb0 ByteWidth >= 464
    after_viewport,   // RSGetViewports 拿到可用视口
    after_output,     // 便宜判据②通过：视口尺寸上存在 RTV 输出候选（随后才读 7 个 SRV）
    fixed_ok,         // 第一签名（固定槽位）识别成功
    dynamic_ok,       // 第二签名（动态槽位评分）识别成功
    identified,       // 任一签名识别成功（= fixed_ok + dynamic_ok）
    count
};

// ---- 按"绘制形状"分桶（B107：在**调用入口**用**调用参数**淘汰）----
//
// 【它回答什么问题】
//   B106 的漏斗证明：函数**体内**的便宜判据已经用尽（cb0 只拦 7.5%、视口 0%），
//   而**调用次数**本身才是问题：每帧 ~490 次内省调用只为找到 1 个真目标
//   （`identified = 8 / 3959`）。⇒ 必须在**调用入口**用**调用参数**淘汰。
//
//   本表按"钩子类型 + 调用参数"分桶，**同时**记录两列：
//     calls = 该形状的内省调用次数；hits = 其中被识别为真目标的次数。
//   只有这两列放在一起，才能看出"目标调用的形状是否高度集中"
//   （例如 8/8 个目标都是 `count=3 start=0 base=0`）——那是入口级判据的唯一合法来源。
//
// 【为什么不能只看参数、不看 hits】
//   `count=3` 这类形状**可能**占了全部调用的 80%（UI 精灵也是 3 顶点）
//   ⇒ 只有 `hits` 能说明它是不是目标的必要条件。
//
// 【"形状"的定义（只用入口已有的实参，零额外 COM 调用）】
//   索引绘制（`DrawIndexed` / `DrawIndexedInstanced`）：
//     count=IndexCount、start=StartIndexLocation、base=BaseVertexLocation、inst=InstanceCount
//   非索引绘制（`Draw` / `DrawInstanced`）：
//     count=VertexCount、start=StartVertexLocation、base=0、inst=InstanceCount
//   ⇒ **不读拓扑**：那要额外一次 `IAGetPrimitiveTopology` COM 调用，
//     本身就要花钱；先用"已经拿在手里的实参"做第一轮分桶。
//
// 【口径 / 纪律】
//   - 纯诊断：不参与 `acct`、不参与 draw_all/draw_inspect 直方图、**不改变识别集合**；
//   - 跟 `PerfProbe` 总开关（默认关），**不新增 ini 键**；
//   - **不抽样**（与漏斗不同）：调用次数本身就是要回答的问题，抽样会把它算错；
//     每桶只加 2 次 relaxed 原子；探针关时 `DrawShapeMark` 只有一次 relaxed 读；
//   - 桶**不复位**：形状数量天然有限（实测量级 8~30 且长期稳定），一旦占满就
//     继续累加到已认领的桶上 ⇒ **不会有"新形状挤压老形状"的漏斗效应**；
//     万一真的占满，未认领的新形状落到 `other_slot`（不丢总数，丢的是细分）；
//   - `calls` 总数由 `g_shape_total` 单独累加 ⇒ **可与 `perf_probe` 的 `draws=` 对账**，
//     对不上就说明"有绘制没进内省"（另一条独立结论）。
enum class DrawKind : std::uint32_t
{
    indexed = 0,          // DrawIndexed
    indexed_instanced,    // DrawIndexedInstanced
    non_indexed,          // Draw
    non_indexed_instanced // DrawInstanced
};

inline int draw_kind_index(DrawKind kind)
{
    const auto value = static_cast<std::uint32_t>(kind);
    return value < 4u ? static_cast<int>(value) : -1;
}

inline const char *draw_kind_name(int index)
{
    switch (index)
    {
    case 0: return "ix";
    case 1: return "ixi";
    case 2: return "dr";
    case 3: return "dri";
    default: return "?";
    }
}

inline bool draw_kind_is_indexed(int index)
{
    return index == 0 || index == 1;
}

// ---- 形状槽的打包/解包（位域固定，版本内稳定）----
//
// bits 0..15  = 元素数（IndexCount 或 VertexCount，饱和到 0xFFFF）
// bits 16..31 = start（StartIndexLocation 或 StartVertexLocation，饱和）
// bits 32..59 = base（BaseVertexLocation 的**低 28 位**，有符号；非索引绘制恒 0）
// bits 60..63 = 种类（`DrawKind + 1`，1..4；0 保留给"空槽"）
//   ⚠️ base 只有 28 位：D3D 的 BaseVertexLocation 是 INT，但实际取值是"模型顶点数"
//   量级（几千）。28 位带符号 = ±1.34 亿，远超任何真实网格；超出则**饱和**，
//   饱和值不会与任何真实形状混淆（不会把两个真实形状合并成一个桶）。
constexpr std::uint64_t k_draw_shape_value_max = 0xFFFFull;
constexpr std::int32_t k_draw_shape_base_min = -134217728;  // -2^27
constexpr std::int32_t k_draw_shape_base_max = 134217727;   //  2^27 - 1

inline std::uint64_t pack_draw_shape(int kind_index, std::uint32_t element_count,
                                     std::uint32_t start, std::int32_t base_vertex)
{
    const std::uint64_t count = element_count > k_draw_shape_value_max
        ? k_draw_shape_value_max : static_cast<std::uint64_t>(element_count);
    const std::uint64_t first = start > k_draw_shape_value_max
        ? k_draw_shape_value_max : static_cast<std::uint64_t>(start);
    const std::int32_t clamped = base_vertex < k_draw_shape_base_min ? k_draw_shape_base_min
        : (base_vertex > k_draw_shape_base_max ? k_draw_shape_base_max : base_vertex);
    const std::uint64_t base_bits = static_cast<std::uint64_t>(static_cast<std::uint32_t>(clamped)) & 0x0FFFFFFFull;
    const std::uint64_t key = count | (first << 16) | (base_bits << 32);
    const std::uint64_t kind_bits = static_cast<std::uint64_t>(kind_index + 1) << 60;
    return key | kind_bits;
}

inline int unpack_draw_shape_kind(std::uint64_t packed)
{
    return static_cast<int>((packed >> 60) & 0xFull) - 1;
}

inline std::uint32_t unpack_draw_shape_count(std::uint64_t packed)
{
    return static_cast<std::uint32_t>(packed & 0xFFFFull);
}

inline std::uint32_t unpack_draw_shape_start(std::uint64_t packed)
{
    return static_cast<std::uint32_t>((packed >> 16) & 0xFFFFull);
}

inline std::int32_t unpack_draw_shape_base(std::uint64_t packed)
{
    // ⚠️ 两步都不能少：
    //   ① 先与 4 个"种类"位（bit 60..63）隔离 —— 不隔离的话 `base=0` 会被读成
    //      `0x10000000`（B107 单测当场抓到），因为种类位与 base 共享高 32 位；
    //   ② 再做 28→32 位的**符号扩展**（左移 4 位再算术右移 4 位，值必然落回
    //      int32 范围，避免"左移溢出"这种实现定义行为）。漏了这一步，
    //      `base=-7` 会被读成一个巨大的正数（单测同样当场抓到）。
    const std::uint32_t low28 = static_cast<std::uint32_t>((packed >> 32) & 0x0FFFFFFFull);
    const std::uint32_t shifted = low28 << 4;
    const std::int32_t signed_shifted = static_cast<std::int32_t>(shifted);
    return static_cast<std::int32_t>(signed_shifted >> 4);
}

// 一行可读的形状描述。**稳定**是硬要求：真机回传的日志要靠它人眼对齐。
inline std::string describe_draw_shape(std::uint64_t packed)
{
    if (packed == 0)
        return "none";
    const int kind_index = unpack_draw_shape_kind(packed);
    const char *name = draw_kind_name(kind_index);
    std::string text = name;
    text += " count=";
    text += std::to_string(unpack_draw_shape_count(packed));
    if (draw_kind_is_indexed(kind_index))
    {
        text += " start=";
        text += std::to_string(unpack_draw_shape_start(packed));
        text += " base=";
        text += std::to_string(unpack_draw_shape_base(packed));
    }
    else
    {
        text += " vstart=";
        text += std::to_string(unpack_draw_shape_start(packed));
    }
    return text;
}

// 打包后的形状槽（**零额外 COM 调用**；两个入口的实参直接够用）。
struct DrawShapeSlot
{
    std::uint64_t value = 0; // 0 = 未提供（调用点没有形状信息 ⇒ 只由 g_shape_total 记账）
    bool valid = false;

    DrawShapeSlot() = default;

    // 索引绘制：`DrawIndexed(ctx, IndexCount, StartIndexLocation, BaseVertexLocation)`
    static DrawShapeSlot indexed(std::uint32_t index_count, std::uint32_t start_index, std::int32_t base_vertex)
    {
        DrawShapeSlot slot;
        slot.value = pack_draw_shape(0, index_count, start_index, base_vertex);
        slot.valid = true;
        return slot;
    }

    // 非索引绘制：`Draw(ctx, VertexCount, StartVertexLocation)`
    static DrawShapeSlot non_indexed(std::uint32_t vertex_count, std::uint32_t start_vertex)
    {
        DrawShapeSlot slot;
        slot.value = pack_draw_shape(2, vertex_count, start_vertex, 0);
        slot.valid = true;
        return slot;
    }

    // 实例化变体（B107 预留：Genshin 实测可能一次都不走，表里为 0 即为实证）。
    static DrawShapeSlot indexed_instanced(std::uint32_t index_count, std::uint32_t instance_count, std::uint32_t start_index, std::int32_t base_vertex)
    {
        DrawShapeSlot slot;
        slot.value = pack_draw_shape(1, index_count, start_index, base_vertex);
        slot.instance_count = instance_count;
        slot.valid = true;
        return slot;
    }

    static DrawShapeSlot non_indexed_instanced(std::uint32_t vertex_count, std::uint32_t instance_count, std::uint32_t start_vertex)
    {
        DrawShapeSlot slot;
        slot.value = pack_draw_shape(3, vertex_count, start_vertex, 0);
        slot.instance_count = instance_count;
        slot.valid = true;
        return slot;
    }

    std::uint32_t instance_count = 0; // 仅实例化入口填；非实例化入口写 0
};

// ---- 元素数直方图（B108：形状表的**容量无关**补充）----
//
// 【为什么必须单独有它（B107 真机的教训）】
//   B107 的形状表是定长 128 槽、键永不清 ⇒ 真机第一秒就饱和
//   （`unique=128` 正是槽数上限）：之后**每一个新形状**（以及它的 `calls`/`hits`）
//   都落进 `other`，**目标形状根本不在表里** ⇒ `identified=0` 而 `identified_shapes`
//   却列着形状（那两列来自另一条路径）。⇒ "形状"这种**开放集合**不能只靠定长哈希表。
//
//   本直方图按**元素数**（索引绘制的 `IndexCount` / 非索引的 `VertexCount`）分桶，
//   **状态空间有限**（29 桶 × 2 组）× 每桶 2 个计数器 ⇒ **永不溢出、永不淘汰**，
//   且**桶里同时记 calls 与 hits** ⇒ 它能直接回答本轮最要紧的那个问题：
//
//     **"`count==3` 是不是目标绘制的必要条件？"**
//       - 若 `count==3` 桶的 `hits` ≈ 全部 `identified`，而其余桶 `hits` = 0
//         ⇒ **在样本内它是必要条件**，可拒掉的调用数 = 总调用 − `count==3` 的调用；
//       - 若别的桶也有 `hits` ⇒ **它不是必要条件**（硬造这个判据会缩小识别集合 ✗）。
//
// 【分桶】
//   0..16 精确（UI 精灵/全屏三角/四边形都落在这里，是判别力最强的一段）；
//   17 以上按 2 的幂分区间（17-31 / 32-63 / … / 32768+）—— 上界只为"粗看构成"。
//   ⚠️ 打包值里的元素数在 0xFFFF 处**饱和**（见 `pack_draw_shape`）⇒ 最后一桶
//   标成 `32768+` 而不是"32768-65535"，不假装知道饱和以上到底是多少。
//
// 【口径】与形状表一样：**非抽样**、跟 `PerfProbe` 总开关、不新增 ini 键、
//   不参与任何判定（纯计数）。每调用 1 次 relaxed 原子加。
struct DrawElementHistogram
{
    static constexpr std::size_t k_exact_max = 16;                                       // 0..16 精确
    static constexpr std::size_t k_range_count = 12;                                     // 12 个幂区间
    static constexpr std::size_t k_bucket_count = k_exact_max + 1 + k_range_count;       // 29
    static constexpr std::size_t k_group_count = 2;                                      // 0 = 索引, 1 = 非索引
    static constexpr std::size_t k_report_buckets = 16;                                  // 每组最多列 16 个桶

    std::atomic_uint64_t calls[k_group_count][k_bucket_count] {};
    std::atomic_uint64_t hits[k_group_count][k_bucket_count] {};
    std::atomic_uint64_t total_calls { 0 };
    std::atomic_uint64_t total_hits { 0 };

    // 元素数 → 桶下标（全函数域有定义，永不越界）。
    static std::size_t bucket_for(std::uint32_t element_count)
    {
        if (element_count <= k_exact_max)
            return element_count;
        std::size_t bucket = k_exact_max + 1;
        std::uint32_t upper = 31;
        while (bucket + 1 < k_bucket_count && element_count > upper)
        {
            ++bucket;
            upper = upper * 2 + 1;
        }
        return bucket;
    }

    // 桶的可读标签（**稳定**：真机日志靠它人眼对齐）。
    static const char *bucket_label(std::size_t bucket)
    {
        static const char *const labels[k_bucket_count] = {
            "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16",
            "17-31", "32-63", "64-127", "128-255", "256-511", "512-1023",
            "1024-2047", "2048-4095", "4096-8191", "8192-16383", "16384-32767", "32768+"
        };
        return bucket < k_bucket_count ? labels[bucket] : "?";
    }

    // 组：0 = 索引绘制（`ix`/`ixi`），1 = 非索引（`dr`/`dri`）。
    static std::size_t group_for_kind(int kind_index)
    {
        return draw_kind_is_indexed(kind_index) ? 0u : 1u;
    }

    static const char *group_label(std::size_t group)
    {
        return group == 0 ? "elem_ix" : "elem_dr";
    }

    void record_call(int kind_index, std::uint32_t element_count)
    {
        total_calls.fetch_add(1, std::memory_order_relaxed);
        calls[group_for_kind(kind_index)][bucket_for(element_count)].fetch_add(1, std::memory_order_relaxed);
    }

    void record_identified(int kind_index, std::uint32_t element_count)
    {
        total_hits.fetch_add(1, std::memory_order_relaxed);
        hits[group_for_kind(kind_index)][bucket_for(element_count)].fetch_add(1, std::memory_order_relaxed);
    }

    struct Snapshot
    {
        std::uint64_t calls[k_group_count][k_bucket_count] {};
        std::uint64_t hits[k_group_count][k_bucket_count] {};
        std::uint64_t total_calls = 0;
        std::uint64_t total_hits = 0;
    };

    Snapshot snapshot()
    {
        Snapshot snap;
        for (std::size_t group = 0; group < k_group_count; ++group)
        {
            for (std::size_t bucket = 0; bucket < k_bucket_count; ++bucket)
            {
                snap.calls[group][bucket] = calls[group][bucket].exchange(0, std::memory_order_relaxed);
                snap.hits[group][bucket] = hits[group][bucket].exchange(0, std::memory_order_relaxed);
            }
        }
        snap.total_calls = total_calls.exchange(0, std::memory_order_relaxed);
        snap.total_hits = total_hits.exchange(0, std::memory_order_relaxed);
        return snap;
    }
};

// ---- 帧来源（用于按帧归一化）----
enum class FrameSource : std::uint32_t
{
    upscale = 0,   // ffx12::dispatch（本桥的 upscale 调用 = 每游戏帧一次）
    present        // hooked_present
};

namespace detail
{

struct SegmentStat
{
    std::atomic_uint64_t calls { 0 };
    std::atomic_uint64_t total_ns { 0 };
    std::atomic_uint64_t max_ns { 0 };
};

struct CounterStat
{
    std::atomic_uint64_t count { 0 };
    std::atomic_uint64_t max_us { 0 };
};

constexpr std::size_t k_segment_count = static_cast<std::size_t>(Segment::count);
constexpr std::size_t k_counter_count = static_cast<std::size_t>(Counter::count);

inline const char *const k_segment_names[k_segment_count] = {
    "present", "il2cpp", "drawhook", "disphook", "jitter", "gpuq", "cfgio", "rscale",
    "ups", "prep", "signal", "w12", "submit", "finw", "finc"
};

inline const char *const k_counter_names[k_counter_count] = {
    "vprotect", "flushic", "sleep", "waitobj", "cfgread", "fileprobe"
};

constexpr std::size_t k_funnel_count = static_cast<std::size_t>(FunnelStage::count);

inline const char *const k_funnel_names[k_funnel_count] = {
    "entry", "after_om", "after_prescreen", "after_ps_query", "after_fast_path",
    "fast_hit", "after_cb", "after_viewport", "after_output",
    "fixed_ok", "dynamic_ok", "identified"
};

inline SegmentStat g_segments[k_segment_count];
inline CounterStat g_counters[k_counter_count];

// 漏斗各阶段（抽样计数）与内省调用总数（**非**抽样：用来算真实的次/帧）。
inline std::atomic_uint64_t g_funnel[k_funnel_count];
inline std::atomic_uint64_t g_funnel_calls { 0 };

// 本区间"识别成功"的**真实次数**（**非抽样**；漏斗行的 `identified` 是抽样口径，
// 要乘步长才能和它比）。用途：与形状层的 `identified` 交叉核对（B108 自洽断言）。
inline std::atomic_uint64_t g_funnel_identified_total { 0 };

// 视图读取次数（B106）：每读一个视图（`read_resource_info` 成功进入）记 1。
//
// 这是**工作量**指标，不是时间指标：它不受时钟粒度、负载、驱动实现影响
// ⇒ 是"优化真的少做了工作"的硬证据（重排便宜判据后它必须下降）。
// ⚠️ 只在内省路径的读取点显式计数（见 Dx11FsrBridge.cpp），不是全局所有读取。
inline std::atomic_uint64_t g_view_reads { 0 };

inline std::atomic_bool g_enabled { false };
inline std::atomic<std::uint32_t> g_interval_ms { 1000 };
inline std::atomic<std::uint32_t> g_draw_sample { 32 };
inline std::atomic<LogSink> g_sink { nullptr };

// 帧计数（两种来源分别计；归一化时优先用 upscale —— 它每游戏帧恰好一次，
// 而 Present 钩子在本机的 ffx12 直连模式下**根本没装**，见 hooked_present 的说明）。
inline std::atomic_uint64_t g_frames_upscale { 0 };
inline std::atomic_uint64_t g_frames_present { 0 };
// draw / dispatch 钩子的**总调用次数**（与抽样计数分开：抽样只测每 N 次）
inline std::atomic_uint64_t g_draw_calls { 0 };
inline std::atomic_uint64_t g_draw_sampled { 0 };
inline std::atomic_uint64_t g_dispatch_calls { 0 };
inline std::atomic_uint64_t g_dispatch_sampled { 0 };
inline std::atomic_uint64_t g_jitter_calls { 0 };
// 探针自身：Scope 的实例数（用来估算自身开销，见文件头）
inline std::atomic_uint64_t g_scope_calls { 0 };

inline std::atomic<std::uint64_t> g_interval_start_ticks { 0 };
inline std::atomic<std::uint64_t> g_last_check_ms { 0 };
inline std::atomic_bool g_flushing { false };

inline std::int64_t qpc_frequency()
{
    static const std::int64_t frequency = []()
        {
            LARGE_INTEGER value {};
            QueryPerformanceFrequency(&value);
            return value.QuadPart != 0 ? value.QuadPart : 1;
        }();
    return frequency;
}

inline std::int64_t qpc_ticks()
{
    LARGE_INTEGER value {};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

inline std::uint64_t ticks_to_ns(std::int64_t ticks)
{
    // ticks 可能为 0（未取到）—— 由调用方保证非负。
    return static_cast<std::uint64_t>(
        static_cast<double>(ticks) * 1.0e9 / static_cast<double>(qpc_frequency()));
}

inline void add_ns(Segment segment, std::uint64_t ns)
{
    SegmentStat &stat = g_segments[static_cast<std::size_t>(segment)];
    stat.calls.fetch_add(1, std::memory_order_relaxed);
    stat.total_ns.fetch_add(ns, std::memory_order_relaxed);
    // max 用"读到更小就写"的松散竞争实现：并发下可能**低估**极值，
    // 永不吹大 ⇒ 对"是否有尖刺"的判断是保守的（宁可漏报不误报）。
    std::uint64_t current = stat.max_ns.load(std::memory_order_relaxed);
    if (ns > current)
        stat.max_ns.store(ns, std::memory_order_relaxed);
}

inline double per_frame_us(std::uint64_t total_ns, std::uint64_t frames)
{
    if (frames == 0)
        return 0.0;
    return static_cast<double>(total_ns) / 1000.0 / static_cast<double>(frames);
}

// ---- 单次调用成本直方图（回答"均值是不是被少数巨贵调用带偏的"）----
//
// 【它回答什么问题】
//   `draw_hook` 段只给**均值**（抽样均值 × 实际次数）。均值有两个盲区：
//     ① 若每帧只有几次调用是微秒级、其余是纳秒级，均值会把账记到"每次调用"头上；
//     ② 若成本均匀分布在每一次调用上，均值才是"逐 draw 固定成本"。
//   两者对应的优化方向**完全相反**（前者要找那几次调用，后者才该削固定成本），
//   所以必须看**分布**，不能只看均值。
//
// 【为什么是指数桶】耗时天然跨数量级（50 ns ~ 500 µs）；线性桶要么分辨率不够、
//   要么桶太细。指数桶（2 的幂）用一次位宽计算定位，且"中位数落在哪个桶"
//   一眼可读 ⇒ 直接给出"绝大多数调用有多便宜"。
//
// 【开销】与 SampledScope 同源：只对**已抽样**的那一次调用记录，即 1/stride；
//   记录本身 = 1 次位宽计算 + 3 次 relaxed 原子加。
struct DrawCostHistogram
{
    // 调用方在**同一个**局部作用域里 record 两次（整体钩子 / 只含内省），
    // 两个下标即可拆出"非内省部分"。
    static constexpr std::size_t k_draw_cost_slots = 2;
    static constexpr std::size_t k_bucket_count = 11; // <1us,1-2,...,256-512,>=512 (us)

    std::atomic_uint64_t count { 0 };
    std::atomic_uint64_t total_ns { 0 };
    std::atomic_uint64_t max_ns { 0 };
    std::atomic_uint64_t buckets[k_bucket_count] {};

    // ns 的最高有效位（0..64）。不用 `__builtin_clzll`：那是 GCC/Clang 内建，
    // MSVC 不认（本仓库用 MSVC 构建）。
    static std::size_t bit_width(std::uint64_t value)
    {
        std::size_t width = 0;
        while (value != 0)
        {
            ++width;
            value >>= 1;
        }
        return width;
    }

    static std::size_t bucket_for(std::uint64_t ns)
    {
        if (ns < 1000)
            return 0;
        // 1000..2047 -> 1, 2048..4095 -> 2, ...
        // bit_width(1000)=10、bit_width(1024)=11 ⇒ 偏移 10 会让 1000..1023 落到 0，
        // 也就是与"<1us"合并 —— 这不是错，但会让 1-2us 的边界变成 1024 而不是 1000，
        // 读数时容易误判，所以显式把 1000..2047 全部归入 1。
        const std::size_t width = bit_width(ns);
        const std::size_t index = width <= 11 ? 1 : width - 10;
        return index < k_bucket_count ? index : k_bucket_count - 1;
    }

    void record(std::uint64_t ns)
    {
        count.fetch_add(1, std::memory_order_relaxed);
        total_ns.fetch_add(ns, std::memory_order_relaxed);
        std::uint64_t current = max_ns.load(std::memory_order_relaxed);
        if (ns > current)
            max_ns.store(ns, std::memory_order_relaxed);
        buckets[bucket_for(ns)].fetch_add(1, std::memory_order_relaxed);
    }

    void reset()
    {
        count.store(0, std::memory_order_relaxed);
        total_ns.store(0, std::memory_order_relaxed);
        max_ns.store(0, std::memory_order_relaxed);
        for (std::size_t i = 0; i < k_bucket_count; ++i)
            buckets[i].store(0, std::memory_order_relaxed);
    }

    // 形如 "n=120 mean_us=0.4 max_us=12.1 <1us:110 1-2:8 8-16:2"（只报非零桶）
    std::string line() const
    {
        static const char *const labels[k_bucket_count] = {
            "<1us", "1-2", "2-4", "4-8", "8-16", "16-32", "32-64", "64-128", "128-256", "256-512", ">=512"
        };
        const std::uint64_t total = count.load(std::memory_order_relaxed);
        std::string text = "n=" + std::to_string(total);
        if (total == 0)
            return text;
        const double mean_us = static_cast<double>(total_ns.load(std::memory_order_relaxed)) / 1000.0 /
            static_cast<double>(total);
        text += " mean_us=" + std::to_string(mean_us);
        text += " max_us=" + std::to_string(
            static_cast<double>(max_ns.load(std::memory_order_relaxed)) / 1000.0);
        for (std::size_t i = 0; i < k_bucket_count; ++i)
        {
            const std::uint64_t value = buckets[i].load(std::memory_order_relaxed);
            if (value != 0)
                text += " " + std::string(labels[i]) + ":" + std::to_string(value);
        }
        return text;
    }
};

// ---- 绘制形状分桶表（B107 建表 / **B108 改成"每区间 Top-N"**）----
//
// 【B108 为什么必须改（真机数据逼出来的）】
//   B107 的选型是"定长 128 槽开放寻址 + **键永不清**"，理由是"行不搬家、跨区间可比"。
//   真机（151 s）读数：`entries=125412 shaped=4478 other=120934 unique=128` ——
//   `unique` 恰好等于槽数上限 ⇒ **表第一秒就饱和**。后果有两个，都是致命的：
//     ① `other=120934`（96.4% 的调用）⇒ `shapes_top` 只覆盖 3.6% 的调用，
//        **看不到"哪个形状调用最多"**（B107 的目标没达成）；
//     ② **目标形状不在表里** ⇒ 它的 `hits` 全落 `other_hits`，而汇总行的
//        `identified` 只累加 表内 `hits[]` ⇒ 打印 `identified=0`，
//        可是同一份快照的 `identified_shapes` 行**明明列着目标形状**（那条路径
//        独立于表）⇒ 两个字段自相矛盾（这就是 B108 要修的 Bug ②：
//        **不是"标记没发生"，是"标记的计数进了一个没人读的口袋"**）。
//
// 【B108 的选型：每区间 Top-N + 有界淘汰 + **精确总量**】
//   - **每区间清键**（不再"键永不清"）：饱和不再是永久状态，每个区间重新分配槽位。
//     代价是失去"行下标跨区间不变"，但**报告本来就按调用数排序**（人眼比对靠
//     `ix count=3 start=0 base=0` 这样的**形状描述串**，不靠下标）⇒ 没有实际损失。
//   - **有界淘汰**：窗口（hash 起点起的 `k_probe_depth` 格）全满时，淘汰其中
//     "最不可能是目标"的一格 —— **hits==0 优先**，其次 **calls 最小**，平局取最小下标。
//     ⇒ 调用数最多的形状（尤其**识别成功过**的形状）**不会被一次性形状挤掉**，
//       而一次性形状彼此轮换 ⇒ 这就是"按调用数保留 Top-N"的最小堆语义，
//       且**零堆分配、零锁、每次未命中只扫一个有界窗口**（8 格）。
//   - **总量精确、永不丢**：`total_calls` / `total_hits` 是两个独立原子，每次调用/命中
//     各 +1；被淘汰形状已计入的量折进 `cold_*`；认领失败（CAS 竞争）落 `other_*`。
//     ⇒ 恒等式（flush 时逐条核对，不成立就打 mismatch 行）：
//         `total_calls == sum(calls[]) + cold_calls + other_calls`
//         `total_hits  == sum(hits[])  + cold_hits  + other_hits`
//   - **`identified` 的口径 = 上面那个精确的 `total_hits`**（含淘汰/溢出的部分）
//     ⇒ 它与**漏斗层**的非抽样 `identified_total` 必须逐区间相等（自洽断言）。
//
// 【为什么每桶 2 个原子】`calls` 与 `hits` 是**同一个桶的两列**；
//   两列分开写不会互相阻塞，且读侧（每区间一次）逐个 exchange 即可。
//
// 【识别成功的形状表】（`DrawShapeMark`）：在识别成功的 return 之前单独记一笔
//   "本区间被识别为真目标的形状"（去重、有界）⇒ 才看得到"目标调用的形状分布"。
//   放在表定义之前（`snapshot()` 要读它）。
struct DrawShapeTable;

struct DrawShapeMark
{
    // 一个区间内**不同目标形状**个数的上界（真机每帧就 1 个目标 ⇒ 24 绰绰有余）。
    static constexpr std::size_t k_observed_capacity = 24;

    std::atomic_uint64_t observed_keys[k_observed_capacity] {};
    std::atomic_uint64_t observed_calls[k_observed_capacity] {};

    // 读空 observed 列表（快照时调用一次）：**键与计数一起清** ⇒ 每个区间的
    // "被识别为目标的形状"是**本区间各自一份**（不是"曾经出现过"）。
    // 竞态说明：本函数与 `note_identified` 都在渲染线程调用（快照来自 flush，
    // 而 flush 由钩子入口的 maybe_flush 触发）⇒ 顺序一致，不需要 CAS 循环；
    // 这里用 exchange/store 只是为了让"读走"这件事在机器码层面显式。
    std::size_t collect_observed(std::uint64_t *out_keys, std::uint64_t *out_calls, std::size_t capacity)
    {
        std::size_t count = 0;
        for (std::size_t i = 0; i < k_observed_capacity && count < capacity; ++i)
        {
            const std::uint64_t key = observed_keys[i].exchange(0, std::memory_order_relaxed);
            const std::uint64_t call_count = observed_calls[i].exchange(0, std::memory_order_relaxed);
            if (key == 0)
                continue;
            out_keys[count] = key;
            out_calls[count] = call_count;
            ++count;
        }
        return count;
    }

    void note_identified(std::uint64_t packed)
    {
        if (packed == 0)
            return;
        for (std::size_t i = 0; i < k_observed_capacity; ++i)
        {
            if (observed_keys[i].load(std::memory_order_relaxed) == packed)
            {
                observed_calls[i].fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        for (std::size_t i = 0; i < k_observed_capacity; ++i)
        {
            std::uint64_t expected = 0;
            if (observed_keys[i].compare_exchange_strong(expected, packed, std::memory_order_relaxed))
            {
                observed_calls[i].fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    }
};

inline DrawShapeMark g_draw_shape_mark;

struct DrawShapeTable
{
    static constexpr std::size_t k_bucket_count = 128; // 形状种类实测远超 128 ⇒ 靠淘汰保 Top-N
    static constexpr std::size_t k_probe_depth = 8;    // 探测/淘汰窗口（有界，最坏 8 次比较）
    static constexpr std::size_t k_report_slots = 24;  // 汇总行里最多列 24 条形状
    static constexpr std::uint64_t k_empty = 0;        // 0 是保留的"空"标记（打包值恒非 0）

    std::atomic_uint64_t keys[k_bucket_count] {};
    std::atomic_uint64_t calls[k_bucket_count] {};
    std::atomic_uint64_t hits[k_bucket_count] {};
    // 认领失败（有界重试内仍是 CAS 竞争失败）⇒ 落这里。**总调用数永不丢**。
    std::atomic_uint64_t other_calls { 0 };
    std::atomic_uint64_t other_hits { 0 };
    // 被淘汰形状**已经计入**的调用/命中（折账进 cold_*，否则总数会凭空消失）。
    std::atomic_uint64_t cold_calls { 0 };
    std::atomic_uint64_t cold_hits { 0 };
    std::atomic_uint64_t evictions { 0 };
    // 精确总量（非抽样；每次调用/命中各 +1）⇒ 报告里的 `identified` 用它，不用桶之和。
    std::atomic_uint64_t total_calls { 0 };
    std::atomic_uint64_t total_hits { 0 };

    // 一次性快照（只在 flush 时构造一次；把复位与读取合成一次遍历）。
    struct Snapshot
    {
        static constexpr std::size_t k_observed_max = 24;

        std::uint64_t total = 0;             // 本区间形状调用总数（精确）
        std::uint64_t bucketed = 0;          // 常驻形状的调用数之和（= shapes_top 覆盖的部分）
        std::uint64_t cold = 0;              // 被淘汰形状的调用数之和
        std::uint64_t other_calls = 0;
        std::uint64_t unique = 0;            // 快照时刻常驻的形状数（≤ k_bucket_count）
        std::uint64_t evictions = 0;
        std::uint64_t identified = 0;        // 本区间识别成功总次数（精确，含 cold/other）
        std::uint64_t identified_bucketed = 0;
        std::uint64_t identified_cold = 0;   // > 0 ⇒ 有目标形状没进表（白名单会漏）
        std::uint64_t identified_other = 0;
        std::uint32_t bucket_count = 0;
        std::uint64_t keys[k_bucket_count] {};
        std::uint64_t calls[k_bucket_count] {};
        std::uint64_t hits[k_bucket_count] {};
        // 本区间内**被识别为真目标**的形状（去重，最多 k_observed_max 个）。
        std::size_t observed_count = 0;
        std::uint64_t observed_keys[k_observed_max] {};
        std::uint64_t observed_calls[k_observed_max] {};

        // B108 两条守恒恒等式（flush 的自洽断言与单测用**同一个谓词** ⇒ 期望值不会写歪）。
        bool calls_conserved() const
        {
            return total == bucketed + cold + other_calls;
        }

        bool identifications_conserved() const
        {
            return identified == identified_bucketed + identified_cold + identified_other;
        }
    };

    static std::size_t bucket_for(std::uint64_t packed)
    {
        return static_cast<std::size_t>(packed % k_bucket_count);
    }

    // 查找已认领的桶下标；未认领返回 k_bucket_count（只读，不认领）。
    std::size_t find_slot(std::uint64_t packed) const
    {
        const std::size_t start = bucket_for(packed);
        for (std::size_t probe = 0; probe < k_probe_depth; ++probe)
        {
            const std::size_t index = (start + probe) % k_bucket_count;
            if (keys[index].load(std::memory_order_relaxed) == packed)
                return index;
        }
        return k_bucket_count;
    }

    std::size_t claim_slot(std::uint64_t packed)
    {
        const std::size_t start = bucket_for(packed);
        // 有界重试：同一线程下第一次就成；跨线程竞争时多给几次机会，失败则落 other。
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            // ① 一趟走完"找已认领"与"找空位"（各走一趟会多一倍 relaxed 读，而本探针
            //    就在被测量的热路径上 ⇒ 表满时这 8 次读是常态成本）。
            std::size_t free_index = k_bucket_count;
            for (std::size_t probe = 0; probe < k_probe_depth; ++probe)
            {
                const std::size_t index = (start + probe) % k_bucket_count;
                const std::uint64_t key = keys[index].load(std::memory_order_relaxed);
                if (key == packed)
                    return index; // 已认领 ⇒ 直接用（同区间内形状不会搬家）
                if (key == k_empty && free_index == k_bucket_count)
                    free_index = index;
            }
            // ② 有空位 ⇒ CAS 认领（新形状的第一笔）
            //    ⚠️ 先判空再 CAS：表满时（真机常态）直接 CAS 会打 8 次注定失败的
            //    locked cmpxchg —— 那比 relaxed 读贵得多（自证数字见 `shape_mark_on_miss`）。
            if (free_index != k_bucket_count)
            {
                std::uint64_t expected = k_empty;
                if (keys[free_index].compare_exchange_strong(expected, packed, std::memory_order_relaxed))
                {
                    calls[free_index].store(0, std::memory_order_relaxed);
                    hits[free_index].store(0, std::memory_order_relaxed);
                    return free_index;
                }
                continue;
            }
            // ③ 窗口满 ⇒ 淘汰"最不可能是目标"的一格
            const std::size_t victim = choose_victim(start);
            const std::uint64_t old_key = keys[victim].load(std::memory_order_relaxed);
            if (old_key == k_empty || old_key == packed)
                continue;
            std::uint64_t expected = old_key;
            if (!keys[victim].compare_exchange_strong(expected, packed, std::memory_order_relaxed))
                continue;
            // 折账：被淘汰形状已经计入的调用/命中必须落到 cold_*（守恒，见文件头）
            const std::uint64_t old_calls = calls[victim].load(std::memory_order_relaxed);
            const std::uint64_t old_hits = hits[victim].load(std::memory_order_relaxed);
            if (old_calls != 0)
                cold_calls.fetch_add(old_calls, std::memory_order_relaxed);
            if (old_hits != 0)
                cold_hits.fetch_add(old_hits, std::memory_order_relaxed);
            calls[victim].store(0, std::memory_order_relaxed);
            hits[victim].store(0, std::memory_order_relaxed);
            evictions.fetch_add(1, std::memory_order_relaxed);
            return victim;
        }
        return k_bucket_count; // 重试仍失败 ⇒ 调用方记到 other（正常路径恒不发生）
    }

    // 淘汰候选：窗口内 **hits==0 优先**，其次 **calls 最小**，平局取最小下标（确定性）。
    // 目的：识别成功过的形状（目标）与高频形状绝不因"来了一次性形状"而消失。
    std::size_t choose_victim(std::size_t start) const
    {
        std::size_t victim = start;
        std::uint64_t victim_calls = 0;
        bool victim_has_hits = false;
        for (std::size_t probe = 0; probe < k_probe_depth; ++probe)
        {
            const std::size_t index = (start + probe) % k_bucket_count;
            const std::uint64_t call_count = calls[index].load(std::memory_order_relaxed);
            const bool has_hits = hits[index].load(std::memory_order_relaxed) != 0;
            if (probe == 0)
            {
                victim = index;
                victim_calls = call_count;
                victim_has_hits = has_hits;
                continue;
            }
            if (victim_has_hits != has_hits)
            {
                if (victim_has_hits)
                {
                    victim = index;
                    victim_calls = call_count;
                    victim_has_hits = has_hits;
                }
                continue;
            }
            if (call_count < victim_calls)
            {
                victim = index;
                victim_calls = call_count;
                victim_has_hits = has_hits;
            }
        }
        return victim;
    }

    // 一次内省调用（**非抽样**：调用次数本身就是被问的问题）。
    // `packed == 0` ⇒ 调用点没有形状信息，只记总数（不落任何形状桶）。
    void record_call(std::uint64_t packed)
    {
        if (packed == k_empty)
            return;
        total_calls.fetch_add(1, std::memory_order_relaxed);
        const std::size_t index = claim_slot(packed);
        if (index == k_bucket_count)
        {
            other_calls.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        calls[index].fetch_add(1, std::memory_order_relaxed);
    }

    // 该形状被**识别为真目标**（在识别成功的 return 之前调用）。
    // ⚠️ B108：即使该形状已被淘汰，这里也会**重新认领**一格（淘汰的是冷形状）
    // ⇒ "识别成功过的形状"在同一区间内必定可见（真机 Bug ② 的直接对策）。
    void record_identified(std::uint64_t packed)
    {
        if (packed == k_empty)
            return;
        total_hits.fetch_add(1, std::memory_order_relaxed);
        const std::size_t index = claim_slot(packed);
        if (index == k_bucket_count)
        {
            other_hits.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        hits[index].fetch_add(1, std::memory_order_relaxed);
    }

    Snapshot snapshot()
    {
        Snapshot snap;
        snap.bucket_count = static_cast<std::uint32_t>(k_bucket_count);
        // 读空 observed 列表（识别成功过的形状），并复位它 ⇒ 每个区间各自给一份。
        snap.observed_count = g_draw_shape_mark.collect_observed(
            snap.observed_keys, snap.observed_calls, Snapshot::k_observed_max);
        // ⚠️ B108：键**也清**（每区间一份 Top-N；见文件头"每区间清键"）。
        for (std::size_t i = 0; i < k_bucket_count; ++i)
        {
            snap.keys[i] = keys[i].exchange(k_empty, std::memory_order_relaxed);
            if (snap.keys[i] != k_empty)
                ++snap.unique;
        }
        for (std::size_t i = 0; i < k_bucket_count; ++i)
        {
            const std::uint64_t value = calls[i].exchange(0, std::memory_order_relaxed);
            snap.calls[i] = value;
            snap.bucketed += value;
        }
        for (std::size_t i = 0; i < k_bucket_count; ++i)
        {
            const std::uint64_t value = hits[i].exchange(0, std::memory_order_relaxed);
            snap.hits[i] = value;
            snap.identified_bucketed += value;
        }
        snap.other_calls = other_calls.exchange(0, std::memory_order_relaxed);
        snap.identified_other = other_hits.exchange(0, std::memory_order_relaxed);
        snap.cold = cold_calls.exchange(0, std::memory_order_relaxed);
        snap.identified_cold = cold_hits.exchange(0, std::memory_order_relaxed);
        snap.evictions = evictions.exchange(0, std::memory_order_relaxed);
        // 精确总量（**这才是 `identified` 的口径**；桶之和只是它的可见部分）
        snap.total = total_calls.exchange(0, std::memory_order_relaxed);
        snap.identified = total_hits.exchange(0, std::memory_order_relaxed);
        return snap;
    }
};

inline DrawShapeTable g_draw_shape_table;

// 元素数直方图（B108）：**容量无关**的构成统计（见 `DrawElementHistogram` 的说明）。
inline DrawElementHistogram g_draw_element_histogram;

// 本区间进入**内省**的调用总数（**非抽样**；含没有形状信息的调用点）。
// 与 `perf_probe` 行的 `draws=` 对账：差额 = 走了钩子但没进内省的绘制。
inline std::atomic_uint64_t g_shape_entries { 0 };

// 本区间 `mark_draw_shape_identified` 的**调用次数**（非抽样）。
// 用途：自洽断言的第二只手 —— 形状表聚合出的 `identified` 必须等于它，
// 它又必须等于漏斗层的 `g_funnel_identified_total`（两者分别埋在不同层，
// 一旦有人把某一处挪到走不到的分支上，flush 就会打出 mismatch 行）。
inline std::atomic_uint64_t g_shape_mark_total { 0 };

// 识别成功时用的形状槽：由调用方（`hooked_draw_*`）在钩子入口算好，一路带进内省函数。
// ⚠️ 这是**函数的形参**，不是全局缓存 ⇒ 没有"跨 draw 的状态缓存"那一类失效模式。

// 下标 0 = 钩子整体（**由 SampledScope 在析构时落桶**）；下标 1 = 其中"内省"
// （`inspect_target_upscaler_draw*`）那一段（由 DrawInspectScope 的窗口计时落桶）。
inline DrawCostHistogram g_draw_cost[DrawCostHistogram::k_draw_cost_slots];

// 本区间内**真正进入内省**的抽样次数（slot 1 里"非 0 样本"的个数）。
// 用途：DrawInspectScope 会把"没进入内省"的 draw 记为 0 ⇒ 只看 slot 1 的
// `<1us` 桶无法区分"内省很快"与"根本没内省"；这个数把两者分开。
inline std::atomic_uint64_t g_draw_inspect_entered { 0 };

} // namespace detail

// ---- 形状分桶的读数（B107 建 / B108 修）----
//
// 【输出形式】四条以行为单位的记录（`sink` 逐行调用），**只在真的收到过内省调用时**输出：
//   ① 计数对账行：
//      `perf_probe_draw_shape_draw entries=… shaped=… bucketed=… cold=… other=…
//         identified=… identified_cold=… unique=… capacity=… evict=… shadow_reject=… |`
//      - `shaped=` = 本区间**带形状信息**的调用数（精确，非抽样）；
//      - `bucketed=` = 其中落在**常驻（可见）形状**上的调用数（= ②覆盖的部分）；
//        `cold=` = 被淘汰形状上的调用数；`other=` = 认领失败的调用数（正常恒 0）。
//        ⇒ **恒等式**：`shaped = bucketed + cold + other`（不成立就是探针自己坏了）。
//      - `identified=` = 本区间识别成功**总次数**（精确）。B107 的 bug 就是这里
//        只累加了表内 `hits[]`，而目标形状早已被挤出表 ⇒ 打成 0 ✗。现在它 = 表内
//        `hits` + 被淘汰形状的 hits + `other` —— 并且与**漏斗层**的
//        `identified_total=` 逐区间核对（不一致就多打一行 `perf_probe_draw_shape_mismatch`）。
//      - `identified_cold=` > 0 ⇒ **有目标形状没进表**（白名单会漏掉它，值得看一眼）；
//      - `unique=` = 快照时刻常驻形状数（`capacity=128`）；`evict=` = 本区间淘汰次数
//        （> 0 说明形状种类确实远超容量 ⇒ 表在**按调用数**做 Top-N 取舍）；
//      - **`shadow_reject=` 是"入口级判据的收益上限"**：把本区间**被识别为目标的形状**
//        当作白名单，其余形状（`hits==0`）的调用数之和。它**只统计、不参与任何判定**
//        ⇒ 读数与识别集合都逐位不变（这是允许"顺手算出判据收益"的唯一形式：
//        判据本身**没有**被实现，只把"实现它值多少"算出来）。
//        若本区间 `identified=0`（没有目标 ⇒ 白名单为空），该值记 0。
//        ⚠️ 只有当 `identified_cold=0 && other=0` 时它才是**上限**；否则白名单本身在漏。
//   ② `… | shapes_top <形状>:calls/hits …` —— 本区间**调用数最多的形状**（Top-N，
//      调用数降序，最多 24 条）。名字里的 `_top` 是**截断声明**：`+N more` 出现时，
//      后面的形状没被列出（`unique=` 给出常驻形状数）——**不假装列全了**。
//      `hits` = 该形状里被识别为真目标的次数 ⇒ 这一行直接回答"目标调用是不是总是某个形状"。
//   ③ `… | identified_shapes <形状>:calls` —— **本区间被识别为目标的形状**（去重）。
//      与②交叉读：凡③里出现过的形状都**不能**被入口级判据淘汰（会缩小识别集合）。
//      `… +N more` 表示超出了上报上限（有界，不刷屏）。
//   ④ `… | elem_ix <元素数>:calls/hits … | elem_dr … | count3 calls=… hits=…
//        reject=… outside_hits=…` —— **元素数直方图**（B108，容量无关 ⇒ 永不饱和）。
//      - 桶：0..16 精确、17 以上按 2 的幂（最后一个是 `32768+`）；
//      - **带 hits** ⇒ 直接读出"`count==3` 是不是必要条件"：
//        `count3 hits` ≈ `identified` 且 `outside_hits=0` ⇒ 样本内它是必要条件，
//        `reject=`（= 总调用 − `count==3` 的调用）就是该判据能拒掉的调用数；
//        若 `outside_hits>0` ⇒ **不是**必要条件（硬造该判据会缩小识别集合 ✗）。
//
// 【四行都只在"本区间真的进过内省"时输出】⇒ 探针开着但这一秒没 draw 时不刷屏。
inline void format_draw_shape_report(const detail::DrawShapeTable::Snapshot &snap,
                                     const DrawElementHistogram::Snapshot &hist,
                                     std::uint64_t entry_count,
                                     std::string &out)
{
    const std::uint64_t shaped_total = snap.total;
    // 影子评估（**不改变任何判定**）：白名单 = 本区间被识别过的形状（`hits != 0`）。
    // 只有当本区间真的识别到过目标时，"拒绝其余形状"这句话才有定义。
    std::uint64_t shadow_reject = 0;
    if (snap.identified != 0)
    {
        for (std::size_t i = 0; i < snap.bucket_count; ++i)
        {
            if (snap.keys[i] == detail::DrawShapeTable::k_empty || snap.hits[i] != 0)
                continue;
            shadow_reject += snap.calls[i];
        }
        // 非常驻形状（被淘汰 / 认领失败）的调用同样落在白名单之外，
        // ⇒ 上限 = 非命中常驻形状的调用 + cold + other（= shaped − 命中形状的调用）。
        shadow_reject += snap.cold + snap.other_calls;
    }
    char header[768] {};
    std::snprintf(header, sizeof(header),
        "perf_probe_draw_shape_draw entries=%llu shaped=%llu bucketed=%llu cold=%llu other=%llu"
        " identified=%llu identified_cold=%llu unique=%llu capacity=%llu evict=%llu shadow_reject=%llu |",
        static_cast<unsigned long long>(entry_count),
        static_cast<unsigned long long>(shaped_total),
        static_cast<unsigned long long>(snap.bucketed),
        static_cast<unsigned long long>(snap.cold),
        static_cast<unsigned long long>(snap.other_calls),
        static_cast<unsigned long long>(snap.identified),
        static_cast<unsigned long long>(snap.identified_cold),
        static_cast<unsigned long long>(snap.unique),
        static_cast<unsigned long long>(snap.bucket_count),
        static_cast<unsigned long long>(snap.evictions),
        static_cast<unsigned long long>(shadow_reject));
    out += header;
    out += "\n";

    // 全部常驻形状按"调用数降序"（并列时按识别数降序，仍然稳定 ⇒ 时序可比）。
    std::vector<std::size_t> order;
    order.reserve(snap.bucket_count);
    for (std::size_t i = 0; i < snap.bucket_count; ++i)
    {
        if (snap.keys[i] != detail::DrawShapeTable::k_empty && (snap.calls[i] != 0 || snap.hits[i] != 0))
            order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [&snap](std::size_t a, std::size_t b)
    {
        if (snap.calls[a] != snap.calls[b])
            return snap.calls[a] > snap.calls[b];
        return snap.hits[a] > snap.hits[b];
    });

    std::string shapes = "perf_probe_draw_shape_draw | shapes_top";
    const std::size_t listed = order.size() < detail::DrawShapeTable::k_report_slots
        ? order.size() : detail::DrawShapeTable::k_report_slots;
    for (std::size_t position = 0; position < listed; ++position)
    {
        const std::size_t index = order[position];
        shapes += " ";
        shapes += describe_draw_shape(snap.keys[index]);
        shapes += ":";
        shapes += std::to_string(snap.calls[index]);
        shapes += "/";
        shapes += std::to_string(snap.hits[index]);
    }
    if (order.size() > listed)
        shapes += " +" + std::to_string(order.size() - listed) + " more";
    out += shapes;
    out += "\n";

    // 本区间**被识别为真目标**的形状（去重；与 calls/hits 交叉读的就是这一份）。
    std::string identified = "perf_probe_draw_shape_draw | identified_shapes";
    for (std::size_t i = 0; i < snap.observed_count; ++i)
    {
        identified += " ";
        identified += describe_draw_shape(snap.observed_keys[i]);
        identified += ":";
        identified += std::to_string(snap.observed_calls[i]);
    }
    if (snap.observed_count == 0)
        identified += " none";
    out += identified;
    out += "\n";

    // ④ 元素数直方图（B108）：**容量无关** ⇒ 即使形状表被淘汰/饱和，这一行也永远完整。
    std::string elements = "perf_probe_draw_shape_draw |";
    for (std::size_t group = 0; group < DrawElementHistogram::k_group_count; ++group)
    {
        elements += " ";
        elements += DrawElementHistogram::group_label(group);
        std::size_t printed = 0;
        std::size_t non_zero = 0;
        for (std::size_t bucket = 0; bucket < DrawElementHistogram::k_bucket_count; ++bucket)
            if (hist.calls[group][bucket] != 0 || hist.hits[group][bucket] != 0)
                ++non_zero;
        for (std::size_t bucket = 0; bucket < DrawElementHistogram::k_bucket_count; ++bucket)
        {
            if (hist.calls[group][bucket] == 0 && hist.hits[group][bucket] == 0)
                continue;
            if (printed == DrawElementHistogram::k_report_buckets)
                break;
            elements += " ";
            elements += DrawElementHistogram::bucket_label(bucket);
            elements += ":";
            elements += std::to_string(hist.calls[group][bucket]);
            elements += "/";
            elements += std::to_string(hist.hits[group][bucket]);
            ++printed;
        }
        if (printed == 0)
            elements += " none";
        if (non_zero > printed)
            elements += " +" + std::to_string(non_zero - printed) + " more";
    }
    // `count==3` 的影子评估（**只统计**）：该判据会拒掉多少调用、会不会漏目标。
    const std::size_t count3_bucket = DrawElementHistogram::bucket_for(3);
    std::uint64_t count3_calls = 0;
    std::uint64_t count3_hits = 0;
    for (std::size_t group = 0; group < DrawElementHistogram::k_group_count; ++group)
    {
        count3_calls += hist.calls[group][count3_bucket];
        count3_hits += hist.hits[group][count3_bucket];
    }
    const std::uint64_t outside_hits = hist.total_hits > count3_hits ? hist.total_hits - count3_hits : 0;
    const std::uint64_t count3_reject = hist.total_calls > count3_calls ? hist.total_calls - count3_calls : 0;
    elements += " | count3 calls=" + std::to_string(count3_calls);
    elements += " hits=" + std::to_string(count3_hits);
    elements += " reject=" + std::to_string(count3_reject);
    elements += " outside_hits=" + std::to_string(outside_hits);
    out += elements;
    out += "\n";
}

// 自洽断言（B108）：三个"识别成功次数"必须相等。它们分别由**不同层**的埋点产生
//   A = 形状表聚合（`snapshot.identified`，含淘汰/溢出折账）
//   B = `mark_draw_shape_identified` 的调用次数（形状层埋点）
//   C = 漏斗层 `note_identified_total()` 的调用次数
// 任何一处被挪到"走不到的分支"、或聚合时漏了一类计数（B107 的真机 Bug ②），
// 这里就会不等 ⇒ 必须打一行告警（而不是安静地给出一个错的分布）。
inline bool draw_shape_consistency_ok(const detail::DrawShapeTable::Snapshot &snap,
                                      std::uint64_t shape_marks,
                                      std::uint64_t funnel_identified)
{
    const bool identified_consistent = snap.identified == shape_marks && shape_marks == funnel_identified;
    return identified_consistent && snap.calls_conserved() && snap.identifications_conserved();
}

// 断言失败时的那一行（只在失败时输出 ⇒ 不刷屏）。
inline std::string format_draw_shape_mismatch(const detail::DrawShapeTable::Snapshot &snap,
                                             std::uint64_t shape_marks,
                                             std::uint64_t funnel_identified)
{
    char line[512] {};
    std::snprintf(line, sizeof(line),
        "perf_probe_draw_shape_mismatch shape_identified=%llu shape_marks=%llu funnel_identified=%llu"
        " calls=%llu bucketed=%llu cold=%llu other_calls=%llu"
        " hits_in_table=%llu hits_cold=%llu hits_other=%llu",
        static_cast<unsigned long long>(snap.identified),
        static_cast<unsigned long long>(shape_marks),
        static_cast<unsigned long long>(funnel_identified),
        static_cast<unsigned long long>(snap.total),
        static_cast<unsigned long long>(snap.bucketed),
        static_cast<unsigned long long>(snap.cold),
        static_cast<unsigned long long>(snap.other_calls),
        static_cast<unsigned long long>(snap.identified_bucketed),
        static_cast<unsigned long long>(snap.identified_cold),
        static_cast<unsigned long long>(snap.identified_other));
    return std::string(line);
}

// 取得（并复位）形状表快照 / 元素数直方图快照（单测与 flush_now 共用）。
inline void snapshot_draw_shapes(detail::DrawShapeTable::Snapshot &out_snapshot)
{
    out_snapshot = detail::g_draw_shape_table.snapshot();
}

inline void snapshot_draw_element_histogram(DrawElementHistogram::Snapshot &out_snapshot)
{
    out_snapshot = detail::g_draw_element_histogram.snapshot();
}

// ---- 配置 ----
inline void set_log_sink(LogSink sink)
{
    detail::g_sink.store(sink, std::memory_order_release);
}

inline void configure(bool enabled, std::uint32_t interval_ms, std::uint32_t draw_sample_stride)
{
    detail::g_interval_ms.store(interval_ms != 0 ? interval_ms : 1000, std::memory_order_relaxed);
    detail::g_draw_sample.store(draw_sample_stride != 0 ? draw_sample_stride : 1, std::memory_order_relaxed);
    detail::g_interval_start_ticks.store(static_cast<std::uint64_t>(detail::qpc_ticks()),
                                         std::memory_order_relaxed);
    detail::g_last_check_ms.store(static_cast<std::uint64_t>(GetTickCount64()), std::memory_order_relaxed);
    detail::g_enabled.store(enabled, std::memory_order_release);
}

inline bool enabled()
{
    return detail::g_enabled.load(std::memory_order_acquire);
}

inline std::uint32_t draw_sample_stride()
{
    return detail::g_draw_sample.load(std::memory_order_relaxed);
}

// ---- 计时作用域（每帧级段用这个）----
class Scope
{
public:
    explicit Scope(Segment segment)
        : m_segment(segment)
    {
        if (!detail::g_enabled.load(std::memory_order_relaxed))
            return;
        m_start = detail::qpc_ticks();
        detail::g_scope_calls.fetch_add(1, std::memory_order_relaxed);
    }

    ~Scope()
    {
        if (m_start == 0)
            return;
        const std::int64_t now = detail::qpc_ticks();
        if (now > m_start)
            detail::add_ns(m_segment, detail::ticks_to_ns(now - m_start));
    }

    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

private:
    Segment m_segment;
    std::int64_t m_start = 0;
};

// ---- 抽样计时作用域（draw / dispatch 钩子专用：绝不逐次计时）----
//
// 每次构造都更新**总调用计数**（一个 relaxed 加），只有每 `stride` 次才真的取时间。
// ⇒ 每一帧的 draw 次数可见，而计时开销降到 1/stride。
class SampledScope
{
public:
    // "不落直方图"的哨兵槽位。
    static constexpr std::size_t k_no_histogram = static_cast<std::size_t>(-1);

    // `histogram_slot` 非哨兵时，**析构**里把本次实测耗时记进该直方图槽。
    //
    // ⚠️ 2026-09-27（B105 修复 `draw_all` 恒 0）：落桶**必须发生在析构**。
    // 旧写法由调用方在函数体内写 `histogram.record(scope.elapsed_ns())`，而
    // `elapsed_ns()` 读的是 `m_elapsed_ns`，它**只在析构时**被填上 ⇒ 每次记的都是 0
    // （真机 `draw_all n=685 mean_us=0.000000 <1us:685`）。
    // 把落桶搬进来之后：**任何** return 路径、以及将来新增的 return 路径都不可能漏记。
    SampledScope(Segment segment, std::atomic_uint64_t &call_counter,
                 std::atomic_uint64_t &sample_counter, std::uint32_t stride,
                 std::size_t histogram_slot = k_no_histogram)
        : m_segment(segment)
        , m_histogram_slot(histogram_slot)
    {
        if (!detail::g_enabled.load(std::memory_order_relaxed))
            return;
        const std::uint64_t index = call_counter.fetch_add(1, std::memory_order_relaxed);
        const std::uint32_t step = stride != 0 ? stride : 1;
        if ((index % step) != 0)
            return;
        sample_counter.fetch_add(1, std::memory_order_relaxed);
        m_start = detail::qpc_ticks();
        detail::g_scope_calls.fetch_add(1, std::memory_order_relaxed);
    }

    ~SampledScope()
    {
        if (m_start == 0)
            return;
        const std::int64_t now = detail::qpc_ticks();
        if (now > m_start)
        {
            m_elapsed_ns = detail::ticks_to_ns(now - m_start);
            // 段累计口径与旧版**逐字不变**：只在真的跨过 tick 时才 add_ns。
            detail::add_ns(m_segment, m_elapsed_ns);
        }
        // 直方图则**每个已抽样的调用都记一次**（含耗时取整为 0 的情况），
        // 这样槽的样本数恒等于 `draw_sampled`，与 `per_frame_us drawhook=` 同源。
        if (m_histogram_slot != k_no_histogram)
            detail::g_draw_cost[m_histogram_slot].record(m_elapsed_ns);
    }

    SampledScope(const SampledScope &) = delete;
    SampledScope &operator=(const SampledScope &) = delete;

    // 本次调用是否真的在计时；调用方据此决定是否把同一份耗时投进直方图。
    // ⚠️ **不要**在 SampledScope 之后再读 draw_call_counter 自己重算：
    // 构造时已经 fetch_add 过同一个计数器，重算会偏移 1 ⇒ 直方图样本集合
    // 与这里的计时样本集合不一致，"均值"就对不上 `per_frame_us drawhook=`。
    bool sampled() const
    {
        return m_start != 0;
    }

    // 已计时的耗时（纳秒）；未计时返回 0。
    //
    // ⚠️ **只在析构之后有效** —— 析构时才会填 `m_elapsed_ns`。在作用域内读它
    // 恒得 0（B104 的 `draw_all` 恒 0 就是这个原因）。B105 起槽 0 的落桶已由
    // 本类自己承担，新代码**不应**再需要这个接口；它留给单测与诊断读取。
    std::uint64_t elapsed_ns() const
    {
        return m_elapsed_ns;
    }

private:
    Segment m_segment;
    std::int64_t m_start = 0;
    std::uint64_t m_elapsed_ns = 0;
    std::size_t m_histogram_slot = k_no_histogram;
};

// ---- 视图读取计数（B106）----
//
// 【它回答什么问题】"优化到底少做了多少工作"：时间读数会被时钟粒度、负载、驱动实现
//   影响，而"读了多少个视图"是一个**确定性**的工作量 ⇒ 只要便宜判据真的提前退出，
//   这个数就必须降低，且降低量可以逐条对上（省 7 个 SRV / 省 2 个 RTV / 省 1 个 RTV）。
//
// 【口径】只在内省路径（`inspect_target_upscaler_draw_on_demand` 与正缓存快路径）的
//   读视图调用点显式计数 —— **不是**全局所有 `read_resource_info`。
//   探针关时：一次 relaxed 读 + 可预测的空分支。
inline void note_view_read()
{
    if (!detail::g_enabled.load(std::memory_order_relaxed))
        return;
    detail::g_view_reads.fetch_add(1, std::memory_order_relaxed);
}

// ---- 绘制形状标记（B107 建 / B108 扩：直方图 + 精确总量）----
//
// 用法（两个 draw 入口各两行）：
//     const perf_probe::DrawShapeMarkArmed shape_mark = perf_probe::make_draw_shape_mark(
//         perf_probe::DrawShapeSlot::indexed(index_count, start_index_location, base_vertex_location));
//     perf_probe::mark_draw_shape_call(shape_mark);
//     ... inspect_target_upscaler_draw(context, index_count, shape_mark) ...
//     （识别成功处：`perf_probe::mark_draw_shape_identified(shape_mark);`
//       + `perf_probe::note_identified_total();` —— 后者是漏斗层的非抽样计数，
//         两者必须成对出现，否则 flush 会打 mismatch 行）
//
// 【开销自证】
//   - 探针**关**：`make_draw_shape_mark` 只做一次 relaxed 读并返回 `armed=false`
//     ⇒ `mark_draw_shape_call()` 与 `mark_draw_shape_identified()` 都是可预测的空分支，
//     **零原子操作**（与 `FunnelScope` 同一纪律）；
//   - 探针**开**：调用一次 = 3 次原子加（总量、直方图桶、形状表桶）+ 形状表的有界探测
//     （命中 1 次比较；未命中最多 8 次比较 + 1 次 CAS；窗口满时再加一次有界淘汰扫描）。
//     识别成功再加一次 `record_identified` + `note_identified`（每帧只 1 次左右）。
//     实测值由 `PerfProbeTest` 的 `bench_probe_cost` 打印（沿用 B106 的自证方式）。
struct DrawShapeMarkArmed
{
    bool armed = false;
    DrawShapeSlot slot {};
};

inline DrawShapeMarkArmed make_draw_shape_mark(const DrawShapeSlot &slot)
{
    DrawShapeMarkArmed mark;
    mark.armed = detail::g_enabled.load(std::memory_order_relaxed);
    mark.slot = slot;
    return mark;
}

inline void mark_draw_shape_call(const DrawShapeMarkArmed &mark)
{
    if (!mark.armed)
        return;
    detail::g_shape_entries.fetch_add(1, std::memory_order_relaxed);
    if (!mark.slot.valid)
        return;
    detail::g_draw_shape_table.record_call(mark.slot.value);
    // B108：容量无关的元素数直方图（同一个实参，不需要额外 COM 调用）。
    detail::g_draw_element_histogram.record_call(
        unpack_draw_shape_kind(mark.slot.value), unpack_draw_shape_count(mark.slot.value));
}

inline void mark_draw_shape_identified(const DrawShapeMarkArmed &mark)
{
    if (!mark.armed)
        return;
    // 形状层埋点计数（**与 `note_identified_total` 是两只独立的手**，见 flush 的自洽断言）
    detail::g_shape_mark_total.fetch_add(1, std::memory_order_relaxed);
    if (!mark.slot.valid)
        return;
    detail::g_draw_shape_table.record_identified(mark.slot.value);
    detail::g_draw_shape_mark.note_identified(mark.slot.value);
    detail::g_draw_element_histogram.record_identified(
        unpack_draw_shape_kind(mark.slot.value), unpack_draw_shape_count(mark.slot.value));
}

// 漏斗层的"识别成功"**非抽样**计数（B108 自洽断言用）。
// ⚠️ 与 `FunnelScope::mark(FunnelStage::identified)`（**抽样**）是两回事：
//   那个要乘步长才能和形状层的精确数比，这个直接可比。
inline void note_identified_total()
{
    if (!detail::g_enabled.load(std::memory_order_relaxed))
        return;
    detail::g_funnel_identified_total.fetch_add(1, std::memory_order_relaxed);
}

#else // !defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS) —— 发布构建：诊断核心整体排除
// ===========================================================================
// 【发布构建的空实现面】
//
// 目标：埋点语句**一行都不删**（源码保持完整），但在发布构建里
//   ① 不产生任何**诊断字符串** —— 全部汇总/格式化/名称表都在上面的 `#if` 里；
//   ② 不产生任何**实际代码** —— 下面全是 `inline` 空体，优化器直接消掉。
//
// 保留的**唯一真实逻辑**是正式功能（B109）要用的两小块：
//   - 注入式日志出口 `set_log_sink`：B109 的自我证伪告警必须能输出
//     （那行 `draw_entry_filter_disabled …` 是**功能**，不是诊断）；
//   - 形状槽打包/解包（`DrawShapeSlot` / `DrawShapeMarkArmed` / `pack_draw_shape` /
//     `unpack_draw_shape_kind`）：告警行要写清是哪个钩子入口（`ix` / `dr`）。
//     ⇒ 它们只是"把已经拿在手里的实参打包"，**不建表、不计数、不取时间、无字符串**。
//
// 其余符号都是**签名兜底的空实现**：只为让任何一处没被 `#if` 排除干净的埋点语句
// 照样能编译通过（防御性 —— 不改变行为，也不引入字符串）。
// ===========================================================================

namespace detail
{
// B109 告警行的输出口（配置见 `set_log_sink`）。
inline std::atomic<LogSink> g_sink { nullptr };
} // namespace detail

inline void set_log_sink(LogSink sink)
{
    detail::g_sink.store(sink, std::memory_order_relaxed);
}

// ---- 形状槽打包/解包（位域与诊断分支**完全一致**：版本内稳定）----
inline std::uint64_t pack_draw_shape(int kind_index, std::uint32_t element_count,
                                     std::uint32_t start, std::int32_t base)
{
    const std::uint64_t kind_bits = static_cast<std::uint64_t>(static_cast<std::uint32_t>(kind_index) & 0x3u);
    const std::uint64_t count_bits = static_cast<std::uint64_t>(element_count > 0xFFFFu ? 0xFFFFu : element_count);
    const std::uint64_t start_bits = static_cast<std::uint64_t>(start & 0xFFFFFFu);
    const std::uint64_t base_bits = static_cast<std::uint64_t>(static_cast<std::uint32_t>(base) & 0xFFFFu);
    return kind_bits | (count_bits << 2) | (start_bits << 18) | (base_bits << 42);
}

inline int unpack_draw_shape_kind(std::uint64_t packed)
{
    return static_cast<int>(packed & 0x3ull);
}

// 打包后的形状槽（发布构建不建表、不计数；只为把入口种类带给 B109 的告警行）。
struct DrawShapeSlot
{
    std::uint64_t value = 0;
    bool valid = false;
    std::uint32_t instance_count = 0;

    DrawShapeSlot() = default;

    static DrawShapeSlot indexed(std::uint32_t index_count, std::uint32_t start_index, std::int32_t base_vertex)
    {
        DrawShapeSlot slot;
        slot.value = pack_draw_shape(0, index_count, start_index, base_vertex);
        slot.valid = true;
        return slot;
    }

    static DrawShapeSlot non_indexed(std::uint32_t vertex_count, std::uint32_t start_vertex)
    {
        DrawShapeSlot slot;
        slot.value = pack_draw_shape(2, vertex_count, start_vertex, 0);
        slot.valid = true;
        return slot;
    }

    static DrawShapeSlot indexed_instanced(std::uint32_t index_count, std::uint32_t instance_count,
                                           std::uint32_t start_index, std::int32_t base_vertex)
    {
        DrawShapeSlot slot;
        slot.value = pack_draw_shape(1, index_count, start_index, base_vertex);
        slot.instance_count = instance_count;
        slot.valid = true;
        return slot;
    }

    static DrawShapeSlot non_indexed_instanced(std::uint32_t vertex_count, std::uint32_t instance_count,
                                               std::uint32_t start_vertex)
    {
        DrawShapeSlot slot;
        slot.value = pack_draw_shape(3, vertex_count, start_vertex, 0);
        slot.instance_count = instance_count;
        slot.valid = true;
        return slot;
    }
};

struct DrawShapeMarkArmed
{
    bool armed = false;
    DrawShapeSlot slot {};
};

inline DrawShapeMarkArmed make_draw_shape_mark(const DrawShapeSlot &slot)
{
    DrawShapeMarkArmed mark;
    mark.armed = false; // 发布构建没有形状表 ⇒ 永远不"上膛"
    mark.slot = slot;
    return mark;
}

// 诊断建表/计数：发布构建为空操作（入口种类仍由 `mark.slot` 携带）。
inline void mark_draw_shape_call(const DrawShapeMarkArmed &) {}
inline void mark_draw_shape_identified(const DrawShapeMarkArmed &) {}
inline void note_identified_total() {}
inline void note_view_read() {}

// ---- 下面全是签名兜底的空实现（发布构建里没有任何调用点会走到真实逻辑）----
enum class Segment : std::uint32_t { present = 0, il2cpp_observer, draw_hook, dispatch_hook, jitter_observer,
    gpu_query, config_io, render_scale, upscale_dispatch, interop_prep, interop_signal, interop_queue_wait,
    interop_submit, finish_wait, finish_copy, count };
enum class Counter : std::uint32_t { virtual_protect = 0, flush_instruction_cache, sleep, wait_single_object,
    config_read, file_probe, count };
enum class FunnelStage : std::uint32_t { entry = 0, after_om, after_prescreen, after_ps_query, after_fast_path,
    fast_hit, after_cb, after_viewport, after_output, fixed_ok, dynamic_ok, identified, count };
enum class FrameSource : std::uint32_t { upscale = 0, present };

inline void configure(bool, std::uint32_t, std::uint32_t) {}
inline bool enabled() { return false; }
inline std::uint32_t draw_sample_stride() { return 32; }
inline const char *segment_name(Segment) { return ""; }

// 计时作用域（空体：不取时间、不计数）。
class Scope
{
public:
    explicit Scope(Segment) {}
};

class SampledScope
{
public:
    SampledScope(Segment, std::atomic_uint64_t &, std::atomic_uint64_t &, std::uint32_t, std::size_t = 0) {}
    bool sampled() const { return false; }
};

// 内省漏斗作用域（空体）。
class FunnelScope
{
public:
    FunnelScope() = default;
    bool sampled() const { return false; }
    void mark(FunnelStage) const {}
};

// 复用已有分段测量：发布构建直接丢弃。
inline void add_span_us(Segment, std::uint64_t) {}

// 重量级系统调用计数：发布构建直接丢弃（热路径上不能有任何计数原子操作）。
inline void count(Counter) {}
inline void count_with_us(Counter, std::uint64_t) {}
inline void sleep_counted(DWORD milliseconds) { Sleep(milliseconds); }

// ⚠️ 下面两个是**功能路径**的包装（Il2CppCallSiteHook / TransparentJitterHook 在装钩子与
// 还原时调用）⇒ 发布构建里必须**原样转发**到 Win32 API，行为与诊断分支完全一致。
inline BOOL virtual_protect_counted(LPVOID address, SIZE_T size, DWORD new_protect, PDWORD old_protect)
{
    return VirtualProtect(address, size, new_protect, old_protect);
}

inline BOOL flush_instruction_cache_counted(HANDLE process, LPCVOID base, SIZE_T size)
{
    return FlushInstructionCache(process, base, size);
}

// 显式阻塞等待：同上，必须原样转发（Ffx12Backend 的 fence 等待语义不能变）。
inline DWORD wait_single_object_counted(HANDLE handle, DWORD milliseconds)
{
    return WaitForSingleObject(handle, milliseconds);
}

inline void note_frame(FrameSource) {}
inline void note_jitter_call() {}
inline void flush_now() {}
inline void maybe_flush() {}

inline std::atomic_uint64_t &draw_call_counter()
{
    static std::atomic_uint64_t counter { 0 };
    return counter;
}

inline std::atomic_uint64_t &draw_sample_counter()
{
    static std::atomic_uint64_t counter { 0 };
    return counter;
}

inline std::atomic_uint64_t &dispatch_call_counter()
{
    static std::atomic_uint64_t counter { 0 };
    return counter;
}

inline std::atomic_uint64_t &dispatch_sample_counter()
{
    static std::atomic_uint64_t counter { 0 };
    return counter;
}

inline constexpr std::size_t k_draw_all_slot = 0;
inline constexpr std::size_t k_draw_inspect_slot = 1;

// draw 钩子内省段的计时作用域（空体）。
class DrawInspectScope
{
public:
    explicit DrawInspectScope(bool) {}
};

#endif // DX11FSRBRIDGE_ENABLE_DIAGNOSTICS

// ===========================================================================
// ---- 入口级过滤器（B109：在**进内省之前**用调用实参 `element_count == 3` 淘汰）----
//
// 【为什么放在这里做】
//   B106 的漏斗证明函数**体内**的便宜判据已经用尽（cb0 只拦 7.5%、视口 0%），
//   B107/B108 的形状分桶把"这 ~490 次调用分别是什么形状"量了出来。真机读数
//   （用户实测 `perf_probe_draw_shape_draw` 第 4 行元素数直方图）是：
//     - `count == 3` 是**唯一有 hits 的桶**（`elem_ix 3: 2490/249`、
//       `elem_dr 3: 6997/250`），其余区间（6 / 12 / 17-31 / … / 32768+）hits 全 0；
//     - ⇒ 样本内约 6000 次识别**全部**满足 `count == 3`（`outside_hits = 0`）；
//     - ⇒ 每区间约 121,100 次调用里有约 111,613 次可拒（**92.2%**）。
//   ⇒ 判据放在**调用入口**（只读一个实参、零 COM 调用）可省下
//     `0.45 µs × 490 draws/帧 × 92.2% ≈ 203 µs/帧`。
//
// 【判据】
//     `element_count != 3` ⇒ **不进内省**（索引绘制的 `IndexCount` 与
//     非索引绘制的 `VertexCount` 走同一处判断，两个钩子入口都覆盖）。
//
// 【为什么必须带"自我证伪"保险】
//   代码层面**两套签名都不检查 `element_count`**
//   （`inspect_target_upscaler_draw_on_demand` 的注释明说"不硬性要求 3"）
//   ⇒ **理论上非 3 的绘制也可能被识别** ⇒ 判据**不是代码级等价**，
//   它只是"样本内的必要条件"（真机 `outside_hits = 0` 是唯一的经验支撑）。
//   ⇒ 于是：
//     ① 在**每一次识别成功处**都检查本次绘制的 `element_count`
//        （`note_draw_identified_element_count`，覆盖全部识别成功路径）；
//     ② 一旦出现**任何一次** `element_count != 3` 的识别成功 ⇒ **一次性、单向**
//        停用本过滤器（原子标志）⇒ 之后所有绘制一律走原路径；
//     ③ 停用是 **fail-open**：最坏情况只是"退回优化前的慢路径"，**不会漏目标**。
//
// 【为什么还要 canary（抽样放行）】
//   ⚠️ 诚实标注：如果入口过滤器把**所有**非 3 绘制都拦下，那么"识别成功处"的检查
//   就**永远看不到**非 3 的识别 ⇒ 保险会变成**空转的死代码**（"有保险"就成了假的安全感）。
//   ⇒ 过滤器生效时按 `stride` **抽样放行**非 3 绘制（canary）：它们照原路径走完整内省，
//   一旦其中任何一个被识别，保险立刻触发并把过滤器永久停用。
//     - `stride = 0`：不放行（完全信任判据 ⇒ 保险退化为纯记账）；
//     - `stride = 1`：全部放行（等价于没装过滤器，最保守）；
//     - `stride = N`：每 N 个非 3 绘制放行 1 个（默认 64）。
//   canary 的代价 = `拒掉比例 / N × 原内省耗时`（默认 N=64 时 ≈3 µs/帧，占省下的
//   203 µs/帧 的 1.5%）；换来的是"保险真的会响"。
//
// 【开销（自证 见 PerfProbeTest 的 bench_probe_cost）】
//   - `element_count == 3`：1 次比较（最热的一支，真机里唯一有 hits 的形状）；
//   - 过滤器关闭（ini 关 / 已被自我证伪）：再加 1 次 relaxed 读，**零原子操作**；
//   - 非 3 且过滤器生效：1 次 relaxed `fetch_add`（canary 序号）。
//
// 【与 `PerfProbe` 总开关的关系】
//   本模块是**正式功能**（默认开），**不跟** `PerfProbe` 开关：即使 `PerfProbe=0`，
//   过滤器照样生效、保险照样会打告警行。探针只负责把它的状态与计数**打印出来**。
// ===========================================================================

// 入口种类（只用于日志里写清"是哪个钩子入口"）。
enum class DrawEntryKind : std::uint32_t
{
    indexed = 0,     // hooked_draw_indexed（IndexCount）
    non_indexed = 1, // hooked_draw（VertexCount）
    mirror = 2,      // 非钩子入口：g_state 镜像路径（`inspect_target_upscaler_draw(UINT)`）
    unknown = 3
};

inline const char *draw_entry_kind_name(DrawEntryKind kind)
{
    switch (kind)
    {
    case DrawEntryKind::indexed: return "ix";
    case DrawEntryKind::non_indexed: return "dr";
    case DrawEntryKind::mirror: return "mirror";
    default: return "?";
    }
}

// 形状槽 → 入口种类。⚠️ 复用形状行的同一套 token（`ix` / `dr`）⇒ 告警行可以直接
// 和形状行对齐着看。刻意**不读 D3D 状态**（用钩子入口已经打包好的实参）。
inline DrawEntryKind draw_entry_kind_from_mark(const DrawShapeMarkArmed &mark)
{
    if (!mark.slot.valid || mark.slot.value == 0)
        return DrawEntryKind::unknown;
    switch (unpack_draw_shape_kind(mark.slot.value))
    {
    case 0: return DrawEntryKind::indexed;
    case 2: return DrawEntryKind::non_indexed;
    default: return DrawEntryKind::unknown; // 实例化变体（本桥没装这两个钩子）
    }
}

// 入口判据的结论：`admitted == false` ⇒ **不要进内省**（零 COM 调用直接返回）。
struct DrawEntryAdmit
{
    bool admitted = true;
    bool canary = false; // true = 这是被抽样放行的非 3 绘制（保险的探针）
};

namespace detail
{
// 过滤器状态。`configured` 来自 ini（`DrawEntryFilter`）；`disabled` 是**自我证伪**的
// 一次性闩锁（只由 `note_draw_identified_element_count` 置位，**永不复位**）。
inline std::atomic_bool g_entry_filter_configured { false };
inline std::atomic_bool g_entry_filter_disabled { false };
inline std::atomic<std::uint32_t> g_entry_filter_canary_stride { 64 };
// 区间计数（flush 无条件读走 ⇒ 不跨区间残留；口径见各字段注释）。
inline std::atomic_uint64_t g_entry_filter_sequence { 0 };   // canary 抽样序号（非区间）
inline std::atomic_uint64_t g_entry_filter_skipped { 0 };    // 被入口淘汰的调用数
inline std::atomic_uint64_t g_entry_filter_canary { 0 };     // 被 canary 放行的非 3 调用数
inline std::atomic_uint64_t g_entry_filter_non3_identified { 0 }; // 非 3 且识别成功的次数
inline std::atomic<std::uint32_t> g_entry_filter_disable_element_count { 0 };
// 初始值 = `unknown`（**不是 0**：0 是 `indexed` ⇒ 会把"还没触发"打印成 `disable_entry=ix`，
// 单测当场抓到过这一点）。
inline std::atomic<std::uint32_t> g_entry_filter_disable_entry {
    static_cast<std::uint32_t>(DrawEntryKind::unknown) };
inline std::atomic_uint64_t g_entry_filter_disable_at_ms { 0 };
} // namespace detail

// ini 配置（由 Dx11FsrBridge 在 initialize() 里调用一次，段必须是 `[Dx11FsrBridge]`）。
inline void configure_draw_entry_filter(bool enabled, std::uint32_t canary_stride)
{
    detail::g_entry_filter_canary_stride.store(canary_stride, std::memory_order_relaxed);
    detail::g_entry_filter_configured.store(enabled, std::memory_order_release);
}

inline bool draw_entry_filter_configured()
{
    return detail::g_entry_filter_configured.load(std::memory_order_acquire);
}

// 自我证伪闩锁（单向）。置位后所有绘制走原路径（fail-open）。
inline bool draw_entry_filter_disabled()
{
    return detail::g_entry_filter_disabled.load(std::memory_order_acquire);
}

inline bool draw_entry_filter_active()
{
    return draw_entry_filter_configured() && !draw_entry_filter_disabled();
}

inline std::uint32_t draw_entry_filter_canary_stride()
{
    return detail::g_entry_filter_canary_stride.load(std::memory_order_relaxed);
}

// 三个状态名（写进日志，让人一眼看出是"关掉"还是"被自我证伪"）：
//   "on"       = ini 开着且未被证伪（判据在生效）
//   "off"      = ini 关着（`DrawEntryFilter=0`）
//   "disabled" = **被自我证伪**（出现过非 3 的识别成功 ⇒ 已 fail-open 回原路径）
inline const char *draw_entry_filter_state_name()
{
    if (draw_entry_filter_disabled())
        return "disabled";
    return draw_entry_filter_configured() ? "on" : "off";
}

// **入口判据**（每次内省调用一次，零 COM）：
//   - `element_count == 3` ⇒ 放行（真机里唯一有 hits 的桶）；
//   - 过滤器未生效（ini 关 / 已自我证伪）⇒ 放行（**原路径**，fail-open）；
//   - 否则按 canary 步长抽样：抽中 ⇒ 放行并标记 `canary`；未抽中 ⇒ `admitted=false`。
inline DrawEntryAdmit draw_entry_filter_admit(std::uint32_t element_count)
{
    DrawEntryAdmit result;
    if (element_count == 3)
        return result;
    if (!draw_entry_filter_active())
        return result;
    const std::uint32_t stride = detail::g_entry_filter_canary_stride.load(std::memory_order_relaxed);
    if (stride == 0)
    {
        // 不放行 canary：判据被完全信任（**保险此时只能记账**，见上面的诚实标注）。
        detail::g_entry_filter_skipped.fetch_add(1, std::memory_order_relaxed);
        result.admitted = false;
        return result;
    }
    if (stride > 1)
    {
        const std::uint64_t sequence =
            detail::g_entry_filter_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((sequence % stride) != 0)
        {
            detail::g_entry_filter_skipped.fetch_add(1, std::memory_order_relaxed);
            result.admitted = false;
            return result;
        }
    }
    detail::g_entry_filter_canary.fetch_add(1, std::memory_order_relaxed);
    result.canary = true;
    return result;
}

// 保险触发时的那一行（醒目、可 grep、**只在触发的那一次**输出）。
inline std::string format_draw_entry_filter_disabled(std::uint32_t element_count, DrawEntryKind entry,
                                                     bool canary, std::uint64_t non3_identified,
                                                     std::uint64_t skipped, std::uint64_t admitted_canary,
                                                     std::uint32_t stride)
{
    char line[384] {};
    std::snprintf(line, sizeof(line),
        "draw_entry_filter_disabled reason=non3_identified element_count=%u entry=%s canary=%u"
        " non3_identified=%llu skipped=%llu admitted_canary=%llu canary_stride=%u"
        " action=filter_disabled_fail_open",
        static_cast<unsigned>(element_count), draw_entry_kind_name(entry), canary ? 1u : 0u,
        static_cast<unsigned long long>(non3_identified),
        static_cast<unsigned long long>(skipped),
        static_cast<unsigned long long>(admitted_canary),
        static_cast<unsigned>(stride));
    return std::string(line);
}

// **保险**：每一次**识别成功**处调用一次（所有识别成功路径都要有）。
//   - `element_count == 3`（正常情形）⇒ 立即返回，**零原子操作**；
//   - `element_count != 3` ⇒ ① 记一笔 `non3_identified`（无论过滤器是否在生效：这是
//     "判据被证伪"的证据）；② 若过滤器正在生效 ⇒ **一次性**停用它（CAS 单向闩锁）
//     并立刻经注入的 `sink` 打一行醒目告警（**不依赖 `PerfProbe` 开关**）。
inline void note_draw_identified_element_count(std::uint32_t element_count, DrawEntryKind entry,
                                               bool canary = false)
{
    if (element_count == 3)
        return;
    detail::g_entry_filter_non3_identified.fetch_add(1, std::memory_order_relaxed);
    if (!draw_entry_filter_configured() || draw_entry_filter_disabled())
        return; // 没有过滤器在生效 ⇒ 只记账（判据被证伪的证据仍然可见）
    bool expected = false;
    if (!detail::g_entry_filter_disabled.compare_exchange_strong(expected, true,
                                                                 std::memory_order_acq_rel))
    {
        return; // 已经被另一次识别停用了（只告警一次，不刷屏）
    }
    detail::g_entry_filter_disable_element_count.store(element_count, std::memory_order_relaxed);
    detail::g_entry_filter_disable_entry.store(static_cast<std::uint32_t>(entry),
                                               std::memory_order_relaxed);
    detail::g_entry_filter_disable_at_ms.store(static_cast<std::uint64_t>(GetTickCount64()),
                                               std::memory_order_relaxed);
    if (const LogSink sink = detail::g_sink.load(std::memory_order_relaxed))
    {
        const std::string line = format_draw_entry_filter_disabled(
            element_count, entry, canary,
            detail::g_entry_filter_non3_identified.load(std::memory_order_relaxed),
            detail::g_entry_filter_skipped.load(std::memory_order_relaxed),
            detail::g_entry_filter_canary.load(std::memory_order_relaxed),
            draw_entry_filter_canary_stride());
        sink(line.c_str());
    }
}

// 形状槽重载：入口种类直接从钩子入口打包好的形状槽里取（零额外 COM、零额外参数）。
inline void note_draw_identified_element_count(std::uint32_t element_count,
                                               const DrawShapeMarkArmed &mark, bool canary)
{
    note_draw_identified_element_count(element_count, draw_entry_kind_from_mark(mark), canary);
}

// 过滤器状态的快照（`snapshot_*` 读走并复位**区间计数**；配置与闩锁不复位）。
struct DrawEntryFilterSnapshot
{
    bool configured = false;
    bool disabled = false;
    std::uint32_t canary_stride = 0;
    std::uint64_t skipped = 0;
    std::uint64_t canary = 0;
    std::uint64_t non3_identified = 0;
    std::uint32_t disable_element_count = 0;
    DrawEntryKind disable_entry = DrawEntryKind::unknown;
    std::uint64_t disable_at_ms = 0;
};

inline void fill_draw_entry_filter_state(DrawEntryFilterSnapshot &out_snapshot)
{
    out_snapshot.configured = draw_entry_filter_configured();
    out_snapshot.disabled = draw_entry_filter_disabled();
    out_snapshot.canary_stride = draw_entry_filter_canary_stride();
    out_snapshot.disable_element_count =
        detail::g_entry_filter_disable_element_count.load(std::memory_order_relaxed);
    out_snapshot.disable_entry = static_cast<DrawEntryKind>(
        detail::g_entry_filter_disable_entry.load(std::memory_order_relaxed));
    out_snapshot.disable_at_ms = detail::g_entry_filter_disable_at_ms.load(std::memory_order_relaxed);
}

// flush 用：读走（并复位）区间计数。**必须无条件调用**（同 B108 纪律：防残值串区间）。
inline DrawEntryFilterSnapshot snapshot_draw_entry_filter()
{
    DrawEntryFilterSnapshot snapshot;
    fill_draw_entry_filter_state(snapshot);
    snapshot.skipped = detail::g_entry_filter_skipped.exchange(0, std::memory_order_relaxed);
    snapshot.canary = detail::g_entry_filter_canary.exchange(0, std::memory_order_relaxed);
    snapshot.non3_identified =
        detail::g_entry_filter_non3_identified.exchange(0, std::memory_order_relaxed);
    return snapshot;
}

// 单测/诊断用：只读，**不动任何计数**。
inline DrawEntryFilterSnapshot read_draw_entry_filter()
{
    DrawEntryFilterSnapshot snapshot;
    fill_draw_entry_filter_state(snapshot);
    snapshot.skipped = detail::g_entry_filter_skipped.load(std::memory_order_relaxed);
    snapshot.canary = detail::g_entry_filter_canary.load(std::memory_order_relaxed);
    snapshot.non3_identified =
        detail::g_entry_filter_non3_identified.load(std::memory_order_relaxed);
    return snapshot;
}

#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)
// 单测/诊断读取（不影响计数）。
inline std::uint64_t read_shape_calls(std::uint64_t packed)
{
    if (packed == 0)
        return 0;
    const std::size_t index = detail::g_draw_shape_table.find_slot(packed);
    if (index >= detail::DrawShapeTable::k_bucket_count)
        return 0;
    return detail::g_draw_shape_table.calls[index].load(std::memory_order_relaxed);
}

inline std::uint64_t read_shape_hits(std::uint64_t packed)
{
    if (packed == 0)
        return 0;
    const std::size_t index = detail::g_draw_shape_table.find_slot(packed);
    if (index >= detail::DrawShapeTable::k_bucket_count)
        return 0;
    return detail::g_draw_shape_table.hits[index].load(std::memory_order_relaxed);
}

inline std::uint64_t read_shape_entries()
{
    return detail::g_shape_entries.load(std::memory_order_relaxed);
}

// 直方图的单测读取（桶的原子上直接读，不影响计数）。
inline std::uint64_t read_element_calls(int kind_index, std::uint32_t element_count)
{
    return detail::g_draw_element_histogram
        .calls[DrawElementHistogram::group_for_kind(kind_index)]
              [DrawElementHistogram::bucket_for(element_count)]
        .load(std::memory_order_relaxed);
}

inline std::uint64_t read_element_hits(int kind_index, std::uint32_t element_count)
{
    return detail::g_draw_element_histogram
        .hits[DrawElementHistogram::group_for_kind(kind_index)]
             [DrawElementHistogram::bucket_for(element_count)]
        .load(std::memory_order_relaxed);
}

inline std::uint64_t read_shape_mark_total()
{
    return detail::g_shape_mark_total.load(std::memory_order_relaxed);
}

inline std::uint64_t read_funnel_identified_total()
{
    return detail::g_funnel_identified_total.load(std::memory_order_relaxed);
}

// 形状表是否已被认领过该形状（单测用来验证"形状确实进了产物里的表"）。
inline bool shape_slot_claimed(std::uint64_t packed)
{
    if (packed == 0)
        return false;
    return detail::g_draw_shape_table.find_slot(packed) < detail::DrawShapeTable::k_bucket_count;
}


// ---- 内省漏斗作用域（B106：每道判定"还剩多少"）----
//
// 【为什么是"作用域"而不是零散的计数器】
//   一次调用的采样决定必须在**构造时定死**，之后每个阶段都只在这个决定为真时自增
//   ⇒ 同一道漏斗的各阶段必然来自**同一批调用**，"逐级相减"才有意义。
//
// 【开销自证（热路径红线：加探针必须先证明探针便宜）】
//   - 探针**关闭**时：构造里只有一次 relaxed 读 ⇒ `m_sampled` 恒假，
//     `mark()` 是一次可预测的"不跳转" ⇒ 每次调用约 1 个分支；
//   - 探针**开启**时：构造 1 次 relaxed 读 + 1 次 relaxed 加（总调用计数），
//     每个阶段 1 次 relaxed 加，且**只有被抽样到的调用**才会命中。
//   ⇒ 关时零原子操作（与 `SampledScope` 同一纪律），开时的成本由抽样步长决定。
class FunnelScope
{
public:
    FunnelScope()
    {
        if (!detail::g_enabled.load(std::memory_order_relaxed))
            return;
        const std::uint64_t index = detail::g_funnel_calls.fetch_add(1, std::memory_order_relaxed);
        const std::uint32_t step = detail::g_draw_sample.load(std::memory_order_relaxed);
        m_sampled = ((index % (step != 0 ? step : 1)) == 0);
    }

    // 本次调用是否被抽样到（未被抽样 ⇒ 所有 `mark()` 都是空操作）。
    bool sampled() const
    {
        return m_sampled;
    }

    // "已到达该阶段"。**只在真正到达时调用**（早期 return 之后的阶段不许标记）。
    void mark(FunnelStage stage) const
    {
        if (!m_sampled)
            return;
        detail::g_funnel[static_cast<std::size_t>(stage)].fetch_add(1, std::memory_order_relaxed);
    }

    // 视图读取（工作量）计数不在这里：见命名空间级的 `perf_probe::note_view_read()`，
    // 内省路径的读取点直接调它（**不按抽样** —— 它要能直接和"次/帧"对上）。

    FunnelScope(const FunnelScope &) = delete;
    FunnelScope &operator=(const FunnelScope &) = delete;

private:
    bool m_sampled = false;
};

// ---- 复用已有的分段测量（不额外取时间）----
// Ffx12Backend 已经用 qpc_us() 量了 prep/signal/w12/submit/wait/copy；
// 这里只把这些已有的数字投进探针，**零附加开销**。
inline void add_span_us(Segment segment, std::uint64_t microseconds)
{
    if (!detail::g_enabled.load(std::memory_order_relaxed))
        return;
    detail::add_ns(segment, microseconds * 1000ull);
}

// ---- 重量级系统调用计数 ----
inline void count(Counter counter)
{
    if (!detail::g_enabled.load(std::memory_order_relaxed))
        return;
    detail::g_counters[static_cast<std::size_t>(counter)].count.fetch_add(1, std::memory_order_relaxed);
}

// `VirtualProtect` / `FlushInstructionCache` 的**计数包装**。
//
// 为什么需要它：这两个是重量级系统调用（改页属性 / 刷指令缓存，会跨核同步），
// 若出现在每帧路径上，量级（几十微秒~毫秒）**正好符合**"每帧多花 0.4~0.6 ms"
// 的症状 ⇒ 必须能一眼看出"每秒调用几次"。代码审计已确认本仓库所有调用点都在
// **安装/还原**路径（见批次数 B103 的清单），运行期计数为 0 即为实证。
inline BOOL virtual_protect_counted(LPVOID address, SIZE_T size, DWORD new_protect, PDWORD old_protect)
{
    count(Counter::virtual_protect);
    return VirtualProtect(address, size, new_protect, old_protect);
}

inline BOOL flush_instruction_cache_counted(HANDLE process, LPCVOID base, SIZE_T size)
{
    count(Counter::flush_instruction_cache);
    return FlushInstructionCache(process, base, size);
}

inline void sleep_counted(DWORD milliseconds)
{
    count(Counter::sleep);
    Sleep(milliseconds);
}

inline void count_with_us(Counter counter, std::uint64_t microseconds)
{
    if (!detail::g_enabled.load(std::memory_order_relaxed))
        return;
    detail::CounterStat &stat = detail::g_counters[static_cast<std::size_t>(counter)];
    stat.count.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t current = stat.max_us.load(std::memory_order_relaxed);
    if (microseconds > current)
        stat.max_us.store(microseconds, std::memory_order_relaxed);
}

// 显式阻塞等待的计数包装（并记录**实测阻塞时长**）。
// 用途：`waitobj` + `wso_max_us` 两个数一起看 —— 次数为 0 就证明"每帧没有显式阻塞"，
// 次数 > 0 且 max 很大就说明"每帧在等"（正是"GPU 空转 + 帧率上不去"的形态）。
inline DWORD wait_single_object_counted(HANDLE handle, DWORD milliseconds)
{
    const ULONGLONG start_ms = GetTickCount64();
    const DWORD result = WaitForSingleObject(handle, milliseconds);
    const ULONGLONG elapsed = GetTickCount64() - start_ms;
    count_with_us(Counter::wait_single_object, static_cast<std::uint64_t>(elapsed) * 1000ull);
    return result;
}

// ---- 帧边界 ----
// 每游戏帧调一次（ffx12::dispatch 入口 / hooked_present 入口）。
inline void note_frame(FrameSource source)
{
    if (!detail::g_enabled.load(std::memory_order_relaxed))
        return;
    if (source == FrameSource::upscale)
        detail::g_frames_upscale.fetch_add(1, std::memory_order_relaxed);
    else
        detail::g_frames_present.fetch_add(1, std::memory_order_relaxed);
}

inline void note_jitter_call()
{
    if (!detail::g_enabled.load(std::memory_order_relaxed))
        return;
    detail::g_jitter_calls.fetch_add(1, std::memory_order_relaxed);
}

// 汇总一行并复位计数器。**任何线程**都可调用（内部用 CAS 保证同一时刻只有一个）。
inline void flush_now()
{
    if (!detail::g_enabled.load(std::memory_order_relaxed))
        return;
    bool expected = false;
    if (!detail::g_flushing.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;

    const std::int64_t now = detail::qpc_ticks();
    const std::uint64_t start = detail::g_interval_start_ticks.exchange(
        static_cast<std::uint64_t>(now), std::memory_order_relaxed);
    const std::uint64_t elapsed_ns = (start != 0 && now > static_cast<std::int64_t>(start))
        ? detail::ticks_to_ns(now - static_cast<std::int64_t>(start))
        : 0;
    const double elapsed_ms = static_cast<double>(elapsed_ns) / 1.0e6;

    const std::uint64_t frames_upscale = detail::g_frames_upscale.exchange(0, std::memory_order_relaxed);
    const std::uint64_t frames_present = detail::g_frames_present.exchange(0, std::memory_order_relaxed);
    const std::uint64_t draw_calls = detail::g_draw_calls.exchange(0, std::memory_order_relaxed);
    const std::uint64_t draw_sampled = detail::g_draw_sampled.exchange(0, std::memory_order_relaxed);
    const std::uint64_t dispatch_calls = detail::g_dispatch_calls.exchange(0, std::memory_order_relaxed);
    const std::uint64_t dispatch_sampled = detail::g_dispatch_sampled.exchange(0, std::memory_order_relaxed);
    const std::uint64_t jitter_calls = detail::g_jitter_calls.exchange(0, std::memory_order_relaxed);
    const std::uint64_t scope_calls = detail::g_scope_calls.exchange(0, std::memory_order_relaxed);
    const std::uint64_t inspect_entered =
        detail::g_draw_inspect_entered.exchange(0, std::memory_order_relaxed);
    // B106 漏斗：内省调用总数（非抽样）、每个阶段的抽样计数、视图读取次数（工作量）。
    const std::uint64_t funnel_calls = detail::g_funnel_calls.exchange(0, std::memory_order_relaxed);
    const std::uint64_t view_reads = detail::g_view_reads.exchange(0, std::memory_order_relaxed);
    std::uint64_t funnel_values[detail::k_funnel_count] {};
    for (std::size_t i = 0; i < detail::k_funnel_count; ++i)
        funnel_values[i] = detail::g_funnel[i].exchange(0, std::memory_order_relaxed);
    const std::uint64_t funnel_sampled = funnel_values[static_cast<std::size_t>(FunnelStage::entry)];
    // B108：漏斗层的"识别成功"**非抽样**总数（与形状层的精确数交叉核对）。
    // ⚠️ 必须**无条件**读走（即使这一秒没有 draw / 没有形状行）：否则残值会串到下一区间。
    const std::uint64_t funnel_identified_total =
        detail::g_funnel_identified_total.exchange(0, std::memory_order_relaxed);
    // B109 入口过滤器：区间计数同样**无条件**读走（配置与自我证伪闩锁不复位）。
    const DrawEntryFilterSnapshot entry_filter = snapshot_draw_entry_filter();

    // 每段：calls / 每帧 µs / max µs，并复位
    std::uint64_t calls[detail::k_segment_count] {};
    double per_frame[detail::k_segment_count] {};
    double max_us[detail::k_segment_count] {};
    const std::uint64_t frames = frames_upscale != 0 ? frames_upscale : frames_present;
    for (std::size_t i = 0; i < detail::k_segment_count; ++i)
    {
        detail::SegmentStat &stat = detail::g_segments[i];
        const std::uint64_t total_ns = stat.total_ns.exchange(0, std::memory_order_relaxed);
        const std::uint64_t max_ns = stat.max_ns.exchange(0, std::memory_order_relaxed);
        calls[i] = stat.calls.exchange(0, std::memory_order_relaxed);
        per_frame[i] = detail::per_frame_us(total_ns, frames);
        max_us[i] = static_cast<double>(max_ns) / 1000.0;
    }

    // draw / dispatch：把**抽样**得到的均值按"实际次数/抽样次数"外推，
    // 得到"这一段每帧真实耗时"的估计（max 仍是单次调用的实测最大值）。
    const auto extrapolate = [&](std::size_t index, std::uint64_t total, std::uint64_t sampled)
    {
        if (sampled == 0 || total == 0 || frames == 0)
            return 0.0;
        const double per_sample_us = per_frame[index] / static_cast<double>(sampled);
        return per_sample_us * static_cast<double>(total);
    };
    const double draw_hook_per_frame = extrapolate(static_cast<std::size_t>(Segment::draw_hook),
                                                   draw_calls, draw_sampled);
    const double dispatch_hook_per_frame = extrapolate(static_cast<std::size_t>(Segment::dispatch_hook),
                                                       dispatch_calls, dispatch_sampled);

    // acct = **不互相嵌套**的段之和；nested 段（ups/prep/signal/w12/submit/finw/finc）
    // 一律不进 acct（它们发生在 draw_hook / present 内部，进来就重复计数）。
    const double acct_us = per_frame[static_cast<std::size_t>(Segment::present)] +
        per_frame[static_cast<std::size_t>(Segment::il2cpp_observer)] +
        draw_hook_per_frame +
        dispatch_hook_per_frame +
        per_frame[static_cast<std::size_t>(Segment::jitter_observer)] +
        per_frame[static_cast<std::size_t>(Segment::gpu_query)] +
        per_frame[static_cast<std::size_t>(Segment::config_io)] +
        per_frame[static_cast<std::size_t>(Segment::render_scale)];
    const double budget_us = frames != 0 ? elapsed_ms * 1000.0 / static_cast<double>(frames) : 0.0;
    const double unacc_us = budget_us - acct_us;
    // 探针自身开销估算：每次 Scope ≈ 2×QPC + 3 原子加 ≈ 60 ns（本机量级）。
    const double self_est_us = static_cast<double>(scope_calls) * 0.00006;

    // 计数器：只报"本区间几次"（热路径要看的就是每秒几次），报完即复位。
    std::uint64_t counter_values[detail::k_counter_count] {};
    std::uint64_t counter_max_us[detail::k_counter_count] {};
    for (std::size_t i = 0; i < detail::k_counter_count; ++i)
    {
        counter_values[i] = detail::g_counters[i].count.exchange(0, std::memory_order_relaxed);
        counter_max_us[i] = detail::g_counters[i].max_us.exchange(0, std::memory_order_relaxed);
    }

    if (const LogSink sink = detail::g_sink.load(std::memory_order_relaxed))
    {
        // ⚠️ 缓冲区按"字段全为 20 位数字"的最坏情形留余量（B109 加了 `entry_filter=`）：
        // 截断会静默吃掉行尾字段，而这个项目的验收流程**逐字段读日志**。
        char line[1536] {};
        const double fps = elapsed_ms > 0.0 ? static_cast<double>(frames) * 1000.0 / elapsed_ms : 0.0;
        std::snprintf(line, sizeof(line),
            "perf_probe ms=%.0f fps=%.1f frames=%llu ups=%llu present=%llu draws=%llu"
            " budget_us=%.1f acct_us=%.1f unacc_us=%.1f self_est_us=%.1f"
            " | per_frame_us present=%.1f il2cpp=%.1f drawhook=%.1f disphook=%.1f jitter=%.1f gpuq=%.1f cfgio=%.1f rscale=%.1f"
            " | nested_us ups=%.1f prep=%.1f signal=%.1f w12=%.1f submit=%.1f finw=%.1f finc=%.1f"
            " | max_us drawhook=%.1f disphook=%.1f ups=%.1f prep=%.1f signal=%.1f w12=%.1f submit=%.1f finw=%.1f finc=%.1f"
            " | n draw_samp=%llu disp_samp=%llu jitter=%llu draw_pf=%.1f"
            " | cnt vprotect=%llu flushic=%llu sleep=%llu waitobj=%llu cfgread=%llu fileprobe=%llu wso_max_us=%llu"
            " | entry_filter=%s",
            elapsed_ms, fps,
            static_cast<unsigned long long>(frames),
            static_cast<unsigned long long>(frames_upscale),
            static_cast<unsigned long long>(frames_present),
            static_cast<unsigned long long>(draw_calls),
            budget_us, acct_us, unacc_us, self_est_us,
            per_frame[static_cast<std::size_t>(Segment::present)],
            per_frame[static_cast<std::size_t>(Segment::il2cpp_observer)],
            draw_hook_per_frame,
            dispatch_hook_per_frame,
            per_frame[static_cast<std::size_t>(Segment::jitter_observer)],
            per_frame[static_cast<std::size_t>(Segment::gpu_query)],
            per_frame[static_cast<std::size_t>(Segment::config_io)],
            per_frame[static_cast<std::size_t>(Segment::render_scale)],
            per_frame[static_cast<std::size_t>(Segment::upscale_dispatch)],
            per_frame[static_cast<std::size_t>(Segment::interop_prep)],
            per_frame[static_cast<std::size_t>(Segment::interop_signal)],
            per_frame[static_cast<std::size_t>(Segment::interop_queue_wait)],
            per_frame[static_cast<std::size_t>(Segment::interop_submit)],
            per_frame[static_cast<std::size_t>(Segment::finish_wait)],
            per_frame[static_cast<std::size_t>(Segment::finish_copy)],
            max_us[static_cast<std::size_t>(Segment::draw_hook)],
            max_us[static_cast<std::size_t>(Segment::dispatch_hook)],
            max_us[static_cast<std::size_t>(Segment::upscale_dispatch)],
            max_us[static_cast<std::size_t>(Segment::interop_prep)],
            max_us[static_cast<std::size_t>(Segment::interop_signal)],
            max_us[static_cast<std::size_t>(Segment::interop_queue_wait)],
            max_us[static_cast<std::size_t>(Segment::interop_submit)],
            max_us[static_cast<std::size_t>(Segment::finish_wait)],
            max_us[static_cast<std::size_t>(Segment::finish_copy)],
            static_cast<unsigned long long>(draw_sampled),
            static_cast<unsigned long long>(dispatch_sampled),
            static_cast<unsigned long long>(jitter_calls),
            frames != 0 ? static_cast<double>(draw_calls) / static_cast<double>(frames) : 0.0,
            static_cast<unsigned long long>(counter_values[0]),
            static_cast<unsigned long long>(counter_values[1]),
            static_cast<unsigned long long>(counter_values[2]),
            static_cast<unsigned long long>(counter_values[3]),
            static_cast<unsigned long long>(counter_values[4]),
            static_cast<unsigned long long>(counter_values[5]),
            static_cast<unsigned long long>(counter_max_us[static_cast<std::size_t>(Counter::wait_single_object)]),
            draw_entry_filter_state_name());
        sink(line);

        // 第二行：**单次调用成本直方图**（只含已抽样的那次调用）。
        // 与上一行 `per_frame_us drawhook=` 的区别：上一行是"抽样均值 × 次数"的外推
        // （每帧总量），本行是**分布** —— 用来判断"均值是不是被少数巨贵调用带偏的"。
        char histogram_line[768] {};
        std::snprintf(histogram_line, sizeof(histogram_line),
            "perf_probe_hist_draw ms=%.0f draws_total=%llu draws_sampled=%llu entered=%llu"
            " | draw_all %s | draw_inspect %s",
            elapsed_ms,
            static_cast<unsigned long long>(draw_calls),
            static_cast<unsigned long long>(draw_sampled),
            static_cast<unsigned long long>(inspect_entered),
            detail::g_draw_cost[0].line().c_str(),
            detail::g_draw_cost[1].line().c_str());
        sink(histogram_line);

        // 第三行：**内省漏斗**（B106）—— 每一道判定"之后还剩多少"。
        // ⚠️ 只在**本区间真的抽到过内省调用**时输出：探针开着但这一秒没有 draw 的区间不刷屏。
        // 读法：相邻两级相减 = 那一道判定拦下的量；`views_per_call` 是**工作量**
        // （优化前后直接对比它，不受时钟/负载影响）。B108 增加 `identified_total=`：
        // **非抽样**的识别成功总数 ⇒ 可与形状行的 `identified=` 直接比（必须相等）。
        if (funnel_sampled != 0)
        {
            char funnel_line[896] {};
            std::snprintf(funnel_line, sizeof(funnel_line),
                "perf_probe_funnel_draw ms=%.0f calls=%llu sampled=%llu stride=%u"
                " | views=%llu views_per_call=%.2f identified_total=%llu"
                " | entry_filter=%s canary_stride=%u skip=%llu canary=%llu non3_id=%llu"
                " disable_ec=%u disable_entry=%s |",
                elapsed_ms,
                static_cast<unsigned long long>(funnel_calls),
                static_cast<unsigned long long>(funnel_sampled),
                static_cast<unsigned>(detail::g_draw_sample.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(view_reads),
                funnel_calls != 0
                    ? static_cast<double>(view_reads) / static_cast<double>(funnel_calls)
                    : 0.0,
                static_cast<unsigned long long>(funnel_identified_total),
                draw_entry_filter_state_name(),
                static_cast<unsigned>(entry_filter.canary_stride),
                static_cast<unsigned long long>(entry_filter.skipped),
                static_cast<unsigned long long>(entry_filter.canary),
                static_cast<unsigned long long>(entry_filter.non3_identified),
                static_cast<unsigned>(entry_filter.disable_element_count),
                draw_entry_kind_name(entry_filter.disable_entry));
            std::string funnel_text = funnel_line;
            for (std::size_t i = 0; i < detail::k_funnel_count; ++i)
            {
                funnel_text += " ";
                funnel_text += detail::k_funnel_names[i];
                funnel_text += "=";
                funnel_text += std::to_string(funnel_values[i]);
            }
            sink(funnel_text.c_str());
        }
        // 第四行起：**绘制形状分桶**（B107 建 / B108 修）—— 在**调用入口**按调用参数分类，
        // 回答"这 ~490 次内省调用分别是什么形状"以及"真目标的形状集中在哪"。
        // ⚠️ 同样只在真的进过内省时输出（`entries=0` 说明这一秒没有 draw ⇒ 不刷屏）。
        // ⚠️ `shape_marks` 必须**无条件**读走（同 funnel_identified_total：防残值串区间）。
        const std::uint64_t shape_entries = detail::g_shape_entries.exchange(0, std::memory_order_relaxed);
        const std::uint64_t shape_marks = detail::g_shape_mark_total.exchange(0, std::memory_order_relaxed);
        if (shape_entries != 0)
        {
            detail::DrawShapeTable::Snapshot shape_snapshot = detail::g_draw_shape_table.snapshot();
            DrawElementHistogram::Snapshot element_snapshot;
            snapshot_draw_element_histogram(element_snapshot);
            std::string shape_text;
            format_draw_shape_report(shape_snapshot, element_snapshot, shape_entries, shape_text);
            std::size_t begin = 0;
            while (begin <= shape_text.size())
            {
                const std::size_t end = shape_text.find('\n', begin);
                const std::string line = shape_text.substr(begin, end == std::string::npos
                    ? std::string::npos : end - begin);
                if (!line.empty())
                    sink(line.c_str());
                if (end == std::string::npos)
                    break;
                begin = end + 1;
            }
            // 自洽断言（B108）：形状行 `identified` ≡ 形状层埋点数 ≡ 漏斗层埋点数，
            // 且两个守恒恒等式成立。**不成立就打一行告警**（而不是安静地输出错分布）。
            if (!draw_shape_consistency_ok(shape_snapshot, shape_marks, funnel_identified_total))
            {
                const std::string mismatch =
                    format_draw_shape_mismatch(shape_snapshot, shape_marks, funnel_identified_total);
                sink(mismatch.c_str());
            }
        }
        for (std::size_t i = 0; i < detail::DrawCostHistogram::k_draw_cost_slots; ++i)
            detail::g_draw_cost[i].reset();
    }

    detail::g_flushing.store(false, std::memory_order_release);
}

// 到达间隔就汇总一行。由帧边界（note_frame）与热路径钩子入口调用。
//
// ⚠️ 便宜闸门：本函数会被**热路径**调用（draw 钩子，可达 300k 次/秒），
// 所以先做一次 `GetTickCount64` 粗判（读 KUSER_SHARED_DATA，约十几纳秒），
// **只有粗判通过才取 QPC** ⇒ 单次调用开销从 ~30 ns（QPC）降到 ~15 ns，
// 且每 interval 只真正进入一次精确路径。
inline void maybe_flush()
{
    if (!detail::g_enabled.load(std::memory_order_relaxed))
        return;
    const ULONGLONG now_ms = GetTickCount64();
    const ULONGLONG last_check = detail::g_last_check_ms.load(std::memory_order_relaxed);
    const std::uint32_t interval = detail::g_interval_ms.load(std::memory_order_relaxed);
    if (now_ms - last_check < interval)
        return;
    detail::g_last_check_ms.store(now_ms, std::memory_order_relaxed);

    const std::uint64_t start = detail::g_interval_start_ticks.load(std::memory_order_relaxed);
    if (start == 0)
        return;
    const std::int64_t now = detail::qpc_ticks();
    if (now <= static_cast<std::int64_t>(start))
        return;
    const std::uint64_t elapsed_ns = detail::ticks_to_ns(now - static_cast<std::int64_t>(start));
    if (elapsed_ns < static_cast<std::uint64_t>(interval) * 1000000ull)
        return;
    flush_now();
}

// ---- 供单测使用的可读状态 ----
inline void read_segment(Segment segment, std::uint64_t *out_calls, std::uint64_t *out_total_ns,
                         std::uint64_t *out_max_ns)
{
    const detail::SegmentStat &stat = detail::g_segments[static_cast<std::size_t>(segment)];
    if (out_calls != nullptr)
        *out_calls = stat.calls.load(std::memory_order_relaxed);
    if (out_total_ns != nullptr)
        *out_total_ns = stat.total_ns.load(std::memory_order_relaxed);
    if (out_max_ns != nullptr)
        *out_max_ns = stat.max_ns.load(std::memory_order_relaxed);
}

inline std::uint64_t read_counter(Counter counter)
{
    return detail::g_counters[static_cast<std::size_t>(counter)].count.load(std::memory_order_relaxed);
}

inline std::uint64_t read_draw_calls()
{
    return detail::g_draw_calls.load(std::memory_order_relaxed);
}

// 本区间真正进入内省的抽样次数（slot 1 里非 0 样本的个数）。
inline std::uint64_t read_draw_inspect_entered()
{
    return detail::g_draw_inspect_entered.load(std::memory_order_relaxed);
}

// B106 漏斗：某个阶段本区间累计的抽样次数（未 flush 时的当前值）。
inline std::uint64_t read_funnel(FunnelStage stage)
{
    const auto index = static_cast<std::size_t>(stage);
    return index < detail::k_funnel_count
        ? detail::g_funnel[index].load(std::memory_order_relaxed)
        : 0;
}

// B106 漏斗：本区间内省调用总数（非抽样）与视图读取总数（工作量）。
inline std::uint64_t read_funnel_calls()
{
    return detail::g_funnel_calls.load(std::memory_order_relaxed);
}

inline std::uint64_t read_view_reads()
{
    return detail::g_view_reads.load(std::memory_order_relaxed);
}

inline const char *funnel_stage_name(FunnelStage stage)
{
    const auto index = static_cast<std::size_t>(stage);
    return index < detail::k_funnel_count ? detail::k_funnel_names[index] : "?";
}

inline std::uint64_t read_frames(FrameSource source)
{
    return source == FrameSource::upscale
        ? detail::g_frames_upscale.load(std::memory_order_relaxed)
        : detail::g_frames_present.load(std::memory_order_relaxed);
}

inline std::uint64_t read_jitter_calls()
{
    return detail::g_jitter_calls.load(std::memory_order_relaxed);
}

// 直方图访问（draw 钩子埋点用；下标含义见 detail::DrawCostHistogram 注释）。
inline detail::DrawCostHistogram &draw_cost_histogram(std::size_t slot)
{
    return detail::g_draw_cost[slot < detail::DrawCostHistogram::k_draw_cost_slots ? slot : 0];
}

// 直方图槽位的具名下标（埋点处**不要**写裸 0/1）。
inline constexpr std::size_t k_draw_all_slot = 0;      // 整个 draw / draw_indexed 钩子
inline constexpr std::size_t k_draw_inspect_slot = 1;  // 其中"内省窗口"（未内省记 0）

inline const char *segment_name(Segment segment)
{
    const auto index = static_cast<std::size_t>(segment);
    return index < detail::k_segment_count ? detail::k_segment_names[index] : "?";
}

// 复位全部状态（单测用；运行时不需要）。
inline void reset_for_test()
{
    for (std::size_t i = 0; i < detail::k_segment_count; ++i)
    {
        detail::g_segments[i].calls.store(0, std::memory_order_relaxed);
        detail::g_segments[i].total_ns.store(0, std::memory_order_relaxed);
        detail::g_segments[i].max_ns.store(0, std::memory_order_relaxed);
    }
    for (std::size_t i = 0; i < detail::k_counter_count; ++i)
    {
        detail::g_counters[i].count.store(0, std::memory_order_relaxed);
        detail::g_counters[i].max_us.store(0, std::memory_order_relaxed);
    }
    detail::g_frames_upscale.store(0, std::memory_order_relaxed);
    detail::g_frames_present.store(0, std::memory_order_relaxed);
    detail::g_draw_calls.store(0, std::memory_order_relaxed);
    detail::g_draw_sampled.store(0, std::memory_order_relaxed);
    detail::g_dispatch_calls.store(0, std::memory_order_relaxed);
    detail::g_dispatch_sampled.store(0, std::memory_order_relaxed);
    detail::g_jitter_calls.store(0, std::memory_order_relaxed);
    detail::g_scope_calls.store(0, std::memory_order_relaxed);
    detail::g_draw_inspect_entered.store(0, std::memory_order_relaxed);
    // B106 漏斗 + 视图读取计数
    for (std::size_t i = 0; i < detail::k_funnel_count; ++i)
        detail::g_funnel[i].store(0, std::memory_order_relaxed);
    detail::g_funnel_calls.store(0, std::memory_order_relaxed);
    detail::g_funnel_identified_total.store(0, std::memory_order_relaxed);
    detail::g_view_reads.store(0, std::memory_order_relaxed);
    // B107/B108 形状分桶表：键与计数**全清**（每区间一份 Top-N；单测要一个干净起点）。
    for (std::size_t i = 0; i < detail::DrawShapeTable::k_bucket_count; ++i)
    {
        detail::g_draw_shape_table.keys[i].store(0, std::memory_order_relaxed);
        detail::g_draw_shape_table.calls[i].store(0, std::memory_order_relaxed);
        detail::g_draw_shape_table.hits[i].store(0, std::memory_order_relaxed);
    }
    detail::g_draw_shape_table.other_calls.store(0, std::memory_order_relaxed);
    detail::g_draw_shape_table.other_hits.store(0, std::memory_order_relaxed);
    detail::g_draw_shape_table.cold_calls.store(0, std::memory_order_relaxed);
    detail::g_draw_shape_table.cold_hits.store(0, std::memory_order_relaxed);
    detail::g_draw_shape_table.evictions.store(0, std::memory_order_relaxed);
    detail::g_draw_shape_table.total_calls.store(0, std::memory_order_relaxed);
    detail::g_draw_shape_table.total_hits.store(0, std::memory_order_relaxed);
    detail::g_shape_entries.store(0, std::memory_order_relaxed);
    detail::g_shape_mark_total.store(0, std::memory_order_relaxed);
    // B108 元素数直方图
    for (std::size_t group = 0; group < DrawElementHistogram::k_group_count; ++group)
    {
        for (std::size_t bucket = 0; bucket < DrawElementHistogram::k_bucket_count; ++bucket)
        {
            detail::g_draw_element_histogram.calls[group][bucket].store(0, std::memory_order_relaxed);
            detail::g_draw_element_histogram.hits[group][bucket].store(0, std::memory_order_relaxed);
        }
    }
    detail::g_draw_element_histogram.total_calls.store(0, std::memory_order_relaxed);
    detail::g_draw_element_histogram.total_hits.store(0, std::memory_order_relaxed);
    // B109 入口过滤器：**配置与自我证伪闩锁也复位**（单测要一个干净的起点；
    // `reset_for_test` 的语义就是"回到刚加载、还没读 ini 的状态"）。
    detail::g_entry_filter_configured.store(false, std::memory_order_relaxed);
    detail::g_entry_filter_disabled.store(false, std::memory_order_relaxed);
    detail::g_entry_filter_canary_stride.store(64, std::memory_order_relaxed);
    detail::g_entry_filter_sequence.store(0, std::memory_order_relaxed);
    detail::g_entry_filter_skipped.store(0, std::memory_order_relaxed);
    detail::g_entry_filter_canary.store(0, std::memory_order_relaxed);
    detail::g_entry_filter_non3_identified.store(0, std::memory_order_relaxed);
    detail::g_entry_filter_disable_element_count.store(0, std::memory_order_relaxed);
    detail::g_entry_filter_disable_entry.store(static_cast<std::uint32_t>(DrawEntryKind::unknown),
                                               std::memory_order_relaxed);
    detail::g_entry_filter_disable_at_ms.store(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < detail::DrawShapeMark::k_observed_capacity; ++i)
    {
        detail::g_draw_shape_mark.observed_keys[i].store(0, std::memory_order_relaxed);
        detail::g_draw_shape_mark.observed_calls[i].store(0, std::memory_order_relaxed);
    }
    detail::g_last_check_ms.store(0, std::memory_order_relaxed);
    detail::g_flushing.store(false, std::memory_order_relaxed);
    for (std::size_t i = 0; i < detail::DrawCostHistogram::k_draw_cost_slots; ++i)
        detail::g_draw_cost[i].reset();
    detail::g_enabled.store(false, std::memory_order_relaxed);
}

// 抽样计数的两个引用（供 SampledScope 用；放在最后以便阅读顺序自然）。
inline std::atomic_uint64_t &draw_call_counter()
{
    return detail::g_draw_calls;
}

inline std::atomic_uint64_t &draw_sample_counter()
{
    return detail::g_draw_sampled;
}

inline std::atomic_uint64_t &dispatch_call_counter()
{
    return detail::g_dispatch_calls;
}

inline std::atomic_uint64_t &dispatch_sample_counter()
{
    return detail::g_dispatch_sampled;
}

// ---- draw 钩子内省段的计时作用域（直方图下标 1）----
//
// 【口径（与读数一一对应，B105 起）】
//   下标 0 `draw_all`     = **整个钩子体**（由 `SampledScope` 在析构时落桶）；
//   下标 1 `draw_inspect` = **只包住 `inspect_target_upscaler_draw*` 调用本身**；
//                           本次 draw 没进入内省时记 **0**。
//   两槽的样本集合都由 `perf_scope.sampled()` 决定 ⇒ **恒等**
//   ⇒ `mean(draw_all) − mean(draw_inspect)` = 每次调用的**非内省部分**
//     （family gate / 探针闸门 / **原始 draw 调用**）。
//
// 【为什么不能像 B104 那样把作用域摆在函数开头】
//   那样量到的是整个钩子体（含 `g_original_draw_indexed` —— 游戏自己的绘制提交），
//   与 `draw_all` 同口径 ⇒ 两数相减恒 ≈0，"1.1 ms 里有多少是内省"**结构上无法回答**
//   （真机上 `draw_inspect ≈ drawhook 均值` 正是这么来的）。
//
// 【为什么用 start()/stop() 而不是纯 RAII】
//   "本次没有内省"本身也是要上报的读数（记 0），所以生命周期必须横跨整条函数；
//   纯 RAII 只能覆盖"进过内省"的那部分，样本集合就会与 slot 0 不一致。
class DrawInspectScope
{
public:
    explicit DrawInspectScope(bool active)
        : m_active(active)
    {
    }

    // 内省窗口开始 / 结束。**只在真的调用内省的那一行前后成对调用**；
    // 提前 return 的路径（family gate / 翻译接管）不调用 ⇒ 析构时记 0。
    void start()
    {
        if (!m_active || m_started)
            return;
        m_started = true;
        m_start = detail::qpc_ticks();
        detail::g_scope_calls.fetch_add(1, std::memory_order_relaxed);
        detail::g_draw_inspect_entered.fetch_add(1, std::memory_order_relaxed);
    }

    void stop()
    {
        if (!m_active || !m_started || m_ended)
            return;
        m_ended = true;
        const std::int64_t now = detail::qpc_ticks();
        detail::g_draw_cost[1].record(now > m_start ? detail::ticks_to_ns(now - m_start) : 0);
    }

    ~DrawInspectScope()
    {
        if (!m_active)
            return;
        if (!m_started)
        {
            // 本次 draw 没有进入内省 ⇒ 如实记 0（保证与 slot 0 的样本集合一致）。
            detail::g_draw_cost[1].record(0);
            return;
        }
        stop();  // 兜底：万一调用方忘了 stop()，不要把这一份样本丢掉
    }

    DrawInspectScope(const DrawInspectScope &) = delete;
    DrawInspectScope &operator=(const DrawInspectScope &) = delete;

private:
    bool m_active = false;
    bool m_started = false;
    bool m_ended = false;
    std::int64_t m_start = 0;
};

#endif // DX11FSRBRIDGE_ENABLE_DIAGNOSTICS —— 尾部诊断 API（漏斗 / 计数包装 / 汇总 / 直方图）

} // namespace perf_probe
