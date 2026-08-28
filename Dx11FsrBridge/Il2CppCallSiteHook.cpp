#include "Il2CppCallSiteHook.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace il2cpp_callsite
{
namespace
{

constexpr std::size_t k_patch_len = 12;
constexpr std::size_t k_stub_len = 64;
// ConfigureJitteredProjectionMatrix starts with seven pushes followed by
// `sub rsp, 0x140` (7 bytes).  A 12-byte patch cuts that instruction, so the
// camera hook needs an independent 14-byte trampoline boundary.
constexpr std::size_t k_camera_patch_len = 14;
constexpr std::size_t k_camera_stub_len = 96;
constexpr std::uint8_t k_camera_head[k_camera_patch_len] = {
    0x41, 0x57, 0x41, 0x56, 0x56, 0x57, 0x53,
    0x48, 0x81, 0xEC, 0x40, 0x01, 0x00, 0x00,
};
constexpr std::size_t k_projection_patch_len = 16;
constexpr std::size_t k_projection_stub_len = 96;
constexpr std::uint8_t k_projection_head[k_projection_patch_len] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
    0xEC, 0x20, 0x48, 0x8B, 0xFA, 0x48, 0x8B, 0xD9,
};

// 7.0 序言期望字节（实测，见 fsr2-callsite-70.md）
// Render: push r15 r14 r13 r12 rsi rdi rbp rbx
constexpr std::uint8_t k_render_head[12] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57, 0x55, 0x53};
// UpdateCommandBuffer: push rsi rdi rbp rbx; sub rsp,0x28
constexpr std::uint8_t k_ucb_head[8] = {0x56, 0x57, 0x55, 0x53, 0x48, 0x83, 0xEC, 0x28};

// ---- 2026-08-26（AA 修复）：相机参数捕获全局（声明须在 shutdown() 之前）----
std::uint8_t *g_camera_render = nullptr;
std::uint8_t g_camera_saved[k_camera_patch_len] {};
std::uint8_t *g_camera_stub = nullptr;
std::atomic_bool g_camera_active { false };
float g_camera_buf[64] {};
std::size_t g_camera_count = 0;
std::atomic_uint64_t g_camera_generation { 0 };
std::mutex g_camera_mutex;

std::uint8_t *g_projection_setter = nullptr;
std::uint8_t g_projection_saved[k_projection_patch_len] {};
std::uint8_t *g_projection_stub = nullptr;
std::atomic_bool g_projection_active { false };
float g_projection_matrix[16] {};
std::uint64_t g_projection_camera = 0;
std::atomic_uint64_t g_projection_generation { 0 };
std::mutex g_projection_mutex;

std::uint8_t *g_render = nullptr;
std::uint8_t g_saved[k_patch_len] {};
std::uint8_t *g_stub = nullptr;
std::atomic_bool g_active { false };
std::atomic_uint64_t g_render_calls { 0 };
std::atomic_uint64_t g_params_generation { 0 };
CapturedParams g_params {};
// 2026-08-25：按实例存储（最多 8 个实例；主/UI 各自独立代次与参数）
struct PerInstanceParams
{
    std::uint64_t instance = 0;
    std::uint64_t generation = 0;
    CapturedParams params {};
    RenderToken pending[4] {};
    std::size_t next_pending = 0;
};
PerInstanceParams g_inst_params[8] {};
std::size_t g_inst_count = 0;
std::mutex g_inst_mutex;

