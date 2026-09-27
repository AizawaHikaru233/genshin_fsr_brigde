// Fsr2FamilyTakeoverTest.cpp — Fsr2FamilyTakeover 状态机单元测试（不起游戏）。
// 覆盖：首帧不跳 / notify(true) 后 PRE 跳、累积与 SMAA 不跳 / notify(false) 不跳 /
//       超时不跳 / 未知哈希不跳 / 时钟回拨防御 / 计数。
#include "Fsr2FamilyTakeover.h"

#include <cstdio>
#include <cstdint>

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
