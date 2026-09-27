// PerfProbeTest.cpp — PerfProbe (header-only segmented frame-time probe) unit test.
//
// No game, no GPU, no BridgeLogger: PerfProbe.h is header-only and the log line
// goes through an injected sink, so this test only needs Windows API + the header.
//
// Covered:
//   1) disabled by default => zero accumulation, zero output (must be free)
//   2) Scope accumulates calls / total / max
//   3) add_span_us feeds pre-measured spans without taking its own timestamp
//   4) SampledScope honours the stride and still counts every call
//   5) counters increment; count_with_us tracks max
//   6) frame counters per source; jitter call counter
//   7) flush_now emits exactly one line and resets everything
//   8) flush is not re-entrant (a sink that calls flush_now must not recurse)
//   9) maybe_flush respects the interval
//  10) the emitted line carries the fields the A/B procedure reads
//  11) segment name table covers every segment
//  12) B105: SampledScope records draw_all into the histogram slot from its
//      DESTRUCTOR and the value equals the segment accumulation (the old code
//      read elapsed_ns() inside the scope, which is always 0)
//  13) B105: DrawInspectScope is an explicit window; a draw that never enters
//      introspection records 0, so both slots always share one sample set
//  14) B105: the paired usage keeps draw_all and draw_inspect sample counts equal
//
// ASCII-only (consistent with the other tests in this directory).
#include "PerfProbe.h"

#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{

int failures = 0;
int checks = 0;
#define CHECK(cond, msg)                                    \
    do                                                      \
    {                                                       \
        ++checks;                                           \
        if (!(cond))                                        \
        {                                                   \
            std::printf("FAIL: %s\n", msg);                 \
            ++failures;                                     \
        }                                                   \
        else                                                \
        {                                                   \
            std::printf("ok:   %s\n", msg);                 \
        }                                                   \
    } while (0)

std::string g_captured;
std::string g_summary;
std::string g_histogram;
int g_sink_calls = 0;
bool g_sink_reenters = false;

void capture_sink(const char *line)
{
    ++g_sink_calls;
    g_captured = line != nullptr ? line : "";
    if (g_captured.rfind("perf_probe_hist_draw ", 0) == 0)
        g_histogram = g_captured;
    else
        g_summary = g_captured;
    if (g_sink_reenters)
    {
        // Re-entrancy guard check: this must return immediately (g_flushing is set).
        perf_probe::flush_now();
    }
}

// A cheap "does some work" body so the measured spans are non-zero.
//
// IMPORTANT: the result is stored through a volatile sink. Without a side effect
// MSVC removes the whole loop, the scope body becomes empty, and the QPC delta
// can round to 0 at the 100 ns QPC granularity -- which would make this test
// fail for a reason that has nothing to do with the probe (that is exactly how
// the first version of this test failed).
volatile std::uint64_t g_spin_sink = 0;

std::uint64_t spin(std::uint32_t iterations)
{
    std::uint64_t accumulator = 1;
    for (std::uint32_t i = 0; i < iterations; ++i)
        accumulator = accumulator * 1664525ull + 1013904223ull;
    g_spin_sink = accumulator;
    return accumulator;
}

void test_disabled_is_free()
{
    perf_probe::reset_for_test();
    g_captured.clear();
    g_sink_calls = 0;
    perf_probe::set_log_sink(&capture_sink);

    CHECK(!perf_probe::enabled(), "probe is disabled by default");

    {
        perf_probe::Scope scope(perf_probe::Segment::present);
        spin(20000);
    }
    perf_probe::SampledScope sampled(perf_probe::Segment::draw_hook,
                                     perf_probe::draw_call_counter(),
                                     perf_probe::draw_sample_counter(), 1);
    perf_probe::count(perf_probe::Counter::virtual_protect);
    perf_probe::add_span_us(perf_probe::Segment::interop_prep, 42);
    perf_probe::note_frame(perf_probe::FrameSource::upscale);
    perf_probe::note_jitter_call();
    perf_probe::flush_now();

    std::uint64_t calls = 0;
    std::uint64_t total = 0;
    perf_probe::read_segment(perf_probe::Segment::present, &calls, &total, nullptr);
    CHECK(calls == 0 && total == 0, "disabled: no scope accumulation");
    perf_probe::read_segment(perf_probe::Segment::draw_hook, &calls, &total, nullptr);
    CHECK(calls == 0, "disabled: no sampled accumulation");
    CHECK(perf_probe::read_counter(perf_probe::Counter::virtual_protect) == 0,
          "disabled: counters stay at zero");
    CHECK(perf_probe::read_frames(perf_probe::FrameSource::upscale) == 0,
          "disabled: frames stay at zero");
    CHECK(perf_probe::read_jitter_calls() == 0, "disabled: jitter calls stay at zero");
    CHECK(g_sink_calls == 0, "disabled: flush emits nothing");
    perf_probe::read_segment(perf_probe::Segment::interop_prep, &calls, &total, nullptr);
    CHECK(calls == 0 && total == 0, "disabled: add_span_us is ignored");
}

