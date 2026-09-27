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
//  15) B106: the introspection funnel (FunnelStage) is coherent -- one sampling
//      decision per call covers every stage, so adjacent levels can be subtracted
//  16) B106: the funnel is really free while the probe is off (not even a call
//      counter is touched), and note_view_read() counts work, not samples
//  17) B106: flush emits the funnel line only when something was sampled (no spam)
//      and resets the funnel state
//  18) B107: the draw-shape slot round-trips (indexed/non-indexed, negative base,
//      saturation) -- the real-machine log is read by eye against these strings
//  19) B107: the shape table counts calls and identified hits per shape and resets the
//      counts per interval; B108: the keys are per interval too (one Top-N per interval)
//  20) B107: flush emits the shape lines (accounting + calls/hits + the list of shapes
//      that were really identified) sorted by call count, and nothing when no draw
//      entered introspection; B108 adds the element histogram as a fourth line
//  21) B107: while the probe is off the shape marker allocates no atomic operation
//      at all (the shipped build must stay bit-identical to B106)
//  22) B108 (Bug 2 regression): `identified` must survive a saturated table -- the B107
//      real-machine log printed `identified=0` while the same snapshot listed the shapes
//      that were identified (the hits had fallen into a pocket nobody printed)
//  23) B108: the table keeps the *most-called* shapes (Top-N with bounded eviction) and
//      never drops a shape that was identified; calls/hits stay conserved (bucketed +
//      cold + other == total, so nothing can silently vanish)
//  24) B108: the element-count histogram is exact, capacity independent and split by
//      kind -- it is what answers "is `count==3` a necessary condition?"
//  25) B108: the self-consistency assertion -- the shape row's `identified`, the shape
//      layer's mark count and the funnel layer's `identified_total` must agree; when they
//      do not, flush prints `perf_probe_draw_shape_mismatch` instead of a wrong number
//  26) B109: the entry filter admits exactly one element count (3) and rejects every other
//      one -- the criterion is the call argument only, and while the filter is off it
//      counts nothing at all (zero atomics, original path)
//  27) B109: the canary sampling stride admits 1 of every N non-3 draws and flags it as
//      `canary`; a `count==3` admission never consumes the canary sequence
//  28) B109 (the important one): a SINGLE non-3 identification disables the filter
//      (one-way atomic latch), emits exactly one
//      `draw_entry_filter_disabled reason=non3_identified element_count=N entry=ix` line,
//      and from then on every draw takes the ORIGINAL path -- fail-open, so the worst case
//      is the old (slower) behaviour and never a missed target
//  29) B109: with the filter configured off a non-3 identification is still recorded (the
//      criterion is falsified -- that is evidence) but nothing is disabled and no warning
//      is printed
//  30) B109: the filter state and its per-interval counters appear in the summary line
//      (`entry_filter=`) and in the funnel line (`skip=` / `canary=` / `non3_id=` /
//      `disable_ec=` / `disable_entry=`), so a missed warning line is still detectable
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
std::string g_funnel_line;
std::string g_shape_header;
std::string g_shape_list;
std::string g_shape_identified;
std::string g_shape_elements;
std::string g_shape_mismatch;
// B109: the self-falsification warning is the one line that does NOT start with
// `perf_probe` (it is emitted the moment the filter disables itself, not at flush time).
std::string g_entry_filter_disabled_line;
int g_sink_calls = 0;
bool g_sink_reenters = false;

void capture_sink(const char *line)
{
    ++g_sink_calls;
    g_captured = line != nullptr ? line : "";
    if (g_captured.rfind("draw_entry_filter_disabled ", 0) == 0)
        g_entry_filter_disabled_line = g_captured;
    else if (g_captured.rfind("perf_probe_funnel_draw ", 0) == 0)
        g_funnel_line = g_captured;
    else if (g_captured.rfind("perf_probe_hist_draw ", 0) == 0)
        g_histogram = g_captured;
    else if (g_captured.rfind("perf_probe_draw_shape_mismatch ", 0) == 0)
        g_shape_mismatch = g_captured;
    else if (g_captured.rfind("perf_probe_draw_shape_draw ", 0) == 0)
    {
        // B107/B108: four shape lines share one prefix; tell them apart by their middle token.
        if (g_captured.find("| shapes_top") != std::string::npos)
            g_shape_list = g_captured;
        else if (g_captured.find("| identified_shapes") != std::string::npos)
            g_shape_identified = g_captured;
        else if (g_captured.find("| elem_ix") != std::string::npos)
            g_shape_elements = g_captured;
        else if (g_captured.find("shadow_reject=") != std::string::npos)
            g_shape_header = g_captured;
        else
            g_summary = g_captured;
    }
    else
        g_summary = g_captured;
    if (g_sink_reenters)
    {
        // Re-entrancy guard check: this must return immediately (g_flushing is set).
        perf_probe::flush_now();
    }
}

// Clears every captured line (the shape tests flush several times).
void clear_captured()
{
    g_sink_calls = 0;
    g_captured.clear();
    g_summary.clear();
    g_histogram.clear();
    g_funnel_line.clear();
    g_shape_header.clear();
    g_shape_list.clear();
    g_shape_identified.clear();
    g_shape_elements.clear();
    g_shape_mismatch.clear();
    g_entry_filter_disabled_line.clear();
}

// Snapshot lookups used by the Top-N test: "is this shape resident, and with what count".
std::uint64_t read_snapshot_calls(const perf_probe::detail::DrawShapeTable::Snapshot &snapshot,
                                  std::uint64_t packed)
{
    for (std::size_t i = 0; i < snapshot.bucket_count; ++i)
    {
        if (snapshot.keys[i] == packed)
            return snapshot.calls[i];
    }
    return 0;
}

std::uint64_t read_snapshot_hits(const perf_probe::detail::DrawShapeTable::Snapshot &snapshot,
                                 std::uint64_t packed)
{
    for (std::size_t i = 0; i < snapshot.bucket_count; ++i)
    {
        if (snapshot.keys[i] == packed)
            return snapshot.hits[i];
    }
    return 0;
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

// ---- B106: introspection funnel --------------------------------------------------

// The funnel must be **coherent**: one sampling decision is taken when the scope is
// constructed, and every stage of that call is counted only if that decision is true.
// Otherwise adjacent levels could come from different call sets and "subtract the
// neighbours" would be meaningless.
//
// Expected values are computed with the SAME predicate the code under test uses
// (the B105 lesson: an expectation written with a different predicate tests a typo,
// not the code).
void test_funnel_sampling_is_coherent()
{
    perf_probe::reset_for_test();
    CHECK(perf_probe::read_funnel_calls() == 0, "funnel: disabled probe counts no calls");
    {
        perf_probe::FunnelScope scope;
        scope.mark(perf_probe::FunnelStage::entry);
        scope.mark(perf_probe::FunnelStage::after_om);
        perf_probe::note_view_read();
    }
    CHECK(perf_probe::read_funnel_calls() == 0,
          "funnel: disabled probe does not even touch the call counter (zero atomics)");
    CHECK(perf_probe::read_funnel(perf_probe::FunnelStage::entry) == 0,
          "funnel: disabled probe records no stage");
    CHECK(perf_probe::read_view_reads() == 0, "funnel: disabled probe records no view read");

    perf_probe::configure(true, 60000, 4);
    std::uint64_t expected_entry = 0;
    std::uint64_t expected_prescreen = 0;
    std::uint64_t expected_view_reads = 0;
    for (int i = 0; i < 12; ++i)
    {
        perf_probe::FunnelScope scope;
        scope.mark(perf_probe::FunnelStage::entry);
        if (scope.sampled())
            ++expected_entry;
        // Only some of the draws pass the second predicate (a shrinking funnel must be
        // observable, otherwise the test cannot tell "coherent" from "counts everything").
        // i%8==0 holds for i=0 and i=8, which are both sampled at stride 4 => 2 of 3.
        if ((i % 8) == 0)
        {
            scope.mark(perf_probe::FunnelStage::after_prescreen);
            if (scope.sampled())
                ++expected_prescreen;
        }
        // view reads are NOT sampled: they count absolute work.
        if ((i % 3) == 0)
        {
            perf_probe::note_view_read();
            ++expected_view_reads;
        }
    }
    CHECK(perf_probe::read_funnel_calls() == 12, "funnel: every call is counted (sampled or not)");
    CHECK(perf_probe::read_funnel(perf_probe::FunnelStage::entry) == expected_entry,
          "funnel: entry counts exactly the sampled calls");
    CHECK(expected_entry == 3, "funnel: stride 4 over 12 calls samples 3 of them");
    CHECK(perf_probe::read_funnel(perf_probe::FunnelStage::after_prescreen) == expected_prescreen,
          "funnel: a later stage only counts when the same call was sampled");
    CHECK(expected_prescreen == 2, "funnel: 2 of the 3 sampled calls reached the second stage");
    CHECK(perf_probe::read_view_reads() == expected_view_reads,
          "funnel: view reads are absolute work (not sampled)");
    CHECK(expected_view_reads == 4, "funnel: 4 of 12 calls read a view");

    // Off must be free again after being on.
    perf_probe::configure(false, 60000, 4);
    perf_probe::reset_for_test();
    {
        perf_probe::FunnelScope scope;
        scope.mark(perf_probe::FunnelStage::entry);
        perf_probe::note_view_read();
    }
    CHECK(perf_probe::read_funnel_calls() == 0, "funnel: turning the probe off stops all counting");
    CHECK(perf_probe::read_view_reads() == 0, "funnel: turning the probe off stops the work counter");
}

// The funnel line must appear **only** when something was sampled (a second without
// draws must not spam the log), must carry every stage name, and must be reset.
void test_funnel_line()
{
    perf_probe::reset_for_test();
    perf_probe::set_log_sink(&capture_sink);
    perf_probe::configure(true, 60000, 1);

    g_sink_calls = 0;
    g_funnel_line.clear();
    perf_probe::flush_now();
    CHECK(g_sink_calls == 2, "funnel line: absent when nothing was sampled (no spam)");
    CHECK(g_funnel_line.empty(), "funnel line: empty when nothing was sampled");

    for (int i = 0; i < 3; ++i)
    {
        perf_probe::FunnelScope scope;
        scope.mark(perf_probe::FunnelStage::entry);
        scope.mark(perf_probe::FunnelStage::after_om);
        scope.mark(perf_probe::FunnelStage::identified);
        perf_probe::note_identified_total(); // B108: the exact pair of the sampled stage above
        perf_probe::note_view_read();
    }
    g_sink_calls = 0;
    g_funnel_line.clear();
    perf_probe::flush_now();
    CHECK(g_sink_calls == 3, "funnel line: summary + histogram + funnel");
    CHECK(g_funnel_line.rfind("perf_probe_funnel_draw ", 0) == 0,
          "funnel line starts with perf_probe_funnel_draw");
    CHECK(g_funnel_line.find("calls=3") != std::string::npos, "funnel line reports the call count");
    CHECK(g_funnel_line.find("sampled=3") != std::string::npos, "funnel line reports the sample count");
    CHECK(g_funnel_line.find("views=3") != std::string::npos, "funnel line reports the view reads");
    CHECK(g_funnel_line.find("views_per_call=1.00") != std::string::npos,
          "funnel line reports views per call (the workload metric)");
    CHECK(g_funnel_line.find("identified_total=3") != std::string::npos,
          "funnel line reports the exact (non-sampled) identification count");
    CHECK(g_funnel_line.find("entry=3") != std::string::npos, "funnel line reports entry");
    CHECK(g_funnel_line.find("after_om=3") != std::string::npos, "funnel line reports after_om");
    CHECK(g_funnel_line.find("identified=3") != std::string::npos, "funnel line reports identified");
    // Every stage must appear, otherwise the reader cannot subtract the neighbours.
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(perf_probe::FunnelStage::count); ++i)
    {
        const char *name = perf_probe::funnel_stage_name(static_cast<perf_probe::FunnelStage>(i));
        CHECK(name != nullptr && g_funnel_line.find(name) != std::string::npos, name);
    }

    CHECK(perf_probe::read_funnel_calls() == 0, "funnel: flush resets the call counter");
    CHECK(perf_probe::read_funnel(perf_probe::FunnelStage::entry) == 0, "funnel: flush resets the stages");
    CHECK(perf_probe::read_view_reads() == 0, "funnel: flush resets the view read counter");
    std::printf("\nexample_funnel_line:\n%s\n", g_funnel_line.c_str());
}

