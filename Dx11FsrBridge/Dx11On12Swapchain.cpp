#include "Dx11On12Swapchain.h"

#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace dx11on12_swapchain
{
namespace
{
std::string hresult(HRESULT hr)
{
    std::ostringstream out;
    out << "0x" << std::hex << static_cast<unsigned long>(hr);
    return out.str();
}

struct Runtime
{
    ComPtr<ID3D12Device> d12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D11Device> d11;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11On12Device> on12;
    ComPtr<ID3D11On12Device2> on12_2;
};

std::mutex g_runtime_mutex;
std::unordered_map<IUnknown *, std::shared_ptr<Runtime>> g_runtimes;
thread_local std::uint32_t g_internal_factory_create_depth = 0;

class InternalFactoryCreateScope
{
public:
    InternalFactoryCreateScope() { ++g_internal_factory_create_depth; }
    ~InternalFactoryCreateScope() { --g_internal_factory_create_depth; }
};

IUnknown *identity(IUnknown *object)
{
    if (object == nullptr)
        return nullptr;
    ComPtr<IUnknown> unknown;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&unknown))))
        return nullptr;
    return unknown.Get();
}

bool make_runtime(
    IDXGIAdapter *adapter,
    D3D_DRIVER_TYPE driver_type,
    UINT d3d11_flags,
    const D3D_FEATURE_LEVEL *feature_levels,
    UINT feature_levels_count,
    std::shared_ptr<Runtime> &out,
    D3D_FEATURE_LEVEL *chosen_feature_level,
    std::string &reason)
{
    // D3D11On12 is meaningful only for hardware adapter-backed creation.  Do
    // not replace WARP/reference/software paths: their semantics are owned by
    // the caller and a D3D12 queue would be a different renderer.
    if (driver_type != D3D_DRIVER_TYPE_HARDWARE && driver_type != D3D_DRIVER_TYPE_UNKNOWN)
    {
        reason = "unsupported_driver_type";
        return false;
    }

    auto runtime = std::make_shared<Runtime>();
    HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&runtime->d12));
    if (FAILED(hr))
    {
        reason = "D3D12CreateDevice=" + hresult(hr);
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    hr = runtime->d12->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&runtime->queue));
    if (FAILED(hr))
    {
        reason = "CreateCommandQueue=" + hresult(hr);
        return false;
    }

    IUnknown *queues[] = { runtime->queue.Get() };
    // A game may ask for the D3D11 debug layer without the D3D12 debug layer
    // being available.  Preserve functional flags but avoid turning that
    // diagnostic preference into a bootstrap failure.
    const UINT on12_flags = d3d11_flags & ~D3D11_CREATE_DEVICE_DEBUG;
    hr = D3D11On12CreateDevice(
        runtime->d12.Get(), on12_flags, feature_levels, feature_levels_count,
        queues, 1, 0, &runtime->d11, &runtime->context, chosen_feature_level);
    if (FAILED(hr))
    {
        reason = "D3D11On12CreateDevice=" + hresult(hr);
        return false;
    }
    hr = runtime->d11.As(&runtime->on12);
    if (FAILED(hr))
    {
        reason = "Query_ID3D11On12Device=" + hresult(hr);
        return false;
    }
    hr = runtime->d11.As(&runtime->on12_2);
    if (FAILED(hr))
    {
        reason = "Query_ID3D11On12Device2=" + hresult(hr);
        return false;
    }
    out = std::move(runtime);
    return true;
}

void register_runtime(const std::shared_ptr<Runtime> &runtime)
{
    IUnknown *const key = identity(runtime->d11.Get());
    if (key == nullptr)
        return;
    std::lock_guard lock(g_runtime_mutex);
    g_runtimes[key] = runtime;
}

std::shared_ptr<Runtime> find_runtime(IUnknown *game_device)
{
    IUnknown *const key = identity(game_device);
    if (key == nullptr)
        return {};
    std::lock_guard lock(g_runtime_mutex);
    const auto it = g_runtimes.find(key);
    return it == g_runtimes.end() ? std::shared_ptr<Runtime> {} : it->second;
}

