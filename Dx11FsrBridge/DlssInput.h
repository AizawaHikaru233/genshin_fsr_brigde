#pragma once
// DlssInput.h — DLSS（NVSDK_NGX）输入契约 + FSR2→DLSS 语义翻译层。
//
// ============================================================================
// 为什么需要这一个新文件
// ============================================================================
// 现状（见 DLSS_INPUT.md §1）：桥把自己扮成"一个调用 FSR2 / ffx-api 的游戏"，
// 于是 OptiScaler 只能走 **FSR2/FFX 输入** 那条降级链路。该链路的契约是
// `FfxFsr2DispatchDescription` / `ffxDispatchDescUpscale`，它在语义上**只是 FSR2**：
//   · jitter 无单位校验、无零中心保证（OptiScaler 直通不翻号）；
//   · motionVectorScale 直通不换算，符号由游戏的 FSR2 约定决定；
//   · 没有 subrect/base、没有 MV.Offset、没有 alpha upscaling；
//   · transparencyAndComposition 在 NGX 侧**没有对应 key**（只有 FSR/FFX/XeSS-Dx11 读它）；
//   · initFlags 被按位翻译成 NGX flags —— 其中 `FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE`
//     → `NVSDK_NGX_DLSS_Feature_Flags_IsHDR`。而本桥的 HDR 位含义是"我喂的是 PQ 码值、
//     请用 NON_LINEAR|AUTO_EXPOSURE 解释" —— NGX 没有这两个概念 ⇒ DLSS 会把 PQ 码值
//     当线性 HDR 处理（色调曲线错、暗部发灰/高光被压）。
// 这些都不是"配置问题"，是**输入契约选错了**。
//
// 本模块换一条契约：桥不再假装自己是 FSR2 游戏，而是**假装自己是一个 DLSS 游戏**
// （nvngx 客户端），按 NVSDK_NGX 的语义逐帧填参数表。这样：
//   · OptiScaler 走 `DLSS_DX11` 输入（ApiUpscalerInput::DLSS_DX11），吃到完整信息；
//   · 不需要 OptiScaler 时，`nvngx.dll`（NVIDIA 的 NGX 运行时）同样接得住 ⇒
//     **N 卡可以像 A 卡直出 FSR4 那样直出 DLSS**，OptiScaler 只留作 XeSS/FSR4-INT8 的可选项。
//
// ============================================================================
// 分层（刻意做成这样）
// ============================================================================
// 1) `纯映射层`：只做数值/语义换算，不碰 D3D、不碰 NGX —— 可离线单测
//    （DlssInputTest.cpp 就是这个消费者）。**所有 N 卡适配问题都能在这一层被钉住**。
// 2) `NGX ABI 层`：声明最小可用的 NVSDK_NGX 调用面（不 vendor NVIDIA 头，
//    只声明 ABI —— 与 nvsdk_ngx.h 的 vtable/签名逐项对应）。
// 3) `驱动层`：解析 nvngx.dll / OptiScaler 的 NGX 导出 → Init → CreateFeature →
//    每帧 Evaluate。和 `ffx12::dispatch` 同级，二选一，**共用同一份 FrameInput**。
//
// ⚠️ 本文件不 include 任何 D3D 头：只用前置声明，测试 target 才能零依赖编译。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Resource;
struct ID3D12Resource;

