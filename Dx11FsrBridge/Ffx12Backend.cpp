#include "Ffx12Backend.h"

#include <Windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3d11on12.h>
#include <d3dcompiler.h>
#include <d3dx12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "ffx_api.h"
#include "ffx_upscale.h"
#include "dx12/ffx_api_dx12.h"

#include <atomic>
#include <array>
#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using namespace Microsoft::WRL;

namespace ffx12
{
namespace
{

std::atomic_bool g_active { false };
std::mutex g_mutex;
std::wstring g_sdk_path;
// 实际装载的 SDK 路径（与 g_sdk_path 区分——g_sdk_path 会被路由覆盖，
// 而句柄可能来自 preload 的旧路径；装载路径不一致时必须释放重载，否则版本列表错配）。
std::wstring g_sdk_loaded_path;
// preload 候选缓存（提前装载防 OptiScaler LoadLibrary hook 劫持——
// 实测 402c 文件名同样被劫持 err=18）。init_locked 按最终路径从缓存取句柄，不再走 LoadLibrary。
struct PreloadEntry
{
    std::wstring path;
    HMODULE module;
};
std::vector<PreloadEntry> g_preloaded;

ComPtr<ID3D11Device> g_d11dev;
ComPtr<ID3D11On12Device2> g_on12dev;
ComPtr<ID3D12Device> g_d12dev;
ComPtr<ID3D12CommandQueue> g_queue;
ComPtr<ID3D12CommandAllocator> g_allocator;
ComPtr<ID3D12GraphicsCommandList> g_cmdlist;
ComPtr<ID3D12Fence> g_fence;
HANDLE g_fence_event = nullptr;
UINT64 g_fence_value = 0;
bool g_dx11on12_available = false;
bool g_gpu_only_transport_available = false;
bool g_uses_on12_queue = false;

// The CPU bridge owns one command list and therefore must wait before each
// reuse.  The On12 path returns resources to the translation layer with a
// fence, so it uses a small ring instead: CPU never waits unless it laps the
// GPU by all ring entries.
struct On12CommandSlot
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    UINT64 fence_value = 0;
};
std::array<On12CommandSlot, 3> g_on12_command_slots {};
std::uint32_t g_on12_command_slot_cursor = 0;
std::atomic_uint64_t g_on12_direct_dispatch_count { 0 };

// ffx-api runtime entry points.  Keeping the module loaded for the lifetime of
// every context is required because the provider owns the temporal history.
HMODULE g_sdk_module = nullptr;
struct RuntimeFns
{
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxConfigure configure = nullptr;
};
RuntimeFns g_runtime {};
std::uint64_t g_sdk_version_id = 0;

// 每实例独立 FSR context（全部实例接管，防历史串流污染）
struct SdkContext
{
    ffxContext ctx = nullptr;
    bool created = false;
    std::uint64_t key = 0;
    std::uint32_t rw = 0, rh = 0, dw = 0, dh = 0;
    bool first = true;
    std::uint64_t last_use = 0;
};
std::vector<SdkContext> g_sdk_ctxs;
std::string g_version_name = "ffx12";
// 实际匹配到的 provider 版本名（如 "4.1.1"/"4.0.2c"/"3.1.5"）——日志如实上报。
std::string g_sdk_matched_name = "?";
// 版本判定简化为大版本前缀（"2."/"3."/"4."），SDK 更新（4.1.1→4.2.x 等）免适配。
std::string g_sdk_version_prefix = "2.";
std::uint64_t g_d11_luid = 0;  // D3D11 适配器 LUID（诊断）
std::uint64_t g_d12_luid = 0;  // D3D12 适配器 LUID（诊断）
float g_velocity_factor = 1.0f; // FSR2 高亮像素时间稳定性（0 改善高亮边缘闪烁）

// ---- AMD SDK 消息回调（fpMessage）----
std::wstring g_sdk_messages;
std::mutex g_sdk_msg_mutex;

// ---- 管线诊断：中间纹理中央像素（readback 采集）----
struct DebugPixel
{
    float v[4] = {0, 0, 0, 0};
};
DebugPixel g_debug_pixels[4]; // 0=PQ解码(color_linear) 1=ffxDispatch(output_linear) 2=PQ编码(输出共享) 3=motion解码(motion_cvt)
std::mutex g_debug_px_mutex;
std::atomic_uint64_t g_dispatch_counter { 0 };
bool g_debug_layer = false;           // D3D12 debug layer + info queue
std::uint32_t g_dump_frames = 0;      // 2026-08-25：dump 前 N 帧 FSR4 输出（诊断边缘抖动）
ComPtr<ID3D12InfoQueue> g_info_queue;
void sdk_message_cb(std::uint32_t type, const wchar_t *message)
{
    if (message == nullptr)
        return;
    try
    {
        std::lock_guard lock(g_sdk_msg_mutex);
        if (g_sdk_messages.size() < 8192)
        {
            if (!g_sdk_messages.empty())
                g_sdk_messages += L" | ";
            g_sdk_messages += (type == FFX_API_MESSAGE_TYPE_ERROR ? L"[ERR] " :
                               type == FFX_API_MESSAGE_TYPE_WARNING ? L"[WARN] " : L"[INFO] ");
            g_sdk_messages += message;
        }
    }
    catch (...)
    {
    }
}

// 共享纹理池（D3D11 侧，legacy SHARED；D3D12 侧 OpenSharedHandle）
struct SharedTex
{
    ComPtr<ID3D11Texture2D> d11;
    ComPtr<ID3D12Resource> d12;
    HANDLE handle = nullptr;
    std::uint32_t w = 0, h = 0;
};
SharedTex g_tex_color, g_tex_depth, g_tex_motion, g_tex_output;
std::uint32_t g_render_w = 0, g_render_h = 0;
std::uint32_t g_display_w = 0, g_display_h = 0;
bool g_last_reset = false;
std::uint64_t g_ctx_recreates = 0;
constexpr int kFfxDispatchNotReached = -2147483647;
std::atomic_int g_last_ffx_dispatch_rc { kFfxDispatchNotReached };
DXGI_FORMAT g_input_color_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
DXGI_FORMAT g_input_depth_fmt = DXGI_FORMAT_R32_FLOAT;
DXGI_FORMAT g_input_motion_fmt = DXGI_FORMAT_R16G16_FLOAT;
DXGI_FORMAT g_input_transparency_fmt = DXGI_FORMAT_R8_UNORM;
DXGI_FORMAT g_input_output_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;

// ---- ：原生 D3D11 纯 GPU 传输（NT 共享句柄 + D3D11.4 共享 fence）----
// 设计依据（推翻 2026-08-24"legacy 共享纹理在 D3D12 读为 0"结论）：
//   D3D11_RESOURCE_MISC_SHARED（legacy 共享面）句柄 D3D12 无法正确打开 → 读零；
//   正确机制 = D3D11_RESOURCE_MISC_SHARED_NTHANDLE + IDXGIResource1::CreateSharedHandle
//   + ID3D12Device::OpenSharedHandle + D3D11.4 ID3D11Fence 共享同步。
//   GpuInteropProbe 在本机（RX 9070 XT，LUID 0x123A3，游戏同适配器）已字节级验证：
//   R8G8B8A8/R10G10B10A2_TYPELESS/R16G16_FLOAT/R32_FLOAT 双向 GPU 流转全 PASS；
//   D3D12→D3D11 直接导入不支持（生产不需要）。OptiScaler/D3DSharingTests 佐证
//   D3D11→D3D12 方向为跨 API 共享的普遍可行方向。
bool g_gpu_interop = false;        // 配置请求
bool g_gpu_interop_ready = false;  // 初始化成功（D3D11.4 + 共享 fence + CS 编译）
ComPtr<ID3D11Device5> g_d11_5;
ComPtr<ID3D11DeviceContext4> g_game_ctx4; // 缓存的游戏 immediate context（首次 dispatch 时 QI）
ComPtr<ID3D12Fence> g_shared_fence;       // 共享 fence（D3D12 创建）
ComPtr<ID3D11Fence> g_shared_fence11;     // 同一 fence 的 D3D11 视图
HANDLE g_shared_fence_handle = nullptr;
std::uint64_t g_shared_fence_value = 0;   // 单调（D3D11/D3D12 共用）
// 深度提取：游戏 R32G8X24_TYPELESS 不可共享 → D3D11 CS 提取到共享 R32_FLOAT（FFX 期望格式）
ComPtr<ID3D11ComputeShader> g_depth_extract_cs;
SharedTex g_tex_depth_share {};           // R32_FLOAT 共享深度（CS 输出/UAV）
ComPtr<ID3D11UnorderedAccessView> g_depth_share_uav;
ComPtr<ID3D11ShaderResourceView> g_depth_src_srv; // 游戏深度的 R32_FLOAT SRV（按纹理缓存）
ID3D11Texture2D *g_depth_src_last = nullptr;
// D3D11 侧 motion 解码（金丝雀证实原 D3D12 decode pass 从不写 cvt）
ComPtr<ID3D11ComputeShader> g_motion_decode_cs;   // D3D11 decode compute
ComPtr<ID3D11ComputeShader> g_motion_decode_dz_cs; // ：静止死区变体
bool g_motion_deadzone = false;                  // =true 用死区变体（微小 motion 归零）
SharedTex g_tex_motion_cvt_share {};              // R16G16_FLOAT 共享解码输出（D3D12 直读给 FFX）
ComPtr<ID3D11UnorderedAccessView> g_motion_cvt_share_uav;
ComPtr<ID3D11ShaderResourceView> g_motion_src_srv; // 游戏 motion R10G10B10A2 SRV（按纹理缓存）
ID3D11Texture2D *g_motion_src_last = nullptr;
// reactive（motion B 通道）GPU 化——D3D11 CS 同 pass 输出共享 R8_UNORM
SharedTex g_tex_reactive_share {};                // R8_UNORM 共享 reactive（D3D12 直读给 FFX）
ComPtr<ID3D11UnorderedAccessView> g_reactive_share_uav;

// 运行期配置（桥 load_config 时设置；启动后固定）
bool g_depth_inverted = true;   // 游戏深度逆方向（0=far）—— 2026-08-23 采样验证
bool g_decode_motion = true;    // 游戏 motion 为 R10G10B10A2 平方编码
float g_motion_flip = 1.0f;     // ：XeSS/DLSS 定向——motion 方向翻转（默认 +1 = FSR 方向）
float g_depth_scale = 1.0f;     // ：XeSS/DLSS 定向——depth 值域归一化（XeSS 期望 [0,1]）
ComPtr<ID3D11Buffer> g_motion_cb;   // MotionParams 常数缓冲（b0: g_flip）
bool g_motion_vectors_jittered = false; // 游戏配置：motion 是否已包含投影 jitter
bool g_hdr_input = true;        // 游戏 10-bit HDR 管线（useRealType）
bool g_auto_exposure = true;    // 自动曝光（OptiScaler 日志 initFlags 实证：AutoExposure=true）
bool g_non_linear = true;       // 非线性色彩空间（OptiScaler 日志：FsrNonLinearColorSpace=true）
bool g_use_pq_chain = false;    // PQ 链开关（2026-08-24 定案：游戏原生直喂 PQ 值 + HDR|NON_LINEAR|AUTO_EXPOSURE
                                // 标志（initFlags 0x129），不做 PqToLinear/LinearToPq；本开关保留旧链作对照）

// half → float（readback 诊断用）
static float debug_half_to_float(std::uint16_t h)
{
    const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000u) << 16;
    const std::uint32_t exp = (h >> 10) & 0x1Fu;
    const std::uint32_t mant = h & 0x3FFu;
    std::uint32_t f = 0;
    if (exp == 0)
        f = sign | (mant << 13);
    else if (exp == 31)
        f = sign | 0x7F800000u | (mant << 13);
    else
        f = sign | ((exp + 112u) << 23) | (mant << 13);
    float out = 0.0f;
    std::memcpy(&out, &f, 4);
    return out;
}

static std::uint16_t float_to_half(float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint32_t sign = (bits >> 16) & 0x8000u;
    const std::uint32_t exp = (bits >> 23) & 0xffu;
    const std::uint32_t mant = bits & 0x7fffffu;
    if (exp == 0xffu)
        return static_cast<std::uint16_t>(sign | 0x7c00u | (mant ? 0x0200u : 0));
    int e = static_cast<int>(exp) - 127 + 15;
    if (e <= 0)
    {
        if (e < -10)
            return static_cast<std::uint16_t>(sign);
        const std::uint32_t m = mant | 0x800000u;
        return static_cast<std::uint16_t>(sign | ((m >> (14 - e)) + ((m >> (13 - e)) & 1u)));
    }
    if (e >= 31)
        return static_cast<std::uint16_t>(sign | 0x7c00u);
    return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(e) << 10) |
                                       ((mant + 0x1000u) >> 13));
}

static const std::uint16_t *motion_decode_lut()
{
    static const std::array<std::uint16_t, 1024> lut = [] {
        std::array<std::uint16_t, 1024> values {};
        for (std::uint32_t i = 0; i < 1024; ++i)
        {
            const float c = static_cast<float>(i) / 1023.0f;
            const float d = c - 0.498039f;
            const float v = (d < 0.0f ? 1.0f : (d > 0.0f ? -1.0f : 0.0f)) * 4.0f * d * d;
            values[i] = float_to_half(v);
        }
        return values;
    }();
    return lut.data();
}

// 读取 D3D12 info queue 消息（诊断；定义在尾部，前向声明供 dispatch 使用）
static void drain_info_queue();

// 读取 D3D12 info queue 消息并追加到 SDK 消息缓冲（诊断）
static void drain_info_queue()
{
    if (!g_info_queue)
        return;
    try
    {
        const UINT64 n = g_info_queue->GetNumStoredMessages();
        if (n == 0)
            return;
        for (UINT64 i = 0; i < n && i < 64; ++i) // P0-3 可观测性：原 8 条截断，扩到 64
        {
            SIZE_T len = 0;
            g_info_queue->GetMessage(static_cast<UINT>(i), nullptr, &len);
            if (len == 0)
                continue;
            std::vector<std::uint8_t> buf(len);
            D3D12_MESSAGE *msg = reinterpret_cast<D3D12_MESSAGE *>(buf.data());
            if (SUCCEEDED(g_info_queue->GetMessage(static_cast<UINT>(i), msg, &len)) && msg->pDescription)
            {
                std::wstring wmsg;
                const int wlen = MultiByteToWideChar(CP_UTF8, 0, msg->pDescription, -1, nullptr, 0);
                if (wlen > 1)
                {
                    wmsg.resize(static_cast<std::size_t>(wlen) - 1);
                    MultiByteToWideChar(CP_UTF8, 0, msg->pDescription, -1, wmsg.data(), wlen);
                }
                else
                {
                    wmsg = L"<d3d12 msg>";
                }
                std::lock_guard lock(g_sdk_msg_mutex);
                if (g_sdk_messages.size() < 8192)
                {
                    if (!g_sdk_messages.empty())
                        g_sdk_messages += L" | ";
                    g_sdk_messages += L"[D3D12] ";
                    g_sdk_messages += wmsg;
                }
            }
        }
        g_info_queue->ClearStoredMessages();
    }
    catch (...)
    {
    }
}

