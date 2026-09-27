#pragma once
// TransparentJitterHook.h — **正式功能**：强制 Unity
// `Camera.useJitteredProjectionMatrixForTransparentRendering = true`，
// 让透明队列物件参与超分的时间重建。
//
// 【它修什么】
//   Unity PostProcessing v2 的 `ConfigureJitteredProjectionMatrix` 里是：
//       camera.nonJitteredProjectionMatrix = camera.projectionMatrix;
//       camera.projectionMatrix           = GetJitteredProjectionMatrix(camera);
//       camera.useJitteredProjectionMatrixForTransparentRendering = false;   // ★
//   ⇒ 透明队列的渲染器用**未抖动**投影矩阵光栅化 ⇒ 屏幕空间采样相位逐帧完全相同
//   ⇒ 时间累积拿不到新信息 ⇒ 角色附属效果部件（翅膀水晶羽片 / 月环）边缘出现硬阶梯。
//   本模块把传给那个 setter 的实参从 `false` 强制成 `true` —— **只改这一个实参**。
//
// 【三大独立坐实（不要再重复验证）】
//   ① 静态反汇编：游戏确实传 false（`xor edx,edx`，国服 + 国际服两份 exe）；
//   ② IL2CPP dump：该属性只有 setter、无 getter；
//   ③ 运行时观测：`value=false` × 16601 次；强制后 `forced=true` × 181 次、`forced=false` × 0。
//   实机验证：用户确认「翅膀变好了，其他地方没有发现明显劣化」。
//
// 【命名与语义（2026-09-27 正式版收尾）】
//   本模块原名 `JitterFlagProbe`（"只读观测探针"），但它承担的其实是**修复**：
//   用户为了关掉诊断而写 `JitterFlagProbe=0` 时，修复会**静默失效**。
//   ⇒ 现在拆成两层，互不牵连：
//       ini 键 `TransparentJitter`（默认 **1** = 修复生效；0 = 回到原行为）
//       ⇒ `Config.hook`（是否安装钩子）与 `Config.force`（是否改写真参）
//   只有测试/诊断才需要"装上钩子但一个字节都不改"（hook=1, force=0）；
//   生产路径永远是 (hook=1, force=1)，**没有任何诊断键能让修复失效**。
//
// ⚠️ 职责边界（force=true 时也一样）：
//   - stub 保存 rcx/rdx → 调 observer（**observer 看到的是原始 rdx**）→
//     rcx 原样恢复、rdx 用 **observer 的返回值**（force=0 时该返回值 == 原始 rdx，
//     逐位相同；force=1 时才变成 1）→ 重放原序言 → 跳回原函数；
//   - **不写任何游戏内存**（除了那块 setter 序言的 jmp 补丁本身），
//     也不解引用 Camera 指针（只把指针当数字记下来）；
//   - 关闭时（hook=false）`install` 直接返回，**一个字节都不改**。
//
// ⚠️ 为什么把"改不改"放在 C++ 侧（observer 的返回值）而不是 stub 里内联一个标志分支：
//   ① 转发值由 observer 返回值给出 ⇒ "原值"与"实际传出的值"天然出自同一处，
//      日志/计数不可能与行为不一致；
//   ② 可以在单测里**端到端**验证（假 setter 体收到的 dl 就是它）。
//
// 【日志纪律（正式版）】
//   只在两处输出：
//     - 安装成功：一行 `transparent_jitter_installed ...`（含定位来源 / setter RVA /
//       调用点静态常量 / 是否强制）—— 这是"钩子挂对了地方"的证据行；
//     - shutdown：一行 `transparent_jitter_stats ...`（本会话调用次数 / 实际改写次数 /
//       是否见过 false/true）—— 一次性回答"本次修复到底有没有落地"。
//   失败原因的 `reason` 由调用方记录（见 install 的 out_reason）。
//   **刻意没有任何每帧 / 首次观测 / 心跳日志** —— 那三样是调查期临时产物，
//   在正式版里既刷屏又没有新信息（观测计数仍可用下面的 API 读到）。
//
// 定位方式（**不写死 RVA** —— 国服/国际服是两份不同构建，写死地址是客户端专用的）：
//   ① 锚点 = `FFX_FSR2.ConfigureJitteredProjectionMatrix`，由既有的
//      `il2cpp_callsite::detect_ffx12_method_rvas()`（g_MethodPointers + 30 方法尺寸
//      序列 gap 匹配）给出；装钩前校验其 14 字节序言。
//   ② 在锚点函数体内扫 `call rel32`，按**被调用者的序言形状**分类：
//      - 16 字节 == Matrix4x4 setter（`Camera.set_projectionMatrix`）；
//      - 19 字节 == **bool setter**（`movzx edi,dl` + `mov rbx,rcx` + `test rcx,rcx`）。
//      IL2CPP 给「(对象, bool)」生成的包装器就是这 19 字节形状 —— 这是**语义签名**，
//      不是地址。
//   ③ 唯一性：bool 形状候选恰 1 个 → 采用；多个 → 只保留「紧跟 Matrix4x4 setter 调用
//      之后（≤24 字节）且实参是编译期常量 bool」的那个；仍不唯一 → **如实失败**，不猜。
//
// 依赖约定：本 TU **刻意不依赖 BridgeLogger**（单测只编译本 cpp + Windows API）：
// 日志通过 `set_log_sink` 注入，帧号通过 `set_frame_provider` 注入。

#include <cstddef>
#include <cstdint>

