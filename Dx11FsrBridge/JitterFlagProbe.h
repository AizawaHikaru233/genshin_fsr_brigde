#pragma once
// JitterFlagProbe.h — 观测（并**可选地强制**）Unity
// `Camera.useJitteredProjectionMatrixForTransparentRendering` 的 setter 实参。
//
// 两种模式：
//   JitterFlagProbe=1, JitterFlagForce=0（默认）—— **只读观测**，一个字节都不改游戏行为；
//   JitterFlagProbe=1, JitterFlagForce=1           —— **受控 A/B 修复尝试**：把传给该 setter
//     的实参从 `false` 强制成 `true`（**只改这一个实参**，其余一切照旧）。
//
// 目的（单一、明确）：**证伪或坐实**一条已定的机制假设。
//
// 假设：Unity PostProcessing v2 的时域 AA 在 `ConfigureJitteredProjectionMatrix` 里
//   camera.nonJitteredProjectionMatrix = camera.projectionMatrix;
//   camera.projectionMatrix           = GetJitteredProjectionMatrix(camera);
//   camera.useJitteredProjectionMatrixForTransparentRendering = false;   // ★
// ⇒ 透明队列的渲染器用**未抖动**投影矩阵光栅化 ⇒ 屏幕空间采样逐帧完全相同
// ⇒ 时间累积拿不到新信息 ⇒ 角色附属效果部件（翅膀水晶羽片 / 月环）出现硬阶梯。
//
// 为什么必须做运行时观测：两份 IL2CPP dump 里**只有 `{ }` + RVA + MethodSize**，
// 没有方法体 ⇒ "游戏确实传了 false" 无法静态证实（dump 侧只能证明
// "setter 可达、getter 不可达" ⇒ 只写不读，与 Unity 那条实现同形）。
//
// ⚠️ 本模块的职责边界（force 打开时也一样）：
//   - stub 保存 rcx/rdx → 调 observer（**observer 看到的是原始 rdx**）→
//     rcx 原样恢复、rdx 用 **observer 的返回值**（force=0 时该返回值 == 原始 rdx，
//     逐位相同；force=1 时才变成 1）→ 重放原序言 → 跳回原函数；
//   - **不写任何游戏内存**（除了那块 setter 序言的 jmp 补丁本身），
//     也不解引用 Camera 指针（只把指针当数字打出来）；
//   - 关闭时（enabled=false）`install` 直接返回，**一个字节都不改**。
//
// 为什么把"改不改"放在 C++ 侧（observer 的返回值）而不是 stub 里内联一个全局标志的分支：
//   ① 热键要能在**同一次会话内随时翻转** ⇒ 判断必须每次调用都重新读（原子变量），
//      内联分支就要在 stub 里嵌全局地址 ⇒ 又多一处"绝对地址写死"；
//   ② 转发值由 observer 返回值给出 ⇒ "原值"与"实际传出的值"天然出自同一处，
//      日志不可能与行为不一致（否则会出现"日志说 forced=true 而实际没改"这类鬼故事）；
//   ③ 可以在单测里**端到端**验证（假 setter 体收到的 dl 就是它）。


//
// 定位方式（**不写死 RVA** —— B88 已证国服/国际服是两份不同构建，写死地址是客户端专用的）：
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

