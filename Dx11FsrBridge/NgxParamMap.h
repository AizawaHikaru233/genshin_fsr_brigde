// NgxParamMap.h —— NGX 参数映射的**纯计算层**（零 D3D / 零 NGX / 零全局状态）。
//
// ============================================================================
// 为什么单独切这一层
// ============================================================================
// 重构前，下面五处计算**物理上长在** `NgxBackend.cpp` / `Dx11FsrBridge.cpp` 的
// D3D 代码里：它们要么读 `g_config`（全局）、要么读 `ComPtr`、要么紧挨着
// `Map()/Unmap()` 与 `CreateBuffer`。后果是**任何一处公式都只能靠实机验证** ——
// 而"jitter 冻结""MV 符号"这类问题的症状（拖影 / 抖动 / 线条抖）在实机上极难二分定位。
//
// 本文件把这五处**逐字搬**成纯函数：输入是已经算好的标量，输出是标量。
// 调用方只负责"取输入 → 调函数 → 写输出"，**公式一个字符都不改**
// （改公式 = 改行为，不属本次重构范围）。
// 于是 `NgxParamMapTest.cpp` 可以在**无 GPU、无游戏、无 NGX 头**的机器上把公式钉住
// —— 与 `DlssInput.h`（build-t6verify/_ref-dlss-input，只读参考）的分层同构。
//
// ============================================================================
// 命名空间为什么是小写 snake_case
// ============================================================================
// 本仓库所有模块级命名空间都是小写：ffx12 / ngx / fsr2_family_takeover /
// transparent_jitter / il2cpp_callsite / fsr2dump / perf_probe / blog。
// 任务书里的 `NgxParamMap::` 是**示意写法**，按仓库既有风格落地为
// `ngx_param_map::`（与 `ffx12::depth_inverted()` 的调用形态一致）。
//
// ============================================================================
// 零依赖的硬约束
// ============================================================================
// 本文件**不** include d3d11.h / dxgi.h / nvsdk_ngx_*.h，不引用 ComPtr，
// 不读任何全局（g_config / g_hdr / 探测缓存）。CreateFlags 的位定义在这里自带一份
// 常量；`NgxBackend.cpp` 用 static_assert 把它们与 NGX 头的枚举**编译期对齐** ——
// 两边一旦漂移，构建直接失败，而不是运行期静默错位。
//
// 行尾：本文件与 .cpp / 测试均为 **LF**（与 `NgxBackend.cpp` 一致）。
#pragma once

#include <cstdint>

