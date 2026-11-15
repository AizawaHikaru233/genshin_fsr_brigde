// DlssInputTest.cpp — DlssInput 纯映射层的离线单测（零 D3D / 零 NGX 依赖）。
//
// 为什么这些断言值得写：
//   DLSS_INPUT.md 把"N 卡适配问题"逐条归因到**语义**，而不是配置。
//   语义是可以用纯函数钉住的。任何一条被改坏，都会以"拖影 / 抖动 / 亮度错"
//   的形式出现在实机上，而那种症状在实机里极难二分定位。
//
// 覆盖：jitter（量级/多样性/自生成）、motion 符号、create flags、perf quality、
//       reset 优先级、mask、subrect、route、build_frame 端到端。

#include "DlssInput.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char *expr, const char *file, int line)
{
    ++g_checks;
    if (!ok)
    {
        ++g_failures;
        std::printf("FAIL %s:%d  %s\n", file, line, expr);
    }
}

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)
#define CHECK_NEAR(a, b) check(std::fabs((a) - (b)) < 1e-5f, #a " ~ " #b, __FILE__, __LINE__)

using namespace dlss_input;

float test_halton(int i, int b)
{
    double f = 1.0, r = 0.0;
    while (i > 0)
    {
        f /= b;
        r += f * (i % b);
        i /= b;
    }
    return static_cast<float>(r);
}

// ---------------------------------------------------------------------------
void test_jitter_modes()
{
    std::printf("[1] jitter: reduce modes at real magnitude (halton/renderSize)\n");

    // 实机取证：游戏 FFX_FSR2.this+0x24 存的是 halton/renderSize（窄带 0~1/W），
    // 不是 [0,1) 裸相位。旧实现当相位再乘 W ⇒ 得到 halton（0..1 像素）
    // ⇒ 实际接近零抖动 ⇒ DLSS 时域不收敛。
    const unsigned W = 2556, H = 1438;

    JitterInput in {};
    in.render_w = W;
    in.render_h = H;
    in.mode = JitterMode::ZeroCentred;
    in.semantics = JitterInput::NormSemantics::HaltonOverSize;

    auto count_distinct = [](const std::vector<float> &v) {
        int n = 0;
        for (std::size_t i = 0; i < v.size(); ++i)
        {
            bool seen = false;
            for (std::size_t j = 0; j < i; ++j)
                if (std::fabs(v[j] - v[i]) < 1e-6f) { seen = true; break; }
            if (!seen) ++n;
        }
        return n;
    };

    const int kFrames = 64;
    std::vector<float> by_mode[3];
    const JitterReduce reduces[3] = { JitterReduce::PhaseScaled, JitterReduce::Wrap,
                                      JitterReduce::Clamp };
    for (int m = 0; m < 3; ++m)
    {
        in.reduce = reduces[m];
        for (int i = 1; i <= kFrames; ++i)
        {
            in.norm_x = test_halton(i, 2) / static_cast<float>(W);
            in.norm_y = test_halton(i, 3) / static_cast<float>(H);
            by_mode[m].push_back(map_jitter(in).px);
        }
    }

    const int d_phase = count_distinct(by_mode[0]);
    const int d_wrap = count_distinct(by_mode[1]);
    const int d_clamp = count_distinct(by_mode[2]);
    std::printf("    distinct over %d frames: PhaseScaled=%d Wrap=%d Clamp=%d\n",
                kFrames, d_phase, d_wrap, d_clamp);

    // 实机量级下三种归约都保留多样性：乘回 renderSize 后值本就落在 ±0.5 内，
    // clamp 没有可压缩空间 ⇒ "归约方式"不是画质主因，量级才是。
    CHECK(d_phase == kFrames);
    CHECK(d_wrap == kFrames);
    CHECK(d_clamp == kFrames);

    {
        float sum = 0.0f;
        bool pos = false, neg = false;
        for (float v : by_mode[0]) { sum += v; if (v > 0.05f) pos = true; if (v < -0.05f) neg = true; }
        const float mean = sum / static_cast<float>(by_mode[0].size());
        std::printf("    PhaseScaled mean=%.4f covers_both=%d\n", mean, (pos && neg) ? 1 : 0);
        CHECK(std::fabs(mean) < 0.1f);
        CHECK(pos && neg);
    }

    // ★ 决定性断言：输入 halton/W（≈0.0003），输出必须是亚像素量级。
    // 若哪天又丢了"乘回 renderSize"，这里立刻红灯。
    {
        const float sample_norm = 0.000342f;
        JitterInput one {};
        one.render_w = W;
        one.render_h = H;
        one.mode = JitterMode::ZeroCentred;
        one.reduce = JitterReduce::PhaseScaled;
        one.semantics = JitterInput::NormSemantics::HaltonOverSize;
        one.norm_x = sample_norm;
        one.norm_y = sample_norm;
        const JitterOutput o = map_jitter(one);
        std::printf("    magnitude: norm=%.6f -> px=%+.4f (must be sub-pixel)\n",
                    sample_norm, o.px);
        CHECK(std::fabs(o.px) > 0.2f);
        CHECK(std::fabs(o.px) <= 0.5f);
    }

    for (int m = 0; m < 3; ++m)
        for (float v : by_mode[m])
            CHECK(v >= -0.5f && v <= 0.5f);
}

