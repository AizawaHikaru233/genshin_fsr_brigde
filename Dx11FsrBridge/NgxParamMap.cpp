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
    // 2026-11-13：MVLowRes 改为按 `in.mv_low_res` 决定（**不再恒设**）。
    //
    // 历史：这里曾是恒设一行（"与重构前逐字对应"）。
    //
    // 该位语义：告诉 DLSS「运动矢量是**渲染分辨率**的、需要内部放大到显示分辨率」。
    // 若游戏实际给的是显示分辨率 MV 却置了这位，DLSS 会按错误比例缩放 MV
    // ⇒ 运动补偿错位 ⇒ 全屏拖影（与本项目早期「若游戏给的是像素单位会出现全屏拖影」
    // 那条记录同源）。
    //
    // 改为可配置的依据：本键设 0 后实机拖影明显减轻（未完全根除 —— 剩余部分与
    // `NgxHdr`、OptiScaler 侧 `[InitFlags] HDR` 有关，不在本仓库范围）。
    // 默认仍由调用方传 true（保持历史行为），需要 A/B 时用 Fsr2MotionVectorsLowRes=0。
    if (in.mv_low_res)        out.flags |= kFlagMvLowRes;
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

// ---------------------------------------------------------------------------
// f) 按 NGX 报告的合法 render 尺寸区间选档（2026-11-13 新增）
// ---------------------------------------------------------------------------
// 本段**没有**「重构前原文」可抄：它是新判据，不是搬运。依据是 OPEN-ISSUES §4 的
// 「推进需要」，以及 third_party/ngx/nvsdk_ngx_helpers.h:65 的 NGX_DLSS_GET_OPTIMAL_SETTINGS
// （查询侧在 NgxBackend.cpp 里照那个实现重写）。
// ⇒ 本段的行为由 NgxParamMapTest.cpp 第 [10] 节的契约断言钉住，而不是 oracle 对照。
const char *perf_quality_name(int index)
{
    switch (index)
    {
    case 0:  return "MaxPerf";
    case 1:  return "Balanced";
    case 2:  return "MaxQuality";
    case 3:  return "UltraPerformance";
    case 4:  return "UltraQuality";
    case 5:  return "DLAA";
    default: return "?";
    }
}

// 上界容差 = upper / 64（整数除法，向零取整）。
// ★ 为什么下界不给容差：render 小于该档的 min 意味着「这一档放大倍率不够」，
//   正是 0xBAD00010 那类错配；对它放宽只会把错配放进来。
// ★ 为什么上界要给容差：max 是驱动按算法算出的**上界**，尺寸相差 1~2 像素时代码
//   仍能正常出图（本项目 2560x1440 下 max=... 与 render 差几个像素是常态），
//   对亚像素级超出直接判不匹配会平白丢失一个可用档位。
std::uint32_t range_upper_tolerance(std::uint32_t upper)
{
    return upper / 64u;
}

bool render_range_contains(const RenderRange &r, std::uint32_t render_w, std::uint32_t render_h)
{
    // 区间本身不可用（查询成功但字段退化）⇒ 不包含。**不能**当成"不限制"：
    // 那会让一个根本没查到的档位被选中，等于用假证据支撑选档。
    if (r.min_w == 0 || r.min_h == 0 || r.max_w == 0 || r.max_h == 0) return false;
    if (r.min_w > r.max_w || r.min_h > r.max_h) return false;
    if (render_w == 0 || render_h == 0) return false;

    const std::uint32_t tol_w = range_upper_tolerance(r.max_w);
    const std::uint32_t tol_h = range_upper_tolerance(r.max_h);
    const bool ok_w = render_w >= r.min_w && render_w <= r.max_w + tol_w;
    const bool ok_h = render_h >= r.min_h && render_h <= r.max_h + tol_h;
    return ok_w && ok_h;
}

QualityPick pick_quality_from_ranges(const RangeQuery *ranges, int count,
                                     std::uint32_t render_w, std::uint32_t render_h)
{
    QualityPick pick {};
    if (ranges == nullptr || count <= 0)
        return pick;

    // 固定优先序：只在"到 optimal 的距离完全相等"时用于打破平局。
    // 取值理由（不是随手排的）：越靠前的档画质越好、且越不容易撞上"倍率过大"的错配；
    // UltraQuality 排在 DLAA 前，是因为 DLAA 是唯一不放大的一档，把它当兜底最危险。
    static const int kTieOrder[kPerfQualityCount] = { 2, 1, 0, 3, 4, 5 };

    std::uint64_t best_dist = 0;
    bool          have_best = false;

    for (int t = 0; t < kPerfQualityCount; ++t)
    {
        const int i = kTieOrder[t];
        // ★ 这里必须是 continue **不是** break：kTieOrder 不是升序（{2,1,0,3,4,5}），
        //   一旦 count < 6 就会在第一个越界项上停下，把后面仍然有效的档位（0/1）
        //   一起跳过 —— 单测 [10] 的 "count 截断" 用例正是踩到这个。
        if (i >= count) continue;
        const RangeQuery &q = ranges[i];
        if (!q.ok) continue;                                        // 查询失败的档位不参与
        if (!render_range_contains(q.range, render_w, render_h)) continue;

        // 距离度量：两轴到 optimal 的绝对差之和（只用相对大小，不做单位换算）。
        const std::uint64_t dw = (render_w > q.range.opt_w) ? (render_w - q.range.opt_w)
                                                            : (q.range.opt_w - render_w);
        const std::uint64_t dh = (render_h > q.range.opt_h) ? (render_h - q.range.opt_h)
                                                            : (q.range.opt_h - render_h);
        const std::uint64_t dist = dw + dh;
        // 严格小于才替换 ⇒ 距离相等时**保留先出现的**（= 优先序更靠前者），结果确定。
        if (have_best && dist >= best_dist) continue;

        have_best   = true;
        best_dist   = dist;
        pick.matched = true;
        pick.index   = i;
        pick.tol_w   = range_upper_tolerance(q.range.max_w);
        pick.tol_h   = range_upper_tolerance(q.range.max_h);
    }
    return pick;
}

// ===========================================================================
// g) CreateFeature 输入 key 的相等判据
// ===========================================================================
// ★ 逐字段比较，**不是** memcmp：结构体有 padding，memcmp 会把未初始化的填充字节
//   当成输入的一部分 ⇒ 同一个 key 有时相等有时不等（随栈内容变化）。这是最隐蔽的一类
//   "判据看似成立、结论全错"（AGENTS.md §六的形态：判据本身有缺陷）。
//   ⇒ 必须一个字段一个字段比。
// ★ 顺序无所谓，但要**全部**比到：新增字段时若漏在这里，本函数会返回"相等"⇒ 该重试的
//   不重试 ⇒ 自愈失效，且没有任何报错。这就是本条存在的理由（单测钉住它）。
bool same_feature_input_key(const CreateFeatureInputKey &a, const CreateFeatureInputKey &b)
{
    return a.render_w      == b.render_w      &&
           a.render_h      == b.render_h      &&
           a.display_w     == b.display_w     &&
           a.display_h     == b.display_h     &&
           a.perf_quality  == b.perf_quality  &&
           a.render_preset == b.render_preset &&
           a.create_flags  == b.create_flags  &&
           a.color_fmt     == b.color_fmt     &&
           a.output_fmt    == b.output_fmt    &&
           a.is_hdr        == b.is_hdr;
}

} // namespace ngx_param_map