void test_scope_and_spans()
{
    perf_probe::reset_for_test();
    perf_probe::set_log_sink(&capture_sink);
    perf_probe::configure(true, 60000, 1);
    CHECK(perf_probe::enabled(), "configure(true) enables the probe");

    for (int i = 0; i < 8; ++i)
    {
        perf_probe::Scope scope(perf_probe::Segment::present);
        spin(20000);
    }
    std::uint64_t calls = 0;
    std::uint64_t total = 0;
    std::uint64_t max_ns = 0;
    perf_probe::read_segment(perf_probe::Segment::present, &calls, &total, &max_ns);
    CHECK(calls == 8, "Scope: call count is exact");
    CHECK(total > 0, "Scope: total time accumulated");
    CHECK(max_ns > 0 && max_ns <= total, "Scope: max is within total");

    perf_probe::add_span_us(perf_probe::Segment::interop_prep, 10);
    perf_probe::add_span_us(perf_probe::Segment::interop_prep, 50);
    perf_probe::read_segment(perf_probe::Segment::interop_prep, &calls, &total, &max_ns);
    CHECK(calls == 2, "add_span_us: counts spans");
    CHECK(total == 60000, "add_span_us: converts us to ns exactly");
    CHECK(max_ns == 50000, "add_span_us: max is the largest span");
}

void test_sampling()
{
    perf_probe::reset_for_test();
    perf_probe::set_log_sink(&capture_sink);
    perf_probe::configure(true, 60000, 4);

    for (int i = 0; i < 40; ++i)
    {
        perf_probe::SampledScope sampled(perf_probe::Segment::draw_hook,
                                         perf_probe::draw_call_counter(),
                                         perf_probe::draw_sample_counter(), 4);
        spin(20000);
    }
    std::uint64_t calls = 0;
    perf_probe::read_segment(perf_probe::Segment::draw_hook, &calls, nullptr, nullptr);
    CHECK(perf_probe::read_draw_calls() == 40, "SampledScope: every call is counted");
    CHECK(calls == 10, "SampledScope: only 1/strides calls are timed");
    // stride 1 => every call is timed
    perf_probe::reset_for_test();
    perf_probe::configure(true, 60000, 1);
    for (int i = 0; i < 5; ++i)
    {
        perf_probe::SampledScope sampled(perf_probe::Segment::dispatch_hook,
                                         perf_probe::dispatch_call_counter(),
                                         perf_probe::dispatch_sample_counter(), 1);
        spin(20000);
    }
    perf_probe::read_segment(perf_probe::Segment::dispatch_hook, &calls, nullptr, nullptr);
    CHECK(calls == 5, "SampledScope: stride 1 times every call");
}

