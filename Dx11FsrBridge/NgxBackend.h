// NgxBackend.h —— D3D11 NGX（DLSS Super Resolution）后端
// 与 Ffx12Backend 完全解耦：不共享全局、不共享 D3D12 设备、不共享已编译 CS。
#pragma once
#include <d3d11.h>
#include <cstdint>
#include <string>
#include <vector>
#include <wrl/client.h>
#include "Ffx12Backend.h"   // ffx12::FrameInput / ffx12::BackendKind

namespace ngx {

// ---- 配置（由 Dx11FsrBridge.cpp 的 ini 读取点调用，全部幂等，只存值）----
// ★ BackendKind / set_backend() / backend() 归 ffx12（Ffx12Backend.h），不在这里。
//   理由：分流点在 ffx12::dispatch() 体内，该 TU 没有 using ngx::…（第七轮 BLK-4）。
void set_application_id(unsigned long long id);             // NgxApplicationId，默认 1337
void set_feature_dirs(const std::vector<std::wstring> &d);  // NgxFeatureDirs；空 = 自动探测
void set_hdr(bool on);                                      // NgxHdr，默认 true（旧接口；T-8 起由 set_hdr_mode 取代，保留不删）
// T-8：HDR 三态。mode: 0=auto（默认，自动探测）1=强制HDR 2=强制SDR。
// auto 时按**三级**顺序探测（与 NgxBackend.cpp 的 resolve_is_hdr 一一对应）：
//   ① swapchain color space（set_swapchain_hdr，由 SetColorSpace1 hook 调用）——最准；
//   ② swapchain buffer 格式（set_swapchain_format，由 install_swapchain_hooks 主动读）——
//      覆盖"游戏从不调 SetColorSpace1"的场合；
//   ③ 输入纹理格式（resolve_is_hdr 的 color_fmt 参数）——最后兜底。
void set_hdr_mode(int mode);                 // NgxHdr = auto / 1 / 0
void set_swapchain_hdr(bool hdr);            // 由 Dx11FsrBridge 的 SetColorSpace1 hook 调用
// T-8 M4-b：swapchain 后缓冲格式（DXGI_SWAP_CHAIN_DESC::BufferDesc.Format）。
// ★ 为什么需要它：DXGI **没有** GetColorSpace（IDXGISwapChain3 只有
//   CheckColorSpaceSupport / SetColorSpace1），无法主动读回"当前" color space。
//   若游戏从不调用 SetColorSpace1，上面的 set_swapchain_hdr 永远不会被调到 ⇒
//   只能靠 install_swapchain_hooks() 主动读 BufferDesc.Format 作为第二来源。
//   DXGI_FORMAT_UNKNOWN(0) 表示"未知/不参与判定"。
void set_swapchain_format(DXGI_FORMAT f);    // 由 install_swapchain_hooks() 主动调用
void set_render_preset(int v);                              // NgxRenderPreset，默认 0（跟随 NGX）
void set_frame_time_delta(bool on);                         // NgxSetFrameTimeDelta，默认 true
void set_use_reactive_mask(bool on);                        // NgxUseReactiveMask，默认 false
void set_log_level(int lv);                                 // NgxLogLevel 0..3，默认 1
void set_passthrough(bool on);                              // NgxPassthrough，默认 false

// ---- 查询 ----
bool active();          // 已成功初始化且 available==1
bool available();       // 最近一次探测结果
bool passthrough();

// ---- 每帧（唯一入口；初始化在内部懒完成）----
// 返回 true  = NGX 已完成超分并把结果写入 in.output_target（调用方应 return true）。
// 返回 false = 本帧未处理，调用方走原生放行（fail-open，D9）。
//
// ★ 没有公开的 init()。首次调用时内部执行：
//     ID3D11Device *dev = nullptr;
//     ctx->GetDevice(&dev);
//     if (dev == nullptr) return false;
//     const bool ok = ensure_init(dev);   // 幂等；只在首次成功时 AddRef 存 g_device
//     dev->Release();                     // ★ 必须成对，否则每帧泄漏一个引用计数
bool dispatch(const ffx12::FrameInput &in, ID3D11DeviceContext *ctx,
              std::uint64_t instance_key);

// ---- fail-open 信号（D9 两道拦截共用）----
// 只读 std::atomic<bool> g_release_all；不碰容器、不取任何锁。
// 进程内一旦为 true 不再回落。语义是"全局放手"，不是"任一实例"。
bool should_release_all();

} // namespace ngx