// ---- motion 解码 compute pass（R10G10B10A2 平方编码 → R16G16_FLOAT）----
// 游戏 FSR2 accumulate shader（0x78057A29AF6C2D99）的 motion 解码（反汇编实证）：
//   d = raw - 0.498039;  mv = -sign(d) * 4 * d^2     （平方编码：小运动压缩、大运动放大）
// 之前用线性解码 c.rg*2-1 在中速运动时补偿约 5 倍过大 → 历史采样超前 → 边缘闪烁。
// mvscale 保持负号（游戏重投影 = UV - mv；FSR2 = UV + mv*scale，两者经 scale 归一化后等价）。
static const char *g_mv_decode_hlsl = R"(
RWTexture2D<float2> out_mv : register(u0);
Texture2D<float4> in_mv  : register(t0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    #if defined(FFX12_MOTION_DECODE_TEST)
    out_mv[id.xy] = float2(0.5, 0.5); // 金丝雀：常数输出，忽略输入——判 pass 是否执行
    #else
    float4 c = in_mv.Load(int3(id.xy, 0));
    float2 d = c.rg - 0.498039;
    out_mv[id.xy] = -sign(d) * (4.0 * d * d);
    #endif
}
)";

// ---- ：深度提取 compute（游戏 R32G8X24_TYPELESS → 共享 R32_FLOAT）----
// R32G8X24 不能建 D3D11 SHARED/NTHANDLE 共享纹理（CreateTexture2D E_INVALIDARG，probe 实证），
// 因此必须先在 D3D11 侧把深度提取为可共享的 R32_FLOAT（恰好是 FFX12 期望的深度 SRV 格式）。
// 提取在游戏的 immediate context 上执行（注入 compute pass，样式同旧 Mode-2 翻译层）。
// XeSS/DLSS 定向——depth 值域归一化（XeSS 期望 [0,1]；游戏逆深度实际 [0,~0.03]，
// 不归一化则 XeSS 深度感知/低分辨率 MV 上采样失真 → 视角移动残影。b0: g_depth_scale，XeSS 时 >1）。
static const char *g_depth_extract_hlsl = R"(
cbuffer DepthParams : register(b0)
{
    float g_depth_scale;
    float3 g_pad;
};
Texture2D<float4> in_depth : register(t0);
RWTexture2D<float> out_depth : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    out_depth[id.xy] = in_depth.Load(int3(id.xy, 0)).x * g_depth_scale;
}
)";

// ---- ：D3D11 侧 motion 解码（对称于深度提取）----
// 金丝雀（MotionDecodeTest）证明原 D3D12 解码 pass 从未写 cvt（On12/GPU-interop 均如此），
// 静态 AA 靠 jitter 相位累积、动态全废的历史原因找到。此处改在游戏原生 D3D11 设备上
// 用 compute 直接解码：t0=游戏 motion（R10G10B10A2 平方编码）→ u0=共享 R16G16_FLOAT
// （probe 已验证该格式可跨 API 共享；与深度提取同机制，写入确定性可控）。
static const char *g_motion_decode_d11_hlsl = R"(
cbuffer MotionParams : register(b0)
{
    float g_flip; // ：XeSS/DLSS 定向——motion 方向翻转（XeSS-SR 约定 prev→curr，与 FSR 相反）
    float3 g_pad;
};
Texture2D<float4> in_mv : register(t0);
RWTexture2D<float2> out_mv : register(u0);
RWTexture2D<float> out_reactive : register(u1);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    float4 c = in_mv.Load(int3(id.xy, 0));
    float2 d = c.rg - 0.498039;
    float2 mv = -sign(d) * (4.0 * d * d) * g_flip;
    #if defined(FFX12_MOTION_DEADZONE)
    // 静止死区——|mv| 低于阈值归零，让 FSR 进入静止锁定（lock），
    // 消除静止时残余 motion 驱动的时间累积网格（竖纹/云刺）。阈值 0.0005 UV ≈ 1px@1920。
    static const float2 k_dz = float2(0.0005, 0.0005);
    if (abs(mv.x) < k_dz.x && abs(mv.y) < k_dz.y)
        mv = float2(0.0, 0.0);
    #endif
    out_mv[id.xy] = mv;
    out_reactive[id.xy] = c.b; // motion B 通道（bits 20..29）= reactive 源（历史第 62 轮实证）
}
)";
ComPtr<ID3D12Resource> g_tex_motion_cvt;        // R16G16_FLOAT render 尺寸（UAV）
ComPtr<ID3D12Resource> g_up_motion_cvt;         // CPU 解码后的 R16G16_FLOAT upload buffer
void *g_up_motion_cvt_ptr = nullptr;
UINT g_up_motion_cvt_pitch = 0;
ComPtr<ID3D12DescriptorHeap> g_mv_heap;         // [SRV(motion源), UAV(cvt)]，shader-visible
UINT g_mv_heap_inc = 0;
ComPtr<ID3D12RootSignature> g_mv_rs;
ComPtr<ID3D12PipelineState> g_mv_pso;
ComPtr<ID3D12PipelineState> g_mv_pso_test; // 金丝雀：常数输出 0.5（诊断 pass 是否执行）
bool g_motion_decode_test = false;
D3D12_RESOURCE_STATES g_motion_src_state = D3D12_RESOURCE_STATE_COMMON;  // 共享源（D3D11 互操作）

// ---- PQ 颜色空间（游戏 color 为 HDR10 PQ 编码，FSR2 需线性 HDR）----
// 输入：PqToLinear 解码 → FSR2（HDR flag）；输出：LinearToPq 编码 → r1。
ComPtr<ID3D12Resource> g_tex_color_linear;     // R16G16B16A16_FLOAT render 尺寸（PQ 解码结果）
ComPtr<ID3D12Resource> g_tex_output_linear;    // R16G16B16A16_FLOAT display 尺寸（FSR2 线性输出）
ComPtr<ID3D12DescriptorHeap> g_pq_in_heap;     // [SRV(color源), UAV(color_linear)]
ComPtr<ID3D12DescriptorHeap> g_pq_out_heap;    // [SRV(output_linear), UAV(输出共享)]
ComPtr<ID3D12RootSignature> g_pq_rs;           // 通用 SRV+UAV 根签名
ComPtr<ID3D12PipelineState> g_pq_decode_pso;   // PqToLinear
ComPtr<ID3D12PipelineState> g_pq_decode_test_pso; // PqToLinear 常数注入测试（0.5）
ComPtr<ID3D12PipelineState> g_pq_encode_pso;   // LinearToPq
ComPtr<ID3D12PipelineState> g_pq_encode_mark_pso; // LinearToPq + 输出标记（诊断）
bool g_output_mark = false;                    // 输出标记开关（诊断）
bool g_decode_test = false;                    // 解码常数注入测试（诊断：判断 pass 执行 vs SRV 读）
D3D12_RESOURCE_STATES g_color_linear_state = D3D12_RESOURCE_STATE_COMMON;
D3D12_RESOURCE_STATES g_output_linear_state = D3D12_RESOURCE_STATE_COMMON;
UINT g_pq_heap_inc = 0;

// ---- 输出落地修复（P0-1 根因，2026-08-24 逆向实证）----
// 游戏侧累计读回全 0 的根因：g_tex_output 是 D3D11 legacy SHARED（非 NT handle）纹理，
// 在 D3D12 侧仅支持 COMMON 状态访问；原先 PQ 编码 pass 将其 barrier 到 UNORDERED_ACCESS
// 做 UAV 写 → 写不落地（连 OutputMark 无条件红标记区都读 0，执行链审计通过 → 写目标异常）。
// 修复：PQ 编码改写到 D3D12 自有 UAV 纹理 g_tex_output_enc（合法）→ readback buffer →
// CPU → D3D11 UpdateSubresource 写入共享纹理 g_tex_output.d11（跨 API 无状态风险）。
ComPtr<ID3D12Resource> g_tex_output_enc;        // D3D12 自有，格式=共享输出格式（UAV 合法）
ComPtr<ID3D12Resource> g_output_readback;       // READBACK heap（rowPitch×height）
UINT g_output_readback_row_pitch = 0;
D3D12_RESOURCE_STATES g_output_enc_state = D3D12_RESOURCE_STATE_COMMON;
DXGI_FORMAT g_output_enc_format = DXGI_FORMAT_R8G8B8A8_UNORM;
// E1 诊断：D3D12 自有输出 5 点采样（区分"链不渲染" vs "写共享失败"）
struct OutputSamples
{
    std::uint32_t raw[5] = {};
    std::uint32_t w = 0, h = 0;
    bool valid = false;
};
OutputSamples g_out_samples;
std::mutex g_out_samples_mutex;

// ---- 链中点二分采样（2026-08-24 enc 全 0 + sdk_msgs=none → 定位 D3D12 链断点）----
// 4 个 D3D12 自有纹理各采样中央像素：
//   motion_cvt（金丝雀：motion 输入非 0 → 若 cvt≠0 则 cmdlist 执行 + 自有纹理 UAV 写正常）
//   color_linear（PQ 解码输出：若=0 → 解码 pass 或共享输入 SRV 读断裂）
//   output_linear（ffxDispatch 输出：若=0 → 派发断裂）
//   enc（PQ 编码输出：若=0 → 编码 pass 断裂；mark=1 时 p0 应为红）
struct ChainSamples
{
    float motion_cvt[2] = {0, 0};    // R16G16_FLOAT 中央
    float color_linear[3] = {0, 0, 0}; // R16G16B16A16_FLOAT 中央（half→float）
    float output_linear[3] = {0, 0, 0};
    std::uint32_t canary = 0;        // 金丝雀 64×64 R8G8B8A8 中央原始值（0xFFC08040=写成功）
    // 自有输入纹理中央（验证 CPU 中转是否送达 D3D12 链）
    std::uint32_t color_own = 0;     // R10G10B10A2 中央原始值（PQ 颜色 ≈0x2xxxxxxx 非 0）
    float depth_own = 0.0f;          // R32_FLOAT 中央（游戏逆深度 0=far）
    std::uint32_t motion_own = 0;    // R10G10B10A2 中央原始值
    bool valid = false;
};
ChainSamples g_chain_samples;
ComPtr<ID3D12Resource> g_rb_motion_cvt;    // readback（render 尺寸 R16G16_FLOAT）
ComPtr<ID3D12Resource> g_rb_color_linear;  // readback（render 尺寸 R16G16B16A16_FLOAT）
ComPtr<ID3D12Resource> g_rb_output_linear; // readback（display 尺寸 R16G16B16A16_FLOAT）
ComPtr<ID3D12Resource> g_rb_color_own;     // readback（render 尺寸 R10G10B10A2）
ComPtr<ID3D12Resource> g_rb_depth_own;     // readback（render 尺寸 R32_FLOAT）
ComPtr<ID3D12Resource> g_rb_motion_own;    // readback（render 尺寸 R10G10B10A2）
UINT g_rb_motion_cvt_pitch = 0;
UINT g_rb_color_linear_pitch = 0;
UINT g_rb_output_linear_pitch = 0;
UINT g_rb_color_own_pitch = 0;
UINT g_rb_depth_own_pitch = 0;
UINT g_rb_motion_own_pitch = 0;

// 版本标记 pass（2026-08-24：全程可见的 SDK 版本标记，画在 enc 上，与窗口无关）：
// 边框 24px + 左上角块 110px，颜色编码版本（2.3.4 红 / 3.1.5 绿 / 4.1.1 蓝）。
static const char *g_marker_hlsl = R"(
RWTexture2D<float4> img : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint2 dims;
    img.GetDimensions(dims.x, dims.y);
    const uint bw = 24u;
    const uint cb = 110u;
    bool border = (id.x < bw) || (id.y < bw) || (id.x >= dims.x - bw) || (id.y >= dims.y - bw);
    bool corner = (id.x < cb) && (id.y < cb);
    if (border || corner)
    {
        #if defined(FFX12_MARKER_315)
            img[id.xy] = float4(0.0, 1.0, 0.0, 1.0);
        #elif defined(FFX12_MARKER_411)
            img[id.xy] = float4(0.0, 0.0, 1.0, 1.0);
        #else
            img[id.xy] = float4(1.0, 0.0, 0.0, 1.0);
        #endif
    }
}
)";
ComPtr<ID3D12PipelineState> g_marker_234_pso;
ComPtr<ID3D12PipelineState> g_marker_315_pso;
ComPtr<ID3D12PipelineState> g_marker_411_pso;
ComPtr<ID3D12DescriptorHeap> g_marker_heap;
D3D12_RESOURCE_STATES g_marker_enc_state = D3D12_RESOURCE_STATE_COMMON;

// 根据选中的版本返回对应标记 PSO
ID3D12PipelineState *marker_pso_for_version()
{
    if (g_version_name == "3.1.5")
        return g_marker_315_pso.Get();
    if (g_version_name == "4.1.1")
        return g_marker_411_pso.Get();
    return g_marker_234_pso.Get();
}
// 版本标记堆创建（起 CPU/On12/GPU-interop 共用；定义在
// ensure_output_landing_resources 之后）。enc 为标记目标纹理的 D3D12 侧。
bool ensure_marker_heap_for(ID3D12Resource *enc);

// ---- 金丝雀（2026-08-24 ）：常数 compute 写 64×64 自有纹理 ----
// 不依赖任何输入；读回 0xFFC08040（R=64,G=128,B=192,A=255 小端）= cmdlist 执行 + 自有 UAV + readback 全通。
static const char *g_canary_hlsl = R"(
RWTexture2D<float4> out_c : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    out_c[id.xy] = float4(0.25, 0.5, 0.75, 1.0);
}
)";
ComPtr<ID3D12Resource> g_canary_tex;       // 64×64 R8G8B8A8_UNORM（UAV）
ComPtr<ID3D12Resource> g_canary_rb;        // readback
ComPtr<ID3D12DescriptorHeap> g_canary_heap; // [UAV(canary)]
ComPtr<ID3D12PipelineState> g_canary_pso;
D3D12_RESOURCE_STATES g_canary_state = D3D12_RESOURCE_STATE_COMMON;
UINT g_canary_pitch = 0;

// enc 清除金丝雀：编码前 ClearUnorderedAccessViewFloat(enc, 灰) ——读回灰色=clear 执行但编码未写；
// 红色（mark）=编码写了；0=clear 都没执行（cmdlist 空转/未执行）。
D3D12_GPU_DESCRIPTOR_HANDLE g_pq_out_uav_gpu {};
D3D12_CPU_DESCRIPTOR_HANDLE g_pq_out_uav_cpu {};
ComPtr<ID3D12DescriptorHeap> g_pq_out_cpu_heap; // 非 shader-visible（CPU 可读）：clear 的 CPU 句柄必须来自此类堆

