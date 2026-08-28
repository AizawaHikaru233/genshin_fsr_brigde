// Ffx12AccumTest.cpp — 离线 FSR2 ffxApi 时间性累积验证（ASCII-only）
// 目标：不跑游戏，在同一 GPU（RDNA4）上驱动 amd_fidelityfx_upscaler_dx12.dll，
// 静态场景连续派发 40 帧（首帧 reset，其余累积），读回输出：
//   帧0（reset=单帧重建） vs 帧39（累积 40 帧） vs 帧40（强制 reset）——
// 若帧39与帧40完全相同 → 时间性累积未生效（历史零作用）→ SDK/驱动级问题；
// 若不同 → 累积在工作，问题在游戏侧集成。
// 对每个可用版本（2.3.4/3.1.5/4.1.1）各跑一遍。
//
// 编译：cl /nologo /EHsc /std:c++17 Ffx12AccumTest.cpp ^
//        /I"D:\DLSSG2FSR4\dependencies\FidelityFX-SDK\ffx-api\include\ffx_api" ^
//        d3d12.lib dxgi.lib /Fe:Ffx12AccumTest.exe

#include <Windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "ffx_api.h"
#include "ffx_upscale.h"
#include "dx12/ffx_api_dx12.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

using namespace Microsoft::WRL;

static const UINT RW = 1920, RH = 1080;   // render
static const UINT DW = 3840, DH = 2160;   // display
static const UINT FRAMES = 40;

// ---- helpers ----

static void wait_fence(ID3D12Fence* fence, HANDLE ev, UINT64 value)
{
    if (fence->GetCompletedValue() < value)
    {
        fence->SetEventOnCompletion(value, ev);
        WaitForSingleObject(ev, 10000);
    }
}

static float halton(int32_t index, int32_t base)
{
    float f = 1.0f, r = 0.0f;
    while (index > 0)
    {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(index % base);
        index /= base;
    }
    return r;
}

struct TestIo
{
    ComPtr<ID3D12Resource> color;    // R10G10B10A2_UNORM RW*RH
    ComPtr<ID3D12Resource> depth;    // R32_FLOAT RW*RH
    ComPtr<ID3D12Resource> motion;   // R16G16_FLOAT RW*RH
    ComPtr<ID3D12Resource> reactive; // R8_UNORM RW*RH, optional FSR input
    ComPtr<ID3D12Resource> output;   // R10G10B10A2_UNORM DW*DH, UAV
    ComPtr<ID3D12Resource> rb;       // readback DW*DH*4
};

