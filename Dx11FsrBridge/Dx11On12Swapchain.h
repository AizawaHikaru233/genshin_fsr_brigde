#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi.h>
#include <dxgi1_2.h>

#include <string>

// Optional process-start renderer bootstrap.  When enabled it returns a
// D3D11On12 device to the game and presents through a D3D12 queue-backed flip
// swapchain.  All functions return false when the request is not compatible;
// callers must then invoke the original D3D11/DXGI entry point unchanged.
namespace dx11on12_swapchain
{
// True only while this module calls DXGI internally to create the real D3D12
// swapchain.  Bridge factory hooks must bypass themselves in that window.
bool internal_factory_create();

// Returns AddRef'd interfaces only for a device created by this module's On12
// bootstrap.  Callers must use the returned queue for all unwrap/FFX work.
bool get_runtime_interfaces(
    ID3D11Device *device,
    ID3D12Device **d12,
    ID3D12CommandQueue **queue,
    ID3D11On12Device2 **on12);

// D3D11On12 wrapped backbuffers must be acquired again after every Present.
// Bridge context hooks call these immediately before a target is bound, cleared
// or drawn to; non-swapchain views are ignored.
void acquire_render_target_views(ID3D11RenderTargetView *const *rtvs, UINT count);
void acquire_current_render_targets(ID3D11DeviceContext *context);

bool create_device(
    IDXGIAdapter *adapter,
    D3D_DRIVER_TYPE driver_type,
    UINT d3d11_flags,
    const D3D_FEATURE_LEVEL *feature_levels,
    UINT feature_levels_count,
    ID3D11Device **device,
    D3D_FEATURE_LEVEL *feature_level,
    ID3D11DeviceContext **context,
    std::string &reason);

bool create_device_and_swapchain(
    IDXGIAdapter *adapter,
    D3D_DRIVER_TYPE driver_type,
    UINT d3d11_flags,
    const D3D_FEATURE_LEVEL *feature_levels,
    UINT feature_levels_count,
    const DXGI_SWAP_CHAIN_DESC *desc,
    IDXGISwapChain **swapchain,
    ID3D11Device **device,
    D3D_FEATURE_LEVEL *feature_level,
    ID3D11DeviceContext **context,
    std::string &reason);

bool create_factory_swapchain(
    IDXGIFactory *factory,
    IUnknown *game_device,
    const DXGI_SWAP_CHAIN_DESC *desc,
    IDXGISwapChain **swapchain,
    std::string &reason);

bool create_factory_swapchain_for_hwnd(
    IDXGIFactory2 *factory,
    IUnknown *game_device,
    HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1 *desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen_desc,
    IDXGIOutput *restrict_to_output,
    IDXGISwapChain1 **swapchain,
    std::string &reason);
} // namespace dx11on12_swapchain