namespace dlss_input
{

// ===========================================================================
// 1. 纯映射层（无 D3D / 无 NGX / 可离线单测）
// ===========================================================================

// ---- 1.1 jitter ----------------------------------------------------------
//
// 契约（NGX）：`Jitter.Offset.X/Y` 是**当前帧**的亚像素抖动，
//   · 单位 = **渲染分辨率像素**（不是 UV，不是 display 像素）；
//   · 零中心，|v| ≤ 0.5（一个 phase 周期内均值 ≈ 0）；
//   · 符号 = "采样点相对像素中心的偏移"，同一时刻 color/MV/depth 必须共用同一个值。
//
// 本桥的游戏侧现状（Dx11FsrBridge.cpp:314-322、:12286-12310）：
//   游戏给的是归一化 [0,1) 的 Halton 相位（含 +0.5 常量偏置），桥上按
//   `Ffx12JitterMode` 换成像素；`Ffx12Mode3` = `-(n·W) + 0.5`，落在 (-0.5, +0.5]。
//
// **N 卡真正的坑不是公式，而是"钳制"**：`xess_or_dlss` 分支用
//   `if (v > 0.5) v = 0.5; if (v < -0.5) v = -0.5;`
// 做饱和钳制。若 `n·W` 的实际量级不是 [0,1)（例如 n 是像素量或相位未归一化），
// 绝大多数样本会被**压成恰好 ±0.5 的常量** ⇒ 时域上完全没有新的亚像素信息。
// 仓库自己也记录了这个现象（Fsr2FamilyTakeover.h:292-302：jitter_px 恒定，
// 13/13、108/108 次采样同值），并归因到"重建只能停在单帧放大"。
//
// 所以这里用 **wrap（取模折叠到半像素）** 而不是 **clamp（饱和）**：
//   · 二者在"值本来就在范围内"时逐位相同（⇒ 对现有 A 卡行为零影响）；
//   · 值超界时，wrap 保留相位序列的多样性，clamp 把它退化成二值常量。
enum class JitterReduce : std::uint8_t
{
    PhaseScaled = 0, // **推荐**：在相位域映射到 (-0.5,0.5]，保连续性与多样性（见下）
    Wrap = 1,        // 在**像素域**折返（旧默认；会把相位打散成稀疏格点，见下）
    Clamp = 2,       // 饱和到 [-0.5, 0.5]（桥最早的行为，A/B 用）
    None = 3,        // 不动（仅诊断：用来观察原始量级）
};

// 【为什么 PhaseScaled 才是对的】
// 游戏给的 `norm` 是 [0,1) 的 Halton 相位。若先算 `px = -(norm × W) + 0.5`
// 再 `wrap` 到 ±0.5，等价于取 `fract(-norm × W) ` 的小数部分——
// 因为 W 是整数，wrap 的结果只由 `norm` 的**低位**决定，落在 W 个格点中的一个。
// 实测（DlssInputTest 第 1 节，W=2556）：32 帧 Halton 序列只产生 **9 种**不同的
// 亚像素位置，且相邻帧大量重复（norm 0.5/0.25/0.75 全部映射到同一个 px）。
//
// 对 FSR2 这勉强能用（它只要求"有一定覆盖"），但 DLSS 的 transformer 模型
// （preset M/L）对**抖动序列的均匀性**敏感得多：它把 jitter 当作校准参数，
// 稀疏且周期化的序列会被解释成"相机在轻微但规律地移动"⇒ 线条/边缘出现
// 细密的规律抖动。这就是 M/L 上线条抖动的直接来源。
//
// PhaseScaled 的做法：**不做像素缩放，直接对相位取模**——
//     px = fract(norm + 0.5) - 0.5    ⇒ 落在 [-0.5, 0.5)，且对 norm 是**连续且双射**的
// 于是 Halton 的每一个新相位都对应一个新的亚像素位置，序列保持均匀覆盖。
// 这也正是 FFX/DLSS 官方 `GetJitterOffset` 的数值形态（它直接给出 [-0.5,0.5) 的
// 亚像素偏移，而不是"像素量再折返"）。

// 与桥 `Ffx12JitterMode` 一一对应（默认 3）。保留 0..5 是为了让 ini 语义不漂移。
enum class JitterMode : std::uint32_t
{
    PlusMinusHalf = 0, // +n·W − 0.5（旧默认，A/B）
    Positive = 1,      // +n·W（符号反 → 整体抖）
    Raw = 2,           // 原样（≈无抖动）
    ZeroCentred = 3,   // −n·W + 0.5（**默认**；repo 实测 DLSS M/L 只在这个模式下正常）
    Negative = 4,      // −n·W（恒定 −0.5px 偏置；DLSS 会出现规则网格状黑线）
    Zero = 5,          // 零抖动（诊断）
};

struct JitterInput
{
    // ---- 数值语义（**这一项决定一切，别照着旧注释想当然**）----
    // 实机取证（2026-11-09，RTX 4070 SUPER / 原神 7.x）：
    // FFX_FSR2 实例 `this+0x24`（Il2CppCallSiteHook 读作 jitter 的那个字段）
    // **不是** [0,1) 的 Halton 相位，而是 **halton / renderW**（Y 为 halton / renderH）。
    //
    // 反推依据：日志里的 norm_x 乘回 W=2556 得到
    //   0.5010  0.5010  0.2505  0.7489  0.1252  0.6262  0.3757  0.8742
    // 与 Halton(base 2) 的 0.5/0.75/0.125/0.625/0.375/0.875 精确对应
    // （Y 相对 base 3 序列**错位一帧** = 游戏双缓冲预录）。
    //
    // 后果：把该值当 [0,1) 相位再乘 W，得到的是 `halton`（0..1 **像素**）
    // ⇒ 等于**零抖动** ⇒ DLSS 时域累积拿不到亚像素位移 ⇒ 线条/边缘持续抖动。
    // 此前看到的"jitter 冻结"也是同一原因：值域只有 0~1/W，打印出来像常数。
    enum class NormSemantics : std::uint8_t
    {
        HaltonOverSize = 0, // halton/renderSize（游戏字段的实测形态）
        RawPhase,           // [0,1) 裸相位（Il2CppCallSiteHook.h 的原始假设）
        AlreadyPixels,      // 已是像素抖动（GetJitterOffset 形态）
        // **自生成**：完全忽略 norm_x/norm_y，用 frame_index 生成 Halton(2,3)。
        // 这是实机验证后采用的默认：游戏字段每 600~1200 帧才更新一次，
        // 冻结的相位 = 时域不收敛 = 拖影/模糊。
        SelfGeneratedHalton,
    };

