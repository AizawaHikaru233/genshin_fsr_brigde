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
// auto 时按**四级**顺序探测（与 NgxBackend.cpp 的 resolve_is_hdr 一一对应）：
//   ① swapchain color space（set_swapchain_hdr，由 SetColorSpace1 hook 调用）——最准；
//   ② Windows 显示配置 API（probe_displayconfig_hdr，ensure_init 时探一次）——
//      系统级真实 HDR 状态，不依赖游戏是否调 SetColorSpace1；
//   ③ swapchain buffer 格式（set_swapchain_format，由 install_swapchain_hooks 主动读）——
//      覆盖"游戏从不调 SetColorSpace1"的场合；
//   ④ 输入纹理格式（resolve_is_hdr 的 color_fmt 参数）——最后兜底。
// ★ 缓存策略：①②③ 都是事件驱动的（hook / ensure_init 写入），所以在**写入点**
//   预计算进 g_hdr_probe_cache，每帧只读一次；只有 ④ 依赖每帧的 color_fmt。
//   2026-11-09（PR #12 审查 E 项）：原注释写「三级」，实现已是四级，此处同步。
void set_hdr_mode(int mode);                 // NgxHdr = auto / 1 / 0
void set_swapchain_hdr(bool hdr);            // 由 Dx11FsrBridge 的 SetColorSpace1 hook 调用
// T-8 M4-b：swapchain 后缓冲格式（DXGI_SWAP_CHAIN_DESC::BufferDesc.Format）。
// ★ 为什么需要它：DXGI **没有** GetColorSpace（IDXGISwapChain3 只有
//   CheckColorSpaceSupport / SetColorSpace1），无法主动读回"当前" color space。
//   若游戏从不调用 SetColorSpace1，上面的 set_swapchain_hdr 永远不会被调到 ⇒
//   只能靠 install_swapchain_hooks() 主动读 BufferDesc.Format 作为第二来源。
//   DXGI_FORMAT_UNKNOWN(0) 表示"未知/不参与判定"。
void set_swapchain_format(DXGI_FORMAT f);    // 由 install_swapchain_hooks() 主动调用

// T-FIX-2：供 Dx11FsrBridge 的 is_hdr10_color_space 判断「G10 + FP16」组合。
// 返回 0 = 未知（DXGI_FORMAT_UNKNOWN），否则是当前 swapchain 后缓冲格式。
DXGI_FORMAT swapchain_format();
void set_render_preset(int v);                              // NgxRenderPreset，默认 0（跟随 NGX）
void set_frame_time_delta(bool on);                         // NgxSetFrameTimeDelta，默认 true
void set_use_reactive_mask(bool on);                        // NgxUseReactiveMask，默认 false
// NgxDepthScale：深度提取 CS 的乘数，默认 1.0（= 上游不缩放）。
// 游戏逆深度值域约 [0, ~0.03]，而 NGX 期望 [0,1]；设为 32 可让前后景可分。
// 这是量纲缩放，不改变方向，与 jitter/MV 的符号问题无关。
void set_depth_scale(float scale);
void set_log_level(int lv);                                 // NgxLogLevel 0..3，默认 1
void set_passthrough(bool on);                              // NgxPassthrough，默认 false
// NgxCrossKeyReuse，默认 true（ini 里 1）。语义：instance_key 变了，但建池/建 feature 真正
//   依赖的「render/display 尺寸 + color/output 格式」全等时，**复用**现有资源池与 feature，
//   不销毁重建。false（ini 里 0）⇒ 每次 key 变化都走原来的 destroy + create，与引入该键之前
//   逐字节同一条路径。
// ★ 为什么需要它：destroy_instance() 里的 g_release(feature) 实测 ~502ms（E2），
//   而进出角色界面这类 key 切换的尺寸/格式并不变，销毁重建纯属浪费 ⇒ ~500ms 卡顿 + 短暂黑屏。
void set_cross_key_reuse(bool on);

// 2026-11-10（B154）：NGX 实现来源（NgxProvider 键），默认 auto。
//   auto     = 先真 NGX（注册表 FullPath → _nvngx.dll），失败再找外部实现（默认）；
//   nvidia   = 只走真 NGX（与 v1 逐条相同：失败即确定性失败，不重试、不换来源）；
//   external = 只走外部实现：先 LoadLibraryW("_nvngx.dll")（OptiScaler 的 LoadLibrary
//              钩子会返回它自己），再枚举已加载模块，找导出 NVSDK_NGX_D3D11_Init_Ext 的那个。
// ★ 外部实现只在"注册表不存在 / 装载失败"这条路上改变结果：真 NGX 一旦拿到，行为不变。
// ★ auto 的 external 失败是**可重试**的（12 × 5 s 窗口），用尽才按 provider_missing 收尾 ——
//   外部实现何时加载不由本 DLL 决定。
void set_provider_mode(int mode);                           // NgxProvider（0=auto 1=nvidia 2=external）

// ---- 查询 ----
bool active();          // 已成功初始化且 available==1
bool available();       // 最近一次探测结果
bool passthrough();
// 当前是否允许跨 key 复用（返回下游消费者读到的**同一个值**，见 NgxCrossKeyReuse）。
bool cross_key_reuse();

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