class SwapchainProxy;
void register_wrapper(ID3D11Resource *resource, SwapchainProxy *owner, UINT index);
void unregister_wrapper(ID3D11Resource *resource, SwapchainProxy *owner);

class SwapchainProxy final : public IDXGISwapChain4
{
public:
    SwapchainProxy(std::shared_ptr<Runtime> runtime, IDXGISwapChain1 *inner)
        : runtime_(std::move(runtime)), inner_(inner)
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        if (out == nullptr)
            return E_POINTER;
        *out = nullptr;
        if (riid == IID_IUnknown || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
            riid == __uuidof(IDXGISwapChain) || riid == __uuidof(IDXGISwapChain1) ||
            riid == __uuidof(IDXGISwapChain2) || riid == __uuidof(IDXGISwapChain3) || riid == __uuidof(IDXGISwapChain4))
        {
            // Only advertise newer interfaces that the real DXGI object has.
            if (riid == __uuidof(IDXGISwapChain2) && !supports<IDXGISwapChain2>()) return E_NOINTERFACE;
            if (riid == __uuidof(IDXGISwapChain3) && !supports<IDXGISwapChain3>()) return E_NOINTERFACE;
            if (riid == __uuidof(IDXGISwapChain4) && !supports<IDXGISwapChain4>()) return E_NOINTERFACE;
            *out = static_cast<IDXGISwapChain4 *>(this);
            AddRef();
            return S_OK;
        }
        return inner_->QueryInterface(riid, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG refs = --refs_;
        if (refs == 0)
            delete this;
        return refs;
    }

    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID id, UINT size, const void *data) override { return inner_->SetPrivateData(id, size, data); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID id, const IUnknown *data) override { return inner_->SetPrivateDataInterface(id, data); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID id, UINT *size, void *data) override { return inner_->GetPrivateData(id, size, data); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **parent) override { return inner_->GetParent(riid, parent); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override
    {
        if (device == nullptr) return E_POINTER;
        *device = nullptr;
        if (SUCCEEDED(runtime_->d11->QueryInterface(riid, device))) return S_OK;
        return runtime_->d12->QueryInterface(riid, device);
    }
    HRESULT STDMETHODCALLTYPE Present(UINT sync, UINT flags) override
    {
        const HRESULT copy_hr = copy_landing_to_backbuffer();
        if (FAILED(copy_hr)) return copy_hr;
        return inner_->Present(sync, flags);
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT index, REFIID riid, void **surface) override
    {
        if (surface == nullptr) return E_POINTER;
        *surface = nullptr;
        if (riid != __uuidof(ID3D11Texture2D) && riid != __uuidof(ID3D11Resource))
            return inner_->GetBuffer(index, riid, surface);
        DXGI_SWAP_CHAIN_DESC desc {};
        if (FAILED(inner_->GetDesc(&desc)) || index >= desc.BufferCount) return E_INVALIDARG;
        if (!ensure_landing()) return E_FAIL;
        acquire(index);
        return landings_[index].d11->QueryInterface(riid, surface);
    }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fullscreen, IDXGIOutput *output) override { return inner_->SetFullscreenState(fullscreen, output); }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL *fullscreen, IDXGIOutput **output) override { return inner_->GetFullscreenState(fullscreen, output); }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC *desc) override { return inner_->GetDesc(desc); }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) override
    {
        discard_landing();
        return inner_->ResizeBuffers(std::max(2u, count), width, height, format, flags);
    }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC *target) override { return inner_->ResizeTarget(target); }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput **output) override { return inner_->GetContainingOutput(output); }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS *stats) override { return inner_->GetFrameStatistics(stats); }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *count) override { return inner_->GetLastPresentCount(count); }

    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1 *desc) override { return inner_->GetDesc1(desc); }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC *desc) override { return inner_->GetFullscreenDesc(desc); }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND *hwnd) override { return inner_->GetHwnd(hwnd); }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID riid, void **window) override { return inner_->GetCoreWindow(riid, window); }
    HRESULT STDMETHODCALLTYPE Present1(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS *params) override
    {
        const HRESULT copy_hr = copy_landing_to_backbuffer();
        if (FAILED(copy_hr)) return copy_hr;
        return inner_->Present1(sync, flags, params);
    }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return inner_->IsTemporaryMonoSupported(); }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput **output) override { return inner_->GetRestrictToOutput(output); }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA *color) override { return inner_->SetBackgroundColor(color); }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA *color) override { return inner_->GetBackgroundColor(color); }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION rotation) override { return inner_->SetRotation(rotation); }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION *rotation) override { return inner_->GetRotation(rotation); }

    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT width, UINT height) override { return with2([&](IDXGISwapChain2 *x) { return x->SetSourceSize(width, height); }); }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT *width, UINT *height) override { return with2([&](IDXGISwapChain2 *x) { return x->GetSourceSize(width, height); }); }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT latency) override { return with2([&](IDXGISwapChain2 *x) { return x->SetMaximumFrameLatency(latency); }); }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *latency) override { return with2([&](IDXGISwapChain2 *x) { return x->GetMaximumFrameLatency(latency); }); }
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override { ComPtr<IDXGISwapChain2> x; return SUCCEEDED(inner_.As(&x)) ? x->GetFrameLatencyWaitableObject() : nullptr; }
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F *matrix) override { return with2([&](IDXGISwapChain2 *x) { return x->SetMatrixTransform(matrix); }); }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F *matrix) override { return with2([&](IDXGISwapChain2 *x) { return x->GetMatrixTransform(matrix); }); }

    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { ComPtr<IDXGISwapChain3> x; return SUCCEEDED(inner_.As(&x)) ? x->GetCurrentBackBufferIndex() : 0; }
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE color_space, UINT *support) override { return with3([&](IDXGISwapChain3 *x) { return x->CheckColorSpaceSupport(color_space, support); }); }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE color_space) override { return with3([&](IDXGISwapChain3 *x) { return x->SetColorSpace1(color_space); }); }
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags, const UINT *node_masks, IUnknown *const *queues) override
    {
        discard_landing();
        return with3([&](IDXGISwapChain3 *x) { return x->ResizeBuffers1(std::max(2u, count), width, height, format, flags, node_masks, queues); });
    }
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE type, UINT size, void *metadata) override { return with4([&](IDXGISwapChain4 *x) { return x->SetHDRMetaData(type, size, metadata); }); }