// ---- 输入 CPU 中转（2026-08-24 legacy 共享 SRV 读返回 0 实证）----
// 金丝雀证明自有纹理 UAV/SRV/readback 全通；解码输出全 0 的唯一解释 = 共享纹理在 D3D12 读为 0。
// 修复：输入不再经 D3D12 读共享纹理——D3D11 staging 读游戏输入（samples 已证明可行）
// → CPU → D3D12 upload buffer → CopyTextureRegion 到自有输入纹理 → 链全部消费自有纹理。
// 深度特别处理（2026-08-24 ）：R32G8X24_TYPELESS 的 D3D12 拷贝 footprint 为 4BPP
// （深度平面；G8X24 是独立 stencil 平面）——若按 8BPP 行距 memcpy 会越界写（已实证 AV）。
// 深度走提取路径：staging 保持 R32G8X24（匹配游戏），memcpy 每像素取前 4B 深度 → R32_FLOAT
// 自有纹理/上传（4BPP，与 D3D12 footprint 一致）。
ComPtr<ID3D12Resource> g_tex_color_own;      // 自有输入（R10G10B10A2，游戏格式）
ComPtr<ID3D12Resource> g_tex_depth_own;      // 自有输入（R32_FLOAT，深度提取后 4BPP）
ComPtr<ID3D12Resource> g_tex_motion_own;     // 自有输入（R10G10B10A2，游戏格式）
// reactive 只由 CPU 从 motion.z 提取并作为 SRV 上传；不要再接入 motion decode 的 UAV/root-signature 路径。
ComPtr<ID3D12Resource> g_tex_reactive_own;   // R8_UNORM，render-size
ComPtr<ID3D12Resource> g_tex_transparency_own; // R8_UNORM，render-size
ComPtr<ID3D12Resource> g_up_color, g_up_depth, g_up_motion, g_up_reactive, g_up_transparency; // upload buffers
void *g_up_color_ptr = nullptr, *g_up_depth_ptr = nullptr, *g_up_motion_ptr = nullptr,
     *g_up_reactive_ptr = nullptr, *g_up_transparency_ptr = nullptr;
UINT g_up_color_pitch = 0, g_up_depth_pitch = 0, g_up_motion_pitch = 0, g_up_reactive_pitch = 0,
     g_up_transparency_pitch = 0;
ComPtr<ID3D11Texture2D> g_stage_color, g_stage_depth, g_stage_motion, g_stage_transparency; // D3D11 staging（CPU 读）
UINT g_stage_color_pitch = 0, g_stage_depth_pitch = 0, g_stage_motion_pitch = 0, g_stage_transparency_pitch = 0;
D3D12_RESOURCE_STATES g_color_own_state = D3D12_RESOURCE_STATE_COMMON;
D3D12_RESOURCE_STATES g_depth_own_state = D3D12_RESOURCE_STATE_COMMON;
D3D12_RESOURCE_STATES g_motion_own_state = D3D12_RESOURCE_STATE_COMMON;
D3D12_RESOURCE_STATES g_reactive_own_state = D3D12_RESOURCE_STATE_COMMON;
D3D12_RESOURCE_STATES g_transparency_own_state = D3D12_RESOURCE_STATE_COMMON;
// cmdlist 开关状态（N1 良性噪音修复：1004 行防御性 Close 每帧对已关闭列表调 Close → E_FAIL + 调试消息）
bool g_cmdlist_open = false;

// SDK 诊断注记（追加到 g_sdk_messages，随 sdk_msgs 上日志）
static void sdk_note(const wchar_t *fmt, ...)
{
    wchar_t buf[512] {};
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    std::lock_guard lock(g_sdk_msg_mutex);
    if (g_sdk_messages.size() < 8192)
    {
        if (!g_sdk_messages.empty())
            g_sdk_messages += L" | ";
        g_sdk_messages += L"[sdk234] ";
        g_sdk_messages += buf;
    }
}

// ---- 崩溃安全步进日志（fopen/fprintf/fclose 同步写；进程被看门狗/设备移除杀死也不丢）----
static void sdk234_step(const char *step)
{
#if defined(FFX12_DEBUG_STEPS)
    FILE *f = nullptr;
    if (fopen_s(&f, "sdk234_steps.log", "a") == 0 && f)
    {
        SYSTEMTIME st {};
        GetLocalTime(&st);
        fprintf(f, "%02u:%02u:%02u.%03u %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, step);
        fclose(f);
    }
#else
    (void)step;
#endif
}

// 步进日志附参（memcpy 边界等）
static void sdk234_info(const char *fmt, ...)
{
#if defined(FFX12_DEBUG_STEPS)
    FILE *f = nullptr;
    if (fopen_s(&f, "sdk234_steps.log", "a") == 0 && f)
    {
        SYSTEMTIME st {};
        GetLocalTime(&st);
        fprintf(f, "%02u:%02u:%02u.%03u ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fprintf(f, "\n");
        fclose(f);
    }
#else
    (void)fmt;
#endif
}

static LONG WINAPI sdk234_exception_filter(EXCEPTION_POINTERS *ep)
{
    FILE *f = nullptr;
    if (fopen_s(&f, "sdk234_steps.log", "a") == 0 && f)
    {
        const std::uint8_t *addr = static_cast<const std::uint8_t *>(ep->ExceptionRecord->ExceptionAddress);
        const std::uint8_t *base =
            reinterpret_cast<const std::uint8_t *>(GetModuleHandleW(L"Dx11FsrBridge.dll"));
        fprintf(f, "EXCEPTION code=0x%08X addr=%p bridge_offset=0x%llX",
                static_cast<unsigned>(ep->ExceptionRecord->ExceptionCode),
                ep->ExceptionRecord->ExceptionAddress,
                base ? static_cast<unsigned long long>(addr - base) : 0ull);
        if (ep->ExceptionRecord->ExceptionCode == 0xC0000005 &&
            ep->ExceptionRecord->NumberParameters >= 2)
        {
            fprintf(f, " access=%s fault_va=0x%llX",
                    ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "WRITE" : "READ",
                    static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[1]));
        }
        fprintf(f, "\n");
        fclose(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static const char *g_pq_decode_hlsl = R"(
RWTexture2D<float4> out_linear : register(u0);
Texture2D<float4> in_pq     : register(t0);
float3 PqToLinear(float3 value)
{
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    const float3 powered = pow(max(value, 0.0), 1.0 / m2);
    return pow(max(powered - c1, 0.0) / max(c2 - c3 * powered, 1e-6), 1.0 / m1);
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    #if defined(FFX12_DECODE_TEST)
    // 常数注入测试：输出固定 0.5 —— 若 cl 读回 0.5 则 decode pass 执行正常（问题在 SRV 读取）
    out_linear[id.xy] = float4(0.5, 0.5, 0.5, 1.0);
    #else
    float4 c = in_pq.Load(int3(id.xy, 0));
    out_linear[id.xy] = float4(PqToLinear(c.rgb), c.a);
    #endif
}
)";

static const char *g_pq_encode_hlsl = R"(
RWTexture2D<float4> out_pq : register(u0);
Texture2D<float4> in_linear : register(t0);
float3 LinearToPq(float3 value)
{
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    const float3 powered = pow(max(value, 0.0), m1);
    return pow((c1 + c2 * powered) / (1.0 + c3 * powered), m2);
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    float4 c = in_linear.Load(int3(id.xy, 0));
    float3 pq = LinearToPq(c.rgb);
    bool mark = false;
    #if defined(FFX12_OUTPUT_MARK)
    {
        uint2 dims;
        out_pq.GetDimensions(dims.x, dims.y);
        const uint2 center = dims / 2u;
        mark = (id.x < dims.x / 8u && id.y < dims.y / 8u) ||
               (abs(int(id.x) - int(center.x)) < int(dims.x / 16u) &&
                abs(int(id.y) - int(center.y)) < int(dims.y / 16u));
    }
    #endif
    if (mark)
        out_pq[id.xy] = float4(1.0, 0.0, 0.0, 1.0);
    else
        out_pq[id.xy] = float4(LinearToPq(c.rgb * 0.5), c.a); // 整体亮度减半（无法忽视的标记）
}
)";

ComPtr<ID3D11Texture2D> make_shared_texture(UINT w, UINT h, DXGI_FORMAT fmt, UINT bind_flags)
{
    D3D11_TEXTURE2D_DESC d {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind_flags;
    // An On12 game device already shares ownership with g_d12dev through its
    // queue.  Legacy shared handles are not valid/needed there; the texture is
    // only the D3D11 landing target for the transitional CPU path.
    d.MiscFlags = g_uses_on12_queue ? 0u : D3D11_RESOURCE_MISC_SHARED;
    ComPtr<ID3D11Texture2D> t;
    if (FAILED(g_d11dev->CreateTexture2D(&d, nullptr, &t)))
        return nullptr;
    return t;
}

bool open_on_d3d12(SharedTex &st)
{
    if (g_uses_on12_queue)
        return st.d11 != nullptr;
    if (st.d12)
        return true;
    if (!st.d11)
        return false;
    ComPtr<IDXGIResource> dxgi;
    if (FAILED(st.d11.As(&dxgi)))
        return false;
    HANDLE h = nullptr;
    if (FAILED(dxgi->GetSharedHandle(&h)))
        return false;
    st.handle = h;
    if (FAILED(g_d12dev->OpenSharedHandle(h, IID_PPV_ARGS(&st.d12))))
        return false;
    return true;
}

// NT 共享纹理（SHARED|SHARED_NTHANDLE）——D3D12 可正确打开的跨 API
// 共享机制（legacy GetSharedHandle 在 D3D12 读零，见 GpuInteropProbe 实证）。
ComPtr<ID3D11Texture2D> make_shared_texture_nt(UINT w, UINT h, DXGI_FORMAT fmt, UINT bind_flags)
{
    D3D11_TEXTURE2D_DESC d {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind_flags;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    ComPtr<ID3D11Texture2D> t;
    if (FAILED(g_d11dev->CreateTexture2D(&d, nullptr, &t)))
        return nullptr;
    return t;
}

// 用 IDXGIResource1::CreateSharedHandle 打开为 D3D12（NT 共享，正确机制）。
bool open_shared_nt(SharedTex &st)
{
    if (st.d12)
        return true;
    if (!st.d11 || !g_d12dev)
        return false;
    ComPtr<IDXGIResource1> r1;
    if (FAILED(st.d11.As(&r1)))
        return false;
    HANDLE h = nullptr;
    if (FAILED(r1->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h)))
        return false;
    const HRESULT hr = g_d12dev->OpenSharedHandle(h, IID_PPV_ARGS(&st.d12));
    CloseHandle(h);
    return SUCCEEDED(hr) && st.d12;
}

void release_shared_nt(SharedTex &st)
{
    st.d12.Reset();
    st.d11.Reset();
    st.w = st.h = 0;
}

void release_shared(SharedTex &st)
{
    st.d12.Reset();
    st.d11.Reset();
    if (st.handle)
    {
        CloseHandle(st.handle);
        st.handle = nullptr;
    }
    st.w = st.h = 0;
}

void release_motion_decode()
{
    if (g_up_motion_cvt && g_up_motion_cvt_ptr)
        g_up_motion_cvt->Unmap(0, nullptr);
    g_up_motion_cvt_ptr = nullptr;
    g_up_motion_cvt.Reset();
    g_tex_motion_cvt.Reset();
    g_rb_motion_cvt.Reset();
    g_rb_motion_cvt_pitch = 0;
    g_mv_heap.Reset();
    g_motion_src_state = D3D12_RESOURCE_STATE_COMMON;
}

void release_input_staging(); // 定义在尾部（ensure_input_staging 旁）

void release_pq_resources()
{
    g_tex_color_linear.Reset();
    g_tex_output_linear.Reset();
    g_tex_output_enc.Reset();
    g_output_readback.Reset();
    g_output_readback_row_pitch = 0;
    g_rb_color_linear.Reset();
    g_rb_output_linear.Reset();
    g_rb_color_linear_pitch = 0;
    g_rb_output_linear_pitch = 0;
    g_output_enc_state = D3D12_RESOURCE_STATE_COMMON;
    g_pq_in_heap.Reset();
    g_pq_out_heap.Reset();
    g_pq_out_cpu_heap.Reset();
    g_pq_out_uav_gpu.ptr = 0;
    g_pq_out_uav_cpu.ptr = 0;
    g_color_linear_state = D3D12_RESOURCE_STATE_COMMON;
    g_output_linear_state = D3D12_RESOURCE_STATE_COMMON;
    release_input_staging();
    // 金丝雀
    g_canary_tex.Reset();
    g_canary_rb.Reset();
    g_canary_heap.Reset();
    g_marker_heap.Reset();
    g_marker_enc_state = D3D12_RESOURCE_STATE_COMMON;
    g_canary_pitch = 0;
    g_canary_state = D3D12_RESOURCE_STATE_COMMON;
    {
        std::lock_guard lock(g_out_samples_mutex);
        g_out_samples = OutputSamples {};
        g_chain_samples = ChainSamples {};
    }
}

// 为 D3D12 自有纹理建 readback buffer（GetCopyableFootprints 对齐 rowPitch）
bool create_readback_for_texture(ID3D12Resource *tex, ComPtr<ID3D12Resource> &rb, UINT &row_pitch)
{
    if (!tex || !g_d12dev)
        return false;
    D3D12_RESOURCE_DESC desc = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    g_d12dev->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, nullptr);
    const UINT64 size = footprint.Footprint.RowPitch * static_cast<UINT64>(desc.Height);
    row_pitch = footprint.Footprint.RowPitch;
    D3D12_RESOURCE_DESC rd_buf {};
    rd_buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd_buf.Width = size;
    rd_buf.Height = 1;
    rd_buf.DepthOrArraySize = 1;
    rd_buf.MipLevels = 1;
    rd_buf.SampleDesc.Count = 1;
    rd_buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp {};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    return SUCCEEDED(g_d12dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd_buf,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&rb)));
}

