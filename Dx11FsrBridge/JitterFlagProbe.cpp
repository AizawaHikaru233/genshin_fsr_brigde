#include "JitterFlagProbe.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace jitter_flag_probe
{
namespace
{

// 16 字节绝对跳转（`48 B8 <stub> FF E0`）+ 0x90 填充。
// 与 Il2CppCallSiteHook.cpp 的 build_jmp_patch 同形 —— install 与 shutdown 的
// "校验后还原"必须共用同一构造，否则 shutdown 永远判定不匹配 → 静默不还原。
constexpr std::size_t k_patch_len = 16;
constexpr std::size_t k_stub_len = 96;

// ---- 锚点：FFX_FSR2.ConfigureJitteredProjectionMatrix 的序言（14 字节）----
// push r15; push r14; push rsi; push rdi; push rbx; sub rsp,0x140
// 与 Il2CppCallSiteHook.cpp 的 k_camera_head 相同（那边用它装相机钩子）。
constexpr std::size_t k_camera_head_len = 14;
constexpr std::uint8_t k_camera_head[k_camera_head_len] = {
    0x41, 0x57, 0x41, 0x56, 0x56, 0x57, 0x53,
    0x48, 0x81, 0xEC, 0x40, 0x01, 0x00, 0x00,
};

// ---- 调用点分类签名 ----
// 两者共享前 10 字节（mov [rsp+8],rbx; push rdi; sub rsp,0x20），第 10 字节起分叉：
//   Matrix4x4 setter：48 8B FA  = mov rdi,rdx   （实参是矩阵指针）
//   bool     setter：0F B6 FA  = movzx edi,dl   （实参是 bool，★ 要找的就是它）
constexpr std::size_t k_matrix_head_len = 16;
constexpr std::uint8_t k_matrix_setter_head[k_matrix_head_len] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
    0xEC, 0x20, 0x48, 0x8B, 0xFA, 0x48, 0x8B, 0xD9,
};
// 19 字节 = 16 字节 + `48 85 C9`（test rcx,rcx，IL2CPP 对托管 this 的判空）。
// 刻意不把紧随其后的 `je` 纳入签名：短跳/长跳编码可能变（74 xx vs 0F 84 xxxxxxxx），
// 而前 19 字节已经是"取 bool 实参 + 取 this + 判空"这一**语义形状**，足够唯一。
constexpr std::size_t k_bool_head_len = 19;
constexpr std::uint8_t k_bool_setter_head[k_bool_head_len] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20,
    0x0F, 0xB6, 0xFA, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9,
};

// 被 patch 的 bool setter 序言（16 字节 = 完整指令边界）：
//   mov [rsp+8],rbx (5) / push rdi (1) / sub rsp,0x20 (4) / movzx edi,dl (3) / mov rbx,rcx (3)
// 必须是**指令边界**：12 字节会切断 `movzx edi,dl`（跨指令覆盖 = 崩溃）。
static_assert(k_bool_head_len >= k_patch_len, "bool setter 签名必须覆盖被 patch 的 16 字节");

// 日志出口与帧号来源（由调用方注入；默认空 → 只计数）
std::atomic<LogSink> g_log_sink { nullptr };
std::atomic<FrameProvider> g_frame_provider { nullptr };

// 安装状态
std::uint8_t *g_target = nullptr;
std::uint8_t g_saved[k_patch_len] {};
std::uint8_t *g_stub = nullptr;
std::atomic_bool g_active { false };
std::uint32_t g_frames_limit = 0; // JitterFlagProbeFrames（0 = 不限制）

// 观测计数（与日志限流无关，始终统计 —— 这样即使日志被限流，
// 也能用"是否见过 false"回答机制问题）
std::atomic_uint64_t g_calls { 0 };
std::atomic_bool g_saw_false { false };
std::atomic_bool g_saw_true { false };
std::atomic<std::uint8_t> g_last_value { 0 };
std::atomic_uint64_t g_last_camera { 0 };
std::atomic_uint64_t g_last_frame { 0 };