namespace transparent_jitter
{

// 锚点函数体内的扫描窗口。取 0x1000：大于实测方法体（0x3E0）两倍以上，
// 又小于"邻居函数里也有 bool setter"的临界点（本机实测窗口放到 0x10000 会多出 3 个候选）。
constexpr std::uint32_t k_scan_window = 0x1000;

struct Config
{
    // 是否安装钩子。`TransparentJitter=1` ⇒ true。
    bool hook = false;
    // 是否把实参强制成 true —— **这就是修复本体**。
    //   (hook=1, force=1) = 修复；（hook=1, force=0）= 纯观测（一个字节都不改，仅测试/诊断用）；
    //   (hook=0, *)       = 完全不碰游戏。
    bool force = false;
    std::uint32_t camera_rva = 0;   // 锚点 RVA（特征识别优先，配置兜底）
    std::uint64_t image_size = 0;   // 模块 SizeOfImage（候选目标越界检查用）
    // 锚点来源，仅用于安装行日志：调用方传 "feature"（特征识别命中）/ "config"（配置兜底）。
    // 必须是静态存储期字符串（钩子不复制它）。
    const char *anchor_source = "config";
};

// 日志出口（由调用方注入；为空则只计数不输出）。line 以 '\0' 结尾。
using LogSink = void (*)(const char *line);
void set_log_sink(LogSink sink);

// 帧号来源（由调用方注入；为空则退回本模块自己的调用计数）。
using FrameProvider = std::uint64_t (*)();
void set_frame_provider(FrameProvider provider);

// 安装钩子（force=false 时纯只读）。失败时**原代码未改动**，原因写入 out_reason。
// 取值：`disabled` / `bad_rva` / `bad_image_size` / `bad_scan_window` /
//       `camera_prologue_mismatch` / `flag_setter_not_found` / `flag_setter_ambiguous` /
//       `flag_setter_prologue_mismatch` / `protect_failed` / `alloc_failed`
bool install(std::uint64_t exe_base, const Config &cfg, const char **out_reason);

// 还原原字节（DLL_PROCESS_DETACH 调用）。不取任何锁（loader lock 安全）。
// 只在字节**仍是我们的 jmp** 时还原；stub 刻意不释放（详见实现处注释）。
// 同时把 force 状态复位为 false（还原后 stub 已不可达，留着会误导下一个读者）。
// 会输出一行 `transparent_jitter_stats ...`（本会话统计）。
void shutdown();

bool active();
std::uint64_t observed_count();

// ---- force（修复本体）状态 ----
//
// force 只在 `install` 时由 `Config.force` 设定、在 `shutdown` 时复位。
// ⚠️ 热键已在正式版移除：`JitterFlagHotkey` 在游戏里被游戏吃掉（用户实测按 F3 无反应），
// 而 A/B 也已经在调查期做完 ⇒ 留一个"看起来能用、实际按不动"的热键只会误导用户。
// 观测计数仍保留，便于将来用日志/调试器回答"修复有没有真的落地"。
bool force_enabled();

// 最近一次观测的原始值 / 实际转发值（尚未观测到则返回 false）。
bool last_forwarding(std::uint8_t *out_original, std::uint8_t *out_forwarded);

// 判定用计数：**语义**改写次数 —— force=1 且原值确实是 false 时 +1。
// （原值本来就是 true 时不计：rdx 高位虽可能被规整，但那个 setter 只读 dl，语义未变。）
// 用于回答"force=1 到底有没有真的生效"——0 表示一次都没把 false 改成 true。
std::uint64_t overridden_count();

// 最近一次观测（值 / 相机指针 / 帧号）。返回 false 表示尚未观测到。
bool last_observation(std::uint8_t *out_value, std::uint64_t *out_camera, std::uint64_t *out_frame);

// 判定用计数：是否真的见过 false / true（与日志无关，始终统计）。
bool saw_false();
bool saw_true();

// ---- 以下为可离线单测的纯函数（无 Windows 依赖）----

// 转发值决策（纯函数，**这是 force 的全部语义**）：
//   force == false → 原样返回 original_value_raw（**逐位相同**，连 rdx 高位垃圾一起保留
//                    —— bool 参数在 Win64 下只有 dl 有意义，原函数的 `movzx edi,dl`
//                    也只读 dl，所以"原样透传"是最忠实的"零改动"）；
//   force == true  → 返回 1（即 `rdx = 1`，对应托管层 `= true`）。
std::uint64_t decide_forwarded_value(bool force, std::uint64_t original_value_raw);

// 定位：在锚点函数体内发现 bool setter。
//   image       —— 模块基址（测试时可以是任意可读缓冲）
//   image_size  —— 可读范围（真实模块传 SizeOfImage）
//   camera_rva  —— 锚点 RVA（相对 image）
//   scan_window —— 锚点内扫描窗口（真实用法见 k_scan_window）
//   out_setter_rva / out_callsite_imm —— 命中结果；imm 为 -1 表示调用点实参
//                 不是可识别的编译期常量（**不作为失败条件**）
// 成功返回 true；失败返回 false 并写 out_reason（"flag_setter_not_found" /
// "flag_setter_ambiguous" 等）。
bool locate_flag_setter(const std::uint8_t *image, std::uint64_t image_size, std::uint32_t camera_rva,
                        std::uint32_t scan_window, std::uint32_t *out_setter_rva,
                        int *out_callsite_imm, const char **out_reason);

} // namespace transparent_jitter
