#include "Dx11On12Swapchain.h"

#include <cstdio>
#include <string>

int main()
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    constexpr wchar_t k_class_name[] = L"Dx11On12SwapchainTestWindow";
    WNDCLASSW window_class {};
    window_class.hInstance = instance;
    window_class.lpszClassName = k_class_name;
    window_class.lpfnWndProc = DefWindowProcW;
    if (RegisterClassW(&window_class) == 0)
        return 2;
    HWND hwnd = CreateWindowExW(0, k_class_name, L"", WS_POPUP, 0, 0, 64, 64,
                                nullptr, nullptr, instance, nullptr);
    if (hwnd == nullptr)
        return 3;

    DXGI_SWAP_CHAIN_DESC desc {};
    desc.BufferDesc.Width = 64;
    desc.BufferDesc.Height = 64;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.OutputWindow = hwnd;
    desc.Windowed = TRUE;

    IDXGISwapChain *swapchain = nullptr;
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    D3D_FEATURE_LEVEL level {};
    std::string reason;
    const bool created = dx11on12_swapchain::create_device_and_swapchain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, &desc, &swapchain, &device, &level, &context, reason);
    if (!created)
    {
        std::printf("FAIL create %s\n", reason.c_str());
        DestroyWindow(hwnd);
        return 4;
    }

    ID3D11Texture2D *backbuffer = nullptr;
    ID3D11Texture2D *backbuffer1 = nullptr;
    ID3D11RenderTargetView *rtv = nullptr;
    HRESULT hr = swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    if (SUCCEEDED(hr))
        hr = swapchain->GetBuffer(1, IID_PPV_ARGS(&backbuffer1));
    // The proxy must preserve DXGI buffer identity.  Returning a single
    // landing texture for every index makes cached RTVs alias each other.
    if (SUCCEEDED(hr) && backbuffer == backbuffer1)
        hr = E_FAIL;
    if (SUCCEEDED(hr))
        hr = device->CreateRenderTargetView(backbuffer, nullptr, &rtv);
    if (SUCCEEDED(hr))
    {
        for (unsigned frame = 0; frame != 3 && SUCCEEDED(hr); ++frame)
        {
            // Mirrors an engine which holds its backbuffer/RTV across Presents
            // and binds it again on the next frame without another GetBuffer.
            dx11on12_swapchain::acquire_render_target_views(&rtv, 1);
            const float color[4] = { 0.125f * (frame + 1), 0.25f, 0.5f, 1.0f };
            context->OMSetRenderTargets(1, &rtv, nullptr);
            context->ClearRenderTargetView(rtv, color);
            context->OMSetRenderTargets(0, nullptr, nullptr);
            hr = swapchain->Present(0, 0);
        }
    }

    if (rtv != nullptr) rtv->Release();
    if (backbuffer1 != nullptr) backbuffer1->Release();
    if (backbuffer != nullptr) backbuffer->Release();
    if (context != nullptr) context->Release();
    if (device != nullptr) device->Release();
    if (swapchain != nullptr) swapchain->Release();
    DestroyWindow(hwnd);
    if (FAILED(hr))
    {
        std::printf("FAIL render 0x%08lx\n", static_cast<unsigned long>(hr));
        return 5;
    }
    std::printf("PASS on12 flip swapchain feature_level=0x%04x\n", static_cast<unsigned>(level));
    return 0;
}
