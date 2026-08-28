// Il2CppCallSiteHookTest.cpp - standalone test for the il2cpp callsite hook.
// Allocates two executable pages, plants fake FFX_FSR2 Render/UpdateCommandBuffer
// prologues, then exercises observe mode (pass-through + count) and skip mode
// (early return + count), plus shutdown restore. ASCII-only.
#include "Il2CppCallSiteHook.h"

#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{
constexpr std::uint8_t k_render_head[12] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57, 0x55, 0x53};
constexpr std::uint8_t k_ucb_head[8] = {0x56, 0x57, 0x55, 0x53, 0x48, 0x83, 0xEC, 0x28};
constexpr std::uint8_t k_camera_head[14] = {
    0x41, 0x57, 0x41, 0x56, 0x56, 0x57, 0x53,
    0x48, 0x81, 0xEC, 0x40, 0x01, 0x00, 0x00,
};
// body right after the 12-byte head. The head pushes 8 regs (64 bytes of stack),
// so the body must restore RSP before ret (mirrors a real epilogue):
//   mov dword ptr [rcx], 0x11223344 ; add rsp, 0x40 ; ret
constexpr std::uint8_t k_body[] = {0xC7, 0x01, 0x44, 0x33, 0x22, 0x11, 0x48, 0x83, 0xC4, 0x40, 0xC3};
constexpr std::uint8_t k_camera_body[] = {
    0xC7, 0x01, 0x88, 0x77, 0x66, 0x55,             // mov dword ptr [rcx], 0x55667788
    0x48, 0x81, 0xC4, 0x40, 0x01, 0x00, 0x00,       // add rsp, 0x140
    0x5B, 0x5F, 0x5E, 0x41, 0x5E, 0x41, 0x5F, 0xC3, // pop saved regs; ret
};

int failures = 0;
#define CHECK(cond, msg)                                                    \
    do                                                                      \
    {                                                                       \
        if (!(cond))                                                        \
        {                                                                   \
            std::printf("FAIL: %s\n", msg);                                 \
            ++failures;                                                     \
        }                                                                   \
        else                                                                \
        {                                                                   \
            std::printf("ok:   %s\n", msg);                                 \
        }                                                                   \
    } while (0)

std::uint8_t *g_base = nullptr;
std::uint8_t *g_render = nullptr;
std::uint8_t *g_ucb = nullptr;
std::uint8_t *g_camera = nullptr;
std::uint32_t g_magic = 0;

void setup_pages()
{
    const std::size_t page_size = 0x2000;
    g_base = static_cast<std::uint8_t *>(VirtualAlloc(nullptr, page_size * 2, MEM_COMMIT | MEM_RESERVE,
                                                      PAGE_EXECUTE_READWRITE));
    // render at page2 start (rva 0x2000 relative to g_base); ucb at render-0x70
    g_render = g_base + page_size;
    g_ucb = g_render - 0x70;
    g_camera = g_base + 0x1000;
    std::memcpy(g_render, k_render_head, sizeof(k_render_head));
    std::memcpy(g_render + sizeof(k_render_head), k_body, sizeof(k_body));
    std::memcpy(g_ucb, k_ucb_head, sizeof(k_ucb_head));
    std::memcpy(g_camera, k_camera_head, sizeof(k_camera_head));
    std::memcpy(g_camera + sizeof(k_camera_head), k_camera_body, sizeof(k_camera_body));
}

using render_fn = void (*)(void *this_ptr, void *context);