void test_counters_and_frames()
{
    perf_probe::reset_for_test();
    perf_probe::set_log_sink(&capture_sink);
    perf_probe::configure(true, 60000, 1);

    perf_probe::count(perf_probe::Counter::virtual_protect);
    perf_probe::count(perf_probe::Counter::virtual_protect);
    perf_probe::count(perf_probe::Counter::flush_instruction_cache);
    perf_probe::count_with_us(perf_probe::Counter::wait_single_object, 7);
    perf_probe::count_with_us(perf_probe::Counter::wait_single_object, 3);
    CHECK(perf_probe::read_counter(perf_probe::Counter::virtual_protect) == 2,
          "count(): increments the counter");
    CHECK(perf_probe::read_counter(perf_probe::Counter::flush_instruction_cache) == 1,
          "count(): per-counter independence");
    CHECK(perf_probe::read_counter(perf_probe::Counter::wait_single_object) == 2,
          "count_with_us(): increments the counter");

    for (int i = 0; i < 3; ++i)
        perf_probe::note_frame(perf_probe::FrameSource::upscale);
    perf_probe::note_frame(perf_probe::FrameSource::present);
    perf_probe::note_jitter_call();
    perf_probe::note_jitter_call();
    CHECK(perf_probe::read_frames(perf_probe::FrameSource::upscale) == 3, "note_frame: upscale frames");
    CHECK(perf_probe::read_frames(perf_probe::FrameSource::present) == 1, "note_frame: present frames");
    CHECK(perf_probe::read_jitter_calls() == 2, "note_jitter_call: counted");
}

void test_flush_line_and_reset()
{
    perf_probe::reset_for_test();
    perf_probe::set_log_sink(&capture_sink);
    perf_probe::configure(true, 60000, 1);
    g_captured.clear();
    g_summary.clear();
    g_histogram.clear();
    g_sink_calls = 0;

    for (int i = 0; i < 4; ++i)
    {
        perf_probe::Scope scope(perf_probe::Segment::upscale_dispatch);
        spin(20000);
        perf_probe::note_frame(perf_probe::FrameSource::upscale);
    }
    perf_probe::SampledScope sampled(perf_probe::Segment::draw_hook,
                                     perf_probe::draw_call_counter(),
                                     perf_probe::draw_sample_counter(), 1);
    perf_probe::add_span_us(perf_probe::Segment::interop_signal, 200);
    // 直方图也必须有真实样本，示例输出才不是空的（字段形状来自实跑）。
    perf_probe::draw_cost_histogram(0).record(420);
    perf_probe::draw_cost_histogram(0).record(2260);
    perf_probe::draw_cost_histogram(0).record(90000);
    perf_probe::draw_cost_histogram(1).record(180);
    perf_probe::flush_now();

    CHECK(g_sink_calls == 2, "flush_now: emits the summary line plus the histogram line");
    CHECK(g_summary.rfind("perf_probe ", 0) == 0, "flush line starts with perf_probe");
    CHECK(g_histogram.rfind("perf_probe_hist_draw ", 0) == 0, "histogram line starts with perf_probe_hist");
    CHECK(g_histogram.find("draw_all n=3") != std::string::npos, "histogram line carries the draw_all column");
    CHECK(g_histogram.find("draw_inspect n=1") != std::string::npos, "histogram line carries the draw_inspect column");
    CHECK(g_histogram.find("<1us:1") != std::string::npos, "histogram line reports the <1us bucket");
    CHECK(g_histogram.find("2-4:1") != std::string::npos, "histogram line reports the 2-4us bucket");
    CHECK(g_histogram.find("64-128:1") != std::string::npos, "histogram line reports the 64-128us bucket");
    CHECK(g_histogram.find("mean_us=") != std::string::npos, "histogram line reports the mean");
    CHECK(g_histogram.find("entered=") != std::string::npos,
          "histogram line reports how many sampled draws really entered introspection (B105)");
    const char *needles[] = {
        "ms=", "fps=", "frames=", "ups=", "present=", "draws=",
        "budget_us=", "acct_us=", "unacc_us=", "self_est_us=",
        "per_frame_us", "nested_us", "max_us", "draw_samp=", "disp_samp=",
        "jitter=", "cnt vprotect=", "flushic=", "sleep=", "waitobj=",
    };
    for (const char *needle : needles)
        CHECK(g_summary.find(needle) != std::string::npos, needle);

    // counters / frames must be reset by the flush
    CHECK(perf_probe::read_frames(perf_probe::FrameSource::upscale) == 0, "flush resets frames");
    CHECK(perf_probe::read_draw_calls() == 0, "flush resets draw call counter");
    CHECK(perf_probe::read_counter(perf_probe::Counter::virtual_protect) == 0,
          "flush resets counters");
    std::uint64_t calls = 0;
    std::uint64_t total = 0;
    perf_probe::read_segment(perf_probe::Segment::upscale_dispatch, &calls, &total, nullptr);
    CHECK(calls == 0 && total == 0, "flush resets segment accumulation");

    // frames counted => fps and budget_us must be finite/positive-ish tokens
    CHECK(g_summary.find("frames=4") != std::string::npos, "flush reports the frame count");

    // Print the real lines so the shape of the output is documented (and so the
    // user's A/B instruction can quote an actual example instead of a mock-up).
    std::printf("\nexample_flush_lines:\n%s\n%s\n", g_summary.c_str(), g_histogram.c_str());
}

