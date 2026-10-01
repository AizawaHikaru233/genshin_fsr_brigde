// Fsr2FamilyTakeoverTest.cpp — Fsr2FamilyTakeover 状态机单元测试（不起游戏）。
// 覆盖：首帧不跳 / notify(true) 后 PRE 跳、累积与 SMAA 不跳 / notify(false) 不跳 /
//       超时不跳 / 未知哈希不跳 / 时钟回拨防御 / 计数。
#include "Fsr2FamilyTakeover.h"

#include <cstdio>
#include <cstdint>
#include <string>

namespace
{
int g_failures = 0;

void expect(bool cond, const char *what)
{
    if (!cond)
    {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

// 用连续 tick 模拟时间（ms）
constexpr std::uint64_t T0 = 1000000ull;
} // namespace

int main()
{
    using namespace fsr2_family_takeover;

    // ---- 1. 初始状态：什么都不跳 ----
    reset();
    expect(!should_skip_pre(k_pre_hash_1, T0, 500), "fresh: pre1 not skipped");
    expect(!should_skip_pre(k_pre_hash_2, T0, 500), "fresh: pre2 not skipped");
    expect(!should_skip_pre(k_pre_hash_3, T0, 500), "fresh: pre3 not skipped");
    expect(!should_skip_pre(k_pre_hash_4, T0, 500), "fresh: pre4 not skipped");

    // ---- 2. 累积被替换后：PRE 跳，累积/SMAA/未知不跳 ----
    notify_accumulate_result(true, T0 + 1);
    expect(should_skip_pre(k_pre_hash_1, T0 + 10, 500), "replaced: pre1 skipped");
    expect(should_skip_pre(k_pre_hash_2, T0 + 10, 500), "replaced: pre2 skipped");
    expect(should_skip_pre(k_pre_hash_3, T0 + 10, 500), "replaced: pre3 skipped");
    expect(should_skip_pre(k_pre_hash_4, T0 + 10, 500), "replaced: pre4 skipped");
    expect(!should_skip_pre(k_accumulate_hash, T0 + 10, 500), "replaced: accumulate NOT skipped");
    expect(!should_skip_pre(k_smaa_hash, T0 + 10, 500), "replaced: smaa NOT skipped");
    expect(!should_skip_pre(0xDEADBEEFCAFEBABEull, T0 + 10, 500), "replaced: unknown NOT skipped");

    // ---- 3. 超时：不再跳 ----
    expect(!should_skip_pre(k_pre_hash_1, T0 + 600, 500), "expired: pre1 not skipped");

    // ---- 4. notify(false)（累积替换失败）：不跳 ----
    notify_accumulate_result(false, T0 + 700);
    expect(!should_skip_pre(k_pre_hash_1, T0 + 710, 500), "notify(false): pre1 not skipped");

    // ---- 5. 再次成功：恢复跳 ----
    notify_accumulate_result(true, T0 + 800);
    expect(should_skip_pre(k_pre_hash_1, T0 + 810, 500), "re-notify(true): pre1 skipped");

    // ---- 6. 时钟回拨防御 ----
    notify_accumulate_result(true, T0 + 900);
    expect(!should_skip_pre(k_pre_hash_1, T0 + 800, 500), "clock rollback: not skipped");

    // ---- 6b. 交棒释放（notify_takeover_leave）：老接管者离开时必须**立刻**停止跳过预处理 pass ----
    // 这是缺陷①的直接修复点：只解除许可、不重置计数；随后接管者自己的累积 pass
    // 会让许可重新武装（下面第 5 步已证明该路径）。
    notify_accumulate_result(true, T0 + 950);
    expect(should_skip_pre(k_pre_hash_1, T0 + 960, 500), "before leave: pre1 skipped");
    notify_takeover_leave();
    expect(!should_skip_pre(k_pre_hash_1, T0 + 970, 500), "after leave: pre1 NOT skipped");
    expect(!should_skip_pre(k_pre_hash_4, T0 + 970, 500), "after leave: pre4 NOT skipped");
    notify_accumulate_result(true, T0 + 980);
    expect(should_skip_pre(k_pre_hash_1, T0 + 990, 500), "re-armed after leave: pre1 skipped");
    expect(takeover_leave_count() >= 1, "takeover_leave_count >= 1");

    // ---- 8. 修复②的判定点（纯函数）：两条"不可饿死"不变式必须成立（实机回归防线）----
    // 回归背景（实机）：接管者的 out_a/out_b 是**第一次成功 dispatch 时**才建立的，而
    // "多义 ⇒ 拒绝"若不含安全前提，会把那一次 bootstrap 拒掉 ⇒ 归属永远建立不起来 ⇒
    // 双实例场景下超分**完全停止**（ffx12_result 停在 14 条）。下面逐条钉住不变式。
    {
        // 基准：所有条件都成立（唯一允许拒绝的形态）
        const auto baseline = []()
        {
            TokenOnlyClaimFacts f {};
            f.single_instance_takeover = true;
            f.second_instance_present = true;
            f.other_instance_currently_live = true;
            f.claimer_is_current_taker = true;
            f.claimed_by_token_only = true;
            f.output_belongs_to_claimer = false;
            f.claim_has_generation = true;
            f.same_size_token_candidates = 2;
            f.claimer_has_ownership = true;
            f.claimer_recently_dispatched = true;
            return f;
        };
        expect(p1_refuse_token_only_claim(baseline()), "ambiguous: baseline refuses");
        // ★★ 第 1 号硬指标（2026-09-28 实机回归的回归防线）：
        //    **单实例场景下不得放行/拒绝任何依赖"多实例"前提的判据**。
        //    实机签名：`ffx12_ambiguous_bootstrap` 每帧命中一次、涨到 5376 ⇒ 接管者每个累积
        //    draw 都被放行给游戏原生 ⇒ 超分大幅降低、静止边缘抖动/锯齿。
        //    下面三条分别代表"只有一个活跃实例"的三种形态 ⇒ 都必须**不拒绝** ✓
        {
            // (1) 从未出现过第二个实例（真正干净的单实例会话）
            TokenOnlyClaimFacts f = baseline();
            f.second_instance_present = false;
            f.other_instance_currently_live = false;
            f.same_size_token_candidates = 1;
            expect(!p1_refuse_token_only_claim(f),
                   "single instance (only ever one) NEVER refuses any accumulate claim");
            // (2) 曾经有第二个实例、但它**当前不再活跃**（幽灵实例：只调 Render 不产画面）
            //     —— 这正是实机回归的形态。
            f = baseline();
            f.second_instance_present = false;
            f.other_instance_currently_live = false;
            f.same_size_token_candidates = 2; // 幽灵的同尺寸新鲜 token 仍被计入候选
            expect(!p1_refuse_token_only_claim(f),
                   "single LIVE instance (stale/ghost second instance) NEVER refuses");
            // (3) 裁决说"当前只有一个活跃实例"，但候选计数仍为 2（口径不一致的历史形态）
            f = baseline();
            f.other_instance_currently_live = false;
            expect(!p1_refuse_token_only_claim(f),
                   "no other instance currently drawing NEVER refuses");
        }
        // ★ 不变式 1：尚无归属记忆 ⇒ **绝不**拒绝（否则归属永远建立不起来 = 死锁）
        {
            TokenOnlyClaimFacts f = baseline();
            f.claimer_has_ownership = false;
            expect(!p1_refuse_token_only_claim(f),
                   "no-deadlock: empty ownership NEVER refuses (first bootstrap)");
        }
        // ★ 不变式 2：久未派发（不健康）⇒ **绝不**拒绝（拒绝不刷新时间戳 ⇒ 必须自愈）
        {
            TokenOnlyClaimFacts f = baseline();
            f.claimer_recently_dispatched = false;
            expect(!p1_refuse_token_only_claim(f),
                   "no-deadlock: unhealthy taker NEVER refuses (self-heal)");
        }
        // 两条不变式同时不成立 ⇒ 仍然放行
        {
            TokenOnlyClaimFacts f = baseline();
            f.claimer_has_ownership = false;
            f.claimer_recently_dispatched = false;
            expect(!p1_refuse_token_only_claim(f),
                   "no-deadlock: empty + unhealthy NEVER refuses");
        }
        // 输出归属已确认（path 1/3）⇒ 不是猜测而是证据 ⇒ 不拒绝
        {
            TokenOnlyClaimFacts f = baseline();
            f.output_belongs_to_claimer = true;
            expect(!p1_refuse_token_only_claim(f), "path1/3 (output owned) never refuses");
        }
        // 同尺寸 token 唯一 / 一个都没有 ⇒ 无多义 ⇒ 不拒绝
        {
            TokenOnlyClaimFacts f = baseline();
            f.same_size_token_candidates = 1;
            expect(!p1_refuse_token_only_claim(f), "unique token candidate never refuses");
            f.same_size_token_candidates = 0;
            expect(!p1_refuse_token_only_claim(f), "no token candidate never refuses");
        }
        // 模式/归属前提不成立 ⇒ 不拒绝（全量接管、单实例、非接管者、path!=2、无代次）
        {
            TokenOnlyClaimFacts f = baseline();
            f.single_instance_takeover = false;
            expect(!p1_refuse_token_only_claim(f),
                   "full takeover (Ffx12SingleInstance=0) never refuses");
            f = baseline();
            f.second_instance_present = false;
            expect(!p1_refuse_token_only_claim(f), "single instance never refuses");
            f = baseline();
            f.claimer_is_current_taker = false;
            expect(!p1_refuse_token_only_claim(f), "non-taker never refuses");
            f = baseline();
            f.claimed_by_token_only = false;
            expect(!p1_refuse_token_only_claim(f), "match_path!=2 never refuses");
            f = baseline();
            f.claim_has_generation = false;
            expect(!p1_refuse_token_only_claim(f), "claim without generation never refuses");
        }
    }

    // ---- 8b. 活性判据纯函数（**非粘性**）：`p1_other_instance_currently_live` ----
    // 这是本轮修复的核心判据：把"多实例"从"历史上出现过"改成"**当前**是否真有另一个
    // 实例在画目标累积 draw"。幽灵实例（只调 Render、不再产画面）必须**不**算活跃。
    {
        constexpr std::uint64_t kLiveMs = 3000;
        const std::uint64_t inst_a = 0xA0A0ull;
        const std::uint64_t inst_b = 0xB0B0ull;
        const std::uint64_t t = T0;
        // (1) 只有一个实例 ⇒ 排除它自己之后恒 false（单实例等价性的**判据本体**）
        {
            const std::uint64_t insts[1] = { inst_a };
            const std::uint64_t seen[1] = { t };
            expect(!p1_other_instance_currently_live(insts, seen, 1, inst_a, t, kLiveMs),
                   "live: single instance -> false (exclude self)");
            // exclude_instance=0 的语义是"不排除任何实例"（调用方传真实实例指针时用不到）
            expect(p1_other_instance_currently_live(insts, seen, 1, 0, t, kLiveMs),
                   "live: exclude none keeps the only fresh instance");
        }
        // (2) 两个实例都新鲜 ⇒ true（真·多实例，此时拒绝才是正当的）
        {
            const std::uint64_t insts[2] = { inst_a, inst_b };
            const std::uint64_t seen[2] = { t, t };
            expect(p1_other_instance_currently_live(insts, seen, 2, inst_a, t, kLiveMs),
                   "live: two fresh instances -> true");
        }
        // (3) ★ 幽灵实例：第二路"画过"但早已沉默（超出活性窗）⇒ false
        //     —— 实机回归形态：它的 token 仍然新鲜，但它**不再产画面**。
        {
            const std::uint64_t insts[2] = { inst_a, inst_b };
            const std::uint64_t seen[2] = { t, t - kLiveMs - 1 };
            expect(!p1_other_instance_currently_live(insts, seen, 2, inst_a, t, kLiveMs),
                   "live: ghost (stale second instance) -> false");
            // 恰好在窗口边界上仍算活跃
            const std::uint64_t edge[2] = { t, t - kLiveMs };
            expect(p1_other_instance_currently_live(insts, edge, 2, inst_a, t, kLiveMs),
                   "live: exactly at window edge -> true");
        }
        // (4) 从未被匹配到（last_seen == 0）⇒ 不活跃
        {
            const std::uint64_t insts[2] = { inst_a, inst_b };
            const std::uint64_t seen[2] = { t, 0 };
            expect(!p1_other_instance_currently_live(insts, seen, 2, inst_a, t, kLiveMs),
                   "live: never-matched second instance -> false");
        }
        // (5) 时钟回拨防御 + 空指针
        {
            const std::uint64_t insts[2] = { inst_a, inst_b };
            const std::uint64_t seen[2] = { t, t + 100 };
            expect(!p1_other_instance_currently_live(insts, seen, 2, inst_a, t, kLiveMs),
                   "live: clock rollback -> false");
            expect(!p1_other_instance_currently_live(nullptr, seen, 2, inst_a, t, kLiveMs),
                   "live: null instances -> false");
            expect(!p1_other_instance_currently_live(insts, nullptr, 2, inst_a, t, kLiveMs),
                   "live: null ticks -> false");
        }
    }

    // ---- 10. 【本批新增 · 问题 A】累积 draw 去向分类（被选中实例"没进 dispatch 的那些去哪了"）----
    // 背景（实机读不出来的局面）：`ffx12_single_instance_passthrough` 只统计**未选中**那一路；
    // 被选中那一路"没进 dispatch"的 draw 此前没有计数器（fail-closed 那一类完全不可见）。
    // 这里的分类口径 = 桥侧各 return 点的**决策顺序**，逐条钉住。
    {
        AccumulateDestinationFacts f {};
        // ④ 未匹配到任何实例 ⇒ untagged（与"匹配到了但没派发"必须分开）
        f = AccumulateDestinationFacts {};
        f.fail_closed = true; // 未匹配时也会走到函数末 fail-closed，但归类必须是 untagged
        expect(classify_accumulate_destination(f) == AccumulateDestination::Untagged,
               "dest: unmatched -> untagged (not fail_closed)");
        // ② 未被选中 ⇒ 放行原生
        f = AccumulateDestinationFacts {};
        f.matched = true;
        f.taker_branch_passthrough = true;
        expect(classify_accumulate_destination(f) == AccumulateDestination::PassthroughNotTaker,
               "dest: not selected -> passthrough_not_taker");
        // ② 交棒后的纯原生窗口
        f = AccumulateDestinationFacts {};
        f.matched = true;
        f.native_window = true;
        expect(classify_accumulate_destination(f) == AccumulateDestination::PassthroughNativeWindow,
               "dest: native window -> passthrough_native_window");
        // ② 修复②多义拒绝
        f = AccumulateDestinationFacts {};
        f.matched = true;
        f.refused_ambiguous = true;
        expect(classify_accumulate_destination(f) == AccumulateDestination::PassthroughRefuse,
               "dest: refuse -> passthrough_refuse");
        // ② E2 严格匹配（默认关）
        f = AccumulateDestinationFacts {};
        f.matched = true;
        f.strict_skip = true;
        expect(classify_accumulate_destination(f) == AccumulateDestination::PassthroughStrict,
               "dest: strict skip -> passthrough_strict");
        // ⑤ 纹理取不到（走 fail-closed 的那一支，但必须与"纯 fail-closed"分开）
        f = AccumulateDestinationFacts {};
        f.matched = true;
        f.textures_missing = true;
        f.fail_closed = true;
        expect(classify_accumulate_destination(f) == AccumulateDestination::TextureMissing,
               "dest: textures missing -> texture_missing (before fail_closed)");
        // ① 真正派发 / ①' 修复拷贝 / ①'' 复用拷贝（三者必须可区分）
        f = AccumulateDestinationFacts {};
        f.matched = true;
        f.dispatch_ok = true;
        expect(classify_accumulate_destination(f) == AccumulateDestination::Dispatch,
               "dest: dispatch -> dispatch");
        f = AccumulateDestinationFacts {};
        f.matched = true;
        f.repair_copy = true;
        expect(classify_accumulate_destination(f) == AccumulateDestination::DispatchRepair,
               "dest: repair copy -> dispatch_repair");
        f = AccumulateDestinationFacts {};
        f.matched = true;
        f.reuse_copy = true;
        expect(classify_accumulate_destination(f) == AccumulateDestination::DispatchReuse,
               "dest: reuse copy -> dispatch_reuse");
        // ⑤ 派发失败（fail-open 交回原生）
        f = AccumulateDestinationFacts {};
        f.matched = true;
        f.dispatch_failed = true;
        expect(classify_accumulate_destination(f) == AccumulateDestination::DispatchFailed,
               "dest: dispatch failed -> dispatch_failed");
        // ★★ ③ 被 fail-closed 吞掉：**必须自成一类**
        //    （实机读不出来的正是这一类：既不派发、也不放行、且 `ffx12_skip` 被抑制）
        f = AccumulateDestinationFacts {};
        f.matched = true;
        f.fail_closed = true;
        expect(classify_accumulate_destination(f) == AccumulateDestination::FailClosed,
               "dest: fail_closed -> fail_closed_swallowed (its own bucket)");
        // 判定顺序（重叠事实）：
        //   · 派发成功 + fail_closed 同时置位 ⇒ 按顺序先命中 dispatch（fail_closed 是函数末兜底）
        f = AccumulateDestinationFacts {};
        f.matched = true;
        f.dispatch_ok = true;
        f.fail_closed = true;
        expect(classify_accumulate_destination(f) == AccumulateDestination::Dispatch,
               "dest precedence: dispatch wins over fail_closed");
        //   · 未选中 + 其它一切 ⇒ 仍是 passthrough_not_taker（该分支最先返回）
        f = AccumulateDestinationFacts {};
        f.matched = true;
        f.taker_branch_passthrough = true;
        f.dispatch_ok = true;
        f.native_window = true;
        f.refused_ambiguous = true;
        expect(classify_accumulate_destination(f) == AccumulateDestination::PassthroughNotTaker,
               "dest precedence: not_taker wins over everything after it");
        //   · 多义拒绝先于严格匹配 / 修复拷贝
        f = AccumulateDestinationFacts {};
        f.matched = true;
        f.refused_ambiguous = true;
        f.strict_skip = true;
        f.repair_copy = true;
        expect(classify_accumulate_destination(f) == AccumulateDestination::PassthroughRefuse,
               "dest precedence: refuse wins over strict/repair");
        // 类别名：五个去向都必须有**互不相同**的名字（日志可判读的前提）
        const char *names[] = {
            accumulate_destination_name(AccumulateDestination::Dispatch),
            accumulate_destination_name(AccumulateDestination::PassthroughNotTaker),
            accumulate_destination_name(AccumulateDestination::PassthroughRefuse),
            accumulate_destination_name(AccumulateDestination::FailClosed),
            accumulate_destination_name(AccumulateDestination::Untagged),
        };
        for (int i = 0; i < 5; ++i)
        {
            expect(names[i] != nullptr && names[i][0] != '\0', "dest: name non-empty");
            for (int j = i + 1; j < 5; ++j)
                expect(std::string(names[i]) != std::string(names[j]), "dest: names distinct");
        }
    }

    // ---- 11. 【本批新增 · 问题 B】`ffx12_input_alias` 判据本体 + 并发/陈旧分类 ----
    // 结论（本批要钉住的）：该判据比较的是**两个裸指针**（color+depth 同时相同），
    // **没有**活性/新鲜度条件 ⇒ 它不能单独证明"两路在共用输入"。
    {
        constexpr std::uint64_t kRecentMs = 100;
        InputAliasFacts f {};
        f.current_color = 0xC0FFEEull;
        f.current_depth = 0xDEEFull;
        f.other_color = 0xC0FFEEull;
        f.other_depth = 0xDEEFull;
        f.other_record_tick = T0;
        f.now_ms = T0 + 10;
        f.recent_ms = kRecentMs;
        // (1) 指针不同 ⇒ 不是别名
        {
            InputAliasFacts g = f;
            g.other_color = 0xBEEFull;
            expect(classify_input_alias(g) == InputAliasKind::None, "alias: different color -> none");
            g = f;
            g.other_depth = 0xBEEFull;
            expect(classify_input_alias(g) == InputAliasKind::None,
                   "alias: color same but depth different -> none (both must match)");
            g = f;
            g.current_color = 0;
            expect(classify_input_alias(g) == InputAliasKind::None, "alias: zero current color -> none");
            g = f;
            g.other_color = 0;
            expect(classify_input_alias(g) == InputAliasKind::None, "alias: no other record -> none");
        }
        // (2) 同指针 + 另一实例**仍在派发**（时间新 / 活性表命中）⇒ Concurrent
        {
            expect(classify_input_alias(f) == InputAliasKind::Concurrent,
                   "alias: same pointers + fresh record -> concurrent");
            InputAliasFacts g = f;
            g.other_record_tick = 0; // 记录时刻未知
            g.other_currently_live = true;
            expect(classify_input_alias(g) == InputAliasKind::Concurrent,
                   "alias: same pointers + other live -> concurrent");
            g = f;
            g.other_record_tick = T0;
            g.now_ms = T0 + kRecentMs; // 恰好在窗口边界
            expect(classify_input_alias(g) == InputAliasKind::Concurrent,
                   "alias: exactly at recent window edge -> concurrent");
        }
        // ★★ (3) 同指针 + 另一实例**早已静默** ⇒ StaleRecord
        //     实机形态：切场景/视图重建后，新实例的输入纹理地址与**已停止派发**的老实例
        //     记录相同 ⇒ 旧实现会把它读成"两路共用输入"（进而推出"覆盖层=别名"的错判）。
        {
            InputAliasFacts g = f;
            g.now_ms = T0 + 3000;
            expect(classify_input_alias(g) == InputAliasKind::StaleRecord,
                   "alias: same pointers + silent other -> stale_record (NOT proof of sharing)");
            g = f;
            g.other_record_tick = 0;
            g.other_currently_live = false;
            expect(classify_input_alias(g) == InputAliasKind::StaleRecord,
                   "alias: same pointers + unknown/live=0 -> stale_record");
            g = f;
            g.other_record_tick = T0 + 100; // 时钟回拨（now < tick）⇒ 不得当成"新鲜"
            g.now_ms = T0;
            expect(classify_input_alias(g) == InputAliasKind::StaleRecord,
                   "alias: clock rollback -> stale_record");
        }
        // (4) 判据本体：只有 (color,depth) **双同**才算
        expect(input_alias_same_pointers(f), "alias: same pointers -> true");
        {
            InputAliasFacts g = f;
            g.current_depth = 0x1234ull;
            expect(!input_alias_same_pointers(g), "alias: depth mismatch -> false");
        }
    }

    // ---- 12. 【本批 2026-09-29 · 分屏抖动】"接管者自己的 draw 不得被无故拒绝" ----
    // 实机形态（B123 探针 `ffx12_accum_dest`，左右 1:1 分屏）：被选中那一路
    //   seen=17561 / disp=2393（13.6%）/ pass_refuse=15119（86%）
    //   ⇒ 那半屏在 FSR4 与游戏原生之间**来回切** = 用户看到的可见抖动。
    // 机制（代码 + 数据逐条对齐）：
    //   · 两路 render 尺寸相同 ⇒ bootstrap 只能按"token 代次最新"认领（match_path=2）；
    //   · 接管者有两个输出缓冲轮换，而 out_b **只在成功派发后**才被记下；
    //   · 写第二个缓冲的 draw ⇒ path 2 + 无输出归属 ⇒ 被拒 ⇒ 那次"能学会 out_b"的派发
    //     永远不发生（**自锁**）。
    // 修法：正面证据从"只有输出归属"扩到"输出归属 **或** 输入归属"（本 draw 采样的 color
    //   就是认领者上次派发用的那一张 ⇒ 是同一路视图）。下面逐条钉住：
    //   (a) 输入归属成立 ⇒ **不得**拒绝（否则抖动不消失）；
    //   (b) 两种归属都不成立 ⇒ 仍然拒绝（另一路的帧不得喂进本实例历史 ⇒ 覆盖层防线不许松）。
    {
        const auto hit = []()
        {
            TokenOnlyClaimFacts f {};
            f.single_instance_takeover = true;
            f.second_instance_present = true;
            f.other_instance_currently_live = true;
            f.claimer_is_current_taker = true;
            f.claimed_by_token_only = true;
            f.output_belongs_to_claimer = false;
            f.input_belongs_to_claimer = false;
            f.claim_has_generation = true;
            f.same_size_token_candidates = 2;
            f.claimer_has_ownership = true;
            f.claimer_recently_dispatched = true;
            return f;
        };
        // (1) 判据本体：两种正面证据任一成立即"已有证据"
        {
            TokenOnlyClaimFacts f = hit();
            expect(!claim_has_positive_evidence(f), "evidence: neither output nor input -> false");
            f.output_belongs_to_claimer = true;
            expect(claim_has_positive_evidence(f), "evidence: output owned -> true");
            f = hit();
            f.input_belongs_to_claimer = true;
            expect(claim_has_positive_evidence(f), "evidence: input owned -> true");
            f.output_belongs_to_claimer = true;
            expect(claim_has_positive_evidence(f), "evidence: both -> true");
        }
        // ★★ (2) **核心钉住**：接管者写自己**第二个输出缓冲**的那一帧（path 2、out_b 还没学到，
        //     但 color 就是我们自己的）**绝不能被拒** ⇒ 它必须能派发 ⇒ 学会 out_b ⇒ 不再来回切。
        //     这条是"分屏可见抖动"的直接反例 —— 旧实现（只看输出归属）在这里返回 true（拒绝）。
        {
            TokenOnlyClaimFacts f = hit();
            f.input_belongs_to_claimer = true;
            f.claimer_has_ownership = true; // out_a 已学到，out_b 还没有（= 实机自锁形态）
            expect(!p1_refuse_token_only_claim(f),
                   "NO-JITTER: taker's own draw (input owned, out_b not yet learned) NEVER refused");
        }
        // (3) 覆盖层防线不许松：两种归属都不成立（= 采的与写的都不是我们的）⇒ 仍然拒绝
        {
            expect(p1_refuse_token_only_claim(hit()),
                   "OVERLAY-GUARD: claim with neither output nor input evidence STILL refused");
        }
        // (4) 既有安全不变式与本批改动正交：无输入记忆时（acc_in_color=0）不得因"输入归属"
        //     而拒绝 —— 判据的方向是"有证据 ⇒ 放行"，**不会**因为缺证据而变成新的拒绝理由。
        {
            TokenOnlyClaimFacts f = hit();
            f.input_belongs_to_claimer = false;
            f.claimer_has_ownership = false; // 还没有任何归属记忆（首次 bootstrap）
            expect(!p1_refuse_token_only_claim(f),
                   "no-deadlock: first bootstrap (no memory at all) NEVER refused");
        }
        // (5) 【本批 · draw→实例 归属】bootstrap 候选的"按采样来源排除"判据：
        //     分屏里另一路的 draw 会因"token 代次最新"被认到接管者头上（接管者的 `seen`
        //     混进别人的 draw）⇒ 用采样来源（每路自己的 G-buffer）把它排掉。
        {
            // 来源命中 ⇒ 是它的 ⇒ **不排除**（调用方把它当首选）
            expect(!p1_exclude_candidate_by_source(true, true, true),
                   "attr: source matches -> NEVER excluded (it is ours)");
            expect(!p1_exclude_candidate_by_source(true, true, false),
                   "attr: source matches (even stale memory) -> NEVER excluded");
            // 没有来源记忆（另一路从未派发过）⇒ 不排除
            expect(!p1_exclude_candidate_by_source(false, false, true),
                   "attr: no source memory -> NEVER excluded");
            expect(!p1_exclude_candidate_by_source(false, false, false),
                   "attr: no source memory & cold -> NEVER excluded");
            // ★ 记忆新鲜 + 来源确非本 draw ⇒ 排除（这才是"不是它的"）
            expect(p1_exclude_candidate_by_source(true, false, true),
                   "attr: fresh memory + different source -> excluded");
            // ★★ 防死锁不变式：记忆可能过期（切视图/切场景）⇒ **不许**排除
            //    （否则接管者换了 G-buffer 之后会既没有正面证据、又被自己排除 ⇒ 永久走原生）
            expect(!p1_exclude_candidate_by_source(true, false, false),
                   "NO-STARVE: stale memory + different source -> NEVER excluded (self-heal)");
        }
    }

    // ---- 13. 【本批 2026-09-29 · B125】分屏：**depth/motion 共用**与**抖动冻结**的判定 ----
    // 实机形态（`Ffx12SingleInstance=0` · 两路都在派发 · 32 秒 16 行 × 2 实例）：
    //   甲 in=0xCE9C5AE9B0/0xCE9C5E86F0/0xCE9C5B2D30/…  乙 in=0xCE9C5E57F0/0xCE9C5E86F0/0xCE9C5B2D30/…
    //   ⇒ color / out 各不同；**depth 与 motion 是同一份**。
    // 本节钉住两件事（**只钉判据，不改任何决策**）：
    //   (a) 这个"同一份"必须被读成**游戏把同一份几何输入绑给了两路**，而**不是**别名/归属错配：
    //       判为共用的条件是 —— 另一路**在线** + color **不同** + depth/motion 同指针；
    //       陈旧记录（另一路早已静默）与"同一路重建"（color 相同）都**不许**算共用；
    //   (b) 抖动"与上一帧**逐位相同**"⇒ 判为**冻结**（= 没有亚像素采样多样性，时域累积无法收敛）；
    //       而首帧没有可比对象 ⇒ 必须算"新"（不许把每一路的首帧误报成冻结）。
    {
        SharedGeometryFacts f {};
        f.self_color = 0xCE9C5AE9B0ull;
        f.self_depth = 0xCE9C5E86F0ull;
        f.self_motion = 0xCE9C5B2D30ull;
        f.other_color = 0xCE9C5E57F0ull;
        f.other_depth = 0xCE9C5E86F0ull;
        f.other_motion = 0xCE9C5B2D30ull;
        f.other_currently_live = true;
        // ★★ (a) 实机形态：另一路在线、color 不同、depth+motion 同指针
        expect(classify_shared_geometry(f) == SharedGeometryKind::DepthAndMotion,
               "shared: live other + different color + same depth&motion -> depth_and_motion");
        expect(std::string(shared_geometry_kind_name(SharedGeometryKind::DepthAndMotion)) == "depth_and_motion",
               "shared: kind name for depth_and_motion");
        {
            SharedGeometryFacts g = f;
            g.other_motion = 0xDEADBEEFull;
            expect(classify_shared_geometry(g) == SharedGeometryKind::DepthOnly,
                   "shared: only depth same -> depth_only");
        }
        {
            SharedGeometryFacts g = f;
            g.other_depth = 0x1234ull;
            expect(classify_shared_geometry(g) == SharedGeometryKind::MotionOnly,
                   "shared: only motion same -> motion_only");
        }
        {
            // ★ 陈旧记录（另一条记录早已静默）⇒ **不算**"两路共用"（B123 已定口径）
            SharedGeometryFacts g = f;
            g.other_currently_live = false;
            expect(classify_shared_geometry(g) == SharedGeometryKind::None,
                   "shared: stale record (other not live) -> NONE (NOT proof of sharing)");
        }
        {
            // ★ 同一路（color 相同 = 视图/实例重建复用同一张 G-buffer）⇒ 不是"两路"
            SharedGeometryFacts g = f;
            g.other_color = f.self_color;
            expect(classify_shared_geometry(g) == SharedGeometryKind::None,
                   "shared: same color -> NONE (same view rebuilt, not two views)");
        }
        {
            // 本路还没成功派发过 ⇒ 不做任何断言（避免把"没记录"读成"没共用"）
            SharedGeometryFacts g = f;
            g.self_color = 0;
            g.self_depth = 0;
            g.self_motion = 0;
            expect(classify_shared_geometry(g) == SharedGeometryKind::None,
                   "shared: no self record -> NONE (nothing to assert)");
        }
        // ★★ (b) 抖动新鲜度：冻结必须被判出来，首帧必须算"新"
        {
            const JitterFreshness first =
                classify_jitter_freshness(-0.0625f, 0.462963f, 0.0f, 0.0f, false, 0);
            expect(first.fresh && first.frozen_frames == 0 && !first.has_previous,
                   "jitter: first frame (no previous) -> fresh");
            const JitterFreshness same =
                classify_jitter_freshness(-0.0625f, 0.462963f, -0.0625f, 0.462963f, true, 0);
            expect(!same.fresh && same.frozen_frames == 1,
                   "jitter: identical to previous -> FROZEN (no sub-pixel diversity)");
            const JitterFreshness same2 =
                classify_jitter_freshness(-0.0625f, 0.462963f, -0.0625f, 0.462963f, true, 12);
            expect(!same2.fresh && same2.frozen_frames == 13,
                   "jitter: frozen counter accumulates (13 = 12+1)");
            const JitterFreshness moved =
                classify_jitter_freshness(0.25f, -0.166667f, -0.0625f, 0.462963f, true, 13);
            expect(moved.fresh && moved.frozen_frames == 0,
                   "jitter: changed -> fresh and frozen counter reset");
            // 只差一个分量也必须算"有变化"（抖动是成对的）
            const JitterFreshness half =
                classify_jitter_freshness(-0.0625f, 0.5f, -0.0625f, 0.462963f, true, 13);
            expect(half.fresh && half.frozen_frames == 0,
                   "jitter: only Y changed -> fresh");
        }
    }

    // ---- 9. 计数 ----
    expect(skipped_count() >= 6, "skipped_count >= 6");
    expect(accumulate_replaced_count() >= 4, "accumulate_replaced_count >= 4");

    if (g_failures == 0)
    {
        std::printf("Fsr2FamilyTakeoverTest: ALL PASS (skipped=%llu replaced=%llu leave=%llu)\n",
            static_cast<unsigned long long>(skipped_count()),
            static_cast<unsigned long long>(accumulate_replaced_count()),
            static_cast<unsigned long long>(takeover_leave_count()));
        return 0;
    }
    std::printf("Fsr2FamilyTakeoverTest: %d FAILURE(S)\n", g_failures);
    return 1;
}
