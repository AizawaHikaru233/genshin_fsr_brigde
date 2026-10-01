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

bool claim_has_positive_evidence(const TokenOnlyClaimFacts &facts)
{
    // 两种正面证据任一成立，就说明"这个 draw 是本实例自己的"，不是猜测：
    //   · 输出归属：它写的就是我们记过的输出缓冲（path 1/3 已确认）；
    //   · 输入归属：它采样用的 color 就是我们上一次派发用的那一张（同一路视图）。
    return facts.output_belongs_to_claimer || facts.input_belongs_to_claimer;
}

bool p1_exclude_candidate_by_source(bool source_known, bool source_matches, bool memory_fresh)
{
    // 来源命中 ⇒ 是它的（不是"排除"这一侧的判断，调用方会把它当首选）。
    if (source_matches)
        return false;
    // 没有来源记忆（例如另一路从未派发过）⇒ 无法据此排除。
    if (!source_known)
        return false;
    // 记忆可能过期（接管者换来换去地切视图 / 切场景）⇒ **不许排除**：
    // 这是"绝不饿死接管者"的同一条不变式 —— 排除掉的候选若正是它自己，
    // 它会既没有正面证据、又被自己排除 ⇒ 永远认不出自己的 draw（视图永久走原生）。
    // 记忆新鲜性由调用方按 k_p1_bootstrap_health_ms 判定（与拒绝判据同一个健康口径）。
    if (!memory_fresh)
        return false;
    // 记忆是新的、且确与本 draw 的来源不同 ⇒ 这个 draw 不是它的。
    return true;
}