namespace jitter_flag_probe
{

// 心跳间隔（**调用次数**）。值长期不变时也要周期性打一行 —— 否则
// "没观察到 false" 与 "钩子根本没装上" **无法区分**（本项目经典教训）。
constexpr std::uint32_t k_heartbeat_calls = 300;

// 锚点函数体内的扫描窗口。取 0x1000：大于实测方法体（0x3E0）两倍以上，
// 又小于"邻居函数里也有 bool setter"的临界点（本机实测窗口放到 0x10000 会多出 3 个候选）。
constexpr std::uint32_t k_scan_window = 0x1000;

struct Config
{
    bool enabled = false;           // JitterFlagProbe（默认 0）
    bool force = false;             // JitterFlagForce（默认 0）：1 = 把实参强制成 true
    std::uint32_t frames_limit = 0; // JitterFlagProbeFrames：0 = 一直记录；N = 只记录前 N 帧
    std::uint32_t camera_rva = 0;   // 锚点 RVA（特征识别优先，配置兜底）
    std::uint64_t image_size = 0;   // 模块 SizeOfImage（候选目标越界检查用）
};

// 日志出口（由调用方注入；为空则只计数不输出）。line 以 '\0' 结尾。
using LogSink = void (*)(const char *line);
void set_log_sink(LogSink sink);

// 帧号来源（由调用方注入；为空则退回本模块自己的调用计数）。
using FrameProvider = std::uint64_t (*)();
void set_frame_provider(FrameProvider provider);

// 安装探针（force=0 时纯只读）。失败时**原代码未改动**，原因写入 out_reason。
// 取值：`disabled` / `bad_rva` / `bad_image_size` / `bad_scan_window` /
//       `camera_prologue_mismatch` / `flag_setter_not_found` / `flag_setter_ambiguous` /
//       `flag_setter_prologue_mismatch` / `protect_failed` / `alloc_failed`
bool install(std::uint64_t exe_base, const Config &cfg, const char **out_reason);

// 还原原字节（DLL_PROCESS_DETACH 调用）。不取任何锁（loader lock 安全）。
// 只在字节**仍是我们的 jmp** 时还原；stub 刻意不释放（详见实现处注释）。
// 同时把 force 状态复位为 false（还原后 stub 已不可达，留着会误导下一个读者）。
void shutdown();

bool active();
std::uint64_t observed_count();

// ---- 【强制 true】开关（JitterFlagForce / 热键）----
//
// 语义：**只改传给那个 setter 的实参**——force 打开时转发 `rdx = 1`，
// 其余寄存器（含 rcx = Camera*）逐位不变。observer 记录到的 `value=`
// **永远是原始值**，日志里另外用 `forced=` 给出我们实际传出去的值。
//
// 切换**不需要重装钩子**：stub 每次都读原子状态，因此可以在同一次游戏会话内
// 随时 A/B（这就是热键的全部意义 —— 否则每次都要重启游戏）。
// 返回 true 表示状态确实变了（并已打一行 `jitter_flag_force toggled ...`）。
// source / frame 只用于日志（如 "hotkey" / 帧号）。
bool set_force(bool on, const char *source, std::uint64_t frame);
bool force_enabled();

// 最近一次观测的原始值 / 实际转发值（尚未观测到则返回 false）。
bool last_forwarding(std::uint8_t *out_original, std::uint8_t *out_forwarded);

// 判定用计数：实际**改写**了实参的调用次数（force=1 时每次调用都会 +1）。
// 用于回答"force=1 到底有没有真的生效"——0 表示一个字节都没改。
std::uint64_t overridden_count();

// 最近一次观测（值 / 相机指针 / 帧号）。返回 false 表示尚未观测到。
bool last_observation(std::uint8_t *out_value, std::uint64_t *out_camera, std::uint64_t *out_frame);

// 判定用计数：是否真的见过 false / true（与日志限流无关，始终统计）。
bool saw_false();
bool saw_true();

// ---- 以下为可离线单测的纯函数（无 Windows 依赖）----

// 日志限流状态机。语义：
//   - 首次观测**必打**（证明钩子活着 —— 否则"没观察到 false"与"钩子没装上"无法区分）；
//   - 值变化**必打**（这就是决定性证据；任务允许"值变化时记录"）；
//   - 值不变时每 heartbeat_interval **次调用**打一次（心跳）。用**调用计数**而不是
//     帧号：帧号来源缺失/冻结时心跳会永远不打，而"钩子还活着"只能靠心跳证明。
//   - frames_limit != 0 时，frame > frames_limit 一律不打。
//
// ⚠️ "值变化"看**两个**维度：`value`（运行时原值）与 `forwarded`（我们实际传出去的值）。
// 只看原值的话，按热键翻转 force 之后，原值仍是恒定的 false ⇒ 要等最多 300 次调用
// 才有下一行心跳，日志里会**延迟约 5 秒**才出现 `forced=true`；而"force 到底生效了没"
// 恰恰是 A/B 最需要**立刻**看到的那一项。所以任意一维变化都立刻打一行。
//
// "绝不刷屏"由心跳间隔保证：setter 每帧约一次 ⇒ 每 300 次调用一行 ≈ 每 300 帧一行，
// 比"每帧最多一行"严格得多；值变化是**一次性**事件（false→true 之后不再变，
// force 翻转也是**按键时**才发生一次），因此即使同帧多相机也不会持续输出。
struct EmitState
{
    bool seen_any = false;
    std::uint8_t last_value = 0;     // 上一次的运行时原值（低 8 位）
    std::uint8_t last_forwarded = 0; // 上一次实际传给 setter 的值（低 8 位）
    std::uint64_t next_heartbeat_call = 0;
};

struct EmitDecision
{
    bool emit = false;
    const char *reason = ""; // "first" / "change" / "heartbeat" / ""
};

EmitDecision decide_emit(EmitState &state, std::uint8_t value, std::uint8_t forwarded, std::uint64_t frame,
                         std::uint64_t call_index, std::uint32_t frames_limit,
                         std::uint32_t heartbeat_interval = k_heartbeat_calls);

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

} // namespace jitter_flag_probe
