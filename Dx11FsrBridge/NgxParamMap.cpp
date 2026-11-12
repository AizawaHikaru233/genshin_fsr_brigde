// NgxParamMap.cpp —— NGX 参数映射的纯计算实现（零 D3D / 零 NGX / 零全局）。
//
// 本文件的每一行都是**逐字搬运**，来源见每个函数上方的「重构前原文」。
// 改这里 = 改行为。若某个公式确实要改，那是另一个任务，且必须同时更新
// NgxParamMapTest.cpp 的 oracle（否则测试会立刻红 —— 这正是它存在的意义）。
//
// 行尾：LF。

#include "NgxParamMap.h"

#include <cmath>    // std::isfinite（仅 JitterOutput::valid 诊断位用）

namespace ngx_param_map
{

// ---------------------------------------------------------------------------
// a) CreateFlags
// ---------------------------------------------------------------------------
// 重构前（NgxBackend.cpp 的 build_create_flags()，§3.8.2）：
//     ev.flags   = 0;
//     if (ev.is_hdr)                              ev.flags |= IsHDR;
//                                                 ev.flags |= MVLowRes;     // ★ 恒设
//     if (ffx12::motion_vectors_jittered())       ev.flags |= MVJittered;
//     if (ffx12::depth_inverted())                ev.flags |= DepthInverted;
//
// ★ 位顺序无关（|= 可交换），但**位的集合**必须一模一样：
//   is_hdr ⇒ bit0；恒有 bit1；mv_jittered ⇒ bit2；depth_inverted ⇒ bit3。
//   重构前那行 MVLowRes 前面**没有 if**（缩进只是排版），所以它是无条件执行。
CreateFlagsOutput map_create_flags(const CreateFlagsInput &in)
{
    CreateFlagsOutput out {};
    out.flags = 0;
    if (in.is_hdr)            out.flags |= kFlagIsHdr;
                              out.flags |= kFlagMvLowRes;   // 1<<1，恒设（与重构前那行逐字对应）
    if (in.mv_jittered)       out.flags |= kFlagMvJittered;
    if (in.depth_inverted)    out.flags |= kFlagDepthInverted;
    return out;
}

// ---------------------------------------------------------------------------
// b) Jitter
// ---------------------------------------------------------------------------
// 重构前（Dx11FsrBridge.cpp 的 try_fsr2_translation_draw()，jitter switch）：
//     const float use_jx = g_config.ffx12_jitter_delay ? st->prev_jx : jit_src_x;
//     const float use_jy = g_config.ffx12_jitter_delay ? st->prev_jy : jit_src_y;
//     switch (g_config.ffx12_jitter_mode)
//     {
//         case 1: sdk_in.jitter_x = use_jx * (float)sdk_in.render_w;  ... break;
//         case 2: break;   // ★ 不写 ⇒ 保留 switch 前的值
//         case 3: sdk_in.jitter_x = -(use_jx * (float)sdk_in.render_w) + 0.5f; ... break;
//         case 4: sdk_in.jitter_x = -(use_jx * (float)sdk_in.render_w); ... break;
//         case 5: sdk_in.jitter_x = 0.0f; ... break;
//         default: sdk_in.jitter_x = use_jx * (float)sdk_in.render_w - 0.5f; ... break;
//     }
//
// ★ 表达式形状逐字保留：`use * (float)W`（先乘后加/减），以及
//   `-(use * (float)W) + 0.5f`（**不是** `0.5f - use*W`；IEEE 下两者等价，
//   但保留原形状才能让"逐位等价"在代码层面一眼可审）。
JitterOutput map_jitter(const JitterInput &in)
{
    // 重构前的 use_jx / use_jy（同一 switch 之前的两行三元表达式）
    const float use_x = in.delay_one_frame ? in.prev_norm_x : in.norm_x;
    const float use_y = in.delay_one_frame ? in.prev_norm_y : in.norm_y;

    JitterOutput out {};
    switch (in.mode)
    {
        case 1: // +norm×render（无 −0.5）
            out.px = use_x * static_cast<float>(in.render_w);
            out.py = use_y * static_cast<float>(in.render_h);
            break;
        case 2: // 原样归一化（≈0，等效关 jitter）
            // ★ 旧代码是 `break;`（不赋值）。这里显式返回"未 delay 的输入"，
            //   语义等价于"switch 前的值"。调用点据此**不回写**，见 NgxParamMap.h 的说明。
            out.px = in.norm_x;
            out.py = in.norm_y;
            break;
        case 3: // −norm×render + 0.5
            out.px = -(use_x * static_cast<float>(in.render_w)) + 0.5f;
            out.py = -(use_y * static_cast<float>(in.render_h)) + 0.5f;
            break;
        case 4: // −norm×render（默认）
            out.px = -(use_x * static_cast<float>(in.render_w));
            out.py = -(use_y * static_cast<float>(in.render_h));
            break;
        case 5: // 零 jitter（诊断）
            out.px = 0.0f;
            out.py = 0.0f;
            break;
        default: // 0: +norm×render − 0.5（旧默认；也兜住一切非法 mode）
            out.px = use_x * static_cast<float>(in.render_w) - 0.5f;
            out.py = use_y * static_cast<float>(in.render_h) - 0.5f;
            break;
    }

    // 诊断位：只描述"这次计算的输入是否可用"，**不参与决策**（调用方不得据此改行为）。
    // ★ 旧代码没有这个位；它不改变任何输出，只是把"尺寸 0 / NaN"这类病态输入
    //   在测试与日志里显式化。生产路径不使用它（保持零行为变更）。
    out.valid = (in.render_w != 0) && (in.render_h != 0) &&
                std::isfinite(out.px) && std::isfinite(out.py);
    return out;
}

// ---------------------------------------------------------------------------
// c) MV scale
// ---------------------------------------------------------------------------
// 重构前（Dx11FsrBridge.cpp 的 try_fsr2_translation_draw()，motion scale 段）：
//     const float mv_sign = g_config.fsr2_positive_motion_vector_scale ? 1.0f : -1.0f;
//     sdk_in.motion_scale_x = mv_sign * (float)sdk_in.render_w * g_config.ffx12_motion_scale;
//     sdk_in.motion_scale_y = mv_sign * (float)sdk_in.render_h * g_config.ffx12_motion_scale;
//
// ★ 结合顺序 = (mv_sign * W) * scale（左结合）。改成 mv_sign * (W * scale)
//   在极端值下可能差一个 ulp ⇒ 这里**照抄左结合**。
MvScaleOutput map_mv_scale(const MvScaleInput &in)
{
    const float mv_sign = in.positive_scale ? 1.0f : -1.0f;
    MvScaleOutput out {};
    out.x = mv_sign * static_cast<float>(in.render_w) * in.motion_scale;
    out.y = mv_sign * static_cast<float>(in.render_h) * in.motion_scale;
    return out;
}

// ---------------------------------------------------------------------------
// d) Reset
// ---------------------------------------------------------------------------
// 重构前（NgxBackend.cpp 的 ngx::dispatch() 第 10 步）：
//     const int reset_val = (in.reset || inst.first) ? 1 : 0;
// ★ 逻辑或，**短路语义保留**（两个 bool 无副作用，短路与否结果相同；照抄即可）。
bool map_reset(const ResetInput &in)
{
    return in.frame_reset || in.instance_first;
}

// ---------------------------------------------------------------------------
// e) Reactive 尺寸守卫
// ---------------------------------------------------------------------------
// 重构前（NgxBackend.cpp 的 ngx::dispatch() 第 10 步，第 13 项 reactive mask）：
//     if (g_use_reactive && inst.rw >= in.render_w && inst.rh >= in.render_h)
// ★ 关系是 >=（池尺寸 **不**小于输入渲染尺寸才允许绑 mask），不是 ==。
bool reactive_size_ok(std::uint32_t pool_w, std::uint32_t pool_h,
                      std::uint32_t render_w, std::uint32_t render_h)
{
    return pool_w >= render_w && pool_h >= render_h;
}

} // namespace ngx_param_map
