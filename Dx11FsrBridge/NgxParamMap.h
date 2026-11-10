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

} // namespace ngx_param_map
