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

// 统计（限频日志用）
std::uint64_t skipped_count();
std::uint64_t accumulate_replaced_count();
} // namespace fsr2_family_takeover
