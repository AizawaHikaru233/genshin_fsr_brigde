#include "Fsr2FamilyTakeover.h"

#include <atomic>
#include <cstdint>

namespace fsr2_family_takeover
{
namespace
{
std::atomic_bool g_accumulate_replaced { false };
std::atomic_uint64_t g_accumulate_tick { 0 };
std::atomic_uint64_t g_skipped { 0 };
std::atomic_uint64_t g_replaced { 0 };
std::atomic_uint64_t g_leave_releases { 0 };
} // namespace

void reset()
{
    g_accumulate_replaced.store(false, std::memory_order_relaxed);
    g_accumulate_tick.store(0, std::memory_order_relaxed);
}

bool is_pre_pass(std::uint64_t hash)
{
    return hash == k_pre_hash_1 || hash == k_pre_hash_2 || hash == k_pre_hash_3 || hash == k_pre_hash_4;
}

bool is_accumulate(std::uint64_t hash)
{
    return hash == k_accumulate_hash;
}

bool is_smaa(std::uint64_t hash)
{
    return hash == k_smaa_hash;
}

bool should_skip_pre(std::uint64_t hash, std::uint64_t now_ms, std::uint64_t expire_ms)
{
    if (!is_pre_pass(hash))
        return false;
    if (!g_accumulate_replaced.load(std::memory_order_relaxed))
        return false;
    const std::uint64_t last = g_accumulate_tick.load(std::memory_order_relaxed);
    if (last == 0)
        return false;
    if (now_ms < last)
        return false; // 时钟回拨防御
    if (now_ms - last > expire_ms)
        return false;
    g_skipped.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void notify_accumulate_result(bool replaced_ok, std::uint64_t now_ms)
{
    if (replaced_ok)
    {
        g_accumulate_replaced.store(true, std::memory_order_relaxed);
        g_accumulate_tick.store(now_ms, std::memory_order_relaxed);
        g_replaced.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        g_accumulate_replaced.store(false, std::memory_order_relaxed);
    }
}

void notify_takeover_leave()
{
    // 只解除许可（不清计数）：下一次 notify_accumulate_result(true) 会重新武装。
    // 这里**不看** now_ms —— 语义是"立刻、无条件地不再跳过预处理 pass"。
    g_accumulate_replaced.store(false, std::memory_order_relaxed);
    g_leave_releases.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t skipped_count()
{
    return g_skipped.load(std::memory_order_relaxed);
}

std::uint64_t accumulate_replaced_count()
{
    return g_replaced.load(std::memory_order_relaxed);
}

std::uint64_t takeover_leave_count()
{
    return g_leave_releases.load(std::memory_order_relaxed);
}

bool p1_refuse_token_only_claim(const TokenOnlyClaimFacts &facts)
{
    // 只有在 P1 单实例接管 + 本进程确实并存多个实例时，才存在"这条路可能属于别人"的风险。
    if (!facts.single_instance_takeover || !facts.second_instance_present)
        return false;
    if (!facts.claimer_is_current_taker || !facts.claim_has_generation)
        return false;
    // 只针对"仅靠 token 认领"：输出归属已确认（path 1/3）时不是猜测，而是证据。
    if (!facts.claimed_by_token_only || facts.output_belongs_to_claimer)
        return false;
    // `same_size_token_candidates > 1` 只是"**没有正面证据**说明这 draw 是我们的"：
    // 两路 render 尺寸相同时它恒为 2（实机 match_unverified 与 ambiguous 同步增长即此）
    // ⇒ **不能**把它当成"这 draw 属于别人"的证据，只能配合下面两条安全前提使用。
    if (facts.same_size_token_candidates <= 1)
        return false;
    // ★★ 两条"不可饿死"不变式（实机回归防线）：
    //    无归属记忆时拒绝 ⇒ 归属永远建立不起来（双实例下超分完全停止）；
    //    不健康（久未派发）时拒绝 ⇒ 拒绝不刷新派发时间戳 ⇒ 会一直拒绝（同样死锁）。
    if (!facts.claimer_has_ownership || !facts.claimer_recently_dispatched)
        return false;
    return true;
}
} // namespace fsr2_family_takeover