private:
    friend void dx11on12_swapchain::acquire_render_target_views(ID3D11RenderTargetView *const *, UINT);
    ~SwapchainProxy() { discard_landing(); }

    template <typename T> bool supports() const { ComPtr<T> x; return SUCCEEDED(inner_.As(&x)); }
    template <typename F> HRESULT with2(F &&fn) { ComPtr<IDXGISwapChain2> x; return FAILED(inner_.As(&x)) ? E_NOINTERFACE : fn(x.Get()); }
    template <typename F> HRESULT with3(F &&fn) { ComPtr<IDXGISwapChain3> x; return FAILED(inner_.As(&x)) ? E_NOINTERFACE : fn(x.Get()); }
    template <typename F> HRESULT with4(F &&fn) { ComPtr<IDXGISwapChain4> x; return FAILED(inner_.As(&x)) ? E_NOINTERFACE : fn(x.Get()); }

    struct CopySlot
    {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        UINT64 fence_value = 0;
    };

    // D3D11 callers are entitled to receive a distinct resource for every
    // valid GetBuffer(index).  They may cache several RTVs at once, so one
    // shared landing texture violates the DXGI resource-identity contract and
    // makes unrelated render targets alias each other.
    struct LandingSlot
    {
        ComPtr<ID3D12Resource> d12;
        ComPtr<ID3D11Resource> d11;
        bool acquired = false;
    };

    bool ensure_landing()
    {
        if (!landings_.empty()) return true;
        DXGI_SWAP_CHAIN_DESC desc {};
        if (FAILED(inner_->GetDesc(&desc)) || desc.BufferDesc.Width == 0 || desc.BufferDesc.Height == 0) return false;
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC texture {};
        texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture.Width = desc.BufferDesc.Width;
        texture.Height = desc.BufferDesc.Height;
        texture.DepthOrArraySize = 1;
        texture.MipLevels = 1;
        texture.Format = desc.BufferDesc.Format;
        texture.SampleDesc.Count = 1;
        texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        landings_.resize(desc.BufferCount);
        D3D11_RESOURCE_FLAGS flags {};
        flags.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        for (UINT index = 0; index < desc.BufferCount; ++index)
        {
            LandingSlot &landing = landings_[index];
            if (FAILED(runtime_->d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture,
                    D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&landing.d12))) ||
                FAILED(runtime_->on12->CreateWrappedResource(landing.d12.Get(), &flags,
                    D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE,
                    IID_PPV_ARGS(&landing.d11))))
            {
                landings_.clear();
                return false;
            }
            register_wrapper(landing.d11.Get(), this, index);
        }
        if (FAILED(runtime_->d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copy_fence_)))) return false;
        copy_fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (copy_fence_event_ == nullptr) return false;
        copy_slots_.resize(std::max(2u, desc.BufferCount));
        for (auto &slot : copy_slots_)
        {
            if (FAILED(runtime_->d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator))) ||
                FAILED(runtime_->d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr, IID_PPV_ARGS(&slot.list))) ||
                FAILED(slot.list->Close())) return false;
        }
        return true;
    }
    void acquire(UINT index)
    {
        if (index >= landings_.size())
            return;
        LandingSlot &landing = landings_[index];
        if (landing.d11 == nullptr || landing.acquired)
            return;
        ID3D11Resource *resource = landing.d11.Get();
        runtime_->on12->AcquireWrappedResources(&resource, 1);
        landing.acquired = true;
        active_landing_index_ = index;
    }
    void release_landing()
    {
        bool flushed = false;
        for (LandingSlot &landing : landings_)
        {
            if (landing.acquired && landing.d11 != nullptr)
            {
                ID3D11Resource *resource = landing.d11.Get();
                runtime_->on12->ReleaseWrappedResources(&resource, 1);
                landing.acquired = false;
                flushed = true;
            }
        }
        if (flushed)
            runtime_->context->Flush();
    }
    HRESULT copy_landing_to_backbuffer()
    {
        if (!ensure_landing()) return E_FAIL;
        release_landing();
        UINT backbuffer_index = 0;
        ComPtr<IDXGISwapChain3> swapchain3;
        if (SUCCEEDED(inner_.As(&swapchain3))) backbuffer_index = swapchain3->GetCurrentBackBufferIndex();
        if (copy_slots_.empty()) return E_FAIL;
        CopySlot &slot = copy_slots_[backbuffer_index % copy_slots_.size()];
        if (slot.fence_value != 0 && copy_fence_->GetCompletedValue() < slot.fence_value)
        {
            if (FAILED(copy_fence_->SetEventOnCompletion(slot.fence_value, copy_fence_event_))) return E_FAIL;
            WaitForSingleObject(copy_fence_event_, INFINITE);
        }
        ComPtr<ID3D12Resource> backbuffer;
        HRESULT hr = inner_->GetBuffer(backbuffer_index, IID_PPV_ARGS(&backbuffer));
        if (FAILED(hr)) return hr;
        if (FAILED(slot.allocator->Reset()) || FAILED(slot.list->Reset(slot.allocator.Get(), nullptr))) return E_FAIL;
        D3D12_RESOURCE_BARRIER to_copy {};
        to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy.Transition.pResource = backbuffer.Get();
        to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        slot.list->ResourceBarrier(1, &to_copy);
        if (active_landing_index_ >= landings_.size() || landings_[active_landing_index_].d12 == nullptr)
            return E_FAIL;
        slot.list->CopyResource(backbuffer.Get(), landings_[active_landing_index_].d12.Get());
        std::swap(to_copy.Transition.StateBefore, to_copy.Transition.StateAfter);
        slot.list->ResourceBarrier(1, &to_copy);
        if (FAILED(slot.list->Close())) return E_FAIL;
        ID3D12CommandList *lists[] = { slot.list.Get() };
        runtime_->queue->ExecuteCommandLists(1, lists);
        slot.fence_value = ++copy_fence_value_;
        return runtime_->queue->Signal(copy_fence_.Get(), slot.fence_value);
    }
    void wait_copy_slots()
    {
        for (const auto &slot : copy_slots_)
        {
            if (slot.fence_value != 0 && copy_fence_ != nullptr && copy_fence_->GetCompletedValue() < slot.fence_value)
            {
                if (SUCCEEDED(copy_fence_->SetEventOnCompletion(slot.fence_value, copy_fence_event_)))
                    WaitForSingleObject(copy_fence_event_, INFINITE);
            }
        }
    }
    void discard_landing()
    {
        release_landing();
        wait_copy_slots();
        for (LandingSlot &landing : landings_)
            if (landing.d11 != nullptr) unregister_wrapper(landing.d11.Get(), this);
        landings_.clear();
        active_landing_index_ = 0;
        copy_slots_.clear();
        copy_fence_.Reset();
        if (copy_fence_event_ != nullptr) { CloseHandle(copy_fence_event_); copy_fence_event_ = nullptr; }
    }

    std::atomic_ulong refs_ { 1 };
    std::shared_ptr<Runtime> runtime_;
    ComPtr<IDXGISwapChain1> inner_;
    std::vector<LandingSlot> landings_;
    ComPtr<ID3D12Fence> copy_fence_;
    HANDLE copy_fence_event_ = nullptr;
    UINT64 copy_fence_value_ = 0;
    std::vector<CopySlot> copy_slots_;
    UINT active_landing_index_ = 0;
};