bool p1_refuse_token_only_claim(const TokenOnlyClaimFacts &facts)
{
    // 只有在 P1 单实例接管 + **当前确实**还存在另一个活跃实例时，才存在"这条路可能属于别人"的风险。
    // ⚠️ 两个条件都必须是"当前"语义（实机回归 2026-09-28）：粘性条件 + 幽灵实例会让接管者
    // 每个累积 draw 都被拒 ⇒ 普通单视图场景超分大幅降低、静止抖动/锯齿。
    if (!facts.single_instance_takeover || !facts.second_instance_present ||
        !facts.other_instance_currently_live)
        return false;
    if (!facts.claimer_is_current_taker || !facts.claim_has_generation)
        return false;
    // 只针对"仅靠 token 认领"且**没有任何正面证据**的形态：
    //   · 输出归属已确认（path 1/3）⇒ 不是猜测，而是证据；
    //   · 输入归属已确认（本批新增）⇒ 这个 draw 用的就是我们自己的 G-buffer，同样是证据。
    // ⚠️ 少一条都会退化成"接管者写自己第二个输出缓冲的每一帧都被拒"（自锁）。
    if (!facts.claimed_by_token_only || claim_has_positive_evidence(facts))
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

// ★★【发布构建裁剪（2026-10-01 · 收尾清理）】下面两组到本文件末尾都是**只喂日志**的纯函数
//   （问题 A 的去向分类 / 问题 B 的输入别名分类 / B125 的共享几何与抖动新鲜度）——
//   没有任何消费者改变行为 ⇒ 发布构建整体排除，诊断构建全部保留。
//   ⚠️ 中间的 `p1_other_instance_currently_live` 是**功能**（P1 多实例活性判据）⇒ 留在门外。
//   ⚠️ 上面的 `claim_has_positive_evidence` / `p1_exclude_candidate_by_source` 是 **B124 功能**
//      （修分屏抖动）⇒ 同样留在门外。
#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)
// ---- 【本批新增 · 问题 A】累积 draw 去向分类（纯函数；判定顺序即分类口径）----
const char *accumulate_destination_name(AccumulateDestination destination)
{
    switch (destination)
    {
    case AccumulateDestination::Dispatch: return "dispatch";
    case AccumulateDestination::DispatchRepair: return "dispatch_repair";
    case AccumulateDestination::DispatchReuse: return "dispatch_reuse";
    case AccumulateDestination::PassthroughNotTaker: return "passthrough_not_taker";
    case AccumulateDestination::PassthroughRefuse: return "passthrough_refuse";
    case AccumulateDestination::PassthroughNativeWindow: return "passthrough_native_window";
    case AccumulateDestination::PassthroughStrict: return "passthrough_strict";
    case AccumulateDestination::FailClosed: return "fail_closed_swallowed";
    case AccumulateDestination::DispatchFailed: return "dispatch_failed";
    case AccumulateDestination::TextureMissing: return "texture_missing";
    case AccumulateDestination::Untagged: return "untagged";
    case AccumulateDestination::None: break;
    }
    return "none";
}

AccumulateDestination classify_accumulate_destination(const AccumulateDestinationFacts &facts)
{
    // ④ 没被匹配到任何实例（最前置：后面所有分支都要求 matched）
    if (!facts.matched)
        return AccumulateDestination::Untagged;
    // ② 未被选中 ⇒ 分支直接放行游戏原生（选举结论）
    if (facts.taker_branch_passthrough)
        return AccumulateDestination::PassthroughNotTaker;
    // ② 交棒后的纯原生窗口（位置在选举之后、归属判定之前）
    if (facts.native_window)
        return AccumulateDestination::PassthroughNativeWindow;
    // ② 修复②多义拒绝（消费 token 后放行）
    if (facts.refused_ambiguous)
        return AccumulateDestination::PassthroughRefuse;
    // ② E2 严格匹配实验（默认关）
    if (facts.strict_skip)
        return AccumulateDestination::PassthroughStrict;
    // ⑤ 其它：纹理没取全 ⇒ 走 invalidate + 函数末 fail-closed
    if (facts.textures_missing)
        return AccumulateDestination::TextureMissing;
    // ①' 输出修复拷贝（同代次配对 draw；不推进历史）
    if (facts.repair_copy)
        return AccumulateDestination::DispatchRepair;
    // ①'' 同代次复用拷贝（不推进历史）
    if (facts.reuse_copy)
        return AccumulateDestination::DispatchReuse;
    // ① 真正派发
    if (facts.dispatch_ok)
        return AccumulateDestination::Dispatch;
    // ⑤ 其它：派发失败（fail-open 交回原生）
    if (facts.dispatch_failed)
        return AccumulateDestination::DispatchFailed;
    // ③ 被 fail-closed 吞掉（既不派发、也不放行）
    if (facts.fail_closed)
        return AccumulateDestination::FailClosed;
    return AccumulateDestination::None;
}

// ---- 【本批新增 · 问题 B】输入别名判据本体（纯函数）----
bool input_alias_same_pointers(const InputAliasFacts &facts)
{
    // 刻意**同时**要求 color 与 depth 两个指针都相同 —— 与桥侧原判据逐字一致。
    // ⚠️ 判据比较的是**裸指针**（不是资源键/尺寸）：同一块地址被游戏释放后重新分配
    //    也会命中，因此"同指针"本身并不蕴含"同一份输入被两路同时使用"。
    if (facts.current_color == 0 || facts.current_depth == 0)
        return false;
    if (facts.other_color == 0)
        return false; // 另一实例没有记录 ⇒ 不构成比较
    return facts.current_color == facts.other_color && facts.current_depth == facts.other_depth;
}

InputAliasKind classify_input_alias(const InputAliasFacts &facts)
{
    if (!input_alias_same_pointers(facts))
        return InputAliasKind::None;
    // "仍在派发"的两种证据（任一成立即算并发）：
    //   ① 活性表：该实例最近被本函数匹配到过（调用方给的是 p1 活性表口径）；
    //   ② 时间：它上次派发的记录在 recent_ms 内（一帧以上 ⇒ 两路确实在同时派发）。
    if (facts.other_currently_live)
        return InputAliasKind::Concurrent;
    if (facts.other_record_tick != 0 && facts.now_ms >= facts.other_record_tick &&
        facts.now_ms - facts.other_record_tick <= facts.recent_ms)
        return InputAliasKind::Concurrent;
    // 同名指针、但那条记录早已静默 ⇒ 陈旧记录 / 指针复用（**不是**两路共用的证据）
    return InputAliasKind::StaleRecord;
}
#endif // DX11FSRBRIDGE_ENABLE_DIAGNOSTICS（只喂日志：去向分类 + 输入别名分类）

bool p1_other_instance_currently_live(const std::uint64_t *instances,
                                      const std::uint64_t *last_seen_ms,
                                      std::size_t count,
                                      std::uint64_t exclude_instance,
                                      std::uint64_t now_ms,
                                      std::uint64_t live_ms)
{
    if (instances == nullptr || last_seen_ms == nullptr)
        return false;
    for (std::size_t i = 0; i < count; ++i)
    {
        const std::uint64_t inst = instances[i];
        if (inst == 0 || inst == exclude_instance)
            continue;
        const std::uint64_t seen = last_seen_ms[i];
        if (seen == 0)
            continue; // 从未被匹配到 ⇒ 不活跃（幽灵实例或全新实例）
        if (now_ms < seen)
            continue; // 时钟回拨防御：当作"未见"
        if (now_ms - seen <= live_ms)
            return true; // 确实还有另一个实例在画 ⇒ 当前真有多个活跃实例
    }
    return false;
}

#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)
// ---- 【本批 2026-09-29 · B125】"两路共用同一份 depth/motion"的分类（纯函数；只喂日志）----
const char *shared_geometry_kind_name(SharedGeometryKind kind)
{
    switch (kind)
    {
    case SharedGeometryKind::DepthOnly: return "depth_only";
    case SharedGeometryKind::MotionOnly: return "motion_only";
    case SharedGeometryKind::DepthAndMotion: return "depth_and_motion";
    case SharedGeometryKind::None: break;
    }
    return "none";
}

SharedGeometryKind classify_shared_geometry(const SharedGeometryFacts &facts)
{
    // 没有可比的本路输入（本实例还没成功派发过）⇒ 不做任何断言（避免把"没记录"读成"没共用"）
    if (facts.self_color == 0 && facts.self_depth == 0 && facts.self_motion == 0)
        return SharedGeometryKind::None;
    // 另一条记录不存在 / 早已静默 ⇒ 陈旧记录，**不算**"两路共用"（B123 已定口径）
    if (!facts.other_currently_live)
        return SharedGeometryKind::None;
    // color 相同 ⇒ 那是**同一路**（视图/实例重建后复用同一张 G-buffer）⇒ 不是"两路"
    if (facts.self_color != 0 && facts.other_color == facts.self_color)
        return SharedGeometryKind::None;
    // 只有"另一路在线、且 color 不是同一张"时，depth/motion 同指针才是"两路共用几何输入"
    const bool depth_same = facts.self_depth != 0 && facts.other_depth == facts.self_depth;
    const bool motion_same = facts.self_motion != 0 && facts.other_motion == facts.self_motion;
    if (depth_same && motion_same)
        return SharedGeometryKind::DepthAndMotion;
    if (depth_same)
        return SharedGeometryKind::DepthOnly;
    if (motion_same)
        return SharedGeometryKind::MotionOnly;
    return SharedGeometryKind::None;
}

// ---- 【本批 2026-09-29 · B125】抖动新鲜度（纯函数；只喂日志）----
JitterFreshness classify_jitter_freshness(float current_x, float current_y,
                                          float previous_x, float previous_y,
                                          bool has_previous,
                                          std::uint32_t frozen_frames_so_far)
{
    JitterFreshness out;
    if (!has_previous)
    {
        // 首帧没有可比对象 ⇒ 视为"新"（否则会把每一路的首帧都误报成冻结）
        out.fresh = true;
        out.frozen_frames = 0;
        out.has_previous = false;
        return out;
    }
    out.has_previous = true;
    if (current_x == previous_x && current_y == previous_y)
    {
        out.fresh = false;
        // 饱和累加（日志用，避免长跑溢出回绕成 0）
        out.frozen_frames = frozen_frames_so_far == 0xFFFFFFFFu
            ? frozen_frames_so_far : frozen_frames_so_far + 1u;
    }
    else
    {
        out.fresh = true;
        out.frozen_frames = 0;
    }
    return out;
}
#endif // DX11FSRBRIDGE_ENABLE_DIAGNOSTICS（只喂日志：共享几何输入分类 + 抖动新鲜度）
} // namespace fsr2_family_takeover
