// GpuInteropProbe.cpp — 离线验证 D3D11 ↔ D3D12 纯 GPU 互操作（无 On12、无 CPU staging）。
//
// 目的（第八十六轮重审"DX11 模式无法纯 GPU 直通 FFX12"前提）：
//   旧结论来自 2026-08-24"legacy 共享纹理在 D3D12 读为 0"——当时用的是
//   D3D11_RESOURCE_MISC_SHARED（legacy 共享面句柄），D3D12 的 OpenSharedHandle
//   不支持该句柄类型。正确机制是 D3D11_RESOURCE_MISC_SHARED_NTHANDLE +
//   IDXGIResource1::CreateSharedHandle + ID3D12Device::OpenSharedHandle，
//   并用 D3D11.4 共享 fence（ID3D11Device5::OpenSharedFence）双向同步。
//
//   Phase A：D3D11 侧自建 SHARED|SHARED_NTHANDLE 纹理 → D3D12 OpenSharedHandle →
//            D3D11 GPU CopyResource 写入 → fence → D3D12 GPU 读回验证（生产主方向）。
//   Phase B：D3D12 侧对共享输出纹理 GPU 写入 → fence → D3D11 GPU 拷贝 → Map 验证。
//   Phase C（诊断保留）：D3D12 建共享纹理 → D3D11 OpenSharedResource1（对照）。
// 任一方向通过即可推翻"纯 GPU 链路不可行"前提；全部失败则输出精确 HRESULT 与适配器信息。

#include <Windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace
{
void line(const char *text)
{
    std::printf("%s\n", text);
    std::fflush(stdout);
}

const char *hres(HRESULT hr)
{
    static char buf[64];
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(hr));
    return buf;
}

void print_adapter(ComPtr<IDXGIAdapter> adapter, const char *tag)
{
    ComPtr<IDXGIAdapter1> a1;
    if (SUCCEEDED(adapter.As(&a1)))
    {
        DXGI_ADAPTER_DESC1 desc {};
        a1->GetDesc1(&desc);
        std::printf("adapter %s: LUID=0x%04X%08X %ls\n", tag,
                    static_cast<unsigned>(desc.AdapterLuid.HighPart),
                    static_cast<unsigned>(desc.AdapterLuid.LowPart), desc.Description);
    }
    else
    {
        DXGI_ADAPTER_DESC desc {};
        adapter->GetDesc(&desc);
        std::printf("adapter %s: LUID=0x%04X%08X %ls\n", tag,
                    static_cast<unsigned>(desc.AdapterLuid.HighPart),
                    static_cast<unsigned>(desc.AdapterLuid.LowPart), desc.Description);
    }
    std::fflush(stdout);
}
} // namespace