static bool upload_tex(ID3D12Device* dev, ID3D12GraphicsCommandList* list, ID3D12Resource* dst,
                       const void* data, UINT width, UINT height, UINT bytes_per_pixel,
                       D3D12_RESOURCE_STATES state_after,
                       std::vector<ComPtr<ID3D12Resource>>& keepalive)
{
    const UINT row_bytes = width * bytes_per_pixel;
    const UINT total = row_bytes * height;
    D3D12_HEAP_PROPERTIES hp {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> upload;
    HRESULT hr_up = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
    if (FAILED(hr_up))
    {
        std::printf("FAIL: create upload buffer hr=0x%08X\n", static_cast<unsigned>(hr_up));
        return false;
    }
    keepalive.push_back(upload); // D3D12 命令列表不持有引用：保持存活到 GPU 执行完
    void* p = nullptr;
    if (FAILED(upload->Map(0, nullptr, &p)))
        return false;
    std::memcpy(p, data, total);
    upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION src {}, dst_loc {};
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = dst->GetDesc().Format;
    src.PlacedFootprint.Footprint.Width = width;
    src.PlacedFootprint.Footprint.Height = height;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = row_bytes;
    dst_loc.pResource = dst;
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc.SubresourceIndex = 0;
    D3D12_RESOURCE_BARRIER b0 = {};
    b0.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b0.Transition.pResource = dst;
    b0.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b0.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b0.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    list->ResourceBarrier(1, &b0);
    list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER b1 = b0;
    b1.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b1.Transition.StateAfter = state_after;
    list->ResourceBarrier(1, &b1);
    return true;
}

static ComPtr<ID3D12Resource> make_tex(ID3D12Device* dev, UINT w, UINT h, DXGI_FORMAT fmt,
                                       bool uav, D3D12_RESOURCE_STATES init_state)
{
    D3D12_HEAP_PROPERTIES hp {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = fmt;
    rd.SampleDesc.Count = 1;
    if (uav) rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, init_state, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

// 填充静态测试画面：温和灰阶斜边（0.5 亮区 / 0.1 暗区，避免 PQ 极端值打爆自动曝光）+ 中央 8x8 棋盘补丁
static std::vector<std::uint32_t> make_color()
{
    // R10G10B10A2: 0.5 灰 = 0xE0080200；0.1 灰 = 0xC6619866（A=3）
    const std::uint32_t bright = 0xE0080200u, dark = 0xC6619866u;
    std::vector<std::uint32_t> c(RW * RH);
    for (UINT y = 0; y < RH; ++y)
        for (UINT x = 0; x < RW; ++x)
        {
            bool is_bright = x < y;
            // 中央棋盘补丁（渲染坐标 880..1040）
            if (x >= 880 && x < 1040 && y >= 480 && y < 600)
            {
                const UINT cx = (x - 880) / 8, cy = (y - 480) / 8;
                is_bright = ((cx + cy) & 1) == 0;
            }
            c[y * RW + x] = is_bright ? bright : dark;
        }
    return c;
}

static std::vector<float> make_depth()
{
    std::vector<float> d(RW * RH);
    for (UINT y = 0; y < RH; ++y)
        for (UINT x = 0; x < RW; ++x)
            d[y * RW + x] = 0.9f - 0.4f * (static_cast<float>(y) / static_cast<float>(RH));
    return d;
}

static std::vector<std::uint16_t> make_motion()
{
    return std::vector<std::uint16_t>(RW * RH * 2, 0); // R16G16_FLOAT 0,0（隔离测试基线）
}

// 输出边缘行分析：显示分辨率 y=1080 行，x∈[1040,1120)，打印归一化亮度
static void print_edge_row(const std::vector<std::uint32_t>& px, const char* tag)
{
    std::printf("[%s] edge row y=%u x=1040..1120:\n", tag, DH / 2);
    for (UINT x = 1040; x < 1120; ++x)
    {
        const std::uint32_t v = px[(DH / 2) * DW + x];
        const float r = static_cast<float>((v >> 0) & 0x3FF) / 1023.0f;
        std::printf("%4.2f ", r);
        if ((x - 1040) % 16 == 15) std::printf("\n");
    }
    std::printf("\n");
    // 平滑度：过渡像素计数（0.05..0.95）
    int transitions = 0;
    for (UINT x = 1041; x < 1120; ++x)
    {
        const std::uint32_t a = px[(DH / 2) * DW + x - 1];
        const std::uint32_t b = px[(DH / 2) * DW + x];
        const float fa = static_cast<float>((a >> 0) & 0x3FF) / 1023.0f;
        const float fb = static_cast<float>((b >> 0) & 0x3FF) / 1023.0f;
        if ((fa > 0.05f && fa < 0.95f) || (fb > 0.05f && fb < 0.95f))
            ++transitions;
    }
    std::printf("[%s] transition_pixels=%d (累积平滑应更多)\n\n", tag, transitions);
}

static bool readback_output(ID3D12Device* dev, ID3D12GraphicsCommandList* list,
                            ID3D12Resource* output, ID3D12Resource* rb,
                            std::vector<std::uint32_t>& px)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = output;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R10G10B10A2_TYPELESS;
    dst.PlacedFootprint.Footprint.Width = DW;
    dst.PlacedFootprint.Footprint.Height = DH;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = DW * 4;
    src.pResource = output;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    list->ResourceBarrier(1, &b);
    return true;
}

static bool save_raw(const char* path, const std::vector<std::uint32_t>& px)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f)
        return false;
    fwrite(px.data(), 4, px.size(), f);
    fclose(f);
    return true;
}

static void sdk_msg_cb(uint32_t type, const wchar_t* message)
{
    std::printf("[SDK-MSG type=%u] %ls\n", type, message ? message : L"(null)");
}

// ---- 每版本测试 ----
struct PfnSet
{
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxConfigure configure = nullptr;
};

