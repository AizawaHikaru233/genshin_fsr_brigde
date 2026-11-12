// NgxParamMapTest.cpp —— NgxParamMap 纯映射层的离线单测。
//
// ============================================================================
// 这个测试在证明什么
// ============================================================================
// 任务性质是 **L1 重构 / 零行为变更**。所以本测试不做"公式看起来对"的
// 自证，而是做两件事：
//
//   ① **oracle 对照**：把重构前 NgxBackend.cpp / Dx11FsrBridge.cpp 里的原始代码
//      **另抄一份**到本文件的 oracle_* 函数里（不是调用被测函数），
//      然后对同一组输入比较**输出的 IEEE 位模式**（std::bit_cast，不是 epsilon）。
//      ⇒ 如果被测函数抄错一个符号、一个 0.5、一个乘法顺序，位模式立刻不同。
//   ② **契约断言**：把与实机症状直接相关的不变量单独钉住
//      （MVLowRes 恒设 / mode 5 恒 0 / mode 2 原样 / reset 四组合 / 守卫 >= 语义）。
//
// ★ 为什么 oracle 必须"另抄一份"而不是调用 map_*：
//   调用被测函数当 oracle 是同义反复 —— 两边同时错就永远绿。
//   本文件里 oracle_* 与 map_* 是**两份独立的代码**，只有公式相同。
//
// 零依赖：不 include d3d11 / dxgi / nvsdk_ngx_*，不需要 GPU、游戏、NGX 头。
// 行尾：LF。
//
// ============================================================================
// 与任务书表述不一致的两处（**如实记录，不迁就**）
// ============================================================================
// (1) 任务书写「map_create_flags：4 个布尔输入的 16 种组合」。但任务书自己给出的
//     CreateFlagsInput 只有 **3 个** bool（is_hdr / depth_inverted / mv_jittered）
//     ⇒ 组合数是 2^3 = **8**。第 4 个"输入"是 MVLowRes，而它是**常量 1**、不是输入。
//     本测试覆盖全部 8 种组合，并对每一种都断言 MVLowRes 位存在（= 把 4 个位全钉死）。
//     若要真做 16 组合，得给结构体加第 4 个字段 —— 那是改接口，超出本次重构范围。
//
// (2) 任务书要求「验证 mode 3 的输出范围是 [-0.5, 0.5)」。**该命题在字面意义上不成立**：
//     mode 3 是 -(n*W) + 0.5。若 n 是裸的 [0,1) 相位（例如 n=0.5、W=2556），
//     输出是 -1277.5 —— 远离 [-0.5, 0.5)。
//     它只在 **n = halton/W** 这个语义下才落在范围里（实机取证：游戏字段存的是
//     halton/renderW，见 build-t6verify/_ref-dlss-input 的 DlssInput.h 注释），
//     且此时输出是 **(-0.5, +0.5]**（左开右闭），不是 [-0.5, 0.5)。
//     ⇒ 本测试断言的是**真实成立的那个命题**，并额外断言"裸相位下确实超界"，
//       把这条边界写进代码，防止后人按任务书的措辞去"修"公式。