int main()
{
    constexpr UINT w = 64, h = 64;
    constexpr std::uint32_t pattern_a = 0xFF332211u; // D3D11 写入 → D3D12 读取
    constexpr std::uint32_t pattern_b = 0xFF77AA55u; // D3D12 写入 → D3D11 读取

    ComPtr<ID3D11Device> d11;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl {};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &d11, &fl, &ctx);
    if (FAILED(hr))
    {
        std::printf("FAIL D3D11CreateDevice %s\n", hres(hr));
        return 1;
    }
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D11Device1> d11_1;
    ComPtr<ID3D11Device5> d11_5;
    ComPtr<ID3D11DeviceContext4> ctx4;
    if (FAILED(d11.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&adapter)) ||
        FAILED(d11.As(&d11_1)) || FAILED(d11.As(&d11_5)) || FAILED(ctx.As(&ctx4)))
    {
        line("FAIL D3D11.4 shared-fence interfaces unavailable");
        return 2;
    }
    print_adapter(adapter, "d3d11");

    ComPtr<ID3D12Device> d12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    D3D12_COMMAND_QUEUE_DESC queue_desc {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d12))) ||
        FAILED(d12->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))) ||
        FAILED(d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                      IID_PPV_ARGS(&list))))
    {
        line("FAIL D3D12CreateDevice/queue/list");
        return 3;
    }

    // ---- 共享 fence（D3D12 建 → D3D11 OpenSharedFence）----
    ComPtr<ID3D12Fence> fence12;
    hr = d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence12));
    HANDLE fence_handle = nullptr;
    if (SUCCEEDED(hr))
        hr = d12->CreateSharedHandle(fence12.Get(), nullptr, GENERIC_ALL, nullptr, &fence_handle);
    ComPtr<ID3D11Fence> fence11;
    if (SUCCEEDED(hr))
        hr = d11_5->OpenSharedFence(fence_handle, IID_PPV_ARGS(&fence11));
    if (fence_handle)
        CloseHandle(fence_handle);
    if (FAILED(hr) || !fence11)
    {
        std::printf("FAIL shared fence %s\n", hres(hr));
        return 4;
    }
    line("PASS shared fence (D3D12 create -> D3D11 open)");

    // ---- D3D12 readback helper ----
    auto make_readback = [&](std::uint64_t size, ComPtr<ID3D12Resource> &rb) -> bool
    {
        D3D12_RESOURCE_DESC buffer_desc {};
        buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer_desc.Width = size;
        buffer_desc.Height = 1;
        buffer_desc.DepthOrArraySize = 1;
        buffer_desc.MipLevels = 1;
        buffer_desc.SampleDesc.Count = 1;
        buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES readback_heap {};
        readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
        return SUCCEEDED(d12->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)));
    };

    // =====================================================================
    // Phase A：D3D11 侧自建 SHARED_NTHANDLE 纹理 → D3D12 打开，
    //         D3D11 GPU 写入 → D3D12 GPU 读回验证（生产主方向）。
    // =====================================================================
    {
        line("--- Phase A: D3D11-owned shared texture -> D3D12 read (production direction) ---");
        // 生产关键格式逐个验证（颜色/运动 R10G10B10A2_TYPELESS、深度 R32G8X24_TYPELESS、
        // R16G16_FLOAT、R8G8B8A8_UNORM 对照）。深度格式共享受限是仅剩的未知数。
        struct SharedFormatTest
        {
            DXGI_FORMAT format;
            std::uint32_t pattern;
            const char *name;
        };
        const std::array<SharedFormatTest, 5> format_tests {{
            { DXGI_FORMAT_R8G8B8A8_UNORM, pattern_a, "R8G8B8A8_UNORM" },
            { DXGI_FORMAT_R10G10B10A2_TYPELESS, 0x3FF003FFu, "R10G10B10A2_TYPELESS" },
            { DXGI_FORMAT_R16G16_FLOAT, 0x3C003C00u, "R16G16_FLOAT" },
            { DXGI_FORMAT_R32_FLOAT, 0x3F800000u, "R32_FLOAT (depth extract target)" },
            { DXGI_FORMAT_R32G8X24_TYPELESS, 0x0000FFFFu, "R32G8X24_TYPELESS (depth src, expect fail)" },
        }};
        std::uint64_t fence_seq = 10; // fmt 循环内单调递增，避免重复 Signal 同值
        for (const SharedFormatTest &fmt : format_tests)
        {
            D3D11_TEXTURE2D_DESC shared_desc {};
            shared_desc.Width = w;
            shared_desc.Height = h;
            shared_desc.MipLevels = 1;
            shared_desc.ArraySize = 1;
            shared_desc.Format = fmt.format;
            shared_desc.SampleDesc.Count = 1;
            shared_desc.Usage = D3D11_USAGE_DEFAULT;
            shared_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            shared_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            ComPtr<ID3D11Texture2D> shared11;
            hr = d11->CreateTexture2D(&shared_desc, nullptr, &shared11);
            if (FAILED(hr))
            {
                std::printf("FAIL[%s] D3D11 shared texture %s\n", fmt.name, hres(hr));
                return 10;
            }
            ComPtr<IDXGIResource1> resource1;
            HANDLE texture_handle = nullptr;
            hr = shared11.As(&resource1);
            if (SUCCEEDED(hr))
                hr = resource1->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &texture_handle);
            if (FAILED(hr))
            {
                std::printf("FAIL[%s] CreateSharedHandle %s\n", fmt.name, hres(hr));
                return 11;
            }
            ComPtr<ID3D12Resource> shared12;
            hr = d12->OpenSharedHandle(texture_handle, IID_PPV_ARGS(&shared12));
            CloseHandle(texture_handle);
            if (FAILED(hr) || !shared12)
            {
                std::printf("FAIL[%s] D3D12 OpenSharedHandle %s\n", fmt.name, hres(hr));
                return 12;
            }
            std::printf("PASS[%s] D3D11 SHARED_NTHANDLE -> D3D12 OpenSharedHandle\n", fmt.name);
            std::fflush(stdout);

            // D3D11 GPU 写入：pattern 纹理 → 共享纹理
            D3D11_TEXTURE2D_DESC source_desc = shared_desc;
            source_desc.MiscFlags = 0;
            std::array<std::uint32_t, w * h> source_pixels {};
            source_pixels.fill(fmt.pattern);
            D3D11_SUBRESOURCE_DATA source_data { source_pixels.data(), w * sizeof(std::uint32_t), 0 };
            ComPtr<ID3D11Texture2D> source11;
            if (FAILED(d11->CreateTexture2D(&source_desc, &source_data, &source11)))
            {
                line("FAIL D3D11 source texture");
                return 13;
            }
        ctx->CopyResource(shared11.Get(), source11.Get());
        const std::uint64_t v1 = ++fence_seq;
        hr = ctx4->Signal(fence11.Get(), v1);
        if (FAILED(hr))
        {
            std::printf("FAIL D3D11 ctx4->Signal %s\n", hres(hr));
            return 14;
        }
        hr = queue->Wait(fence12.Get(), v1);
        if (FAILED(hr))
        {
            std::printf("FAIL D3D12 queue->Wait %s\n", hres(hr));
            return 15;
        }
        // D3D12 GPU 读回
        D3D12_RESOURCE_DESC tex_desc = shared12->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
        UINT64 total = 0;
        d12->GetCopyableFootprints(&tex_desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
        ComPtr<ID3D12Resource> readback;
        if (!make_readback(total, readback))
        {
            line("FAIL D3D12 readback alloc");
            return 16;
        }
        list->Close();
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = shared12.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION dst { .pResource = readback.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        dst.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION src { .pResource = shared12.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        list->ResourceBarrier(1, &barrier);
        list->Close();
        ID3D12CommandList *lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        const std::uint64_t v2 = ++fence_seq;
        queue->Signal(fence12.Get(), v2);
        HANDLE done = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        fence12->SetEventOnCompletion(v2, done);
        if (WaitForSingleObject(done, 5000) != WAIT_OBJECT_0)
        {
            line("FAIL D3D12 readback wait timeout");
            CloseHandle(done);
            return 17;
        }
        CloseHandle(done);
        D3D12_RANGE range { 0, static_cast<SIZE_T>(total) };
        void *mapped = nullptr;
        hr = readback->Map(0, &range, &mapped);
        std::uint32_t value = 0;
        if (SUCCEEDED(hr) && mapped)
        {
            std::memcpy(&value, mapped, 4);
            readback->Unmap(0, nullptr);
        }
        const bool ok = SUCCEEDED(hr) && value == fmt.pattern;
        std::printf("%s Phase A [%s] D3D11->D3D12 GPU flow; read 0x%08X expected 0x%08X\n",
                    ok ? "PASS" : "FAIL", fmt.name, value, fmt.pattern);
        std::fflush(stdout);
        if (!ok)
            return 20;
        // 资源归还 D3D11 侧后也必须可被 D3D12 再次打开使用（模拟下一帧）：本轮已按
        // COMMON 归还；下一轮循环会用新的纹理实例，天然覆盖该路径。
        }
    }

    // =====================================================================
    // Phase B：D3D12 对共享输出纹理 GPU 写入 → D3D11 GPU 拷贝 → Map 验证。
    // =====================================================================
    {
        line("--- Phase B: D3D12 GPU write -> D3D11 read ---");
        // D3D11 侧自建带 UAV 的共享输出纹理
        D3D11_TEXTURE2D_DESC shared_desc {};
        shared_desc.Width = w;
        shared_desc.Height = h;
        shared_desc.MipLevels = 1;
        shared_desc.ArraySize = 1;
        shared_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        shared_desc.SampleDesc.Count = 1;
        shared_desc.Usage = D3D11_USAGE_DEFAULT;
        shared_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        shared_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        ComPtr<ID3D11Texture2D> shared11;
        hr = d11->CreateTexture2D(&shared_desc, nullptr, &shared11);
        if (FAILED(hr))
        {
            std::printf("FAIL D3D11 shared UAV texture %s\n", hres(hr));
            return 21;
        }
        ComPtr<IDXGIResource1> resource1;
        HANDLE texture_handle = nullptr;
        hr = shared11.As(&resource1);
        if (SUCCEEDED(hr))
            hr = resource1->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &texture_handle);
        if (FAILED(hr))
        {
            std::printf("FAIL CreateSharedHandle(D3D11 UAV tex) %s\n", hres(hr));
            return 22;
        }
        ComPtr<ID3D12Resource> shared12;
        hr = d12->OpenSharedHandle(texture_handle, IID_PPV_ARGS(&shared12));
        CloseHandle(texture_handle);
        if (FAILED(hr) || !shared12)
        {
            std::printf("FAIL D3D12 OpenSharedHandle(UAV tex) %s\n", hres(hr));
            return 23;
        }

        // D3D12 UAV clear 写 pattern_b
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.NumDescriptors = 1;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ComPtr<ID3D12DescriptorHeap> uav_heap;
        if (FAILED(d12->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&uav_heap))))
        {
            line("FAIL D3D12 UAV heap");
            return 24;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc {};
        uav_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        d12->CreateUnorderedAccessView(shared12.Get(), nullptr, &uav_desc,
                                       uav_heap->GetCPUDescriptorHandleForHeapStart());
        list->Close();
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = shared12.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        list->ResourceBarrier(1, &barrier);
        const UINT clear_value[4] = { pattern_b & 0xFF, (pattern_b >> 8) & 0xFF,
                                      (pattern_b >> 16) & 0xFF, (pattern_b >> 24) & 0xFF };
        list->ClearUnorderedAccessViewUint(uav_heap->GetGPUDescriptorHandleForHeapStart(),
                                           uav_heap->GetCPUDescriptorHandleForHeapStart(),
                                           shared12.Get(), clear_value, 0, nullptr);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        list->ResourceBarrier(1, &barrier);
        list->Close();
        ID3D12CommandList *lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence12.Get(), 3)))
        {
            line("FAIL D3D12 queue->Signal(B)");
            return 25;
        }
        if (FAILED(ctx4->Wait(fence11.Get(), 3)))
        {
            std::printf("FAIL D3D11 ctx4->Wait %s\n", hres(hr));
            return 26;
        }
        // D3D11 GPU 拷贝到 staging 并读回
        D3D11_TEXTURE2D_DESC staging_desc = shared_desc;
        staging_desc.Usage = D3D11_USAGE_STAGING;
        staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging_desc.BindFlags = 0;
        staging_desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(d11->CreateTexture2D(&staging_desc, nullptr, &staging)))
        {
            line("FAIL D3D11 staging alloc(B)");
            return 27;
        }
        ctx->CopyResource(staging.Get(), shared11.Get());
        ctx->Flush();
        D3D11_MAPPED_SUBRESOURCE mapped {};
        hr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        std::uint32_t value = 0;
        if (SUCCEEDED(hr) && mapped.pData)
        {
            std::memcpy(&value, mapped.pData, 4);
            ctx->Unmap(staging.Get(), 0);
        }
        const bool ok = SUCCEEDED(hr) && value == pattern_b;
        std::printf("%s Phase B D3D12->D3D11 GPU flow; read 0x%08X expected 0x%08X\n",
                    ok ? "PASS" : "FAIL", value, pattern_b);
        if (!ok)
            return 30;
    }

    // =====================================================================
    // Phase C（诊断对照）：D3D12 建共享纹理 → D3D11 OpenSharedResource1。
    // =====================================================================
    {
        line("--- Phase C (diagnostic): D3D12-owned shared texture -> D3D11 import ---");
        D3D12_RESOURCE_DESC texture_desc {};
        texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture_desc.Width = w;
        texture_desc.Height = h;
        texture_desc.DepthOrArraySize = 1;
        texture_desc.MipLevels = 1;
        texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texture_desc.SampleDesc.Count = 1;
        D3D12_HEAP_PROPERTIES default_heap {};
        default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> shared12;
        hr = d12->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_SHARED, &texture_desc,
                                          D3D12_RESOURCE_STATE_COMMON, nullptr,
                                          IID_PPV_ARGS(&shared12));
        if (FAILED(hr))
        {
            std::printf("FAIL D3D12 shared texture %s\n", hres(hr));
            return 40;
        }
        HANDLE texture_handle = nullptr;
        hr = d12->CreateSharedHandle(shared12.Get(), nullptr, GENERIC_ALL, nullptr, &texture_handle);
        ComPtr<ID3D11Texture2D> shared11;
        if (SUCCEEDED(hr))
            hr = d11_1->OpenSharedResource1(texture_handle, IID_PPV_ARGS(&shared11));
        std::printf("%s Phase C D3D12 shared texture -> D3D11 import (%s)\n",
                    (SUCCEEDED(hr) && shared11) ? "PASS" : "FAIL  ", hres(hr));
        if (texture_handle)
            CloseHandle(texture_handle);
    }

    line("ALL PASS — native-D3D11 GPU-only interop is feasible");
    return 0;
}