// B107: pack/unpack of the draw shape slot must round-trip exactly, because the
// real-machine log is compared by eye against the parameters the game passed.
void test_draw_shape_pack_roundtrip()
{
    const auto indexed = perf_probe::DrawShapeSlot::indexed(3, 0, 0);
    CHECK(indexed.valid, "shape: an indexed slot is valid");
    CHECK(perf_probe::unpack_draw_shape_kind(indexed.value) == 0, "shape: kind 0 = DrawIndexed");
    CHECK(perf_probe::unpack_draw_shape_count(indexed.value) == 3, "shape: index count round-trips");
    CHECK(perf_probe::unpack_draw_shape_start(indexed.value) == 0, "shape: start index round-trips");
    CHECK(perf_probe::unpack_draw_shape_base(indexed.value) == 0, "shape: base vertex round-trips");
    CHECK(perf_probe::describe_draw_shape(indexed.value) == "ix count=3 start=0 base=0",
          "shape: fullscreen-triangle description");

    // A negative base vertex must survive: D3D allows it and it changes the draw.
    const auto negative = perf_probe::DrawShapeSlot::indexed(6, 12, -7);
    CHECK(perf_probe::unpack_draw_shape_base(negative.value) == -7,
          "shape: a negative base vertex survives the packing");
    CHECK(perf_probe::unpack_draw_shape_start(negative.value) == 12,
          "shape: a non-zero start index survives the packing");

    const auto non_indexed = perf_probe::DrawShapeSlot::non_indexed(4, 2);
    CHECK(perf_probe::unpack_draw_shape_kind(non_indexed.value) == 2, "shape: kind 2 = Draw");
    CHECK(perf_probe::describe_draw_shape(non_indexed.value) == "dr count=4 vstart=2",
          "shape: non-indexed description names the vertex start");

    // Saturation: an absurd count must not overflow into the neighbouring field.
    const auto huge = perf_probe::DrawShapeSlot::indexed(0x7FFFFFFFu, 0xFFFFFFFFu, 0);
    CHECK(perf_probe::unpack_draw_shape_count(huge.value) == 0xFFFF,
          "shape: a huge element count saturates");
    CHECK(perf_probe::unpack_draw_shape_start(huge.value) == 0xFFFF,
          "shape: a huge start saturates");
    CHECK(perf_probe::unpack_draw_shape_kind(huge.value) == 0,
          "shape: saturation does not disturb the kind bits");

    CHECK(perf_probe::describe_draw_shape(0) == "none", "shape: an empty slot describes as none");
    CHECK(perf_probe::pack_draw_shape(0, 3, 0, 0) != 0,
          "shape: a real shape never packs to the reserved empty value");
}

// The table must count calls and identified hits **separately** per shape, and the
// snapshot must reset the interval (B108: keys are cleared per interval, so the table
// is a per-interval Top-N instead of a table that saturates forever -- see the B107
// real-machine reading `unique=128 other=120934` that made the old keys sticky forever).
void test_draw_shape_table_counts()
{
    perf_probe::reset_for_test();
    perf_probe::configure(true, 60000, 1);

    const std::uint64_t ui_sprite = perf_probe::DrawShapeSlot::indexed(3, 0, 0).value;
    const std::uint64_t fullscreen = perf_probe::DrawShapeSlot::indexed(3, 0, 0).value;
    const std::uint64_t indexed_quad = perf_probe::DrawShapeSlot::indexed(6, 0, 0).value;
    const std::uint64_t plain_quad = perf_probe::DrawShapeSlot::non_indexed(4, 0).value;
    CHECK(ui_sprite == fullscreen, "shape table: the same call arguments pack to the same shape");

    // The target shape and two decoys; only the target shape is ever identified.
    for (int i = 0; i < 40; ++i)
    {
        const auto mark = perf_probe::make_draw_shape_mark(
            perf_probe::DrawShapeSlot::indexed(3, 0, 0));
        perf_probe::mark_draw_shape_call(mark);
    }
    for (int i = 0; i < 25; ++i)
    {
        const auto mark = perf_probe::make_draw_shape_mark(
            perf_probe::DrawShapeSlot::indexed(6, 0, 0));
        perf_probe::mark_draw_shape_call(mark);
    }
    for (int i = 0; i < 10; ++i)
    {
        const auto mark = perf_probe::make_draw_shape_mark(
            perf_probe::DrawShapeSlot::non_indexed(4, 0));
        perf_probe::mark_draw_shape_call(mark);
    }
    // 3 of the 40 "indexed count=3" calls are the real target (one per frame).
    for (int i = 0; i < 3; ++i)
    {
        const auto mark = perf_probe::make_draw_shape_mark(
            perf_probe::DrawShapeSlot::indexed(3, 0, 0));
        perf_probe::mark_draw_shape_call(mark);
        perf_probe::mark_draw_shape_identified(mark);
        perf_probe::note_identified_total();
    }

    CHECK(perf_probe::read_shape_entries() == 78, "shape table: every marked call is counted");
    CHECK(perf_probe::read_shape_calls(ui_sprite) == 43, "shape table: calls are per shape");
    CHECK(perf_probe::read_shape_calls(indexed_quad) == 25, "shape table: a decoy shape is counted too");
    CHECK(perf_probe::read_shape_calls(plain_quad) == 10, "shape table: the non-indexed shape is counted");
    CHECK(perf_probe::read_shape_hits(ui_sprite) == 3, "shape table: only the target shape has hits");
    CHECK(perf_probe::read_shape_hits(indexed_quad) == 0,
          "shape table: a decoy shape with 25 calls has zero hits (that is what makes it rejectable)");
    CHECK(perf_probe::read_shape_hits(plain_quad) == 0, "shape table: a non-indexed shape has zero hits");

    // Snapshot resets counters **and keys** (B108: one Top-N per interval).
    perf_probe::detail::DrawShapeTable::Snapshot snapshot;
    perf_probe::snapshot_draw_shapes(snapshot);
    CHECK(snapshot.total == 78, "shape snapshot: total equals the real call count");
    CHECK(snapshot.bucketed == 78, "shape snapshot: every call is visible (nothing was evicted)");
    CHECK(snapshot.cold == 0, "shape snapshot: no eviction happened yet");
    CHECK(snapshot.other_calls == 0, "shape snapshot: nothing fell out of the table");
    CHECK(snapshot.identified == 3, "shape snapshot: identified equals the real hit count");
    CHECK(snapshot.identified_bucketed == 3, "shape snapshot: the hits are visible in the table");
    CHECK(snapshot.identified_cold == 0, "shape snapshot: no hit was lost to eviction");
    CHECK(snapshot.unique == 3, "shape snapshot: three distinct shapes were claimed");
    CHECK(snapshot.observed_count == 1, "shape snapshot: one distinct identified shape this interval");
    CHECK(snapshot.observed_keys[0] == ui_sprite,
          "shape snapshot: the identified shape is the 'indexed count=3' one");
    CHECK(snapshot.observed_calls[0] == 3, "shape snapshot: the identified shape was hit 3 times");
    // B108 conservation identities (they are what the mismatch line checks at runtime).
    CHECK(snapshot.total == snapshot.bucketed + snapshot.cold + snapshot.other_calls,
          "shape snapshot: calls are conserved (bucketed + cold + other == total)");
    CHECK(snapshot.identified ==
              snapshot.identified_bucketed + snapshot.identified_cold + snapshot.identified_other,
          "shape snapshot: hits are conserved (table + cold + other == total)");
    CHECK(perf_probe::read_shape_calls(ui_sprite) == 0,
          "shape snapshot: counts are per interval (reset on snapshot)");
    CHECK(!perf_probe::shape_slot_claimed(ui_sprite),
          "shape snapshot: keys are per interval too (B108: the table must not saturate forever)");
    perf_probe::detail::DrawShapeTable::Snapshot second;
    perf_probe::snapshot_draw_shapes(second);
    CHECK(second.total == 0, "shape snapshot: a second snapshot must not double count");
    CHECK(second.unique == 0, "shape snapshot: the keys were cleared with the counts");
    CHECK(second.observed_count == 0, "shape snapshot: the observed list is per interval");
}

