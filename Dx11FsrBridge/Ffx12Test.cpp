// Ffx12Test.cpp — Phase 0: in-process FSR2 (ffxApi) integration test.
// Chain under test: D3D11 shared textures -> D3D12 OpenSharedHandle ->
// amd_fidelityfx_upscaler_dx12.dll (ffxCreateContext/ffxDispatch) -> output readback.
// Also queries the bundled FSR version (ffxQueryDescGetVersions).
// ASCII-only source (CP936 hazard).
#include <Windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdint>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ffx_api.h"
#include "ffx_upscale.h"
#include "dx12/ffx_api_dx12.h"

using namespace Microsoft::WRL;

namespace
{
// Loaded from the exe directory (build step copies it next to the test).
const char *k_upscaler_dll = "amd_fidelityfx_upscaler_dx12.dll";
constexpr UINT k_render_w = 960, k_render_h = 540;
constexpr UINT k_display_w = 1920, k_display_h = 1080;

int g_failures = 0;
#define CHECK(cond, msg)                                       \
    do                                                         \
    {                                                          \
        if (!(cond))                                           \
        {                                                      \
            std::printf("FAIL: %s\n", std::string(msg).c_str()); \
            ++g_failures;                                      \
        }                                                      \
        else                                                   \
        {                                                      \
            std::printf("ok:   %s\n", std::string(msg).c_str()); \
        }                                                      \
    } while (0)

ComPtr<ID3D11Device> g_d11dev;
ComPtr<ID3D11DeviceContext> g_d11ctx;
ComPtr<ID3D12Device> g_d12dev;
ComPtr<ID3D12CommandQueue> g_queue;
ComPtr<ID3D12CommandAllocator> g_allocator;
ComPtr<ID3D12GraphicsCommandList> g_cmdlist;
ComPtr<ID3D12Fence> g_fence;
HANDLE g_fence_event = nullptr;
UINT64 g_fence_value = 0;

struct SharedTex
{
    ComPtr<ID3D11Texture2D> d11;
    ComPtr<ID3D12Resource> d12;
    HANDLE handle = nullptr;
    bool nt_handle = false;
};

ComPtr<ID3D11Texture2D> make_d11_shared(UINT w, UINT h, DXGI_FORMAT fmt, UINT bind_flags, bool use_nt)
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
    // D3D11 requires SHARED_KEYEDMUTEX together with SHARED_NTHANDLE.  The
    // previous probe used NTHANDLE alone and therefore tested an invalid desc.
    d.MiscFlags = use_nt
        ? (D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)
        : D3D11_RESOURCE_MISC_SHARED;
    ComPtr<ID3D11Texture2D> t;
    const HRESULT hr = g_d11dev->CreateTexture2D(&d, nullptr, &t);
    if (FAILED(hr))
        std::printf("  make_d11_shared fmt=%u bind=0x%X nt=%d failed hr=0x%08X\n", fmt, bind_flags,
                    use_nt ? 1 : 0, static_cast<unsigned>(hr));
    return t;
}

bool open_shared(SharedTex &st)
{
    if (st.nt_handle)
    {
        ComPtr<IDXGIResource1> dxgi;
        if (FAILED(st.d11.As(&dxgi)))
            return false;
        if (FAILED(dxgi->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &st.handle)))
            return false;
    }
    else
    {
        ComPtr<IDXGIResource> dxgi;
        if (FAILED(st.d11.As(&dxgi)))
            return false;
        if (FAILED(dxgi->GetSharedHandle(&st.handle)))
            return false;
    }
    if (FAILED(g_d12dev->OpenSharedHandle(st.handle, IID_PPV_ARGS(&st.d12))))
        return false;
    return true;
}

