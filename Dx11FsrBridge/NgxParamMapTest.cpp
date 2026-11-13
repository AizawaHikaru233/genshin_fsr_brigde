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
#include <string>   // std::string（第 [10] 节的档位名断言）

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

// ===========================================================================
// [10] 按 NGX 区间选档（2026-11-13 新增；本段**没有** oracle —— 见下）
// ===========================================================================
// 与 [1]~[9] 的性质不同：那些段是"重构、零行为变更"，靠 oracle 逐位对照；
// 本段是**新判据**，没有"重构前原文"可抄。所以这里钉的是**契约断言**：
//   ① 容差公式（1/64）与下界无容差的非对称；
//   ② 区间退化（含查询成功但字段为 0）一律判"不包含"，绝不退化成"不限制"；
//   ③ 多档重叠时取 optimal 最近者；距离完全相等时取固定优先序；
//   ④ 一个都不包含 / 全部查询失败 ⇒ matched=false（调用方据此回退比例表）。
// ===========================================================================
// [11] same_feature_input_key —— §5 自愈判据（2026-11-13 新增）
// ===========================================================================
// 为什么必须钉住它：这是"要不要重试一次 CreateFeature"的**唯一**判据。
//   漏一个字段 ⇒ 该重试的没重试（自愈失效）；多认一个每帧变的量 ⇒ 每帧重试（卡顿）。
//   两种失效都**不崩、不报错、编译零告警**，只有实机日志能看出来。
// ★ 断言方式是逐字段单独翻转再比较，而不是"整体相等/不等"两下 ——
//   后者在"比较函数少写一个字段"时**依然全绿**（那正是要抓的形态，AGENTS.md §三 #21）。
void test_same_feature_input_key()
{
    std::printf("[11] same_feature_input_key: 逐字段敏感 + 反射性 + 全等\n");

    CreateFeatureInputKey base {};
    base.render_w      = 1920;
    base.render_h      = 1080;
    base.display_w     = 2560;
    base.display_h     = 1440;
    base.perf_quality  = 2;
    base.render_preset = 0;
    base.create_flags  = 0x2u;
    base.color_fmt     = 23;   // R10G10B10A2_TYPELESS
    base.output_fmt    = 23;
    base.is_hdr        = false;

    // ---- 反射性：同一份 key 与自身相等（逐字段比较漏字段时这条仍会过，故不能只靠它）----
    CHECK(same_feature_input_key(base, base));
    {
        CreateFeatureInputKey copy = base;
        CHECK(same_feature_input_key(base, copy));   // 逐字段拷贝也必须相等
    }

    // ---- 逐字段敏感：**每一个**字段单独改一格，都必须判"不相等" ----
    //   ★ 这里刻意用表驱动 + 逐条 CHECK，让失败信息能指名道姓是哪个字段漏了比较。
    {
        CreateFeatureInputKey k = base; k.render_w     = base.render_w + 1;
        CHECK(!same_feature_input_key(base, k));
    }
    {
        CreateFeatureInputKey k = base; k.render_h     = base.render_h + 1;
        CHECK(!same_feature_input_key(base, k));
    }
    {
        CreateFeatureInputKey k = base; k.display_w    = base.display_w + 1;
        CHECK(!same_feature_input_key(base, k));
    }
    {
        CreateFeatureInputKey k = base; k.display_h    = base.display_h + 1;
        CHECK(!same_feature_input_key(base, k));
    }
    {
        CreateFeatureInputKey k = base; k.perf_quality = base.perf_quality + 1;
        CHECK(!same_feature_input_key(base, k));
    }
    {
        CreateFeatureInputKey k = base; k.render_preset = base.render_preset + 1;
        CHECK(!same_feature_input_key(base, k));
    }
    {
        CreateFeatureInputKey k = base; k.create_flags = base.create_flags | 0x1u;
        CHECK(!same_feature_input_key(base, k));
    }
    {
        CreateFeatureInputKey k = base; k.is_hdr       = !base.is_hdr;
        CHECK(!same_feature_input_key(base, k));
    }

    // ---- ★★ 独立审查第 5 条的反例：只改格式、其余全同 ----
    //   这正是原实现漏掉两个格式字段时"判为相同"的那个场景。
    //   AGENTS.md §三 #24：先自证前提 —— 除格式外其余字段确实完全相同，
    //   否则下面两条断言可能因别的原因通过（那会让它们变成假证据）。
    {
        CreateFeatureInputKey k = base;
        k.color_fmt  = 10;   // R16G16B16A16_FLOAT
        k.output_fmt = 10;
        // 前提自证：其余 8 个字段与 base 逐项相同。
        CHECK(k.render_w      == base.render_w);
        CHECK(k.render_h      == base.render_h);
        CHECK(k.display_w     == base.display_w);
        CHECK(k.display_h     == base.display_h);
        CHECK(k.perf_quality  == base.perf_quality);
        CHECK(k.render_preset == base.render_preset);
        CHECK(k.create_flags  == base.create_flags);
        CHECK(k.is_hdr        == base.is_hdr);
        // ⇒ 唯一差异就是格式 ⇒ 必须判不相等（否则自愈漏判）。
        CHECK(!same_feature_input_key(base, k));
        // 反向：单独改 output_fmt 也一样。
        CreateFeatureInputKey k2 = base;
        k2.output_fmt = 10;
        CHECK(k2.color_fmt == base.color_fmt);   // 前提自证
        CHECK(!same_feature_input_key(base, k2));
    }

    // ---- ★★ 2026-11-13（独立审查第二轮 · 阻断项 1）：color_fmt **单独**变化必须被判出来 ----
    //   缺陷原状（上述两块）：「只改格式」那个反例**同时**改了 color_fmt 与 output_fmt
    //   （见上块 k.color_fmt / k.output_fmt 两行）⇒ 它证明的是"至少有一个格式字段被比较"，
    //   **不是**"color_fmt 被比较"。实测：把 NgxParamMap.cpp 的 `a.color_fmt == b.color_fmt`
    //   换成 `true &&` 重新编译后，本段**依然** `3647 checks, 0 failures` / ALL PASS ——
    //   因为 output_fmt 仍不同，上面那条 CHECK 靠它通过，掩盖了 color_fmt 的比较缺失。
    //   而 k2 那条确实只改了 output_fmt ⇒ 缺的正是 color_fmt 的对称用例。
    //   ⇒ 这就是本函数开头注释警告、却恰好没被覆盖的形态（AGENTS.md §三 #21：
    //     "检查通过"不等于"内容正确"）。下面补一条，只动 color_fmt。
    //   AGENTS.md §三 #24（先自证前提）：先断言 output_fmt 与其余 8 个字段都与 base 相同，
    //   否则这条 CHECK 可能因**别的原因**通过，变成假证据。
    {
        CreateFeatureInputKey k = base;
        k.color_fmt = 10;   // R16G16B16A16_FLOAT；**只有这一个字段**变化
        // 前提自证（★ 最关键的一条）：output_fmt 必须仍等于 base
        // —— 它一旦不同，本块就退化成上面那块，抓不到 color_fmt 漏比。
        CHECK(k.output_fmt == base.output_fmt);
        CHECK(k.render_w      == base.render_w);
        CHECK(k.render_h      == base.render_h);
        CHECK(k.display_w     == base.display_w);
        CHECK(k.display_h     == base.display_h);
        CHECK(k.perf_quality  == base.perf_quality);
        CHECK(k.render_preset == base.render_preset);
        CHECK(k.create_flags  == base.create_flags);
        CHECK(k.is_hdr        == base.is_hdr);
        // ⇒ 唯一差异就是 color_fmt ⇒ 必须判不相等。
        //   变异测试（本轮实测）：只把 color_fmt 的比较换成 `true` ⇒ 本条变红。
        CHECK(!same_feature_input_key(base, k));
    }

    // ---- 非对称性护栏：两份**不同**的 key 不能因为"某字段恰好相等"而被判相同 ----
    {
        CreateFeatureInputKey a = base;
        CreateFeatureInputKey b = base;
        a.render_w = 1280; b.render_w = 1920;   // 只有 render_w 不同
        b.create_flags = base.create_flags;      // 其余保持
        CHECK(!same_feature_input_key(a, b));
    }

    // ---- 全 0 key：默认构造的两份必须相等（feature_fail_key 清零后与新建的比较）----
    {
        CreateFeatureInputKey z1 {};
        CreateFeatureInputKey z2 {};
        CHECK(same_feature_input_key(z1, z2));
        // 全 0 与 base 不同（避免"默认值恰好等于真实 key"的退化）。
        CHECK(!same_feature_input_key(z1, base));
    }
}