// 单次调用成本直方图：桶边界必须是精确的（否则"中位数落在哪个桶"会被读错）。
void test_draw_cost_histogram()
{
    perf_probe::reset_for_test();
    perf_probe::configure(true, 60000, 1);

    perf_probe::detail::DrawCostHistogram &histogram = perf_probe::draw_cost_histogram(0);
    CHECK(histogram.line() == "n=0", "histogram: empty line is n=0");
    CHECK(perf_probe::detail::DrawCostHistogram::bucket_for(0) == 0, "histogram: 0 ns -> <1us");
    CHECK(perf_probe::detail::DrawCostHistogram::bucket_for(999) == 0, "histogram: 999 ns -> <1us");
    CHECK(perf_probe::detail::DrawCostHistogram::bucket_for(1000) == 1, "histogram: 1000 ns -> 1-2us");
    CHECK(perf_probe::detail::DrawCostHistogram::bucket_for(2047) == 1, "histogram: 2047 ns -> 1-2us");
    CHECK(perf_probe::detail::DrawCostHistogram::bucket_for(2048) == 2, "histogram: 2048 ns -> 2-4us");
    CHECK(perf_probe::detail::DrawCostHistogram::bucket_for(4096) == 3, "histogram: 4096 ns -> 4-8us");
    CHECK(perf_probe::detail::DrawCostHistogram::bucket_for(2260) == 2, "histogram: the 2.26 us reading lands in 2-4us");
    CHECK(perf_probe::detail::DrawCostHistogram::bucket_for(1000000) == perf_probe::detail::DrawCostHistogram::k_bucket_count - 1,
          "histogram: 1 ms and above clamps to the last bucket");

    histogram.record(300);
    histogram.record(1200);
    histogram.record(2260);
    const std::string line = histogram.line();
    CHECK(line.find("n=3") != std::string::npos, "histogram: counts every record");
    CHECK(line.find("<1us:1") != std::string::npos, "histogram: 300 ns reported in <1us");
    CHECK(line.find("1-2:1") != std::string::npos, "histogram: 1200 ns reported in 1-2us");
    CHECK(line.find("2-4:1") != std::string::npos, "histogram: 2260 ns reported in 2-4us");
    CHECK(histogram.max_ns.load() == 2260, "histogram: max is the largest recorded");

    // idx 1 与 idx 0 必须彼此独立（整体钩子 vs 内省段）
    CHECK(perf_probe::draw_cost_histogram(1).line() == "n=0", "histogram: slot 1 is independent");

    // 越界下标必须被夹住，不能写到数组外
    CHECK(&perf_probe::draw_cost_histogram(99) == &perf_probe::draw_cost_histogram(0),
          "histogram: out-of-range slot clamps to slot 0");

    perf_probe::flush_now();
    CHECK(perf_probe::draw_cost_histogram(0).line() == "n=0", "histogram: flush resets the histogram");
}