// B108 regression for the real-machine Bug (2): the shape line printed `identified=0`
// while the very same snapshot listed identified shapes. Root cause: `identified` only
// summed the hits **inside** the saturated 128-slot table, and the target shape had
// fallen into `other_hits` -- a pocket nobody printed. The old code therefore reported
// 0 hits for a shape that was identified every frame.
void test_draw_shape_identified_survives_saturation()
{
    perf_probe::reset_for_test();
    perf_probe::configure(true, 60000, 1);

    // 200 distinct one-call shapes: far more than the 128 slots (this is the real ratio:
    // 96.4% of the draws belong to shapes the table cannot hold).
    for (std::uint32_t i = 0; i < 200; ++i)
    {
        const auto mark = perf_probe::make_draw_shape_mark(
            perf_probe::DrawShapeSlot::indexed(100 + i, 0, 0));
        perf_probe::mark_draw_shape_call(mark);
    }
    // The target shape appears late (after the table is long saturated) and is identified.
    const std::uint64_t target = perf_probe::DrawShapeSlot::indexed(3, 0, 0).value;
    for (int i = 0; i < 5; ++i)
    {
        const auto mark = perf_probe::make_draw_shape_mark(
            perf_probe::DrawShapeSlot::indexed(3, 0, 0));
        perf_probe::mark_draw_shape_call(mark);
    }
    {
        const auto mark = perf_probe::make_draw_shape_mark(
            perf_probe::DrawShapeSlot::indexed(3, 0, 0));
        perf_probe::mark_draw_shape_call(mark);
        perf_probe::mark_draw_shape_identified(mark);
        perf_probe::note_identified_total();
    }

    perf_probe::detail::DrawShapeTable::Snapshot snapshot;
    perf_probe::snapshot_draw_shapes(snapshot);
    CHECK(snapshot.identified == 1,
          "saturation: the identified count must survive a saturated table (B107 reported 0)");
    CHECK(snapshot.identified_cold == 0,
          "saturation: an identified shape must be re-claimed instead of being lost");
    CHECK(snapshot.total == 206, "saturation: the call total is exact");
    CHECK(snapshot.total == snapshot.bucketed + snapshot.cold + snapshot.other_calls,
          "saturation: calls are conserved across evictions");
    CHECK(snapshot.identifications_conserved(),
          "saturation: hits are conserved across evictions");
    CHECK(snapshot.observed_count == 1 && snapshot.observed_keys[0] == target,
          "saturation: the observed (identified) shape list is capacity independent");
    CHECK(snapshot.evictions != 0, "saturation: the table really evicted (that is the Top-N trade)");
    CHECK(perf_probe::read_shape_mark_total() == 1, "saturation: the shape-layer mark counter agrees");
    CHECK(perf_probe::read_funnel_identified_total() == 1,
          "saturation: the funnel-layer counter agrees (this is the cross-check)");
}

// B108 Top-N: with more shapes than slots, the table must keep the **most-called** ones
// (and never drop a shape that was identified), instead of freezing on the first 128
// shapes that ever appeared.
void test_draw_shape_table_topn()
{
    perf_probe::reset_for_test();
    perf_probe::configure(true, 60000, 1);

    // (a) A target shape that is identified once, very early.
    const std::uint64_t target = perf_probe::DrawShapeSlot::non_indexed(3, 11333).value;
    {
        const auto mark = perf_probe::make_draw_shape_mark(
            perf_probe::DrawShapeSlot::non_indexed(3, 11333));
        perf_probe::mark_draw_shape_call(mark);
        perf_probe::mark_draw_shape_identified(mark);
        perf_probe::note_identified_total();
    }
    // (b) A hot shape: 400 calls (the "count=9480" style bucket of the real log).
    const std::uint64_t hot = perf_probe::DrawShapeSlot::indexed(9480, 0, 0).value;
    for (int i = 0; i < 400; ++i)
    {
        const auto mark = perf_probe::make_draw_shape_mark(
            perf_probe::DrawShapeSlot::indexed(9480, 0, 0));
        perf_probe::mark_draw_shape_call(mark);
    }
    // (c) 600 one-off shapes afterwards: they must rotate through the table without
    //     evicting the hot shape or the identified shape.
    for (std::uint32_t i = 0; i < 600; ++i)
    {
        const auto mark = perf_probe::make_draw_shape_mark(
            perf_probe::DrawShapeSlot::indexed(2000 + i, 0, 0));
        perf_probe::mark_draw_shape_call(mark);
    }

    perf_probe::detail::DrawShapeTable::Snapshot snapshot;
    perf_probe::snapshot_draw_shapes(snapshot);
    CHECK(snapshot.unique == perf_probe::detail::DrawShapeTable::k_bucket_count,
          "top-n: the table is genuinely full (the interesting case)");
    CHECK(snapshot.evictions > 0, "top-n: one-off shapes were evicted");
    CHECK(read_snapshot_calls(snapshot, hot) == 400,
          "top-n: the shape with 400 calls stays resident with its exact count");
    CHECK(read_snapshot_calls(snapshot, target) == 1,
          "top-n: the identified shape stays resident even with a single call");
    CHECK(read_snapshot_hits(snapshot, target) == 1, "top-n: its hit is visible");
    CHECK(snapshot.identified_cold == 0, "top-n: no hit was lost to eviction");
    CHECK(snapshot.total == 1001, "top-n: total calls are exact");
    CHECK(snapshot.total == snapshot.bucketed + snapshot.cold + snapshot.other_calls,
          "top-n: calls are conserved (evicted counts are folded into cold)");
    // The top reported shape must be the hot one (the reader sorts by call count).
    std::string report;
    perf_probe::snapshot_draw_shapes(snapshot);
    perf_probe::configure(true, 60000, 1);
    for (int i = 0; i < 50; ++i)
    {
        const auto mark = perf_probe::make_draw_shape_mark(
            perf_probe::DrawShapeSlot::indexed(9480, 0, 0));
        perf_probe::mark_draw_shape_call(mark);
    }
    perf_probe::detail::DrawShapeTable::Snapshot top;
    perf_probe::snapshot_draw_shapes(top);
    perf_probe::DrawElementHistogram::Snapshot elements;
    perf_probe::snapshot_draw_element_histogram(elements);
    perf_probe::format_draw_shape_report(top, elements, 50, report);
    CHECK(report.find("| shapes_top ix count=9480 start=0 base=0:50/0") != std::string::npos,
          "top-n: the hottest shape is listed first");
}

