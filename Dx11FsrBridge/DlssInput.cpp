// DlssInput.cpp — DLSS 输入契约的纯映射层实现 + NGX 驱动层。
//
// 设计约束：本 TU 只依赖 <cstdint>/<cstddef>/<string>/<vector>/<cmath>，
// 不 include 任何 D3D / Windows 头（前置声明即可），因此 DlssInputTest 能零依赖编译。
// 唯一的平台调用是驱动层的 `GetProcAddress`，用弱依赖的方式在下面单独隔离。

#include "DlssInput.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#if defined(_WIN32)
// 主 target 已经在 CMake 里定义这两个宏；用 #ifndef 包裹避免 -Wmacro-redefined。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace dlss_input
{
namespace
{

bool finite(float v)
{
    return std::isfinite(v);
}

} // namespace

// ---------------------------------------------------------------------------
// narrow_ascii — 宽字符转窄（诊断日志用）
// ---------------------------------------------------------------------------
// 为什么不用 WideCharToMultiByte(CP_UTF8)：这个函数在**纯映射层**所在的 TU 里，
// 而该 TU 刻意不依赖 Windows（测试 target 零依赖编译）。这里给一个只做
// "ASCII 直通、非 ASCII 降为 '?'" 的保守实现 —— 够用于日志里的模块名
// （nvngx.dll / OptiScaler.dll 都是 ASCII），且永远不会失败。
std::string narrow_ascii(const std::wstring &wide)
{
    std::string out;
    out.reserve(wide.size());
    for (wchar_t c : wide)
        out.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?');
    return out;
}

// ---------------------------------------------------------------------------
// 1.1 jitter
// ---------------------------------------------------------------------------
float wrap_to_half_pixel(float v)
{
    if (!finite(v))
        return 0.0f;
    // 折叠到 [-0.5, 0.5)：先减 0.5，取小数部分得 [0,1)，再减 0.5。
    // 用 floor 而不是 fmod，保证负数行为对称（fmod(-0.7,1) = -0.7 会出界）。
    float shifted = v - 0.5f;
    shifted -= std::floor(shifted); // [0,1)
    return shifted - 0.5f;          // [-0.5, 0.5)  ⇒ |v| <= 0.5，满足 NGX 契约
}

float clamp_to_half_pixel(float v)
{
    if (!finite(v))
        return 0.0f;
    if (v > 0.5f)
        return 0.5f;
    if (v < -0.5f)
        return -0.5f;
    return v;
}

// ---------------------------------------------------------------------------
// Halton(2,3) —— 与 FFX/DLSS 官方 GetJitterOffset 同一序列
// ---------------------------------------------------------------------------
float halton(int index, int base)
{
    if (index < 1 || base < 2)
        return 0.0f;
    double f = 1.0;
    double r = 0.0;
    int i = index;
    while (i > 0)
    {
        f /= static_cast<double>(base);
        r += f * static_cast<double>(i % base);
        i /= base;
    }
    return static_cast<float>(r); // (0,1)
}

void halton_jitter(int index, float &out_x, float &out_y)
{
    // 官方形式：jitter = (halton(i,2) - 0.5, halton(i,3) - 0.5)，落在 [-0.5, +0.5)。
    // 这是**零中心**的亚像素偏移，满足 NGX 对 Jitter.Offset 的契约。
    out_x = halton(index, 2) - 0.5f;
    out_y = halton(index, 3) - 0.5f;
}

JitterOutput map_jitter(const JitterInput &in)
{
    JitterOutput out {};
    if (in.render_w == 0 || in.render_h == 0)
        return out;

    // cb0 路径落后一个 token（Dx11FsrBridge.cpp:12143-12147）⇒ 用上一帧的相位，
    // 让 color/MV 与上采样器看到的是**同一快门时刻**的抖动。
    const bool use_prev = in.cb_lags_one_frame;
    const float nx = use_prev ? in.prev_norm_x : in.norm_x;
    const float ny = use_prev ? in.prev_norm_y : in.norm_y;
    out.source = use_prev ? "cb_prev" : "render";

    // ---- 自生成 Halton：完全绕开游戏字段 ----
    // 这条路径不看 norm_x/norm_y（它们可能长期冻结），直接用帧序号生成标准序列。
    // 官方 GetJitterOffset 就是这个形态，DLSS 对它有最好的预期。
    if (in.semantics == JitterInput::NormSemantics::SelfGeneratedHalton)
    {
        float jx = 0.0f, jy = 0.0f;
        halton_jitter(in.frame_index > 0 ? in.frame_index : 1, jx, jy);
        out.px = jx;
        out.py = jy;
        out.raw_px = jx;
        out.raw_py = jy;
        out.reduced = false;
        out.source = "halton_self";
        out.valid = true;
        return out;
    }

    if (!finite(nx) || !finite(ny))
        return out;

    const float w = static_cast<float>(in.render_w);
    const float h = static_cast<float>(in.render_h);

    // ---- 第一步：把输入统一成**像素域抖动**（这是修复的核心）----
    // 见 JitterInput::NormSemantics 的实机取证说明：
    // 桥读到的字段是 halton/renderSize（窄带 0~1/W），不是 [0,1) 相位。
    // 旧实现把它当相位再乘 W，等于得到 halton（0..1 像素）⇒ 实际零抖动。
    float pix_x = 0.0f;
    float pix_y = 0.0f;
    switch (in.semantics)
    {
    case JitterInput::NormSemantics::HaltonOverSize:
        // 字段已是 halton/W ⇒ 乘回去得到 halton∈[0,1)，再乘 W 得像素。
        // 两步合并即 `field × W × W`？不对——精确关系是：
        //   field = halton / W  ⇒  halton = field × W（这里 halton∈[0,1)）
        //   像素抖动 = halton（因为 halton 本身就是"像素"量级的相位）
        // 直接乘一次 W 即可。
        pix_x = nx * w;
        pix_y = ny * h;
        break;
    case JitterInput::NormSemantics::RawPhase:
        // 裸相位 [0,1) ⇒ 乘 renderSize 得像素（桥历史上的写法）。
        pix_x = nx * w;
        pix_y = ny * h;
        break;
    case JitterInput::NormSemantics::AlreadyPixels:
    default:
        pix_x = nx;
        pix_y = ny;
        break;
    }

    float px = 0.0f;
    float py = 0.0f;

    // ---- 相位归一（**必须在模式变换之前**）----
    //
    // 实测：游戏字段 norm 乘以 renderSize 后，`pix = norm*W` 在很多帧 **> 1**
    // （实测样本 0.50 / 0.87 / 1.62 / 1.81 / 2.50 / 3.02）。而下面的
    // `ZeroCentred` 用 `0.5 - pix`，pix=1.62 时得到 **-1.12**，pix=2.50 时
    // 得到 **-2.00** —— 远超 NGX `Jitter.Offset` 的 |v| ≤ 0.5 契约。
    //
    // 旧实现在**模式变换之后**才用 `wrap_to_half_pixel` 折返，后果是
    // **方向和量级都被破坏**（实测）：
    //   jitter=-1.123 -> -0.123   （幅度压掉 9 倍）
    //   jitter=-1.997 -> +0.003   （**方向翻转**）
    //   jitter=-2.524 -> +0.476   （**方向翻转**）
    // DLSS 因此每帧朝错误位置寻找历史 ⇒ **静止时画面也在抖**
    //（用户实测：三渲二角色线条静止时也抖，M/L 比 K 明显）。
    //
    // 正确顺序：先把**相位**折回 [0,1)，再做模式变换。这样
    // `0.5 - pix` 天然落在 (-0.5, +0.5]，契约恒成立，且方向不丢。
    // 相位折返对序列是单射（模 1），多样性不受损。
    if (in.mode != JitterMode::Zero && in.mode != JitterMode::Raw)
    {
        pix_x = pix_x - std::floor(pix_x);
        pix_y = pix_y - std::floor(pix_y);
    }

    switch (in.mode)
    {
    case JitterMode::PlusMinusHalf:
        px = pix_x - 0.5f;
        py = pix_y - 0.5f;
        break;
    case JitterMode::Positive:
        // 游戏约定：采样点 = 像素中心 − jitter ⇒ 取反号即"反向抖动"。
        px = -pix_x;
        py = -pix_y;
        break;
    case JitterMode::Raw:
        // 原样（≈ 关抖动）；保留是为了 A/B 时不改 ini 语义。
        px = pix_x;
        py = pix_y;
        break;
    case JitterMode::ZeroCentred:
        // 默认：零中心化（把 [0,1) 的相位映射到 (-0.5, +0.5]）。
        px = 0.5f - pix_x;
        py = 0.5f - pix_y;
        break;
    case JitterMode::Negative:
        px = -pix_x;
        py = -pix_y;
        break;
    case JitterMode::Zero:
    default:
        px = 0.0f;
        py = 0.0f;
        break;
    }

    out.raw_px = px;
    out.raw_py = py;

    switch (in.reduce)
    {
    case JitterReduce::PhaseScaled:
    {
        // ⚠️ 这里**不能**再回到 `base_x`（原始字段）。它已经过上面两步：
        //   ① 语义修正（halton/W → 像素）
        //   ② 模式（符号/零中心/偏置）
        // 旧实现在这里重新取 `in.norm_x` 并只翻符号，等于把①的修正整个丢掉
        // ⇒ 送出去的是 halton/W（≈0.0003 像素）⇒ 仍然零抖动。
        // 现在直接对**已修正的 px/py** 做相位折叠。
        // `fract(v + 0.5) - 0.5` 对 v 连续且单射（模 1），保持序列多样性。
        px = wrap_to_half_pixel(px);
        py = wrap_to_half_pixel(py);
        out.reduced = false; // 相位折叠不丢多样性，不算"被归约"
        break;
    }
    case JitterReduce::Wrap:
    {
        const float wx = wrap_to_half_pixel(px);
        const float wy = wrap_to_half_pixel(py);
        out.reduced = (wx != px) || (wy != py);
        px = wx;
        py = wy;
        break;
    }
    case JitterReduce::Clamp:
    {
        const float cx = clamp_to_half_pixel(px);
        const float cy = clamp_to_half_pixel(py);
        out.reduced = (cx != px) || (cy != py);
        px = cx;
        py = cy;
        break;
    }
    case JitterReduce::None:
    default:
        break;
    }

    out.px = px;
    out.py = py;
    out.valid = true;
    return out;
}

// ---------------------------------------------------------------------------
// 1.2 motion vector
// ---------------------------------------------------------------------------
MotionOutput map_motion_scale(const MotionInput &in)
{
    MotionOutput out {};
    if (in.render_w == 0 || in.render_h == 0)
        return out;

    // 【符号推导 —— 错一次就是"全局方向反了"，症状正是运动后残留旧内容（鬼影）】
    //
    // 已知（桥的反汇编实证，Ffx12Backend.cpp:498-499）：
    //   游戏原生：     history_uv = uv - mv_g
    //   桥解码后纹理： mv_tex = -mv_g          ← 解码式 `-sign(d)*4d²` 取了一次负
    //
    // 两个后端的重投影公式不同：
    //   FSR2/FFX： history_uv = uv + mv_tex * scale
    //              代入 ⇒ uv + (-mv_g)*s = uv - mv_g  ⇒  **s = +1**
    //   DLSS/NGX： prev_uv    = uv - mv_tex * scale
    //              代入 ⇒ uv - (-mv_g)*s = uv - mv_g  ⇒  **s = -1**
    //
    // 即：两条路径的 scale **符号相反**，因为两个 SDK 的重投影公式相差一个负号。
    // 桥给 FFX 用 `+renderSize`（Fsr2MotionVectorScaleMode=1），给 DLSS 必须
    // `-renderSize`。这正是当前实现，且与 OptiScaler 自家 FSR2 输入路径的做法一致
    // （它把 motionVectorScale 原样透传，因为游戏那边本来就是给 "uv - mv" 用的）。
    //
    // ⚠️ 曾经误以为"纹理已取负 ⇒ 两边同号"，那是漏掉了两个公式本身的负号差异。
    const bool texture_is_fsr2 = (in.texture_sign == MotionTextureSign::Fsr2Negated);

    switch (in.fixup)
    {
    case MotionFixup::UseNegativeScale:
    {
        const float sign = texture_is_fsr2 ? -1.0f : 1.0f;
        out.scale_x = sign * static_cast<float>(in.render_w);
        out.scale_y = sign * static_cast<float>(in.render_h);
        out.note = texture_is_fsr2 ? "neg_scale_for_decoded_mv" : "positive_scale_native_mv";
        break;
    }
    case MotionFixup::RequireNativeSign:
    {
        // 备选路径：不改纹理符号，改用正 scale（等价于把纹理当原生符号用）。
        out.needs_texture_resign = texture_is_fsr2;
        out.scale_x = static_cast<float>(in.render_w);
        out.scale_y = static_cast<float>(in.render_h);
        out.note = texture_is_fsr2 ? "requires_native_sign_texture" : "native_mv_positive";
        break;
    }
    }

    // 显示分辨率 MV 时 DLSS 要的正 scale 是 display 尺寸（这里只有 render 尺寸语义，
    // 桥的 MV 恒为 render 尺寸 —— 保留分支以便将来接 display-res MV 的引擎）。
    if (in.display_resolution_mv)
    {
        // 调用方若真的给 display-res MV，renderSize 就是错误的量纲。
        out.note = "display_resolution_mv_not_supported";
        out.valid = false;
        return out;
    }

    out.valid = true;
    return out;
}

// ---------------------------------------------------------------------------
// 1.3 create flags
// ---------------------------------------------------------------------------
CreateFlagOutput map_create_flags(const CreateFlagInput &in)
{
    CreateFlagOutput out {};
    auto add = [&out](bool cond, std::uint32_t bit, const char *why) {
        if (cond)
        {
            out.flags |= bit;
            out.reasons.emplace_back(why);
        }
    };

    // IsHDR —— **回退（2026-11-09 实机否决）**。
    //
    // 我曾把它改成"PQ 码值属于 HDR 管线 ⇒ 置 IsHDR"，理由是 FFX 路径用
    // `0x129`（含 HDR 位）。实机结果：**拖影模糊明显加重** ⇒ 回退。
    //
    // 教训：FFX 的 `HIGH_DYNAMIC_RANGE` 与 NGX 的 `IsHDR` **不是同一件事**。
    // FFX 那个位配合 NON_LINEAR_COLORSPACE + AUTO_EXPOSURE 让 SDK 自己解释
    // PQ 码值；NGX 的 IsHDR 只是告诉 DLSS"内容是非 SDR 的"，并**不提供**
    // 非线性色彩空间的处理路径 ⇒ DLSS 会把 PQ 当线性 HDR，反而更错。
    // 因此这里维持原判：只有"线性 + 值域确实 >1"才置 IsHDR。
    const bool hdr = (in.color == ColorEncoding::LinearFp16) && in.color_values_exceed_one;
    add(hdr, kFlagIsHdr,
        in.color == ColorEncoding::PqCodeValues ? "pq_code_values_not_hdr"
                                                : (hdr ? "linear_and_hdr_range" : "linear_sdr_range"));

    add(in.depth_inverted, kFlagDepthInverted, "reverse_z_depth");
    add(in.mvs_contain_jitter, kFlagMvJittered, "mvs_contain_jitter");
    add(!in.mvs_at_display_resolution, kFlagMvLowRes, "mvs_at_render_resolution");
    // AutoExposure：只有真的绑了 1x1 exposure 纹理才能开。
    add(in.have_exposure_texture, kFlagAutoExposure, "exposure_texture_bound");

    // AlphaUpscaling（1<<7）—— **实机 A/B 开关，默认关**。
    // NVIDIA 官方头只给出这一位，没有任何语义注释；不臆测它的行为，
    // 因此由 ini（DlssInputAlphaUpscaling）显式控制，并如实记入 reasons。
    //
    // 动机：三渲二角色的描边/头发/衣摆边缘是 alpha 混合或 alpha 裁切，
    // 若 DLSS 把它们当作不透明刚性几何累积历史，边缘就会在时域里抖动。
    // 若实机开启后线条抖动消失 => 这一位是有效解；变差则永久保持关闭。
    add(in.alpha_upscaling, kFlagAlphaUpscaling, "alpha_upscaling_user_enabled");

    // ⚠️ **不再置 `DoSharpening`**（2026-11-10 修复"开锐化就黑屏"）。
    //
    // NVIDIA 已在 NGX 头里把这一位标记为**废弃**：
    //   nvsdk_ngx_defs.h:296
    //     NVSDK_NGX_DLSS_Feature_Flags_DoSharpening SR_DEPRECATED_SHARPENING = 1 << 5,
    //     // Sharpness is not supported
    //
    // 实机证据（用户报告）：OptiScaler 打开锐化后**游戏正常运行但画面全黑**。
    // 机制：OptiScaler 的 `IFeature.cpp:39` 会把本函数置的 `DoSharpening`
    // 解析成 `_initFlags.SharpenEnabled`，随后 `DLSSFeature.cpp:53` 又在
    // CreateFeature 时把这一位加回去；而 DLSS v310.7 收到"已废弃的锐化标志 +
    // Sharpness 参数"后走的是未定义路径 ⇒ 输出全黑。
    //
    // 正确做法：**锐化完全交给 OptiScaler 的 RCAS 后处理**（与 DLSS 解耦、
    // 是该版本唯一受支持的路径）。桥只负责不添乱。
    if (in.sharpening)
        out.reasons.emplace_back("sharpening_ignored_ngx_flag_deprecated_use_optiscaler_rcas");

    if (in.color == ColorEncoding::PqCodeValues)
        out.reasons.emplace_back("pq_input_requires_linearize_before_ngx");
    if (!in.have_exposure_texture)
        out.reasons.emplace_back("no_exposure_texture_use_pre_exposure_1");
    return out;
}

// ---------------------------------------------------------------------------
// 1.4 perf quality
// ---------------------------------------------------------------------------
std::uint32_t map_perf_quality(float display_over_render_ratio, bool avoid_ultra_quality_rewrite)
{
    if (!finite(display_over_render_ratio) || display_over_render_ratio <= 0.0f)
        return kPerfMaxQuality;

    // ⚠️ **关键修正（2026-11-09 实机测量驱动）**
    //
    // `PerfQualityValue` 是 DLSS 的**质量档位标签**，而不是精确比例。
    // 但库里另有一个隐含约束：DLSS 会用它构造内部的重投影矩阵，
    // 若标签与实际 render/display 比例**不匹配**，运动会朝错误位置采样历史。
    //
    // 实机数据：render=2556x1438，display=2560x1440 ⇒ 实际比例 **1.0016**（几乎 1:1）。
    // 而旧实现把它量化成 `UltraQuality`→(被 OptiScaler 改写为)`MaxQuality` = **1.5×**。
    // 于是 DLSS 按 1.5× 构造重投影，位移被放大 ~1.5 倍 ⇒ **一动就拖影**。
    // 这也解释了 `DlssInputMotionSign=2` 时画面变灰：MV 参与的重投影整体错乱。
    //
    // 因此：**接近 1:1 时必须返回 DLAA**（= 无缩放），而不是最近的高档位。
    // 判据用"比例落在 DLAA 区间"而不是"最接近"，因为标签错配的代价远大于档位不精确。
    if (display_over_render_ratio <= 1.05f)
        return kPerfDlaa; // 1.0x：DLSS 按输入分辨率直接重建（本游戏 2556→2560 属此类）

    std::uint32_t q;
    if (display_over_render_ratio > 2.0f)
        q = kPerfUltraPerformance;
    else if (display_over_render_ratio > 1.7f)
        q = kPerfMaxPerf;
    else if (display_over_render_ratio > 1.5f)
        q = kPerfBalanced;
    else if (display_over_render_ratio > 1.3f)
        q = kPerfMaxQuality;
    else if (display_over_render_ratio > 1.05f)
        q = kPerfUltraQuality;
    else
        q = kPerfDlaa;

    // OptiScaler 的 DLSSFeature::ProcessInitParams 会把 UltraQuality 改写成 MaxQuality
    // （DLSSFeature.cpp:254-259）。若调用方依赖"不小于 UltraQuality"，提前改写以免
    // 后端行为与日志不一致。
    if (avoid_ultra_quality_rewrite && q == kPerfUltraQuality)
        q = kPerfMaxQuality;
    return q;
}

float perf_quality_ratio(std::uint32_t quality)
{
    switch (quality)
    {
    case kPerfUltraPerformance:
        return 3.0f;
    case kPerfMaxPerf:
        return 2.0f;
    case kPerfBalanced:
        return 1.7f;
    case kPerfMaxQuality:
        return 1.5f;
    case kPerfUltraQuality:
        return 1.3f;
    case kPerfDlaa:
    default:
        return 1.0f;
    }
}

// ---------------------------------------------------------------------------
// 1.5 reset
// ---------------------------------------------------------------------------
const char *reset_reason_name(ResetReason reason)
{
    switch (reason)
    {
    case ResetReason::Forced:
        return "forced";
    case ResetReason::DispatchFailed:
        return "dispatch_failed";
    case ResetReason::InstanceChanged:
        return "instance_changed";
    case ResetReason::SizeChanged:
        return "size_changed";
    case ResetReason::FirstFrame:
        return "first_frame";
    case ResetReason::ResumeAfterGap:
        return "resume_after_gap";
    case ResetReason::ExplicitCameraCut:
        return "camera_cut";
    case ResetReason::None:
    default:
        return "none";
    }
}

ResetDecision decide_reset(const ResetFacts &facts)
{
    // 顺序 = 优先级：结构性原因（尺寸/实例/失败/首帧）优先于启发式的空窗。
    // 为什么空窗放最后：它是**间接代理**，一次卡顿就会凭空丢历史；
    // 而"尺寸变了却不 reset"会直接导致 DLSS 用错历史尺度（更严重）。
    if (facts.first_frame)
        return { true, ResetReason::FirstFrame };
    if (facts.size_changed)
        return { true, ResetReason::SizeChanged };
    if (facts.instance_changed)
        return { true, ResetReason::InstanceChanged };
    if (facts.dispatch_failed)
        return { true, ResetReason::DispatchFailed };
    if (facts.forced)
        return { true, ResetReason::Forced };
    if (facts.explicit_camera_cut)
        return { true, ResetReason::ExplicitCameraCut };
    if (facts.resume_gap_ms != 0 && facts.ms_since_last_dispatch > facts.resume_gap_ms)
        return { true, ResetReason::ResumeAfterGap };
    return { false, ResetReason::None };
}

// ---------------------------------------------------------------------------
// 1.6 mask
// ---------------------------------------------------------------------------
MaskPlan plan_masks(const MaskPlanInput &in)
{
    MaskPlan out {};
    if (in.disable_reactive)
    {
        out.note = "reactive_disabled_by_config";
        return out;
    }
    if (!in.has_reactive)
    {
        out.note = in.has_transparency ? "only_transparency_present_but_ngx_has_no_tc_slot"
                                       : "no_masks";
        return out;
    }
    out.bind_bias_mask = true;
    out.reactive_scale = finite(in.reactive_scale) ? in.reactive_scale : 1.0f;
    if (in.has_transparency && in.transparency == TransparencyHandling::FoldIntoReactive)
    {
        out.fold_transparency = true;
        out.note = "transparency_folded_into_reactive";
    }
    else if (in.has_transparency)
    {
        // 诚实记账：NGX 没有 transparency 槽位，这里**确实**丢掉了。
        out.note = "transparency_dropped_ngx_has_no_slot";
    }
    else
    {
        out.note = "reactive_only";
    }
    return out;
}

// ---------------------------------------------------------------------------
// 1.7 subrect
// ---------------------------------------------------------------------------
Rect full_rect(std::uint32_t w, std::uint32_t h)
{
    Rect r {};
    r.w = w;
    r.h = h;
    return r;
}

bool validate_subrect(const Rect &r, std::uint32_t tex_w, std::uint32_t tex_h)
{
    if (r.w == 0 || r.h == 0)
        return false;
    if (r.x >= tex_w || r.y >= tex_h)
        return false;
    if (r.x + r.w > tex_w || r.y + r.h > tex_h)
        return false;
    return true;
}

// ---------------------------------------------------------------------------
// 1.8 route
// ---------------------------------------------------------------------------
// 注意 `dlss_runtime_present` 的语义：它是"**此刻**模块已在进程里"，由
// `GetModuleHandleW` 得到。这与"**能否**加载到 NGX"是两件事 —— OptiScaler 会把
// `LoadLibraryW("nvngx.dll")` 在 LdrLoadDll 层拦下并返回自身句柄，而那个模块
// 在加载之前当然不在模块列表里。所以：
//   · 强制模式（user_preference==1）**不能**用它做门控 —— 否则永远切不过去；
//   · auto 模式用它只是"便宜的前置判断"，OptiScaler 在场时应当照样尝试。
RouteOutput route_output(const RouteInput &in)
{
    if (in.user_preference == 0)
        return { OutputPath::FfxNative, "user_forced_ffx" };
    if (in.user_preference == 1)
    {
        // 强制：直接试。能不能成由后面的 LoadLibrary/符号解析回答，
        // 失败会 fail-open 回到 ffx 并留下可读原因。
        if (in.dlss_runtime_present)
            return { OutputPath::DlssInput, "user_forced_dlss_and_runtime_ok" };
        if (in.optiscaler_present)
            return { OutputPath::DlssInput, "user_forced_dlss_optiscaler_will_serve" };
        return { OutputPath::DlssInput, "user_forced_dlss_try_load" };
    }

    // auto：
    //   ① 已经加载了 NGX ⇒ 直接用。
    //   ② OptiScaler 在场 ⇒ 它的 LdrLoadDll 钩子会把 nvngx.dll 换成自己，值得一试。
    //   ③ 都不满足 ⇒ 保持现状（绝不因为"是 N 卡"就盲目切换）。
    if (in.dlss_runtime_present)
    {
        if (in.vendor == Vendor::Nvidia || in.optiscaler_present)
            return { OutputPath::DlssInput, "ngx_runtime_present" };
        return { OutputPath::FfxNative, "ngx_present_but_vendor_not_nvidia" };
    }

    if (in.optiscaler_present)
        return { OutputPath::DlssInput, "optiscaler_present_will_serve_ngx" };

    return { OutputPath::FfxNative, "no_dlss_runtime_and_no_optiscaler" };
}

// ---------------------------------------------------------------------------
// 1.9 frame assembly
// ---------------------------------------------------------------------------
FrameParams build_frame(const FrameSpec &spec)
{
    FrameParams out {};

    out.render_w = spec.render_w;
    out.render_h = spec.render_h;
    out.display_w = spec.display_w;
    out.display_h = spec.display_h;
    out.render_subrect = full_rect(spec.render_w, spec.render_h);

    JitterInput ji {};
    ji.norm_x = spec.norm_jitter_x;
    ji.norm_y = spec.norm_jitter_y;
    ji.prev_norm_x = spec.prev_norm_jitter_x;
    ji.prev_norm_y = spec.prev_norm_jitter_y;
    ji.render_w = spec.render_w;
    ji.render_h = spec.render_h;
    ji.mode = spec.jitter_mode;
    ji.reduce = spec.jitter_reduce;
    ji.cb_lags_one_frame = spec.jitter_from_cb0;
    ji.semantics = spec.jitter_semantics;
    ji.frame_index = spec.jitter_frame_index;
    const JitterOutput jo = map_jitter(ji);
    out.jitter_px = jo.px;
    out.jitter_py = jo.py;

    MotionInput mi {};
    mi.texture_sign = spec.motion_sign;
    mi.fixup = spec.motion_fixup;
    mi.render_w = spec.render_w;
    mi.render_h = spec.render_h;
    mi.display_resolution_mv = spec.mvs_display_res;
    const MotionOutput mo = map_motion_scale(mi);
    // 排障档：彻底关掉 MV。DLSS 收到 scale=0 等于"所有像素静止"，
    // 它会退化成纯空间放大 —— 画面会变锐利但当镜头移动时出现拖尾/抖动。
    // 判定价值：若此时"一动就模糊"消失，则 MV 链路是根因；若仍在，不是。
    if (spec.motion_disabled)
    {
        out.mv_scale_x = 0.0f;
        out.mv_scale_y = 0.0f;
    }
    else
    {
        out.mv_scale_x = mo.scale_x;
        out.mv_scale_y = mo.scale_y;
    }

    out.sharpness = spec.sharpening ? spec.sharpness : 0.0f;
    out.frame_time_ms = finite(spec.frame_time_ms) ? spec.frame_time_ms : 16.7f;
    out.camera_near = spec.camera_near;
    out.camera_far = spec.camera_far;
    out.camera_fov_vertical = spec.camera_fov_vertical;
    out.bind_exposure_texture = spec.have_exposure_texture;
    out.bind_bias_mask = spec.mask.bind_bias_mask;

    ResetFacts rf {};
    rf.first_frame = spec.reset_first_frame;
    rf.size_changed = spec.reset_size_changed;
    rf.instance_changed = spec.reset_instance_changed;
    rf.dispatch_failed = spec.reset_dispatch_failed;
    rf.forced = spec.reset_forced;
    rf.explicit_camera_cut = spec.reset_camera_cut;
    rf.ms_since_last_dispatch = spec.ms_since_last_dispatch;
    const ResetDecision rd = decide_reset(rf);
    out.reset = rd.reset ? 1 : 0;
    out.reset_reason = reset_reason_name(rd.reason);

    return out;
}

// ===========================================================================
// 2/3 驱动层
// ===========================================================================
bool NgxApi::complete() const
{
    // 最小可用集合。注意 `init` 与 `init_project_id` 二选一即可（OptiScaler 两条都提供，
    // NVIDIA 的 nvngx.dll 也都有；但历史上某些版本只导出一个）。
    if (init == nullptr && init_project_id == nullptr)
        return false;
    if (get_capability_parameters == nullptr && get_parameters == nullptr &&
        allocate_parameters == nullptr)
        return false;
    return create_feature != nullptr && release_feature != nullptr && evaluate_feature != nullptr;
}

ResolveReport NgxApi::resolve(void *module)
{
    ResolveReport report {};
#if defined(_WIN32)
    HMODULE h = static_cast<HMODULE>(module);
    if (h == nullptr)
        return report;

    wchar_t path[MAX_PATH] {};
    if (GetModuleFileNameW(h, path, MAX_PATH) != 0)
    {
        char narrow[MAX_PATH * 4] {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, sizeof(narrow), nullptr, nullptr);
        if (n > 0)
            report.module_path.assign(narrow);
        // 只保留文件名：日志里要一眼看出"实际拿到的是谁"
        // （例如 OptiScaler 冒充 nvngx.dll 时，路径会指向 OptiScaler.dll）。
        const std::wstring full(path);
        const std::size_t slash = full.find_last_of(L"\\/");
        report.module_name = narrow_ascii(slash == std::wstring::npos ? full : full.substr(slash + 1));
    }

    auto get = [h](const char *name) -> void * {
        return reinterpret_cast<void *>(GetProcAddress(h, name));
    };

    init = reinterpret_cast<PFN_Init>(get("NVSDK_NGX_D3D11_Init"));
    init_project_id = reinterpret_cast<PFN_InitProjectId>(get("NVSDK_NGX_D3D11_Init_ProjectID"));
    shutdown = reinterpret_cast<PFN_Shutdown>(get("NVSDK_NGX_D3D11_Shutdown"));
    shutdown1 = reinterpret_cast<PFN_Shutdown1>(get("NVSDK_NGX_D3D11_Shutdown1"));
    get_capability_parameters =
        reinterpret_cast<PFN_GetParams>(get("NVSDK_NGX_D3D11_GetCapabilityParameters"));
    get_parameters = reinterpret_cast<PFN_GetParams>(get("NVSDK_NGX_D3D11_GetParameters"));
    allocate_parameters = reinterpret_cast<PFN_GetParams>(get("NVSDK_NGX_D3D11_AllocateParameters"));
    destroy_parameters = reinterpret_cast<PFN_DestroyParams>(get("NVSDK_NGX_D3D11_DestroyParameters"));
    get_scratch_buffer_size =
        reinterpret_cast<PFN_GetScratch>(get("NVSDK_NGX_D3D11_GetScratchBufferSize"));
    create_feature = reinterpret_cast<PFN_CreateFeature>(get("NVSDK_NGX_D3D11_CreateFeature"));
    release_feature = reinterpret_cast<PFN_ReleaseFeature>(get("NVSDK_NGX_D3D11_ReleaseFeature"));
    evaluate_feature = reinterpret_cast<PFN_Evaluate>(get("NVSDK_NGX_D3D11_EvaluateFeature"));
    get_feature_requirements =
        reinterpret_cast<PFN_GetRequirements>(get("NVSDK_NGX_D3D11_GetFeatureRequirements"));

    // 区分实现来源：OptiScaler 会导出 OptiScaler 专有符号。
    if (get("OptiScaler") != nullptr || get("NVSDK_NGX_D3D12_EvaluateFeature") != nullptr)
        report.impl = "optiscaler";
    else if (get_capability_parameters != nullptr)
        report.impl = "nvidia-ngx";

    std::string missing;
    auto need = [&missing](const void *p, const char *name) {
        if (p == nullptr)
        {
            if (!missing.empty())
                missing += ',';
            missing += name;
        }
    };
    need(reinterpret_cast<const void *>(create_feature), "NVSDK_NGX_D3D11_CreateFeature");
    need(reinterpret_cast<const void *>(evaluate_feature), "NVSDK_NGX_D3D11_EvaluateFeature");
    need(reinterpret_cast<const void *>(release_feature), "NVSDK_NGX_D3D11_ReleaseFeature");
    report.missing = missing;
    report.ok = complete();
#else
    (void)module;
#endif
    return report;
}

bool Feature::create(NgxApi &api, ID3D11DeviceContext *context, NvngxParameter *params,
                     const Desc &desc)
{
    error_.clear();
    handle_ = nullptr;
    if (!api.complete())
    {
        error_ = "ngx_api_incomplete";
        return false;
    }
    if (params == nullptr)
    {
        error_ = "params_null";
        return false;
    }
    if (desc.render_w == 0 || desc.render_h == 0 || desc.display_w == 0 || desc.display_h == 0)
    {
        error_ = "bad_sizes";
        return false;
    }

    params->Set(key::kWidth, desc.render_w);
    params->Set(key::kHeight, desc.render_h);
    params->Set(key::kOutWidth, desc.display_w);
    params->Set(key::kOutHeight, desc.display_h);
    params->Set(key::kCreateFlags, static_cast<unsigned int>(desc.flags.flags));
    params->Set(key::kPerfQualityValue, static_cast<int>(desc.perf_quality));
    // subrect 显式给全尺寸：缺失时 DLSS 会回退 Width/Height，但显式给更利于排障。
    params->Set(key::kSubrectW, desc.render_w);
    params->Set(key::kSubrectH, desc.render_h);

    // ⚠️ context **不能为 null**。OptiScaler 的 NVSDK_NGX_D3D11_CreateFeature
    // （NVNGX_DLSS_Dx11.cpp:559）第一件事就是：
    //     InDevCtx->GetDevice(&D3D11Device);   // ← null 时 READ 0x0
    // 实机崩溃栈印证：fault_module OptiScaler.dll+0x246570（CreateFeature 内），
    //   access violation **READ address=0x0**。
    // NGX 的 DK11 CreateFeature 需要用 context 反查 device，所以必须传真 context。
    if (context == nullptr)
    {
        error_ = "context_null";
        return false;
    }

    constexpr int kNvngxFeatureSuperSampling = 1;
    NvngxHandle *handle = nullptr;
    const std::uint32_t rc = api.create_feature(context, kNvngxFeatureSuperSampling, params, &handle);
    if (rc != 1u || handle == nullptr)
    {
        char buf[96] {};
        std::snprintf(buf, sizeof(buf), "create_feature_failed(0x%08X)", static_cast<unsigned>(rc));
        error_ = buf;
        handle_ = nullptr;
        return false;
    }
    handle_ = handle;
    return true;
}

bool Feature::evaluate(NgxApi &api, ID3D11DeviceContext *context, NvngxParameter *params,
                       const FrameParams &frame, ID3D11Resource *color, ID3D11Resource *depth,
                       ID3D11Resource *motion, ID3D11Resource *output, ID3D11Resource *exposure,
                       ID3D11Resource *bias_mask, ID3D11Resource *transparency)
{
    error_.clear();
    if (handle_ == nullptr)
    {
        error_ = "not_created";
        return false;
    }
    if (params == nullptr)
    {
        error_ = "params_null";
        return false;
    }

    params->Set(key::kJitterX, frame.jitter_px);
    params->Set(key::kJitterY, frame.jitter_py);
    params->Set(key::kMvScaleX, frame.mv_scale_x);
    params->Set(key::kMvScaleY, frame.mv_scale_y);
    params->Set(key::kMvOffsetX, 0.0f);
    params->Set(key::kMvOffsetY, 0.0f);
    params->Set(key::kReset, frame.reset);
    // `Sharpness` 值：**照常传桥算出的值**，因为 OptiScaler 靠它决定是否
    // 启动 RCAS 后处理（`IFeature_Dx11.cpp:112` `if (_sharpness == 0) useRcas = false`）。
    // OptiScaler 在真正调用 NGX 之前会按自己的配置覆盖这个键
    // （`DLSSFeature.cpp:14-22` ProcessEvaluateParams），因此这里传值不会
    // 直接灌给 DLSS。
    //
    // 真正会触发"黑屏"的是 **CreateFeature 的 `DoSharpening` 标志**（那个
    // 已被 NVIDIA 标记废弃），本文件的 `map_create_flags` 已不再置它。
    params->Set(key::kSharpness, frame.sharpness);
    params->Set(key::kWidth, frame.render_w);
    params->Set(key::kHeight, frame.render_h);
    params->Set(key::kSubrectW, frame.render_subrect.w);
    params->Set(key::kSubrectH, frame.render_subrect.h);
    params->Set(key::kColor, color);
    params->Set(key::kDepth, depth);
    params->Set(key::kMotionVectors, motion);
    params->Set(key::kOutput, output);
    params->Set(key::kPreExposure, frame.pre_exposure);
    params->Set(key::kExposureScale, 1.0f);
    if (frame.bind_exposure_texture)
        params->Set(key::kExposureTexture, exposure);
    if (frame.bind_bias_mask)
    {
        params->Set(key::kBiasMask, bias_mask);
        // 同时喂给独立的 TransparencyMask 槽位：真实 NGX 两个都认，
        // OptiScaler 的 DLSS 后端把参数表原样透传（DLSSFeature_Dx11.cpp:88）。
        // 半透明/粒子区域靠它降低历史权重 —— 这是最常见的拖影来源。
        params->Set(key::kTransparencyMask, bias_mask);
    }
    // 子矩形基址显式置 0：DLSS 会读 `DLSS.Render.Subrect.Base.*` 定位渲染区域。
    // 桥只处理整张纹理（无 rect 概念），必须明确给 0 而不是留空 ——
    // 留空时某些驱动版本会沿用上一次的值，分屏/切分辨率后就会出现错位的鬼影。
    params->Set(key::kSubrectBaseX, 0u);
    params->Set(key::kSubrectBaseY, 0u);

    // OptiScaler 私有键：填了不会影响 DLSS 后端，但用户切回 FSR/FFX 输出时
    // 后端能拿到真实相机参数（否则会退化成 ini 的 0.1 / 100000 / 60°）。
    params->Set(key::kFsrCameraNear, frame.camera_near);
    params->Set(key::kFsrCameraFar, frame.camera_far);
    params->Set(key::kFsrCameraFov, frame.camera_fov_vertical);
    params->Set(key::kFsrFrameTimeDelta, frame.frame_time_ms);
    params->Set(key::kFrameTimeDeltaMsec, frame.frame_time_ms);
    params->Set(key::kFsrUpscaleW, frame.display_w);
    params->Set(key::kFsrUpscaleH, frame.display_h);
    // 桥的 transparency 在 NGX 侧没有槽位；OptiScaler 的 FSR/FFX 后端仍然读这个私有键，
    // 所以照常填 —— 这样"DLSS 输入 + FSR 输出"不会比"FSR2 输入 + FSR 输出"更差。
    params->Set(key::kFsrTransparency, transparency);
    params->Set(key::kFsrReactive, bias_mask);

    const std::uint32_t rc = api.evaluate_feature(context, handle_, params, nullptr);
    if (rc != 1u)
    {
        char buf[96] {};
        std::snprintf(buf, sizeof(buf), "evaluate_feature_failed(0x%08X)", static_cast<unsigned>(rc));
        error_ = buf;
        return false;
    }
    return true;
}

void Feature::destroy(NgxApi &api)
{
    if (handle_ != nullptr && api.release_feature != nullptr)
        api.release_feature(handle_);
    handle_ = nullptr;
}

bool Runtime::initialize(const RuntimeConfig &config, ID3D11Device *device,
                         ID3D11DeviceContext *context)
{
    (void)context;
    ready_ = false;
    error_.clear();
#if defined(_WIN32)
    HMODULE module = nullptr;
    // 候选顺序很重要：
    //   1. 配置名（默认 nvngx.dll）—— OptiScaler 的 LdrLoadDll 钩子认这个名字，
    //      会返回它自己的句柄，于是我们拿到完整的 NGX 实现（DLSS/XeSS/FSR 都能选）。
    //   2. _nvngx.dll —— OptiScaler 的 DllNames 也认这个名；且 N 卡驱动把它放在
    //      DriverStore 里（OptiScaler 自己就是从那里加载的）。游戏直接调它时
    //      同样会被拦下。
    //   3. nvngx_dlss.dll —— **不是** DLSS 输入（OptiScaler 的 CompareFileNameW
    //      要求匹配串前面是分隔符，nvngx_dlss 不匹配 nvngx 后缀），放最后只作兜底：
    //      若它真是 N 卡驱动运行时，也只是拿不到 OptiScaler 的完整链路。
    //   4. 已加载的模块 —— 游戏/其它插件可能自己先加载过。
    //
    // 为什么必须多候选：OptiScaler 只在 **LoadLibrary 那一刻**把名字换成自己；
    // 名字写错就完全拦不到，而 GetModuleHandle 也问不到（它还没被加载）。
    // 这两个坑本轮实机都踩过（日志 ngx=0 而 OptiScaler 明明在场）。
    std::vector<std::wstring> candidates;
    if (!config.module_hint.empty())
        candidates.push_back(config.module_hint);
    if (config.module_hint != L"nvngx.dll")
        candidates.emplace_back(L"nvngx.dll");
    candidates.emplace_back(L"_nvngx.dll");
    candidates.emplace_back(L"nvngx_dlss.dll");

    std::string tried;
    for (const std::wstring &name : candidates)
    {
        // 已在进程里就先用它（避免重复 LoadLibrary 触发 OptiScaler 的引用计数问题）
        module = GetModuleHandleW(name.c_str());
        if (module == nullptr)
            module = LoadLibraryW(name.c_str());
        if (!tried.empty())
            tried += ',';
        tried += narrow_ascii(name);
        if (module == nullptr)
            continue;

        // 解析导出；拿到就停（优先 OptiScaler，其次真实 NGX）。
        report_ = api_.resolve(module);
        if (report_.ok)
            break;
        module = nullptr;
    }

    if (module == nullptr)
    {
        error_ = "nvngx_not_found tried=" + tried;
        return false;
    }
    if (!report_.ok)
    {
        error_ = "ngx_exports_incomplete missing=" + report_.missing;
        return false;
    }
    if (config.require_optiscaler && report_.impl != "optiscaler")
    {
        error_ = "optiscaler_required_but_absent impl=" + report_.impl;
        return false;
    }
    if (!config.allow_real_ngx && report_.impl != "optiscaler")
    {
        error_ = "real_ngx_disallowed impl=" + report_.impl;
        return false;
    }

    // Init：优先 Init_ProjectID（OptiScaler 与 NVIDIA 都支持），
    // InSDKVersion 必须 >= 0x14 才会采用 LoggingInfo；用 0x0000015。
    //
    // ⚠️ 参数顺序：Init_ProjectID 是 (…, device, **InSDKVersion**, **InFeatureInfo**)，
    //    与 Init 的 (…, device, **InFeatureInfo**, **InSDKVersion**) 相反。
    //    写反会把 0x15 当指针 → 0xC0000005 READ 0x15（实机炸过一次）。
    constexpr std::uint32_t kSdkVersion = 0x0000015;
    std::uint32_t rc = 0;
    if (api_.init_project_id != nullptr)
    {
        rc = api_.init_project_id(config.project_id.c_str(), config.engine_type,
                                  config.engine_version.c_str(), L".", device,
                                  kSdkVersion, static_cast<const void *>(nullptr));
    }
    else if (api_.init != nullptr)
    {
        // Init 顺序相反：InFeatureInfo 在 InSDKVersion 之前。
        rc = api_.init(0x1337ull, L".", device, static_cast<const void *>(nullptr), kSdkVersion);
    }
    if (rc != 1u)
    {
        char buf[96] {};
        std::snprintf(buf, sizeof(buf), "ngx_init_failed(0x%08X)", static_cast<unsigned>(rc));
        error_ = buf;
        return false;
    }
    device_inited_ = true;

    NvngxParameter *params = nullptr;
    if (api_.get_capability_parameters != nullptr)
        rc = api_.get_capability_parameters(&params);
    else if (api_.get_parameters != nullptr)
        rc = api_.get_parameters(&params);
    if (rc != 1u || params == nullptr)
    {
        char buf[96] {};
        std::snprintf(buf, sizeof(buf), "ngx_params_failed(0x%08X)", static_cast<unsigned>(rc));
        error_ = buf;
        return false;
    }
    params_ = params;
    ready_ = true;
    return true;
#else
    // 非 Windows 构建：只保留纯映射层（测试可在任何平台跑）。
    (void)config;
    (void)device;
    error_ = "windows_only";
    return false;
#endif
}

bool Runtime::ensure_feature(const Feature::Desc &desc, ID3D11DeviceContext *context)
{
    if (!ready_)
        return false;
    const bool same = feature_.alive() && current_.render_w == desc.render_w &&
                      current_.render_h == desc.render_h && current_.display_w == desc.display_w &&
                      current_.display_h == desc.display_h &&
                      current_.flags.flags == desc.flags.flags &&
                      current_.perf_quality == desc.perf_quality;
    if (same)
        return true;
    // 尺寸/flags 变化必须重建：DLSS 对 CreateFeature 时的分辨率是硬约束。
    feature_.destroy(api_);
    // ⚠️ 早先这里传的是 nullptr，注释还写着"DX11 侧仅 Optionally 取 device"——**错的**。
    // OptiScaler 的实现（NVNGX_DLSS_Dx11.cpp:559）无条件 `InDevCtx->GetDevice(...)`，
    // 传 null 就是 READ 0x0（实机崩在 OptiScaler.dll+0x246570）。必须传真 context。
    if (!feature_.create(api_, context, params_, desc))
        return false;
    current_ = desc;
    return true;
}

void Runtime::shutdown()
{
    feature_.destroy(api_);
    if (device_inited_ && api_.shutdown != nullptr)
        api_.shutdown();
    device_inited_ = false;
    ready_ = false;
    params_ = nullptr;
}

} // namespace dlss_input