// 输入 CPU 中转资源：D3D12 自有输入纹理 + upload buffer + D3D11 staging。
// 格式沿用游戏输入（color/motion R10G10B10A2、depth R32G8X24）；upload 持久 Map。
bool ensure_input_staging()
{
    if (g_tex_color_own && g_tex_depth_own && g_tex_motion_own && g_tex_reactive_own &&
        g_tex_transparency_own && g_up_color && g_up_depth && g_up_motion && g_up_reactive &&
        g_up_transparency && g_stage_color && g_stage_depth && g_stage_motion && g_stage_transparency)
        return true;
    if (g_render_w == 0 || g_render_h == 0 || !g_d12dev || !g_d11dev)
        return false;
    sdk234_step("input_staging begin");
    const DXGI_FORMAT color_fmt = g_input_color_fmt;
    const DXGI_FORMAT depth_fmt = g_input_depth_fmt;
    const DXGI_FORMAT motion_fmt = g_input_motion_fmt;
    const DXGI_FORMAT transparency_fmt = g_input_transparency_fmt;

    const auto make_own = [&](DXGI_FORMAT fmt, ComPtr<ID3D12Resource> &own) -> bool
    {
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = g_render_w;
        rd.Height = g_render_h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = fmt;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_NONE; // 仅 SRV 读
        return SUCCEEDED(g_d12dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                           D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                           IID_PPV_ARGS(&own)));
    };
    const auto make_upload = [&](ID3D12Resource *own, ComPtr<ID3D12Resource> &up,
                                 void *&ptr, UINT &pitch) -> bool
    {
        D3D12_RESOURCE_DESC desc = own->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        g_d12dev->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, nullptr);
        const UINT64 size = fp.Footprint.RowPitch * static_cast<UINT64>(desc.Height);
        pitch = fp.Footprint.RowPitch;
        D3D12_RESOURCE_DESC rd_buf {};
        rd_buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd_buf.Width = size;
        rd_buf.Height = 1;
        rd_buf.DepthOrArraySize = 1;
        rd_buf.MipLevels = 1;
        rd_buf.SampleDesc.Count = 1;
        rd_buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        if (FAILED(g_d12dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd_buf,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&up))))
            return false;
        D3D12_RANGE range {0, 0}; // 只写不读
        return SUCCEEDED(up->Map(0, &range, &ptr));
    };
    // staging 只建纹理，不在创建期 Map（启动期对游戏 context 做探测 Map 是闪退嫌疑；
    // RowPitch 在首次真实拷贝的 Map 里用 m.RowPitch 直接取，无需缓存）
    const auto make_stage = [&](DXGI_FORMAT fmt, ComPtr<ID3D11Texture2D> &stg) -> bool
    {
        D3D11_TEXTURE2D_DESC d {};
        d.Width = g_render_w;
        d.Height = g_render_h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = fmt;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(g_d11dev->CreateTexture2D(&d, nullptr, &stg)))
        {
            sdk_note(L"create staging failed fmt=%d stage=input", static_cast<int>(fmt));
            return false;
        }
        return true;
    };

    // 深度自有纹理用 R32_FLOAT（4BPP）：staging 仍为 R32G8X24（匹配游戏），拷贝时提取深度分量
    if (!make_own(color_fmt, g_tex_color_own) ||
        !make_own(DXGI_FORMAT_R32_FLOAT, g_tex_depth_own) ||
        !make_own(motion_fmt, g_tex_motion_own) ||
        !make_own(DXGI_FORMAT_R8_UNORM, g_tex_reactive_own) ||
        !make_own(DXGI_FORMAT_R8_UNORM, g_tex_transparency_own))
    {
        sdk_note(L"create own input texture failed stage=input");
        return false;
    }
    if (!make_upload(g_tex_color_own.Get(), g_up_color, g_up_color_ptr, g_up_color_pitch) ||
        !make_upload(g_tex_depth_own.Get(), g_up_depth, g_up_depth_ptr, g_up_depth_pitch) ||
        !make_upload(g_tex_motion_own.Get(), g_up_motion, g_up_motion_ptr, g_up_motion_pitch) ||
        !make_upload(g_tex_reactive_own.Get(), g_up_reactive, g_up_reactive_ptr, g_up_reactive_pitch) ||
        !make_upload(g_tex_transparency_own.Get(), g_up_transparency, g_up_transparency_ptr,
                     g_up_transparency_pitch))
    {
        sdk_note(L"create upload buffer failed stage=input");
        return false;
    }
    if (!make_stage(color_fmt, g_stage_color) || !make_stage(depth_fmt, g_stage_depth) ||
        !make_stage(motion_fmt, g_stage_motion) || !make_stage(transparency_fmt, g_stage_transparency))
    {
        sdk_note(L"create staging failed stage=input");
        return false;
    }
    // 自有输入 readback（链输入送达验证）
    if (!create_readback_for_texture(g_tex_color_own.Get(), g_rb_color_own, g_rb_color_own_pitch) ||
        !create_readback_for_texture(g_tex_depth_own.Get(), g_rb_depth_own, g_rb_depth_own_pitch) ||
        !create_readback_for_texture(g_tex_motion_own.Get(), g_rb_motion_own, g_rb_motion_own_pitch))
    {
        sdk_note(L"create own-input readback failed stage=input");
        return false;
    }
    g_color_own_state = g_depth_own_state = g_motion_own_state =
        g_reactive_own_state = g_transparency_own_state = D3D12_RESOURCE_STATE_COMMON;
    sdk234_step("input_staging ok");
    return true;
}

void release_input_staging()
{
    if (g_up_color && g_up_color_ptr)
        g_up_color->Unmap(0, nullptr);
    if (g_up_depth && g_up_depth_ptr)
        g_up_depth->Unmap(0, nullptr);
    if (g_up_motion && g_up_motion_ptr)
        g_up_motion->Unmap(0, nullptr);
    if (g_up_reactive && g_up_reactive_ptr)
        g_up_reactive->Unmap(0, nullptr);
    if (g_up_transparency && g_up_transparency_ptr)
        g_up_transparency->Unmap(0, nullptr);
    g_up_color_ptr = g_up_depth_ptr = g_up_motion_ptr = g_up_reactive_ptr = g_up_transparency_ptr = nullptr;
    g_tex_color_own.Reset();
    g_tex_depth_own.Reset();
    g_tex_motion_own.Reset();
    g_tex_reactive_own.Reset();
    g_tex_transparency_own.Reset();
    g_up_color.Reset();
    g_up_depth.Reset();
    g_up_motion.Reset();
    g_up_reactive.Reset();
    g_up_transparency.Reset();
    g_stage_color.Reset();
    g_stage_depth.Reset();
    g_stage_motion.Reset();
    g_stage_transparency.Reset();
    g_rb_color_own.Reset();
    g_rb_depth_own.Reset();
    g_rb_motion_own.Reset();
    g_rb_color_own_pitch = g_rb_depth_own_pitch = g_rb_motion_own_pitch = 0;
    g_up_color_pitch = g_up_depth_pitch = g_up_motion_pitch = g_up_reactive_pitch =
        g_up_transparency_pitch = 0;
    g_stage_color_pitch = g_stage_depth_pitch = g_stage_motion_pitch = g_stage_transparency_pitch = 0;
    g_color_own_state = g_depth_own_state = g_motion_own_state =
        g_reactive_own_state = g_transparency_own_state = D3D12_RESOURCE_STATE_COMMON;
}

// 输出落地资源（P0-1 修复）：D3D12 自有 UAV 纹理 + readback buffer。
// legacy SHARED 纹理在 D3D12 侧仅 COMMON 状态 → 不能直接 UAV 写；
// 改由自有纹理承接编码/ffxDispatch 写，再 readback → D3D11 UpdateSubresource 落地共享纹理。
// 无条件创建（非 PQ 路径的 ffxDispatch 输出也走它）。
bool ensure_output_landing_resources()
{
    if (g_tex_output_enc && g_output_readback)
        return true;
    if (g_display_w == 0 || g_display_h == 0 || !g_d12dev)
        return false;
    if (g_tex_output_enc)
        g_tex_output_enc.Reset();
    if (g_output_readback)
    {
        g_output_readback.Reset();
        g_output_readback_row_pitch = 0;
    }
    {
        switch (g_input_output_fmt)
        {
            case DXGI_FORMAT_R10G10B10A2_TYPELESS: g_output_enc_format = DXGI_FORMAT_R10G10B10A2_UNORM; break;
            case DXGI_FORMAT_R8G8B8A8_TYPELESS: g_output_enc_format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
            case DXGI_FORMAT_B8G8R8A8_TYPELESS: g_output_enc_format = DXGI_FORMAT_B8G8R8A8_UNORM; break;
            default: g_output_enc_format = g_input_output_fmt; break;
        }
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = g_display_w;
        rd.Height = g_display_h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = g_output_enc_format;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(g_d12dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&g_tex_output_enc))))
            return false;
        // readback buffer（GetCopyableFootprints 对齐 rowPitch）
        D3D12_RESOURCE_DESC enc_desc = g_tex_output_enc->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
        g_d12dev->GetCopyableFootprints(&enc_desc, 0, 1, 0, &footprint, nullptr, nullptr, nullptr);
        const UINT64 buf_size = footprint.Footprint.RowPitch * static_cast<UINT64>(enc_desc.Height);
        g_output_readback_row_pitch = footprint.Footprint.RowPitch;
        D3D12_RESOURCE_DESC rd_buf {};
        rd_buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd_buf.Width = buf_size;
        rd_buf.Height = 1;
        rd_buf.DepthOrArraySize = 1;
        rd_buf.MipLevels = 1;
        rd_buf.SampleDesc.Count = 1;
        rd_buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES hp_ro {};
        hp_ro.Type = D3D12_HEAP_TYPE_READBACK;
        if (FAILED(g_d12dev->CreateCommittedResource(&hp_ro, D3D12_HEAP_FLAG_NONE, &rd_buf,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&g_output_readback))))
            return false;
        g_output_enc_state = D3D12_RESOURCE_STATE_COMMON;
    }
    // 金丝雀：64×64 R8G8B8A8_UNORM 自有纹理 + readback + UAV 堆（执行链证明）
    if (!g_canary_tex)
    {
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = 64;
        rd.Height = 64;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(g_d12dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&g_canary_tex))))
            return false;
        if (!create_readback_for_texture(g_canary_tex.Get(), g_canary_rb, g_canary_pitch))
            return false;
        D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 2; // 根签名声明 SRV(t0)+UAV(u0) 两 range：t0 放 canary 自身 SRV（shader 不用），u0 放 UAV
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_d12dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_canary_heap))))
            return false;
        const UINT inc = g_d12dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu0 = g_canary_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE cpu1 = cpu0;
        cpu1.ptr += inc;
        // UAV 在前（本驱动描述符表 UAV range → offset 0）；SRV(canary)@1（两者同资源，兼容两种映射）
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
        uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        g_d12dev->CreateUnorderedAccessView(g_canary_tex.Get(), nullptr, &uav, cpu0);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        g_d12dev->CreateShaderResourceView(g_canary_tex.Get(), &srv, cpu1);
        g_canary_state = D3D12_RESOURCE_STATE_COMMON;
    }
    // 版本标记堆：[UAV(enc)@0, SRV(enc)@1]（UAV 在前，本驱动映射；SRV 供表布局占位，shader 不用）
    if (!ensure_marker_heap_for(g_tex_output_enc.Get()))
        return false;
    return true;
}

// 版本标记堆创建（起供 CPU / On12 / GPU-interop 三路径共用）。
// heap 内按 g_mv_rs root signature 布局：[SRV(enc)@0, UAV(enc)@1]（t0=desc0/u0=desc1）。
// 旧实现把 UAV 放 0、SRV 放 1 与 g_mv_rs 相反，u0 会绑定到 SRV 描述符 → 驱动层崩溃
// （进场闪退根因；该标记路径此前从未被真实启用过）。
// 调用方在画标记时把 UAV 覆盖为当前输出纹理。
bool ensure_marker_heap_for(ID3D12Resource *enc)
{
    if (g_marker_heap)
        return true;
    if (!enc || !g_d12dev)
        return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_d12dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_marker_heap))))
        return false;
    const UINT inc = g_d12dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu0 = g_marker_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE cpu1 = cpu0;
    cpu1.ptr += inc;
    // desc0 = t0（SRV 占位，marker shader 不用；root table 引用必须有效）
    D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.Format = g_output_enc_format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g_d12dev->CreateShaderResourceView(enc, &srv, cpu0);
    // desc1 = u0（UAV，marker shader 的 RWTexture2D img）
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
    uav.Format = g_output_enc_format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_d12dev->CreateUnorderedAccessView(enc, nullptr, &uav, cpu1);
    g_marker_enc_state = D3D12_RESOURCE_STATE_COMMON;
    return true;
}

bool ensure_pq_resources()
{
    if (g_tex_color_linear && g_tex_output_linear && g_pq_in_heap && g_pq_out_heap)
        return true;
    if (g_render_w == 0 || g_render_h == 0 || g_display_w == 0 || g_display_h == 0 || !g_d12dev)
        return false;
    if (!g_tex_output_enc || !g_output_readback)
        return false; // 落地资源必须先于 PQ 堆创建（UAV 指向 enc）
    // color_linear：R16G16B16A16_FLOAT render 尺寸（UAV）
    {
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = g_render_w;
        rd.Height = g_render_h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(g_d12dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&g_tex_color_linear))))
            return false;
    }
    // output_linear：R16G16B16A16_FLOAT display 尺寸（UAV）
    {
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = g_display_w;
        rd.Height = g_display_h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(g_d12dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&g_tex_output_linear))))
            return false;
    }
    // 链中点采样 readback（color_linear / output_linear）
    if (!create_readback_for_texture(g_tex_color_linear.Get(), g_rb_color_linear, g_rb_color_linear_pitch))
        return false;
    if (!create_readback_for_texture(g_tex_output_linear.Get(), g_rb_output_linear, g_rb_output_linear_pitch))
        return false;
    g_pq_heap_inc = g_d12dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // PQ 解码堆：[UAV(color_linear)@0, SRV(color_own)@1]
    // 2026-08-24 离线复现定案：本驱动（AMD RDNA4）描述符表把 UAV range 排到 offset 0
    // （与声明顺序无关）——堆内描述符必须 UAV 在前，否则 pass 写到 SRV 槽位资源（co=0.5 灰实证）。
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 2;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_d12dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_pq_in_heap))))
            return false;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu0 = g_pq_in_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE cpu1 = cpu0;
        cpu1.ptr += g_pq_heap_inc;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
        uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        g_d12dev->CreateUnorderedAccessView(g_tex_color_linear.Get(), nullptr, &uav, cpu0);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        g_d12dev->CreateShaderResourceView(g_tex_color_own.Get(), &srv, cpu1);
    }
    // PQ 编码堆：[UAV(输出落地纹理 g_tex_output_enc)@0, SRV(output_linear)@1]
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 2;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_d12dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_pq_out_heap))))
            return false;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu0 = g_pq_out_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE cpu1 = cpu0;
        cpu1.ptr += g_pq_heap_inc;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
        uav.Format = g_output_enc_format;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        g_d12dev->CreateUnorderedAccessView(g_tex_output_enc.Get(), nullptr, &uav, cpu0);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        g_d12dev->CreateShaderResourceView(g_tex_output_linear.Get(), &srv, cpu1);
        // enc 清除金丝雀句柄（UAV 在 offset 0；CPU 句柄必须来自非 shader-visible 堆）
        g_pq_out_uav_cpu = cpu0;
        g_pq_out_uav_gpu = g_pq_out_heap->GetGPUDescriptorHandleForHeapStart(); // offset 0 = UAV
        // CPU 句柄必须来自非 shader-visible（CPU 可读）堆：另建 1 描述符堆放同一 UAV
        if (!g_pq_out_cpu_heap)
        {
            D3D12_DESCRIPTOR_HEAP_DESC hd_cpu {};
            hd_cpu.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            hd_cpu.NumDescriptors = 1;
            hd_cpu.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            if (FAILED(g_d12dev->CreateDescriptorHeap(&hd_cpu, IID_PPV_ARGS(&g_pq_out_cpu_heap))))
                return false;
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav_cpu {};
            uav_cpu.Format = g_output_enc_format;
            uav_cpu.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            g_d12dev->CreateUnorderedAccessView(g_tex_output_enc.Get(), nullptr, &uav_cpu,
                                                g_pq_out_cpu_heap->GetCPUDescriptorHandleForHeapStart());
        }
    }
    return true;
}