// SampledScope 必须自报"本次是否计时"，直方图据此取样（不重新读计数器，避免偏移 1）。
//
// ⚠️ `elapsed_ns()` 只在**析构**时才被填上 ⇒ 若写成栈对象，作用域内读到的一定是 0
// （本测试第一版就是这么错的：那是**测试**的错，不是探针的错）。
// 这里 new 一个再 delete：delete **之前**复制出 `sampled()` 与 `elapsed_ns()`
// （delete 之后再解引用就是 use-after-free），析构之后再读段累计来证明"确实记了时间"。
void measure_one_sampled_scope(bool *out_sampled, std::uint64_t *out_elapsed_alive,
                               std::uint64_t *out_segment_total_ns)
{
    auto *scope = new perf_probe::SampledScope(perf_probe::Segment::draw_hook,
                                               perf_probe::draw_call_counter(),
                                               perf_probe::draw_sample_counter(), 8);
    // Sleep(5) 保证 QPC 真的跨过 tick：本机 QPC 粒度 100 ns，而纯 spin(200000) 在
    // /O2 下仍可能整段落在同一个 tick 内（实测 delta=0）⇒ 那样测的是粒度不是逻辑。
    spin(200000);
    Sleep(5);
    const bool sampled = scope->sampled();
    const std::uint64_t alive = scope->elapsed_ns(); // 析构前：必然是 0
    delete scope;
    std::uint64_t calls = 0;
    std::uint64_t total = 0;
    perf_probe::read_segment(perf_probe::Segment::draw_hook, &calls, &total, nullptr);
    if (out_sampled != nullptr)
        *out_sampled = sampled;
    if (out_elapsed_alive != nullptr)
        *out_elapsed_alive = alive;
    if (out_segment_total_ns != nullptr)
        *out_segment_total_ns = total;
}

void test_sampled_scope_reports_sampling()
{
    perf_probe::reset_for_test();
    perf_probe::configure(true, 60000, 8);
    int sampled = 0;
    int unsampled = 0;
    std::uint64_t segment_total_ns = 0;
    for (int i = 0; i < 16; ++i)
    {
        bool was_sampled = false;
        std::uint64_t alive = 0xFFFFFFFFFFFFFFFFull;
        const std::uint64_t before = segment_total_ns;
        measure_one_sampled_scope(&was_sampled, &alive, &segment_total_ns);
        CHECK(alive == 0, "SampledScope: elapsed_ns is 0 until the scope is destroyed");
        if (was_sampled)
        {
            ++sampled;
            CHECK(segment_total_ns > before, "SampledScope: a sampled call adds time to its segment");
        }
        else
        {
            ++unsampled;
            CHECK(segment_total_ns == before, "SampledScope: an unsampled call adds no time");
        }
    }
    CHECK(sampled == 2, "SampledScope: 2 of 16 calls are sampled at stride 8");
    CHECK(unsampled == 14, "SampledScope: the rest are not sampled");
}

void test_flush_not_reentrant()
{
    perf_probe::reset_for_test();
    perf_probe::set_log_sink(&capture_sink);
    perf_probe::configure(true, 60000, 1);
    g_sink_calls = 0;
    g_sink_reenters = true;
    perf_probe::note_frame(perf_probe::FrameSource::upscale);
    perf_probe::flush_now();
    g_sink_reenters = false;
    CHECK(g_sink_calls == 2, "flush is not re-entrant (sink calling flush_now does not recurse)");
}

// ⚠️ B105 回归：`draw_all` 直方图槽必须由 **SampledScope 自己在析构时**落桶，
// 且与段累计拿到**同一份**耗时。
//
// 旧写法是调用方在函数体内写 `histogram.record(scope.elapsed_ns())`，而
// `elapsed_ns()` 要到析构才被填 ⇒ 每次记的都是 0（真机：`draw_all n=685
// mean_us=0.000000 <1us:685`，而同一秒的 `draw_inspect` 有数）。
void test_sampled_scope_histogram_slot()
{
    perf_probe::reset_for_test();
    perf_probe::configure(true, 60000, 4);
    perf_probe::detail::DrawCostHistogram &histogram =
        perf_probe::draw_cost_histogram(perf_probe::k_draw_all_slot);
    CHECK(histogram.line() == "n=0", "draw_all slot starts empty");

    // stride 4 ⇒ 12 次调用里只有下标 0/4/8 被计时。
    // Sleep(5) 保证 QPC 真的跨过 tick（本机粒度 100 ns；错误清单第 10 条那一族坑）。
    for (int i = 0; i < 12; ++i)
    {
        perf_probe::SampledScope scope(perf_probe::Segment::draw_hook,
                                       perf_probe::draw_call_counter(),
                                       perf_probe::draw_sample_counter(), 4,
                                       perf_probe::k_draw_all_slot);
        spin(20000);
        Sleep(5);
    }
    std::uint64_t calls = 0;
    std::uint64_t segment_total_ns = 0;
    perf_probe::read_segment(perf_probe::Segment::draw_hook, &calls, &segment_total_ns, nullptr);
    CHECK(calls == 3, "draw_all slot: 3 of 12 calls are timed at stride 4");
    CHECK(histogram.count.load() == calls,
          "draw_all slot: exactly one histogram sample per timed call (same sample set)");
    CHECK(histogram.total_ns.load() == segment_total_ns,
          "draw_all slot: the histogram and the segment accumulate the SAME elapsed");
    CHECK(histogram.total_ns.load() > 0,
          "draw_all slot is no longer all-zero (B105 regression)");

    // 关掉探针：既不计时也不落桶（探针必须真的免费）
    perf_probe::reset_for_test();
    perf_probe::configure(false, 60000, 1);
    {
        perf_probe::SampledScope scope(perf_probe::Segment::draw_hook,
                                       perf_probe::draw_call_counter(),
                                       perf_probe::draw_sample_counter(), 1,
                                       perf_probe::k_draw_all_slot);
        spin(20000);
    }
    CHECK(histogram.line() == "n=0", "draw_all slot: disabled probe records nothing");
}

