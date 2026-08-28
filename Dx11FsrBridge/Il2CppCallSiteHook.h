#pragma once
// Il2CppCallSiteHook.h — 游戏 il2cpp 驱动方法调用点钩子（Phase 1.5 落地）。
// 背景：7.0 的 FFX_FSR2::Render 位于 exe_base + 0x06B59670（经 g_MethodPointers
// 全量匹配 + 字段偏移字节验证，见 D:\Dump\work\fsr2-callsite-70.md）。
// 本模块对 Render 做 12 字节绝对跳转 patch（mov rax,stub; jmp rax）：
//   observe 模式：stub 计数后透传原函数（零行为改变，验证调用点 + 调用频率）；
//   skip 模式：stub 直接返回（实验性：会切断桥 Mode 2 的 jitter/触发链，勿与
//   Fsr2TranslationMode=2 同开；仅用于隔离测试）。
// 安装前验证 Render/UpdateCommandBuffer 序言字节，不匹配则放弃（回退 draw 层家族跳过）。

#include <cstddef>
#include <cstdint>

namespace il2cpp_callsite
{
struct Config
{
    bool skip_render = false;
    std::uint32_t render_rva = 0x06B59670;            // FFX_FSR2::Render (7.0)
    std::uint32_t update_cmd_buffer_rva = 0x06B59600; // UpdateCommandBuffer (布局校验用)
    bool capture_camera = false;                      // 2026-08-26：抓取游戏真实相机参数
    std::uint32_t camera_rva = 0x06B558D0;            // ConfigureJitteredProjectionMatrix (7.0，每帧被 OnPreCull 调用)
    bool capture_projection = false;                  // 只读抓取已构造的 jittered projection matrix
    std::uint32_t projection_setter_rva = 0x013DC510; // Camera setter: rcx=Camera*, rdx=Matrix4x4*
};

// stub 从 FFX_FSR2 实例捕获的每帧参数（字段偏移来自 6.6/6.7 dump，getter 字节已实证）
struct CapturedParams
{
    std::uint64_t instance = 0; // this 指针（区分多实例/多相机渲染）
    std::uint64_t context = 0;  // Render 第二参数（渲染上下文，可能含相机参数）
    std::uint32_t render_w = 0, render_h = 0;
    std::uint32_t display_w = 0, display_h = 0;
    std::uint32_t frame_index = 0;
    float jitter_x = 0.0f, jitter_y = 0.0f;
};

// One concrete FFX_FSR2::Render invocation.  The draw hook must consume a
// token, not guess from a mutable "latest parameters" snapshot.
struct RenderToken
{
    CapturedParams params {};
    std::uint64_t generation = 0;
    std::uint64_t capture_tick = 0;
};

// 安装钩子。失败时原代码未改动（调用方应回退到 draw 层家族跳过）。
bool install(std::uint64_t exe_base, const Config &cfg);
// 还原原字节并释放 stub（DLL_PROCESS_DETACH 调用）。
void shutdown();
bool active();
// observe/skip 模式下 stub 累计的 Render 调用次数。
std::uint64_t render_call_count();
// 参数捕获：每次 Render 调用（stub 透传时）更新；返回捕获代数（桥据此判断新鲜度）。
std::uint64_t params_generation();
bool last_params(CapturedParams &out);
// 2026-08-25（多实例修复）：按实例存储参数——主/UI 实例的 Render 各自更新自己的
// 参数与代次，避免 UI 实例污染主实例的 dispatch（参数错用 + 代次门控阻塞 = 交替输出）。
// instance=0 时返回全局最新（兼容旧调用方）。
bool last_params_for(std::uint64_t instance, CapturedParams &out, std::uint64_t &generation);
// Return the newest unconsumed Render token for this exact instance and render
// size.  A successful SDK dispatch must subsequently retire that generation
// and every older pending token for the same instance.
bool latest_pending_render_token_for(std::uint64_t instance,
                                     std::uint32_t render_w, std::uint32_t render_h,
                                     RenderToken &out);
bool consume_render_token_for(std::uint64_t instance, std::uint64_t generation);
// 已记录的实例列表（用于 draw→实例匹配）。
void known_instances(std::uint64_t *out, std::size_t capacity, std::size_t &count);
// 上一帧 jitter 需按实例隔离：桥用 instance 区分 jdelay 状态，故提供按实例的帧代次辅助。
std::uint64_t params_generation_for(std::uint64_t instance);

// 2026-08-26（AA 修复）：相机参数捕获——ConfigureJitteredProjectionMatrix(camera)
// 每帧把本帧 jitter 写进相机投影矩阵（OnPreCull 调用，rdx=camera 结构体指针）。
// 抓取其结构体前 64 个 float（512B），用于确定游戏真实 near/far/FOV（RE 未确定）。
bool install_camera(std::uint64_t exe_base, const Config &cfg);
bool camera_ready();
std::uint64_t camera_generation();
std::size_t camera_floats(float *out, std::size_t capacity); // 拷贝捕获到的相机前段浮点

// 在 ConfigureJitteredProjectionMatrix 的已确认 setter 入口观察最终 4x4 矩阵。
// 只复制 setter 的 rdx 入参，绝不修改 Camera 或矩阵内容。
bool install_projection_setter(std::uint64_t exe_base, const Config &cfg);
bool projection_ready();
std::uint64_t projection_generation();
std::size_t projection_matrix(float *out, std::size_t capacity, std::uint64_t *camera_ptr);
} // namespace il2cpp_callsite