// The shape lines: one accounting line, one calls/hits Top-N, one list of the shapes
// that were actually identified, and (B108) the capacity-independent element histogram.
// That set of lists is the B107/B108 deliverable.
void test_draw_shape_lines()
{
    perf_probe::reset_for_test();
    perf_probe::set_log_sink(&capture_sink);
    perf_probe::configure(true, 60000, 1);

    clear_captured();
    perf_probe::flush_now();
    CHECK(g_sink_calls == 2, "shape lines: absent when no draw entered introspection (no spam)");
    CHECK(g_shape_header.empty(), "shape lines: no shape header without draws");

    // One sampled introspection call through the funnel + three shapes.
    {
        perf_probe::FunnelScope funnel;
        funnel.mark(perf_probe::FunnelStage::entry);
    }
    for (int i = 0; i < 7; ++i)
    {
        const auto mark = perf_probe::make_draw_shape_mark(perf_probe::DrawShapeSlot::indexed(3, 0, 0));
        perf_probe::mark_draw_shape_call(mark);
    }
    {
        const auto mark = perf_probe::make_draw_shape_mark(perf_probe::DrawShapeSlot::indexed(3, 0, 0));
        perf_probe::mark_draw_shape_call(mark);
        perf_probe::mark_draw_shape_identified(mark);
        perf_probe::note_identified_total();
    }
    for (int i = 0; i < 5; ++i)
    {
        const auto mark = perf_probe::make_draw_shape_mark(perf_probe::DrawShapeSlot::indexed(6, 0, 0));
        perf_probe::mark_draw_shape_call(mark);
    }
    for (int i = 0; i < 2; ++i)
    {
        const auto mark = perf_probe::make_draw_shape_mark(perf_probe::DrawShapeSlot::non_indexed(4, 0));
        perf_probe::mark_draw_shape_call(mark);
    }

    clear_captured();
    perf_probe::flush_now();
    CHECK(g_sink_calls == 7, "shape lines: summary + histogram + funnel + 4 shape lines");
    CHECK(g_shape_mismatch.empty(), "shape lines: no consistency warning when the layers agree");
    CHECK(g_shape_header.rfind("perf_probe_draw_shape_draw ", 0) == 0,
          "shape header starts with perf_probe_draw_shape_draw");
    CHECK(g_shape_header.find("entries=15") != std::string::npos,
          "shape header reports the entry count (reconciles with the hook call count)");
    CHECK(g_shape_header.find("shaped=15") != std::string::npos, "shape header reports the shaped count");
    CHECK(g_shape_header.find("bucketed=15") != std::string::npos,
          "shape header reports the visible (resident) call count");
    CHECK(g_shape_header.find("cold=0") != std::string::npos,
          "shape header reports the evicted call count");
    CHECK(g_shape_header.find("other=0") != std::string::npos, "shape header reports the overflow bucket");
    CHECK(g_shape_header.find("identified=1") != std::string::npos,
          "shape header reports the identified count");
    CHECK(g_shape_header.find("identified_cold=0") != std::string::npos,
          "shape header reports whether a target shape was lost to eviction");
    CHECK(g_shape_header.find("unique=3") != std::string::npos, "shape header reports the shape count");
    CHECK(g_shape_header.find("capacity=128") != std::string::npos,
          "shape header reports the table capacity (so unique can be read)");
    CHECK(g_shape_header.find("shadow_reject=7") != std::string::npos,
          "shape header reports the shadow reject count (the entry-level ceiling)");
    // Highest call count first -- the reader wants the biggest bucket at the left.
    const std::size_t first = g_shape_list.find("ix count=3 start=0 base=0:8/1");
    const std::size_t second = g_shape_list.find("ix count=6 start=0 base=0:5/0");
    const std::size_t third = g_shape_list.find("dr count=4 vstart=0:2/0");
    CHECK(first != std::string::npos, "shape list carries the target shape with its calls/hits");
    CHECK(second != std::string::npos, "shape list carries the indexed-quad decoy");
    CHECK(third != std::string::npos, "shape list carries the non-indexed decoy");
    CHECK(first < second && second < third, "shape list is sorted by call count descending");
    CHECK(g_shape_identified.find("ix count=3 start=0 base=0:1") != std::string::npos,
          "identified-shape list names the shape that was really identified");
    CHECK(g_shape_identified.find("count=6") == std::string::npos,
          "identified-shape list must NOT contain a shape that never matched");
    // (4) the element histogram: capacity independent, and it carries hits.
    CHECK(g_shape_elements.find("elem_ix 3:8/1") != std::string::npos,
          "element histogram: the indexed 'count=3' bucket carries calls/hits");
    CHECK(g_shape_elements.find("elem_ix 3:8/1 6:5/0") != std::string::npos,
          "element histogram: the indexed count buckets carry calls/hits in order");
    CHECK(g_shape_elements.find("elem_dr 4:2/0") != std::string::npos,
          "element histogram: the non-indexed shape lands in the 'dr' group");
    CHECK(g_shape_elements.find("count3 calls=8 hits=1 reject=7 outside_hits=0") != std::string::npos,
          "element histogram: the count==3 shadow evaluation is printed");
    // The funnel line must carry the non-sampled identified total so the two rows can be
    // compared by eye (this is the B108 self-consistency requirement).
    CHECK(g_funnel_line.find("identified_total=1") != std::string::npos,
          "funnel line reports the non-sampled identified total (comparable with the shape row)");
    CHECK(perf_probe::read_shape_entries() == 0, "shape lines: flush resets the entry counter");
    std::printf("\nexample_draw_shape_lines:\n%s\n%s\n%s\n%s\n",
                g_shape_header.c_str(), g_shape_list.c_str(), g_shape_identified.c_str(),
                g_shape_elements.c_str());
}

// The shadow evaluation must be readable as "the ceiling of an entry-level filter":
// every call whose shape never matched the target would be rejected. It is a pure
// statistic -- the identification set must not change, which is why it is computed
// in the report and not in the inspect path.
void test_draw_shape_shadow_reject()
{
    perf_probe::reset_for_test();
    perf_probe::configure(true, 60000, 1);

    // 20 calls of a shape that never matches + 2 calls of the shape that does.
    for (int i = 0; i < 20; ++i)
    {
        const auto mark = perf_probe::make_draw_shape_mark(
            perf_probe::DrawShapeSlot::non_indexed(2, 0));
        perf_probe::mark_draw_shape_call(mark);
    }
    for (int i = 0; i < 2; ++i)
    {
        const auto mark = perf_probe::make_draw_shape_mark(
            perf_probe::DrawShapeSlot::indexed(3, 0, 0));
        perf_probe::mark_draw_shape_call(mark);
        perf_probe::mark_draw_shape_identified(mark);
        perf_probe::note_identified_total();
    }

    perf_probe::detail::DrawShapeTable::Snapshot snapshot;
    perf_probe::DrawElementHistogram::Snapshot elements;
    perf_probe::snapshot_draw_shapes(snapshot);
    perf_probe::snapshot_draw_element_histogram(elements);
    std::string report;
    perf_probe::format_draw_shape_report(snapshot, elements, 22, report);
    CHECK(report.find("entries=22") != std::string::npos, "shadow: entries are reported");
    CHECK(report.find("identified=2") != std::string::npos, "shadow: identified count is reported");
    CHECK(report.find("shadow_reject=20") != std::string::npos,
          "shadow: the never-matching shape's 20 calls are the rejectable set");
    CHECK(report.find("identified_shapes ix count=3 start=0 base=0:2") != std::string::npos,
          "shadow: the whitelist is exactly the shapes that matched");
    // The element histogram gives the **capacity independent** version of the same
    // question: "vertex/index count == 2" would reject 20, "== 3" would reject nothing.
    CHECK(report.find("elem_dr 2:20/0") != std::string::npos,
          "shadow: the element histogram exposes the rejectable 'count=2' population");
    CHECK(report.find("count3 calls=2 hits=2 reject=20 outside_hits=0") != std::string::npos,
          "shadow: count==3 is necessary in this sample (reject=20, outside_hits=0)");

    // With nothing identified there is no whitelist, so the shadow number must be 0
    // (it must never pretend that "everything can be rejected").
    for (int i = 0; i < 5; ++i)
    {
        const auto mark = perf_probe::make_draw_shape_mark(
            perf_probe::DrawShapeSlot::non_indexed(2, 0));
        perf_probe::mark_draw_shape_call(mark);
    }
    perf_probe::detail::DrawShapeTable::Snapshot second;
    perf_probe::DrawElementHistogram::Snapshot elements2;
    perf_probe::snapshot_draw_shapes(second);
    perf_probe::snapshot_draw_element_histogram(elements2);
    std::string report2;
    perf_probe::format_draw_shape_report(second, elements2, 5, report2);
    CHECK(report2.find("identified=0") != std::string::npos, "shadow: identified is 0 this interval");
    CHECK(report2.find("shadow_reject=0") != std::string::npos,
          "shadow: no whitelist => no reject claim");
    CHECK(report2.find("outside_hits=0") != std::string::npos,
          "shadow: no identification => no outside-count3 hit claim");
}