// 由 stub 调用的计数 + 参数捕获入口（rcx=this, rdx=context）。
// 字段偏移（7.0，getter 字节实证）：jitter Vector2@0x24, m_FrameIndex int@0x50,
// m_RenderSize Vector2Int@0x58, m_DisplaySize Vector2Int@0x68。
__declspec(noinline) void on_render_enter(void *this_ptr, void *context_ptr)
{
    g_render_calls.fetch_add(1, std::memory_order_relaxed);
    if (this_ptr != nullptr)
    {
        const auto *p = static_cast<const std::uint8_t *>(this_ptr);
        CapturedParams cp {};
        cp.instance = reinterpret_cast<std::uint64_t>(this_ptr);
        cp.context = reinterpret_cast<std::uint64_t>(context_ptr);
        std::memcpy(&cp.render_w, p + 0x58, 4);
        std::memcpy(&cp.render_h, p + 0x5C, 4);
        std::memcpy(&cp.display_w, p + 0x68, 4);
        std::memcpy(&cp.display_h, p + 0x6C, 4);
        std::memcpy(&cp.frame_index, p + 0x50, 4);
        std::memcpy(&cp.jitter_x, p + 0x24, 4);
        std::memcpy(&cp.jitter_y, p + 0x28, 4);
        g_params = cp;
        const std::uint64_t generation =
            g_params_generation.fetch_add(1, std::memory_order_release) + 1;
        // 按实例记录（保持全局兼容 + 每实例代次）
        {
            std::lock_guard lock(g_inst_mutex);
            const std::uint64_t inst = cp.instance;
            std::size_t slot = g_inst_count;
            for (std::size_t i = 0; i < g_inst_count; ++i)
            {
                if (g_inst_params[i].instance == inst)
                {
                    slot = i;
                    break;
                }
            }
            if (slot >= 8)
            {
                // 超过容量：LRU 覆盖最旧的（用 generation 最小者）
                std::size_t oldest = 0;
                for (std::size_t i = 1; i < 8; ++i)
                    if (g_inst_params[i].generation < g_inst_params[oldest].generation)
                        oldest = i;
                slot = oldest;
            }
            else if (slot == g_inst_count)
            {
                ++g_inst_count;
            }
            if (g_inst_params[slot].instance != inst)
                g_inst_params[slot] = PerInstanceParams {};
            PerInstanceParams &entry = g_inst_params[slot];
            entry.instance = inst;
            entry.generation = generation;
            entry.params = cp;
            RenderToken &token = entry.pending[entry.next_pending];
            token.params = cp;
            token.generation = generation;
            token.capture_tick = GetTickCount64();
            entry.next_pending = (entry.next_pending + 1u) % 4u;
        }
    }
}

void write_u64(std::uint8_t *dst, std::uint64_t value)
{
    std::memcpy(dst, &value, 8);
}

// 构建 observe/skip 两种 stub。
bool build_stub(bool skip_render, std::uint8_t *render, std::uint8_t *stub)
{
    const std::uint64_t counter = reinterpret_cast<std::uint64_t>(&on_render_enter);
    std::size_t p = 0;
    // Reserve shadow space plus two private save slots.  The earlier +0x18
    // slot sat inside the callee's shadow space and could be overwritten by
    // on_render_enter once token bookkeeping added further calls.
    // sub rsp, 0x38
    stub[p++] = 0x48; stub[p++] = 0x83; stub[p++] = 0xEC; stub[p++] = 0x38;
    // mov [rsp+0x20], rcx  (private save slot after 32-byte shadow space)
    stub[p++] = 0x48; stub[p++] = 0x89; stub[p++] = 0x4C; stub[p++] = 0x24; stub[p++] = 0x20;
    // mov [rsp+0x28], rdx
    stub[p++] = 0x48; stub[p++] = 0x89; stub[p++] = 0x54; stub[p++] = 0x24; stub[p++] = 0x28;
    // mov rax, on_render_enter; call rax
    stub[p++] = 0x48; stub[p++] = 0xB8; write_u64(stub + p, counter); p += 8;
    stub[p++] = 0xFF; stub[p++] = 0xD0;
    if (!skip_render)
    {
        // mov rcx, [rsp+0x20]; mov rdx, [rsp+0x28]
        stub[p++] = 0x48; stub[p++] = 0x8B; stub[p++] = 0x4C; stub[p++] = 0x24; stub[p++] = 0x20;
        stub[p++] = 0x48; stub[p++] = 0x8B; stub[p++] = 0x54; stub[p++] = 0x24; stub[p++] = 0x28;
        // add rsp, 0x38
        stub[p++] = 0x48; stub[p++] = 0x83; stub[p++] = 0xC4; stub[p++] = 0x38;
        // 原 12 字节序言
        std::memcpy(stub + p, g_saved, k_patch_len);
        p += k_patch_len;
        // mov rax, render+12; jmp rax
        stub[p++] = 0x48; stub[p++] = 0xB8;
        write_u64(stub + p, reinterpret_cast<std::uint64_t>(render) + k_patch_len);
        p += 8;
        stub[p++] = 0xFF; stub[p++] = 0xE0;
    }
    else
    {
        // add rsp, 0x38; ret（void 方法，直接返回）
        stub[p++] = 0x48; stub[p++] = 0x83; stub[p++] = 0xC4; stub[p++] = 0x38;
        stub[p++] = 0xC3;
    }
    while (p < k_stub_len)
        stub[p++] = 0x90; // nop 填充
    return true;
}

} // namespace