    float norm_x = 0.0f;
    float norm_y = 0.0f;
    std::uint32_t render_w = 0;
    std::uint32_t render_h = 0;
    JitterMode mode = JitterMode::ZeroCentred;
    JitterReduce reduce = JitterReduce::PhaseScaled;
    // ⚠️ 默认是 **HaltonOverSize**（用游戏字段），不是 SelfGeneratedHalton。
    // 自生成序列曾在实机上让拖影明显加重：DLSS 的 Jitter.Offset 必须与游戏
    // **实际施加**到投影矩阵的抖动一致，否则每帧都在错误位置采样历史。
    // 保留 SelfGeneratedHalton 仅作对照实验，不能作为默认。
    NormSemantics semantics = NormSemantics::HaltonOverSize;
    // 自生成模式用的帧序号（仅 SelfGeneratedHalton 使用）。
    int frame_index = 0;
    // 上采样器必须与实际采样用的 jitter 对齐。桥从 cb0 取 jitter 时实测
    // **落后一个 token**（Dx11FsrBridge.cpp:12143-12147），而 Render 钩子里那枚
    // 是当前帧的 ⇒ 走 cb0 路径必须补偿。true 时用上一帧的 norm（调用方传入）。
    bool cb_lags_one_frame = false;
    float prev_norm_x = 0.0f;
    float prev_norm_y = 0.0f;
};

struct JitterOutput
{
    float px = 0.0f;   // 送 NGX 的像素抖动（**已满足 |v| ≤ 0.5**，reduce=None 除外）
    float py = 0.0f;
    float raw_px = 0.0f; // reduce 之前的值（诊断/日志：证伪"抖动被钳死"）
    float raw_py = 0.0f;
    bool reduced = false;   // 发生过 wrap/clamp（说明原始量级超界 ⇒ 上游单位可疑）
    bool valid = false;     // render_w/h 非 0 且非 NaN
    const char *source = ""; // "render" / "cb_prev"（诊断）
};

JitterOutput map_jitter(const JitterInput &in);

// 把任意像素值折返到 [-0.5, 0.5)（保留相位多样性）。这是 NGX 契约的上界，
// 也是"抖动不再被钳成常量"的关键：钳制只保证幅度、不保证序列。
float wrap_to_half_pixel(float v);
// 饱和到 [-0.5, 0.5]（桥的旧行为，A/B 用）。
float clamp_to_half_pixel(float v);

// ---------------------------------------------------------------------------
// Halton 序列生成（**实测必需**）
// ---------------------------------------------------------------------------
// 实机取证（2026-11-09）：游戏 `FFX_FSR2.this+0x24` 那个字段**不是逐帧相位**——
//   · `render_calls` 与帧号严格 1:1（钩子每帧都触发）；
//   · 但该字段每 600~1200 帧才变一次（日志 n=34200/34800 同值，35400 才跳）。
// 把这种"长期冻结"的值当 jitter 喂给 DLSS，后果是：
//   每帧都从**同一亚像素位置**采样 ⇒ 时域累积无法收敛 ⇒
//   既像拖影又像模糊，且 K(CNN) 最敏感、M/L(transformer) 相对耐受
//   —— 与实机观察完全一致。
//
// 因此正确做法是**自己生成**标准 Halton(2,3) 低差异序列。这也是 FFX/DLSS
// 官方 `GetJitterOffset` 的做法（Halton base 2/3，输出 [-0.5,+0.5) 亚像素偏移），
// 不依赖任何游戏侧不确定字段。
float halton(int index, int base);
// 生成第 index 帧的零中心亚像素抖动（[-0.5, +0.5)），与官方 GetJitterOffset 同形。
void halton_jitter(int index, float &out_x, float &out_y);

// ---- 1.2 motion vector ---------------------------------------------------
//
// 三方约定（依次更"原始"）：
//   · 游戏原生：`mv_g`，满足 `history_uv = uv − mv_g`（repo 反汇编实证，:12356-12358）。
//   · FSR2：`history_uv = uv + scale·mv` ⇒ 桥把解码结果取负、`scale = +renderSize`。
//   · NGX/DLSS：`prev_uv = uv − mv`（mv = curr − prev）。**与 FSR2 反号**。
//
// 所以"同一张解码纹理 + 同一个正 scale" 交给 DLSS 会得到**反向运动**。
// 两条等价修法：
//   A) 用负 scale（纹理不动，零额外 GPU 成本）—— 推荐给"复用桥的解码结果"，
//      但要求消费方不把 MV.Scale 当"幅值"去 clamp；
//   B) 解码阶段不取负（纹理即游戏原生约定），scale 保持正 —— 语义最干净，
//      代价是多一份解码产物（或把 Ffx12MotionDecode 的符号做成按输出模式切换）。
enum class MotionTextureSign : std::uint8_t
{
    Fsr2Negated = 0, // 桥现状：解码时已取负（供 FSR2）
    GameNative,      // 未取负（游戏原生；DLSS 首选）
};

enum class MotionFixup : std::uint8_t
{
    UseNegativeScale = 0, // A：纹理不动，MV.Scale 取负
    RequireNativeSign,    // B：要求解码纹理是游戏原生符号（否则拒绝，避免静默拖影）
};

struct MotionInput
{
    MotionTextureSign texture_sign = MotionTextureSign::Fsr2Negated;
    MotionFixup fixup = MotionFixup::UseNegativeScale;
    std::uint32_t render_w = 0;
    std::uint32_t render_h = 0;
    bool display_resolution_mv = false; // 桥当前恒 false（render 尺寸语义）
};

struct MotionOutput
{
    float scale_x = 0.0f;
    float scale_y = 0.0f;
    bool needs_texture_resign = false; // fixup=RequireNativeSign 且纹理符号不对 ⇒ 调用方必须换纹理
    bool valid = false;
    const char *note = "";
};

MotionOutput map_motion_scale(const MotionInput &in);

// ---- 1.3 create flags ----------------------------------------------------
//
// NGX flags（nvsdk_ngx_defs.h:254-267）：
//   IsHDR=1<<0  MVLowRes=1<<1  MVJittered=1<<2  DepthInverted=1<<3
//   DoSharpening=1<<5  AutoExposure=1<<6  AlphaUpscaling=1<<7
constexpr std::uint32_t kFlagIsHdr = 1u << 0;
constexpr std::uint32_t kFlagMvLowRes = 1u << 1;
constexpr std::uint32_t kFlagMvJittered = 1u << 2;
constexpr std::uint32_t kFlagDepthInverted = 1u << 3;
constexpr std::uint32_t kFlagDoSharpening = 1u << 5;
constexpr std::uint32_t kFlagAutoExposure = 1u << 6;
constexpr std::uint32_t kFlagAlphaUpscaling = 1u << 7;

// 颜色的**真实**编码。注意这与"FFX 的 HDR flag"不是一个概念：
//   · PqCodeValues —— 游戏原生 R10G10B10A2，存 PQ 码值（≈SDR 量纲，非线性）。
//     NGX 没有 NON_LINEAR_COLORSPACE，也没有 AUTO_EXPOSURE 的等价物 ⇒ 必须
//     IsHDR=false，并如实声明 PreExposure=1（或走 PQ→线性 FP16 预处理后声明 LinearFp16）。
//   · LinearFp16 —— 已经过 PqToLinear 的 R16G16B16A16_FLOAT。此时 IsHDR **可以**为真
//     （前提是值域确实 >1）。
enum class ColorEncoding : std::uint8_t
{
    PqCodeValues = 0,
    LinearFp16,
    SdrGamma,
};

struct CreateFlagInput
{
    ColorEncoding color = ColorEncoding::PqCodeValues;
    bool color_values_exceed_one = false; // 只有线性且确实 >1 才算 HDR
    bool depth_inverted = true;
    bool mvs_contain_jitter = false;
    bool mvs_at_display_resolution = false;
    bool have_exposure_texture = false; // 有 1x1 exposure 才能开 AutoExposure
    bool sharpening = false;            // 内置锐化（桥用 RCAS 时不设，避免双重锐化）
    // AlphaUpscaling（1<<7）：NVIDIA 官方头只有枚举、**没有任何语义说明**，
    // 因此不猜它的行为——只暴露成开关做实机 A/B，默认关。
    // 候选用途：让 DLSS 正确处理 alpha 混合边缘（三渲二描边 / 头发边缘）。
    // 实测有效才考虑改默认；无效则永久保持关闭。
    bool alpha_upscaling = false;
};

struct CreateFlagOutput
{
    std::uint32_t flags = 0;
    std::vector<std::string> reasons; // 每个 bit 的判定依据（日志/取证）
};

CreateFlagOutput map_create_flags(const CreateFlagInput &in);

// ---- 1.4 质量档 ----------------------------------------------------------
//
// NGX 的 `PerfQualityValue` 是**离散**枚举（0..5）。OptiScaler 的 FSR2 输入路径
// 会先用连续 ratio 量化到 6 档（FSR2_Dx11.cpp:92-105），随后 DLSSFeature 又把
// `UltraQuality` 改写成 `MaxQuality`（DLSSFeature.cpp:254-259）—— **两处信息损失**。
// 走 DLSS 输入时我们直接给离散值，绕开第一次量化；第二次改写只能靠"选一个不会被
// 改写的档"规避：需要 UltraQuality 的 ratio 时，用 MaxQuality 显式表达并在日志里说明。
enum : std::uint32_t
{
    kPerfMaxPerf = 0,
    kPerfBalanced = 1,
    kPerfMaxQuality = 2,
    kPerfUltraPerformance = 3,
    kPerfUltraQuality = 4,
    kPerfDlaa = 5,
};

std::uint32_t map_perf_quality(float display_over_render_ratio, bool avoid_ultra_quality_rewrite);
float perf_quality_ratio(std::uint32_t quality);

// ---- 1.5 reset 判定 ------------------------------------------------------
//
// NGX 的 `Reset` 语义是"丢掉该 feature handle 的历史"。它与 FFX 的 reset 同样是
// per-context（= per instance），但**触发条件不能照抄**：
//   · 桥现有触发是"距上次 dispatch > 500ms"（:12105-12121）——这是**间接代理**，
//     真实相机跳变（切场景/过场）不会被它捕获，而一次网络卡顿会**凭空**丢历史；
//   · 多实例并存时，A 实例的 gap 不能当成 B 实例的 reset。
// 这里把它写成**纯状态机**，逐条给出理由，便于日志对账与单测。
enum class ResetReason : std::uint8_t
{
    None = 0,
    Forced,          // 配置强制（A/B、排障）
    DispatchFailed,  // 上一次派发失败 ⇒ 历史已不可信
    InstanceChanged, // feature handle 换了（新视图/新 context）
    SizeChanged,     // render/display 尺寸变化 ⇒ 必须重建
    FirstFrame,      // 首次派发
    ResumeAfterGap,  // 接管空窗后恢复（间接代理，最低优先级）
    ExplicitCameraCut, // 未来：由相机钩子给出真实跳变
};

const char *reset_reason_name(ResetReason reason);

struct ResetFacts
{
    bool first_frame = false;
    bool size_changed = false;
    bool instance_changed = false;
    bool dispatch_failed = false;
    bool forced = false;
    bool explicit_camera_cut = false;
    std::uint64_t ms_since_last_dispatch = 0;
    std::uint64_t resume_gap_ms = 500; // 桥的现有阈值
};

struct ResetDecision
{
    bool reset = false;
    ResetReason reason = ResetReason::None;
};

ResetDecision decide_reset(const ResetFacts &facts);

// ---- 1.6 mask 策略 -------------------------------------------------------
//
// NGX 只有一个"当前帧颜色偏差"蒙版（`DLSS.Input.Bias.Current.Color.Mask`，即 reactive），
// **没有** transparencyAndComposition 的等价物。
//   · 桥的 reactive 来自 motion.b（Ffx12Backend.cpp:509），值域/含义从未为 NGX 标定；
//   · 桥的 transparency 抓到但从未送进 dispatch（:12799-12803）。
// 诚实做法：reactive 送，但要显式声明重标定系数；transparency 不能假装能送，
// 要么折进 reactive（取 max），要么明确关闭并在日志里留痕。
enum class TransparencyHandling : std::uint8_t
{
    Drop = 0,      // 丢弃（与现状等价，但**承认**它被丢弃）
    FoldIntoReactive, // 折进 reactive（取 max），对粒子/半透明更友好
};

struct MaskPlanInput
{
    bool has_reactive = false;
    bool has_transparency = false;
    bool disable_reactive = false; // 对应 OptiScaler 的 DisableReactiveMask
    float reactive_scale = 1.0f;
    TransparencyHandling transparency = TransparencyHandling::Drop;
};

struct MaskPlan
{
    bool bind_bias_mask = false;
    float reactive_scale = 1.0f;
    bool fold_transparency = false;
    const char *note = "";
};

MaskPlan plan_masks(const MaskPlanInput &in);

// ---- 1.7 subrect --------------------------------------------------------
//
// DLSS 需要 render/output 的**矩形**而不只是尺寸（超宽、分屏、动态分辨率都要它）。
// 桥现在只有 renderSize（取自纹理 desc），完全没有 rect 概念。
struct Rect
{
    std::uint32_t x = 0, y = 0, w = 0, h = 0;
};

Rect full_rect(std::uint32_t w, std::uint32_t h);
// 校验 subrect 是否落在纹理内（DLSS 对越界 rect 会直接失败）。
bool validate_subrect(const Rect &r, std::uint32_t tex_w, std::uint32_t tex_h);

// ---- 1.8 输出路由 --------------------------------------------------------
//
// "要不要走 DLSS 输入"是个纯决策：与显卡厂商、OptiScaler 是否在场、用户偏好有关。
enum class Vendor : std::uint8_t
{
    Unknown = 0,
    Amd,
    Nvidia,
    Intel,
};

enum class OutputPath : std::uint8_t
{
    FfxNative = 0, // 现状：桥自己 ffx-api → FSR2/3/4
    DlssInput,     // 本模块：桥作为 NGX 客户端
};

struct RouteInput
{
    Vendor vendor = Vendor::Unknown;
    bool optiscaler_present = false;
    bool dlss_runtime_present = false; // nvngx.dll 可解析到 NVSDK_NGX_D3D11_*
    int user_preference = -1;          // ini：-1=auto / 0=ffx / 1=dlss
};

struct RouteOutput
{
    OutputPath path = OutputPath::FfxNative;
    const char *reason = "";
};

RouteOutput route_output(const RouteInput &in);

// ---- 1.9 单帧参数包 ------------------------------------------------------
//
// 纯数据：由纯映射层填，驱动层只负责"把它 Set 进 NGX 参数表"。
// 之所以分两步，是为了让单测能直接断言"最终送给 NGX 的每个字段"。
struct FrameParams
{
    std::uint32_t render_w = 0, render_h = 0;
    std::uint32_t display_w = 0, display_h = 0;
    Rect render_subrect {};
    float jitter_px = 0.0f, jitter_py = 0.0f;
    float mv_scale_x = 0.0f, mv_scale_y = 0.0f;
    float pre_exposure = 1.0f;
    int reset = 0;
    float sharpness = 0.0f;
    float frame_time_ms = 16.7f;
    float camera_near = 0.1f, camera_far = 1000.0f, camera_fov_vertical = 1.0472f;
    bool bind_exposure_texture = false;
    bool bind_bias_mask = false;
    std::string reset_reason;
};

struct FrameSpec
{
    // 输入（全部来自桥已有的抓取结果）
    std::uint32_t render_w = 0, render_h = 0;
    std::uint32_t display_w = 0, display_h = 0;
    float norm_jitter_x = 0.0f, norm_jitter_y = 0.0f;
    float prev_norm_jitter_x = 0.0f, prev_norm_jitter_y = 0.0f;
    JitterMode jitter_mode = JitterMode::ZeroCentred;
    JitterReduce jitter_reduce = JitterReduce::Wrap;
    // 抖动来源：默认自生成 Halton（游戏字段实测冻结，见 JitterInput 的说明）。
    JitterInput::NormSemantics jitter_semantics =
        JitterInput::NormSemantics::SelfGeneratedHalton;
    int jitter_frame_index = 0;
    bool jitter_from_cb0 = false; // true ⇒ 需要补偿一帧相位差
    MotionTextureSign motion_sign = MotionTextureSign::Fsr2Negated;
    MotionFixup motion_fixup = MotionFixup::UseNegativeScale;
    bool mvs_display_res = false;
    // 排障档：true ⇒ 完全不用运动矢量（scale 置 0）。
    // 这是**决定性诊断**——若关闭后"一动就模糊"消失，问题 100% 在 MV 链路
    // （符号或量级）；若仍存在，则与 MV 无关（mask / depth / jitter 方向）。
    bool motion_disabled = false;
    bool have_exposure_texture = false;
    bool sharpening = false;
    float sharpness = 0.0f;
    float frame_time_ms = 16.7f;
    float camera_near = 0.1f, camera_far = 1000.0f, camera_fov_vertical = 1.0472f;
    bool reset_forced = false;
    bool reset_dispatch_failed = false;
    bool reset_instance_changed = false;
    bool reset_size_changed = false;
    bool reset_first_frame = false;
    bool reset_camera_cut = false;
    std::uint64_t ms_since_last_dispatch = 0;
    MaskPlan mask {};
};

// 组装一帧的全部 NGX 字段（纯函数）。
FrameParams build_frame(const FrameSpec &spec);

// ===========================================================================
// 2. NGX ABI 层（最小声明，不 vendor NVIDIA 头）
// ===========================================================================
// 以下 vtable 顺序必须与 nvsdk_ngx_params.h 中 `NVSDK_NGX_Parameter` 逐项一致：
//   8 × Set，8 × Get，1 × Reset。
// 名字里的 `Nvngx` 前缀是刻意的：**我们不是在复刻 NVIDIA 的头文件**，
// 而是在声明一个进程内 ABI 形状（参数对象由 nvngx/OptiScaler 构造并交给我们）。
struct NvngxResult
{
    std::uint32_t value = 0;
    bool success() const { return value == 1u; } // NVSDK_NGX_Result_Success == 1
    explicit operator bool() const { return success(); }
};

struct NvngxParameter
{
    virtual void Set(const char *name, unsigned long long v) = 0;
    virtual void Set(const char *name, float v) = 0;
    virtual void Set(const char *name, double v) = 0;
    virtual void Set(const char *name, unsigned int v) = 0;
    virtual void Set(const char *name, int v) = 0;
    virtual void Set(const char *name, ID3D11Resource *v) = 0;
    virtual void Set(const char *name, ID3D12Resource *v) = 0;
    virtual void Set(const char *name, void *v) = 0;