// B108: the element histogram must be exact, capacity independent, and split by kind
// (index count and vertex count are different questions). Bucket boundaries are part of
// the contract because the real-machine log is read by eye.
void test_draw_element_histogram()
{
    perf_probe::reset_for_test();
    perf_probe::configure(true, 60000, 1);

    // Bucket mapping contract.
    CHECK(perf_probe::DrawElementHistogram::bucket_for(0) == 0, "elem: 0 maps to the exact bucket 0");
    CHECK(perf_probe::DrawElementHistogram::bucket_for(16) == 16, "elem: 16 is still exact");
    CHECK(perf_probe::DrawElementHistogram::bucket_for(17) == 17, "elem: 17 starts the 17-31 range");
    CHECK(perf_probe::DrawElementHistogram::bucket_for(31) == 17, "elem: 31 is inside 17-31");
    CHECK(perf_probe::DrawElementHistogram::bucket_for(32) == 18, "elem: 32 starts 32-63");
    CHECK(perf_probe::DrawElementHistogram::bucket_for(1024) == 23, "elem: 1024 starts 1024-2047");
    CHECK(perf_probe::DrawElementHistogram::bucket_for(9480) == 26, "elem: 9480 is inside 8192-16383");
    CHECK(perf_probe::DrawElementHistogram::bucket_for(65535) ==
              perf_probe::DrawElementHistogram::k_bucket_count - 1,
          "elem: the saturated count lands in the last bucket");
    CHECK(perf_probe::DrawElementHistogram::bucket_for(4000000) ==
              perf_probe::DrawElementHistogram::k_bucket_count - 1,
          "elem: an absurd count is clamped (never out of bounds)");
    CHECK(std::strcmp(perf_probe::DrawElementHistogram::bucket_label(3), "3") == 0,
          "elem: the label of bucket 3 is '3' (that is the string the reader greps)");
    CHECK(std::strcmp(perf_probe::DrawElementHistogram::bucket_label(
                          perf_probe::DrawElementHistogram::k_bucket_count - 1), "32768+") == 0,
          "elem: the last label admits that the packed count saturates");

    // 40 indexed count=3 calls (3 of them the target) + 5 indexed count=1536 + 4 non-indexed
    // count=3 calls (1 of them the target) -- i.e. the two families the real log showed.
    for (int i = 0; i < 40; ++i)
    {
        const auto mark = perf_probe::make_draw_shape_mark(perf_probe::DrawShapeSlot::indexed(3, 0, 0));
        perf_probe::mark_draw_shape_call(mark);
    }
    for (int i = 0; i < 3; ++i)
    {
        const auto mark = perf_probe::make_draw_shape_mark(perf_probe::DrawShapeSlot::indexed(3, 0, 0));
        perf_probe::mark_draw_shape_call(mark);
        perf_probe::mark_draw_shape_identified(mark);
        perf_probe::note_identified_total();
    }
    for (int i = 0; i < 5; ++i)
    {
        const auto mark = perf_probe::make_draw_shape_mark(perf_probe::DrawShapeSlot::indexed(1536, 0, 0));
        perf_probe::mark_draw_shape_call(mark);
    }
    for (int i = 0; i < 4; ++i)
    {
        const auto mark = perf_probe::make_draw_shape_mark(
            perf_probe::DrawShapeSlot::non_indexed(3, 11333));
        perf_probe::mark_draw_shape_call(mark);
    }
    {
        const auto mark = perf_probe::make_draw_shape_mark(
            perf_probe::DrawShapeSlot::non_indexed(3, 11335));
        perf_probe::mark_draw_shape_call(mark);
        perf_probe::mark_draw_shape_identified(mark);
        perf_probe::note_identified_total();
    }

    CHECK(perf_probe::read_element_calls(0, 3) == 43, "elem: indexed count=3 calls are exact");
    CHECK(perf_probe::read_element_hits(0, 3) == 3, "elem: indexed count=3 hits are exact");
    CHECK(perf_probe::read_element_calls(0, 1536) == 5, "elem: indexed count=1536 calls are exact");
    CHECK(perf_probe::read_element_hits(0, 1536) == 0, "elem: the 1536 bucket never matched");
    CHECK(perf_probe::read_element_calls(2, 3) == 5,
          "elem: non-indexed count=3 calls are exact (both vstart values share the bucket)");
    CHECK(perf_probe::read_element_hits(2, 3) == 1, "elem: non-indexed count=3 hits are exact");
    CHECK(perf_probe::read_element_calls(0, 4) == 0, "elem: an untouched bucket stays 0");

    perf_probe::DrawElementHistogram::Snapshot elements;
    perf_probe::snapshot_draw_element_histogram(elements);
    CHECK(elements.total_calls == 53, "elem: the histogram total equals the shape-marked calls");
    CHECK(elements.total_hits == 4, "elem: the histogram hit total equals the identifications");
    CHECK(elements.calls[0][3] == 43 && elements.calls[1][3] == 5,
          "elem: the count==3 bucket is split by kind but sums to 48");
    CHECK(elements.hits[0][3] + elements.hits[1][3] == 4,
          "elem: every identification in this sample has element count 3");
    CHECK(elements.total_calls == 53, "elem: snapshot resets the counters");
    perf_probe::DrawElementHistogram::Snapshot again;
    perf_probe::snapshot_draw_element_histogram(again);
    CHECK(again.total_calls == 0, "elem: a second snapshot must not double count");
}

// B108 self-consistency: the shape row's `identified` must equal the funnel row's
// `identified_total` (the two counters are incremented by two independent call sites).
// When they disagree, flush must say so instead of quietly printing a wrong distribution.
void test_draw_shape_consistency_check()
{
    perf_probe::reset_for_test();
    perf_probe::set_log_sink(&capture_sink);
    perf_probe::configure(true, 60000, 1);

    // (a) Consistent: both layers see the same 3 identifications.
    //     A funnel scope is needed as well, otherwise the funnel line (which carries
    //     `identified_total=`) is not printed at all.
    for (int i = 0; i < 3; ++i)
    {
        perf_probe::FunnelScope funnel;
        funnel.mark(perf_probe::FunnelStage::entry);
        const auto mark = perf_probe::make_draw_shape_mark(perf_probe::DrawShapeSlot::indexed(3, 0, 0));
        perf_probe::mark_draw_shape_call(mark);
        perf_probe::mark_draw_shape_identified(mark);
        perf_probe::note_identified_total();
        funnel.mark(perf_probe::FunnelStage::identified);
    }
    clear_captured();
    perf_probe::flush_now();
    CHECK(g_shape_mismatch.empty(), "consistency: agreeing layers must not warn");
    CHECK(g_shape_header.find("identified=3") != std::string::npos,
          "consistency: the shape row reports 3 identifications");
    CHECK(g_funnel_line.find("identified_total=3") != std::string::npos,
          "consistency: the funnel row reports the same 3 identifications");

    // (b) The predicate itself is the one the runtime uses (same helper, not a copy).
    perf_probe::detail::DrawShapeTable::Snapshot probe_snap;
    probe_snap.total = 3;
    probe_snap.bucketed = 3;
    probe_snap.identified = 3;
    probe_snap.identified_bucketed = 3;
    CHECK(perf_probe::draw_shape_consistency_ok(probe_snap, 3, 3),
          "consistency: (3,3,3) is judged consistent");
    CHECK(!perf_probe::draw_shape_consistency_ok(probe_snap, 2, 3),
          "consistency: the shape-layer mark count must match as well");
    CHECK(!perf_probe::draw_shape_consistency_ok(probe_snap, 3, 2),
          "consistency: the funnel-layer count must match as well");
    probe_snap.cold = 1;
    CHECK(!perf_probe::draw_shape_consistency_ok(probe_snap, 3, 3),
          "consistency: a broken call-conservation identity is a failure too");
    probe_snap.cold = 0;
    probe_snap.identified_cold = 1;
    CHECK(!perf_probe::draw_shape_consistency_ok(probe_snap, 3, 3),
          "consistency: a broken hit-conservation identity is a failure too");

    // (c) Inconsistent: the funnel layer does not see the identifications (this is what a
    //     mark moved onto an unreachable branch looks like) => the warning line appears.
    perf_probe::configure(true, 60000, 1);
    for (int i = 0; i < 2; ++i)
    {
        const auto mark = perf_probe::make_draw_shape_mark(perf_probe::DrawShapeSlot::indexed(6, 0, 0));
        perf_probe::mark_draw_shape_call(mark);
        perf_probe::mark_draw_shape_identified(mark);
    }
    clear_captured();
    perf_probe::flush_now();
    CHECK(!g_shape_mismatch.empty(), "consistency: a disagreement must print a warning line");
    CHECK(g_shape_mismatch.rfind("perf_probe_draw_shape_mismatch ", 0) == 0,
          "consistency: the warning line has its own greppable prefix");
    CHECK(g_shape_mismatch.find("shape_identified=2") != std::string::npos,
          "consistency: the warning names the shape-layer count");
    CHECK(g_shape_mismatch.find("funnel_identified=0") != std::string::npos,
          "consistency: the warning names the funnel-layer count");
}

// A draw entry that provably cannot be the target almost always exists in this game
// (2 vertices / 3 indices), but B107/B108 ship the *measurement* only: while the probe is
// off the marker must cost nothing and count nothing, so the shipped build keeps the
// identification set bit-identical to B106.
void test_draw_shape_mark_is_free_when_disabled()
{
    perf_probe::reset_for_test();
    perf_probe::configure(false, 60000, 32);
    perf_probe::set_log_sink(&capture_sink);
    const std::uint64_t shape = perf_probe::DrawShapeSlot::indexed(3, 0, 0).value;
    for (int i = 0; i < 100; ++i)
    {
        const auto mark = perf_probe::make_draw_shape_mark(perf_probe::DrawShapeSlot::indexed(3, 0, 0));
        CHECK(!mark.armed, "shape mark: not armed while the probe is off");
        perf_probe::mark_draw_shape_call(mark);
        perf_probe::mark_draw_shape_identified(mark);
        perf_probe::note_identified_total();
    }
    CHECK(perf_probe::read_shape_entries() == 0, "shape mark: off => not even the entry counter moves");
    CHECK(perf_probe::read_shape_mark_total() == 0, "shape mark: off => the mark counter stays 0");
    CHECK(perf_probe::read_funnel_identified_total() == 0,
          "shape mark: off => the funnel-layer counter stays 0");
    CHECK(perf_probe::read_element_calls(0, 3) == 0, "shape mark: off => the histogram stays empty");
    CHECK(perf_probe::read_shape_calls(shape) == 0, "shape mark: off => no shape bucket is touched");
    CHECK(!perf_probe::shape_slot_claimed(shape), "shape mark: off => no shape key is claimed");
    g_sink_calls = 0;
    perf_probe::flush_now();
    CHECK(g_sink_calls == 0, "shape mark: off => flush emits nothing");
}