void test_quality_from_ranges()
{
    std::printf("[10] pick_quality_from_ranges: 区间包含 / 容差 / 重叠 / 回退\n");

    // ---- 档位整数值必须与 NGX 的枚举一致（NgxBackend.cpp 另有 static_assert 钉同一件事）----
    CHECK(static_cast<int>(PerfQuality::MaxPerformance)   == 0);
    CHECK(static_cast<int>(PerfQuality::Balanced)         == 1);
    CHECK(static_cast<int>(PerfQuality::MaxQuality)       == 2);
    CHECK(static_cast<int>(PerfQuality::UltraPerformance) == 3);
    CHECK(static_cast<int>(PerfQuality::UltraQuality)     == 4);
    CHECK(static_cast<int>(PerfQuality::Dlaa)             == 5);

    // ---- 档位名 ----
    CHECK(std::string(perf_quality_name(0)) == "MaxPerf");
    CHECK(std::string(perf_quality_name(5)) == "DLAA");
    CHECK(std::string(perf_quality_name(-1)) == "?");
    CHECK(std::string(perf_quality_name(6)) == "?");

    // ---- 容差公式：upper/64，整数除法 ----
    CHECK(range_upper_tolerance(0)    == 0);
    CHECK(range_upper_tolerance(63)   == 0);
    CHECK(range_upper_tolerance(64)   == 1);
    CHECK(range_upper_tolerance(127)  == 1);
    CHECK(range_upper_tolerance(1280) == 20);
    CHECK(range_upper_tolerance(2560) == 40);
    // ★ 明确钉住"不是 2 像素"：OPEN-ISSUES §4 原文写「1/64（约 2 像素）」，
    //   而公式在 1280 上是 20 像素。以公式为准，此断言防止后人按文字改成常量 2。
    CHECK(range_upper_tolerance(1280) != 2);

    // ---- 单档包含判定 ----
    RenderRange r {};
    r.opt_w = 1706; r.opt_h = 960;   // 2560x1440 输出、MaxQuality 档的典型值（**推断**，非实测）
    r.min_w = 1280; r.min_h = 720;
    r.max_w = 1706; r.max_h = 960;

    CHECK(render_range_contains(r, 1706, 960));          // 正好 optimal（本例中也是上界）
    CHECK(render_range_contains(r, 1280, 720));          // 正好下界
    CHECK(render_range_contains(r, 1706 + 26, 960 + 14)); // 上界 + (26, 14)，正好等于容差 (26, 15)
    CHECK(!render_range_contains(r, 1706 + 27, 960));    // 超容差 1 像素 ⇒ 不包含
    CHECK(!render_range_contains(r, 1706, 960 + 16));    // 高同理（容差 15）
    CHECK(!render_range_contains(r, 1279, 720));         // 低于下界 1 像素 ⇒ 不包含（下界无容差）
    CHECK(!render_range_contains(r, 1706, 719));

    // ---- 退化区间：一律"不包含"，不退化成"不限制" ----
    RenderRange zero = r;
    zero.min_w = 0;
    CHECK(!render_range_contains(zero, 1706, 960));
    RenderRange zero_max = r;
    zero_max.max_h = 0;
    CHECK(!render_range_contains(zero_max, 1706, 960));
    RenderRange inverted = r;
    inverted.min_w = 2000; inverted.max_w = 1000;
    CHECK(!render_range_contains(inverted, 1706, 960));
    CHECK(!render_range_contains(r, 0, 960));            // render 为 0 的防御

    // ---- 六档一起选：单个档位命中 ----
    RangeQuery q[6] {};
    auto set_ok = [&q](int i, std::uint32_t ow, std::uint32_t oh,
                       std::uint32_t mnw, std::uint32_t mnh,
                       std::uint32_t mxw, std::uint32_t mxh)
    {
        q[i].ok = true;
        q[i].range.opt_w = ow; q[i].range.opt_h = oh;
        q[i].range.min_w = mnw; q[i].range.min_h = mnh;
        q[i].range.max_w = mxw; q[i].range.max_h = mxh;
    };
    // 只让 MaxQuality 命中 1920x1080
    set_ok(0, 1280, 720, 1100, 620, 1300, 730);   // MaxPerf
    set_ok(1, 1481, 833, 1400, 790, 1500, 850);   // Balanced
    set_ok(2, 1920, 1080, 1700, 960, 1920, 1080); // MaxQuality ← 命中
    set_ok(3, 853, 480, 800, 450, 900, 500);      // UltraPerformance
    set_ok(4, 1920, 1080, 1800, 1000, 1900, 1070);// UltraQuality（max 略小 ⇒ 不命中）
    set_ok(5, 2560, 1440, 2500, 1400, 2560, 1440);// DLAA

    QualityPick p = pick_quality_from_ranges(q, 6, 1920, 1080);
    CHECK(p.matched == true);
    CHECK(p.index == static_cast<int>(PerfQuality::MaxQuality));
    CHECK(p.tol_w == 1920u / 64u);

    // ---- 区间重叠：取 optimal 离实际最近的那档 ----
    // 让 MaxQuality 与 Balanced 都包含 1500x850，但 Balanced 的 optimal 更近。
    set_ok(1, 1500, 850, 1400, 790, 1600, 900);   // Balanced  optimal=1500x850 ⇒ 距离 0
    set_ok(2, 1920, 1080, 1400, 790, 2000, 1120); // MaxQuality optimal=1920x1080 ⇒ 距离 420+230
    p = pick_quality_from_ranges(q, 6, 1500, 850);
    CHECK(p.matched == true);
    CHECK(p.index == static_cast<int>(PerfQuality::Balanced));

    // ---- 距离完全相等：按固定优先序（MaxQuality 先于 Balanced）----
    set_ok(1, 1500, 850, 1400, 790, 1600, 900);
    set_ok(2, 1500, 850, 1400, 790, 2000, 1120);
    p = pick_quality_from_ranges(q, 6, 1500, 850);
    CHECK(p.matched == true);
    CHECK(p.index == static_cast<int>(PerfQuality::MaxQuality));

    // ---- 一个都不包含 ⇒ matched=false（调用方回退比例表）----
    p = pick_quality_from_ranges(q, 6, 4000, 3000);
    CHECK(p.matched == false);
    CHECK(p.index == -1);

    // ---- 查询失败的档位不参与：把唯一命中的那档标成 ok=false ----
    q[2].ok = false;
    p = pick_quality_from_ranges(q, 6, 1500, 850);
    CHECK(p.matched == true);                                     // Balanced 仍命中
    CHECK(p.index == static_cast<int>(PerfQuality::Balanced));

    // ---- 全部查询失败 ⇒ matched=false ----
    for (int i = 0; i < 6; ++i) q[i].ok = false;
    p = pick_quality_from_ranges(q, 6, 1920, 1080);
    CHECK(p.matched == false);

    // ---- count 截断：只在传入的档位里选 ----
    for (int i = 0; i < 6; ++i) q[i].ok = true;
    p = pick_quality_from_ranges(q, 2, 1500, 850);   // 只允许 MaxPerf / Balanced
    CHECK(p.matched == true);
    CHECK(p.index == static_cast<int>(PerfQuality::Balanced));

    // ---- nullptr / count=0 不崩、判不命中 ----
    p = pick_quality_from_ranges(nullptr, 6, 1920, 1080);
    CHECK(p.matched == false);
    p = pick_quality_from_ranges(q, 0, 1920, 1080);
    CHECK(p.matched == false);

    // ---- 回归：0.999 精度那一档（2560x1440 display / 2556x1438 render）----
    // 旧比例表把它判成 UltraQuality ⇒ 实机 0xBAD00010。这里用**假设**的驱动区间。
    // ★ 断言的是"选档器行为"，不是"驱动真实区间"——后者只有实机日志能证明。
    // ★★ 本用例必须让 UltraQuality **因区间不包含而被排除**，不能靠平局优先序取胜：
    //    首版写成两档 opt 相同 ⇒ 距离相等 ⇒ 靠 kTieOrder 选中 MaxQuality，那样即使
    //    包含判据完全失效本用例也是绿的（AGENTS.md §三 #21 的形态）。
    //    ⇒ 现在让两档 opt **不同**，且 UltraQuality 的上界**不够**：
    //      max_w=2500 ⇒ 容差 2500/64=39 ⇒ 上界 2539 < 2556 ⇒ 宽超限；
    //      max_h=1400 ⇒ 容差 1400/64=21 ⇒ 上界 1421 < 1438 ⇒ 高超限。
    RangeQuery real[6] {};
    // DLAA 只接受恰好等于输出尺寸（不放大）；2556x1438 不在其中。
    real[5].ok = true;
    real[5].range = { 2560, 1440, 2560, 1440, 2560, 1440 };
    // MaxQuality：opt=1974x1111，min 低到 1280x720、max 顶到 2560x1440 ⇒ 2556x1438 在区间内。
    real[2].ok = true;
    real[2].range = { 1974, 1111, 1280, 720, 2560, 1440 };
    // UltraQuality：opt 与 MaxQuality 相同（距离也就相同），但上界太小 ⇒ 不包含。
    //   这样"选中 MaxQuality"只可能来自包含判据，不可能来自平局优先序。
    real[4].ok = true;
    real[4].range = { 1974, 1111, 1280, 720, 2500, 1400 };

    p = pick_quality_from_ranges(real, 6, 2556, 1438);
    CHECK(p.matched == true);
    CHECK(p.index == static_cast<int>(PerfQuality::MaxQuality));   // ★ 0.999 落 MaxQuality
    CHECK(p.index != static_cast<int>(PerfQuality::UltraQuality)); // ★ 不再落 UltraQuality
    // ★ 关键前提自证（AGENTS.md §三 #24）：证明 UltraQuality 确实是**因包含判据**被排除，
    //   而不是它压根没被查出来（那会让上一条断言以另一种方式"通过"）。
    CHECK(real[4].ok == true);                                     // 它是查到了的
    CHECK(!render_range_contains(real[4].range, 2556, 1438));      // 只是区间不包含
    CHECK(render_range_contains(real[2].range, 2556, 1438));       // MaxQuality 确实包含
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
    test_quality_from_ranges();
    test_same_feature_input_key();

    std::printf("=== %d checks, %d failures ===\n", g_checks, g_failures);
    if (g_failures == 0)
        std::printf("ALL PASS\n");
    return g_failures == 0 ? 0 : 1;
}