    virtual NvngxResult Get(const char *name, unsigned long long *v) const = 0;
    virtual NvngxResult Get(const char *name, float *v) const = 0;
    virtual NvngxResult Get(const char *name, double *v) const = 0;
    virtual NvngxResult Get(const char *name, unsigned int *v) const = 0;
    virtual NvngxResult Get(const char *name, int *v) const = 0;
    virtual NvngxResult Get(const char *name, ID3D11Resource **v) const = 0;
    virtual NvngxResult Get(const char *name, ID3D12Resource **v) const = 0;
    virtual NvngxResult Get(const char *name, void **v) const = 0;

    virtual void Reset() = 0;
};

struct NvngxHandle
{
    unsigned int Id = 0;
};

// 公开的参数名常量（与 nvsdk_ngx_defs.h 的字面量一致）。
namespace key
{
constexpr const char *kWidth = "Width";
constexpr const char *kHeight = "Height";
constexpr const char *kOutWidth = "OutWidth";
constexpr const char *kOutHeight = "OutHeight";
constexpr const char *kPerfQualityValue = "PerfQualityValue";
constexpr const char *kCreateFlags = "DLSS.Feature.Create.Flags";
constexpr const char *kJitterX = "Jitter.Offset.X";
constexpr const char *kJitterY = "Jitter.Offset.Y";
constexpr const char *kMvScaleX = "MV.Scale.X";
constexpr const char *kMvScaleY = "MV.Scale.Y";
constexpr const char *kMvOffsetX = "MV.Offset.X";
constexpr const char *kMvOffsetY = "MV.Offset.Y";
constexpr const char *kReset = "Reset";
constexpr const char *kSharpness = "Sharpness";
constexpr const char *kColor = "Color";
constexpr const char *kDepth = "Depth";
constexpr const char *kMotionVectors = "MotionVectors";
constexpr const char *kOutput = "Output";
constexpr const char *kExposureTexture = "ExposureTexture";
constexpr const char *kPreExposure = "DLSS.Pre.Exposure";
constexpr const char *kExposureScale = "DLSS.Exposure.Scale";
constexpr const char *kBiasMask = "DLSS.Input.Bias.Current.Color.Mask";
// 独立的透明度蒙版参数（与 bias mask 不同）。真实 NGX 会读它，
// 而 OptiScaler 的 DLSS 后端把 InParameters **原样透传**给 nvngx
// （DLSSFeature_Dx11.cpp:88），所以设了就能生效。
// 桥抓到的 T4 合成蒙版语义上正是"这一帧有多少透明/粒子覆盖"，
// 同时喂给这两个槽位是抑制半透明区域拖影的双保险。
constexpr const char *kTransparencyMask = "TransparencyMask";
constexpr const char *kSubrectBaseX = "DLSS.Render.Subrect.Base.X";
constexpr const char *kSubrectBaseY = "DLSS.Render.Subrect.Base.Y";
constexpr const char *kSubrectW = "DLSS.Render.Subrect.Dimensions.Width";
constexpr const char *kSubrectH = "DLSS.Render.Subrect.Dimensions.Height";
// OptiScaler 私有键（只有 FSR/FFX/XeSS-Dx11 后端读；DLSS 后端读不到）。
// 仍然填：这样用户切回 FSR 输出时相机参数不会退化成 ini 默认值。
constexpr const char *kFsrCameraNear = "FSR.cameraNear";
constexpr const char *kFsrCameraFar = "FSR.cameraFar";
constexpr const char *kFsrCameraFov = "FSR.cameraFovAngleVertical";
constexpr const char *kFsrFrameTimeDelta = "FSR.frameTimeDelta";
constexpr const char *kFsrTransparency = "FSR.transparencyAndComposition";
constexpr const char *kFsrReactive = "FSR.reactive";
constexpr const char *kFsrUpscaleW = "FSR.upscaleSize.width";
constexpr const char *kFsrUpscaleH = "FSR.upscaleSize.height";
constexpr const char *kFrameTimeDeltaMsec = "FrameTimeDeltaInMsec";
} // namespace key

// ===========================================================================
// 3. 驱动层
// ===========================================================================
// 解析结果（失败时给出**可读原因**，不静默）。
struct ResolveReport
{
    bool ok = false;
    std::string module_path;
    std::string module_name; // 实际拿到的模块名（可能是 OptiScaler 冒充的那个）
    std::string impl;        // "nvidia-ngx" / "optiscaler" / "unknown"
    std::string missing;     // 缺失的导出名（逗号分隔）
};

class NgxApi
{
  public:
    // 从已加载模块解析导出。`module` 为 HMODULE。
    ResolveReport resolve(void *module);