void fill_color_gradient(ComPtr<ID3D11Texture2D> &tex)
{
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    ComPtr<ID3D11Texture2D> staging;
    D3D11_TEXTURE2D_DESC s = d;
    s.Usage = D3D11_USAGE_STAGING;
    s.BindFlags = 0;
    s.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    g_d11dev->CreateTexture2D(&s, nullptr, &staging);
    D3D11_MAPPED_SUBRESOURCE map {};
    if (SUCCEEDED(g_d11ctx->Map(staging.Get(), 0, D3D11_MAP_WRITE, 0, &map)))
    {
        const UINT h = d.Height, w = d.Width;
        for (UINT y = 0; y < h; ++y)
        {
            auto *row = static_cast<std::uint8_t *>(map.pData) + static_cast<std::size_t>(y) * map.RowPitch;
            for (UINT x = 0; x < w; ++x)
            {
                row[x * 4 + 0] = static_cast<std::uint8_t>(x * 255 / w);
                row[x * 4 + 1] = static_cast<std::uint8_t>(y * 255 / h);
                row[x * 4 + 2] = 128;
                row[x * 4 + 3] = 255;
            }
        }
        g_d11ctx->Unmap(staging.Get(), 0);
    }
    g_d11ctx->CopyResource(tex.Get(), staging.Get());
}

void fill_constant(ComPtr<ID3D11Texture2D> &tex, std::uint32_t value)
{
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    ComPtr<ID3D11Texture2D> staging;
    D3D11_TEXTURE2D_DESC s = d;
    s.Usage = D3D11_USAGE_STAGING;
    s.BindFlags = 0;
    s.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    g_d11dev->CreateTexture2D(&s, nullptr, &staging);
    D3D11_MAPPED_SUBRESOURCE map {};
    if (SUCCEEDED(g_d11ctx->Map(staging.Get(), 0, D3D11_MAP_WRITE, 0, &map)))
    {
        const UINT h = d.Height;
        const UINT bpp = (d.Format == DXGI_FORMAT_R16G16_FLOAT) ? 4 : (d.Format == DXGI_FORMAT_R32_FLOAT) ? 4 : 4;
        const UINT row_bytes = d.Width * bpp;
        for (UINT y = 0; y < h; ++y)
        {
            auto *row = static_cast<std::uint8_t *>(map.pData) + static_cast<std::size_t>(y) * map.RowPitch;
            std::uint8_t *src = reinterpret_cast<std::uint8_t *>(&value);
            for (UINT x = 0; x < row_bytes; ++x)
                row[x] = src[x % 4];
        }
        g_d11ctx->Unmap(staging.Get(), 0);
    }
    g_d11ctx->CopyResource(tex.Get(), staging.Get());
}

void verify_output(ComPtr<ID3D11Texture2D> &tex)
{
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    const UINT w = d.Width, h = d.Height;
    ComPtr<ID3D11Texture2D> staging;
    D3D11_TEXTURE2D_DESC s = d;
    s.Usage = D3D11_USAGE_STAGING;
    s.BindFlags = 0;
    s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    g_d11dev->CreateTexture2D(&s, nullptr, &staging);
    g_d11ctx->CopyResource(staging.Get(), tex.Get());
    D3D11_MAPPED_SUBRESOURCE map {};
    std::uint64_t sum = 0;
    std::uint32_t nonzero = 0;
    if (SUCCEEDED(g_d11ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)))
    {
        for (UINT y = 0; y < h; y += 4)
        {
            const auto *row = static_cast<const std::uint8_t *>(map.pData) + static_cast<std::size_t>(y) * map.RowPitch;
            for (UINT x = 0; x < w; x += 4)
            {
                const std::uint32_t v = row[x * 4];
                sum += v;
                if (v != 0)
                    ++nonzero;
            }
        }
        g_d11ctx->Unmap(staging.Get(), 0);
    }
    const std::uint64_t samples = (static_cast<std::uint64_t>(w / 4) * (h / 4));
    std::printf("  output %ux%u sum=%llu nonzero=%u/%llu\n", w, h,
                static_cast<unsigned long long>(sum), nonzero,
                static_cast<unsigned long long>(samples));
    CHECK(nonzero > samples / 4, "output contains meaningful content");
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