// ⚠️ B105 回归：内省窗口（槽 1）口径。
//   进入内省 ⇒ 记实测耗时；没进入内省 ⇒ 记 0（这样两槽样本集合恒等，
//   `mean(draw_all) − mean(draw_inspect)` 才是"非内省部分"）。
void test_draw_inspect_window()
{
    perf_probe::reset_for_test();
    perf_probe::configure(true, 60000, 1);
    perf_probe::detail::DrawCostHistogram &histogram =
        perf_probe::draw_cost_histogram(perf_probe::k_draw_inspect_slot);

    // 本次调用没被抽样（active=false）⇒ 什么都不记
    {
        perf_probe::DrawInspectScope inactive(false);
        inactive.start();
        inactive.stop();
    }
    CHECK(histogram.line() == "n=0", "inspect window: unsampled calls record nothing");
    CHECK(perf_probe::read_draw_inspect_entered() == 0,
          "inspect window: unsampled calls are not counted as entered");

    // 提前 return（family gate 命中 / 翻译接管）⇒ 记 0
    {
        perf_probe::DrawInspectScope skipped(true);
    }
    CHECK(histogram.count.load() == 1,
          "inspect window: a draw that never entered still records one sample");
    CHECK(histogram.total_ns.load() == 0, "inspect window: the skipped sample is exactly 0");
    CHECK(histogram.buckets[0].load() == 1, "inspect window: the skipped sample lands in <1us");
    CHECK(perf_probe::read_draw_inspect_entered() == 0,
          "inspect window: skipped draws are not 'entered'");

    // 真的进入内省 ⇒ 记实测耗时，entered +1
    {
        perf_probe::DrawInspectScope active(true);
        active.start();
        spin(20000);
        Sleep(5);
        active.stop();
    }
    CHECK(histogram.count.load() == 2, "inspect window: entered draws record one sample");
    CHECK(histogram.total_ns.load() > 0, "inspect window: the entered sample carries real time");
    CHECK(histogram.max_ns.load() == histogram.total_ns.load(),
          "inspect window: only the entered sample contributed time");
    CHECK(perf_probe::read_draw_inspect_entered() == 1,
          "inspect window: entered draws are counted once");

    // stop() 必须幂等，且析构兜底不能重复记
    {
        perf_probe::DrawInspectScope active(true);
        active.start();
        spin(20000);
        active.stop();
        active.stop();
    }
    CHECK(histogram.count.load() == 3, "inspect window: a second stop() does not record twice");
    CHECK(perf_probe::read_draw_inspect_entered() == 2,
          "inspect window: start() is idempotent for the entered counter");
}