struct WrappedBinding
{
    SwapchainProxy *owner = nullptr;
    UINT index = 0;
};
std::mutex g_wrapped_resource_mutex;
std::unordered_map<ID3D11Resource *, WrappedBinding> g_wrapped_resources;

void register_wrapper(ID3D11Resource *resource, SwapchainProxy *owner, UINT index)
{
    std::lock_guard lock(g_wrapped_resource_mutex);
    g_wrapped_resources[resource] = { owner, index };
}

void unregister_wrapper(ID3D11Resource *resource, SwapchainProxy *owner)
{
    std::lock_guard lock(g_wrapped_resource_mutex);
    const auto it = g_wrapped_resources.find(resource);
    if (it != g_wrapped_resources.end() && it->second.owner == owner)
        g_wrapped_resources.erase(it);
}

bool validate_desc(const DXGI_SWAP_CHAIN_DESC1 *desc, HWND hwnd, std::string &reason)
{
    if (desc == nullptr || hwnd == nullptr) { reason = "missing_hwnd_or_desc"; return false; }
    if (desc->SampleDesc.Count != 1) { reason = "multisample_swapchain_unsupported"; return false; }
    if (desc->Format == DXGI_FORMAT_UNKNOWN) { reason = "unknown_swapchain_format"; return false; }
    return true;
}