bool install(std::uint64_t exe_base, const Config &cfg)
{
    if (g_active.load(std::memory_order_relaxed))
        return true;
    if (exe_base == 0 || cfg.render_rva == 0)
        return false;

    std::uint8_t *render = reinterpret_cast<std::uint8_t *>(exe_base + cfg.render_rva);
    std::uint8_t *ucb = reinterpret_cast<std::uint8_t *>(exe_base + cfg.update_cmd_buffer_rva);

    // 序言验证（RVA 或版本不匹配时安全放弃）
    if (std::memcmp(render, k_render_head, sizeof(k_render_head)) != 0)
        return false;
    if (ucb >= render || render - ucb != static_cast<std::ptrdiff_t>(cfg.render_rva - cfg.update_cmd_buffer_rva))
        return false;
    if (std::memcmp(ucb, k_ucb_head, sizeof(k_ucb_head)) != 0)
        return false;

    // 保存原字节
    DWORD old_protect = 0;
    if (!VirtualProtect(render, k_patch_len, PAGE_EXECUTE_READWRITE, &old_protect))
        return false;
    std::memcpy(g_saved, render, k_patch_len);

    // 分配可执行 stub
    g_stub = static_cast<std::uint8_t *>(
        VirtualAlloc(nullptr, k_stub_len, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (g_stub == nullptr)
    {
        VirtualProtect(render, k_patch_len, old_protect, &old_protect);
        return false;
    }
    build_stub(cfg.skip_render, render, g_stub);

    // 写 12 字节绝对跳转: 48 B8 <stub> FF E0
    std::uint8_t patch[k_patch_len] {};
    patch[0] = 0x48;
    patch[1] = 0xB8;
    write_u64(patch + 2, reinterpret_cast<std::uint64_t>(g_stub));
    patch[10] = 0xFF;
    patch[11] = 0xE0;
    std::memcpy(render, patch, k_patch_len);
    VirtualProtect(render, k_patch_len, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), render, k_patch_len);

    g_render = render;
    g_active.store(true, std::memory_order_release);
    return true;
}

void shutdown()
{
    const bool render_active = g_active.exchange(false, std::memory_order_acq_rel);
    const bool camera_active = g_camera_active.exchange(false, std::memory_order_acq_rel);
    const bool projection_active = g_projection_active.exchange(false, std::memory_order_acq_rel);
    if (!render_active && !camera_active && !projection_active)
        return;
    if (render_active && g_render != nullptr)
    {
        DWORD old_protect = 0;
        if (VirtualProtect(g_render, k_patch_len, PAGE_EXECUTE_READWRITE, &old_protect))
        {
            std::memcpy(g_render, g_saved, k_patch_len);
            VirtualProtect(g_render, k_patch_len, old_protect, &old_protect);
            FlushInstructionCache(GetCurrentProcess(), g_render, k_patch_len);
        }
        g_render = nullptr;
    }
    if (render_active && g_stub != nullptr)
    {
        VirtualFree(g_stub, 0, MEM_RELEASE);
        g_stub = nullptr;
    }
    if (camera_active && g_camera_render != nullptr)
    {
        DWORD old_protect = 0;
        if (VirtualProtect(g_camera_render, k_camera_patch_len, PAGE_EXECUTE_READWRITE, &old_protect))
        {
            std::memcpy(g_camera_render, g_camera_saved, k_camera_patch_len);
            VirtualProtect(g_camera_render, k_camera_patch_len, old_protect, &old_protect);
            FlushInstructionCache(GetCurrentProcess(), g_camera_render, k_camera_patch_len);
        }
        g_camera_render = nullptr;
    }
    if (camera_active && g_camera_stub != nullptr)
    {
        VirtualFree(g_camera_stub, 0, MEM_RELEASE);
        g_camera_stub = nullptr;
    }
    if (projection_active && g_projection_setter != nullptr)
    {
        DWORD old_protect = 0;
        if (VirtualProtect(g_projection_setter, k_projection_patch_len, PAGE_EXECUTE_READWRITE, &old_protect))
        {
            std::memcpy(g_projection_setter, g_projection_saved, k_projection_patch_len);
            VirtualProtect(g_projection_setter, k_projection_patch_len, old_protect, &old_protect);
            FlushInstructionCache(GetCurrentProcess(), g_projection_setter, k_projection_patch_len);
        }
        g_projection_setter = nullptr;
    }
    if (projection_active && g_projection_stub != nullptr)
    {
        VirtualFree(g_projection_stub, 0, MEM_RELEASE);
        g_projection_stub = nullptr;
    }
}

bool active()
{
    return g_active.load(std::memory_order_relaxed);
}

std::uint64_t render_call_count()
{
    return g_render_calls.load(std::memory_order_relaxed);
}

std::uint64_t params_generation()
{
    return g_params_generation.load(std::memory_order_acquire);
}

bool last_params(CapturedParams &out)
{
    out = g_params;
    return out.render_w != 0 && out.render_h != 0 && out.display_w != 0 && out.display_h != 0;
}

bool last_params_for(std::uint64_t instance, CapturedParams &out, std::uint64_t &generation)
{
    std::lock_guard lock(g_inst_mutex);
    if (instance == 0)
    {
        // 兼容旧调用方：返回全局最新
        out = g_params;
        generation = g_params_generation.load(std::memory_order_relaxed);
        return out.render_w != 0 && out.render_h != 0 && out.display_w != 0 && out.display_h != 0;
    }
    for (std::size_t i = 0; i < g_inst_count; ++i)
    {
        if (g_inst_params[i].instance == instance)
        {
            out = g_inst_params[i].params;
            generation = g_inst_params[i].generation;
            return true;
        }
    }
    return false;
}

bool latest_pending_render_token_for(std::uint64_t instance,
                                     std::uint32_t render_w, std::uint32_t render_h,
                                     RenderToken &out)
{
    std::lock_guard lock(g_inst_mutex);
    if (instance == 0 || render_w == 0 || render_h == 0)
        return false;
    const std::uint64_t now = GetTickCount64();
    for (std::size_t i = 0; i < g_inst_count; ++i)
    {
        const PerInstanceParams &entry = g_inst_params[i];
        if (entry.instance != instance)
            continue;
        // Render 可能在渲染线程 draw 之前连续记录多个 token。按“最新”选择
        // 会把较早 token 一起淘汰，造成 gen/frame 跳号；时域 AA 尤其在局部
        // 动态时会短暂失效。按 generation 顺序消费最早的有效 token。
        std::uint64_t best_generation = UINT64_MAX;
        for (const RenderToken &token : entry.pending)
        {
            if (token.generation != 0 && token.generation < best_generation &&
                token.capture_tick <= now && now - token.capture_tick <= 250 &&
                token.params.render_w == render_w && token.params.render_h == render_h)
            {
                best_generation = token.generation;
                out = token;
            }
        }
        return best_generation != 0;
    }
    return false;
}

bool consume_render_token_for(std::uint64_t instance, std::uint64_t generation)
{
    std::lock_guard lock(g_inst_mutex);
    if (instance == 0 || generation == 0)
        return false;
    for (std::size_t i = 0; i < g_inst_count; ++i)
    {
        PerInstanceParams &entry = g_inst_params[i];
        if (entry.instance != instance)
            continue;
        bool consumed = false;
        for (RenderToken &token : entry.pending)
        {
            if (token.generation != 0 && token.generation <= generation)
            {
                token = RenderToken {};
                consumed = true;
            }
        }
        return consumed;
    }
    return false;
}

void known_instances(std::uint64_t *out, std::size_t capacity, std::size_t &count)
{
    std::lock_guard lock(g_inst_mutex);
    count = 0;
    for (std::size_t i = 0; i < g_inst_count && count < capacity; ++i)
        out[count++] = g_inst_params[i].instance;
}

std::uint64_t params_generation_for(std::uint64_t instance)
{
    std::lock_guard lock(g_inst_mutex);
    if (instance == 0)
        return g_params_generation.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < g_inst_count; ++i)
    {
        if (g_inst_params[i].instance == instance)
            return g_inst_params[i].generation;
    }
    return 0;
}