// ---- 【强制 true】状态（JitterFlagForce / 热键）----
// stub 不内联读它，而是读 observer 的返回值 ⇒ 这里每次调用都重新 load，
// 因此热键翻转**下一次 setter 调用就生效**（不需要重装钩子）。
std::atomic<std::uint8_t> g_force { 0 };
std::atomic_uint64_t g_overridden { 0 };      // 实际改写了实参的调用次数
std::atomic<std::uint8_t> g_last_forwarded { 0 };
// 调用点静态常量实参（安装时由签名发现给出）。放进观测行里，
// 好让 A/B 时**一行**就能同时看到：静态原值 / 运行时原值 / 我们传出的值。
std::atomic<int> g_callsite_imm { -1 };

// 日志限流状态。
// ⚠️ 刻意**不加锁**：写方只有游戏渲染线程（OnPreCull → 每帧一次），
// shutdown() 不碰这些字段。多相机并发时最坏结果是多一行/少一行心跳，对诊断无影响；
// 而加锁会让本模块在 DLL_PROCESS_DETACH 里变得不 loader-lock 安全。
// ⚠️ set_force()（热键线程）也**不**碰它 —— 翻转后"下一行日志"由 decide_emit 的
// `forwarded != last_forwarded` 判据自然带出（见头文件说明）。
EmitState g_emit_state;

void write_u64(std::uint8_t *dst, std::uint64_t value)
{
    std::memcpy(dst, &value, 8);
}

void build_jmp_patch(std::uint8_t *patch, std::size_t len, const std::uint8_t *stub)
{
    std::memset(patch, 0x90, len);
    patch[0] = 0x48;
    patch[1] = 0xB8;
    write_u64(patch + 2, reinterpret_cast<std::uint64_t>(stub));
    patch[10] = 0xFF;
    patch[11] = 0xE0;
}