// Capability probe for the production GPU-only transport direction:
// D3D12 owns a shareable texture, D3D11 imports it, writes via CopyResource,
// then D3D12 waits on a shared fence and reads the exact bytes back.
bool probe_d3d12_owned_shared_texture()
{
    constexpr UINT k_w = 64, k_h = 64;
    ComPtr<ID3D11Device1> d11_1;
    ComPtr<ID3D11Device5> d11_5;
    ComPtr<ID3D11DeviceContext4> ctx4;
    if (FAILED(g_d11dev.As(&d11_1)) || FAILED(g_d11dev.As(&d11_5)) || FAILED(g_d11ctx.As(&ctx4)))
    {
        std::printf("  gpu-only probe unavailable: D3D11.1/5 or context4 missing\n");
        return false;
    }

    D3D12_RESOURCE_DESC td {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = k_w;
    td.Height = k_h;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc = {1, 0};
    D3D12_HEAP_PROPERTIES hp {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> shared12;
    HRESULT hr = g_d12dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &td,
                                                    D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                    IID_PPV_ARGS(&shared12));
    if (FAILED(hr))
    {
        std::printf("  gpu-only probe CreateCommittedResource hr=0x%08X\n", static_cast<unsigned>(hr));
        return false;
    }
    HANDLE texture_handle = nullptr;
    hr = g_d12dev->CreateSharedHandle(shared12.Get(), nullptr, GENERIC_ALL, nullptr, &texture_handle);
    if (FAILED(hr) || !texture_handle)
    {
        std::printf("  gpu-only probe CreateSharedHandle(texture) hr=0x%08X\n", static_cast<unsigned>(hr));
        return false;
    }
    ComPtr<ID3D11Texture2D> shared11;
    hr = d11_1->OpenSharedResource1(texture_handle, IID_PPV_ARGS(&shared11));
    CloseHandle(texture_handle);
    if (FAILED(hr) || !shared11)
    {
        std::printf("  gpu-only probe OpenSharedResource1(texture) hr=0x%08X\n", static_cast<unsigned>(hr));
        return false;
    }

    ComPtr<ID3D12Fence> fence12;
    hr = g_d12dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence12));
    HANDLE fence_handle = nullptr;
    if (SUCCEEDED(hr))
        hr = g_d12dev->CreateSharedHandle(fence12.Get(), nullptr, GENERIC_ALL, nullptr, &fence_handle);
    ComPtr<ID3D11Fence> fence11;
    if (SUCCEEDED(hr))
        hr = d11_5->OpenSharedFence(fence_handle, IID_PPV_ARGS(&fence11));
    if (fence_handle)
        CloseHandle(fence_handle);
    if (FAILED(hr) || !fence11)
    {
        std::printf("  gpu-only probe shared fence hr=0x%08X\n", static_cast<unsigned>(hr));
        return false;
    }

    D3D11_TEXTURE2D_DESC sd {};
    sd.Width = k_w; sd.Height = k_h; sd.MipLevels = 1; sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_DEFAULT; sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::array<std::uint32_t, k_w * k_h> pixels {};
    pixels.fill(0xFF332211u);
    D3D11_SUBRESOURCE_DATA initial {};
    initial.pSysMem = pixels.data();
    initial.SysMemPitch = k_w * sizeof(std::uint32_t);
    ComPtr<ID3D11Texture2D> source11;
    hr = g_d11dev->CreateTexture2D(&sd, &initial, &source11);
    if (FAILED(hr))
        return false;
    g_d11ctx->CopyResource(shared11.Get(), source11.Get());
    hr = ctx4->Signal(fence11.Get(), 1);
    if (FAILED(hr) || FAILED(g_queue->Wait(fence12.Get(), 1)))
    {
        std::printf("  gpu-only probe D3D11->D3D12 fence hr=0x%08X\n", static_cast<unsigned>(hr));
        return false;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT64 total = 0;
    g_d12dev->GetCopyableFootprints(&td, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    D3D12_RESOURCE_DESC bd {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1;
    bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback;
    hr = g_d12dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                           IID_PPV_ARGS(&readback));
    if (FAILED(hr))
        return false;
    g_cmdlist->Close();
    g_allocator->Reset();
    g_cmdlist->Reset(g_allocator.Get(), nullptr);
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = shared12.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g_cmdlist->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = shared12.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g_cmdlist->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    g_cmdlist->Close();
    ID3D12CommandList *lists[] = {g_cmdlist.Get()};
    g_queue->ExecuteCommandLists(1, lists);
    wait_gpu();
    void *mapped = nullptr;
    D3D12_RANGE range {0, static_cast<SIZE_T>(total)};
    hr = readback->Map(0, &range, &mapped);
    std::uint32_t value = 0;
    if (SUCCEEDED(hr) && mapped)
    {
        std::memcpy(&value, mapped, sizeof(value));
        readback->Unmap(0, nullptr);
    }
    const bool ok = SUCCEEDED(hr) && value == 0xFF332211u;
    std::printf("  gpu-only probe result=%s value=0x%08X\n", ok ? "ok" : "bad", value);
    return ok;
}
} // namespace

