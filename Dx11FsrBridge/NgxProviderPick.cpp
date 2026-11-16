// NgxProviderPick.cpp —— `NgxProviderPick.h` 的实现。
//
// 本文件**不含任何平台调用**：没有 LoadLibrary、没有注册表、没有 D3D。
// 全部逻辑是「给定输入枚举 → 返回决策枚举」。这样单测可以在任何机器上跑。

#include "NgxProviderPick.h"

namespace ngx_provider_pick
{

Mode clamp_mode(std::int32_t raw)
{
    // 只接受 0/1/2；其它（含负数、越界、拼错后读成默认的 -1）一律 Auto。
    // 与 Dx11FsrBridge 读 NgxProvider 时的行为一致：**拼错等于默认**，但日志里
    // 会由 provider_invalid 位单独告警，两者能区分开。
    switch (raw)
    {
    case static_cast<std::int32_t>(Mode::Nvidia):   return Mode::Nvidia;
    case static_cast<std::int32_t>(Mode::External): return Mode::External;
    default:                                        return Mode::Auto;
    }
}

PickOutcome pick(Mode mode, const ProbeResults &probes)
{
    PickOutcome out {};

    // ---- 第 1 步：要不要试注册表 ----
    // 原实现：`if (g_provider_mode != kProviderExternal)`。即 **Nvidia 也试**，
    // 只有 External 明确跳过。这条容易看错（"只走 nvidia"不等于"跳过 nvidia 探测"），
    // 故在此显式保留同一条件。
    if (mode != Mode::External)
    {
        if (probes.registry == RegistryStage::Ok)
        {
            // 注册表命中优先于一切外部实现 —— 这是原实现的顺序，不能反过来。
            out.decision = Decision::UseRegistry;
            return out;
        }

        if (mode == Mode::Nvidia)
        {
            // 显式 nvidia：与原实现逐条相同 —— 三条 reason + rc 原样上抛，
            // 不重试、不换来源。这里不记 fallback 明细，因为失败已是终局。
            out.decision = Decision::HardFailRegistry;
            return out;
        }

        // auto 模式：注册表失败但还要继续找外部 ⇒ 这次失败需要留线索，
        // 否则最终软失败时日志里看不出"真 NGX 曾经试过并且为什么没成"。
        out.record_registry_detail = true;
    }

    // ---- 第 2 步：外部实现（load 与 scan 任一命中即算命中）----
    // 原实现里两者的顺序是 load 先、scan 后，但**对决策结果没有影响**
    // （都是 provided = true），差别只在 detail 字符串的措辞。
    // 因此这里合并成一个 or，不复制其优先级。
    if (probes.external_load_ok || probes.external_scan_ok)
    {
        out.decision = Decision::UseExternal;
        return out;
    }

    // ---- 第 3 步：谁都没命中 ----
    out.decision              = Decision::SoftFailNoProvider;
    out.record_external_detail = true;
    out.external_detail_empty  = true;   // 调用方按实际 detail 覆盖
    return out;
}

const char *mode_name(Mode mode)
{
    switch (mode)
    {
    case Mode::Auto:     return "auto";
    case Mode::Nvidia:   return "nvidia";
    case Mode::External: return "external";
    default:             return "unknown";
    }
}

const char *stage_reason(RegistryStage stage)
{
    // 与 NgxBackend.cpp 的 provider_stage_reason() 返回的 wchar_t* 一一对应。
    // 这里用窄字符串：本文件服务于纯函数单测，不参与面向用户的日志拼接。
    switch (stage)
    {
    case RegistryStage::NoRegistry:   return "no_registry";
    case RegistryStage::LoadFailed:   return "load_failed";
    case RegistryStage::MissingEntry: return "missing_entry";
    default:                          return "unknown";
    }
}

const char *decision_name(Decision decision)
{
    switch (decision)
    {
    case Decision::UseRegistry:        return "use_registry";
    case Decision::UseExternal:        return "use_external";
    case Decision::HardFailRegistry:   return "hard_fail_registry";
    case Decision::SoftFailNoProvider: return "soft_fail_no_provider";
    default:                           return "unknown";
    }
}

bool is_failure(Decision decision)
{
    return decision == Decision::HardFailRegistry || decision == Decision::SoftFailNoProvider;
}

bool is_soft_failure(Decision decision)
{
    return decision == Decision::SoftFailNoProvider;
}

} // namespace ngx_provider_pick