// ---------------------------------------------------------------------------
void test_jitter_wrap_math()
{
    std::printf("[2] jitter: wrap boundaries and symmetry\n");

    CHECK_NEAR(wrap_to_half_pixel(0.0f), 0.0f);
    CHECK_NEAR(wrap_to_half_pixel(0.4999f), 0.4999f);
    CHECK_NEAR(wrap_to_half_pixel(-0.4999f), -0.4999f);
    CHECK_NEAR(wrap_to_half_pixel(0.5f), -0.5f);
    CHECK_NEAR(wrap_to_half_pixel(1.5f), -0.5f);
    CHECK_NEAR(wrap_to_half_pixel(1.0f), 0.0f);
    CHECK_NEAR(wrap_to_half_pixel(-1.0f), 0.0f);
    CHECK_NEAR(wrap_to_half_pixel(-1.5f), -0.5f);
    CHECK_NEAR(wrap_to_half_pixel(-0.7f), 0.3f);
    CHECK_NEAR(wrap_to_half_pixel(std::nanf("")), 0.0f);

    CHECK_NEAR(clamp_to_half_pixel(std::nanf("")), 0.0f);
    CHECK_NEAR(clamp_to_half_pixel(0.9f), 0.5f);
    CHECK_NEAR(clamp_to_half_pixel(-0.9f), -0.5f);
}