bool ensure_motion_decode_resources()
{
    if (g_tex_motion_cvt && g_mv_heap && (g_uses_on12_queue || (g_up_motion_cvt && g_up_motion_cvt_ptr)))
        return true;
    if (g_render_w == 0 || g_render_h == 0 || !g_d12dev)
        return false;
    // cvt：R16G16_FLOAT，render 尺寸，UAV
    D3D12_HEAP_PROPERTIES heap_props {};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC res_desc {};
    res_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    res_desc.Width = g_render_w;
    res_desc.Height = g_render_h;
    res_desc.DepthOrArraySize = 1;
    res_desc.MipLevels = 1;
    res_desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    res_desc.SampleDesc.Count = 1;
    res_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(g_d12dev->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &res_desc,
                                                 D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&g_tex_motion_cvt))))
        return false;
    if (!g_uses_on12_queue)
    {
        // Legacy interop needs this CPU decoded upload.  The On12 branch below
        // reads the unwrapped game motion texture in the GPU decode pass.
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        g_d12dev->GetCopyableFootprints(&res_desc, 0, 1, 0, &fp, nullptr, nullptr, nullptr);
        g_up_motion_cvt_pitch = fp.Footprint.RowPitch;
        D3D12_RESOURCE_DESC ub {};
        ub.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        ub.Width = fp.Footprint.RowPitch * static_cast<UINT64>(g_render_h);
        ub.Height = 1;
        ub.DepthOrArraySize = 1;
        ub.MipLevels = 1;
        ub.SampleDesc.Count = 1;
        ub.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES uhp {};
        uhp.Type = D3D12_HEAP_TYPE_UPLOAD;
        if (FAILED(g_d12dev->CreateCommittedResource(&uhp, D3D12_HEAP_FLAG_NONE, &ub,
                                                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                      IID_PPV_ARGS(&g_up_motion_cvt))))
            return false;
        D3D12_RANGE ur {0, 0};
        if (FAILED(g_up_motion_cvt->Map(0, &ur, &g_up_motion_cvt_ptr)) || !g_up_motion_cvt_ptr)
            return false;
    }
    // 链中点采样 readback（金丝雀：motion 输入非 0 → cvt 非 0 即证明 cmdlist 执行 + 自有 UAV 写正常）
    if (!create_readback_for_texture(g_tex_motion_cvt.Get(), g_rb_motion_cvt, g_rb_motion_cvt_pitch))
        return false;
    // 描述符堆：[SRV(motion源), UAV(cvt)]，shader-visible
    D3D12_DESCRIPTOR_HEAP_DESC hd {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_d12dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_mv_heap))))
        return false;
    g_mv_heap_inc = g_d12dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu0 = g_mv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE cpu1 = cpu0;
    cpu1.ptr += g_mv_heap_inc;
    // 描述符顺序必须与 root signature ranges 一致：ranges[0]=SRV(t0) → offset 0，ranges[1]=UAV(u0) → offset 1。
    // （2026-08-25 修复：此前 UAV@0/SRV@1 是反的 → u0 写到 motion 源、t0 读到从未写入的 cvt
    //   纹理 → FSR2 motion 输入全垃圾 → 重投影失败 → 累积死 → 无 AA；mv_cvt 读回 -65504 实证。）
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
    uav.Format = DXGI_FORMAT_R16G16_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    // On12 descriptors are filled with the per-frame unwrapped resource.
    // A null descriptor here is intentional until the first direct dispatch.
    g_d12dev->CreateShaderResourceView(g_tex_motion_own.Get(), &srv, cpu0);   // SRV(motion 源)@0
    g_d12dev->CreateUnorderedAccessView(g_tex_motion_cvt.Get(), nullptr, &uav, cpu1); // UAV(cvt)@1
    return true;
}