namespace ngx_param_map
{

// ===========================================================================
// a) CreateFlags 计算
// ===========================================================================
// 位定义逐字取自 third_party/ngx/nvsdk_ngx_defs.h 中 NVSDK_NGX_DLSS_Feature_Flags_*
// 的四个枚举子（IsHDR / MVLowRes / MVJittered / DepthInverted）。
// ★ 这里**复制**而不是 include 那个头：该头**已随仓库正式分发**（third_party/ngx/，
//   由上游"NGX 头随仓库分发"那一提交入库，是被跟踪的文件，**并非忽略项**），
//   但本层必须能在**裁剪了该目录的检出**下编译 —— 只取部分目录、或 CI 只 checkout
//   部分路径时都拿不到这个头；测试 target 正是按"零 NGX 头"建的（只 include 本头，
//   不 include nvsdk_ngx_*）。
//   复制带来的漂移风险由 NgxBackend.cpp 的 static_assert 在编译期拦住。
constexpr std::uint32_t kFlagIsHdr         = 1u << 0;   // NVSDK_NGX_DLSS_Feature_Flags_IsHDR
constexpr std::uint32_t kFlagMvLowRes      = 1u << 1;   // NVSDK_NGX_DLSS_Feature_Flags_MVLowRes
constexpr std::uint32_t kFlagMvJittered    = 1u << 2;   // NVSDK_NGX_DLSS_Feature_Flags_MVJittered
constexpr std::uint32_t kFlagDepthInverted = 1u << 3;   // NVSDK_NGX_DLSS_Feature_Flags_DepthInverted

// 与重构前 `NgxBackend.cpp` 的 `build_create_flags()`（§3.8.2）**逐字等价**的决策部分
// （不含日志、不含 M4-a 位图、不含 resolve_is_hdr 的全局探测）。
//
// 重构前原文（保留在此供逐位比对，★ 不改）：
//     ev.flags = 0;
//     if (ev.is_hdr)                              ev.flags |= IsHDR;        // 1<<0
//                                                 ev.flags |= MVLowRes;     // 1<<1，恒设
//     if (ffx12::motion_vectors_jittered())       ev.flags |= MVJittered;   // 1<<2，默认不设
//     if (ffx12::depth_inverted())                ev.flags |= DepthInverted;// 1<<3
//
// ★ MVLowRes **恒设**：重构前那一行是**无条件**语句（缩进成 if 块的样子，
//   但它前面没有 if）—— 这是本层最容易被"顺手整理"掉的一位，单测专门钉它。
struct CreateFlagsInput
{
    bool is_hdr;
    bool depth_inverted;
    bool mv_jittered;
    // 2026-11-13：MVLowRes 由"恒设"改为可配置（见 .cpp 的 map_create_flags 注释）。
    // 为什么必须能关：本游戏（原神）给的是**渲染分辨率**的 MV，但实测恒设
    // MVLowRes 会和 dlss-input 分支产生明显画质差异（后者只在 MV 非显示分辨率时置位）。
    // 由调用方按 Fsr2MotionVectorsLowRes 配置传入；默认 true 保持历史行为。
    bool mv_low_res;
};

struct CreateFlagsOutput
{
    std::uint32_t flags;
};

CreateFlagsOutput map_create_flags(const CreateFlagsInput &in);

// ===========================================================================
// b) Jitter 后处理（归一化 → 像素）
// ===========================================================================
// 与重构前 `Dx11FsrBridge.cpp` 里 `try_fsr2_translation_draw()` 的 jitter switch **逐字等价**。
// 模式语义（对应 ini 的 Ffx12JitterMode，默认 3）：
//   mode 0: +n·W − 0.5   （旧默认，A/B 保留；= switch 的 default 分支）
//   mode 1: +n·W         （无 −0.5）
//   mode 2: raw          （不乘 W；旧实现是 `case 2: break;`，见下）
//   mode 3: −n·W + 0.5   （默认；X/Y 翻转）
//   mode 4: −n·W         （游戏反汇编约定）
//   mode 5: 0            （诊断）
//   **其它任意值**（6、7、…、或 ini 里写错）落 default ⇒ 与 mode 0 同式。
//   重构前 default 分支兜住一切非 1..5 的值，本层保持同一行为（★ 不要改成
//   "非法值 → 0"：那是行为变更）。
//
// ★ mode 2 的精确语义（**最容易搞错的一处**）：
//   旧代码 `case 2: break;` **不写** jitter，于是 sdk_in.jitter_x 保留 switch
//   **之前**那次赋值（同一函数里的 `sdk_in.jitter_x = call_params.jitter_x`）。
//   本函数把 mode 2 定义为"原样返回传入的 norm_x"（不 delay、不缩放）——
//   与旧代码在"传入的 norm_x 就是 switch 前的值"时逐位相同。
//   生产调用点传的 norm_x 是 jit_src_x（可能与那次赋值的值不同，见 cb0 回退），
//   因此调用点对 mode 2 **不回写**，由那一行显式保证逐位等价。
struct JitterInput
{
    float norm_x;              // 归一化输入（switch 实际消费的那一个）
    float norm_y;
    std::uint32_t render_w;
    std::uint32_t render_h;
    std::uint32_t mode;        // 0..5；其它值按 0 处理（旧 default 分支）
    bool delay_one_frame;      // ↔ g_config.ffx12_jitter_delay
    float prev_norm_x;         // delay 模式用（↔ st->prev_jx）
    float prev_norm_y;         //          （↔ st->prev_jy）
};

struct JitterOutput
{
    float px;      // 像素抖动（送 NGX/FSR2 的原值，**不做 clamp**）
    float py;
    bool valid;    // 仅诊断：尺寸非 0 且输入有限。★ 调用方**不**据此改行为
};

JitterOutput map_jitter(const JitterInput &in);

// ===========================================================================
// c) MV scale 计算
// ===========================================================================
// 与重构前 `Dx11FsrBridge.cpp` 里 `try_fsr2_translation_draw()` 的 motion scale 段**逐字等价**：
//     const float mv_sign = positive ? 1.0f : -1.0f;
//     motion_scale_x = mv_sign * (float)render_w * ffx12_motion_scale;
// ★ 乘法结合顺序 (sign·W)·scale 必须保持（左结合），否则浮点末位可能不同。
struct MvScaleInput
{
    bool positive_scale;   // ↔ g_config.fsr2_positive_motion_vector_scale
    float motion_scale;    // ↔ g_config.ffx12_motion_scale（ini 侧已 clamp 到 [0,4]）
    std::uint32_t render_w;
    std::uint32_t render_h;
};

struct MvScaleOutput
{
    float x;
    float y;
};

MvScaleOutput map_mv_scale(const MvScaleInput &in);

// ===========================================================================
// d) Reset 判定
// ===========================================================================
// 与重构前 `NgxBackend.cpp` 里 `ngx::dispatch()` 第 10 步的 reset 判定**逐字等价**：
//     const int reset_val = (in.reset || inst.first) ? 1 : 0;
// 本函数只给布尔；"1/0" 的整数化留在调用点（保持原写法）。
struct ResetInput
{
    bool frame_reset;      // ↔ in.reset
    bool instance_first;   // ↔ inst.first
};

bool map_reset(const ResetInput &in);

// ===========================================================================
// e) Reactive 尺寸守卫
// ===========================================================================
// 与重构前 `NgxBackend.cpp` 里 `ngx::dispatch()` 第 10 步第 13 项（reactive mask）的尺寸守卫**逐字等价**：
//     if (g_use_reactive && inst.rw >= in.render_w && inst.rh >= in.render_h)
// （`g_use_reactive` 那一位留在调用点：它是配置开关，不是尺寸计算。）
// ★ 守卫恒真（inst.reactive 就是按 inst.rw x inst.rh 建的），保留它是为了与
//   FFX 侧口径一致 —— 重构不改这一点，只把比较搬成纯函数。
bool reactive_size_ok(std::uint32_t pool_w, std::uint32_t pool_h,
                      std::uint32_t render_w, std::uint32_t render_h);

// ===========================================================================
// f) 按 NGX 报告的合法 render 尺寸区间选档（2026-11-13 新增；**不是**搬运既有公式）
// ===========================================================================
// 为什么要有这一段：原来的选档（NgxBackend.cpp 的 quality_from_ratio）假设
// 「DLSS 档位 = 固定的 display/render 比例」（1.3 / 1.5 / 1.7 / 2.0 / 3.0）。
// 实际上每个档位能接受的 render 尺寸区间由驱动与模型决定，不是比例：同一档位在
// 不同 display 尺寸下的合法区间不同，相邻档位的区间还会重叠。已知事故：渲染精度
// 0.999（ratio≈1.0016）被比例表判成 UltraQuality ⇒ CreateFeature 返回 0xBAD00010
// （NVSDK_NGX_Result_FAIL_UnsupportedParameter，nvsdk_ngx_defs.h:169）。
// ⇒ 正确判据是「问驱动：这个档位接受哪些 render 尺寸」，即
//   NVSDK_NGX_DLSS_GET_OPTIMAL_SETTINGS 做的事（third_party/ngx/nvsdk_ngx_helpers.h:65）。
//
// 本段只做**纯计算**：真正的查询在 NgxBackend.cpp（需要 NGX 头），这里只判断
// 「给定各档区间 + 当前 render 尺寸，选哪一档」。于是它能在无 GPU、无游戏、无 NGX
// 头的机器上被 NgxParamMapTest.cpp 钉住 —— 与 a)~e) 五段的分层同构。
//
// ★ 零依赖约束照旧：本段不 include NGX 头。档位的整数值与
//   NVSDK_NGX_PerfQuality_Value 的一致性由 NgxBackend.cpp 的 static_assert 保证。

// 六个档位。**整数值必须与 NVSDK_NGX_PerfQuality_Value 完全一致**：
//   MaxPerf=0 / Balanced=1 / MaxQuality=2 / UltraPerformance=3 / UltraQuality=4 / DLAA=5
// （nvsdk_ngx_defs.h:251-260）。NgxBackend.cpp 用 static_assert 逐项钉死。
enum class PerfQuality : int
{
    MaxPerformance   = 0,   // NVSDK_NGX_PerfQuality_Value_MaxPerf
    Balanced         = 1,
    MaxQuality       = 2,
    UltraPerformance = 3,
    UltraQuality     = 4,
    Dlaa             = 5,   // NVSDK_NGX_PerfQuality_Value_DLAA（ratio = 1，不放大）
};

constexpr int kPerfQualityCount = 6;

// 档位名（日志用）。越界返回 "?"，不返回 nullptr —— 调用方直接交给 %hs。
const char *perf_quality_name(int index);

// 一次区间查询的结果（= NGX_DLSS_GET_OPTIMAL_SETTINGS 的六项输出里的五项；
// Sharpness 与选档无关，留在查询侧作取证，不进本层）。
struct RenderRange
{
    std::uint32_t opt_w = 0;   // DLSS 建议的该档 optimal render 宽（NVSDK_NGX_Parameter_OutWidth）
    std::uint32_t opt_h = 0;   // 同上（OutHeight）
    std::uint32_t min_w = 0;   // DLSS.Get.Dynamic.Min.Render.Width
    std::uint32_t min_h = 0;
    std::uint32_t max_w = 0;   // DLSS.Get.Dynamic.Max.Render.Width
    std::uint32_t max_h = 0;
};

// 单个档位的查询态。ok=false 表示查询失败（回调缺失 / 返回非 Success / 区间退化），
// **绝不能**把失败当成"空区间"或"不限制"。
struct RangeQuery
{
    bool        ok = false;
    RenderRange range {};
};

// 选档结果。matched=false ⇒ 没有任何档位的区间包含当前 render 尺寸，
// 调用方必须回退到既有的比例表（quality_from_ratio + perf_quality_for_create）。
struct QualityPick
{
    bool          matched = false;
    int           index   = -1;   // 命中档位（= static_cast<int>(PerfQuality)）
    std::uint32_t tol_w   = 0;    // 命中档位在该轴上的上界容差（日志/验收用）
    std::uint32_t tol_h   = 0;
};

// 上界容差：每轴允许超出区间上限 upper/64（OPEN-ISSUES §4「最大尺寸的 1/64」）。
// ★ 与原文的出入（如实记录）：原文写「1/64（约 2 像素）」，但按本公式 upper=1280 时
//   是 20 像素、upper=2560 时是 40 像素 ——「约 2 像素」在任何常见分辨率下都不成立，
//   应是原文的估算。这里以**公式**为准（1/64），并把 tol 打进日志，实机可对照真实区校核。
std::uint32_t range_upper_tolerance(std::uint32_t upper);

// 该档的合法 render 尺寸区间是否包含 (render_w, render_h)。
// 下界**无**容差（render 比 min 还小 = 这一档的倍率不够，正是要消除的错配形态）；
// 上界允许 +range_upper_tolerance(max)。任一维为 0 或 min>max ⇒ 判为不包含（区间不可用）。
bool render_range_contains(const RenderRange &r, std::uint32_t render_w, std::uint32_t render_h);

// 在六档里挑区间包含当前 render 尺寸的那一档。
//   · ranges[i] 必须对应 PerfQuality(i)（i 即档位整数值），count 不得超过 6；
//   · 多个档位的区间同时包含时，取 **optimal 尺寸离实际 render 尺寸最近** 的那档
//     （区间重叠是常态，最近者是"最贴合的档"）；
//   · 距离完全相等时才看固定优先序（MaxQuality → Balanced → MaxPerf →
//     UltraPerformance → UltraQuality → DLAA），因此结果确定、可复现。
QualityPick pick_quality_from_ranges(const RangeQuery *ranges, int count,
                                     std::uint32_t render_w, std::uint32_t render_h);

// ===========================================================================
// g) CreateFeature 输入 key 的相等判据（2026-11-13 新增；§5 自愈路径的唯一比较点）
// ===========================================================================
// 为什么把这条比较搬到纯函数层：它是"要不要重试 CreateFeature"的**唯一**判据。
//   留在 NgxBackend.cpp 里时**只能靠读代码**保证"字段加齐了"，而漏字段不报编译错 ——
//   那种失效形态是"自愈永远不触发"（key 恒相同）或"每帧都触发"（某字段每帧变），
//   两者都不会崩、都不会报错，只有实机日志才看得出来。搬到这里后 NgxParamMapTest 能钉住它。
//
// ★ 判据是**逐字段相等**，不是哈希：碰撞会把"不同的输入"判成"相同" ⇒ 该重试的没重试，
//   且这种漏在日志里看不出来（AGENTS.md §三 #21 的形态：检查通过 ≠ 内容正确）。
//
// ★ 字段清单 = CreateFeature 真正 Set 进 NGX 的全部可变输入（见 NgxBackend.cpp 的
//   create_feature 第 1-7 项）。新增一个 Set 项时必须同时加字段 + 加比较，
//   否则"改了参数却判不出变化"。
//
// ⚠️ 2026-11-13 修复（独立审查第 5 条）：**原实现漏了 color_fmt / output_fmt**。
//   反例（审查给出，已核实成立）：后缓冲由 R10G10B10A2 换成 R16G16B16A16_FLOAT，
//   而 HDR 决策不变（两者在 resolve_is_hdr 的回退层都不是"浮点 HDR"以外的分支…
//   实际更关键的是 R10G10B10A2→R16G16B16A16 会改变 IsHDR 位的来源判定，
//   但**存在**该位不变的组合）⇒ create_flags 不变 ⇒ 旧 key 判"相同" ⇒ **该重试的没重试**。
//   格式是 CreateFeature 的输入吗？是 —— 它决定 IsHDR 位的取值来源，且资源池按它建；
//   dispatch 第 5 步的**重建判据**里本来就有 cdesc.Format / odesc.Format 两项
//   （见 NgxBackend.cpp "六元组比较"），key 判据漏掉它们属同族漏项。
struct CreateFeatureInputKey
{
    std::uint32_t render_w       = 0;
    std::uint32_t render_h       = 0;
    std::uint32_t display_w      = 0;
    std::uint32_t display_h      = 0;
    int           perf_quality   = 0;
    int           render_preset  = 0;
    unsigned int  create_flags   = 0;
    int           color_fmt      = 0;   // DXGI_FORMAT（本层零 D3D 依赖 ⇒ 存 int，调用点转）
    int           output_fmt     = 0;
    bool          is_hdr         = false;
};

// 逐字段相等。★ **唯一实现点就是本函数**（`NgxParamMap.cpp`）：NgxBackend.cpp 既不自己
// 抄一份比较，也**不写**同名中转包装 —— 抄一份 = 两个维护点 = 下次加字段必漏一个；
// 而写包装会因 ADL 引发二义（`CreateFeatureInputKey` 是 ngx_param_map 的类型 ⇒ ADL 会把
// 本函数一起拉进候选集，与本文件外的同名包装签名完全一致 ⇒ C2668，实测记录见
// `NgxBackend.cpp` 的 `kFeatureCreateRetryMinMs` 下方注释）。
// ⇒ 调用点一律写**全限定**的 `ngx_param_map::same_feature_input_key(...)`（当前唯一调用点：
//   `NgxBackend.cpp` 的 dispatch 重试判据）。
// 本注释与代码的一致性由"全仓 grep 该函数名"核对：除本文件与本单测外，只应出现
// NgxBackend.cpp 的**限定的**调用点与解释性注释，不应出现第二个定义或同名包装。
bool same_feature_input_key(const CreateFeatureInputKey &a, const CreateFeatureInputKey &b);

} // namespace ngx_param_map
