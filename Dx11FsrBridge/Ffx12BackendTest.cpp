// Ffx12BackendTest.cpp — 后端模块测试：模拟游戏设备/纹理，走 ffx12::dispatch
// 全链路（懒初始化 → D3D11 拷贝 → 事件同步 → D3D12 ffxDispatch(2.3.4) → 输出拷贝 → 读回验证）。
// ASCII-only source.
#include "Ffx12Backend.h"

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace Microsoft::WRL;

namespace
{
constexpr UINT k_render_w = 960, k_render_h = 540;
constexpr UINT k_display_w = 1920, k_display_h = 1080;
const wchar_t *k_sdk_dll = L"D:\\miHoYo Games\\Starward\\原神解帧FSR插件包\\payload\\OptiScaler\\amd_fidelityfx_upscaler_dx12.dll";

int g_failures = 0;
#define CHECK(cond, msg)                                                 \
    do                                                                   \
    {                                                                    \
        if (!(cond))                                                     \
        {                                                                \
            std::printf("FAIL: %s\n", msg);                              \
            ++g_failures;                                                \
        }                                                                \
        else                                                             \
        {                                                                \
            std::printf("ok:   %s\n", msg);                              \
        }                                                                \
        std::fflush(stdout);                                             \
    } while (0)

ComPtr<ID3D11Device> g_dev;
ComPtr<ID3D11DeviceContext> g_ctx;
ComPtr<ID3D11Texture2D> g_color, g_depth, g_motion, g_output;

ComPtr<ID3D11Texture2D> make_tex(UINT w, UINT h, DXGI_FORMAT fmt, UINT bind)
{
    D3D11_TEXTURE2D_DESC d {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    ComPtr<ID3D11Texture2D> t;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &t)))
        return nullptr;
    return t;
}

void fill_gradient(ComPtr<ID3D11Texture2D> &tex, DXGI_FORMAT fmt)
{
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    ComPtr<ID3D11Texture2D> staging;
    D3D11_TEXTURE2D_DESC s = d;
    s.Usage = D3D11_USAGE_STAGING;
    s.BindFlags = 0;
    s.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateTexture2D(&s, nullptr, &staging);
    D3D11_MAPPED_SUBRESOURCE map {};
    if (SUCCEEDED(g_ctx->Map(staging.Get(), 0, D3D11_MAP_WRITE, 0, &map)))
    {
        const UINT w = d.Width, h = d.Height;
        const UINT bpp = (fmt == DXGI_FORMAT_R16G16_FLOAT) ? 4 : (fmt == DXGI_FORMAT_R32_FLOAT) ? 4 : 4;
        for (UINT y = 0; y < h; ++y)
        {
            auto *row = static_cast<std::uint8_t *>(map.pData) + static_cast<std::size_t>(y) * map.RowPitch;
            for (UINT x = 0; x < w; ++x)
            {
                if (fmt == DXGI_FORMAT_R8G8B8A8_UNORM)
                {
                    row[x * 4 + 0] = static_cast<std::uint8_t>(x * 255 / w);
                    row[x * 4 + 1] = static_cast<std::uint8_t>(y * 255 / h);
                    row[x * 4 + 2] = 128;
                    row[x * 4 + 3] = 255;
                }
                else
                {
                    const std::uint32_t v = fmt == DXGI_FORMAT_R32_FLOAT ? 0x3F000000 : 0;
                    std::memcpy(row + x * bpp, &v, bpp);
                }
            }
        }
        g_ctx->Unmap(staging.Get(), 0);
    }
    g_ctx->CopyResource(tex.Get(), staging.Get());
}

void verify_output()
{
    D3D11_TEXTURE2D_DESC d;
    g_output->GetDesc(&d);
    ComPtr<ID3D11Texture2D> staging;
    D3D11_TEXTURE2D_DESC s = d;
    s.Usage = D3D11_USAGE_STAGING;
    s.BindFlags = 0;
    s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    g_dev->CreateTexture2D(&s, nullptr, &staging);
    g_ctx->CopyResource(staging.Get(), g_output.Get());
    g_ctx->Flush();
    D3D11_MAPPED_SUBRESOURCE map {};
    std::uint64_t sum = 0;
    std::uint32_t nonzero = 0;
    if (SUCCEEDED(g_ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)))
    {
        for (UINT y = 0; y < d.Height; y += 4)
        {
            const auto *row = static_cast<const std::uint8_t *>(map.pData) + static_cast<std::size_t>(y) * map.RowPitch;
            for (UINT x = 0; x < d.Width; x += 4)
            {
                const std::uint32_t v = row[x * 4];
                sum += v;
                if (v != 0)
                    ++nonzero;
            }
        }
        g_ctx->Unmap(staging.Get(), 0);
    }
    const std::uint64_t samples = static_cast<std::uint64_t>(d.Width / 4) * (d.Height / 4);
    std::printf("  output %ux%u sum=%llu nonzero=%u/%llu\n", d.Width, d.Height,
                static_cast<unsigned long long>(sum), nonzero,
                static_cast<unsigned long long>(samples));
    CHECK(nonzero > samples / 4, "backend output contains content");
}
} // namespace

int main()
{
    std::printf("Ffx12BackendTest\n");
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                   D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx);
    CHECK(SUCCEEDED(hr), "game D3D11 device");

    g_color = make_tex(k_render_w, k_render_h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    g_depth = make_tex(k_render_w, k_render_h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE);
    g_motion = make_tex(k_render_w, k_render_h, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
    g_output = make_tex(k_display_w, k_display_h, DXGI_FORMAT_R8G8B8A8_UNORM,
                        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
    CHECK(g_color && g_depth && g_motion && g_output, "game textures");
    fill_gradient(g_color, DXGI_FORMAT_R8G8B8A8_UNORM);
    fill_gradient(g_depth, DXGI_FORMAT_R32_FLOAT);
    fill_gradient(g_motion, DXGI_FORMAT_R16G16_FLOAT);
    g_ctx->Flush();

    ffx12::set_sdk_dll_path(k_sdk_dll);
    CHECK(!ffx12::active(), "backend inactive before dispatch");

    ffx12::FrameInput in {};
    in.color = g_color.Get();
    in.depth = g_depth.Get();
    in.motion = g_motion.Get();
    in.output_target = g_output.Get();
    in.render_w = k_render_w;
    in.render_h = k_render_h;
    in.display_w = k_display_w;
    in.display_h = k_display_h;
    in.jitter_x = 0.0f;
    in.jitter_y = 0.0f;
    in.motion_scale_x = 1.0f;
    in.motion_scale_y = 1.0f;
    in.camera_near = 0.1f;
    in.camera_far = 1000.0f;
    in.camera_fov_vertical = 1.0472f;
    in.frame_time_delta_ms = 16.7f;
    in.reset = true;

    const bool ok1 = ffx12::dispatch(in, g_ctx.Get());
    std::printf("  dispatch#1=%d version=%s\n", ok1 ? 1 : 0, ffx12::selected_version_name());
    CHECK(ok1, "dispatch #1 (auto-init) ok");
    CHECK(ffx12::active(), "backend active after dispatch");

    // 第二帧（reset=false，验证连续派发）
    in.reset = false;
    const bool ok2 = ffx12::dispatch(in, g_ctx.Get());
    std::printf("  dispatch#2=%d\n", ok2 ? 1 : 0);
    CHECK(ok2, "dispatch #2 (steady frame) ok");

    verify_output();

    ffx12::shutdown();
    CHECK(!ffx12::active(), "backend inactive after shutdown");
    std::printf(g_failures == 0 ? "ALL PASS\n" : "FAILURES: %d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