// B109: the entry filter's criterion. It reads ONE call argument and nothing else, so it
// must admit exactly one element count (3 -- the only bucket with hits in the in-game
// element-count histogram) and reject every other one without any COM call. While the
// filter is off (ini `DrawEntryFilter=0`) every draw must take the original path and the
// filter must not touch a single atomic.
void test_draw_entry_filter_admits_only_three()
{
    perf_probe::reset_for_test();
    perf_probe::set_log_sink(&capture_sink);
    perf_probe::configure_draw_entry_filter(true, 0); // stride 0 = no canary: the criterion alone

    CHECK(perf_probe::draw_entry_filter_active(), "entry filter: on + not falsified => active");
    CHECK(std::strcmp(perf_probe::draw_entry_filter_state_name(), "on") == 0,
          "entry filter: the state name is on");

    std::uint64_t admitted = 0;
    std::uint64_t rejected = 0;
    std::uint64_t admitted_canary = 0;
    for (std::uint32_t count = 0; count <= 64; ++count)
    {
        const perf_probe::DrawEntryAdmit admit = perf_probe::draw_entry_filter_admit(count);
        if (admit.admitted)
        {
            ++admitted;
            if (admit.canary)
                ++admitted_canary;
        }
        else
            ++rejected;
    }
    CHECK(admitted == 1, "entry filter: exactly one element count is admitted");
    CHECK(rejected == 64, "entry filter: every other element count is rejected");
    CHECK(admitted_canary == 0, "entry filter: stride 0 admits no canary at all");
    const perf_probe::DrawEntryFilterSnapshot on_snapshot = perf_probe::read_draw_entry_filter();
    CHECK(on_snapshot.skipped == rejected, "entry filter: the skip counter equals the rejects");
    CHECK(on_snapshot.canary == 0, "entry filter: stride 0 records no canary");
    CHECK(on_snapshot.configured, "entry filter: the snapshot carries the configured flag");
    CHECK(!on_snapshot.disabled, "entry filter: the snapshot is not falsified");

    // The one admitted count must be 3 (checked with the same predicate the runtime uses).
    CHECK(perf_probe::draw_entry_filter_admit(3).admitted, "entry filter: 3 is admitted");
    for (std::uint32_t count : { 0u, 1u, 2u, 4u, 6u, 12u, 1536u, 9480u })
    {
        const perf_probe::DrawEntryAdmit admit = perf_probe::draw_entry_filter_admit(count);
        CHECK(!admit.admitted, "entry filter: a non-3 count is rejected");
        CHECK(!admit.canary, "entry filter: a rejected draw is not flagged as a canary");
    }

    // Off: the original path, and provably zero bookkeeping (the shipped fallback after the
    // insurance fires, and the `DrawEntryFilter=0` setting).
    perf_probe::configure_draw_entry_filter(false, 0);
    CHECK(!perf_probe::draw_entry_filter_active(), "entry filter: configured off => not active");
    CHECK(std::strcmp(perf_probe::draw_entry_filter_state_name(), "off") == 0,
          "entry filter: the state name is off");
    const perf_probe::DrawEntryFilterSnapshot before = perf_probe::read_draw_entry_filter();
    for (int i = 0; i < 16; ++i)
    {
        const perf_probe::DrawEntryAdmit admit = perf_probe::draw_entry_filter_admit(6);
        CHECK(admit.admitted, "entry filter: off => every draw is admitted (original path)");
        CHECK(!admit.canary, "entry filter: off => an admission is not a canary");
    }
    const perf_probe::DrawEntryFilterSnapshot after = perf_probe::read_draw_entry_filter();
    CHECK(after.skipped == before.skipped, "entry filter: off => the skip counter does not move");
    CHECK(after.canary == before.canary, "entry filter: off => the canary counter does not move");

    // `configure` is also the only place that can set the stride back.
    perf_probe::configure_draw_entry_filter(true, 64);
    CHECK(perf_probe::draw_entry_filter_canary_stride() == 64,
          "entry filter: configure() takes the canary stride from the ini value");
}

// B109: the canary sampling is what makes the insurance observable. Without it nothing
// non-3 would ever reach an identification, and the check at the identification sites
// would be dead code -- i.e. a false sense of safety.
void test_draw_entry_filter_canary()
{
    perf_probe::reset_for_test();
    perf_probe::set_log_sink(&capture_sink);
    perf_probe::configure_draw_entry_filter(true, 4);

    std::uint64_t admitted = 0;
    for (int i = 0; i < 12; ++i)
    {
        const perf_probe::DrawEntryAdmit admit = perf_probe::draw_entry_filter_admit(6);
        if (admit.admitted)
        {
            ++admitted;
            CHECK(admit.canary, "canary: an admitted non-3 draw is flagged as a canary");
        }
        // A count==3 admission must not consume the canary sequence (it returns before
        // touching any atomic) -- otherwise the stride would depend on how many target
        // draws happened to be interleaved.
        CHECK(perf_probe::draw_entry_filter_admit(3).admitted, "canary: count==3 stays admitted");
        CHECK(!perf_probe::draw_entry_filter_admit(3).canary, "canary: count==3 is never a canary");
    }
    CHECK(admitted == 3, "canary: stride 4 over 12 non-3 draws admits 3");
    const perf_probe::DrawEntryFilterSnapshot snapshot = perf_probe::read_draw_entry_filter();
    CHECK(snapshot.canary == 3, "canary: the canary counter matches the admissions");
    CHECK(snapshot.skipped == 9, "canary: the other 9 non-3 draws are rejected");

    // stride 1 == admit everything == equivalent to having no filter at all (safest setting).
    perf_probe::reset_for_test();
    perf_probe::configure_draw_entry_filter(true, 1);
    for (int i = 0; i < 5; ++i)
    {
        const perf_probe::DrawEntryAdmit admit = perf_probe::draw_entry_filter_admit(6);
        CHECK(admit.admitted && admit.canary, "canary: stride 1 admits every non-3 draw");
    }
    CHECK(perf_probe::read_draw_entry_filter().skipped == 0, "canary: stride 1 rejects nothing");

    // stride 0 == trust the criterion (the insurance degrades to bookkeeping only: visible
    // as non3_id= in the funnel line).
    perf_probe::reset_for_test();
    perf_probe::configure_draw_entry_filter(true, 0);
    for (int i = 0; i < 5; ++i)
        CHECK(!perf_probe::draw_entry_filter_admit(6).admitted, "canary: stride 0 admits nothing");
    CHECK(perf_probe::read_draw_entry_filter().canary == 0, "canary: stride 0 records no canary");
}