bool make_proxy(const std::shared_ptr<Runtime> &runtime, IDXGIFactory2 *factory, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1 *input, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen,
    IDXGIOutput *restrict_output, IDXGISwapChain1 **out, std::string &reason)
{
    if (out == nullptr) { reason = "missing_swapchain_output"; return false; }
    *out = nullptr;
    if (!validate_desc(input, hwnd, reason)) return false;
    DXGI_SWAP_CHAIN_DESC1 desc = *input;
    desc.BufferCount = std::max(2u, desc.BufferCount);
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage |= DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    ComPtr<IDXGISwapChain1> inner;
    InternalFactoryCreateScope internal_factory_create;
    const HRESULT hr = factory->CreateSwapChainForHwnd(runtime->queue.Get(), hwnd, &desc, fullscreen, restrict_output, &inner);
    if (FAILED(hr)) { reason = "CreateSwapChainForHwnd=" + hresult(hr); return false; }
    *out = new SwapchainProxy(runtime, inner.Get());
    return true;
}

bool legacy_to_desc1(const DXGI_SWAP_CHAIN_DESC *legacy, DXGI_SWAP_CHAIN_DESC1 &desc1,
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC &fullscreen, HWND &hwnd, std::string &reason)
{
    if (legacy == nullptr) { reason = "missing_legacy_desc"; return false; }
    hwnd = legacy->OutputWindow;
    desc1.Width = legacy->BufferDesc.Width;
    desc1.Height = legacy->BufferDesc.Height;
    desc1.Format = legacy->BufferDesc.Format;
    desc1.SampleDesc = legacy->SampleDesc;
    desc1.BufferUsage = legacy->BufferUsage;
    desc1.BufferCount = legacy->BufferCount;
    desc1.Scaling = DXGI_SCALING_STRETCH;
    desc1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc1.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc1.Flags = legacy->Flags;
    fullscreen.RefreshRate = legacy->BufferDesc.RefreshRate;
    fullscreen.ScanlineOrdering = legacy->BufferDesc.ScanlineOrdering;
    fullscreen.Scaling = legacy->BufferDesc.Scaling;
    fullscreen.Windowed = legacy->Windowed;
    return validate_desc(&desc1, hwnd, reason);
}
} // namespace