bool ensure_pool(const FrameInput &input)
{
    const bool pool_ok =
        (g_uses_on12_queue ? g_tex_output_enc != nullptr : g_tex_color.d11 != nullptr) &&
        (!g_gpu_interop_ready ||
         (g_tex_motion.d11 != nullptr && g_tex_depth_share.d11 != nullptr && g_tex_output.d11 != nullptr)) &&
        g_render_w == input.render_w && g_render_h == input.render_h &&
        g_display_w == input.display_w && g_display_h == input.display_h;
    if (pool_ok)
        return true;
    release_shared(g_tex_color);
    release_shared(g_tex_depth);
    release_shared(g_tex_motion);
    release_shared(g_tex_output);
    release_shared_nt(g_tex_depth_share);
    release_shared_nt(g_tex_motion_cvt_share);
    release_shared_nt(g_tex_reactive_share);
    g_depth_share_uav.Reset();
    g_motion_cvt_share_uav.Reset();
    g_reactive_share_uav.Reset();
    g_depth_src_srv.Reset();
    g_depth_src_last = nullptr;
    g_motion_src_srv.Reset();
    g_motion_src_last = nullptr;
    g_render_w = g_render_h = g_display_w = g_display_h = 0;

    // 池格式 = 输入纹理的实际格式（CopyResource 要求格式一致；游戏多为 TYPELESS）
    DXGI_FORMAT color_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_FORMAT depth_fmt = DXGI_FORMAT_R32_FLOAT;
    DXGI_FORMAT motion_fmt = DXGI_FORMAT_R16G16_FLOAT;
    DXGI_FORMAT transparency_fmt = DXGI_FORMAT_R8_UNORM;
    DXGI_FORMAT output_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D11_TEXTURE2D_DESC td {};
    if (input.color && (input.color->GetDesc(&td), td.Format != DXGI_FORMAT_UNKNOWN))
        color_fmt = td.Format;
    if (input.depth && (input.depth->GetDesc(&td), td.Format != DXGI_FORMAT_UNKNOWN))
        depth_fmt = td.Format;
    if (input.motion && (input.motion->GetDesc(&td), td.Format != DXGI_FORMAT_UNKNOWN))
        motion_fmt = td.Format;
    if (input.transparency && (input.transparency->GetDesc(&td), td.Format != DXGI_FORMAT_UNKNOWN))
        transparency_fmt = td.Format;
    if (input.output_target && (input.output_target->GetDesc(&td), td.Format != DXGI_FORMAT_UNKNOWN))
        output_fmt = td.Format;

    // 原生 D3D11 纯 GPU 传输池（NT 共享句柄纹理；无 CPU staging）。
    // 游戏保持原生 D3D11 设备；自建 D3D12 设备经共享句柄读取同一 GPU 内存。
    if (g_gpu_interop_ready)
    {
        g_input_color_fmt = color_fmt;
        g_input_depth_fmt = depth_fmt;
        g_input_motion_fmt = motion_fmt;
        g_input_transparency_fmt = transparency_fmt;
        g_input_output_fmt = output_fmt;
        g_render_w = input.render_w;
        g_render_h = input.render_h;
        g_display_w = input.display_w;
        g_display_h = input.display_h;
        // 共享输入（游戏同格式；颜色/运动 R10G10B10A2_TYPELESS，probe 验证可共享）
        g_tex_color.d11 = make_shared_texture_nt(input.render_w, input.render_h, color_fmt,
                                                 D3D11_BIND_SHADER_RESOURCE);
        g_tex_motion.d11 = make_shared_texture_nt(input.render_w, input.render_h, motion_fmt,
                                                  D3D11_BIND_SHADER_RESOURCE);
        // 共享深度：R32_FLOAT（游戏 R32G8X24 不可共享 → 每帧 D3D11 CS 提取到这里）
        g_tex_depth_share.d11 = make_shared_texture_nt(input.render_w, input.render_h,
                                                       DXGI_FORMAT_R32_FLOAT,
                                                       D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
        // 共享解码 motion（R16G16_FLOAT；D3D11 CS 解码写 → D3D12 直读给 FFX）
        g_tex_motion_cvt_share.d11 = make_shared_texture_nt(input.render_w, input.render_h,
                                                            DXGI_FORMAT_R16G16_FLOAT,
                                                            D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
        // 共享 reactive（R8_UNORM；motion B 通道提取，同 CS 输出）
        g_tex_reactive_share.d11 = make_shared_texture_nt(input.render_w, input.render_h,
                                                          DXGI_FORMAT_R8_UNORM,
                                                          D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
        // 共享输出（FFX UAV 输出目标；随后 D3D11 GPU CopyResource 回游戏输出）
        g_tex_output.d11 = make_shared_texture_nt(input.display_w, input.display_h, output_fmt,
                                                  D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
        g_tex_color.w = g_tex_motion.w = g_tex_depth_share.w = g_tex_motion_cvt_share.w = g_tex_reactive_share.w = input.render_w;
        g_tex_color.h = g_tex_motion.h = g_tex_depth_share.h = g_tex_motion_cvt_share.h = g_tex_reactive_share.h = input.render_h;
        g_tex_output.w = input.display_w;
        g_tex_output.h = input.display_h;
        if (!g_tex_color.d11 || !g_tex_motion.d11 || !g_tex_depth_share.d11 ||
            !g_tex_motion_cvt_share.d11 || !g_tex_reactive_share.d11 || !g_tex_output.d11)
            return false;
        if (!open_shared_nt(g_tex_color) || !open_shared_nt(g_tex_motion) ||
            !open_shared_nt(g_tex_depth_share) || !open_shared_nt(g_tex_motion_cvt_share) ||
            !open_shared_nt(g_tex_reactive_share) || !open_shared_nt(g_tex_output))
            return false;
        // 共享深度 UAV（CS 输出目标；纹理固定，视图建一次）
        if (!g_depth_share_uav)
        {
            D3D11_UNORDERED_ACCESS_VIEW_DESC uav {};
            uav.Format = DXGI_FORMAT_R32_FLOAT;
            uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            if (FAILED(g_d11dev->CreateUnorderedAccessView(g_tex_depth_share.d11.Get(), &uav,
                                                           &g_depth_share_uav)) ||
                !g_depth_share_uav)
                return false;
        }
        // 共享解码 motion UAV（D3D11 CS 输出目标）
        if (!g_motion_cvt_share_uav)
        {
            D3D11_UNORDERED_ACCESS_VIEW_DESC uav {};
            uav.Format = DXGI_FORMAT_R16G16_FLOAT;
            uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            if (FAILED(g_d11dev->CreateUnorderedAccessView(g_tex_motion_cvt_share.d11.Get(), &uav,
                                                           &g_motion_cvt_share_uav)) ||
                !g_motion_cvt_share_uav)
                return false;
        }
        // 共享 reactive UAV（同 CS 的 u1 输出目标）
        if (!g_reactive_share_uav)
        {
            D3D11_UNORDERED_ACCESS_VIEW_DESC uav {};
            uav.Format = DXGI_FORMAT_R8_UNORM;
            uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            if (FAILED(g_d11dev->CreateUnorderedAccessView(g_tex_reactive_share.d11.Get(), &uav,
                                                           &g_reactive_share_uav)) ||
                !g_reactive_share_uav)
                return false;
        }
        // FFX 输出格式映射（UAV 不能用 TYPELESS 格式，须用 typed 变体；与
        // ensure_output_landing_resources 同规则）。资源本体保持游戏 Typeless 以便
        // D3D11 CopyResource(游戏输出 ← 共享输出) 格式一致。
        g_output_enc_format = output_fmt;
        if (output_fmt == DXGI_FORMAT_R10G10B10A2_TYPELESS)
            g_output_enc_format = DXGI_FORMAT_R10G10B10A2_UNORM;
        else if (output_fmt == DXGI_FORMAT_R8G8B8A8_TYPELESS)
            g_output_enc_format = DXGI_FORMAT_R8G8B8A8_UNORM;
        else if (output_fmt == DXGI_FORMAT_B8G8R8A8_TYPELESS)
            g_output_enc_format = DXGI_FORMAT_B8G8R8A8_UNORM;
        g_motion_src_state = D3D12_RESOURCE_STATE_COMMON;
        if (g_use_pq_chain)
        {
            sdk_note(L"gpu interop path rejects legacy PQ conversion stage=pool");
            return false;
        }
        return true;
    }

    // 清理：CPU staging 回退路径已移除——GPU 互操作未就绪时直接失败，
    // 由 dispatch 返回 false 让游戏走原有原生 FSR 路径（fail-open 不损画质）。
    sdk_note(L"gpu interop not ready stage=pool");
    return false;
}

void wait_gpu()
{
    ++g_fence_value;
    g_queue->Signal(g_fence.Get(), g_fence_value);
    if (g_fence->GetCompletedValue() < g_fence_value)
    {
        g_fence->SetEventOnCompletion(g_fence_value, g_fence_event);
        WaitForSingleObject(g_fence_event, INFINITE);
    }
}

void destroy_context(SdkContext &sc)
{
    if (sc.created && g_runtime.destroy != nullptr)
        g_runtime.destroy(&sc.ctx, nullptr);
    sc.ctx = nullptr;
    sc.created = false;
}

// 按 instance_key 查找/创建 SdkContext（上限 4 个，LRU 驱逐）
SdkContext *ctx_for_key(std::uint64_t key)
{
    for (SdkContext &sc : g_sdk_ctxs)
    {
        if (sc.created && sc.key == key)
        {
            sc.last_use = GetTickCount64();
            return &sc;
        }
    }
    // 新实例：优先复用已销毁的空槽，否则 LRU 驱逐最旧
    SdkContext *slot = nullptr;
    for (SdkContext &sc : g_sdk_ctxs)
    {
        if (!sc.created && sc.key == 0)
        {
            slot = &sc;
            break;
        }
    }
    if (!slot && g_sdk_ctxs.size() < 4)
    {
        g_sdk_ctxs.emplace_back();
        slot = &g_sdk_ctxs.back();
    }
    if (!slot)
    {
        // 驱逐最久未用
        std::uint64_t oldest = ~0ull;
        for (SdkContext &sc : g_sdk_ctxs)
        {
            if (sc.last_use < oldest)
            {
                oldest = sc.last_use;
                slot = &sc;
            }
        }
        if (slot && slot->created)
        {
            destroy_context(*slot);
            ++g_ctx_recreates;
        }
    }
    if (!slot)
        return nullptr;
    slot->key = key;
    slot->last_use = GetTickCount64();
    slot->rw = slot->rh = slot->dw = slot->dh = 0;
    slot->first = true;
    return slot;
}

bool create_context(SdkContext &sc)
{
    if (sc.created)
        return true;
    if (g_render_w == 0 || g_render_h == 0 || g_display_w == 0 || g_display_h == 0)
        return false;

    if (!g_runtime.create || g_sdk_version_id == 0)
        return false;

    ffxCreateBackendDX12Desc backend {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = g_d12dev.Get();

    ffxOverrideVersion override_version {};
    override_version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    override_version.versionId = g_sdk_version_id;
    backend.header.pNext = &override_version.header;

    ffxCreateContextDescUpscale desc {};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.header.pNext = &backend.header;
    desc.maxRenderSize = {g_render_w, g_render_h};
    desc.maxUpscaleSize = {g_display_w, g_display_h};
    desc.fpMessage = sdk_message_cb;
    // 游戏深度为逆深度（0=far，透视编码）：必须翻转 FSR2 的深度假设，否则
    // disocclusion/depth-clip 全反 → 历史每帧被拒 → 输出退化为单帧放大（无 AA + 抖动）。
    // 色彩/曝光：参照 OptiScaler 实测 initFlags 0x129（HDR|DEPTH_INVERTED|AUTO_EXPOSURE|NON_LINEAR_COLORSPACE）。
    desc.flags = (g_depth_inverted ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0u) |
                 (g_hdr_input ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE : 0u) |
                 (g_auto_exposure ? FFX_UPSCALE_ENABLE_AUTO_EXPOSURE : 0u) |
                 (g_non_linear ? FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE : 0u) |
                 // 仅当输入 motion 已包含投影 jitter 时启用抵消；否则该标志会对
                 // 未带 jitter 的矢量重复施加补偿，表现为静止画面细微抖动。
                 (g_motion_vectors_jittered ? FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION : 0u);
    const ffxReturnCode_t rc = g_runtime.create(
        &sc.ctx, reinterpret_cast<ffxCreateContextDescHeader *>(&desc), nullptr);
    if (rc != FFX_API_RETURN_OK || sc.ctx == nullptr)
    {
        sdk_note(L"ffxFsr2ContextCreate rc=%d version=%s stage=ctx",
                 static_cast<int>(rc), g_version_name.c_str());
        return false;
    }
    sc.created = true;
    if (g_runtime.configure != nullptr)
    {
        float velocity = g_velocity_factor;
        ffxConfigureDescUpscaleKeyValue config {};
        config.header.type = FFX_API_CONFIGURE_DESC_TYPE_UPSCALE_KEYVALUE;
        config.key = FFX_API_CONFIGURE_UPSCALE_KEY_FVELOCITYFACTOR;
        config.ptr = &velocity;
        g_runtime.configure(&sc.ctx, reinterpret_cast<ffxConfigureDescHeader *>(&config));
    }
    return true;
}



// 原生 D3D11 纯 GPU 传输（游戏设备保持原生 D3D11，不劫持为 On12）。
// 与 dispatch_on12_direct 等价的全 GPU 链路，但资源经 NT 共享句柄（D3D11 建 /
// D3D12 打开）跨设备流转，同步用 D3D11.4 共享 fence：
//   ① D3D11（游戏 stream）：深度 CS 提取 + CopyResource 输入 → Signal(f11, v1)
//   ② D3D12 队列 Wait(f12, v1) → motion 解码 + ffxDispatch（共享纹理 SRV/UAV）→
//     Signal(f12, v2)
//   ③ D3D11 Wait(f11, v2) → CopyResource 共享输出 → 游戏输出
// 全程无 CPU staging / Map / UpdateSubresource。单变量原则：FFX 参数与 On12 路径一致。
bool dispatch_gpu_shared(const FrameInput &input, ID3D11DeviceContext *game_context,
                         SdkContext &sc, bool reset)
{
    if (!g_gpu_interop_ready || !g_shared_fence || !g_shared_fence11 || !game_context)
        return false;
    if (!g_game_ctx4)
    {
        if (FAILED(game_context->QueryInterface(IID_PPV_ARGS(&g_game_ctx4))) || !g_game_ctx4)
        {
            sdk_note(L"gpu interop ctx4 unavailable stage=direct");
            return false;
        }
    }
    // 上一轮 D3D12 对共享输入/输出的使用必须已完成（否则 D3D11 复用会踩未完结的 GPU 工作）。
    if (g_shared_fence_value != 0 &&
        FAILED(g_game_ctx4->Wait(g_shared_fence11.Get(), g_shared_fence_value)))
        return false;

    // ---- ① D3D11 侧：深度提取（CS）＋ 输入 GPU 拷贝 ----
    if (input.depth && g_depth_extract_cs && g_depth_share_uav)
    {
        if (!g_depth_src_srv || g_depth_src_last != input.depth)
        {
            g_depth_src_srv.Reset();
            // 按深度纹理实际格式选视图（绑定深度曾全 0；
            // DSV 深度可能为 D24 族或 D32 族——Load().x 统一得到 0..1）
            DXGI_FORMAT srv_fmt = DXGI_FORMAT_R32_FLOAT;
            ID3D11Texture2D *dtex = nullptr;
            if (SUCCEEDED(input.depth->QueryInterface(__uuidof(ID3D11Texture2D),
                                                      reinterpret_cast<void **>(&dtex))))
            {
                D3D11_TEXTURE2D_DESC td {};
                dtex->GetDesc(&td);
                switch (td.Format)
                {
                    case DXGI_FORMAT_R24G8_TYPELESS:
                    case DXGI_FORMAT_D24_UNORM_S8_UINT:
                    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
                        srv_fmt = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
                        break;
                    default:
                        srv_fmt = DXGI_FORMAT_R32_FLOAT;
                        break;
                }
                dtex->Release();
            }
            D3D11_SHADER_RESOURCE_VIEW_DESC srv {};
            srv.Format = srv_fmt;
            srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = 1;
            if (FAILED(g_d11dev->CreateShaderResourceView(input.depth, &srv, &g_depth_src_srv)))
                g_depth_src_srv.Reset();
            g_depth_src_last = g_depth_src_srv ? input.depth : nullptr;
        }
        if (g_depth_src_srv)
        {
            if (g_motion_cb)
            {
                // 复用 16B 常数缓冲（b0）：depth 提取用 g_depth_scale（XeSS 激活时归一化 [0,1]）
                const float dscale[4] = {g_depth_scale, 0.0f, 0.0f, 0.0f};
                game_context->UpdateSubresource(g_motion_cb.Get(), 0, nullptr, dscale, 0, 0);
                ID3D11Buffer *cbs[] = {g_motion_cb.Get()};
                game_context->CSSetConstantBuffers(0, 1, cbs);
            }
            game_context->CSSetShader(g_depth_extract_cs.Get(), nullptr, 0);
            ID3D11ShaderResourceView *srvs[] = { g_depth_src_srv.Get() };
            game_context->CSSetShaderResources(0, 1, srvs);
            ID3D11UnorderedAccessView *uavs[] = { g_depth_share_uav.Get() };
            game_context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
            game_context->Dispatch((g_render_w + 7u) / 8u, (g_render_h + 7u) / 8u, 1u);
            ID3D11UnorderedAccessView *null_uavs[] = { nullptr };
            game_context->CSSetUnorderedAccessViews(0, 1, null_uavs, nullptr);
            ID3D11ShaderResourceView *null_srvs[] = { nullptr };
            game_context->CSSetShaderResources(0, 1, null_srvs);
            game_context->CSSetShader(nullptr, nullptr, 0);
        }
    }
    // D3D11 侧 motion 解码——游戏 motion（R10G10B10A2 平方编码）→
    // 共享 R16G16_FLOAT。绕过原 D3D12 decode pass（金丝雀证实从不写 cvt）。
    if (input.motion && g_motion_decode_cs && g_motion_cvt_share_uav)
    {
        if (!g_motion_src_srv || g_motion_src_last != input.motion)
        {
            g_motion_src_srv.Reset();
            D3D11_SHADER_RESOURCE_VIEW_DESC srv {};
            srv.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
            srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = 1;
            if (FAILED(g_d11dev->CreateShaderResourceView(input.motion, &srv, &g_motion_src_srv)))
                g_motion_src_srv.Reset();
            g_motion_src_last = g_motion_src_srv ? input.motion : nullptr;
        }
        if (g_motion_src_srv)
        {
            if (g_motion_cb)
            {
                const float flip[4] = {g_motion_flip, 0.0f, 0.0f, 0.0f};
                game_context->UpdateSubresource(g_motion_cb.Get(), 0, nullptr, flip, 0, 0);
                ID3D11Buffer *cbs[] = {g_motion_cb.Get()};
                game_context->CSSetConstantBuffers(0, 1, cbs);
            }
            game_context->CSSetShader(
                g_motion_deadzone && g_motion_decode_dz_cs ? g_motion_decode_dz_cs.Get()
                                                           : g_motion_decode_cs.Get(),
                nullptr, 0);
            ID3D11ShaderResourceView *srvs[] = { g_motion_src_srv.Get() };
            game_context->CSSetShaderResources(0, 1, srvs);
            // u0 = 解码 motion（R16G16），u1 = reactive（R8，motion B 通道）
            ID3D11UnorderedAccessView *uavs[] = { g_motion_cvt_share_uav.Get(), g_reactive_share_uav.Get() };
            game_context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
            game_context->Dispatch((g_render_w + 7u) / 8u, (g_render_h + 7u) / 8u, 1u);
            ID3D11UnorderedAccessView *null_uavs[2] = { nullptr, nullptr };
            game_context->CSSetUnorderedAccessViews(0, 2, null_uavs, nullptr);
            ID3D11ShaderResourceView *null_srvs[] = { nullptr };
            game_context->CSSetShaderResources(0, 1, null_srvs);
            game_context->CSSetShader(nullptr, nullptr, 0);
        }
    }
    if (input.color)
        game_context->CopyResource(g_tex_color.d11.Get(), input.color);
    // motion 不再拷贝进共享 raw 纹理——解码 CS 已直接读游戏纹理，
    // 解码结果（共享 R16G16）才是 FFX 的 motion 输入。
    game_context->Flush();
    const std::uint64_t v1 = g_shared_fence_value + 1;
    if (FAILED(g_game_ctx4->Signal(g_shared_fence11.Get(), v1)))
        return false;
    g_shared_fence_value = v1;
    if (FAILED(g_queue->Wait(g_shared_fence.Get(), v1)))
        return false;

    // ---- ② D3D12 侧：motion 解码 + FFX（共享纹理直读） ----
    On12CommandSlot &slot = g_on12_command_slots[
        g_on12_command_slot_cursor++ % static_cast<std::uint32_t>(g_on12_command_slots.size())];
    if (!slot.allocator || !slot.list)
        return false;
    if (slot.fence_value != 0 && g_fence->GetCompletedValue() < slot.fence_value)
    {
        // 超时保护：GPU 卡死时不再无限挂起（1s 后失败返回，上层 fail-open）。
        if (FAILED(g_fence->SetEventOnCompletion(slot.fence_value, g_fence_event)) ||
            WaitForSingleObject(g_fence_event, 1000) != WAIT_OBJECT_0)
            return false;
    }
    if (FAILED(slot.allocator->Reset()) || FAILED(slot.list->Reset(slot.allocator.Get(), nullptr)))
        return false;
    ID3D12GraphicsCommandList *const cmd = slot.list.Get();
    const auto barrier = [&](ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                             D3D12_RESOURCE_STATES after)
    {
        if (resource == nullptr || before == after)
            return;
        D3D12_RESOURCE_BARRIER item {};
        item.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        item.Transition.pResource = resource;
        item.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        item.Transition.StateBefore = before;
        item.Transition.StateAfter = after;
        cmd->ResourceBarrier(1, &item);
    };

    // 共享输入统一先到 NON_PIXEL_SHADER_RESOURCE（FFX 将从这里再转自身需要的状态）
    // motion 输入 = 共享解码 R16G16（D3D11 CS 已解码）；raw motion 不再进 D3D12
    ID3D12Resource *inputs[4] = {};
    inputs[0] = g_tex_color.d12.Get();
    inputs[1] = g_tex_depth_share.d12.Get();
    inputs[2] = g_tex_motion_cvt_share.d12.Get();
    if (input.use_reactive_mask && g_tex_reactive_share.d12)
        inputs[3] = g_tex_reactive_share.d12.Get();
    for (ID3D12Resource *resource : inputs)
        barrier(resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // FFX：color/depth/motion 全为共享纹理（SRV），输出直写共享输出（UAV）
    barrier(g_tex_output.d12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ffxDispatchDescUpscale dispatch {};
    dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dispatch.commandList = cmd;
    dispatch.color = ffxApiGetResourceDX12(g_tex_color.d12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.color.description.format = FFX_API_SURFACE_FORMAT_R10G10B10A2_TYPELESS;
    dispatch.depth = ffxApiGetResourceDX12(g_tex_depth_share.d12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.depth.description.format = FFX_API_SURFACE_FORMAT_R32_FLOAT;
    dispatch.motionVectors =
        ffxApiGetResourceDX12(g_tex_motion_cvt_share.d12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.motionVectors.description.format = FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    // reactive（motion B 通道提取，共享 R8）——历史"无改善"结论在 motion=0 坏基线上，重估
    if (input.use_reactive_mask && g_tex_reactive_share.d12)
    {
        dispatch.reactive =
            ffxApiGetResourceDX12(g_tex_reactive_share.d12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        dispatch.reactive.description.format = FFX_API_SURFACE_FORMAT_R8_UNORM;
    }
    dispatch.output = ffxApiGetResourceDX12(g_tex_output.d12.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.output.description.format = ffxApiGetSurfaceFormatDX12(g_output_enc_format);
    dispatch.jitterOffset = {input.jitter_x, input.jitter_y};
    dispatch.motionVectorScale = {input.motion_scale_x, input.motion_scale_y};
    dispatch.renderSize = {input.render_w, input.render_h};
    dispatch.enableSharpening = input.enable_sharpening;
    dispatch.sharpness = input.sharpness;
    dispatch.frameTimeDelta = input.frame_time_delta_ms;
    dispatch.preExposure = input.pre_exposure;
    dispatch.reset = reset;
    dispatch.cameraNear = input.camera_near;
    dispatch.cameraFar = input.camera_far;
    dispatch.cameraFovAngleVertical = input.camera_fov_vertical;
    dispatch.viewSpaceToMetersFactor = 1.0f;
    const ffxReturnCode_t rc = g_runtime.dispatch(
        &sc.ctx, reinterpret_cast<const ffxDispatchDescHeader *>(&dispatch));
    g_last_ffx_dispatch_rc.store(static_cast<int>(rc), std::memory_order_relaxed);
    if (rc != FFX_API_RETURN_OK)
        sdk_note(L"gpu ffxDispatch rc=%d stage=direct", static_cast<int>(rc));

    // 全部归还 COMMON（跨 API 交接契约），再交给 D3D11 侧
    barrier(g_tex_output.d12.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COMMON);
    for (ID3D12Resource *resource : inputs)
        barrier(resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COMMON);
    if (FAILED(cmd->Close()))
        return false;
    ID3D12CommandList *lists[] = {cmd};
    g_queue->ExecuteCommandLists(1, lists);
    const UINT64 ring_signal = ++g_fence_value;
    const std::uint64_t v2 = g_shared_fence_value + 1;
    // 双 signal：ring 槽位完成（自有 fence）+ 跨 API 交接完成（共享 fence）
    if (FAILED(g_queue->Signal(g_fence.Get(), ring_signal)) ||
        FAILED(g_queue->Signal(g_shared_fence.Get(), v2)))
        return false;
    slot.fence_value = ring_signal;
    g_shared_fence_value = v2;

    // ---- ③ D3D11 侧：等待输出完成 → GPU 拷贝回游戏输出 ----
    if (FAILED(g_game_ctx4->Wait(g_shared_fence11.Get(), v2)))
        return false;
    if (input.output_target)
        game_context->CopyResource(input.output_target, g_tex_output.d11.Get());
    game_context->Flush();

    if (rc == FFX_API_RETURN_OK)
    {
        g_gpu_only_transport_available = true;
        const std::uint64_t count = g_on12_direct_dispatch_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 4 || (count % 1024u) == 0)
            sdk_note(L"gpu interop ffx12 completed count=%llu no_cpu_bridge=1",
                     static_cast<unsigned long long>(count));
    }
    return rc == FFX_API_RETURN_OK;
}

} // namespace








bool init_locked(ID3D11Device *game_device, const wchar_t *sdk_dll_path)
{
    if (g_active.load(std::memory_order_relaxed))
        return true;
    if (game_device == nullptr)
        return false;
    g_d11dev = game_device;
    // 清理：On12 引导已移除（不发生产品路线）。自建 D3D12 设备 + 共享句柄互操作。
    g_gpu_only_transport_available = false; // enabled only after direct gpu-interop dispatch lands.
    SetUnhandledExceptionFilter(sdk234_exception_filter); // 崩溃定位：异常写 sdk234_steps.log
    sdk234_step("init begin");
#if defined(FFX12_DEBUG_STEPS)
    std::printf("[sdk234] init: device=%p dll=%ls\n", game_device, sdk_dll_path);
    std::fflush(stdout);
#endif

    // 同适配器建 D3D12 设备（共享句柄互操作必需同一 LUID）
    ComPtr<IDXGIDevice> dxgi_dev;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(g_d11dev.As(&dxgi_dev)) || FAILED(dxgi_dev->GetAdapter(&adapter)))
        return false;
#if defined(FFX12_DEBUG_STEPS)
    std::printf("[sdk234] init: adapter obtained\n");
    std::fflush(stdout);
#endif
    // D3D12 设备（debug layer 可配：先启用 debug interface）
    if (g_debug_layer)
    {
        ComPtr<ID3D12Debug> debug;
        const HRESULT dbg_hr = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
        if (FAILED(dbg_hr) || !debug)
        {
            sdk_note(L"d3d12 debug layer unavailable hr=0x%08X", static_cast<unsigned>(dbg_hr));
        }
        else
        {
            debug->EnableDebugLayer();
        }
    }
    {
        const HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_d12dev));
        if (FAILED(hr) || !g_d12dev)
            return false;
        if (g_debug_layer)
        {
            if (FAILED(g_d12dev.As(&g_info_queue)))
                g_info_queue.Reset();
            else
                g_info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
        }
    }
    // 适配器 LUID（验证 D3D11/D3D12 同适配器）
    {
        DXGI_ADAPTER_DESC ad {};
        adapter->GetDesc(&ad);
        g_d11_luid = (static_cast<std::uint64_t>(ad.AdapterLuid.HighPart) << 32) | ad.AdapterLuid.LowPart;
        const LUID d12 = g_d12dev->GetAdapterLuid();
        g_d12_luid = (static_cast<std::uint64_t>(d12.HighPart) << 32) | d12.LowPart;
    }
#if defined(FFX12_DEBUG_STEPS)
    std::printf("[sdk234] init: d3d12 device ok\n");
    std::fflush(stdout);
#endif
    {
        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        if (FAILED(g_d12dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue))))
            return false;
    }
    if (FAILED(g_d12dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_allocator))))
        return false;
    if (FAILED(g_d12dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocator.Get(), nullptr,
                                           IID_PPV_ARGS(&g_cmdlist))))
        return false;
    // CreateCommandList 创建的 cmdlist 处于 open 状态；Reset 要求 closed → 创建后立即 Close
    g_cmdlist->Close();
    g_cmdlist_open = false;
    if (FAILED(g_d12dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence))))
        return false;
    g_fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (g_fence_event == nullptr)
        return false;

    // ---- ：原生 D3D11 纯 GPU 传输初始化 ----
    // 共享 fence（D3D12 创建 → D3D11 OpenSharedFence）+ 深度提取/motion 解码 CS 编译。
    g_gpu_interop_ready = false;
    if (g_gpu_interop)
    {
        bool setup_ok = true;
        if (FAILED(g_d12dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_shared_fence))) ||
            !g_shared_fence)
            setup_ok = false;
        if (setup_ok && FAILED(g_d12dev->CreateSharedHandle(
                             g_shared_fence.Get(), nullptr, GENERIC_ALL, nullptr, &g_shared_fence_handle)))
            setup_ok = false;
        if (setup_ok && (FAILED(g_d11dev.As(&g_d11_5)) || !g_d11_5))
            setup_ok = false;
        if (setup_ok && FAILED(g_d11_5->OpenSharedFence(
                             g_shared_fence_handle, IID_PPV_ARGS(&g_shared_fence11))))
            setup_ok = false;
        if (g_shared_fence_handle)
        {
            CloseHandle(g_shared_fence_handle);
            g_shared_fence_handle = nullptr;
        }
        if (setup_ok && !g_shared_fence11)
            setup_ok = false;
        // 深度提取 CS 编译
        if (setup_ok)
        {
            ComPtr<ID3DBlob> blob, err;
            const HRESULT chr = D3DCompile(g_depth_extract_hlsl, std::strlen(g_depth_extract_hlsl),
                                           nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0,
                                           &blob, &err);
            if (FAILED(chr) || !blob)
                setup_ok = false;
            else if (FAILED(g_d11dev->CreateComputeShader(
                             blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                             &g_depth_extract_cs)))
                setup_ok = false;
        }
        // D3D11 侧 motion 解码 CS（绕过从不写 cvt 的 D3D12 decode pass）
        if (setup_ok)
        {
            ComPtr<ID3DBlob> blob, err;
            const HRESULT chr = D3DCompile(g_motion_decode_d11_hlsl, std::strlen(g_motion_decode_d11_hlsl),
                                           nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0,
                                           &blob, &err);
            if (FAILED(chr) || !blob)
                setup_ok = false;
            else if (FAILED(g_d11dev->CreateComputeShader(
                             blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                             &g_motion_decode_cs)))
                setup_ok = false;
        }
        // 静止死区变体（微小 motion 归零；触发 FSR 静止锁定）
        if (setup_ok)
        {
            ComPtr<ID3DBlob> blob, err;
            const D3D_SHADER_MACRO defs[] = {{"FFX12_MOTION_DEADZONE", "1"}, {nullptr, nullptr}};
            const HRESULT chr = D3DCompile(g_motion_decode_d11_hlsl, std::strlen(g_motion_decode_d11_hlsl),
                                           nullptr, defs, nullptr, "main", "cs_5_0", 0, 0,
                                           &blob, &err);
            if (FAILED(chr) || !blob)
                setup_ok = false;
            else if (FAILED(g_d11dev->CreateComputeShader(
                             blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                             &g_motion_decode_dz_cs)))
                setup_ok = false;
        }
        if (setup_ok)
        {
            // motion 解码 CS 常数缓冲（b0: g_flip——XeSS/DLSS 方向翻转）
            D3D11_BUFFER_DESC mcb {};
            mcb.ByteWidth = 16;
            mcb.Usage = D3D11_USAGE_DEFAULT;
            mcb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            const float mcb_init[4] = {1.0f, 0.0f, 0.0f, 0.0f};
            D3D11_SUBRESOURCE_DATA mcb_data { mcb_init, 0, 0 };
            if (FAILED(g_d11dev->CreateBuffer(&mcb, &mcb_data, &g_motion_cb)))
                g_motion_cb.Reset();
            g_gpu_interop_ready = true;
            g_shared_fence_value = 0;
            sdk_note(L"gpu interop ready stage=init (NT shared handles + D3D11.4 fence)");
        }
        else
        {
            g_gpu_interop_ready = false;
            sdk_note(L"gpu interop unavailable stage=init");
            g_shared_fence.Reset();
            g_shared_fence11.Reset();
            g_d11_5.Reset();
            g_depth_extract_cs.Reset();
        }
    }
    if (g_gpu_interop_ready)
    {
        for (On12CommandSlot &slot : g_on12_command_slots)
        {
            if (FAILED(g_d12dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                         IID_PPV_ARGS(&slot.allocator))) ||
                FAILED(g_d12dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    slot.allocator.Get(), nullptr,
                                                    IID_PPV_ARGS(&slot.list))) ||
                FAILED(slot.list->Close()))
            {
                sdk_note(L"command ring creation failed stage=init");
                return false;
            }
            slot.fence_value = 0;
        }
        g_on12_command_slot_cursor = 0;
    }

    // Load the known-good ffx-api provider and select exactly the configured
    // effect version.  Do not silently fall back: a version mismatch must fail
    // open to the game's native path rather than alter the temporal baseline.
    if (sdk_dll_path == nullptr || sdk_dll_path[0] == L'\0')
    {
        sdk_note(L"ffx-api path missing stage=runtime");
        return false;
    }
    // 路由可能把 SDK 路径从默认 4.1.1 切到 402c（RDNA2/N 卡 16-50/Intel Arc）。
    // 切换时从 preload 候选缓存取句柄（候选已在 OptiScaler hook 装好前装载，
    // 避免 LoadLibrary 被劫持 err=18 → 4070 Super 实测）；缓存未命中才兜底 LoadLibrary。
    if (g_sdk_module != nullptr && g_sdk_loaded_path == sdk_dll_path)
    {
        // 路径一致：复用当前句柄
    }
    else
    {
        HMODULE target = nullptr;
        // 起：候选缓存优先（preload 的干净句柄，防 OptiScaler hook 劫持 err=18），
        // 缓存未命中才 LoadLibrary 兜底（Auto402c=0 手动路径场景：与 OptiScaler 模块对齐）。
        // 曾尝试统一 LoadLibrary——破坏手动接管，回滚恢复本逻辑。
        for (const PreloadEntry &e : g_preloaded)
        {
            if (e.path == sdk_dll_path)
            {
                target = e.module;
                break;
            }
        }
        if (target == nullptr)
            target = LoadLibraryW(sdk_dll_path); // 候选外路径兜底
        if (target == nullptr)
        {
            sdk_note(L"LoadLibrary ffx-api failed err=%lu stage=runtime",
                     static_cast<unsigned long>(GetLastError()));
            return false;
        }
        if (g_sdk_module != nullptr)
            FreeLibrary(g_sdk_module); // 释放旧主句柄（切换）
        g_sdk_module = target;
        g_sdk_loaded_path = sdk_dll_path;
    }
    g_runtime.create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(g_sdk_module, "ffxCreateContext"));
    g_runtime.destroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(g_sdk_module, "ffxDestroyContext"));
    g_runtime.dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(g_sdk_module, "ffxDispatch"));
    g_runtime.query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(g_sdk_module, "ffxQuery"));
    g_runtime.configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(g_sdk_module, "ffxConfigure"));
    if (!g_runtime.create || !g_runtime.destroy || !g_runtime.dispatch || !g_runtime.query || !g_runtime.configure)
    {
        sdk_note(L"ffx-api export resolve failed stage=runtime");
        FreeLibrary(g_sdk_module);
        g_sdk_module = nullptr;
        g_runtime = {};
        return false;
    }
    std::uint64_t version_count = 0;
    ffxQueryDescGetVersions query {};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    query.device = g_d12dev.Get();
    query.outputCount = &version_count;
    if (g_runtime.query(nullptr, reinterpret_cast<ffxQueryDescHeader *>(&query)) != FFX_API_RETURN_OK ||
        version_count == 0 || version_count > 64)
    {
        sdk_note(L"ffx-api version query failed count=%llu stage=runtime", version_count);
        FreeLibrary(g_sdk_module);
        g_sdk_module = nullptr;
        g_runtime = {};
        return false;
    }
    std::vector<std::uint64_t> version_ids(static_cast<std::size_t>(version_count));
    std::vector<const char *> version_names(static_cast<std::size_t>(version_count));
    std::uint64_t version_capacity = version_count;
    query.outputCount = &version_capacity;
    query.versionIds = version_ids.data();
    query.versionNames = version_names.data();
    if (g_runtime.query(nullptr, reinterpret_cast<ffxQueryDescHeader *>(&query)) != FFX_API_RETURN_OK)
    {
        sdk_note(L"ffx-api version enumerate failed stage=runtime");
        FreeLibrary(g_sdk_module);
        g_sdk_module = nullptr;
        g_runtime = {};
        return false;
    }
    g_sdk_version_id = 0;
    // （N 卡降级）：大版本前缀匹配，无匹配时自动降级 4→3→2。
    // 例：N 卡上 4.1.1 provider 的 GET_VERSIONS 列表不含 "4.x"（provider 按 GPU 能力过滤：
    // FSR4 仅 RDNA4/RDNA3 dGPU 官方支持），请求 4.x 应自动回落 3.1.5（理论预期），
    // 而非硬失败导致每帧 DISPATCH_FAILED。降级结果同步 OSD 显示名，避免误导。
    std::string matched_name;
    std::string prefix = g_sdk_version_prefix; // "4."/"3."/"2."
    while (!prefix.empty() && g_sdk_version_id == 0)
    {
        for (std::uint64_t i = 0; i < version_capacity; ++i)
        {
            if (version_names[i] != nullptr &&
                std::strncmp(version_names[i], prefix.c_str(), prefix.size()) == 0)
            {
                g_sdk_version_id = version_ids[i];
                matched_name = version_names[i];
                break;
            }
        }
        if (g_sdk_version_id == 0)
        {
            if (prefix == "4.")
            {
                prefix = "3.";
                sdk_note(L"ffx-api version 4.x unavailable on this GPU, falling back to 3.x");
            }
            else if (prefix == "3.")
            {
                prefix = "2.";
                sdk_note(L"ffx-api version 3.x unavailable on this GPU, falling back to 2.x");
            }
            else
            {
                break; // 2.x 也无匹配
            }
        }
    }
    if (g_sdk_version_id == 0)
    {
        sdk_note(L"requested ffx-api version unavailable stage=runtime");
        FreeLibrary(g_sdk_module);
        g_sdk_module = nullptr;
        g_runtime = {};
        return false;
    }
    // 显示名按实际匹配版本（含降级）更新，OSD 如实反映
    if (!matched_name.empty())
    {
        g_sdk_matched_name = matched_name;
        const char major = matched_name[0];
        if (major == '4')
            g_version_name = "ffx12/FSR4";
        else if (major == '3')
            g_version_name = "ffx12/FSR3";
        else if (major == '2')
            g_version_name = "ffx12/FSR2";
    }


    g_active.store(true, std::memory_order_release);
    return true;
}