// ---- 2026-08-26（AA 修复）：相机参数捕获 ----
// ConfigureJitteredProjectionMatrix(camera) @0x06B558D0 每帧被 OnPreCull 调用（7.0 实证：
// Prepare 已内联，此函数是每帧携带 camera 指针的稳定调用点）。抓取相机结构体前 64 个
// float（512B），用于确定游戏真实 near/far/FOV（全部 RE 资料未确定；桥此前硬编码占位）。
// （全局变量 g_camera_* 声明在文件顶部。）

// MSVC 不允许同一函数同时含有 SEH 和需要 C++ 展开的对象（下面的 mutex lock）。
// 将不可信指针读取隔离在无析构对象的 helper 内，随后再进入 C++ 同步区。
static bool try_copy_camera(void *camera_ptr, float out[64])
{
    __try
    {
        std::memcpy(out, camera_ptr, sizeof(float) * 64u);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static bool try_copy_projection(const void *matrix_ptr, float out[16])
{
    __try
    {
        std::memcpy(out, matrix_ptr, sizeof(float) * 16u);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

__declspec(noinline) void on_projection_setter_enter(void *camera_ptr, const void *matrix_ptr)
{
    if (matrix_ptr == nullptr)
        return;
    float matrix[16] {};
    if (!try_copy_projection(matrix_ptr, matrix))
        return;
    std::lock_guard lock(g_projection_mutex);
    std::memcpy(g_projection_matrix, matrix, sizeof(matrix));
    g_projection_camera = reinterpret_cast<std::uint64_t>(camera_ptr);
    g_projection_generation.fetch_add(1, std::memory_order_release);
}

// 由 stub 调用（rcx=this, rdx=camera）。SEH 保护：camera 指针可能非法/越界。
__declspec(noinline) void on_camera_enter(void *this_ptr, void *camera_ptr)
{
    if (camera_ptr == nullptr)
        return;
    float buf[64] {};
    if (!try_copy_camera(camera_ptr, buf))
        return;
    std::lock_guard lock(g_camera_mutex);
    std::memcpy(g_camera_buf, buf, sizeof(buf));
    g_camera_count = 64;
    g_camera_generation.fetch_add(1, std::memory_order_release);
}

// 构建相机捕获 stub：保存 rcx/rdx → 调 on_camera_enter → 恢复 → 透传原函数。
bool build_camera_stub(std::uint8_t *target, std::uint8_t *stub)
{
    const std::uint64_t observer = reinterpret_cast<std::uint64_t>(&on_camera_enter);
    std::size_t p = 0;
    // Shadow space plus two private save slots.  Saving at +0x18/+0x20 put
    // the old stub inside the callee's shadow space and allowed the observer
    // to overwrite rcx/rdx before the original function resumed.
    // sub rsp, 0x38
    stub[p++] = 0x48; stub[p++] = 0x83; stub[p++] = 0xEC; stub[p++] = 0x38;
    // mov [rsp+0x20], rcx（this）; mov [rsp+0x28], rdx（camera）
    stub[p++] = 0x48; stub[p++] = 0x89; stub[p++] = 0x4C; stub[p++] = 0x24; stub[p++] = 0x20;
    stub[p++] = 0x48; stub[p++] = 0x89; stub[p++] = 0x54; stub[p++] = 0x24; stub[p++] = 0x28;
    // mov rax, on_camera_enter; call rax
    stub[p++] = 0x48; stub[p++] = 0xB8; write_u64(stub + p, observer); p += 8;
    stub[p++] = 0xFF; stub[p++] = 0xD0;
    // mov rcx, [rsp+0x20]; mov rdx, [rsp+0x28]
    stub[p++] = 0x48; stub[p++] = 0x8B; stub[p++] = 0x4C; stub[p++] = 0x24; stub[p++] = 0x20;
    stub[p++] = 0x48; stub[p++] = 0x8B; stub[p++] = 0x54; stub[p++] = 0x24; stub[p++] = 0x28;
    // add rsp, 0x38
    stub[p++] = 0x48; stub[p++] = 0x83; stub[p++] = 0xC4; stub[p++] = 0x38;
    // 原完整 14 字节序言
    std::memcpy(stub + p, g_camera_saved, k_camera_patch_len);
    p += k_camera_patch_len;
    // mov rax, target+14; jmp rax
    stub[p++] = 0x48; stub[p++] = 0xB8;
    write_u64(stub + p, reinterpret_cast<std::uint64_t>(target) + k_camera_patch_len);
    p += 8;
    stub[p++] = 0xFF; stub[p++] = 0xE0;
    while (p < k_camera_stub_len)
        stub[p++] = 0x90;
    return true;
}

bool install_camera(std::uint64_t exe_base, const Config &cfg)
{
    if (g_camera_active.load(std::memory_order_relaxed))
        return true;
    if (exe_base == 0 || cfg.camera_rva == 0)
        return false;

    std::uint8_t *target = reinterpret_cast<std::uint8_t *>(exe_base + cfg.camera_rva);
    // 只接受经当前游戏运行时探针确认的完整序言，避免错误 RVA 或跨指令覆盖。
    if (std::memcmp(target, k_camera_head, k_camera_patch_len) != 0)
        return false;

    DWORD old_protect = 0;
    if (!VirtualProtect(target, k_camera_patch_len, PAGE_EXECUTE_READWRITE, &old_protect))
        return false;
    std::memcpy(g_camera_saved, target, k_camera_patch_len);

    g_camera_stub = static_cast<std::uint8_t *>(
        VirtualAlloc(nullptr, k_camera_stub_len, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (g_camera_stub == nullptr)
    {
        std::memcpy(target, g_camera_saved, k_camera_patch_len);
        VirtualProtect(target, k_camera_patch_len, old_protect, &old_protect);
        return false;
    }
    build_camera_stub(target, g_camera_stub);

    std::uint8_t patch[k_camera_patch_len] {};
    patch[0] = 0x48;
    patch[1] = 0xB8;
    write_u64(patch + 2, reinterpret_cast<std::uint64_t>(g_camera_stub));
    patch[10] = 0xFF;
    patch[11] = 0xE0;
    patch[12] = 0x90;
    patch[13] = 0x90;
    std::memcpy(target, patch, k_camera_patch_len);
    VirtualProtect(target, k_camera_patch_len, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), target, k_camera_patch_len);

    g_camera_render = target;
    g_camera_active.store(true, std::memory_order_release);
    return true;
}

bool camera_ready()
{
    return g_camera_active.load(std::memory_order_relaxed) && g_camera_count > 0;
}

std::uint64_t camera_generation()
{
    return g_camera_generation.load(std::memory_order_acquire);
}

std::size_t camera_floats(float *out, std::size_t capacity)
{
    std::lock_guard lock(g_camera_mutex);
    const std::size_t n = g_camera_count < capacity ? g_camera_count : capacity;
    std::memcpy(out, g_camera_buf, n * sizeof(float));
    return n;
}

bool build_projection_stub(std::uint8_t *target, std::uint8_t *stub)
{
    const std::uint64_t observer = reinterpret_cast<std::uint64_t>(&on_projection_setter_enter);
    std::size_t p = 0;
    // Shadow space plus two private saved argument slots.
    stub[p++] = 0x48; stub[p++] = 0x83; stub[p++] = 0xEC; stub[p++] = 0x38;
    stub[p++] = 0x48; stub[p++] = 0x89; stub[p++] = 0x4C; stub[p++] = 0x24; stub[p++] = 0x20;
    stub[p++] = 0x48; stub[p++] = 0x89; stub[p++] = 0x54; stub[p++] = 0x24; stub[p++] = 0x28;
    stub[p++] = 0x48; stub[p++] = 0xB8; write_u64(stub + p, observer); p += 8;
    stub[p++] = 0xFF; stub[p++] = 0xD0;
    stub[p++] = 0x48; stub[p++] = 0x8B; stub[p++] = 0x4C; stub[p++] = 0x24; stub[p++] = 0x20;
    stub[p++] = 0x48; stub[p++] = 0x8B; stub[p++] = 0x54; stub[p++] = 0x24; stub[p++] = 0x28;
    stub[p++] = 0x48; stub[p++] = 0x83; stub[p++] = 0xC4; stub[p++] = 0x38;
    std::memcpy(stub + p, g_projection_saved, k_projection_patch_len);
    p += k_projection_patch_len;
    stub[p++] = 0x48; stub[p++] = 0xB8;
    write_u64(stub + p, reinterpret_cast<std::uint64_t>(target) + k_projection_patch_len);
    p += 8;
    stub[p++] = 0xFF; stub[p++] = 0xE0;
    while (p < k_projection_stub_len)
        stub[p++] = 0x90;
    return true;
}

bool install_projection_setter(std::uint64_t exe_base, const Config &cfg)
{
    if (g_projection_active.load(std::memory_order_relaxed))
        return true;
    if (exe_base == 0 || cfg.projection_setter_rva == 0)
        return false;
    std::uint8_t *target = reinterpret_cast<std::uint8_t *>(exe_base + cfg.projection_setter_rva);
    if (std::memcmp(target, k_projection_head, k_projection_patch_len) != 0)
        return false;
    DWORD old_protect = 0;
    if (!VirtualProtect(target, k_projection_patch_len, PAGE_EXECUTE_READWRITE, &old_protect))
        return false;
    std::memcpy(g_projection_saved, target, k_projection_patch_len);
    g_projection_stub = static_cast<std::uint8_t *>(
        VirtualAlloc(nullptr, k_projection_stub_len, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (g_projection_stub == nullptr)
    {
        std::memcpy(target, g_projection_saved, k_projection_patch_len);
        VirtualProtect(target, k_projection_patch_len, old_protect, &old_protect);
        return false;
    }
    build_projection_stub(target, g_projection_stub);
    std::uint8_t patch[k_projection_patch_len] {};
    patch[0] = 0x48; patch[1] = 0xB8;
    write_u64(patch + 2, reinterpret_cast<std::uint64_t>(g_projection_stub));
    patch[10] = 0xFF; patch[11] = 0xE0;
    for (std::size_t i = 12; i < k_projection_patch_len; ++i)
        patch[i] = 0x90;
    std::memcpy(target, patch, k_projection_patch_len);
    VirtualProtect(target, k_projection_patch_len, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), target, k_projection_patch_len);
    g_projection_setter = target;
    g_projection_active.store(true, std::memory_order_release);
    return true;
}

bool projection_ready()
{
    return g_projection_active.load(std::memory_order_relaxed) &&
        g_projection_generation.load(std::memory_order_acquire) != 0;
}

std::uint64_t projection_generation()
{
    return g_projection_generation.load(std::memory_order_acquire);
}

std::size_t projection_matrix(float *out, std::size_t capacity, std::uint64_t *camera_ptr)
{
    if (out == nullptr || capacity < 16)
        return 0;
    std::lock_guard lock(g_projection_mutex);
    std::memcpy(out, g_projection_matrix, sizeof(g_projection_matrix));
    if (camera_ptr != nullptr)
        *camera_ptr = g_projection_camera;
    return 16;
}

} // namespace il2cpp_callsite
