#pragma once
// Ffx12Backend.h — 进程内 ffx-api DX12 后端。
// 目标：桥在自有 D3D12 设备上通过部署的 runtime provider 运行 FSR，
// 输入/输出经 D3D11 共享纹理与游戏互操作。
//
// 已验证（Phase 0 测试宿主）：legacy SHARED 句柄 + D3D12 OpenSharedHandle、
// ffxApi 版本查询（4.1.1/3.1.5/2.3.4）、ffxOverrideVersion 链（backend 在前）选择 2.3.4、
// ffxCreateContext/ffxDispatch 真实渲染、读回一致。

#include <cstdint>
#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace ffx12
{
struct FrameInput
{
    ID3D11Texture2D *color = nullptr;       // render-size（游戏场景颜色）
    ID3D11Texture2D *depth = nullptr;       // render-size（深度）
    ID3D11Texture2D *motion = nullptr;      // render-size（运动矢量）
    ID3D11Texture2D *transparency = nullptr; // render-size（原生 T4 transparency/composition mask）
    ID3D11Texture2D *output_target = nullptr; // display-size（写入目标）
    std::uint32_t render_w = 0, render_h = 0;
    std::uint32_t display_w = 0, display_h = 0;
    float jitter_x = 0.0f, jitter_y = 0.0f;
    float motion_scale_x = 1.0f, motion_scale_y = 1.0f;
    float camera_near = 0.1f;
    float camera_far = 1000.0f;
    float camera_fov_vertical = 1.0472f;
    float frame_time_delta_ms = 16.7f;
    float pre_exposure = 1.0f;
    bool reset = false;
    bool enable_sharpening = false;
    // 从游戏运动纹理的第三通道提取的 R8 reactive mask；仅在后端能够安全生成时使用。
    bool use_reactive_mask = false;
    bool use_transparency_mask = false;
    float sharpness = 0.0f;
};

// 初始化：解析游戏 D3D11 设备的适配器 → 建 D3D12 设备 → 加载 ffx-api runtime。
// sdk_dll_path 是提供 ffxCreateContext/ffxDispatch 的运行时 DLL。失败返回 false。
bool init(ID3D11Device *game_device, const wchar_t *sdk_dll_path);
void shutdown();
bool active();
// 设置 ffx-api runtime DLL 路径。
void set_sdk_dll_path(const wchar_t *path);
// 运行期配置（桥 load_config 时调用；启动后固定，变更需重启）。
//  - depth_inverted：游戏深度为逆深度（0=far）时置 true（FSR2 默认假设 0=near）。
//  - decode_motion：motion 纹理为 R10G10B10A2 signed-in-unorm 时置 true，
//    后端用 compute pass 解码为 R16G16_FLOAT 再交给 FSR2（否则 FSR2 按 UNORM 读出 0.5px 假位移）。
void set_depth_inverted(bool inverted);
void set_decode_motion(bool decode);
void set_motion_flip(float flip); // ：XeSS/DLSS 定向——motion 方向翻转（±1）
void set_depth_scale(float scale); // ：XeSS/DLSS 定向——depth 值域归一化（XeSS 期望 [0,1]）
// 运动矢量是否已经包含投影 jitter。仅在已包含时启用 SDK 的 jitter cancellation。
void set_motion_vectors_jittered(bool jittered);
// 输入颜色为 HDR（10-bit 线性 HDR 管线）时置 true（FSR2 需按 HDR 处理颜色/曝光）。
void set_hdr_input(bool hdr);
// 自动曝光 + 非线性色彩空间（OptiScaler 实测 initFlags 含 AUTO_EXPOSURE|NON_LINEAR_COLORSPACE）。
void set_auto_exposure(bool auto_exposure);
void set_non_linear(bool non_linear);
// PQ 链开关：=true 时走 PqToLinear→FSR2→LinearToPq（旧链，线性输入会致自动曝光爆炸→白屏）；
// =false（默认，复刻游戏原生 0x129 标志组合）时直喂 PQ 值 + HDR|NON_LINEAR|AUTO_EXPOSURE。
void set_use_pq_chain(bool use_chain);
// fVelocityFactor（0.0-1.0）：FSR2 高亮像素时间稳定性因子（0 改善高亮边缘闪烁）。
void set_velocity_factor(float factor);
// 选择 ffxApi 版本（"2.3.4"/"3.1.5"/"4.1.1"，默认 "2.3.4"）。桥 load_config 时调用。
void set_sdk_version(const char *name);
// 适配器 LUID（诊断：验证 D3D11/D3D12 同适配器）。
void adapter_luids(std::uint64_t &d11_luid, std::uint64_t &d12_luid);
// OptiScaler-style DX11-on-12 capability probe result (does not imply that
// native game D3D11 resources can be wrapped in-place).
void interop_capabilities(bool &dx11on12, bool &gpu_only_transport);
// 原生 D3D11 纯 GPU 传输（自建 D3D12 设备 + NT 共享句柄纹理 +
// D3D11.4 共享 fence，不劫持游戏设备为 On12）。遵守该开关时后端在 dispatch 中
// 走 dispatch_gpu_shared（GPU 拷贝进共享纹理 → FFX12 → GPU 拷贝出），无 CPU staging。
void set_gpu_interop(bool enable);
// 真实状态：初始化成功且共享 fence/纹理就绪（供桥诊断日志与回退判断）。
bool gpu_interop_ready();
// AMD SDK 消息缓冲（fpMessage 回调捕获，诊断版本/功能启用）。
void get_sdk_messages(std::wstring &out);
// 输出标记（诊断）：=true 时 ffx12 输出加亮红点（左上角+中央），验证画面来源。
void set_output_mark(bool mark);
// 解码常数注入测试（诊断）：=true 时 PQ 解码 pass 输出固定 0.5（判断 pass 执行 vs SRV 读取断裂）。
void set_decode_test(bool test);
// motion 解码金丝雀——=true 时 motion 解码 pass 输出固定 (0.5,0.5)（无视输入）。
// 判读：cvt 变 0.5 → pass 执行、描述符链路 OK（问题在 SRV 源/输入交接）；
//       cvt 仍 0 → pass 未执行或 UAV/描述符断。
void set_motion_decode_test(bool test);
// 静止死区——=true 时 motion 解码把 |mv|<0.0005UV（≈1px@1920）的像素归零，
// 触发 FSR 静止锁定（消除静止残余 motion 驱动的时间累积网格/竖纹）。动态不受影响。
void set_motion_deadzone(bool enable);
// 与 OptiScaler 共存兼容——
// preload：注入早期（OptiScaler loader hook 前）装载 FFX 模块并缓存句柄。
// passthrough 机制已整体移除（实测会让 OptiScaler 丢失 FFX 输入识别）——
// 所有显卡统一桥直连；OptiScaler 共存时并行（各自独立链路，实测无冲突）。
void preload(const wchar_t *path);
// 诊断：后端输出共享纹理的 D3D11 侧（桥读回对比 CopyResource 是否生效）。
ID3D11Texture2D *debug_output_texture();
// 诊断：后端输入 color 共享纹理的 D3D11 侧（验证输入拷贝是否成功）。
ID3D11Texture2D *debug_color_texture();
// 诊断：后端 D3D12 中间纹理中央像素（readback 采集）：[0]=PQ解码输出, [1]=ffxDispatch输出,
// [2]=PQ编码输出, [3]=motion解码输出。每项 4 float (r,g,b,a)。
void debug_pixels(float out_pixels[16]);
// 诊断：D3D12 debug layer（info queue 消息捕获到 SDK 消息缓冲）。桥配置调用。
void set_debug_layer(bool enable);
// 实际输入/输出格式（DXGI_FORMAT 值，诊断用）。
void input_formats(std::uint32_t &color, std::uint32_t &depth, std::uint32_t &motion, std::uint32_t &output);
// 诊断：最近一次 dispatch 的 reset 标志 + 上下文重建累计次数。
void debug_state(bool &last_reset, std::uint64_t &ctx_recreates);
// 最近一次实际 ffxDispatch 的返回码。负哨兵值表示本次调用尚未到达 FFX dispatch
// （例如资源/上下文准备阶段已经失败）。仅用于 bridge 的结果日志。
int last_ffx_dispatch_return_code();
// 诊断（2026-08-25）：dump 前 N 帧 FSR4 输出（enc readback，R10G10B10A2 raw，游戏 cwd）。
// 用于离线分析"边缘抖动"：连续帧 diff 的振荡模式（亚像素 vs 像素级）。
void set_dump_frames(std::uint32_t n);
// 诊断（E1）：D3D12 自有输出纹理（PQ 编码结果，readback 采集）5 点原始像素。
// 用于区分"D3D12 链不渲染"（全 0）vs "写了共享纹理但 D3D11 不可见"（有内容）——
// P0-1 修复后输出经 readback → D3D11 UpdateSubresource 落地共享纹理。
void get_output_samples(std::uint32_t raw[5], std::uint32_t &w, std::uint32_t &h, bool &valid);

// 诊断（链中点二分）：motion_cvt（金丝雀）/ color_linear（PQ 解码）/ output_linear（ffxDispatch）
// 中央像素——定位 D3D12 链断点（执行 / 共享输入读 / 解码 / 派发 / 编码）。
struct ChainSampleData
{
    float motion_cvt[2] = {0, 0};
    float color_linear[3] = {0, 0, 0};
    float output_linear[3] = {0, 0, 0};
    std::uint32_t canary = 0; // 金丝雀中央原始值（0xFFC08040=cmdlist 执行+自有 UAV+readback 全通）
    std::uint32_t color_own = 0; // 自有输入颜色中央（非 0 = CPU 中转送达）
    float depth_own = 0.0f;      // 自有输入深度中央
    std::uint32_t motion_own = 0; // 自有输入运动中央
    bool valid = false;
};
void get_chain_samples(ChainSampleData &out);

// 每帧派发：拷贝输入 → D3D12 ffxDispatch(FSR2 2.3.4) → 拷贝输出。
// 内部懒创建共享纹理池与 FSR2 上下文（尺寸变化时重建）。
// instance_key：区分游戏多个 FFX_FSR2 实例（主渲染/UI 次渲染），每实例独立 context 防历史串流。
// 线程安全（内部互斥）。返回 false 表示本帧失败（调用方回退）。
bool dispatch(const FrameInput &input, ID3D11DeviceContext *game_context, std::uint64_t instance_key = 0);

// 查询当前选择的后端版本名（"2.3.4"/"4.1.1"/...）——诊断用。
const char *selected_version_name();
// 实际匹配到的 provider 版本名（"4.1.1"/"4.0.2c"/"3.1.5" 等，含降级结果）。
const char *matched_version_name();
} // namespace ffx12