static void run_version(const char* version_name, std::uint64_t version_id,
                        ID3D12Device* dev, ID3D12CommandQueue* queue,
                        ID3D12CommandAllocator* alloc, ID3D12GraphicsCommandList* list,
                        ID3D12Fence* fence, HANDLE ev, const PfnSet& pfn,
                        const TestIo& io, std::uint32_t ctx_flags, const char* flags_tag,
                        int jit_mode, const char* jit_tag, bool use_reactive)
{
    std::printf("===== version %s [flags 0x%03X %s jit=%s] =====\n", version_name, ctx_flags, flags_tag, jit_tag);

    ffxCreateBackendDX12Desc backend {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = dev;

    ffxOverrideVersion override_version {};
    override_version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    override_version.versionId = version_id;

    ffxCreateContextDescUpscale desc {};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.header.pNext = &backend.header;
    backend.header.pNext = &override_version.header;
    desc.maxRenderSize = {RW, RH};
    desc.maxUpscaleSize = {DW, DH};
    desc.fpMessage = &sdk_msg_cb;
    // 组合矩阵：0=纯 LDR 基线；单 flag 各测；0x129=游戏内全套（HDR|DEPTH_INVERTED|AUTO_EXPOSURE|NON_LINEAR）
    desc.flags = ctx_flags;

    ffxContext ctx = nullptr;
    std::printf("calling ffxCreateContext...\n");
    std::fflush(stdout);
    const ffxReturnCode_t rc_create = pfn.create(&ctx, reinterpret_cast<ffxCreateContextDescHeader*>(&desc), nullptr);
    std::printf("create rc=%d\n", static_cast<int>(rc_create));
    if (rc_create != FFX_API_RETURN_OK || !ctx)
    {
        std::printf("===== version %s: CREATE FAILED =====\n\n", version_name);
        return;
    }

    std::vector<std::uint32_t> f0, f38, f39, f40;
    f0.resize(DW * DH); f38.resize(DW * DH); f39.resize(DW * DH); f40.resize(DW * DH);

    // 与游戏内一致：fVelocityFactor 配置（0.5）
    {
        float velocity = 0.5f;
        ffxConfigureDescUpscaleKeyValue cfg {};
        cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_UPSCALE_KEYVALUE;
        cfg.key = FFX_API_CONFIGURE_UPSCALE_KEY_FVELOCITYFACTOR;
        cfg.ptr = &velocity;
        pfn.configure(&ctx, reinterpret_cast<ffxConfigureDescHeader*>(&cfg));
    }

    // enc 状态跟踪：游戏内初值 COMMON → dispatch 声明 UAV（隐式提升）→ 帧末 COPY_SOURCE（读回）→ 次帧从 COPY_SOURCE 派发
    D3D12_RESOURCE_STATES out_state = D3D12_RESOURCE_STATE_COMMON;

    // fence 基线：必须高于 main 已用过的值（2），否则 wait 立即返回、GPU 未同步
    UINT64 frame_value = 1000;
    for (UINT f = 0; f < FRAMES; ++f)
    {
        alloc->Reset();
        list->Reset(alloc, nullptr);

        ffxDispatchDescUpscale d {};
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList = list;
        d.color = ffxApiGetResourceDX12(io.color.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.color.description.format = FFX_API_SURFACE_FORMAT_R10G10B10A2_TYPELESS;
        d.depth = ffxApiGetResourceDX12(io.depth.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.depth.description.format = FFX_API_SURFACE_FORMAT_R32_FLOAT;
        d.motionVectors = ffxApiGetResourceDX12(io.motion.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.motionVectors.description.format = FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
        if (use_reactive)
        {
            d.reactive = ffxApiGetResourceDX12(io.reactive.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
            d.reactive.description.format = FFX_API_SURFACE_FORMAT_R8_UNORM;
        }
        d.output = ffxApiGetResourceDX12(io.output.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        d.output.description.format = FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
        // jitter：jit_mode 0=halton 逐帧变化；1=恒定 0；2=恒定 5px（A/B 判定 jitter 是否到达采样）
        if (jit_mode == 1)
        {
            d.jitterOffset.x = 0.0f;
            d.jitterOffset.y = 0.0f;
        }
        else if (jit_mode == 2)
        {
            d.jitterOffset.x = 5.0f;
            d.jitterOffset.y = 5.0f;
        }
        else if (jit_mode == 3)
        {
            // 模式 0 风格（游戏约定 +norm×render−0.5 的亚像素等价）：+(halton − 0.5)
            d.jitterOffset.x = +(halton(static_cast<int32_t>(f) + 1, 2) - 0.5f);
            d.jitterOffset.y = +(halton(static_cast<int32_t>(f) + 1, 3) - 0.5f);
        }
        else
        {
            // 模式 3 风格（−norm×render+0.5 的亚像素等价）：−(halton − 0.5)
            d.jitterOffset.x = -(halton(static_cast<int32_t>(f) + 1, 2) - 0.5f);
            d.jitterOffset.y = -(halton(static_cast<int32_t>(f) + 1, 3) - 0.5f);
        }
        d.motionVectorScale.x = static_cast<float>(RW);
        d.motionVectorScale.y = static_cast<float>(RH);
        d.renderSize = {RW, RH};
        d.upscaleSize = {DW, DH};
        d.enableSharpening = false; // 与游戏内一致（RCAS 关闭）
        d.sharpness = 0.0f;
        d.frameTimeDelta = 16.7f;
        d.preExposure = 1.0f;
        d.reset = (f == 0);
        d.cameraNear = 0.25f;
        d.cameraFar = 6000.0f;
        d.cameraFovAngleVertical = 0.7853981634f;
        d.viewSpaceToMetersFactor = 1.0f;
        d.flags = 0;

        const ffxReturnCode_t rc = pfn.dispatch(&ctx, reinterpret_cast<const ffxDispatchDescHeader*>(&d));
        if (rc != FFX_API_RETURN_OK)
        {
            std::printf("dispatch f=%u rc=%d\n", f, static_cast<int>(rc));
            break;
        }

        // 游戏内帧序：dispatch 后 enc → UAV（显式；游戏在此也做同样转换）
        if (out_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            D3D12_RESOURCE_BARRIER b {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = io.output.Get();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = out_state;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            list->ResourceBarrier(1, &b);
            out_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }

        // 读回：f=0（reset 单帧）、f=FRAMES-2/f=FRAMES-1（累积收敛对比）——读回后 enc 留在 COPY_SOURCE（游戏内同）
        if (f == 0 || f == FRAMES - 1 || f == FRAMES - 2)
        {
            if (out_state != D3D12_RESOURCE_STATE_COPY_SOURCE)
            {
                D3D12_RESOURCE_BARRIER b {};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = io.output.Get();
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = out_state;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                list->ResourceBarrier(1, &b);
                out_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
            }
            D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
            dst.pResource = io.rb.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
            dst.PlacedFootprint.Footprint.Width = DW;
            dst.PlacedFootprint.Footprint.Height = DH;
            dst.PlacedFootprint.Footprint.Depth = 1;
            dst.PlacedFootprint.Footprint.RowPitch = DW * 4;
            src.pResource = io.output.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }

        list->Close();
        ID3D12CommandList* cl[] = { list };
        queue->ExecuteCommandLists(1, cl);
        queue->Signal(fence, frame_value);
        wait_fence(fence, ev, frame_value);
        ++frame_value;

        if (f == 0 || f == FRAMES - 1 || f == FRAMES - 2)
        {
            D3D12_RANGE rng {0, 0};
            const std::uint32_t* p = nullptr;
            if (SUCCEEDED(io.rb->Map(0, &rng, reinterpret_cast<void**>(const_cast<std::uint32_t**>(&p)))))
            {
                if (f == 0)
                    std::memcpy(f0.data(), p, f0.size() * 4);
                else if (f == FRAMES - 2)
                    std::memcpy(f38.data(), p, f38.size() * 4);
                else
                    std::memcpy(f39.data(), p, f39.size() * 4);
                io.rb->Unmap(0, nullptr);
            }
        }
    }

    // D3D12 debug 消息（若有）——状态违例等线索
    {
        ComPtr<ID3D12InfoQueue> qi;
        if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&qi))))
        {
            const UINT64 n = qi->GetNumStoredMessages();
            if (n > 0)
            {
                std::printf("[%s] D3D12 debug messages: %llu\n", version_name,
                            static_cast<unsigned long long>(n));
                for (UINT64 i = 0; i < n && i < 30; ++i)
                {
                    SIZE_T len = 0;
                    qi->GetMessage(static_cast<UINT>(i), nullptr, &len);
                    std::vector<std::uint8_t> buf(len);
                    D3D12_MESSAGE* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
                    if (SUCCEEDED(qi->GetMessage(static_cast<UINT>(i), m, &len)) && m->pDescription)
                        std::printf("  [%s] %s\n", version_name, m->pDescription);
                }
            }
            qi->ClearStoredMessages();
        }
    }

    // 强制 reset 的对照帧（在同一累积上下文上，最后一帧 reset=true）
    {
        alloc->Reset();
        list->Reset(alloc, nullptr);
        ffxDispatchDescUpscale d {};
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList = list;
        d.color = ffxApiGetResourceDX12(io.color.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.color.description.format = FFX_API_SURFACE_FORMAT_R10G10B10A2_TYPELESS;
        d.depth = ffxApiGetResourceDX12(io.depth.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.depth.description.format = FFX_API_SURFACE_FORMAT_R32_FLOAT;
        d.motionVectors = ffxApiGetResourceDX12(io.motion.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.motionVectors.description.format = FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
        if (use_reactive)
        {
            d.reactive = ffxApiGetResourceDX12(io.reactive.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
            d.reactive.description.format = FFX_API_SURFACE_FORMAT_R8_UNORM;
        }
        d.output = ffxApiGetResourceDX12(io.output.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        d.output.description.format = FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
        d.jitterOffset.x = 0.0f; // reset 对照帧统一恒定 jitter（避免与主序列 jitter 混杂）
        d.jitterOffset.y = 0.0f;
        d.motionVectorScale.x = static_cast<float>(RW);
        d.motionVectorScale.y = static_cast<float>(RH);
        d.renderSize = {RW, RH};
        d.upscaleSize = {DW, DH};
        d.enableSharpening = false; // 与游戏内一致（RCAS 关闭）
        d.sharpness = 0.0f;
        d.frameTimeDelta = 16.7f;
        d.preExposure = 1.0f;
        d.reset = true;
        d.cameraNear = 0.25f;
        d.cameraFar = 6000.0f;
        d.cameraFovAngleVertical = 0.7853981634f;
        d.viewSpaceToMetersFactor = 1.0f;
        d.flags = 0;
        const ffxReturnCode_t rc = pfn.dispatch(&ctx, reinterpret_cast<const ffxDispatchDescHeader*>(&d));
        std::printf("reset-contra dispatch rc=%d\n", static_cast<int>(rc));
        if (out_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            D3D12_RESOURCE_BARRIER b {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = io.output.Get();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = out_state;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            list->ResourceBarrier(1, &b);
            out_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        if (out_state != D3D12_RESOURCE_STATE_COPY_SOURCE)
        {
            D3D12_RESOURCE_BARRIER b {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = io.output.Get();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = out_state;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1, &b);
            out_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        }
        D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
        dst.pResource = io.rb.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        dst.PlacedFootprint.Footprint.Width = DW;
        dst.PlacedFootprint.Footprint.Height = DH;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = DW * 4;
        src.pResource = io.output.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        list->Close();
        ID3D12CommandList* cl[] = { list };
        queue->ExecuteCommandLists(1, cl);
        queue->Signal(fence, frame_value);
        wait_fence(fence, ev, frame_value);
        D3D12_RANGE rng {0, 0};
        const std::uint32_t* p = nullptr;
        if (SUCCEEDED(io.rb->Map(0, &rng, reinterpret_cast<void**>(const_cast<std::uint32_t**>(&p)))))
        {
            std::memcpy(f40.data(), p, f40.size() * 4);
            io.rb->Unmap(0, nullptr);
        }
    }

    // 分析
    print_edge_row(f0, "f0 reset");
    print_edge_row(f39, "f39 accumulated");
    print_edge_row(f40, "f40 forced-reset");

    // 累积 vs 强制 reset 差异统计（全帧）
    std::uint64_t diff_pixels = 0, max_diff = 0;
    for (UINT i = 0; i < DW * DH; ++i)
    {
        const std::uint32_t a = f39[i], b = f40[i];
        const int da = (a & 0x3FF) - (int)(b & 0x3FF);
        const int db = ((a >> 10) & 0x3FF) - (int)((b >> 10) & 0x3FF);
        const int dc = ((a >> 20) & 0x3FF) - (int)((b >> 20) & 0x3FF);
        if (da || db || dc)
        {
            ++diff_pixels;
            std::uint32_t m = (std::uint32_t)(std::abs(da) > std::abs(db) ? (std::abs(da) > std::abs(dc) ? std::abs(da) : std::abs(dc)) : (std::abs(db) > std::abs(dc) ? std::abs(db) : std::abs(dc)));
            if (m > max_diff) max_diff = m;
        }
    }
    std::printf("[%s|%s] DIFF(f39 accumulated, f40 forced-reset): diff_pixels=%llu/%llu max_channel_delta=%llu\n",
                version_name, flags_tag, static_cast<unsigned long long>(diff_pixels),
                static_cast<unsigned long long>(DW) * DH, static_cast<unsigned long long>(max_diff));
    // 输出非零统计（判断 FSR2 是否真的写了输出）
    std::uint64_t nz39 = 0, nz40 = 0;
    for (UINT i = 0; i < DW * DH; ++i)
    {
        if ((f39[i] & 0x3FF) || ((f39[i] >> 10) & 0x3FF) || ((f39[i] >> 20) & 0x3FF)) ++nz39;
        if ((f40[i] & 0x3FF) || ((f40[i] >> 10) & 0x3FF) || ((f40[i] >> 20) & 0x3FF)) ++nz40;
    }
    std::printf("[%s|%s] nonzero_pixels: f39=%llu f40(reset)=%llu (全 0 => FSR2 未写输出)\n",
                version_name, flags_tag, static_cast<unsigned long long>(nz39), static_cast<unsigned long long>(nz40));
    // 收敛判定（静态输入下决定性的累积证据）：f38 vs f39（连续两帧，均非 reset）
    //   累积工作：连续帧收敛 → f38≈f39（diff 小）
    //   累积失效：每帧独立单帧重建（jitter 不同）→ f38≠f39（diff 大）
    std::uint64_t conv_pixels = 0, conv_max = 0;
    for (UINT i = 0; i < DW * DH; ++i)
    {
        const std::uint32_t a = f38[i], b = f39[i];
        const int da = (a & 0x3FF) - (int)(b & 0x3FF);
        const int db = ((a >> 10) & 0x3FF) - (int)((b >> 10) & 0x3FF);
        const int dc = ((a >> 20) & 0x3FF) - (int)((b >> 20) & 0x3FF);
        if (da || db || dc)
        {
            ++conv_pixels;
            std::uint32_t m = (std::uint32_t)(std::abs(da) > std::abs(db) ? (std::abs(da) > std::abs(dc) ? std::abs(da) : std::abs(dc)) : (std::abs(db) > std::abs(dc) ? std::abs(db) : std::abs(dc)));
            if (m > conv_max) conv_max = m;
        }
    }
    std::printf("[%s|%s] CONVERGENCE(f38 vs f39, 静态输入): diff_pixels=%llu/%llu max_channel_delta=%llu\n",
                version_name, flags_tag, static_cast<unsigned long long>(conv_pixels),
                static_cast<unsigned long long>(DW) * DH, static_cast<unsigned long long>(conv_max));
    std::printf("  -> diff 小 = 累积在收敛（工作）；diff 大 = 每帧单帧重建（累积失效）\n");
    std::printf("  -> diff_pixels==0 => 时间性累积未生效（历史零作用）\n");
    std::printf("  -> diff_pixels>0  => 累积在工作（历史影响输出）\n\n");

    char path[128];
    std::snprintf(path, sizeof(path), "accum_%s_%s_%s_f0.raw", version_name, flags_tag, jit_tag);
    save_raw(path, f0);
    std::snprintf(path, sizeof(path), "accum_%s_%s_%s_f39.raw", version_name, flags_tag, jit_tag);
    save_raw(path, f39);
    std::snprintf(path, sizeof(path), "accum_%s_%s_%s_f40reset.raw", version_name, flags_tag, jit_tag);
    save_raw(path, f40);

    pfn.destroy(&ctx, nullptr);
}

// 单版本安全运行（SEH 捕获崩溃；无 C++ 对象，允许 __try）
static void safe_run_version(const char* name, std::uint64_t id, ID3D12Device* dev, ID3D12CommandQueue* queue,
                             ID3D12CommandAllocator* alloc, ID3D12GraphicsCommandList* list,
                             ID3D12Fence* fence, HANDLE ev, const PfnSet& pfn, const TestIo& io,
                             std::uint32_t ctx_flags, const char* flags_tag,
                             int jit_mode, const char* jit_tag, bool use_reactive)
{
    std::printf(">>> running version %s (id=%llu) flags=0x%03X %s jit=%s\n", name, static_cast<unsigned long long>(id),
                ctx_flags, flags_tag, jit_tag);
    std::fflush(stdout);
    __try
    {
        run_version(name, id, dev, queue, alloc, list, fence, ev, pfn, io, ctx_flags, flags_tag, jit_mode, jit_tag, use_reactive);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        std::printf("!!! version %s CRASHED (SEH 0x%08X) !!!\n", name, static_cast<unsigned>(GetExceptionCode()));
    }
    std::fflush(stdout);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0); // 崩溃时保留已打印输出
    // 适配器选择：argv[1] = 设备 ID（默认 0x7550 = RX 9070 XT；可传 0x13C0 = AMD iGPU 对照 RDNA4 假说）
    std::uint64_t want_dev = 0x7550;
    if (argc > 1)
        want_dev = std::strtoull(argv[1], nullptr, 0);
    std::printf("adapter: want DeviceId=0x%llX\n", static_cast<unsigned long long>(want_dev));
    std::vector<ComPtr<ID3D12Resource>> keepalive_global; // 上传 buffer 存活到程序结束
    // 与桥一致：预加载 amdxc64.dll（runtime FSR2 后端可能需要）
    HMODULE amdxc = LoadLibraryA("amdxc64.dll");
    std::printf("amdxc64.dll load: %s\n", amdxc ? "ok" : "fail");
    ComPtr<ID3D12Debug> dbg;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))))
        dbg->EnableDebugLayer();

    // 枚举适配器，选 RX 9070 XT（DEV 0x7550；游戏 LUID 0x125BE 对照）
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        std::printf("FAIL: CreateDXGIFactory1\n");
        return 1;
    }
    ComPtr<IDXGIAdapter1> chosen;
    for (UINT i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 d {};
        a->GetDesc1(&d);
        std::printf("adapter[%u] %ls LUID=%08X:%08X dev=%04X rev=%04X\n", i, d.Description,
                    d.AdapterLuid.HighPart, d.AdapterLuid.LowPart, d.DeviceId, d.Revision);
        if (d.DeviceId == want_dev && !chosen)
            chosen = a; // 目标适配器
    }
    if (!chosen)
    {
        std::printf("FAIL: adapter DeviceId 0x%llX not found\n", static_cast<unsigned long long>(want_dev));
        return 1;
    }
    ComPtr<ID3D12Device> dev;
    if (FAILED(D3D12CreateDevice(chosen.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev))))
    {
        std::printf("FAIL: D3D12CreateDevice (adapter 0x%llX)\n", static_cast<unsigned long long>(want_dev));
        return 1;
    }
    ComPtr<ID3D12InfoQueue> iq;
    if (SUCCEEDED(dev.As(&iq)))
    {
        iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
        iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
        iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
    }
    std::printf("using adapter DeviceId=0x%llX (debug layer on)\n", static_cast<unsigned long long>(want_dev));
    ComPtr<ID3D12CommandQueue> queue;
    {
        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))))
        {
            std::printf("FAIL: CreateCommandQueue\n");
            return 1;
        }
    }
    ComPtr<ID3D12CommandAllocator> alloc;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))))
        return 1;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))))
        return 1;
    list->Close();
    ComPtr<ID3D12Fence> fence;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        return 1;
    HANDLE ev = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // 加载 runtime
    const wchar_t* dll_path = L"D:\\miHoYo Games\\Starward\\\x539f\x795e\x89e3\x5e27" L"FSR\x63d2\x4ef6\x5305\\payload\\Bridge\\amd_fidelityfx_upscaler_dx12.dll";
    HMODULE mod = LoadLibraryW(dll_path);
    if (!mod)
    {
        std::printf("FAIL: LoadLibrary runtime (err=%lu)\n", GetLastError());
        return 1;
    }
    PfnSet pfn;
    pfn.create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(mod, "ffxCreateContext"));
    pfn.destroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(mod, "ffxDestroyContext"));
    pfn.dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(mod, "ffxDispatch"));
    pfn.query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(mod, "ffxQuery"));
    pfn.configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(mod, "ffxConfigure"));
    if (!pfn.create || !pfn.destroy || !pfn.dispatch || !pfn.query || !pfn.configure)
    {
        std::printf("FAIL: resolve exports\n");
        return 1;
    }

    // 版本查询
    std::uint64_t count = 0;
    {
        ffxQueryDescGetVersions q {};
        q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        q.device = dev.Get();
        q.outputCount = &count;
        pfn.query(nullptr, reinterpret_cast<ffxQueryDescHeader*>(&q));
    }
    std::printf("available versions: count=%llu\n", static_cast<unsigned long long>(count));
    if (count == 0 || count > 16)
        return 1;
    std::vector<std::uint64_t> ids(count + 8, 0);
    std::vector<const char*> names(count + 8, nullptr);
    std::uint64_t capacity = count + 8;
    {
        ffxQueryDescGetVersions q {};
        q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        q.device = dev.Get();
        q.outputCount = &capacity;
        q.versionIds = ids.data();
        q.versionNames = names.data();
        pfn.query(nullptr, reinterpret_cast<ffxQueryDescHeader*>(&q));
    }
    for (std::uint64_t i = 0; i < count; ++i)
        std::printf("  version[%llu] id=%llu name=%s\n", static_cast<unsigned long long>(i),
                    static_cast<unsigned long long>(ids[i]), names[i] ? names[i] : "(null)");

    // 资源（精确复刻游戏内：color/output TYPELESS 输入 → UNORM 输出，enc 初始 COMMON）
    TestIo io;
    io.color = make_tex(dev.Get(), RW, RH, DXGI_FORMAT_R10G10B10A2_TYPELESS, false, D3D12_RESOURCE_STATE_COMMON);
    io.depth = make_tex(dev.Get(), RW, RH, DXGI_FORMAT_R32_FLOAT, false, D3D12_RESOURCE_STATE_COMMON);
    io.motion = make_tex(dev.Get(), RW, RH, DXGI_FORMAT_R16G16_FLOAT, false, D3D12_RESOURCE_STATE_COMMON);
    io.reactive = make_tex(dev.Get(), RW, RH, DXGI_FORMAT_R8_UNORM, false, D3D12_RESOURCE_STATE_COMMON);
    io.output = make_tex(dev.Get(), DW, DH, DXGI_FORMAT_R10G10B10A2_UNORM, true, D3D12_RESOURCE_STATE_COMMON);
    D3D12_HEAP_PROPERTIES hp {};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = DW * DH * 4; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT hr_rb = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&io.rb));
    if (FAILED(hr_rb))
    {
        std::printf("FAIL: create readback hr=0x%08X\n", static_cast<unsigned>(hr_rb));
        return 1;
    }

    // 上传输入（一次）
    {
        alloc->Reset();
        list->Reset(alloc.Get(), nullptr);
        auto color = make_color();
        // 2026-08-25（flag 矩阵）：加载真实 color（R10G10B10A2 4BPP，游戏 PQ HDR 输入）
        {
            FILE* f = nullptr;
            if (fopen_s(&f, "dump_fsr4_in_color_0.raw", "rb") == 0 && f)
            {
                const size_t got = fread(color.data(), 4, RW * RH, f);
                fclose(f);
                std::printf("loaded real game color dump: %zu px\n", got);
            }
        }
        auto depth = make_depth();
        auto motion = make_motion();
        std::vector<std::uint8_t> reactive(RW * RH, 0);
        // 2026-08-25：加载真实 depth（R32 float 4BPP）与真实 motion（R10G10B10A2 平方编码 → R16G16F）
        {
            FILE *f = nullptr;
            if (fopen_s(&f, "dump_fsr4_in_depth_0.raw", "rb") == 0 && f)
            {
                const size_t got = fread(depth.data(), 4, RW * RH, f);
                fclose(f);
                std::printf("loaded real game depth dump: %zu px\n", got);
            }
            if (fopen_s(&f, "dump_fsr4_in_motion_0.raw", "rb") == 0 && f)
            {
                std::vector<std::uint32_t> raw(RW * RH);
                const size_t got = fread(raw.data(), 4, RW * RH, f);
                fclose(f);
                auto f32_to_f16 = [](float v) -> std::uint16_t {
                    // 简单 float→half
                    const std::uint32_t u = *reinterpret_cast<const std::uint32_t*>(&v);
                    const std::uint32_t sign = (u >> 16) & 0x8000u;
                    const std::int32_t exp = static_cast<std::int32_t>((u >> 23) & 0xFF) - 127 + 15;
                    std::uint32_t mant = (u >> 13) & 0x3FFu;
                    if (exp <= 0) return static_cast<std::uint16_t>(sign);
                    if (exp >= 31) return static_cast<std::uint16_t>(sign | 0x7C00u);
                    return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exp) << 10) | mant);
                };
                for (UINT i = 0; i < RW * RH; ++i)
                {
                    const std::uint32_t v = raw[i];
                    const float raw_r = static_cast<float>(v & 0x3FFu) / 1023.0f;
                    const float raw_g = static_cast<float>((v >> 10) & 0x3FFu) / 1023.0f;
                    const float dr = raw_r - 0.498039f;
                    const float dg = raw_g - 0.498039f;
                    const float mvr = (dr < 0.0f) ? 4.0f * dr * dr : -4.0f * dr * dr; // −sign(d)·4d²
                    const float mvg = (dg < 0.0f) ? 4.0f * dg * dg : -4.0f * dg * dg;
                    motion[i * 2] = f32_to_f16(mvr);
                    motion[i * 2 + 1] = f32_to_f16(mvg);
                }
                std::printf("loaded+decoded real game motion dump: %zu px\n", got);
            }
        }
        if (!upload_tex(dev.Get(), list.Get(), io.color.Get(), color.data(), RW, RH, 4,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, keepalive_global) ||
            !upload_tex(dev.Get(), list.Get(), io.depth.Get(), depth.data(), RW, RH, 4,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, keepalive_global) ||
            !upload_tex(dev.Get(), list.Get(), io.motion.Get(), motion.data(), RW, RH, 4,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, keepalive_global) ||
            !upload_tex(dev.Get(), list.Get(), io.reactive.Get(), reactive.data(), RW, RH, 1,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, keepalive_global))
        {
            std::printf("FAIL: upload inputs\n");
            return 1;
        }
        list->Close();
        ID3D12CommandList* cmdlists[] = { list.Get() };
        queue->ExecuteCommandLists(1, cmdlists);
        fence->Signal(1);
        wait_fence(fence.Get(), ev, 1);

        // 验证输入上传：读回 color 并保存 input_color.raw（GetCopyableFootprints 真实 pitch）
        {
            alloc->Reset();
            list->Reset(alloc.Get(), nullptr);
            D3D12_RESOURCE_DESC cdesc = io.color->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT cfp {};
            dev->GetCopyableFootprints(&cdesc, 0, 1, 0, &cfp, nullptr, nullptr, nullptr);
            D3D12_RESOURCE_BARRIER b {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = io.color.Get();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1, &b);
            D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
            dst.pResource = io.rb.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R10G10B10A2_TYPELESS;
            dst.PlacedFootprint.Footprint.Width = RW;
            dst.PlacedFootprint.Footprint.Height = RH;
            dst.PlacedFootprint.Footprint.Depth = 1;
            dst.PlacedFootprint.Footprint.RowPitch = cfp.Footprint.RowPitch;
            src.pResource = io.color.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            list->ResourceBarrier(1, &b);
            list->Close();
            ID3D12CommandList* cl2[] = { list.Get() };
            queue->ExecuteCommandLists(1, cl2);
            fence->Signal(2);
            wait_fence(fence.Get(), ev, 2);
            D3D12_RANGE rng {0, 0};
            const std::uint32_t* p = nullptr;
            if (SUCCEEDED(io.rb->Map(0, &rng, reinterpret_cast<void**>(const_cast<std::uint32_t**>(&p)))))
            {
                std::printf("input copyable row pitch = %u\n", cfp.Footprint.RowPitch);
                for (const auto& xy : {std::pair<UINT, UINT>{100u, 200u}, {200u, 100u}, {960u, 540u}, {0u, 0u}})
                {
                    const std::uint32_t v = p[xy.second * (cfp.Footprint.RowPitch / 4) + xy.first];
                    std::printf("input color (%u,%u)=0x%08X\n", xy.first, xy.second, v);
                }
                FILE* f = nullptr;
                if (fopen_s(&f, "input_color.raw", "wb") == 0 && f)
                {
                    fwrite(p, 4, RW * RH, f); // 密集行（RowPitch=4*RW 时正确）
                    fclose(f);
                }
                io.rb->Unmap(0, nullptr);
            }
        }
    }

    // 逐版本跑（2.3.4 优先——目标版本；SEH 保护单版本崩溃）
    std::printf("main: starting version loop...\n");
    const std::vector<std::string> order = {"2.3.4", "3.1.5", "4.1.1"};
    std::printf("main: order built, count=%llu\n", static_cast<unsigned long long>(count));
    // flag 组合矩阵：0=基线、单 flag 各测、0x129=游戏内全套（HDR|DEPTH_INVERTED|AUTO_EXPOSURE|NON_LINEAR）
    const std::uint32_t combos_flags[] = {0x000u, 0x008u, 0x020u, 0x001u, 0x100u, 0x129u};
    const char* combos_tag[] = {"f0", "di", "ae", "hdr", "nl", "ingame"};
    const std::size_t combo_n = sizeof(combos_flags) / sizeof(combos_flags[0]);
    for (std::size_t ci = 0; ci < combo_n; ++ci)
    {
        std::printf("main: ===== flag combo %s (0x%03X) =====\n", combos_tag[ci], combos_flags[ci]);
        for (const auto& want : order)
        {
            for (std::uint64_t i = 0; i < count; ++i)
            {
                if (!names[i] || names[i][0] == '\0' || want != names[i])
                    continue;
                safe_run_version(names[i], ids[i], dev.Get(), queue.Get(), alloc.Get(), list.Get(),
                                 fence.Get(), ev, pfn, io, combos_flags[ci], combos_tag[ci], 0, "jit", false);
                break;
            }
        }
    }

    // jitter A/B（判定 jitterOffset 是否到达 FSR2 采样）：2.3.4 × ingame flags ×
    //   jit0=halton 变化 / jit0px=恒定 0 / jit5px=恒定 5px
    std::printf("main: ===== jitter A/B (2.3.4 ingame) =====\n");
    {
        const int jit_modes[] = {0, 1, 2, 3};
        const char* jit_tags[] = {"jit_minus", "jit0px", "jit5px", "jit_plus"};
        for (int jm = 0; jm < 4; ++jm)
        {
            for (std::uint64_t i = 0; i < count; ++i)
            {
                if (!names[i] || names[i][0] == '\0' || std::string("2.3.4") != names[i])
                    continue;
                safe_run_version(names[i], ids[i], dev.Get(), queue.Get(), alloc.Get(), list.Get(),
                                 fence.Get(), ev, pfn, io, 0x129u, "ingame", jit_modes[jm], jit_tags[jm], false);
                break;
            }
        }
    }

    // 可选 reactive 的离线 API 验证：只传已上传的 R8 零 mask，不经 UAV 写入。
    // 若这里失败/黑屏，问题是 runtime 接受或资源声明；若正常，则游戏黑屏来自此前的自定义 UAV pass。
    std::printf("main: ===== reactive R8 API probe (4.1.1) =====\n");
    for (std::uint64_t i = 0; i < count; ++i)
    {
        if (!names[i] || names[i][0] == '\0' || std::string("4.1.1") != names[i])
            continue;
        safe_run_version(names[i], ids[i], dev.Get(), queue.Get(), alloc.Get(), list.Get(),
                         fence.Get(), ev, pfn, io, 0x008u, "di_reactive", 0, "jit", true);
        break;
    }

    std::printf("DONE\n");
    return 0;
}