// B109 -- THE important one: the self-falsification insurance.
//
// The entry filter is NOT a code-level equivalence (neither signature inspects
// `element_count`), so the filter must be able to prove itself wrong: every identification
// success reports the element count of that draw, and a single non-3 identification must
// disable the filter permanently and loudly. After that every draw takes the original path
// again -- fail-open, so the worst case is the old (slower) behaviour, never a lost target.
void test_draw_entry_filter_failsafe_fail_open()
{
    perf_probe::reset_for_test();
    perf_probe::set_log_sink(&capture_sink);
    perf_probe::configure_draw_entry_filter(true, 1); // every non-3 draw is a canary
    clear_captured();

    const perf_probe::DrawShapeMarkArmed indexed_mark =
        perf_probe::make_draw_shape_mark(perf_probe::DrawShapeSlot::indexed(6, 0, 0));
    const perf_probe::DrawEntryAdmit admit = perf_probe::draw_entry_filter_admit(6);
    CHECK(admit.admitted && admit.canary,
          "failsafe: a non-3 draw reaches introspection (as a canary) while the filter is on");

    // A count==3 identification is the normal case: no latch, no warning, no atomic.
    perf_probe::note_draw_identified_element_count(3, indexed_mark, false);
    CHECK(perf_probe::draw_entry_filter_active(),
          "failsafe: a count==3 identification keeps the filter active");
    CHECK(g_entry_filter_disabled_line.empty(),
          "failsafe: a count==3 identification prints no warning");
    CHECK(perf_probe::read_draw_entry_filter().non3_identified == 0,
          "failsafe: a count==3 identification is not counted as a falsification");

    // The falsification: ONE non-3 identification.
    perf_probe::note_draw_identified_element_count(6, indexed_mark, true);
    const perf_probe::DrawEntryFilterSnapshot tripped = perf_probe::read_draw_entry_filter();
    CHECK(tripped.disabled, "failsafe: one non-3 identification disables the filter (one-way latch)");
    CHECK(!perf_probe::draw_entry_filter_active(), "failsafe: the filter is no longer active");
    CHECK(std::strcmp(perf_probe::draw_entry_filter_state_name(), "disabled") == 0,
          "failsafe: the state name becomes disabled");
    CHECK(tripped.configured, "failsafe: the latch is separate from the ini switch (still configured on)");
    CHECK(tripped.disable_element_count == 6, "failsafe: the latch names the offending element count");
    CHECK(tripped.disable_entry == perf_probe::DrawEntryKind::indexed,
          "failsafe: the latch names the offending entry (ix)");
    CHECK(tripped.non3_identified == 1, "failsafe: the falsification is counted");

    CHECK(!g_entry_filter_disabled_line.empty(), "failsafe: the warning line is emitted");
    CHECK(g_entry_filter_disabled_line.rfind("draw_entry_filter_disabled reason=non3_identified ", 0) == 0,
          "failsafe: the warning line has its own greppable prefix and reason");
    CHECK(g_entry_filter_disabled_line.find("element_count=6") != std::string::npos,
          "failsafe: the warning names the element count");
    CHECK(g_entry_filter_disabled_line.find("entry=ix") != std::string::npos,
          "failsafe: the warning names the entry (ix|dr)");
    CHECK(g_entry_filter_disabled_line.find("canary=1") != std::string::npos,
          "failsafe: the warning says the offending draw came in as a canary");
    CHECK(g_entry_filter_disabled_line.find("action=filter_disabled_fail_open") != std::string::npos,
          "failsafe: the warning states the fail-open action");
    std::printf("\nexample_draw_entry_filter_warning:\n%s\n", g_entry_filter_disabled_line.c_str());

    // FAIL-OPEN: after the latch every draw takes the original path, and the filter stops
    // doing any bookkeeping at all (zero atomics -- it must not keep charging for itself).
    const perf_probe::DrawEntryFilterSnapshot before_tail = perf_probe::read_draw_entry_filter();
    for (int i = 0; i < 32; ++i)
    {
        const perf_probe::DrawEntryAdmit disabled_admit = perf_probe::draw_entry_filter_admit(6);
        CHECK(disabled_admit.admitted,
              "failsafe (fail-open): after disabling, non-3 draws go the original path");
        CHECK(!disabled_admit.canary, "failsafe (fail-open): they are not canaries either");
    }
    const perf_probe::DrawEntryFilterSnapshot after_tail = perf_probe::read_draw_entry_filter();
    CHECK(after_tail.skipped == before_tail.skipped, "failsafe: disabled => the skip counter freezes");
    CHECK(after_tail.canary == before_tail.canary, "failsafe: disabled => the canary counter freezes");

    // One-way and one-shot: a later non-3 identification must not re-warn (no log spam) but
    // is still recorded (the evidence survives).
    clear_captured();
    perf_probe::note_draw_identified_element_count(12, indexed_mark, false);
    CHECK(g_sink_calls == 0, "failsafe: the warning is printed once, not once per identification");
    CHECK(perf_probe::read_draw_entry_filter().non3_identified == 2,
          "failsafe: the later falsification is still counted");
    CHECK(perf_probe::draw_entry_filter_disabled(), "failsafe: the latch stays set");
}

// B109: with the filter configured off there is nothing to disable, but the observation is
// still recorded -- "a non-3 draw was identified" falsifies the criterion and is exactly
// the evidence a future decision needs. No warning, because no filter was switched off.
void test_draw_entry_filter_off_records_falsification()
{
    perf_probe::reset_for_test();
    perf_probe::set_log_sink(&capture_sink);
    perf_probe::configure_draw_entry_filter(false, 64);
    clear_captured();
    const perf_probe::DrawShapeMarkArmed mark =
        perf_probe::make_draw_shape_mark(perf_probe::DrawShapeSlot::non_indexed(6, 0));
    perf_probe::note_draw_identified_element_count(6, mark, false);
    const perf_probe::DrawEntryFilterSnapshot snapshot = perf_probe::read_draw_entry_filter();
    CHECK(!snapshot.disabled, "entry filter off: a non-3 identification has nothing to disable");
    CHECK(snapshot.non3_identified == 1,
          "entry filter off: the falsifying observation is still recorded (evidence)");
    CHECK(g_entry_filter_disabled_line.empty(),
          "entry filter off: no disable warning (nothing was enabled)");
    CHECK(std::strcmp(perf_probe::draw_entry_filter_state_name(), "off") == 0,
          "entry filter off: the state name stays off");
}

// B109: the filter state and its counters must be visible in the log even if the reader
// missed the one-shot warning line: the summary line carries `entry_filter=` and the funnel
// line carries the per-interval counters. Also pins the reporting side of the two
// acceptance readings: `calls=`/`entry=` keep the B106/B108 meaning (every hook -> inspect
// call) while `views=` (and therefore `views_per_call`) drops because the rejected draws
// never read a view.
void test_draw_entry_filter_line()
{
    perf_probe::reset_for_test();
    perf_probe::set_log_sink(&capture_sink);
    perf_probe::configure(true, 60000, 1);
    perf_probe::configure_draw_entry_filter(true, 4);

    for (int i = 0; i < 5; ++i)
    {
        perf_probe::FunnelScope funnel;
        funnel.mark(perf_probe::FunnelStage::entry);
        const perf_probe::DrawEntryAdmit admit = perf_probe::draw_entry_filter_admit(6);
        if (!admit.admitted)
            continue; // rejected at the entry: no state query, no view read
        funnel.mark(perf_probe::FunnelStage::after_om);
        perf_probe::note_view_read();
    }

    clear_captured();
    perf_probe::flush_now();
    CHECK(g_summary.find("entry_filter=on") != std::string::npos,
          "entry filter line: the summary line reports the filter state");
    CHECK(g_summary.find("wso_max_us=") != std::string::npos,
          "entry filter line: the summary line is not truncated by the new field");
    CHECK(g_funnel_line.find("entry_filter=on") != std::string::npos,
          "entry filter line: the funnel line reports the filter state");
    CHECK(g_funnel_line.find("canary_stride=4") != std::string::npos,
          "entry filter line: the funnel line reports the canary stride");
    // 5 non-3 draws, stride 4 => the 4th one is a canary, the other 4 are rejected.
    CHECK(g_funnel_line.find("skip=4") != std::string::npos,
          "entry filter line: the funnel line reports the skipped calls");
    CHECK(g_funnel_line.find("canary=1") != std::string::npos,
          "entry filter line: the funnel line reports the canary admissions");
    CHECK(g_funnel_line.find("non3_id=0") != std::string::npos,
          "entry filter line: the funnel line reports the falsifying identifications (0 = none)");
    CHECK(g_funnel_line.find("disable_ec=0") != std::string::npos,
          "entry filter line: no disable element count while nothing tripped");
    CHECK(g_funnel_line.find("disable_entry=?") != std::string::npos,
          "entry filter line: the disable entry is ? while nothing tripped");
    CHECK(g_funnel_line.find("calls=5") != std::string::npos,
          "entry filter line: calls= keeps the B106 meaning (all hook -> inspect calls)");
    CHECK(g_funnel_line.find("entry=5") != std::string::npos,
          "entry filter line: entry= keeps the B106 meaning");
    CHECK(g_funnel_line.find("views=1") != std::string::npos,
          "entry filter line: only the admitted draw read a view (work went down)");
    CHECK(g_funnel_line.find("views_per_call=0.20") != std::string::npos,
          "entry filter line: views_per_call drops (1 view over 5 calls)");
    CHECK(perf_probe::read_draw_entry_filter().skipped == 0,
          "entry filter line: flush resets the per-interval counters (no residue)");

    // Once the insurance trips, both lines say so -- a reader who missed the one-shot
    // warning still sees that the filter is gone.
    const perf_probe::DrawShapeMarkArmed mark =
        perf_probe::make_draw_shape_mark(perf_probe::DrawShapeSlot::indexed(6, 0, 0));
    perf_probe::note_draw_identified_element_count(6, mark, true);
    {
        perf_probe::FunnelScope funnel;
        funnel.mark(perf_probe::FunnelStage::entry);
    }
    clear_captured();
    perf_probe::flush_now();
    CHECK(g_summary.find("entry_filter=disabled") != std::string::npos,
          "entry filter line: the summary line switches to disabled");
    CHECK(g_funnel_line.find("entry_filter=disabled") != std::string::npos,
          "entry filter line: the funnel line switches to disabled");
    CHECK(g_funnel_line.find("non3_id=1") != std::string::npos,
          "entry filter line: the funnel line reports the falsifying identification");
    CHECK(g_funnel_line.find("disable_ec=6") != std::string::npos,
          "entry filter line: the funnel line reports the offending element count");
    CHECK(g_funnel_line.find("disable_entry=ix") != std::string::npos,
          "entry filter line: the funnel line reports the offending entry");
}