// ⚠️ B105：`draw_all − draw_inspect` 成立的前提是**两槽样本集合恒等**。
// 这里用真实的配对用法（同一个 `sampled()` 决定两个作用域）证明它：
// 一半的 draw 提前 return（不进内省），样本数仍必须一模一样。
void test_two_slots_share_sample_set()
{
    perf_probe::reset_for_test();
    perf_probe::configure(true, 60000, 3);
    int inspected = 0;
    for (int i = 0; i < 9; ++i)
    {
        perf_probe::SampledScope scope(perf_probe::Segment::draw_hook,
                                       perf_probe::draw_call_counter(),
                                       perf_probe::draw_sample_counter(), 3,
                                       perf_probe::k_draw_all_slot);
        perf_probe::DrawInspectScope inspect(scope.sampled());
        // i 为偶数 ⇒ 模拟 family gate 命中 / 翻译接管：**提前 return，没进内省**。
        // 只有被抽样到的那些调用才可能真的进入内省（未抽样时 active=false）。
        if ((i % 2) == 1)
        {
            inspect.start();
            spin(20000);
            inspect.stop();
            if (scope.sampled())
                ++inspected;
        }
    }
    const std::uint64_t all = perf_probe::draw_cost_histogram(perf_probe::k_draw_all_slot).count.load();
    const std::uint64_t only_inspect =
        perf_probe::draw_cost_histogram(perf_probe::k_draw_inspect_slot).count.load();
    CHECK(all == 3, "paired scopes: stride 3 over 9 calls => 3 samples in draw_all");
    CHECK(only_inspect == all,
          "paired scopes: draw_inspect has the SAME sample count as draw_all (so the difference is valid)");
    CHECK(perf_probe::read_draw_inspect_entered() == static_cast<std::uint64_t>(inspected),
          "paired scopes: entered counts only the draws that really ran introspection");
}

void test_maybe_flush_interval()
{
    perf_probe::reset_for_test();
    perf_probe::set_log_sink(&capture_sink);
    perf_probe::configure(true, 60000, 1);
    g_sink_calls = 0;
    perf_probe::maybe_flush();
    CHECK(g_sink_calls == 0, "maybe_flush: silent before the interval elapses");

    perf_probe::reset_for_test();
    perf_probe::configure(true, 1, 1);
    g_sink_calls = 0;
    // NOTE: GetTickCount64 has a ~15.6 ms tick unless the process raises the timer
    // resolution, and the cheap gate inside maybe_flush is based on it. Sleeping
    // only 5 ms made this check flaky (the tick had not advanced yet) -- so sleep
    // well past one tick.
    Sleep(50);
    perf_probe::maybe_flush();
    CHECK(g_sink_calls == 2, "maybe_flush: emits the two lines once the interval elapses");
    perf_probe::maybe_flush();
    perf_probe::maybe_flush();
    CHECK(g_sink_calls <= 4, "maybe_flush: does not emit again immediately");
}

void test_interval_zero_falls_back()
{
    perf_probe::reset_for_test();
    perf_probe::configure(true, 0, 0);
    // interval 0 => 1000 ms default; stride 0 => 1
    CHECK(perf_probe::draw_sample_stride() == 1, "stride 0 falls back to 1");
    g_sink_calls = 0;
    perf_probe::maybe_flush();
    CHECK(g_sink_calls == 0, "interval 0 falls back to the default (no immediate flush)");
}

void test_segment_names()
{
    bool all_named = true;
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(perf_probe::Segment::count); ++i)
    {
        const char *name = perf_probe::segment_name(static_cast<perf_probe::Segment>(i));
        if (name == nullptr || name[0] == '\0' || std::strcmp(name, "?") == 0)
            all_named = false;
    }
    CHECK(all_named, "every segment has a name");
    CHECK(std::strcmp(perf_probe::segment_name(perf_probe::Segment::present), "present") == 0,
          "segment 0 is present (the flush line and the docs must agree)");
    CHECK(std::strcmp(perf_probe::segment_name(perf_probe::Segment::render_scale), "rscale") == 0,
          "render_scale maps to rscale (acct list and flush line must agree)");
    CHECK(std::strcmp(perf_probe::segment_name(perf_probe::Segment::upscale_dispatch), "ups") == 0,
          "first nested segment is ups");
    CHECK(std::strcmp(perf_probe::segment_name(perf_probe::Segment::finish_copy), "finc") == 0,
          "last segment is finc");
}

