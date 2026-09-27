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

    // ---- 6b. 交棒释放（）：老接管者离开时必须**立刻**停止跳过预处理 pass ----
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
