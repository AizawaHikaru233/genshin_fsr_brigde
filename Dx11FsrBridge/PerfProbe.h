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
//   - 日志：**每 `PerfProbeIntervalMs`（默认 1000 ms）一行**，不刷屏。
//   - 探针自身开销估算也打进那一行（`self_est_us/frame`），让读数可以自我核对。
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
    SampledScope(Segment segment, std::atomic_uint64_t &call_counter,
                 std::atomic_uint64_t &sample_counter, std::uint32_t stride)
        : m_segment(segment)
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
            detail::add_ns(m_segment, detail::ticks_to_ns(now - m_start));
    }

    SampledScope(const SampledScope &) = delete;
    SampledScope &operator=(const SampledScope &) = delete;

private:
    Segment m_segment;
    std::int64_t m_start = 0;
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
    detail::g_last_check_ms.store(0, std::memory_order_relaxed);
    detail::g_flushing.store(false, std::memory_order_relaxed);
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

} // namespace perf_probe