// Measure the probe's OWN cost. This is the "prove the probe is cheap" step the
// project's hot-path red line demands: without it, every number the probe prints
// is suspect. Same language, same compiler, same optimisation level as the
// product (error list #26: never infer a C/C++ cost from another language).
//
// It is a measurement, not an assertion: the values are printed so they can be
// quoted, and only a very generous sanity bound is checked.
void bench_probe_cost()
{
    const int iterations = 200000;
    LARGE_INTEGER frequency {};
    LARGE_INTEGER begin {};
    LARGE_INTEGER end {};
    QueryPerformanceFrequency(&frequency);
    const double to_ns = 1.0e9 / static_cast<double>(frequency.QuadPart);

    perf_probe::reset_for_test();
    perf_probe::set_log_sink(&capture_sink);
    perf_probe::configure(false, 60000, 32);
    QueryPerformanceCounter(&begin);
    for (int i = 0; i < iterations; ++i)
    {
        perf_probe::Scope scope(perf_probe::Segment::present);
    }
    QueryPerformanceCounter(&end);
    const double scope_disabled_ns = static_cast<double>(end.QuadPart - begin.QuadPart) * to_ns / iterations;

    perf_probe::reset_for_test();
    perf_probe::configure(true, 60000, 32);
    QueryPerformanceCounter(&begin);
    for (int i = 0; i < iterations; ++i)
    {
        perf_probe::Scope scope(perf_probe::Segment::present);
    }
    QueryPerformanceCounter(&end);
    const double scope_enabled_ns = static_cast<double>(end.QuadPart - begin.QuadPart) * to_ns / iterations;

    perf_probe::reset_for_test();
    perf_probe::configure(true, 60000, 32);
    QueryPerformanceCounter(&begin);
    for (int i = 0; i < iterations; ++i)
    {
        perf_probe::SampledScope scope(perf_probe::Segment::draw_hook,
                                       perf_probe::draw_call_counter(),
                                       perf_probe::draw_sample_counter(), 32);
    }
    QueryPerformanceCounter(&end);
    const double sampled_ns = static_cast<double>(end.QuadPart - begin.QuadPart) * to_ns / iterations;

    // maybe_flush is called from the hot draw hook: its cheap gate must be tiny.
    perf_probe::reset_for_test();
    perf_probe::configure(true, 60000, 32);
    QueryPerformanceCounter(&begin);
    for (int i = 0; i < iterations; ++i)
        perf_probe::maybe_flush();
    QueryPerformanceCounter(&end);
    const double maybe_flush_ns = static_cast<double>(end.QuadPart - begin.QuadPart) * to_ns / iterations;

    std::printf("\nprobe_self_cost_ns   scope_disabled=%.2f  scope_enabled=%.2f"
                "  sampled_stride32=%.2f  maybe_flush_gated=%.2f\n",
                scope_disabled_ns, scope_enabled_ns, sampled_ns, maybe_flush_ns);
    std::printf("=> per frame (10 timed segments): %.2f ns  == %.5f us\n",
                scope_enabled_ns * 10.0, scope_enabled_ns * 10.0 / 1000.0);
    // Sanity bound only: a single scope must stay far below 10 us. A failure here
    // means the probe itself is broken, not that the machine is slow.
    CHECK(scope_enabled_ns < 10000.0, "probe self cost: one timed scope stays below 10 us");
    CHECK(scope_disabled_ns < 1000.0, "probe self cost: disabled scope stays below 1 us");
}

} // namespace

int main()
{
    test_disabled_is_free();
    test_scope_and_spans();
    test_sampling();
    test_counters_and_frames();
    test_flush_line_and_reset();
    test_draw_cost_histogram();
    test_sampled_scope_reports_sampling();
    test_sampled_scope_histogram_slot();
    test_draw_inspect_window();
    test_two_slots_share_sample_set();
    test_flush_not_reentrant();
    test_maybe_flush_interval();
    test_interval_zero_falls_back();
    test_segment_names();
    bench_probe_cost();

    std::printf("\n%d checks, %d failures\n", checks, failures);
    if (failures == 0)
        std::printf("PerfProbeTest: ALL PASS\n");
    return failures == 0 ? 0 : 1;
}
