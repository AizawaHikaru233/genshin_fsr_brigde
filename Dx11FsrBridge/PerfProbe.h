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
//   - 探针自身开销估算也打进那一行（`self_est_us/frame`），让读数可以自我核对。
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
//
// 【与日志的关系】本模块刻意不依赖 BridgeLogger：日志行通过 `set_log_sink` 注入
//   （与 TransparentJitterHook 同一纪律），这样单测只需本头文件 + Windows API。

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace perf_probe
{

// 日志出口（由调用方注入；为空则只统计不输出）。line 以 '\0' 结尾。
using LogSink = void (*)(const char *line);

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

inline SegmentStat g_segments[k_segment_count];
inline CounterStat g_counters[k_counter_count];

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

// 下标 0 = 钩子整体（**由 SampledScope 在析构时落桶**）；下标 1 = 其中"内省"
// （`inspect_target_upscaler_draw*`）那一段（由 DrawInspectScope 的窗口计时落桶）。
inline DrawCostHistogram g_draw_cost[DrawCostHistogram::k_draw_cost_slots];

// 本区间内**真正进入内省**的抽样次数（slot 1 里"非 0 样本"的个数）。
// 用途：DrawInspectScope 会把"没进入内省"的 draw 记为 0 ⇒ 只看 slot 1 的
// `<1us` 桶无法区分"内省很快"与"根本没内省"；这个数把两者分开。
inline std::atomic_uint64_t g_draw_inspect_entered { 0 };

} // namespace detail

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
        char line[1024] {};
        const double fps = elapsed_ms > 0.0 ? static_cast<double>(frames) * 1000.0 / elapsed_ms : 0.0;
        std::snprintf(line, sizeof(line),
            "perf_probe ms=%.0f fps=%.1f frames=%llu ups=%llu present=%llu draws=%llu"
            " budget_us=%.1f acct_us=%.1f unacc_us=%.1f self_est_us=%.1f"
            " | per_frame_us present=%.1f il2cpp=%.1f drawhook=%.1f disphook=%.1f jitter=%.1f gpuq=%.1f cfgio=%.1f rscale=%.1f"
            " | nested_us ups=%.1f prep=%.1f signal=%.1f w12=%.1f submit=%.1f finw=%.1f finc=%.1f"
            " | max_us drawhook=%.1f disphook=%.1f ups=%.1f prep=%.1f signal=%.1f w12=%.1f submit=%.1f finw=%.1f finc=%.1f"
            " | n draw_samp=%llu disp_samp=%llu jitter=%llu draw_pf=%.1f"
            " | cnt vprotect=%llu flushic=%llu sleep=%llu waitobj=%llu cfgread=%llu fileprobe=%llu wso_max_us=%llu",
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
            static_cast<unsigned long long>(counter_max_us[static_cast<std::size_t>(Counter::wait_single_object)]));
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

} // namespace perf_probe
