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

std::uint64_t skipped_count()
{
    return g_skipped.load(std::memory_order_relaxed);
}

std::uint64_t accumulate_replaced_count()
{
    return g_replaced.load(std::memory_order_relaxed);
}
} // namespace fsr2_family_takeover
