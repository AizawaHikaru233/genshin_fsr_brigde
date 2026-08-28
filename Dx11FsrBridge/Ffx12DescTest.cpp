// Ffx12DescTest.cpp — 离线描述符布局验证（ASCII-only）
// 复刻后端 g_pq_in_heap 创建 + PQ 解码 pass 派发，读回 color_own / color_linear，
// 判明解码 UAV 写入目标是否正确（游戏内 co=0.5 灰疑为解码写进了 color_own）。
#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace Microsoft::WRL;

static float half2float(std::uint16_t h)
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

int main()
{
    ComPtr<ID3D12Device> dev;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev))))
    {
        std::printf("FAIL: D3D12CreateDevice\n");
        return 1;
    }
    ComPtr<ID3D12CommandQueue> queue;
    {
        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    }
    ComPtr<ID3D12CommandAllocator> alloc;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    ComPtr<ID3D12GraphicsCommandList> list;
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list));
    list->Close();
    ComPtr<ID3D12Fence> fence;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE ev = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    const UINT W = 64, H = 64;
    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    std::printf("descriptor inc = %u\n", inc);

    ComPtr<ID3D12Resource> color_own;
    {
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = W; rd.Height = H; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R10G10B10A2_TYPELESS;
        rd.SampleDesc.Count = 1;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                IID_PPV_ARGS(&color_own))))
        {
            std::printf("FAIL: create color_own\n");
            return 1;
        }
    }
    ComPtr<ID3D12Resource> color_linear;
    {
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = W; rd.Height = H; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                IID_PPV_ARGS(&color_linear))))
        {
            std::printf("FAIL: create color_linear\n");
            return 1;
        }
    }
    std::printf("color_own=%p color_linear=%p\n", color_own.Get(), color_linear.Get());

    // pq_in_heap: [UAV(color_linear)@0, SRV(color_own)@1] —— 本驱动 UAV range 排在 offset 0
    ComPtr<ID3D12DescriptorHeap> pq_in;
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 2;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&pq_in))))
            return 1;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu0 = pq_in->GetCPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE cpu1 = cpu0;
        cpu1.ptr += inc;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
        uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        dev->CreateUnorderedAccessView(color_linear.Get(), nullptr, &uav, cpu0);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(color_own.Get(), &srv, cpu1);
        std::printf("pq_in cpu0=%llu cpu1=%llu (UAV@0, SRV@1)\n", cpu0.ptr, cpu1.ptr);
    }

    // root sig: 1 param, table [SRV t0, UAV u0]
    ComPtr<ID3D12RootSignature> rs;
    {
        D3D12_DESCRIPTOR_RANGE ranges[2] {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER param {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 2;
        param.DescriptorTable.pDescriptorRanges = ranges;
        D3D12_ROOT_SIGNATURE_DESC rsd {};
        rsd.NumParameters = 1;
        rsd.pParameters = &param;
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
            FAILED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                            IID_PPV_ARGS(&rs))))
        {
            std::printf("FAIL: root signature\n");
            return 1;
        }
    }

    // decode test shader: 常数 0.5 写 u0
    ComPtr<ID3D12PipelineState> pso;
    {
        const char *src = R"(
        RWTexture2D<float4> out_linear : register(u0);
        [numthreads(8,8,1)]
        void main(uint3 id : SV_DispatchThreadID) { out_linear[id.xy] = float4(0.5,0.5,0.5,1.0); }
        )";
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, "main",
                              "cs_5_0", 0, 0, &blob, &err)) ||
            !blob)
        {
            std::printf("FAIL: shader compile\n");
            return 1;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
        pd.pRootSignature = rs.Get();
        pd.CS.pShaderBytecode = blob->GetBufferPointer();
        pd.CS.BytecodeLength = blob->GetBufferSize();
        if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso))))
        {
            std::printf("FAIL: pso\n");
            return 1;
        }
    }

    // readbacks
    ComPtr<ID3D12Resource> rb_cl, rb_co;
    UINT pitch_cl = 0, pitch_co = 0;
    {
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        auto make_rb = [&](UINT size) -> ComPtr<ID3D12Resource> {
            D3D12_RESOURCE_DESC rd {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
            rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> r;
            dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r));
            return r;
        };
        rb_cl = make_rb(W * H * 8);
        rb_co = make_rb(W * H * 4);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        D3D12_RESOURCE_DESC d = color_linear->GetDesc();
        dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, nullptr);
        pitch_cl = fp.Footprint.RowPitch;
        d = color_own->GetDesc();
        dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, nullptr);
        pitch_co = fp.Footprint.RowPitch;
        std::printf("pitch_cl=%u pitch_co=%u\n", pitch_cl, pitch_co);
    }

    // record: barrier color_linear→UAV, dispatch decode, copies to rb
    list->Reset(alloc.Get(), nullptr);
    {
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = color_linear.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        list->ResourceBarrier(1, &b);
        list->SetComputeRootSignature(rs.Get());
        list->SetPipelineState(pso.Get());
        ID3D12DescriptorHeap *heaps[] = {pq_in.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootDescriptorTable(0, pq_in->GetGPUDescriptorHandleForHeapStart());
        list->Dispatch(W / 8, H / 8, 1);
        // color_linear → COPY_SOURCE → rb_cl
        D3D12_RESOURCE_BARRIER b2 {};
        b2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b2.Transition.pResource = color_linear.Get();
        b2.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b2.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b2.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &b2);
        D3D12_TEXTURE_COPY_LOCATION dst_cl {}, src_cl {};
        dst_cl.pResource = rb_cl.Get();
        dst_cl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst_cl.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        dst_cl.PlacedFootprint.Footprint.Width = W;
        dst_cl.PlacedFootprint.Footprint.Height = H;
        dst_cl.PlacedFootprint.Footprint.Depth = 1;
        dst_cl.PlacedFootprint.Footprint.RowPitch = pitch_cl;
        src_cl.pResource = color_linear.Get();
        src_cl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src_cl.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst_cl, 0, 0, 0, &src_cl, nullptr);
        // color_own → COPY_SOURCE → rb_co
        D3D12_RESOURCE_BARRIER b3 {};
        b3.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b3.Transition.pResource = color_own.Get();
        b3.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b3.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b3.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &b3);
        D3D12_TEXTURE_COPY_LOCATION dst_co {}, src_co {};
        dst_co.pResource = rb_co.Get();
        dst_co.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst_co.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R10G10B10A2_TYPELESS;
        dst_co.PlacedFootprint.Footprint.Width = W;
        dst_co.PlacedFootprint.Footprint.Height = H;
        dst_co.PlacedFootprint.Footprint.Depth = 1;
        dst_co.PlacedFootprint.Footprint.RowPitch = pitch_co;
        src_co.pResource = color_own.Get();
        src_co.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src_co.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst_co, 0, 0, 0, &src_co, nullptr);
    }
    list->Close();
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    UINT64 fv = 1;
    queue->Signal(fence.Get(), fv);
    fence->SetEventOnCompletion(fv, ev);
    WaitForSingleObject(ev, INFINITE);

    {
        D3D12_RANGE r {0, static_cast<SIZE_T>(pitch_cl) * H};
        void *p = nullptr;
        rb_cl->Map(0, &r, &p);
        std::uint16_t hf[4] {};
        std::memcpy(hf, static_cast<std::uint8_t *>(p) + (H / 2) * pitch_cl + (W / 2) * 8, 8);
        std::printf("RESULT color_linear center = (%.3f, %.3f, %.3f, %.3f) [expect 0.5,0.5,0.5]\n",
                    half2float(hf[0]), half2float(hf[1]), half2float(hf[2]), half2float(hf[3]));
        rb_cl->Unmap(0, nullptr);
    }
    {
        D3D12_RANGE r {0, static_cast<SIZE_T>(pitch_co) * H};
        void *p = nullptr;
        rb_co->Map(0, &r, &p);
        std::uint32_t raw = 0;
        std::memcpy(&raw, static_cast<std::uint8_t *>(p) + (H / 2) * pitch_co + (W / 2) * 4, 4);
        std::printf("RESULT color_own center = 0x%08X [expect 0x00000000 untouched]\n", raw);
        rb_co->Unmap(0, nullptr);
    }
    return 0;
}