// ---------------------------------------------------------------------------
void test_jitter_modes_and_phase_compensation()
{
    std::printf("[3] jitter: modes + cb0 one-frame lag compensation\n");

    JitterInput in {};
    in.render_w = 1000;
    in.render_h = 1000;
    in.norm_x = 0.25f;
    in.norm_y = 0.75f;
    in.reduce = JitterReduce::None;
    in.semantics = JitterInput::NormSemantics::RawPhase;

    // ★ 2026-11-10 修复：相位**在模式变换之前**折回 [0,1)。
    // 旧行为允许 `0.5 - pix` 输出远超 ±0.5（实测 -1.12 / -2.00 / -2.52），
    // 破坏 NGX 的 Jitter.Offset 契约；而事后再 wrap 会翻转方向，
    // 表现就是"静止时线条也抖"。这里的断言钉住修复后的行为。
    //
    // norm=0.25, render_w=1000 => pix=250 => 折回 0 => ZeroCentred 得 0.5
    in.mode = JitterMode::ZeroCentred;
    JitterOutput o = map_jitter(in);
    CHECK_NEAR(o.px, 0.5f);
    CHECK(o.source == std::string("render"));

    // Negative：-pix 折回后仍为 -0.0（相位域 0）
    in.mode = JitterMode::Negative;
    o = map_jitter(in);
    CHECK_NEAR(o.px, 0.0f);

    in.mode = JitterMode::Zero;
    CHECK_NEAR(map_jitter(in).px, 0.0f);

    // cb 落后一帧 + prev_norm=0.5 => pix=500 => 折回 0 => 0.5
    in.mode = JitterMode::ZeroCentred;
    in.cb_lags_one_frame = true;
    in.prev_norm_x = 0.5f;
    in.prev_norm_y = 0.5f;
    o = map_jitter(in);
    CHECK(o.source == std::string("cb_prev"));
    CHECK_NEAR(o.px, 0.5f);

    JitterInput bad = in;
    bad.render_w = 0;
    CHECK(!map_jitter(bad).valid);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
void test_jitter_contract_bounds()
{
    std::printf("[3b] jitter: |offset| must stay within the NGX contract\n");

    // ★ 实机回归（2026-11-10）：三渲二角色线条**静止时也抖**。
    //
    // 根因不在 alpha，而在相位未被归一：游戏字段 norm 乘以 renderSize 后
    // 实测出现过 pix = 1.62 / 1.81 / 2.50 / 3.02，而 `0.5 - pix` 随即输出
    // -1.12 / -1.31 / -2.00 / -2.52 —— 远超 NGX 要求的 |Jitter.Offset| ≤ 0.5。
    // 旧实现在模式变换**之后**才 wrap，把 -1.997 折成 +0.003（**方向翻转**），
    // DLSS 因此每帧朝错误位置找历史 ⇒ 静止也抖。
    //
    // 这组断言用实测出现过的 norm 值钉住"相位先折回、再做模式变换"的顺序。
    const unsigned W = 2556, H = 1438;
    const float observed_norms[][2] = {
        { 0.000196f, 0.000232f }, { 0.000342f, 0.000386f },
        { 0.000635f, 0.000977f }, { 0.000708f, 0.001183f },
        { 0.000977f, 0.001183f }, { 0.001183f, 0.000635f },
    };
    const JitterMode modes[] = { JitterMode::ZeroCentred, JitterMode::PlusMinusHalf,
                                 JitterMode::Negative, JitterMode::Positive };
    for (int m = 0; m < 4; ++m)
    {
        for (int i = 0; i < 6; ++i)
        {
            JitterInput in {};
            in.render_w = W;
            in.render_h = H;
            in.mode = modes[m];
            in.reduce = JitterReduce::PhaseScaled;
            in.semantics = JitterInput::NormSemantics::HaltonOverSize;
            in.norm_x = observed_norms[i][0];
            in.norm_y = observed_norms[i][1];
            const JitterOutput o = map_jitter(in);
            CHECK(o.valid);
            // NGX 契约：严格落在 [-0.5, +0.5]
            CHECK(o.px >= -0.5f && o.px <= 0.5f);
            CHECK(o.py >= -0.5f && o.py <= 0.5f);
            // 说明：`raw_px/raw_py` 是**折回之前**的原始值，故意不设上下界
            // （它的作用就是让诊断日志能看到"原始值越界了多少"）。
            // 只有实际送出去的 `px/py` 受 NGX 契约约束 —— 即上面两条断言。
        }
    }

    // 方向不能因折返而翻转：pix 从 0.9 增到 1.1 时，ZeroCentred 的输出
    //   0.5-0.9 = -0.4 ；0.5-1.1 = -0.6 -> 折回后相位 0.1 -> +0.4
    // 这是**相位域**的正确行为（跨过一个完整周期），而不是空间方向的翻转。
    // 用同周期的两个相位验证连续性：pix=1.2 与 pix=1.8 应给出 0.5-0.2 与 0.5-0.8
    {
        JitterInput a {};
        a.render_w = 1000; a.render_h = 1000;
        a.mode = JitterMode::ZeroCentred;
        a.reduce = JitterReduce::PhaseScaled;
        a.semantics = JitterInput::NormSemantics::HaltonOverSize;
        a.norm_x = 0.0012f; // pix = 1.2 -> 相位 0.2
        a.norm_y = 0.0012f;
        CHECK_NEAR(map_jitter(a).px, 0.3f);

        a.norm_x = 0.0018f; // pix = 1.8 -> 相位 0.8
        a.norm_y = 0.0018f;
        CHECK_NEAR(map_jitter(a).px, -0.3f);
    }
}

// ---------------------------------------------------------------------------
void test_motion_sign()
{
    std::printf("[4] motion: sign derivation (FSR2 vs DLSS differ by one negation)\n");

    //   游戏原生：   history_uv = uv - mv_g
    //   桥解码纹理： mv_tex = -mv_g
    //   FSR2/FFX：   history = uv + mv*scale  ⇒ scale = +1
    //   DLSS/NGX：   prev    = uv - mv*scale  ⇒ scale = -1
    MotionInput in {};
    in.render_w = 1920;
    in.render_h = 1080;

    in.texture_sign = MotionTextureSign::Fsr2Negated;
    in.fixup = MotionFixup::UseNegativeScale;
    MotionOutput a = map_motion_scale(in);
    CHECK(a.valid);
    CHECK_NEAR(a.scale_x, -1920.0f);
    CHECK_NEAR(a.scale_y, -1080.0f);
    CHECK(!a.needs_texture_resign);

    in.fixup = MotionFixup::RequireNativeSign;
    MotionOutput b = map_motion_scale(in);
    CHECK(b.valid);
    CHECK(b.needs_texture_resign);
    CHECK_NEAR(b.scale_x, 1920.0f);

    in.texture_sign = MotionTextureSign::GameNative;
    in.fixup = MotionFixup::UseNegativeScale;
    MotionOutput c = map_motion_scale(in);
    CHECK_NEAR(c.scale_x, 1920.0f);
    CHECK(!c.needs_texture_resign);

    in.fixup = MotionFixup::UseNegativeScale;
    in.display_resolution_mv = true;
    CHECK(!map_motion_scale(in).valid);
}

// ---------------------------------------------------------------------------
void test_create_flags()
{
    std::printf("[5] flags: PQ code values must NOT be flagged IsHDR\n");

    CreateFlagInput in {};
    in.color = ColorEncoding::PqCodeValues;
    in.color_values_exceed_one = true;
    in.depth_inverted = true;
    in.have_exposure_texture = false;
    CreateFlagOutput o = map_create_flags(in);

    // PQ 码值 **不**置 IsHDR —— 实机否决过"置 IsHDR"的版本（拖影加重）。
    // FFX 的 HIGH_DYNAMIC_RANGE 与 NGX 的 IsHDR 不是同一件事：前者配合
    // NON_LINEAR_COLORSPACE+AUTO_EXPOSURE 让 SDK 解释 PQ 码值；NGX 没有那条路径。
    CHECK((o.flags & kFlagIsHdr) == 0);
    CHECK((o.flags & kFlagDepthInverted) != 0);
    CHECK((o.flags & kFlagAutoExposure) == 0);
    CHECK((o.flags & kFlagMvLowRes) != 0);

    in.color = ColorEncoding::LinearFp16;
    CHECK((map_create_flags(in).flags & kFlagIsHdr) != 0);

    // 线性 + 值域不超过 1 ⇒ SDR
    in.color_values_exceed_one = false;
    CHECK((map_create_flags(in).flags & kFlagIsHdr) == 0);

    // sRGB gamma 编码的 LDR ⇒ 不置 HDR
    in.color = ColorEncoding::SdrGamma;
    CHECK((map_create_flags(in).flags & kFlagIsHdr) == 0);

    in.have_exposure_texture = true;
    CHECK((map_create_flags(in).flags & kFlagAutoExposure) != 0);

    // ⚠️ 实机回归（2026-11-10）：`DoSharpening` 已被 NVIDIA 标记废弃
    // （nvsdk_ngx_defs.h:296 "Sharpness is not supported"）。
    // 置这一位会让 DLSS v310.7 输出**全黑**（游戏正常运行、画面无内容）。
    // 桥现在**永不**置它，锐化交给 OptiScaler 的 RCAS 后处理。
    in.sharpening = true;
    CHECK((map_create_flags(in).flags & kFlagDoSharpening) == 0);

    in.sharpening = false;
    in.mvs_contain_jitter = true;
    in.mvs_at_display_resolution = true;
    o = map_create_flags(in);
    CHECK((o.flags & kFlagMvJittered) != 0);
    CHECK((o.flags & kFlagMvLowRes) == 0);
    CHECK(!o.reasons.empty());
}

// ---------------------------------------------------------------------------
void test_perf_quality()
{
    std::printf("[6] perf quality: quantisation + UltraQuality rewrite avoidance\n");

    CHECK(map_perf_quality(3.0f, false) == kPerfUltraPerformance);
    CHECK(map_perf_quality(1.8f, false) == kPerfMaxPerf);
    CHECK(map_perf_quality(1.6f, false) == kPerfBalanced);
    CHECK(map_perf_quality(1.4f, false) == kPerfMaxQuality);
    CHECK(map_perf_quality(1.2f, false) == kPerfUltraQuality);
    CHECK(map_perf_quality(1.0f, false) == kPerfDlaa);

    // ★ 实机回归（2026-11-09）：render=2556x1438 display=2560x1440 => 1.0016。
    // 旧实现量化成 MaxQuality(1.5x) => DLSS 按 1.5x 构造重投影 => 运动拖影。
    // 接近 1:1 必须返回 DLAA，否则档位标签与实际比例错配。
    CHECK(map_perf_quality(2560.0f / 2556.0f, false) == kPerfDlaa);
    CHECK(map_perf_quality(2560.0f / 2556.0f, true) == kPerfDlaa);
    CHECK(map_perf_quality(1.04f, false) == kPerfDlaa);
    CHECK(map_perf_quality(1.06f, false) == kPerfUltraQuality);

    // OptiScaler 把 UltraQuality 改写成 MaxQuality
    CHECK(map_perf_quality(1.2f, true) == kPerfMaxQuality);
    CHECK(map_perf_quality(1.0f, true) == kPerfDlaa);

    CHECK_NEAR(perf_quality_ratio(kPerfDlaa), 1.0f);
    CHECK_NEAR(perf_quality_ratio(kPerfUltraPerformance), 3.0f);

    CHECK(map_perf_quality(-1.0f, false) == kPerfMaxQuality);
    CHECK(map_perf_quality(std::nanf(""), false) == kPerfMaxQuality);
}

// ---------------------------------------------------------------------------
void test_reset()
{
    std::printf("[7] reset: structural causes outrank heuristic gap\n");

    ResetFacts f {};
    f.first_frame = true;
    ResetDecision d = decide_reset(f);
    CHECK(d.reset && d.reason == ResetReason::FirstFrame);

    f.first_frame = false;
    f.size_changed = true;
    f.ms_since_last_dispatch = 900;
    d = decide_reset(f);
    CHECK(d.reset && d.reason == ResetReason::SizeChanged);

    ResetFacts g {};
    g.ms_since_last_dispatch = 900;
    d = decide_reset(g);
    CHECK(d.reset && d.reason == ResetReason::ResumeAfterGap);
    CHECK(std::string(reset_reason_name(d.reason)) == "resume_after_gap");

    ResetFacts h {};
    h.ms_since_last_dispatch = 100;
    d = decide_reset(h);
    CHECK(!d.reset && d.reason == ResetReason::None);

    ResetFacts i {};
    i.dispatch_failed = true;
    CHECK(decide_reset(i).reason == ResetReason::DispatchFailed);

    ResetFacts j {};
    j.forced = true;
    CHECK(decide_reset(j).reason == ResetReason::Forced);

    ResetFacts k {};
    k.explicit_camera_cut = true;
    k.ms_since_last_dispatch = 900;
    CHECK(decide_reset(k).reason == ResetReason::ExplicitCameraCut);
}

// ---------------------------------------------------------------------------
void test_masks()
{
    std::printf("[8] mask: transparency without a slot must be admitted as lost\n");

    MaskPlanInput in {};
    in.has_reactive = true;
    in.has_transparency = true;
    in.transparency = TransparencyHandling::Drop;
    MaskPlan p = plan_masks(in);
    CHECK(p.bind_bias_mask);
    CHECK(!p.fold_transparency);
    CHECK(std::string(p.note) == "transparency_dropped_ngx_has_no_slot");

    in.transparency = TransparencyHandling::FoldIntoReactive;
    p = plan_masks(in);
    CHECK(p.fold_transparency);
    CHECK(std::string(p.note) == "transparency_folded_into_reactive");

    MaskPlanInput only_tc {};
    only_tc.has_transparency = true;
    p = plan_masks(only_tc);
    CHECK(!p.bind_bias_mask);
    CHECK(std::string(p.note) == "only_transparency_present_but_ngx_has_no_tc_slot");

    MaskPlanInput dis {};
    dis.has_reactive = true;
    dis.disable_reactive = true;
    p = plan_masks(dis);
    CHECK(!p.bind_bias_mask);
    CHECK_NEAR(p.reactive_scale, 1.0f);

    MaskPlanInput scaled {};
    scaled.has_reactive = true;
    scaled.reactive_scale = 0.45f;
    CHECK_NEAR(plan_masks(scaled).reactive_scale, 0.45f);
}

// ---------------------------------------------------------------------------
void test_subrect()
{
    std::printf("[9] subrect: out-of-bounds must be rejected\n");

    const Rect full = full_rect(1920, 1080);
    CHECK(full.x == 0 && full.y == 0 && full.w == 1920 && full.h == 1080);
    CHECK(validate_subrect(full, 1920, 1080));

    Rect over {};
    over.w = 1920;
    over.h = 1080;
    over.x = 1;
    CHECK(!validate_subrect(over, 1920, 1080));

    Rect zero {};
    CHECK(!validate_subrect(zero, 1920, 1080));

    Rect inside {};
    inside.x = 960;
    inside.y = 540;
    inside.w = 960;
    inside.h = 540;
    CHECK(validate_subrect(inside, 1920, 1080));
}

// ---------------------------------------------------------------------------
void test_route()
{
    std::printf("[10] route: forced mode must actually be able to switch\n");

    RouteInput forced {};
    forced.vendor = Vendor::Nvidia;
    forced.user_preference = 1;
    forced.dlss_runtime_present = false;
    RouteOutput r = route_output(forced);
    CHECK(r.path == OutputPath::DlssInput);
    CHECK(std::string(r.reason) == "user_forced_dlss_try_load");

    forced.optiscaler_present = true;
    r = route_output(forced);
    CHECK(std::string(r.reason) == "user_forced_dlss_optiscaler_will_serve");

    forced.dlss_runtime_present = true;
    r = route_output(forced);
    CHECK(std::string(r.reason) == "user_forced_dlss_and_runtime_ok");

    RouteInput in {};
    in.vendor = Vendor::Nvidia;
    in.dlss_runtime_present = false;
    in.optiscaler_present = false;
    r = route_output(in);
    CHECK(r.path == OutputPath::FfxNative);
    CHECK(std::string(r.reason) == "no_dlss_runtime_and_no_optiscaler");

    in.optiscaler_present = true;
    r = route_output(in);
    CHECK(std::string(r.reason) == "optiscaler_present_will_serve_ngx");

    in.dlss_runtime_present = true;
    CHECK(std::string(route_output(in).reason) == "ngx_runtime_present");

    RouteInput amd {};
    amd.vendor = Vendor::Amd;
    amd.dlss_runtime_present = true;
    r = route_output(amd);
    CHECK(r.path == OutputPath::FfxNative);
    CHECK(std::string(r.reason) == "ngx_present_but_vendor_not_nvidia");

    amd.optiscaler_present = true;
    CHECK(route_output(amd).path == OutputPath::DlssInput);

    RouteInput off {};
    off.user_preference = 0;
    off.dlss_runtime_present = true;
    off.optiscaler_present = true;
    off.vendor = Vendor::Nvidia;
    CHECK(route_output(off).path == OutputPath::FfxNative);
    CHECK(std::string(route_output(off).reason) == "user_forced_ffx");
}

// ---------------------------------------------------------------------------
void test_build_frame()
{
    std::printf("[11] build_frame: end-to-end field consistency\n");

    FrameSpec spec {};
    spec.render_w = 1280;
    spec.render_h = 720;
    spec.display_w = 1920;
    spec.display_h = 1080;
    spec.norm_jitter_x = 0.25f;
    spec.norm_jitter_y = 0.25f;
    spec.jitter_mode = JitterMode::ZeroCentred;
    spec.jitter_reduce = JitterReduce::None;
    // 默认已改为 SelfGeneratedHalton（不读 norm）；本节验证"给定 norm 的换算"。
    spec.jitter_semantics = JitterInput::NormSemantics::RawPhase;
    spec.motion_sign = MotionTextureSign::Fsr2Negated;
    spec.motion_fixup = MotionFixup::UseNegativeScale;
    spec.mask = plan_masks([] {
        MaskPlanInput m {};
        m.has_reactive = true;
        return m;
    }());
    spec.reset_first_frame = true;

    const FrameParams f = build_frame(spec);
    CHECK(f.render_w == 1280 && f.render_h == 720);
    CHECK(f.display_w == 1920 && f.display_h == 1080);
    CHECK(f.render_subrect.w == 1280 && f.render_subrect.h == 720);
    // ★ 相位先折回 [0,1) 再变换：norm=0.25, w=1280 => pix=320 => 折回 0
    //   => ZeroCentred 得 0.5（旧断言把越界当正确，已更新）
    CHECK_NEAR(f.jitter_px, 0.5f);
    CHECK_NEAR(f.mv_scale_x, -1280.0f);
    CHECK(f.reset == 1);
    CHECK(f.reset_reason == std::string("first_frame"));
    CHECK(f.bind_bias_mask);
    CHECK(!f.bind_exposure_texture);
    CHECK_NEAR(f.pre_exposure, 1.0f);
    CHECK_NEAR(f.sharpness, 0.0f);

    FrameSpec calm = spec;
    calm.reset_first_frame = false;
    calm.ms_since_last_dispatch = 16;
    const FrameParams g = build_frame(calm);
    CHECK(g.reset == 0);
    CHECK(g.reset_reason == std::string("none"));

    FrameSpec sharp = calm;
    sharp.sharpening = true;
    sharp.sharpness = 0.4f;
    CHECK_NEAR(build_frame(sharp).sharpness, 0.4f);

    FrameSpec bad = calm;
    bad.frame_time_ms = std::nanf("");
    CHECK_NEAR(build_frame(bad).frame_time_ms, 16.7f);

    // 排障档：motion_disabled ⇒ scale 归零
    FrameSpec nomv = calm;
    nomv.motion_disabled = true;
    const FrameParams nm = build_frame(nomv);
    CHECK_NEAR(nm.mv_scale_x, 0.0f);
    CHECK_NEAR(nm.mv_scale_y, 0.0f);
}

// ---------------------------------------------------------------------------
void test_self_generated_halton()
{
    std::printf("[12] jitter: self-generated Halton (bypasses frozen game field)\n");

    // 实机确认 this+0x24 每 600~1200 帧才更新一次（render_calls 却与帧号 1:1）。
    // 冻结相位 ⇒ 每帧同一亚像素位置 ⇒ 时域不收敛 ⇒ K 拖影模糊、M/L 锯齿。
    float prev = 1e9f;
    int changes = 0;
    std::vector<float> xs;
    for (int i = 1; i <= 64; ++i)
    {
        JitterInput ji {};
        ji.semantics = JitterInput::NormSemantics::SelfGeneratedHalton;
        ji.frame_index = i;
        ji.render_w = 1920;
        ji.render_h = 1080;
        const JitterOutput o = map_jitter(ji);
        CHECK(o.valid);
        CHECK(o.px >= -0.5f && o.px <= 0.5f);
        CHECK(o.py >= -0.5f && o.py <= 0.5f);
        CHECK(o.source == std::string("halton_self"));
        if (std::fabs(o.px - prev) > 1e-6f)
            ++changes;
        prev = o.px;
        xs.push_back(o.px);
    }
    std::printf("    64 frames: %d distinct px values\n", changes);
    CHECK(changes == 64);

    {
        float sum = 0.0f;
        bool pos = false, neg = false;
        for (float v : xs) { sum += v; if (v > 0.05f) pos = true; if (v < -0.05f) neg = true; }
        const float mean = sum / static_cast<float>(xs.size());
        std::printf("    mean=%.4f covers_both=%d\n", mean, (pos && neg) ? 1 : 0);
        CHECK(std::fabs(mean) < 0.15f);
        CHECK(pos && neg);
    }

    // 完全忽略 norm 字段
    {
        JitterInput a {};
        a.semantics = JitterInput::NormSemantics::SelfGeneratedHalton;
        a.frame_index = 7;
        a.render_w = 1280;
        a.render_h = 720;
        a.norm_x = 0.0f;
        a.norm_y = 0.0f;
        const JitterOutput o1 = map_jitter(a);
        a.norm_x = 0.9f;
        a.norm_y = 0.1f;
        const JitterOutput o2 = map_jitter(a);
        CHECK_NEAR(o1.px, o2.px);
        CHECK_NEAR(o1.py, o2.py);
    }

    // 与官方 halton 值对齐
    {
        float x = 0.0f, y = 0.0f;
        halton_jitter(1, x, y);
        CHECK_NEAR(x, 0.0f);
        CHECK_NEAR(y, -0.1666667f);
        halton_jitter(2, x, y);
        CHECK_NEAR(x, -0.25f);
        CHECK_NEAR(y, 0.1666667f);
    }
}

} // namespace

int main()
{
    std::printf("=== DlssInputTest ===\n");
    test_jitter_modes();
    test_jitter_wrap_math();
    test_jitter_modes_and_phase_compensation();
    test_jitter_contract_bounds();
    test_motion_sign();
    test_create_flags();
    test_perf_quality();
    test_reset();
    test_masks();
    test_subrect();
    test_route();
    test_build_frame();
    test_self_generated_halton();

    std::printf("=== %d checks, %d failures ===\n", g_checks, g_failures);
    if (g_failures == 0)
        std::printf("ALL PASS\n");
    return g_failures == 0 ? 0 : 1;
}