int main()
{
    std::printf("Ffx12Test\n");
    std::fflush(stdout);

    // ---- D3D11 + shared textures ----
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                   D3D11_SDK_VERSION, &g_d11dev, nullptr, &g_d11ctx);
    CHECK(SUCCEEDED(hr), "D3D11 device");
    {
        D3D_FEATURE_LEVEL fl = g_d11dev->GetFeatureLevel();
        std::printf("  D3D11 feature level: 0x%X (%s)\n", fl, fl >= D3D_FEATURE_LEVEL_11_1 ? "11.1+" : "11.0");
        ComPtr<ID3D11Device1> d1;
        if (SUCCEEDED(g_d11dev.As(&d1)))
            std::printf("  D3D11.1 runtime: available\n");
        else
            std::printf("  D3D11.1 runtime: NOT available (SHARED_NTHANDLE unsupported)\n");
    }
    std::fflush(stdout);

    SharedTex color, depth, motion, output;
    // 优先 NT handle（D3D12 互操作首选），失败回退传统 SHARED
    color.nt_handle = true;
    depth.nt_handle = true;
    motion.nt_handle = true;
    output.nt_handle = true;
    color.d11 = make_d11_shared(k_render_w, k_render_h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, true);
    if (!color.d11) { color.nt_handle = false; color.d11 = make_d11_shared(k_render_w, k_render_h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, false); }
    depth.d11 = make_d11_shared(k_render_w, k_render_h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, true);
    if (!depth.d11) { depth.nt_handle = false; depth.d11 = make_d11_shared(k_render_w, k_render_h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, false); }
    motion.d11 = make_d11_shared(k_render_w, k_render_h, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE, true);
    if (!motion.d11) { motion.nt_handle = false; motion.d11 = make_d11_shared(k_render_w, k_render_h, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE, false); }
    output.d11 = make_d11_shared(k_display_w, k_display_h, DXGI_FORMAT_R8G8B8A8_UNORM,
                                 D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, true);
    if (!output.d11) { output.nt_handle = false; output.d11 = make_d11_shared(k_display_w, k_display_h, DXGI_FORMAT_R8G8B8A8_UNORM,
                                 D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, false); }
    CHECK(color.d11 && depth.d11 && motion.d11 && output.d11, "D3D11 shared textures created");
    std::printf("  shared mode: color=%s depth=%s motion=%s output=%s\n",
                color.nt_handle ? "nt" : "legacy", depth.nt_handle ? "nt" : "legacy",
                motion.nt_handle ? "nt" : "legacy", output.nt_handle ? "nt" : "legacy");
    if (!(color.d11 && depth.d11 && motion.d11 && output.d11))
    {
        std::printf("aborting: shared texture creation failed\n");
        return 1;
    }
    std::fflush(stdout);

    fill_color_gradient(color.d11);
    std::uint32_t depth_val = 0x3F000000; // 0.5f as float bits
    fill_constant(depth.d11, depth_val);
    std::uint32_t motion_val = 0; // (0,0)
    fill_constant(motion.d11, motion_val);
    // 提交 D3D11 命令并等待（一次性测试：Flush+SLEEP 足够；正式桥需要跨 API fence）
    g_d11ctx->Flush();
    Sleep(200);
    std::printf("  inputs filled + flushed\n");
    std::fflush(stdout);

    // ---- D3D12 ----
    hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_d12dev));
    CHECK(SUCCEEDED(hr), "D3D12 device");
    // 早期读回创建自检（确认 desc/设备可用）
    {
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = k_display_w;
        rd.Height = k_display_h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc = {1, 0};
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; // 读回堆纹理必须显式 ROW_MAJOR
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
        hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
        ComPtr<ID3D12Resource> probe;
        const HRESULT ph = g_d12dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                             IID_PPV_ARGS(&probe));
        std::printf("  early readback probe (explicit) hr=0x%08X\n", static_cast<unsigned>(ph));
        D3D12_HEAP_PROPERTIES hp_auto {};
        hp_auto.Type = D3D12_HEAP_TYPE_READBACK;
        ComPtr<ID3D12Resource> probe_a;
        const HRESULT pha = g_d12dev->CreateCommittedResource(&hp_auto, D3D12_HEAP_FLAG_NONE, &rd,
                                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                              IID_PPV_ARGS(&probe_a));
        std::printf("  early readback probe (auto) hr=0x%08X\n", static_cast<unsigned>(pha));
        // 最简 BUFFER 读回对照
        D3D12_RESOURCE_DESC bd {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 4096;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc = {1, 0};
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> probe_b;
        const HRESULT phb = g_d12dev->CreateCommittedResource(&hp_auto, D3D12_HEAP_FLAG_NONE, &bd,
                                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                              IID_PPV_ARGS(&probe_b));
        std::printf("  readback BUFFER probe hr=0x%08X\n", static_cast<unsigned>(phb));
        // UPLOAD 堆对照
        D3D12_HEAP_PROPERTIES hp_up {};
        hp_up.Type = D3D12_HEAP_TYPE_UPLOAD;
        ComPtr<ID3D12Resource> probe_u;
        const HRESULT phu = g_d12dev->CreateCommittedResource(&hp_up, D3D12_HEAP_FLAG_NONE, &bd,
                                                              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                              IID_PPV_ARGS(&probe_u));
        std::printf("  UPLOAD BUFFER probe hr=0x%08X\n", static_cast<unsigned>(phu));
        // WARP 控制组
        ComPtr<IDXGIFactory4> factory;
        ComPtr<ID3D12Device> warp_dev;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        {
            ComPtr<IDXGIAdapter> warp_adapter;
            if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp_adapter))) &&
                SUCCEEDED(D3D12CreateDevice(warp_adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                            IID_PPV_ARGS(&warp_dev))))
            {
                ComPtr<ID3D12Resource> probe_w;
                const HRESULT phw = warp_dev->CreateCommittedResource(&hp_auto, D3D12_HEAP_FLAG_NONE, &rd,
                                                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                      IID_PPV_ARGS(&probe_w));
                std::printf("  WARP readback probe hr=0x%08X\n", static_cast<unsigned>(phw));
            }
        }
    }
    std::fflush(stdout);
    std::fflush(stdout);
    D3D12_COMMAND_QUEUE_DESC qd {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    g_d12dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue));
    g_d12dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_allocator));
    g_d12dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocator.Get(), nullptr,
                                IID_PPV_ARGS(&g_cmdlist));
    g_d12dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    g_fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    CHECK(g_queue && g_allocator && g_cmdlist && g_fence && g_fence_event, "D3D12 queue/allocator/list/fence");
    std::fflush(stdout);

    CHECK(probe_d3d12_owned_shared_texture(),
          "GPU-only probe: D3D12-owned NT shared texture + shared fence + D3D11 import");
    std::fflush(stdout);

    CHECK(open_shared(color) && open_shared(depth) && open_shared(motion) && open_shared(output),
          "shared handles opened on D3D12");
    std::fflush(stdout);

    // ---- load the AMD upscaler (ffxApi) ----
    HMODULE mod = LoadLibraryA(k_upscaler_dll);
    CHECK(mod != nullptr, "amd_fidelityfx_upscaler_dx12.dll loaded (err=" +
                              std::to_string(GetLastError()) + ")");
    std::fflush(stdout);
    if (!mod)
        return 1;
    auto pfn_create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(mod, "ffxCreateContext"));
    auto pfn_destroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(mod, "ffxDestroyContext"));
    auto pfn_dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(mod, "ffxDispatch"));
    auto pfn_query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(mod, "ffxQuery"));
    std::printf("  entry: create=%p destroy=%p dispatch=%p query=%p\n", pfn_create, pfn_destroy, pfn_dispatch, pfn_query);
    std::fflush(stdout);
    CHECK(pfn_create && pfn_destroy && pfn_dispatch && pfn_query, "ffxApi entry points resolved");
    std::fflush(stdout);

    // ---- query bundled FSR versions ----
    std::uint64_t version_count = 0;
    std::vector<std::uint64_t> version_ids(16, 0);          // 容量放大吸收可能的越界写
    std::vector<const char *> version_names(16, nullptr);
    {
        ffxQueryDescGetVersions q {};
        q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        q.device = g_d12dev.Get();
        std::uint64_t count = 0;
        q.outputCount = &count;
        ffxReturnCode_t rc = pfn_query(nullptr, reinterpret_cast<ffxQueryDescHeader *>(&q));
        std::printf("  version query rc=%u count=%llu\n", rc, static_cast<unsigned long long>(count));
        std::fflush(stdout);
        CHECK(rc == FFX_API_RETURN_OK, "ffxQuery get versions ok");
        version_count = count < 16 ? count : 16;
        std::uint64_t capacity = 16;
        q.outputCount = &capacity;
        q.versionIds = version_ids.data();
        q.versionNames = version_names.data();
        std::printf("  second query...\n");
        std::fflush(stdout);
        rc = pfn_query(nullptr, reinterpret_cast<ffxQueryDescHeader *>(&q));
        std::printf("  second query rc=%u filled=%llu\n", rc, static_cast<unsigned long long>(capacity));
        std::fflush(stdout);
        for (std::uint64_t i = 0; i < version_count; ++i)
            std::printf("  version[%llu] id=%llu nameptr=%p\n", static_cast<unsigned long long>(i),
                        static_cast<unsigned long long>(version_ids[i]),
                        static_cast<const void *>(version_names[i]));
        std::fflush(stdout);
    }

    // ---- create FSR upscale context ----
    ffxContext ctx = nullptr;
    {
        const auto name_valid = [](const char *p) {
            MEMORY_BASIC_INFORMATION mbi {};
            if (p == nullptr || VirtualQuery(p, &mbi, sizeof(mbi)) == 0)
                return false;
            return mbi.State == MEM_COMMIT &&
                   (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0;
        };
        std::uint64_t version_id_234 = 0;
        for (std::uint64_t i = 0; i < version_count; ++i)
            if (name_valid(version_names[i]) && std::strcmp(version_names[i], "2.3.4") == 0)
                version_id_234 = version_ids[i];
        std::printf("  2.3.4 version id = %llu\n", static_cast<unsigned long long>(version_id_234));
        std::fflush(stdout);

        ffxCreateBackendDX12Desc backend {};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = g_d12dev.Get();

        // 版本覆盖链（backend 在前 → override 在后）：选择 2.3.4
        ffxOverrideVersion override_version {};
        override_version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        override_version.versionId = version_id_234;

        ffxCreateContextDescUpscale desc {};
        desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        desc.header.pNext = &backend.header;
        backend.header.pNext = &override_version.header;
        desc.maxRenderSize = {k_render_w, k_render_h};
        desc.maxUpscaleSize = {k_display_w, k_display_h};
        desc.flags = 0;
        ffxReturnCode_t rc = pfn_create(&ctx, reinterpret_cast<ffxCreateContextDescHeader *>(&desc), nullptr);
        std::printf("  ffxCreateContext(2.3.4 override) rc=%u\n", rc);
        std::fflush(stdout);
        CHECK(rc == FFX_API_RETURN_OK, "ffxCreateContext (upscale 2.3.4 override) ok");
    }

    // ---- dispatch ----
    if (ctx)
    {
        g_allocator->Reset();
        g_cmdlist->Reset(g_allocator.Get(), nullptr);
        ffxDispatchDescUpscale d {};
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList = g_cmdlist.Get();
        d.color = ffxApiGetResourceDX12(color.d12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.color.description.format = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
        d.depth = ffxApiGetResourceDX12(depth.d12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.depth.description.format = FFX_API_SURFACE_FORMAT_R32_FLOAT;
        d.motionVectors = ffxApiGetResourceDX12(motion.d12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.motionVectors.description.format = FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
        d.output = ffxApiGetResourceDX12(output.d12.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        d.output.description.format = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
        d.jitterOffset = {0.0f, 0.0f};
        d.motionVectorScale = {1.0f, 1.0f};
        d.renderSize = {k_render_w, k_render_h};
        d.upscaleSize = {k_display_w, k_display_h};
        d.enableSharpening = false;
        d.frameTimeDelta = 16.7f;
        d.preExposure = 1.0f;
        d.reset = true;
        d.cameraNear = 0.1f;
        d.cameraFar = 1000.0f;
        d.cameraFovAngleVertical = 1.0472f;
        d.viewSpaceToMetersFactor = 1.0f;
        d.flags = 0;
        ffxReturnCode_t rc = pfn_dispatch(&ctx, reinterpret_cast<const ffxDispatchDescHeader *>(&d));
        std::printf("  ffxDispatch rc=%u\n", rc);
        std::fflush(stdout);
        CHECK(rc == FFX_API_RETURN_OK, "ffxDispatch (upscale) ok");
        std::printf("  closing/executing...\n");
        std::fflush(stdout);
        g_cmdlist->Close();
        ID3D12CommandList *lists[] = {g_cmdlist.Get()};
        g_queue->ExecuteCommandLists(1, lists);
        wait_gpu();
        std::printf("  gpu done, readback...\n");
        std::fflush(stdout);

        // D3D12 侧直读交叉验证（读回缓冲方式：本环境读回纹理 E_INVALIDARG，buffer 正常）
        {
            D3D12_RESOURCE_DESC od = output.d12->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
            UINT64 total_bytes = 0;
            g_d12dev->GetCopyableFootprints(&od, 0, 1, 0, &footprint, nullptr, nullptr, &total_bytes);
            D3D12_RESOURCE_DESC bd {};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = total_bytes;
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.SampleDesc = {1, 0};
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES hp {};
            hp.Type = D3D12_HEAP_TYPE_READBACK;
            ComPtr<ID3D12Resource> readback;
            const HRESULT cr_hr = g_d12dev->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&readback));
            std::printf("  readback buffer create hr=0x%08X footprint=%llu\n",
                        static_cast<unsigned>(cr_hr), static_cast<unsigned long long>(total_bytes));
            std::fflush(stdout);
            if (SUCCEEDED(cr_hr) && readback)
            {
                g_allocator->Reset();
                g_cmdlist->Reset(g_allocator.Get(), nullptr);
                D3D12_TEXTURE_COPY_LOCATION dst_loc {};
                dst_loc.pResource = readback.Get();
                dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst_loc.PlacedFootprint = footprint;
                D3D12_TEXTURE_COPY_LOCATION src_loc {};
                src_loc.pResource = output.d12.Get();
                src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src_loc.SubresourceIndex = 0;
                g_cmdlist->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
                g_cmdlist->Close();
                ID3D12CommandList *copy_lists[] = {g_cmdlist.Get()};
                g_queue->ExecuteCommandLists(1, copy_lists);
                wait_gpu();
                void *mapped = nullptr;
                readback->Map(0, nullptr, &mapped);
                const UINT64 row_bytes = footprint.Footprint.RowPitch;
                std::uint64_t sum12 = 0;
                std::uint32_t nonzero12 = 0;
                const auto *src = static_cast<const std::uint8_t *>(mapped);
                for (UINT y = 0; y < k_display_h; y += 4)
                    for (UINT x = 0; x < k_display_w; x += 4)
                    {
                        const std::uint32_t v = src[static_cast<std::size_t>(y) * row_bytes + x * 4];
                        sum12 += v;
                        if (v != 0)
                            ++nonzero12;
                    }
                readback->Unmap(0, nullptr);
                std::printf("  d3d12 readback sum=%llu nonzero=%u\n",
                            static_cast<unsigned long long>(sum12), nonzero12);
            }
        }

        verify_output(output.d11);
        pfn_destroy(&ctx, nullptr);
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "FAILURES: %d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