bool init(ID3D11Device *game_device, const wchar_t *sdk_dll_path)
{
    std::lock_guard lock(g_mutex);
    return init_locked(game_device, sdk_dll_path);
}

void shutdown()
{
    std::lock_guard lock(g_mutex);
    if (!g_active.exchange(false, std::memory_order_acq_rel))
        return;
    for (SdkContext &sc : g_sdk_ctxs)
    {
        if (sc.created)
            destroy_context(sc);
    }
    g_sdk_ctxs.clear();
    release_shared(g_tex_color);
    release_shared(g_tex_depth);
    release_shared(g_tex_motion);
    release_shared(g_tex_output);
    release_shared_nt(g_tex_depth_share);
    release_shared_nt(g_tex_motion_cvt_share);
    release_shared_nt(g_tex_reactive_share);
    g_depth_share_uav.Reset();
    g_motion_cvt_share_uav.Reset();
    g_reactive_share_uav.Reset();
    g_depth_src_srv.Reset();
    g_depth_src_last = nullptr;
    g_motion_src_srv.Reset();
    g_motion_src_last = nullptr;
    g_depth_extract_cs.Reset();
    g_motion_decode_cs.Reset();
    g_motion_decode_dz_cs.Reset();
    g_shared_fence.Reset();
    g_shared_fence11.Reset();
    g_d11_5.Reset();
    g_game_ctx4.Reset();
    g_shared_fence_value = 0;
    g_gpu_interop_ready = false;
    g_active.store(false, std::memory_order_release);
    g_pq_decode_pso.Reset();
    g_pq_decode_test_pso.Reset();
    g_pq_encode_pso.Reset();
    g_pq_encode_mark_pso.Reset();
    if (g_fence_event)
    {
        CloseHandle(g_fence_event);
        g_fence_event = nullptr;
    }
    g_fence.Reset();
    g_cmdlist.Reset();
    g_cmdlist_open = false;
    g_allocator.Reset();
    for (On12CommandSlot &slot : g_on12_command_slots)
    {
        slot.list.Reset();
        slot.allocator.Reset();
        slot.fence_value = 0;
    }
    g_on12_command_slot_cursor = 0;
    g_on12dev.Reset();
    g_uses_on12_queue = false;
    g_gpu_only_transport_available = false;
    g_queue.Reset();
    g_d12dev.Reset();
    if (g_sdk_module != nullptr)
        FreeLibrary(g_sdk_module);
    g_sdk_module = nullptr;
    g_runtime = {};
    g_sdk_version_id = 0;
}