bool internal_factory_create()
{
    return g_internal_factory_create_depth != 0;
}

bool get_runtime_interfaces(ID3D11Device *device, ID3D12Device **d12,
    ID3D12CommandQueue **queue, ID3D11On12Device2 **on12)
{
    if (d12 == nullptr || queue == nullptr || on12 == nullptr)
        return false;
    *d12 = nullptr;
    *queue = nullptr;
    *on12 = nullptr;
    const auto runtime = find_runtime(device);
    if (!runtime || runtime->on12_2 == nullptr)
        return false;
    runtime->d12.CopyTo(d12);
    runtime->queue.CopyTo(queue);
    runtime->on12_2.CopyTo(on12);
    return true;
}

void acquire_render_target_views(ID3D11RenderTargetView *const *rtvs, UINT count)
{
    if (rtvs == nullptr)
        return;
    for (UINT i = 0; i < count; ++i)
    {
        if (rtvs[i] == nullptr)
            continue;
        ID3D11Resource *resource = nullptr;
        rtvs[i]->GetResource(&resource);
        if (resource == nullptr)
            continue;
        WrappedBinding binding {};
        {
            std::lock_guard lock(g_wrapped_resource_mutex);
            const auto it = g_wrapped_resources.find(resource);
            if (it != g_wrapped_resources.end())
                binding = it->second;
        }
        resource->Release();
        if (binding.owner != nullptr)
            binding.owner->acquire(binding.index);
    }
}

void acquire_current_render_targets(ID3D11DeviceContext *context)
{
    if (context == nullptr)
        return;
    ID3D11RenderTargetView *rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] {};
    ID3D11DepthStencilView *dsv = nullptr;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    acquire_render_target_views(rtvs, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT);
    for (ID3D11RenderTargetView *rtv : rtvs)
        if (rtv != nullptr) rtv->Release();
    if (dsv != nullptr) dsv->Release();
}

