#pragma once
// JitterFlagProbe.h — 【只读观测】Unity `Camera.useJitteredProjectionMatrixForTransparentRendering`
// 的 setter 实参（诊断，默认关闭）。
//
// 目的（单一、明确）：**证伪或坐实**一条已定的机制假设，而不是修任何东西。
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
// ⚠️ 本模块的**唯一职责是记录**：
//   - stub 保存 rcx/rdx → 调 observer → **原样恢复 rcx/rdx** → 重放原序言 → 跳回原函数；
//   - 不写任何游戏内存，也不解引用 Camera 指针（只把指针当数字打出来）；
//   - 关闭时（enabled=false）`install` 直接返回，**一个字节都不改**。
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

// 安装只读探针。失败时**原代码未改动**，原因写入 out_reason。
// 取值：`disabled` / `bad_rva` / `bad_image_size` / `bad_scan_window` /
//       `camera_prologue_mismatch` / `flag_setter_not_found` / `flag_setter_ambiguous` /
//       `flag_setter_prologue_mismatch` / `protect_failed` / `alloc_failed`
bool install(std::uint64_t exe_base, const Config &cfg, const char **out_reason);

// 还原原字节（DLL_PROCESS_DETACH 调用）。不取任何锁（loader lock 安全）。
// 只在字节**仍是我们的 jmp** 时还原；stub 刻意不释放（详见实现处注释）。
void shutdown();

bool active();
std::uint64_t observed_count();

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
// "绝不刷屏"由心跳间隔保证：setter 每帧约一次 ⇒ 每 300 次调用一行 ≈ 每 300 帧一行，
// 比"每帧最多一行"严格得多；值变化是**一次性**事件（false→true 之后不再变），
// 因此即使同帧多相机也不会持续输出。
struct EmitState
{
    bool seen_any = false;
    std::uint8_t last_value = 0;
    std::uint64_t next_heartbeat_call = 0;
};

struct EmitDecision
{
    bool emit = false;
    const char *reason = ""; // "first" / "change" / "heartbeat" / ""
};

EmitDecision decide_emit(EmitState &state, std::uint8_t value, std::uint64_t frame,
                         std::uint64_t call_index, std::uint32_t frames_limit,
                         std::uint32_t heartbeat_interval = k_heartbeat_calls);

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
