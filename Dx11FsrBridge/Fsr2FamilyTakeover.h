#pragma once
// Fsr2FamilyTakeover.h — Phase 1：FSR2 5-PS 合成族识别与预处理 pass 跳过。
//
// 背景（探针实测 7.0，见 D:\Dump\work\probe-verdicts-20260822.md）：
//   游戏 FSR2 上采样 = 每帧 5 个连续合成 PS pass（固定顺序），其中前 4 个是
//   render-size 预处理（重建/膨胀/混合等），第 5 个是 display-size 累积/上采样
//   （现桥 Mode 2 的替换目标）。前 4 个 pass 的输出只被族内消费。
//
// 本模块（默认关闭）在"上一次累积 pass 被桥成功替换"的前提下跳过 4 个预处理 pass，
// 消除双跑残余。纯 C++ 状态机，不依赖 D3D11/Windows（时间由调用方注入，可单测）。

#include <cstdint>

namespace fsr2_family_takeover
{
// ---- 7.0 观测的合成族哈希（适配表初值；随版本复核） ----
constexpr std::uint64_t k_pre_hash_1 = 0x3CDF78FAC0ABCF6Dull; // PRE-1: cb0=1696, t1=render-size R8_TYPELESS
constexpr std::uint64_t k_pre_hash_2 = 0xAC63A3AF611EC7C9ull; // PRE-2: cb0=480, t1=160x560（SMAA LUT 变体）
constexpr std::uint64_t k_pre_hash_3 = 0x6018B8E925D4124Bull; // PRE-3: cb0=480, t1=render-size R8G8B8A8_TYPELESS
constexpr std::uint64_t k_pre_hash_4 = 0x590E69FEB210010Eull; // PRE-4: cb0=480, t1=render-size R8G8B8A8_TYPELESS
constexpr std::uint64_t k_accumulate_hash = 0x78057A29AF6C2D99ull; // 累积/上采样（现 Mode 2 目标）
constexpr std::uint64_t k_smaa_hash = 0xF41E6080D4BEA352ull;      // SMAA 模式合成（排除项）

// 重置状态（进程初始化/上下文重建时调用）
void reset();

bool is_pre_pass(std::uint64_t hash);
bool is_accumulate(std::uint64_t hash);
bool is_smaa(std::uint64_t hash);

// 预处理 pass 是否应跳过：
//   1) hash ∈ PRE 集合
//   2) 上次累积 pass 被桥成功替换（notify_accumulate_result(true)）
//   3) 未超过 expire_ms（now_ms - last_accumulate_tick <= expire_ms）
bool should_skip_pre(std::uint64_t hash, std::uint64_t now_ms, std::uint64_t expire_ms);

// 累积 pass 处理结果回填（try_fsr2_translation_draw 的返回值语义 + 当前时刻）
void notify_accumulate_result(bool replaced_ok, std::uint64_t now_ms);

// 接管权**交棒**（老接管者"离开接管"）时调用：立刻解除"跳过预处理 pass"的许可。
//
// 为什么必须有这一步（实机定案，2026-09-28）：
//   本状态机是**全局单份**——它只记住"上次累积 pass 被桥替换过"，不看实例。
//   P1 单实例接管下，接管权换给另一个实例后：
//     · 老实例的累积 pass 改由**游戏原生**执行（passthrough）；
//     · 可它的 4 个预处理 pass 仍被本状态机的全局许可跳过 ✗
//   ⇒ 原生 FSR2 输入缺失 ⇒ 该路视图冻在最后一帧（与"另一个机位实时"叠成残影）。
//   交棒瞬间显式解除许可，保证"放行"真的把原生路径还回去（下一次累积 pass 若是
//   接管者自己的，会由 notify_accumulate_result(true) 重新武装——语义不变）。
void notify_takeover_leave();

// 交棒释放次数（诊断/验收：应随交棒事件增长，而不是恒 0）
std::uint64_t takeover_leave_count();

// ---------------------------------------------------------------------------
// P1 单实例接管：修复②的**唯一判定点**（纯函数 ⇒ 可离线单测）。
//
// 背景（实机回归，本轮）：修复②的第一版直接"多义 ⇒ 拒绝"，但接管者的 out_a/out_b 是
// **第一次成功 dispatch 时**才建立的 ⇒ 第一次 bootstrap 被拒 ⇒ 归属永远建立不起来 ⇒
// 双实例场景下超分**完全停止**（实机：ffx12_ambiguous_bootstrap=3072、out_a=out_b=0、
// ffx12_result 停在 14 条）。所以判据抽成纯函数，并把两条"不可饿死"不变式写死在里面：
//   · claimer_has_ownership == false       ⇒ 永不拒绝（放行它是建立归属的唯一途径）
//   · claimer_recently_dispatched == false ⇒ 永不拒绝（拒绝不刷新派发时间戳 ⇒ 自愈）
// 单测（Fsr2FamilyTakeoverTest）逐条钉住这两条 ⇒ 回归不可能再次悄悄发生。
struct TokenOnlyClaimFacts
{
    bool single_instance_takeover = false;  // Ffx12SingleInstance=1（P1 生效）
    bool second_instance_present = false;   // 本进程出现过第二个实例（粘性互锁）
    bool claimer_is_current_taker = false;  // 认领者就是当前接管者
    bool claimed_by_token_only = false;     // match_path == 2（无输出归属校验）
    bool output_belongs_to_claimer = false; // 输出命中认领者的 out_a/out_b（path 1/3）
    bool claim_has_generation = false;      // call_gen != 0（确有新 Render 代次）
    std::uint64_t same_size_token_candidates = 0; // 同尺寸未消费 token 的实例数
    bool claimer_has_ownership = false;     // out_a/out_b 非 0（已有归属记忆）
    bool claimer_recently_dispatched = false; // 最近 500ms 内尝试过派发（健康）
};

// true = 拒绝这次认领（调用方：消费 token 后放行游戏原生）
bool p1_refuse_token_only_claim(const TokenOnlyClaimFacts &facts);

// 统计（限频日志用）
std::uint64_t skipped_count();
std::uint64_t accumulate_replaced_count();
} // namespace fsr2_family_takeover