    // ⚠️⚠️ 这两个函数的**后两个参数顺序不同**，是本项目实机崩溃过一次的地方：
    //
    //   NVSDK_NGX_D3D11_Init(appId, path, device, InFeatureInfo, InSDKVersion)
    //                                                   ^^^^^^^^^^^^  ^^^^^^^^^^^ 先 FeatureInfo
    //   NVSDK_NGX_D3D11_Init_ProjectID(projectId, engineType, engineVersion,
    //                                  path, device, InSDKVersion, InFeatureInfo)
    //                                                ^^^^^^^^^^^  ^^^^^^^^^^^^ 先 SDKVersion
    //
    // 写反的后果不是"参数被忽略"，而是把整数 0x15（SDKVersion）当成指针解引用：
    //   OptiScaler.dll+0x244D09: movups xmm0, xmmword ptr [rax]   ; rax = 0x15
    //   ⇒ 0xC0000005 READ address=0x15 ⇒ 整个游戏进程被带走。
    //
    // 依据：external/nvngx_dlss_sdk/nvsdk_ngx.h:134（Init）与
    //       OptiScaler/inputs/NVNGX_DLSS_Dx11.cpp:206-212（Init_ProjectID 的实现）。
    using PFN_Init = std::uint32_t (*)(unsigned long long, const wchar_t *, ID3D11Device *,
                                       const void *, std::uint32_t);
    using PFN_InitProjectId = std::uint32_t (*)(const char *, int, const char *, const wchar_t *,
                                                ID3D11Device *, std::uint32_t, const void *);
    using PFN_Shutdown = std::uint32_t (*)();
    using PFN_Shutdown1 = std::uint32_t (*)(ID3D11Device *);
    using PFN_GetParams = std::uint32_t (*)(NvngxParameter **);
    using PFN_DestroyParams = std::uint32_t (*)(NvngxParameter *);
    using PFN_GetScratch = std::uint32_t (*)(int, const NvngxParameter *, std::size_t *);
    using PFN_CreateFeature = std::uint32_t (*)(ID3D11DeviceContext *, int, NvngxParameter *,
                                                NvngxHandle **);
    using PFN_ReleaseFeature = std::uint32_t (*)(NvngxHandle *);
    using PFN_Evaluate = std::uint32_t (*)(ID3D11DeviceContext *, const NvngxHandle *,
                                           NvngxParameter *, void *);
    using PFN_GetRequirements = std::uint32_t (*)(void *, const void *, void *);