bool create_device(IDXGIAdapter *adapter, D3D_DRIVER_TYPE driver_type, UINT flags,
    const D3D_FEATURE_LEVEL *feature_levels, UINT feature_levels_count, ID3D11Device **device,
    D3D_FEATURE_LEVEL *feature_level, ID3D11DeviceContext **context, std::string &reason)
{
    if (device == nullptr || context == nullptr) { reason = "missing_device_or_context_output"; return false; }
    *device = nullptr; *context = nullptr;
    std::shared_ptr<Runtime> runtime;
    if (!make_runtime(adapter, driver_type, flags, feature_levels, feature_levels_count, runtime, feature_level, reason)) return false;
    runtime->d11.CopyTo(device);
    runtime->context.CopyTo(context);
    register_runtime(runtime);
    reason = "ok";
    return true;
}

bool create_device_and_swapchain(IDXGIAdapter *adapter, D3D_DRIVER_TYPE driver_type, UINT flags,
    const D3D_FEATURE_LEVEL *feature_levels, UINT feature_levels_count, const DXGI_SWAP_CHAIN_DESC *legacy,
    IDXGISwapChain **swapchain, ID3D11Device **device, D3D_FEATURE_LEVEL *feature_level,
    ID3D11DeviceContext **context, std::string &reason)
{
    if (swapchain == nullptr || device == nullptr || context == nullptr) { reason = "missing_creation_output"; return false; }
    *swapchain = nullptr; *device = nullptr; *context = nullptr;
    std::shared_ptr<Runtime> runtime;
    if (!make_runtime(adapter, driver_type, flags, feature_levels, feature_levels_count, runtime, feature_level, reason)) return false;
    DXGI_SWAP_CHAIN_DESC1 desc1 {}; DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen {}; HWND hwnd = nullptr;
    if (!legacy_to_desc1(legacy, desc1, fullscreen, hwnd, reason)) return false;
    ComPtr<IDXGIFactory2> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { reason = "CreateDXGIFactory1=" + hresult(hr); return false; }
    ComPtr<IDXGISwapChain1> proxy;
    if (!make_proxy(runtime, factory.Get(), hwnd, &desc1, &fullscreen, nullptr, &proxy, reason)) return false;
    runtime->d11.CopyTo(device);
    runtime->context.CopyTo(context);
    hr = proxy->QueryInterface(IID_PPV_ARGS(swapchain));
    if (FAILED(hr)) { reason = "Query_IDXGISwapChain=" + hresult(hr); return false; }
    register_runtime(runtime);
    reason = "ok";
    return true;
}

bool create_factory_swapchain(IDXGIFactory *factory, IUnknown *game_device, const DXGI_SWAP_CHAIN_DESC *legacy,
    IDXGISwapChain **swapchain, std::string &reason)
{
    const auto runtime = find_runtime(game_device);
    if (!runtime) { reason = "not_on12_device"; return false; }
    ComPtr<IDXGIFactory2> factory2;
    if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory2)))) { reason = "factory2_unavailable"; return false; }
    DXGI_SWAP_CHAIN_DESC1 desc1 {}; DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen {}; HWND hwnd = nullptr;
    if (!legacy_to_desc1(legacy, desc1, fullscreen, hwnd, reason)) return false;
    ComPtr<IDXGISwapChain1> proxy;
    if (!make_proxy(runtime, factory2.Get(), hwnd, &desc1, &fullscreen, nullptr, &proxy, reason)) return false;
    const HRESULT hr = proxy->QueryInterface(IID_PPV_ARGS(swapchain));
    if (FAILED(hr)) { reason = "Query_IDXGISwapChain=" + hresult(hr); return false; }
    reason = "ok";
    return true;
}

bool create_factory_swapchain_for_hwnd(IDXGIFactory2 *factory, IUnknown *game_device, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1 *desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen_desc,
    IDXGIOutput *restrict_to_output, IDXGISwapChain1 **swapchain, std::string &reason)
{
    const auto runtime = find_runtime(game_device);
    if (!runtime) { reason = "not_on12_device"; return false; }
    if (!make_proxy(runtime, factory, hwnd, desc, fullscreen_desc, restrict_to_output, swapchain, reason)) return false;
    reason = "ok";
    return true;
}
} // namespace dx11on12_swapchain