bool active()
{
    return g_active.load(std::memory_order_relaxed);
}

// 设备移除恢复（）：放弃当前 D3D12/互操作后端并复位活动标志，
// 下一次 dispatch 惰性重建（新设备/队列/共享纹理/FFX context）。
// 坏设备上不调用 runtime.destroy（可能 AV）；旧 context 随进程回收（一次事故级泄漏可接受）。
// 调用方必须已持有 g_mutex（dispatch 路径）。
void recover_device_removed_locked()
{
    if (!g_active.load(std::memory_order_relaxed))
        return;
    g_active.store(false, std::memory_order_release);
    sdk234_step("recover device removed");
    for (SdkContext &sc : g_sdk_ctxs)
    {
        if (sc.created && g_runtime.destroy)
        {
            try
            {
                g_runtime.destroy(&sc.ctx, nullptr);
            }
            catch (...)
            {
            }
        }
        sc.ctx = nullptr;
        sc.created = false;
    }
    g_sdk_ctxs.clear();
    release_shared(g_tex_color);
    release_shared(g_tex_depth);
    release_shared(g_tex_motion);
    release_shared(g_tex_output);
    release_shared_nt(g_tex_depth_share);
    release_shared_nt(g_tex_motion_cvt_share);
    release_shared_nt(g_tex_reactive_share);
    g_depth_share_uav.Reset();
    g_motion_cvt_share_uav.Reset();
    g_reactive_share_uav.Reset();
    g_depth_src_srv.Reset();
    g_depth_src_last = nullptr;
    g_motion_src_srv.Reset();
    g_motion_src_last = nullptr;
    g_shared_fence.Reset();
    g_shared_fence11.Reset();
    if (g_shared_fence_handle)
    {
        CloseHandle(g_shared_fence_handle);
        g_shared_fence_handle = nullptr;
    }
    g_d11_5.Reset();
    g_game_ctx4.Reset();
    g_depth_extract_cs.Reset();
    g_motion_decode_cs.Reset();
    g_motion_decode_dz_cs.Reset();
    g_gpu_interop_ready = false;
    g_shared_fence_value = 0;
    g_d12dev.Reset();
    g_queue.Reset();
    g_allocator.Reset();
    g_cmdlist.Reset();
    g_fence.Reset();
    if (g_fence_event)
    {
        CloseHandle(g_fence_event);
        g_fence_event = nullptr;
    }
    for (On12CommandSlot &slot : g_on12_command_slots)
        slot = On12CommandSlot {};
    g_on12_command_slot_cursor = 0;
    g_on12_direct_dispatch_count.store(0, std::memory_order_relaxed);
}

bool dispatch(const FrameInput &input, ID3D11DeviceContext *game_context, std::uint64_t instance_key)
{
    std::lock_guard lock(g_mutex);
    g_dispatch_counter.fetch_add(1, std::memory_order_relaxed);
    g_last_ffx_dispatch_rc.store(kFfxDispatchNotReached, std::memory_order_relaxed);
#if defined(FFX12_DEBUG_STEPS)
    std::printf("[sdk234] dispatch enter active=%d\n", g_active.load(std::memory_order_relaxed) ? 1 : 0);
    std::fflush(stdout);
#endif
    if (game_context == nullptr || input.color == nullptr || input.depth == nullptr ||
        input.motion == nullptr || input.output_target == nullptr)
        return false;
    if (input.render_w == 0 || input.render_h == 0 || input.display_w == 0 || input.display_h == 0)
        return false;

    // 懒初始化：用游戏 context 的设备 + 配置的 DLL 路径（调用方已持锁）
    if (!g_active.load(std::memory_order_relaxed))
    {
        ID3D11Device *device = nullptr;
        game_context->GetDevice(&device);
        if (device == nullptr || g_sdk_path.empty())
            return false;
        if (!init_locked(device, g_sdk_path.c_str()))
            return false;
    }
#if defined(FFX12_DEBUG_STEPS)
    std::printf("[sdk234] init done, ensuring pool %ux%u -> %ux%u\n", input.render_w, input.render_h,
                input.display_w, input.display_h);
    std::fflush(stdout);
#endif

    // 设备移除检测（P0-3 可观测性）：D3D12/D3D11 任一侧移除即失败并留痕
    if (g_d12dev)
    {
        const HRESULT drr = g_d12dev->GetDeviceRemovedReason();
        if (FAILED(drr))
        {
            sdk_note(L"dispatch device-removed d3d12 reason=0x%08X stage=entry",
                     static_cast<unsigned>(drr));
            recover_device_removed_locked(); // ：放弃后端，下次 dispatch 惰性重建
            return false;
        }
    }
    if (g_d11dev)
    {
        const HRESULT drr11 = g_d11dev->GetDeviceRemovedReason();
        if (FAILED(drr11))
        {
            sdk_note(L"dispatch device-removed d3d11 reason=0x%08X stage=entry",
                     static_cast<unsigned>(drr11));
            recover_device_removed_locked();
            return false;
        }
    }
    // 尺寸变化时重建共享池 + 上下文
    if (!ensure_pool(input))
    {
        sdk_note(L"ensure_pool failed render=%ux%u->%ux%u stage=pool",
                 input.render_w, input.render_h, input.display_w, input.display_h);
        return false;
    }
    sdk234_step("pool ok");
    // 每实例独立 context（全部实例接管；实例重建=新 key→新 context=隐式 reset）
    SdkContext *sc = ctx_for_key(instance_key);
    if (!sc)
    {
        sdk_note(L"ctx slot exhausted stage=ctx");
        return false;
    }
    if (sc->created && (sc->rw != input.render_w || sc->rh != input.render_h ||
                    sc->dw != input.display_w || sc->dh != input.display_h))
    {
        destroy_context(*sc);
        ++g_ctx_recreates;
    }
    if (!create_context(*sc))
    {
        sdk_note(L"create_context failed render=%ux%u->%ux%u stage=ctx",
                 input.render_w, input.render_h, input.display_w, input.display_h);
        return false;
    }
    sdk234_step("ctx ok");
    if (sc->rw != g_render_w || sc->rh != g_render_h ||
        sc->dw != g_display_w || sc->dh != g_display_h)
    {
        sc->rw = g_render_w;
        sc->rh = g_render_h;
        sc->dw = g_display_w;
        sc->dh = g_display_h;
        sc->first = true;
    }
    const bool reset = input.reset || sc->first;
    g_last_reset = reset;
    sc->first = false;

    if (!g_gpu_interop_ready)
    {
        sdk_note(L"gpu interop unavailable stage=dispatch");
        return false;
    }
    return dispatch_gpu_shared(input, game_context, *sc, reset);
}


const char *selected_version_name()
{
    return g_version_name.empty() ? "?" : g_version_name.c_str();
}

const char *matched_version_name()
{
    return g_sdk_matched_name.empty() ? "?" : g_sdk_matched_name.c_str();
}

void set_sdk_dll_path(const wchar_t *path)
{
    if (path != nullptr)
        g_sdk_path = path;
}

void set_depth_inverted(bool inverted)
{
    g_depth_inverted = inverted;
}

void set_decode_motion(bool decode)
{
    g_decode_motion = decode;
}

void set_motion_flip(float flip) // ：XeSS/DLSS 定向——motion 方向翻转（±1）
{
    g_motion_flip = flip;
}

void set_depth_scale(float scale) // ：XeSS/DLSS 定向——depth 值域归一化（XeSS 期望 [0,1]）
{
    g_depth_scale = scale;
}

void set_motion_vectors_jittered(bool jittered)
{
    g_motion_vectors_jittered = jittered;
}

void set_hdr_input(bool hdr)
{
    g_hdr_input = hdr;
}

void set_auto_exposure(bool auto_exposure)
{
    g_auto_exposure = auto_exposure;
}

void set_non_linear(bool non_linear)
{
    g_non_linear = non_linear;
}

void set_use_pq_chain(bool use_chain)
{
    g_use_pq_chain = use_chain;
}

void set_velocity_factor(float factor)
{
    g_velocity_factor = factor;
}

void set_dump_frames(std::uint32_t n)
{
    g_dump_frames = n;
}

// 版本命名规范——对外统一 ffx12 品牌（ffx12-fsr4.1.1 / ffx12-fsr3.1.5 /
// ffx12-fsr2.3.4），内部映射回 ffx-api 枚举名（4.1.1/3.1.5/2.3.4）做版本匹配。
// 版本判定简化为大版本（ffx12-fsr4.x / ffx12-fsr3.x / ffx12-fsr2.x）——
// 按前缀匹配 provider 支持的版本列表（4.x 命中 4.0.2c/4.1.1/未来 4.2.x），SDK 更新免适配。
void set_sdk_version(const char *name)
{
    if (name == nullptr || name[0] == '\0')
        return;
    std::string input = name;
    // 兼容旧值（2.3.4/3.1.5/4.1.1 直接可用；新规范带 ffx12-fsr 前缀）
    const char *prefix = "ffx12-";
    if (input.rfind(prefix, 0) == 0)
        input = input.substr(std::strlen(prefix));
    if (input.rfind("fsr", 0) == 0)
        input = input.substr(3); // "fsr4.x" -> "4.x"（ffx-api 枚举名前缀）
    // 大版本前缀（只认 2/3/4 系）
    if (!input.empty() && input[0] >= '2' && input[0] <= '4')
        g_sdk_version_prefix = std::string(1, input[0]) + ".";
    else
        g_sdk_version_prefix.clear();
    // 显示名统一为 ffx12 品牌（4.0.2c 为 RDNA2 专用 FSR4 模型，）
    if (g_sdk_version_prefix == "4.")
        g_version_name = "ffx12/FSR4";
    else if (g_sdk_version_prefix == "3.")
        g_version_name = "ffx12/FSR3";
    else if (g_sdk_version_prefix == "2.")
        g_version_name = "ffx12/FSR2";
    else
        g_version_name = "ffx12";
}

void adapter_luids(std::uint64_t &d11_luid, std::uint64_t &d12_luid)
{
    d11_luid = g_d11_luid;
    d12_luid = g_d12_luid;
}

void interop_capabilities(bool &dx11on12, bool &gpu_only_transport)
{
    std::lock_guard lock(g_mutex);
    dx11on12 = g_dx11on12_available;
    gpu_only_transport = g_gpu_only_transport_available;
}

void get_sdk_messages(std::wstring &out)
{
    std::lock_guard lock(g_sdk_msg_mutex);
    out = g_sdk_messages;
}

void set_output_mark(bool mark)
{
    g_output_mark = mark;
}

void set_decode_test(bool test)
{
    g_decode_test = test;
}

void set_motion_decode_test(bool test)
{
    g_motion_decode_test = test;
}

void set_motion_deadzone(bool enable)
{
    g_motion_deadzone = enable;
}

// 与 OptiScaler 共存兼容。
// preload 提前装载**全部候选** provider（默认 4.1.1 + 402c）并缓存句柄——
// OptiScaler 的 LoadLibrary hook 会劫持任意 ffx-api 文件名（含 402c，4070 Super 实测 err=18），
// 必须在 hook 装好前（DllMain attach 早期）加载；init_locked 按最终路径从缓存取句柄。
void preload(const wchar_t *path)
{
    if (path == nullptr || path[0] == L'\0')
        return;
    HMODULE h = LoadLibraryW(path);
    if (h == nullptr)
        return;
    std::lock_guard lock(g_mutex);
    for (const PreloadEntry &e : g_preloaded)
        if (e.path == path)
            return;
    g_preloaded.push_back({path, h});
    if (g_sdk_module == nullptr)
    {
        g_sdk_module = h;
        g_sdk_path = path;
        g_sdk_loaded_path = path;
    }
}

void set_debug_layer(bool enable)
{
    g_debug_layer = enable;
}

void set_gpu_interop(bool enable)
{
    g_gpu_interop = enable;
}

bool gpu_interop_ready()
{
    return g_gpu_interop_ready;
}

ID3D11Texture2D *debug_output_texture()
{
    return g_tex_output.d11.Get();
}

ID3D11Texture2D *debug_color_texture()
{
    return g_tex_color.d11.Get();
}

// 实际输入格式（桥日志诊断用）
void input_formats(std::uint32_t &color, std::uint32_t &depth, std::uint32_t &motion, std::uint32_t &output)
{
    std::lock_guard lock(g_mutex);
    color = static_cast<std::uint32_t>(g_input_color_fmt);
    depth = static_cast<std::uint32_t>(g_input_depth_fmt);
    motion = static_cast<std::uint32_t>(g_input_motion_fmt);
    output = static_cast<std::uint32_t>(g_input_output_fmt);
}

// 诊断：最近一次 dispatch 的 reset 标志 + 上下文重建累计次数
void debug_state(bool &last_reset, std::uint64_t &ctx_recreates)
{
    std::lock_guard lock(g_mutex);
    last_reset = g_last_reset;
    ctx_recreates = g_ctx_recreates;
}

int last_ffx_dispatch_return_code()
{
    return g_last_ffx_dispatch_rc.load(std::memory_order_relaxed);
}

void debug_pixels(float out_pixels[16])
{
    std::lock_guard lock(g_debug_px_mutex);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            out_pixels[i * 4 + j] = g_debug_pixels[i].v[j];
}

void get_output_samples(std::uint32_t raw[5], std::uint32_t &w, std::uint32_t &h, bool &valid)
{
    std::lock_guard lock(g_out_samples_mutex);
    for (int i = 0; i < 5; ++i)
        raw[i] = g_out_samples.raw[i];
    w = g_out_samples.w;
    h = g_out_samples.h;
    valid = g_out_samples.valid;
}

void get_chain_samples(ChainSampleData &out)
{
    std::lock_guard lock(g_out_samples_mutex);
    for (int i = 0; i < 2; ++i)
        out.motion_cvt[i] = g_chain_samples.motion_cvt[i];
    for (int i = 0; i < 3; ++i)
    {
        out.color_linear[i] = g_chain_samples.color_linear[i];
        out.output_linear[i] = g_chain_samples.output_linear[i];
    }
    out.canary = g_chain_samples.canary;
    out.color_own = g_chain_samples.color_own;
    out.depth_own = g_chain_samples.depth_own;
    out.motion_own = g_chain_samples.motion_own;
    out.valid = g_chain_samples.valid;
}

} // namespace ffx12