    PFN_Init init = nullptr;
    PFN_InitProjectId init_project_id = nullptr;
    PFN_Shutdown shutdown = nullptr;
    PFN_Shutdown1 shutdown1 = nullptr;
    PFN_GetParams get_capability_parameters = nullptr;
    PFN_GetParams get_parameters = nullptr;
    PFN_GetParams allocate_parameters = nullptr;
    PFN_DestroyParams destroy_parameters = nullptr;
    PFN_GetScratch get_scratch_buffer_size = nullptr;
    PFN_CreateFeature create_feature = nullptr;
    PFN_ReleaseFeature release_feature = nullptr;
    PFN_Evaluate evaluate_feature = nullptr;
    PFN_GetRequirements get_feature_requirements = nullptr;

    bool complete() const;
};

// 一次 feature 的生命周期（每个游戏实例一个）。
class Feature
{
  public:
    struct Desc
    {
        std::uint32_t render_w = 0, render_h = 0;
        std::uint32_t display_w = 0, display_h = 0;
        CreateFlagOutput flags {};
        std::uint32_t perf_quality = kPerfMaxQuality;
        bool bind_exposure_texture = false;
    };

    // 建 feature。失败时 `error()` 给出原因（含 NGX 返回码的十六进制）。
    bool create(NgxApi &api, ID3D11DeviceContext *context, NvngxParameter *params,
                const Desc &desc);
    bool evaluate(NgxApi &api, ID3D11DeviceContext *context, NvngxParameter *params,
                  const FrameParams &frame, ID3D11Resource *color, ID3D11Resource *depth,
                  ID3D11Resource *motion, ID3D11Resource *output,
                  ID3D11Resource *exposure, ID3D11Resource *bias_mask,
                  ID3D11Resource *transparency);
    void destroy(NgxApi &api);
    bool alive() const { return handle_ != nullptr; }
    const std::string &error() const { return error_; }
    NvngxHandle *handle() { return handle_; }