void run_test(bool skip_mode)
{
    il2cpp_callsite::Config cfg;
    cfg.skip_render = skip_mode;
    cfg.render_rva = 0x2000;
    cfg.update_cmd_buffer_rva = 0x2000 - 0x70;
    const std::uint64_t exe_base = reinterpret_cast<std::uint64_t>(g_base);
    const bool installed = il2cpp_callsite::install(exe_base, cfg);
    CHECK(installed, "install returns true");
    CHECK(il2cpp_callsite::active(), "active() true after install");

    // 假 FFX_FSR2 实例（字段布局：jitter Vector2@0x24, frameIndex@0x50,
    // renderSize Vector2Int@0x58, displaySize Vector2Int@0x68）
    std::uint8_t instance[0x80] {};
    *reinterpret_cast<float *>(instance + 0x24) = 0.25f;
    *reinterpret_cast<float *>(instance + 0x28) = -0.125f;
    *reinterpret_cast<std::uint32_t *>(instance + 0x50) = 42;
    *reinterpret_cast<std::uint32_t *>(instance + 0x58) = 960;
    *reinterpret_cast<std::uint32_t *>(instance + 0x5C) = 540;
    *reinterpret_cast<std::uint32_t *>(instance + 0x68) = 1920;
    *reinterpret_cast<std::uint32_t *>(instance + 0x6C) = 1080;

    const std::uint64_t gen_before = il2cpp_callsite::params_generation();
    const std::uint64_t calls_before = il2cpp_callsite::render_call_count();
    const render_fn fn = reinterpret_cast<render_fn>(g_render);
    fn(instance, nullptr);
    const std::uint64_t calls_after = il2cpp_callsite::render_call_count();
    CHECK(calls_after == calls_before + 1, "render call counted once");
    CHECK(il2cpp_callsite::params_generation() == gen_before + 1, "params generation advanced");

    il2cpp_callsite::CapturedParams cp {};
    const bool got = il2cpp_callsite::last_params(cp);
    CHECK(got, "last_params returns true");
    CHECK(cp.render_w == 960 && cp.render_h == 540, "render size captured");
    CHECK(cp.display_w == 1920 && cp.display_h == 1080, "display size captured");
    CHECK(cp.frame_index == 42, "frame index captured");
    CHECK(cp.jitter_x == 0.25f && cp.jitter_y == -0.125f, "jitter captured");

    if (skip_mode)
        CHECK(*reinterpret_cast<std::uint32_t *>(instance + 0x00) == 0, "skip mode: original body NOT executed");
    else
        CHECK(*reinterpret_cast<std::uint32_t *>(instance + 0x00) == 0x11223344,
              "observe mode: original body executed (pass-through)");

    il2cpp_callsite::shutdown();
    CHECK(!il2cpp_callsite::active(), "active() false after shutdown");
    CHECK(std::memcmp(g_render, k_render_head, sizeof(k_render_head)) == 0, "shutdown restores original bytes");
}

void run_verify_failure()
{
    // wrong RVA: install must fail without touching memory
    const std::uint64_t exe_base = reinterpret_cast<std::uint64_t>(g_base);
    il2cpp_callsite::Config cfg;
    cfg.render_rva = 0x2000 + 0x80; // points mid-function, head mismatch
    cfg.update_cmd_buffer_rva = 0x2000 - 0x70;
    const bool installed = il2cpp_callsite::install(exe_base, cfg);
    CHECK(!installed, "install fails on prologue mismatch");
    CHECK(!il2cpp_callsite::active(), "not active after failed install");
    CHECK(std::memcmp(g_render, k_render_head, sizeof(k_render_head)) == 0, "memory untouched after failed install");
    il2cpp_callsite::shutdown();
}

void run_camera_hook_test()
{
    il2cpp_callsite::Config cfg;
    cfg.camera_rva = 0x1000;
    const std::uint64_t exe_base = reinterpret_cast<std::uint64_t>(g_base);
    CHECK(il2cpp_callsite::install_camera(exe_base, cfg), "camera: install returns true");

    std::uint32_t instance_magic = 0;
    float camera[64] {};
    camera[0] = 1.25f;
    camera[17] = -3.5f;
    using camera_fn = void (*)(void *this_ptr, void *camera_ptr);
    const camera_fn fn = reinterpret_cast<camera_fn>(g_camera);
    fn(&instance_magic, camera);
    CHECK(instance_magic == 0x55667788, "camera: original body executed with preserved rcx");
    CHECK(il2cpp_callsite::camera_ready(), "camera: capture became ready");
    float captured[64] {};
    const std::size_t n = il2cpp_callsite::camera_floats(captured, 64);
    CHECK(n == 64 && captured[0] == 1.25f && captured[17] == -3.5f,
          "camera: rdx data captured and preserved");

    il2cpp_callsite::shutdown();
    CHECK(std::memcmp(g_camera, k_camera_head, sizeof(k_camera_head)) == 0,
          "camera: shutdown restores full 14-byte prologue");
}
} // namespace

