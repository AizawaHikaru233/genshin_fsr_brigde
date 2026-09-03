// SPDX-License-Identifier: GPL-3.0-or-later
// gdds_interop.h — GDDS DirectStorage GPU 解压互操作层
//
// 链路（Phase 1 GPU 实测通过）：
//   自建同适配器 D3D12 设备 → DirectStorage 队列（qd.Device 必须设）
//   → TEXTURE_REGION 请求 GPU GDeflate 解压直写共享纹理
//   → 共享 fence（D3D12 Signal → D3D11 Wait）→ 替换纹理就绪
//
// 设计决策：
//   - 自建同适配器 D3D12 设备（不绑定 Bridge）——性能影响最低（无 fence
//     共锁、失败隔离），多设备实例合法（OptiScaler/ReShade 并存无异常）
//   - 惰性初始化：仅首次遇 .gdds 时创建（不增加无 GDDS 场景的启动开销）
//   - 跨 API 交接：共享纹理回 COMMON；UncompressedSize 由统一层
//     GetCopyableFootprints 计算（非插件猜测）

#pragma once

#include <windows.h>
#include <d3d11.h>
#include <cstdint>

namespace tloader_gdds
{

// 惰性初始化：D3D12 设备（同适配器）+ DirectStorage 工厂/队列 + 共享 fence。
// 线程安全；重复调用幂等。game_device 为游戏 D3D11 设备（取适配器 LUID）。
bool Initialize(ID3D11Device *game_device);

// GDDS 文件 → 共享纹理（GPU GDeflate 解压直写，零拷贝）。
// 成功返回替换纹理（AddRef 后，调用方必须 Release）；失败返回 nullptr。
// *out_skipped（可为 nullptr）：因 max_texture_side 等配置主动跳过（非错误）
// 时为 true——调用方据此记 [skip] 而非 [fail]。
// 注意：DS 完成事件已保证 GPU 侧写入完成；D3D11 侧可见性需由**渲染线程**
// 在绑定前执行 WaitOnRenderThread(ready_fence_value) 建立（立即上下文非线程
// 安全——严禁在后台线程调用 ctx4->Wait，会与游戏渲染提交竞态导致 TDR）。
// 返回的纹理对应的 fence 值写入 *out_ready_fence（可为 nullptr）。
ID3D11Texture2D *LoadGddsTexture(const wchar_t *gdds_path, uint64_t *out_ready_fence,
                                 bool *out_skipped = nullptr);

// 渲染线程在绑定替换纹理前调用：等待 GDDS 写入完成（幂等，仅当 fence 值
// 未达到时阻塞；DS 已完成后通常立即返回）。必须在游戏渲染线程（拥有立即
// 上下文的线程）调用。
void WaitOnRenderThread(ID3D11DeviceContext *immediate_ctx, uint64_t fence_value);

// 进程退出清理（D3D12 设备/DS 队列/共享 fence）。
void Shutdown();

bool Active();

} // namespace tloader_gdds
