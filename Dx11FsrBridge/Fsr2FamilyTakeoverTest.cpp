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

    // ---- 7. 计数 ----
    expect(skipped_count() >= 5, "skipped_count >= 5");
    expect(accumulate_replaced_count() >= 3, "accumulate_replaced_count >= 3");

    if (g_failures == 0)
    {
        std::printf("Fsr2FamilyTakeoverTest: ALL PASS (skipped=%llu replaced=%llu)\n",
            static_cast<unsigned long long>(skipped_count()),
            static_cast<unsigned long long>(accumulate_replaced_count()));
        return 0;
    }
    std::printf("Fsr2FamilyTakeoverTest: %d FAILURE(S)\n", g_failures);
    return 1;
}