void run_per_instance_test()
{
    // 2026-08-25：多实例隔离——两个假实例各自 Render，验证 per-instance 参数/代次互不污染
    // （修复前：全局代次 + 全局参数，UI 实例 Render 会污染主实例 dispatch）。
    std::uint8_t inst_a[0x80] {};
    *reinterpret_cast<float *>(inst_a + 0x24) = 0.25f;
    *reinterpret_cast<float *>(inst_a + 0x28) = -0.125f;
    *reinterpret_cast<std::uint32_t *>(inst_a + 0x50) = 42;
    *reinterpret_cast<std::uint32_t *>(inst_a + 0x58) = 960;
    *reinterpret_cast<std::uint32_t *>(inst_a + 0x5C) = 540;
    *reinterpret_cast<std::uint32_t *>(inst_a + 0x68) = 1920;
    *reinterpret_cast<std::uint32_t *>(inst_a + 0x6C) = 1080;

    std::uint8_t inst_b[0x80] {};
    *reinterpret_cast<float *>(inst_b + 0x24) = 0.5f;
    *reinterpret_cast<float *>(inst_b + 0x28) = 0.125f;
    *reinterpret_cast<std::uint32_t *>(inst_b + 0x50) = 7;
    *reinterpret_cast<std::uint32_t *>(inst_b + 0x58) = 1280;
    *reinterpret_cast<std::uint32_t *>(inst_b + 0x5C) = 720;
    *reinterpret_cast<std::uint32_t *>(inst_b + 0x68) = 2560;
    *reinterpret_cast<std::uint32_t *>(inst_b + 0x6C) = 1440;

    il2cpp_callsite::Config cfg;
    cfg.skip_render = true;
    cfg.render_rva = 0x2000;
    cfg.update_cmd_buffer_rva = 0x2000 - 0x70;
    const std::uint64_t exe_base = reinterpret_cast<std::uint64_t>(g_base);
    const bool installed = il2cpp_callsite::install(exe_base, cfg);
    CHECK(installed, "per-instance: install returns true");

    const render_fn fn = reinterpret_cast<render_fn>(g_render);
    fn(inst_a, nullptr);
    fn(inst_b, nullptr);
    fn(inst_a, nullptr);

    // per-instance params 相互隔离
    il2cpp_callsite::CapturedParams pa {};
    il2cpp_callsite::CapturedParams pb {};
    std::uint64_t ga = 0, gb = 0;
    const std::uint64_t a_ptr = reinterpret_cast<std::uint64_t>(inst_a);
    const std::uint64_t b_ptr = reinterpret_cast<std::uint64_t>(inst_b);
    CHECK(il2cpp_callsite::last_params_for(a_ptr, pa, ga), "per-instance: last_params_for(A) true");
    CHECK(il2cpp_callsite::last_params_for(b_ptr, pb, gb), "per-instance: last_params_for(B) true");
    CHECK(pa.frame_index == 42 && pb.frame_index == 7, "per-instance: frame index isolated");
    CHECK(pa.render_w == 960 && pb.render_w == 1280, "per-instance: render size isolated");
    CHECK(pa.jitter_x == 0.25f && pb.jitter_x == 0.5f, "per-instance: jitter isolated");

    // 代次：A 渲染两次（第 1、3 次）、B 一次（第 2 次）→ A 代次更高且都 > 0
    CHECK(ga > gb && gb > 0, "per-instance: generations isolated (A newer than B)");
    CHECK(il2cpp_callsite::params_generation_for(a_ptr) == ga, "per-instance: generation_for(A) matches");
    CHECK(il2cpp_callsite::params_generation_for(b_ptr) == gb, "per-instance: generation_for(B) matches");

    // known_instances 返回两个实例
    std::uint64_t insts[8] {};
    std::size_t n = 0;
    il2cpp_callsite::known_instances(insts, 8, n);
    CHECK(n == 2, "per-instance: known_instances count == 2");
    bool found_a = false, found_b = false;
    for (std::size_t i = 0; i < n; ++i)
    {
        if (insts[i] == a_ptr) found_a = true;
        if (insts[i] == b_ptr) found_b = true;
    }
    CHECK(found_a && found_b, "per-instance: both instances listed");

    // 全局兼容接口仍返回"最近一次 Render"（= A 的第二次）
    il2cpp_callsite::CapturedParams gp {};
    const bool got = il2cpp_callsite::last_params(gp);
    CHECK(got && gp.frame_index == 42, "per-instance: global last_params still latest (A)");

    il2cpp_callsite::shutdown();
    CHECK(!il2cpp_callsite::active(), "per-instance: inactive after shutdown");
}

int main()
{
    std::printf("Il2CppCallSiteHookTest\n");
    setup_pages();
    run_verify_failure();
    run_test(false); // observe
    run_test(true);  // skip
    run_per_instance_test();
    run_camera_hook_test();
    VirtualFree(g_base, 0, MEM_RELEASE);
    std::printf(failures == 0 ? "ALL PASS\n" : "FAILURES: %d\n", failures);
    return failures == 0 ? 0 : 1;
}