#include "NgxParamMap.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace
{

int g_checks = 0;
int g_failures = 0;

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

// 位模式相等（不是 epsilon 相等）。NaN 也要按位比：同一段公式算出的 NaN
// 位模式相同，算错一步就会不同 —— 这正是重构要钉的东西。
bool same_bits(float a, float b)
{
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

void check_same_bits(float a, float b, const char *what, const char *file, int line)
{
    ++g_checks;
    if (!same_bits(a, b))
    {
        ++g_failures;
        std::printf("FAIL %s:%d  %s  got=%.9g(0x%08X) want=%.9g(0x%08X)\n", file, line, what,
                    static_cast<double>(a), std::bit_cast<std::uint32_t>(a),
                    static_cast<double>(b), std::bit_cast<std::uint32_t>(b));
    }
}

#define CHECK_BITS(got, want, what) check_same_bits((got), (want), (what), __FILE__, __LINE__)

using namespace ngx_param_map;

// ===========================================================================
// oracle —— 从**重构前源码**逐字另抄的一份实现
// ===========================================================================

// 来源：NgxBackend.cpp:1415-1421（build_create_flags 的决策部分）。
// 注意 :1417 那行 MVLowRes 前面**没有 if**（缩进只是排版）⇒ 无条件执行。
std::uint32_t oracle_create_flags(bool is_hdr, bool depth_inverted, bool mv_jittered)
{
    std::uint32_t flags = 0;
    if (is_hdr)             flags |= (1u << 0);   // IsHDR
                            flags |= (1u << 1);   // MVLowRes，恒设
    if (mv_jittered)        flags |= (1u << 2);   // MVJittered
    if (depth_inverted)     flags |= (1u << 3);   // DepthInverted
    return flags;
}

// 来源：Dx11FsrBridge.cpp:12580-12606（switch 逐字）。
// pre_x/pre_y = switch **之前** sdk_in.jitter_x/y 的值（生产上是 :12437 的 call_params）。
// case 2 是 break —— 不赋值，因此输出保留 pre 值。这正是本 oracle 必须带 pre 的原因。
struct OracleJitter
{
    float x;
    float y;
};

OracleJitter oracle_jitter(float pre_x, float pre_y, float use_x, float use_y,
                           std::uint32_t W, std::uint32_t H, std::uint32_t mode)
{
    float x = pre_x;
    float y = pre_y;
    switch (mode)
    {
        case 1:
            x = use_x * static_cast<float>(W);
            y = use_y * static_cast<float>(H);
            break;
        case 2:
            break;
        case 3:
            x = -(use_x * static_cast<float>(W)) + 0.5f;
            y = -(use_y * static_cast<float>(H)) + 0.5f;
            break;
        case 4:
            x = -(use_x * static_cast<float>(W));
            y = -(use_y * static_cast<float>(H));
            break;
        case 5:
            x = 0.0f;
            y = 0.0f;
            break;
        default:
            x = use_x * static_cast<float>(W) - 0.5f;
            y = use_y * static_cast<float>(H) - 0.5f;
            break;
    }
    return OracleJitter { x, y };
}

// 来源：Dx11FsrBridge.cpp:12655-12661。
struct OracleMvScale
{
    float x;
    float y;
};

OracleMvScale oracle_mv_scale(bool positive, float motion_scale,
                              std::uint32_t W, std::uint32_t H)
{
    const float mv_sign = positive ? 1.0f : -1.0f;
    OracleMvScale o {};
    o.x = mv_sign * static_cast<float>(W) * motion_scale;
    o.y = mv_sign * static_cast<float>(H) * motion_scale;
    return o;
}

// 来源：NgxBackend.cpp:2818。
bool oracle_reset(bool frame_reset, bool instance_first)
{
    return (frame_reset || instance_first) ? true : false;
}

// 来源：NgxBackend.cpp:2867。
bool oracle_reactive_size_ok(std::uint32_t pw, std::uint32_t ph,
                             std::uint32_t rw, std::uint32_t rh)
{
    return pw >= rw && ph >= rh;
}

// Halton(base) —— 实机取证里游戏字段的分子（见 DlssInput.h 的注释）。
float halton(int i, int b)
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

// ===========================================================================
// [1] map_create_flags —— 全部 8 种组合 + 4 个位逐个钉
// ===========================================================================
void test_create_flags()
{
    std::printf("[1] map_create_flags: 2^3 combinations vs oracle + MVLowRes constant\n");

    for (int mask = 0; mask < 8; ++mask)
    {
        CreateFlagsInput in {};
        in.is_hdr         = (mask & 1) != 0;
        in.depth_inverted = (mask & 2) != 0;
        in.mv_jittered    = (mask & 4) != 0;

        const CreateFlagsOutput out = map_create_flags(in);
        const std::uint32_t want = oracle_create_flags(in.is_hdr, in.depth_inverted, in.mv_jittered);
        CHECK(out.flags == want);

        // 4 个位逐位对账（不是只看整体值 —— 整体错位时逐位能指出是哪一位）。
        CHECK(((out.flags & kFlagIsHdr) != 0) == in.is_hdr);
        CHECK(((out.flags & kFlagDepthInverted) != 0) == in.depth_inverted);
        CHECK(((out.flags & kFlagMvJittered) != 0) == in.mv_jittered);
        // ★ 恒设位：与其余三个输入**无关**，8 种组合都必须为 1。
        CHECK((out.flags & kFlagMvLowRes) != 0);

        // 不得出现任何未声明的位（1<<4 / 1<<5 DoSharpening / 1<<6 / 1<<7 / 1<<8 一律不设）。
        CHECK((out.flags & ~(kFlagIsHdr | kFlagMvLowRes | kFlagMvJittered | kFlagDepthInverted)) == 0);
    }

    // 常量本身的值（NGX 头 nvsdk_ngx_defs.h:291-294 的口径）。
    CHECK(kFlagIsHdr == 1u << 0);
    CHECK(kFlagMvLowRes == 1u << 1);
    CHECK(kFlagMvJittered == 1u << 2);
    CHECK(kFlagDepthInverted == 1u << 3);

    // 全开 / 全关的两个具体值（便于人眼核对）。
    {
        CreateFlagsInput on {};
        on.is_hdr = true; on.depth_inverted = true; on.mv_jittered = true;
        CHECK(map_create_flags(on).flags == 0xFu);            // 1|2|4|8
        CreateFlagsInput off {};
        off.is_hdr = false; off.depth_inverted = false; off.mv_jittered = false;
        CHECK(map_create_flags(off).flags == 0x2u);           // 只剩 MVLowRes
    }
}

// ===========================================================================
// [2] map_jitter —— 6 个模式 x 边界值，逐位对 oracle
// ===========================================================================
void test_jitter_modes_vs_oracle()
{
    std::printf("[2] map_jitter: 6 modes x boundary values vs oracle (bit-exact)\n");

    const std::uint32_t sizes[][2] = {
        { 2556u, 1438u },   // 实机尺寸
        { 1920u, 1080u },
        { 1280u,  720u },
        {    1u,    1u },
        {    0u,    0u },   // 病态：尺寸 0
    };

    const float norms[] = {
        0.0f,
        0.25f,
        0.5f,
        0.75f,
        -0.5f,
        -0.25f,
        1.0f,                                    // 超界（[0,1) 语义下的越界点）
        1.5f,
        -1.0f,
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
    };

    int compared = 0;
    for (const auto &sz : sizes)
    {
        for (float n : norms)
        {
            for (std::uint32_t mode = 0; mode <= 6; ++mode)   // 含 6 = 非法值，必须落 default
            {
                for (int delay = 0; delay <= 1; ++delay)
                {
                    JitterInput in {};
                    in.norm_x = n;
                    in.norm_y = n * 0.5f;
                    in.render_w = sz[0];
                    in.render_h = sz[1];
                    in.mode = mode;
                    in.delay_one_frame = delay != 0;
                    in.prev_norm_x = 0.125f;
                    in.prev_norm_y = 0.375f;

                    const JitterOutput got = map_jitter(in);

                    const float use_x = delay ? in.prev_norm_x : in.norm_x;
                    const float use_y = delay ? in.prev_norm_y : in.norm_y;
                    // ★ 关键建模：生产上 switch **之前** sdk_in.jitter_x 已被赋成
                    //   call_params.jitter_x（:12437）。本测试让 pre == norm，这是
                    //   "调用点把 norm 传进来"的那条路径；pre != norm 的情形见 [3]。
                    const OracleJitter want =
                        oracle_jitter(in.norm_x, in.norm_y, use_x, use_y,
                                      in.render_w, in.render_h, in.mode);

                    CHECK_BITS(got.px, want.x, "jitter px vs oracle");
                    CHECK_BITS(got.py, want.y, "jitter py vs oracle");
                    ++compared;
                }
            }
        }
    }
    std::printf("    compared %d (size,norm,mode,delay) cases\n", compared);
}

// ===========================================================================
// [3] mode 2 的精确语义（**旧代码 case 2 是 break，不赋值**）
// ===========================================================================
void test_jitter_mode2_raw()
{
    std::printf("[3] map_jitter: mode 2 is raw (old case 2: break, no assignment)\n");

    // mode 2 不受 delay 影响：旧代码在 case 2 里根本不读 use_jx/use_jy。
    JitterInput in {};
    in.render_w = 2556;
    in.render_h = 1438;
    in.mode = 2;
    in.norm_x = 0.3125f;
    in.norm_y = 0.6875f;
    in.delay_one_frame = true;
    in.prev_norm_x = 0.9f;
    in.prev_norm_y = 0.1f;

    const JitterOutput out = map_jitter(in);
    CHECK_BITS(out.px, 0.3125f, "mode2 px == raw norm_x (not prev, not scaled)");
    CHECK_BITS(out.py, 0.6875f, "mode2 py == raw norm_y");

    // 与 oracle 在 pre == norm 时逐位一致。
    const OracleJitter want = oracle_jitter(in.norm_x, in.norm_y,
                                            in.prev_norm_x, in.prev_norm_y,
                                            in.render_w, in.render_h, 2);
    CHECK_BITS(out.px, want.x, "mode2 vs oracle (pre==norm)");

    // ★ 诚实记录差异：若 pre != norm（生产上 call_params.jitter_x != jit_src_x，
    //   例如走 cb0 回退路径），旧代码输出 pre，本函数输出 norm。
    //   ⇒ **调用点对 mode 2 必须不回写**，否则就是行为变更。
    //   本断言把这条约束写进测试：oracle(pre=0.9) 与 map(0.3125) **应当不同**。
    const OracleJitter pre_diff = oracle_jitter(0.9f, 0.1f, in.prev_norm_x, in.prev_norm_y,
                                                in.render_w, in.render_h, 2);
    CHECK(!same_bits(out.px, pre_diff.x));   // 证明"两者确实不同"⇒ 调用点不可回写
    std::printf("    note: pre!=norm would differ (%.4f vs %.4f) -> call site must not overwrite\n",
                static_cast<double>(out.px), static_cast<double>(pre_diff.x));
}

// ===========================================================================
// [4] mode 5 恒为 0；非法 mode 落 default（= mode 0）
// ===========================================================================
void test_jitter_zero_and_default()
{
    std::printf("[4] map_jitter: mode 5 always 0; unknown modes fall back to mode 0\n");

    const float norms[] = { 0.0f, 0.5f, 0.999f, -3.0f,
                            std::numeric_limits<float>::quiet_NaN(),
                            std::numeric_limits<float>::infinity() };
    for (float n : norms)
    {
        for (int delay = 0; delay <= 1; ++delay)
        {
            JitterInput in {};
            in.norm_x = n;
            in.norm_y = n;
            in.render_w = 2556;
            in.render_h = 1438;
            in.mode = 5;
            in.delay_one_frame = delay != 0;
            in.prev_norm_x = 0.7f;
            in.prev_norm_y = 0.7f;
            const JitterOutput out = map_jitter(in);
            CHECK_BITS(out.px, 0.0f, "mode5 px == 0");
            CHECK_BITS(out.py, 0.0f, "mode5 py == 0");
        }
    }

    // 非法 mode（6/7/100/0xFFFFFFFF）必须与 mode 0 逐位相同。
    const std::uint32_t bad_modes[] = { 6u, 7u, 100u, 0xFFFFFFFFu };
    for (std::uint32_t bad : bad_modes)
    {
        JitterInput a {};
        a.norm_x = 0.375f; a.norm_y = 0.625f;
        a.render_w = 1920; a.render_h = 1080;
        a.mode = bad;
        JitterInput b = a;
        b.mode = 0;
        const JitterOutput oa = map_jitter(a);
        const JitterOutput ob = map_jitter(b);
        CHECK_BITS(oa.px, ob.px, "bad mode == mode 0 (px)");
        CHECK_BITS(oa.py, ob.py, "bad mode == mode 0 (py)");
    }
}

// ===========================================================================
// [5] mode 3 的真实值域（见文件头 (2)：任务书的措辞不成立）
// ===========================================================================
void test_jitter_mode3_range()
{
    std::printf("[5] map_jitter: mode 3 range under halton/renderW semantics\n");

    const std::uint32_t W = 2556, H = 1438;

    // 实机语义：游戏字段 = halton/renderSize（分子 halton 在 [0,1)）⇒ n*W = halton。
    // 此时 -(halton) + 0.5 落在 (-0.5, +0.5]  —— **左开右闭**，不是任务书写的 [-0.5, 0.5)。
    bool saw_pos_half = false;
    bool saw_neg = false;
    for (int k = 0; k < 256; ++k)
    {
        JitterInput in {};
        in.norm_x = halton(k, 2) / static_cast<float>(W);
        in.norm_y = halton(k, 3) / static_cast<float>(H);
        in.render_w = W;
        in.render_h = H;
        in.mode = 3;
        const JitterOutput out = map_jitter(in);
        CHECK(out.px <= 0.5f);
        CHECK(out.px > -0.5f);
        CHECK(out.py <= 0.5f);
        CHECK(out.py > -0.5f);
        if (same_bits(out.px, 0.5f)) saw_pos_half = true;   // halton(0)=0 ⇒ +0.5 恰好取到
        if (out.px < 0.0f) saw_neg = true;
    }
    CHECK(saw_pos_half);   // 证明右端闭（0.5 可达）
    CHECK(saw_neg);        // 证明左端确实取负值

    // ★ 反过来：若 n 是**裸 [0,1) 相位**（旧注释的错误假设），mode 3 会大幅超界。
    //   这条断言是"防止后人按任务书措辞把公式改成 clamp"的证据。
    {
        JitterInput in {};
        in.norm_x = 0.5f;
        in.norm_y = 0.5f;
        in.render_w = W;
        in.render_h = H;
        in.mode = 3;
        const JitterOutput out = map_jitter(in);
        CHECK(out.px < -1000.0f);   // -(0.5*2556)+0.5 = -1277.5
        std::printf("    note: raw phase n=0.5 -> px=%.1f (out of +-0.5; semantics matter)\n",
                    static_cast<double>(out.px));
    }

    // mode 0 在 halton/renderW 语义下才是 [-0.5, +0.5)：halton-0.5，halton 在 [0,1)。
    {
        bool saw_left = false;
        for (int k = 0; k < 256; ++k)
        {
            JitterInput in {};
            in.norm_x = halton(k, 2) / static_cast<float>(W);
            in.norm_y = halton(k, 3) / static_cast<float>(H);
            in.render_w = W;
            in.render_h = H;
            in.mode = 0;
            const JitterOutput out = map_jitter(in);
            CHECK(out.px >= -0.5f);
            CHECK(out.px < 0.5f);
            if (same_bits(out.px, -0.5f)) saw_left = true;
        }
        CHECK(saw_left);   // halton(0)=0 ⇒ -0.5 恰好取到（左端闭）
    }
}

// ===========================================================================
// [6] map_mv_scale —— 正/负 scale x 不同尺寸，逐位对 oracle
// ===========================================================================
void test_mv_scale()
{
    std::printf("[6] map_mv_scale: sign x size x factor vs oracle (bit-exact)\n");

    const std::uint32_t sizes[][2] = {
        { 2556u, 1438u },
        { 1920u, 1080u },
        { 3840u, 2160u },
        {    1u,    1u },
        {    0u,    0u },
    };
    const float factors[] = { 1.0f, 0.5f, 2.0f, 4.0f, 0.0f, 0.01f, 1.0f / 3.0f };

    int compared = 0;
    for (const auto &sz : sizes)
    {
        for (float f : factors)
        {
            for (int pos = 0; pos <= 1; ++pos)
            {
                MvScaleInput in {};
                in.positive_scale = pos != 0;
                in.motion_scale = f;
                in.render_w = sz[0];
                in.render_h = sz[1];

                const MvScaleOutput got = map_mv_scale(in);
                const OracleMvScale want = oracle_mv_scale(in.positive_scale, f, sz[0], sz[1]);
                CHECK_BITS(got.x, want.x, "mv scale x vs oracle");
                CHECK_BITS(got.y, want.y, "mv scale y vs oracle");

                // 符号契约：正 scale ⇒ 非负；负 scale ⇒ 非正。
                // 尺寸 0 或 factor 0 时两边都是 ±0.0，只对非零情形断言严格符号。
                if (sz[0] != 0 && f != 0.0f)
                {
                    CHECK((got.x > 0.0f) == in.positive_scale);
                    CHECK((got.y > 0.0f) == in.positive_scale);
                }
                ++compared;
            }
        }
    }
    std::printf("    compared %d (size,factor,sign) cases\n", compared);

    // 本机默认配置（Ffx12MotionScale=1.0、Fsr2PositiveMotionVectorScale=0、2556x1438）
    // 的具体值，人眼可核：(-2556, -1438)。
    {
        MvScaleInput in {};
        in.positive_scale = false;
        in.motion_scale = 1.0f;
        in.render_w = 2556;
        in.render_h = 1438;
        const MvScaleOutput out = map_mv_scale(in);
        CHECK_BITS(out.x, -2556.0f, "default mv scale x");
        CHECK_BITS(out.y, -1438.0f, "default mv scale y");
    }
}

// ===========================================================================
// [7] map_reset —— 4 种组合
// ===========================================================================
void test_reset()
{
    std::printf("[7] map_reset: 4 combinations vs oracle\n");

    for (int mask = 0; mask < 4; ++mask)
    {
        ResetInput in {};
        in.frame_reset    = (mask & 1) != 0;
        in.instance_first = (mask & 2) != 0;
        const bool got = map_reset(in);
        const bool want = oracle_reset(in.frame_reset, in.instance_first);
        CHECK(got == want);
    }
    // 具体值：只有 (false,false) 为 false。
    {
        ResetInput a {}; a.frame_reset = false; a.instance_first = false;
        CHECK(map_reset(a) == false);
        ResetInput b {}; b.frame_reset = true;  b.instance_first = false;
        CHECK(map_reset(b) == true);
        ResetInput c {}; c.frame_reset = false; c.instance_first = true;
        CHECK(map_reset(c) == true);
        ResetInput d {}; d.frame_reset = true;  d.instance_first = true;
        CHECK(map_reset(d) == true);
    }
}

// ===========================================================================
// [8] reactive_size_ok —— 相等 / 大于 / 小于 / 零
// ===========================================================================
void test_reactive_size_ok()
{
    std::printf("[8] reactive_size_ok: equal / greater / less / zero\n");

    struct Case { std::uint32_t pw, ph, rw, rh; };
    const Case cases[] = {
        { 1920, 1080, 1920, 1080 },   // 相等 ⇒ true
        { 1920, 1080, 1280,  720 },   // 池更大 ⇒ true
        { 1920, 1080, 1921, 1080 },   // 宽小 1 ⇒ false
        { 1920, 1080, 1920, 1081 },   // 高小 1 ⇒ false
        { 1280,  720, 1920, 1080 },   // 池更小 ⇒ false
        { 1920, 1080, 1920,  720 },   // 只有高更大 ⇒ true（逐轴独立比较）
        { 1920, 1080,  720, 1080 },   // 只有宽更大 ⇒ true
        {    0,    0,    0,    0 },   // 全 0 ⇒ true（0>=0）
        {    0,    0,    1,    0 },   // 池 0 宽 vs 输入 1 ⇒ false
        { 2556, 1438, 2556, 1438 },   // 实机尺寸相等
    };
    for (const Case &c : cases)
    {
        const bool got = reactive_size_ok(c.pw, c.ph, c.rw, c.rh);
        const bool want = oracle_reactive_size_ok(c.pw, c.ph, c.rw, c.rh);
        CHECK(got == want);
    }

    // 极端值：UINT32_MAX 不得溢出误判。
    CHECK(reactive_size_ok(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu) == true);
    CHECK(reactive_size_ok(0xFFFFFFFFu, 0u, 0xFFFFFFFFu, 1u) == false);
}

// ===========================================================================
// [9] jitter valid 位（诊断用；不得影响 px/py）
// ===========================================================================
void test_jitter_valid_bit()
{
    std::printf("[9] map_jitter: valid is diagnostic only, never changes px/py\n");

    JitterInput ok {};
    ok.norm_x = 0.25f; ok.norm_y = 0.5f;
    ok.render_w = 1920; ok.render_h = 1080; ok.mode = 3;
    CHECK(map_jitter(ok).valid == true);

    JitterInput zero_size = ok;
    zero_size.render_w = 0;
    CHECK(map_jitter(zero_size).valid == false);

    JitterInput nan_in = ok;
    nan_in.norm_x = std::numeric_limits<float>::quiet_NaN();
    const JitterOutput nan_out = map_jitter(nan_in);
    CHECK(nan_out.valid == false);
    CHECK(std::isnan(nan_out.px));

    // valid 不改变 px/py：同一组输入，无论 valid 真假，公式结果都等于 oracle。
    const OracleJitter want = oracle_jitter(nan_in.norm_x, nan_in.norm_y,
                                            nan_in.norm_x, nan_in.norm_y,
                                            nan_in.render_w, nan_in.render_h, nan_in.mode);
    CHECK_BITS(nan_out.px, want.x, "NaN path px still equals oracle");
}

} // namespace

int main()
{
    std::printf("=== NgxParamMapTest ===\n");
    test_create_flags();
    test_jitter_modes_vs_oracle();
    test_jitter_mode2_raw();
    test_jitter_zero_and_default();
    test_jitter_mode3_range();
    test_mv_scale();
    test_reset();
    test_reactive_size_ok();
    test_jitter_valid_bit();

    std::printf("=== %d checks, %d failures ===\n", g_checks, g_failures);
    if (g_failures == 0)
        std::printf("ALL PASS\n");
    return g_failures == 0 ? 0 : 1;
}