  private:
    NvngxHandle *handle_ = nullptr;
    std::string error_;
};

// 模块名/路径的窄字符化（诊断日志用；无法转换的字符降为 '?'，绝不抛异常）。
std::string narrow_ascii(const std::wstring &wide);

// 模块级单例（桥在 initialize() 里 Initialize，dispatch 里 Dispatch）。
// 刻意做成"显式生命周期"而不是全局构造：DllMain 里禁止 LoadLibrary。
struct RuntimeConfig
{
    // 模块名/路径用**宽字符**：安装目录可能含中文（见 README 的"不含中文"警告，
    // 但用户目录并不保证），而 LoadLibraryW 只吃宽字符。
    std::wstring module_hint = L"nvngx.dll"; // 可被 ini 覆盖；OptiScaler 会把它拦下并返回自身
    bool allow_real_ngx = true;              // 没有 OptiScaler 时直接用 NVIDIA NGX（直出 DLSS）
    bool require_optiscaler = false;
    std::string project_id = "genshin-fsr-bridge-dlss";
    int engine_type = 3; // NVSDK_NGX_EngineType_Custom
    std::string engine_version = "1.0.0";
};

class Runtime
{
  public:
    // 解析模块 + 解析导出 + 构造参数表。可重复调用（内部缓存）。
    bool initialize(const RuntimeConfig &config, ID3D11Device *device, ID3D11DeviceContext *context);
    bool ready() const { return ready_; }
    const ResolveReport &report() const { return report_; }
    const std::string &error() const { return error_; }
    NvngxParameter *params() { return params_; }
    NgxApi &api() { return api_; }
    // 尺寸/实例变化时重建 feature。
    // ⚠️ `context` **必须**传游戏的 immediate context：NGX 的 D3D11 CreateFeature
    //    用 `InDevCtx->GetDevice()` 反查设备，传 null 会在 OptiScaler 内部 READ 0x0。
    bool ensure_feature(const Feature::Desc &desc, ID3D11DeviceContext *context);
    Feature &feature() { return feature_; }
    const std::string &feature_error() const { return feature_.error(); }
    void shutdown();

  private:
    NgxApi api_ {};
    ResolveReport report_ {};
    NvngxParameter *params_ = nullptr;
    Feature feature_ {};
    bool ready_ = false;
    bool device_inited_ = false;
    std::string error_;
    Feature::Desc current_ {};
};

} // namespace dlss_input