// SEH 保护的序言比较：候选目标由 rel32 算出，虽然已做区间检查，
// 但"读到未提交页"仍可能发生 —— 一次读取失败不该让安装崩掉游戏。
// 本函数刻意不含任何需要栈展开的对象（MSVC 不允许 __try 与 C++ 展开共存）。
bool safe_head_match(const std::uint8_t *p, const std::uint8_t *expected, std::size_t n)
{
    __try
    {
        return std::memcmp(p, expected, n) == 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// 解析调用点紧邻前缀里的常量 bool 实参（仅用于**报告**与并列候选的裁决，
// 不作为发现的前提 —— 否则"实参不是编译期常量"的构建会直接装不上）。
//   31 D2 / 33 D2 = xor edx,edx   → 0
//   B2 imm8       = mov dl,imm8   → imm8
//   BA imm32      = mov edx,imm32 → imm32
// 返回 -1 表示前缀不是可识别的常量写。
int parse_callsite_imm(const std::uint8_t *code, std::size_t call_offset)
{
    if (call_offset >= 2 && (code[call_offset - 2] == 0x31 || code[call_offset - 2] == 0x33) &&
        code[call_offset - 1] == 0xD2)
        return 0;
    if (call_offset >= 2 && code[call_offset - 2] == 0xB2)
        return static_cast<int>(code[call_offset - 1]);
    if (call_offset >= 5 && code[call_offset - 5] == 0xBA)
    {
        std::int32_t imm = 0;
        std::memcpy(&imm, code + call_offset - 4, 4);
        return static_cast<int>(imm);
    }
    return -1;
}

// 由 stub 调用（rcx = Camera*，rdx = 实参原始值）。**只读**：不解引用 camera 指针。
//
// ⚠️ 返回值 = **stub 要转发给原 setter 的 rdx**：
//   force=0 → 原始 rdx（逐位相同 ⇒ 行为零改动）；
//   force=1 → 1（这就是"强制 true"的**全部**改动）。
// 这样"日志里的 forced=" 与"实际传出去的值"必然一致 —— 同一个表达式算出来的。
__declspec(noinline) std::uint64_t on_flag_setter_enter(void *camera_ptr, std::uint64_t value_raw)
{
    // ABI：bool 走 dl；rdx 高位未定义 ⇒ 只取低 8 位（与原函数的 `movzx edi,dl` 一致）。
    const std::uint8_t value = static_cast<std::uint8_t>(value_raw & 0xFFu);
    const bool force = g_force.load(std::memory_order_relaxed) != 0;
    const std::uint64_t forwarded = decide_forwarded_value(force, value_raw);
    if (forwarded != value_raw)
        g_overridden.fetch_add(1, std::memory_order_relaxed);

    const std::uint64_t call_index = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;

    if (value != 0)
        g_saw_true.store(true, std::memory_order_relaxed);
    else
        g_saw_false.store(true, std::memory_order_relaxed);
    g_last_value.store(value, std::memory_order_relaxed);
    g_last_forwarded.store(static_cast<std::uint8_t>(forwarded & 0xFFu), std::memory_order_relaxed);
    g_last_camera.store(reinterpret_cast<std::uint64_t>(camera_ptr), std::memory_order_relaxed);

    const FrameProvider provider = g_frame_provider.load(std::memory_order_relaxed);
    // 没有帧号来源时退回调用计数（setter 每帧一次 ⇒ 二者等价），
    // 这样 frame= 字段不会永远是假的 0，帧数上限也仍然可用。
    const std::uint64_t frame = provider != nullptr ? provider() : call_index;
    g_last_frame.store(frame, std::memory_order_relaxed);

    // ⚠️ 限流只影响**日志**，绝不影响转发：下面所有提前返回都必须返回 forwarded。
    const EmitDecision decision = decide_emit(g_emit_state, value, static_cast<std::uint8_t>(forwarded & 0xFFu),
                                              frame, call_index, g_frames_limit);
    if (!decision.emit)
        return forwarded;

    const LogSink sink = g_log_sink.load(std::memory_order_relaxed);
    if (sink == nullptr)
        return forwarded;

    // 栈上格式化：钩子内不做分配、不取锁、不碰日志器内部状态。
    char line[224] {};
    std::snprintf(line, sizeof(line),
                  "jitter_flag value=%s forced=%s callsite_imm=%d camera=0x%llx frame=%llu calls=%llu reason=%s",
                  value != 0 ? "true" : "false",
                  (forwarded & 0xFFu) != 0 ? "true" : "false",
                  g_callsite_imm.load(std::memory_order_relaxed),
                  static_cast<unsigned long long>(reinterpret_cast<std::uint64_t>(camera_ptr)),
                  static_cast<unsigned long long>(frame),
                  static_cast<unsigned long long>(call_index), decision.reason);
    sink(line);
    return forwarded;
}

} // namespace

std::uint64_t decide_forwarded_value(bool force, std::uint64_t original_value_raw)
{
    // force=0：**逐位原样**返回。刻意不"顺手规范化"成 0/1 ——
    // 那会改掉 rdx 高位（原始实参里那几位是未定义垃圾），
    // 虽然原函数的 `movzx edi,dl` 不读它们，但"只读观测"模式必须做到字面意义的零改动。
    return force ? 1ull : original_value_raw;
}

EmitDecision decide_emit(EmitState &state, std::uint8_t value, std::uint8_t forwarded, std::uint64_t frame,
                         std::uint64_t call_index, std::uint32_t frames_limit,
                         std::uint32_t heartbeat_interval)
{
    EmitDecision out;
    if (heartbeat_interval == 0)
        heartbeat_interval = k_heartbeat_calls;
    // 帧数上限：超过后**不再记录**，但钩子仍在（透传，行为零改动）。
    if (frames_limit != 0 && frame > frames_limit)
        return out;

    if (!state.seen_any)
    {
        out.emit = true;
        out.reason = "first";
    }
    else if (value != state.last_value || forwarded != state.last_forwarded)
    {
        // 原值变了（游戏自己改的）或转发值变了（我们按热键翻的）都要立刻可见。
        out.emit = true;
        out.reason = "change";
    }
    else if (call_index >= state.next_heartbeat_call)
    {
        out.emit = true;
        out.reason = "heartbeat";
    }

    if (out.emit)
    {
        // 心跳用**调用计数**而不是帧号：帧号来源缺失/冻结时心跳会永远不打，
        // 而"钩子还活着"恰恰只能靠心跳证明。
        state.next_heartbeat_call = call_index + heartbeat_interval;
    }
    state.seen_any = true;
    state.last_value = value;
    state.last_forwarded = forwarded;
    return out;
}

bool locate_flag_setter(const std::uint8_t *image, std::uint64_t image_size, std::uint32_t camera_rva,
                        std::uint32_t scan_window, std::uint32_t *out_setter_rva,
                        int *out_callsite_imm, const char **out_reason)
{
    if (out_setter_rva != nullptr)
        *out_setter_rva = 0;
    if (out_callsite_imm != nullptr)
        *out_callsite_imm = -1;
    if (out_reason != nullptr)
        *out_reason = nullptr;
    const auto fail = [out_reason](const char *reason) {
        if (out_reason != nullptr)
            *out_reason = reason;
        return false;
    };

    if (image == nullptr || camera_rva == 0)
        return fail("bad_rva");
    if (image_size == 0 || camera_rva >= image_size)
        return fail("bad_image_size");
    if (scan_window == 0)
        return fail("bad_scan_window");

    const std::uint64_t available = image_size - camera_rva;
    const std::uint64_t window = scan_window < available ? scan_window : available;
    if (window < 8)
        return fail("bad_image_size");

    const std::uint8_t *code = image + camera_rva;
    const std::size_t limit = static_cast<std::size_t>(window);

    // 调用点表（一次性安装路径；定长数组，避免在含 __try 的调用链里引入栈展开对象）
    struct CallSite
    {
        std::size_t offset = 0;   // 相对锚点函数起点的 E8 偏移
        std::uint32_t target = 0; // 目标 RVA
        int imm = -1;             // 调用点常量 bool 实参（-1 = 不可识别）
        bool matrix = false;
        bool boolean = false;
    };
    constexpr std::size_t k_max_calls = 64;
    CallSite sites[k_max_calls] {};
    std::size_t site_count = 0;

    for (std::size_t i = 0; i + 5 <= limit && site_count < k_max_calls; ++i)
    {
        if (code[i] != 0xE8)
            continue; // 只认直接 call rel32
        std::int32_t rel = 0;
        std::memcpy(&rel, code + i + 1, 4);
        const std::int64_t target =
            static_cast<std::int64_t>(camera_rva) + static_cast<std::int64_t>(i) + 5 + rel;
        if (target < 0 || static_cast<std::uint64_t>(target) + k_bool_head_len > image_size)
            continue;
        const auto *p = image + static_cast<std::uint64_t>(target);
        CallSite &site = sites[site_count];
        site.offset = i;
        site.target = static_cast<std::uint32_t>(target);
        site.imm = parse_callsite_imm(code, i);
        // 先判 bool（两者第 10 字节分叉，19 字节签名更严格）
        if (safe_head_match(p, k_bool_setter_head, k_bool_head_len))
            site.boolean = true;
        else if (safe_head_match(p, k_matrix_setter_head, k_matrix_head_len))
            site.matrix = true;
        else
            continue;
        ++site_count;
    }

    // Tier 1：bool 形状的调用点**恰好一个** → 直接采用。
    std::size_t bool_count = 0;
    const CallSite *only_bool = nullptr;
    for (std::size_t i = 0; i < site_count; ++i)
    {
        if (!sites[i].boolean)
            continue;
        ++bool_count;
        only_bool = &sites[i];
    }
    if (bool_count == 1)
    {
        if (out_setter_rva != nullptr)
            *out_setter_rva = only_bool->target;
        if (out_callsite_imm != nullptr)
            *out_callsite_imm = only_bool->imm;
        return true;
    }
    if (bool_count == 0)
        return fail("flag_setter_not_found");

    // Tier 2：多个候选 → 只保留「紧跟在 Matrix4x4 setter 调用之后（≤24 字节）
    // 且实参是编译期常量 bool」的那个。这正是 `= false` 那一行的代码形状。
    const CallSite *picked = nullptr;
    std::size_t picked_count = 0;
    for (std::size_t i = 0; i < site_count; ++i)
    {
        const CallSite &candidate = sites[i];
        if (!candidate.boolean || (candidate.imm != 0 && candidate.imm != 1))
            continue;
        bool follows_matrix = false;
        for (std::size_t j = 0; j < site_count; ++j)
        {
            if (!sites[j].matrix)
                continue;
            if (candidate.offset > sites[j].offset && candidate.offset - sites[j].offset <= 24)
            {
                follows_matrix = true;
                break;
            }
        }
        if (!follows_matrix)
            continue;
        ++picked_count;
        picked = &candidate;
    }
    if (picked_count != 1)
        return fail("flag_setter_ambiguous");

    if (out_setter_rva != nullptr)
        *out_setter_rva = picked->target;
    if (out_callsite_imm != nullptr)
        *out_callsite_imm = picked->imm;
    return true;
}

namespace
{

// 构建 stub：保存 rcx → 调 observer → **rcx 原样恢复、rdx 取 observer 返回值**
// → 重放原 16 字节序言 → 跳回。
// 形状与 Il2CppCallSiteHook.cpp 的 build_projection_stub 一致（那边同样重放
// `mov [rsp+8],rbx; push rdi; sub rsp,0x20; ...`，已在实机验证可行）。
// 保存槽放在影子空间之后（+0x20）：放在 +0x18 会被被调方覆盖。
// ⚠️ **刻意只保存 rcx**：rdx 不再需要"保存再恢复"——它由 observer 的返回值给出
//（force=0 时该返回值就是原始 rdx 的逐位拷贝）⇒ 少一个死存储，stub 里每个字节都承重。
void build_flag_stub(std::uint8_t *target, std::uint8_t *stub)
{
    constexpr std::size_t k_needed = 4 + 5 + 10 + 2 + 5 + 3 + 4 + k_patch_len + 10 + 2;
    static_assert(k_needed <= k_stub_len, "stub 超出 k_stub_len —— 同步调整 k_stub_len");
    const std::uint64_t observer = reinterpret_cast<std::uint64_t>(&on_flag_setter_enter);
    std::size_t p = 0;
    // sub rsp, 0x38
    stub[p++] = 0x48; stub[p++] = 0x83; stub[p++] = 0xEC; stub[p++] = 0x38;
    // mov [rsp+0x20], rcx  = 48 89 4C 24 20      （保存 this）
    stub[p++] = 0x48; stub[p++] = 0x89; stub[p++] = 0x4C; stub[p++] = 0x24; stub[p++] = 0x20;
    // mov rax, observer ; call rax                （observer 看到的是**原始** rcx/rdx）
    stub[p++] = 0x48; stub[p++] = 0xB8; write_u64(stub + p, observer); p += 8;
    stub[p++] = 0xFF; stub[p++] = 0xD0;
    // mov rcx, [rsp+0x20]  = 48 8B 4C 24 20      （this 指针**逐位原样**恢复）
    stub[p++] = 0x48; stub[p++] = 0x8B; stub[p++] = 0x4C; stub[p++] = 0x24; stub[p++] = 0x20;
    // mov rdx, rax         = 48 89 C2            （★ 唯一被"改"的东西：转发值）
    // —— force=0 时 rax 就是原始 rdx（逐位相同）⇒ 与改动前的 stub 语义完全等价。
    stub[p++] = 0x48; stub[p++] = 0x89; stub[p++] = 0xC2;
    // add rsp, 0x38
    stub[p++] = 0x48; stub[p++] = 0x83; stub[p++] = 0xC4; stub[p++] = 0x38;
    // 原 16 字节序言
    std::memcpy(stub + p, g_saved, k_patch_len);
    p += k_patch_len;
    // mov rax, target+16 ; jmp rax
    stub[p++] = 0x48; stub[p++] = 0xB8;
    write_u64(stub + p, reinterpret_cast<std::uint64_t>(target) + k_patch_len);
    p += 8;
    stub[p++] = 0xFF; stub[p++] = 0xE0;
    while (p < k_stub_len)
        stub[p++] = 0x90;
}

} // namespace

void set_log_sink(LogSink sink)
{
    g_log_sink.store(sink, std::memory_order_release);
}

void set_frame_provider(FrameProvider provider)
{
    g_frame_provider.store(provider, std::memory_order_release);
}

bool install(std::uint64_t exe_base, const Config &cfg, const char **out_reason)
{
    if (out_reason != nullptr)
        *out_reason = nullptr;
    const auto fail = [out_reason](const char *reason) {
        if (out_reason != nullptr)
            *out_reason = reason;
        return false;
    };

    // 关闭时**零开销、零字节改动**：连序言都不读。
    if (!cfg.enabled)
        return fail("disabled");
    if (g_active.load(std::memory_order_relaxed))
        return true;
    if (exe_base == 0 || cfg.camera_rva == 0)
        return fail("bad_rva");
    if (cfg.image_size == 0 || cfg.camera_rva >= cfg.image_size)
        return fail("bad_image_size");

    const auto *base = reinterpret_cast<const std::uint8_t *>(exe_base);
    // 锚点序言校验（RVA 或版本不匹配时安全放弃 —— 绝不盲 patch）。
    // 用 SEH 版本读：camera_rva 可能落在 SizeOfImage 内但**未提交**的页上
    //（配置兜底 RVA 对不上版本时就是这种情形），裸 memcmp 会直接把游戏打崩在启动路径。
    if (!safe_head_match(base + cfg.camera_rva, k_camera_head, k_camera_head_len))
        return fail("camera_prologue_mismatch");

    std::uint32_t setter_rva = 0;
    int callsite_imm = -1;
    const char *locate_reason = nullptr;
    if (!locate_flag_setter(base, cfg.image_size, cfg.camera_rva, k_scan_window, &setter_rva,
                            &callsite_imm, &locate_reason))
        return fail(locate_reason != nullptr ? locate_reason : "flag_setter_not_found");

    auto *target = const_cast<std::uint8_t *>(base) + setter_rva;
    // 装钩前**再校验一次**序言（发现与安装之间不信任任何隐式假设）
    if (!safe_head_match(target, k_bool_setter_head, k_patch_len))
        return fail("flag_setter_prologue_mismatch");

    DWORD old_protect = 0;
    if (!VirtualProtect(target, k_patch_len, PAGE_EXECUTE_READWRITE, &old_protect))
        return fail("protect_failed");
    std::memcpy(g_saved, target, k_patch_len);

    g_stub = static_cast<std::uint8_t *>(
        VirtualAlloc(nullptr, k_stub_len, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (g_stub == nullptr)
    {
        // 目标字节此刻仍是原始内容（下面才写 patch）⇒ 只需恢复保护属性。
        DWORD ignored = 0;
        VirtualProtect(target, k_patch_len, old_protect, &ignored);
        return fail("alloc_failed");
    }
    build_flag_stub(target, g_stub);

    std::uint8_t patch[k_patch_len] {};
    build_jmp_patch(patch, k_patch_len, g_stub);
    std::memcpy(target, patch, k_patch_len);
    VirtualProtect(target, k_patch_len, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), target, k_patch_len);

    g_frames_limit = cfg.frames_limit;
    g_target = target;
    // 初值来自 ini（JitterFlagForce）。热键之后可以随时翻转它 —— stub 每次调用都重读，
    // 因此**不需要重装钩子**就能在同一次会话内 A/B。
    g_force.store(cfg.force ? 1 : 0, std::memory_order_relaxed);
    g_callsite_imm.store(callsite_imm, std::memory_order_relaxed);
    g_active.store(true, std::memory_order_release);

    // 安装成功后立刻打一行"证据行"：把**签名发现的结果**、**调用点的静态常量实参**
    // 与 **force 初值**写进日志。静态常量就是那条 `= false` 的机器码（`xor edx,edx`）；
    // 它与运行时钩子互相独立 —— 两者一致才说明钩子挂在了正确的地方。
    if (const LogSink sink = g_log_sink.load(std::memory_order_relaxed))
    {
        char line[224] {};
        std::snprintf(line, sizeof(line),
                      "jitter_flag_probe_installed setter_rva=0x%x callsite_imm=%d camera_rva=0x%x force=%d",
                      static_cast<unsigned>(setter_rva), callsite_imm,
                      static_cast<unsigned>(cfg.camera_rva), cfg.force ? 1 : 0);
        sink(line);
    }
    return true;
}

void shutdown()
{
    if (!g_active.exchange(false, std::memory_order_acq_rel))
        return;
    if (g_target == nullptr || g_stub == nullptr)
        return;

    // ⚠️ 校验后还原：只有当前字节**仍是我们写入的那条 jmp** 时才还原。
    // 若期间有别的插件在我们的跳转之上又写了补丁，盲目还原会把**对方的**补丁抹掉
    //（对方的钩子静默失效，且表现为"用了本桥之后另一个插件就不工作了"）。
    std::uint8_t expected[k_patch_len] {};
    build_jmp_patch(expected, k_patch_len, g_stub);
    if (std::memcmp(g_target, expected, k_patch_len) == 0)
    {
        DWORD old_protect = 0;
        if (VirtualProtect(g_target, k_patch_len, PAGE_EXECUTE_READWRITE, &old_protect))
        {
            std::memcpy(g_target, g_saved, k_patch_len);
            VirtualProtect(g_target, k_patch_len, old_protect, &old_protect);
            FlushInstructionCache(GetCurrentProcess(), g_target, k_patch_len);
        }
    }

    // ⚠️ 刻意**不** VirtualFree stub：还原只能阻止"新的"跳入，无法排除此刻已有线程
    // 正执行在 stub 内（FreeLibrary 不终止其他线程）⇒ 释放会让它们跳进已解除映射的
    // 内存而崩溃。stub 仅 96 字节，泄漏远优于崩溃。（与 Il2CppCallSiteHook 同一处置。）
    g_target = nullptr;
    g_stub = nullptr;
    // force 状态复位：还原后 stub 已不可达，"强制开着"这个残留状态会误导下一个读者
    // （以及误报给 A/B 结论）。init 侧会按 ini 重新安装。
    g_force.store(0, std::memory_order_relaxed);
}

bool active()
{
    return g_active.load(std::memory_order_relaxed);
}

std::uint64_t observed_count()
{
    return g_calls.load(std::memory_order_relaxed);
}

bool set_force(bool on, const char *source, std::uint64_t frame)
{
    const std::uint8_t next = on ? 1 : 0;
    const std::uint8_t previous = g_force.exchange(next, std::memory_order_acq_rel);
    if (previous == next)
        return false;

    // 每次**真的**切换都必打一行（这正是 A/B 的时间锚点：日志里能定位到哪一帧翻的面）。
    // 状态未变时不打 —— 热键是边沿检测，但万一被重复调用也不该刷屏。
    if (const LogSink sink = g_log_sink.load(std::memory_order_relaxed))
    {
        char line[192] {};
        std::snprintf(line, sizeof(line), "jitter_flag_force toggled on=%s source=%s frame=%llu",
                      on ? "true" : "false", source != nullptr ? source : "unknown",
                      static_cast<unsigned long long>(frame));
        sink(line);
    }
    return true;
}

bool force_enabled()
{
    return g_force.load(std::memory_order_relaxed) != 0;
}

bool last_forwarding(std::uint8_t *out_original, std::uint8_t *out_forwarded)
{
    if (g_calls.load(std::memory_order_relaxed) == 0)
        return false;
    if (out_original != nullptr)
        *out_original = g_last_value.load(std::memory_order_relaxed);
    if (out_forwarded != nullptr)
        *out_forwarded = g_last_forwarded.load(std::memory_order_relaxed);
    return true;
}

std::uint64_t overridden_count()
{
    return g_overridden.load(std::memory_order_relaxed);
}

bool last_observation(std::uint8_t *out_value, std::uint64_t *out_camera, std::uint64_t *out_frame)
{
    if (g_calls.load(std::memory_order_relaxed) == 0)
        return false;
    if (out_value != nullptr)
        *out_value = g_last_value.load(std::memory_order_relaxed);
    if (out_camera != nullptr)
        *out_camera = g_last_camera.load(std::memory_order_relaxed);
    if (out_frame != nullptr)
        *out_frame = g_last_frame.load(std::memory_order_relaxed);
    return true;
}

bool saw_false()
{
    return g_saw_false.load(std::memory_order_relaxed);
}

bool saw_true()
{
    return g_saw_true.load(std::memory_order_relaxed);
}

} // namespace jitter_flag_probe