void test_funnel_stage_names()
{
    CHECK(perf_probe::funnel_stage_name(perf_probe::FunnelStage::entry) != nullptr &&
          std::strcmp(perf_probe::funnel_stage_name(perf_probe::FunnelStage::entry), "entry") == 0,
          "funnel stage 0 is entry (the funnel must start where the function is entered)");
    CHECK(std::strcmp(perf_probe::funnel_stage_name(perf_probe::FunnelStage::identified),
                      "identified") == 0,
          "the last funnel stage is identified");
    CHECK(std::strcmp(perf_probe::funnel_stage_name(perf_probe::FunnelStage::after_cb), "after_cb") == 0,
          "the hoisted cb0 predicate has its own funnel stage (B106)");
    CHECK(std::strcmp(perf_probe::funnel_stage_name(perf_probe::FunnelStage::after_output),
                      "after_output") == 0,
          "the hoisted output-candidate predicate has its own funnel stage (B106)");
    CHECK(std::strcmp(perf_probe::funnel_stage_name(static_cast<perf_probe::FunnelStage>(999)), "?") == 0,
          "out-of-range funnel stage is reported as ? (no out-of-bounds read)");
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

    // B106: the introspection funnel. While the probe is off this must cost no atomic
    // operation at all (the whole point is that it can sit on the per-draw path).
    // Every stage is marked so this measures the worst case (the real function has 9
    // mark() call sites on its way to a successful identification).
    const auto mark_all_funnel_stages = [](const perf_probe::FunnelScope &funnel)
    {
        funnel.mark(perf_probe::FunnelStage::entry);
        funnel.mark(perf_probe::FunnelStage::after_om);
        funnel.mark(perf_probe::FunnelStage::after_prescreen);
        funnel.mark(perf_probe::FunnelStage::after_ps_query);
        funnel.mark(perf_probe::FunnelStage::after_fast_path);
        funnel.mark(perf_probe::FunnelStage::after_cb);
        funnel.mark(perf_probe::FunnelStage::after_viewport);
        funnel.mark(perf_probe::FunnelStage::after_output);
        funnel.mark(perf_probe::FunnelStage::identified);
    };

    perf_probe::reset_for_test();
    perf_probe::configure(false, 60000, 32);
    QueryPerformanceCounter(&begin);
    for (int i = 0; i < iterations; ++i)
    {
        perf_probe::FunnelScope funnel;
        mark_all_funnel_stages(funnel);
        perf_probe::note_view_read();
    }
    QueryPerformanceCounter(&end);
    const double funnel_disabled_ns = static_cast<double>(end.QuadPart - begin.QuadPart) * to_ns / iterations;

    perf_probe::reset_for_test();
    perf_probe::configure(true, 60000, 32);
    QueryPerformanceCounter(&begin);
    for (int i = 0; i < iterations; ++i)
    {
        perf_probe::FunnelScope funnel;
        mark_all_funnel_stages(funnel);
    }
    QueryPerformanceCounter(&end);
    const double funnel_enabled_ns = static_cast<double>(end.QuadPart - begin.QuadPart) * to_ns / iterations;

    // B107: the per-draw shape marker. The probe is OFF here => the whole thing must
    // boil down to one relaxed load plus two predictable branches (the shipped state).
    perf_probe::reset_for_test();
    perf_probe::configure(false, 60000, 32);
    QueryPerformanceCounter(&begin);
    for (int i = 0; i < iterations; ++i)
    {
        const auto mark = perf_probe::make_draw_shape_mark(
            perf_probe::DrawShapeSlot::indexed(3, 0, 0));
        perf_probe::mark_draw_shape_call(mark);
    }
    QueryPerformanceCounter(&end);
    const double shape_off_ns = static_cast<double>(end.QuadPart - begin.QuadPart) * to_ns / iterations;

    // ON: one record_call per draw (hash + bounded probe + 2 atomic adds for the totals
    // and the element histogram). This is the worst case a shape-filtered build would pay
    // -- quoted so the trade-off is honest.
    perf_probe::reset_for_test();
    perf_probe::configure(true, 60000, 32);
    QueryPerformanceCounter(&begin);
    for (int i = 0; i < iterations; ++i)
    {
        const auto mark = perf_probe::make_draw_shape_mark(
            perf_probe::DrawShapeSlot::indexed(3, 0, 0));
        perf_probe::mark_draw_shape_call(mark);
    }
    QueryPerformanceCounter(&end);
    const double shape_on_ns = static_cast<double>(end.QuadPart - begin.QuadPart) * to_ns / iterations;

    // ON, cold miss: every call is a brand-new shape => the claim path has to walk the
    // probe window, and once the window is full it evicts (the B108 Top-N cost).
    perf_probe::reset_for_test();
    perf_probe::configure(true, 60000, 32);
    QueryPerformanceCounter(&begin);
    for (int i = 0; i < iterations; ++i)
    {
        const auto mark = perf_probe::make_draw_shape_mark(
            perf_probe::DrawShapeSlot::indexed(static_cast<std::uint32_t>(i + 1), 0, 0));
        perf_probe::mark_draw_shape_call(mark);
    }
    QueryPerformanceCounter(&end);
    const double shape_miss_ns = static_cast<double>(end.QuadPart - begin.QuadPart) * to_ns / iterations;

    // B109: the entry filter sits on the per-draw path, so its own cost must be quoted.
    //   - OFF (or already self-falsified) + non-3: one relaxed load and a branch;
    //   - ON + count==3 (the hot branch the filter turns into the shipped path): one compare;
    //   - ON + reject: one relaxed fetch_add (the canary sequence) + one more for the counter.
    perf_probe::reset_for_test();
    perf_probe::configure_draw_entry_filter(false, 64);
    QueryPerformanceCounter(&begin);
    for (int i = 0; i < iterations; ++i)
        perf_probe::draw_entry_filter_admit(6);
    QueryPerformanceCounter(&end);
    const double entry_filter_off_ns = static_cast<double>(end.QuadPart - begin.QuadPart) * to_ns / iterations;

    perf_probe::reset_for_test();
    perf_probe::configure_draw_entry_filter(true, 64);
    QueryPerformanceCounter(&begin);
    for (int i = 0; i < iterations; ++i)
        perf_probe::draw_entry_filter_admit(3);
    QueryPerformanceCounter(&end);
    const double entry_filter_three_ns = static_cast<double>(end.QuadPart - begin.QuadPart) * to_ns / iterations;

    perf_probe::reset_for_test();
    perf_probe::configure_draw_entry_filter(true, 64);
    QueryPerformanceCounter(&begin);
    for (int i = 0; i < iterations; ++i)
        perf_probe::draw_entry_filter_admit(6);
    QueryPerformanceCounter(&end);
    const double entry_filter_reject_ns = static_cast<double>(end.QuadPart - begin.QuadPart) * to_ns / iterations;

    std::printf("\nprobe_self_cost_ns   scope_disabled=%.2f  scope_enabled=%.2f"
                "  sampled_stride32=%.2f  maybe_flush_gated=%.2f\n",
                scope_disabled_ns, scope_enabled_ns, sampled_ns, maybe_flush_ns);
    std::printf("probe_self_cost_ns   funnel_off=%.2f  funnel_on_stride32=%.2f"
                "  shape_mark_off=%.2f  shape_mark_on=%.2f  shape_mark_on_miss=%.2f\n",
                funnel_disabled_ns, funnel_enabled_ns, shape_off_ns, shape_on_ns, shape_miss_ns);
    std::printf("probe_self_cost_ns   entry_filter_off=%.2f  entry_filter_count3=%.2f"
                "  entry_filter_reject=%.2f\n",
                entry_filter_off_ns, entry_filter_three_ns, entry_filter_reject_ns);
    std::printf("=> per frame (10 timed segments): %.2f ns  == %.5f us\n",
                scope_enabled_ns * 10.0, scope_enabled_ns * 10.0 / 1000.0);
    // Sanity bound only: a single scope must stay far below 10 us. A failure here
    // means the probe itself is broken, not that the machine is slow.
    CHECK(scope_enabled_ns < 10000.0, "probe self cost: one timed scope stays below 10 us");
    CHECK(scope_disabled_ns < 1000.0, "probe self cost: disabled scope stays below 1 us");
    CHECK(funnel_disabled_ns < 1000.0, "probe self cost: disabled funnel stays below 1 us");
    CHECK(shape_off_ns < 1000.0, "probe self cost: disabled shape mark stays below 1 us");
    CHECK(shape_miss_ns < 10000.0, "probe self cost: a cold shape mark stays below 10 us");
    // B109: the entry filter must be a branch in the shipped (off/disabled) state.
    CHECK(entry_filter_off_ns < 1000.0, "probe self cost: a disabled entry filter stays below 1 us");
    CHECK(entry_filter_three_ns < 1000.0, "probe self cost: the count==3 fast branch stays below 1 us");
    CHECK(entry_filter_reject_ns < 10000.0, "probe self cost: one rejected non-3 draw stays below 10 us");
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
    test_funnel_sampling_is_coherent();
    test_funnel_line();
    test_draw_shape_pack_roundtrip();
    test_draw_shape_table_counts();
    test_draw_shape_identified_survives_saturation();
    test_draw_shape_table_topn();
    test_draw_shape_lines();
    test_draw_shape_shadow_reject();
    test_draw_element_histogram();
    test_draw_shape_consistency_check();
    test_draw_shape_mark_is_free_when_disabled();
    test_draw_entry_filter_admits_only_three();
    test_draw_entry_filter_canary();
    test_draw_entry_filter_failsafe_fail_open();
    test_draw_entry_filter_off_records_falsification();
    test_draw_entry_filter_line();
    test_funnel_stage_names();
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
