#include <Windows.h>
#include <TlHelp32.h>
#include <d3d11.h>
#include <d3d11_3.h>
#include <d3d11_4.h>
#include <d3d11on12.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <dxgi1_6.h>
#include <detours/detours.h>
#include "RenderScaleMenu.h"
#include "Fsr2FamilyTakeover.h"
#include "Il2CppCallSiteHook.h"
#include "Ffx12Backend.h"
// 旧方案（On12 引导 / FSR2 翻译层）已移除，不再编译。

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
std::once_flag g_initialize_once;
constexpr std::size_t k_context_vtable_size = 128;
constexpr std::size_t k_context4_vtable_size = 149;
constexpr std::size_t k_device_vtable_size = 80;
// DLSSG exposes the returned DX11-on-DX12 swap chain as IDXGISwapChain4.
// Keep all inherited entries when cloning its vtable, not only the older
// IDXGISwapChain2-sized prefix.
constexpr std::size_t k_swapchain_vtable_size = 41;
constexpr std::size_t k_factory_vtable_size = 32;
constexpr std::size_t k_factory2_vtable_size = 40;
#if defined(DX11FSRBRIDGE_COLOR_DIAGNOSTICS)
constexpr bool k_color_diagnostics_enabled = true;
#else
constexpr bool k_color_diagnostics_enabled = false;
#endif
constexpr std::size_t k_idx_device_create_buffer = 3;
constexpr std::size_t k_idx_device_create_texture_2d = 5;
constexpr std::size_t k_idx_device_create_vertex_shader = 12;
constexpr std::size_t k_idx_device_create_pixel_shader = 15;
constexpr std::size_t k_idx_device_create_compute_shader = 18;
constexpr std::uint32_t k_context_base_methods = 7;
constexpr std::size_t k_idx_vs_set_constant_buffers = k_context_base_methods + 0;
constexpr std::size_t k_idx_ps_set_shader_resources = k_context_base_methods + 1;
constexpr std::size_t k_idx_ps_set_shader = k_context_base_methods + 2;
constexpr std::size_t k_idx_vs_set_shader = k_context_base_methods + 4;
constexpr std::size_t k_idx_ps_set_constant_buffers = k_context_base_methods + 9;
constexpr std::size_t k_idx_map = k_context_base_methods + 7;
constexpr std::size_t k_idx_unmap = k_context_base_methods + 8;
constexpr std::size_t k_idx_draw_indexed = k_context_base_methods + 5;
constexpr std::size_t k_idx_draw = k_context_base_methods + 6;
constexpr std::size_t k_idx_om_set_render_targets = k_context_base_methods + 26;
constexpr std::size_t k_idx_om_set_render_targets_and_uavs = k_context_base_methods + 27;
constexpr std::size_t k_idx_dispatch = k_context_base_methods + 34;
constexpr std::size_t k_idx_rs_set_viewports = k_context_base_methods + 37;
constexpr std::size_t k_idx_copy_subresource_region = k_context_base_methods + 39;
constexpr std::size_t k_idx_copy_resource = k_context_base_methods + 40;
constexpr std::size_t k_idx_update_subresource = k_context_base_methods + 41;
constexpr std::size_t k_idx_clear_rtv = k_context_base_methods + 43;
constexpr std::size_t k_idx_clear_dsv = k_context_base_methods + 46;
constexpr std::size_t k_idx_cs_set_shader_resources = k_context_base_methods + 60;
constexpr std::size_t k_idx_cs_set_uavs = k_context_base_methods + 61;
constexpr std::size_t k_idx_cs_set_shader = k_context_base_methods + 62;
constexpr std::size_t k_idx_cs_set_constant_buffers = k_context_base_methods + 64;
constexpr std::size_t k_swapchain_base_methods = 8;
constexpr std::size_t k_idx_present = k_swapchain_base_methods + 0;
constexpr std::size_t k_idx_set_fullscreen_state = k_swapchain_base_methods + 2;
constexpr std::size_t k_idx_get_fullscreen_state = k_swapchain_base_methods + 3;
constexpr std::size_t k_idx_resize_buffers = k_swapchain_base_methods + 5;
constexpr std::size_t k_idx_resize_target = k_swapchain_base_methods + 6;
constexpr std::size_t k_idx_check_color_space_support = 37;
constexpr std::size_t k_idx_set_color_space1 = 38;
constexpr std::size_t k_idx_set_hdr_metadata = 39;
constexpr std::size_t k_idx_factory_create_swap_chain = 10;
constexpr std::size_t k_idx_factory2_create_swap_chain_for_hwnd = 15;
constexpr std::size_t k_idx_output6_get_desc1 = 27;

struct Config
{
    bool enabled = true;
    bool enable_logging = false;
    DWORD target_process_id = 0;
    std::wstring target_process_name;
    bool log_all_dispatch = false;
    bool log_resource_ops = false;
    bool log_loader_activity = false;
    bool log_interesting_dispatch_details = false;
    bool hook_present = false;
    bool final_scene_probe = false;
    std::uint32_t final_scene_probe_limit = 6;
    std::uint32_t final_scene_probe_signature_limit = 128;
    bool final_scene_snapshot = false;
    std::uint32_t final_scene_snapshot_interval_frames = 240;
    bool final_scene_optifg_input = false;
    int dlssg_dxgi_workaround = -1;
    // Exposes HDR10 capability to the game while keeping the physical output
    // on the SDR color space. This is an isolated experimental path.
    bool hdr_swapchain_spoof = false;
    // Actively requests the legacy linear HDR color space on the physical
    // swapchain. It never rewrites swapchain or render-target formats.
    bool hdr_swapchain_force = false;
    // Records the system advanced-color capability query used by the engine.
    // This diagnostic path never changes the returned display state.
    bool hdr_environment_probe = false;
    // Records DXGI output color capabilities without changing output objects.
    bool hdr_output_desc_probe = false;
    // Reports HDR output capability to the game through the DXGI output
    // descriptor only. It does not change the physical display state.
    bool hdr_output_desc_spoof = false;
    // Experimental native-LDR test: request UNORM swapchain buffers instead of sRGB.
    bool native_ldr_swapchain_unorm = false;
    // Experimental process-start renderer replacement.  It is deliberately
    // opt-in: enables a D3D11On12 device plus a DX12 flip-model swapchain.
    bool dx11_on12_swapchain = false;
    // Experimental native-LDR test: rewrite only full-resolution RTV|SRV targets.
    bool native_ldr_final_target_unorm = false;
    // Compresses the spoofed HDR backbuffer to the SDR display range immediately
    // before Present. This is intentionally isolated from OptiScaler and the
    // game's render passes.
    bool hdr_sdr_tone_map = false;
    bool hdr_sdr_tone_map_pq_input = true;
    std::uint32_t hdr_sdr_tone_map_paper_white = 80;
    std::uint32_t hdr_sdr_tone_map_peak = 100;
    // Read-only identification of the last full-resolution draw into a known
    // swapchain target. It is used to select a future tone-map insertion point.
    bool hdr_composite_probe = false;
    std::uint32_t hdr_composite_probe_limit = 32;
    bool capture_metadata_only = true;
    bool dump_compute_shaders = false;
    bool dump_pixel_shaders = false;
    bool trace_pixel_shader_draws = false;
    bool trace_texture_creates = false;
    std::uint32_t texture_trace_hotkey = VK_F11;
    std::uint32_t texture_trace_duration_ms = 10000;
    std::uint32_t texture_trace_limit = 128;
    std::uint64_t trace_pixel_shader_hash = 0x78057A29AF6C2D99ull;
    std::uint32_t pixel_shader_trace_limit = 512;
    std::uint64_t target_pixel_shader_hash = 0x78057A29AF6C2D99ull;
    std::uint32_t pixel_shader_replacement_mode = 0;
#if defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
    bool enable_fsr2_get_proc_address_shim = false;
    std::uint32_t fsr2_translation_mode = 0;
    bool fsr2_fast_state_tracking = false;
    bool fsr2_mode2_on_demand_state = true;
    std::uint32_t fsr2_output_validation_target = 0;
    bool fsr2_motion_vectors_jittered = false;
    bool fsr2_positive_motion_vector_scale = false;
    bool fsr2_use_reactive_mask = false;
    bool fsr2_use_transparency_mask = false;
    std::uint32_t fsr2_jitter_mode = 0;
    std::uint32_t fsr2_dump_input_textures = 0;
    bool fsr2_compare_output_capture = false;
    std::uint32_t fsr2_sharpness_percent = 0;
    bool fsr2_hdr10_pq_color = false;
    bool fsr2_use_native_exposure = true;
    bool fsr2_fast_metadata_copy = false;
    bool fsr2_compact_linear_output = false;
    bool fsr2_lock_color_producer_shader = true;
    bool fsr2_gpu_timing = false;
    bool fsr2_reset_on_color_path_change = false;
    bool fsr2_reset_on_optiscaler_config_change = false;
    std::uint32_t fsr2_optiscaler_config_reset_frames = 4;
    bool fsr2_reset_on_optiscaler_log_change = false;
    std::uint32_t fsr2_optiscaler_log_reset_duration_ms = 4000;
    std::uint32_t fsr2_auto_recover_upscaler_ms = 0;
    bool fsr2_trace_color_producers = false;
    bool fsr2_early_output_probe = false;
    std::uint32_t fsr2_early_output_probe_frames = 60;
    bool block_dx11_on12_upscalers = true;
    // Phase 1：FSR2 5-PS 合成族跳过（默认关闭）。开启后在"上一次累积 pass 被桥成功替换"
    // 的前提下跳过 4 个 render-size 预处理 pass，消除双跑残余。见 Fsr2FamilyTakeover.h。
    bool fsr2_family_skip = false;
    std::uint32_t fsr2_family_expire_ms = 500;
    // Phase 1.5：il2cpp 调用点钩子（默认关闭）。FFX_FSR2::Render 的 inline patch。
    // observe 模式透传+计数（验证调用点/频率）；skip 模式实验性（切断 Mode 2 的
    // jitter/触发链，勿与 Fsr2TranslationMode=2 同开）。
    bool fsr2_il2cpp_hook = false;
    bool fsr2_il2cpp_skip_render = false;
    std::uint32_t fsr2_il2cpp_render_rva = 0x06B59670;
    std::uint32_t fsr2_il2cpp_ucb_rva = 0x06B59600;
    // 2026-08-26（AA 修复）：抓取游戏真实相机参数（near/far/FOV，RE 未确定）——
    // ConfigureJitteredProjectionMatrix(camera) 每帧被 OnPreCull 调用。
    bool ffx12_probe_camera = true;
    bool ffx12_camera_hook = false;    // 只在已验证完整序言后显式启用
    std::uint32_t ffx12_camera_rva = 0x06B558D0;
    bool ffx12_projection_hook = false; // 观察已构造的 Camera projection matrix
    std::uint32_t ffx12_projection_setter_rva = 0x013DC510;
    // Phase 1：进程内 FSR2 2.3.4 后端（ffxApi → amd_fidelityfx_upscaler_dx12.dll）。
    // 桥直接驱动 AMD SDK（不经 OptiScaler）；Phase 2 的调用点接管会调用它的 dispatch。
    bool ffx12 = false;
    std::wstring ffx12_dll_path;
    bool ffx12_fail_closed = false; // ：禁止回退原生（测试/故障显式暴露）
    bool ffx12_full_logging = false; // ：Release 下日志全开（排查用；log_line 不过滤）
    bool ffx12_probe = false; // 一次性槽位/cb0 探测（诊断用，默认关）
    bool optiscaler_bridge_probe = false; // 遗留 OptiScaler 候选桥路径（frames.jsonl 记录，默认关）
    std::uint32_t ffx12_jitter_mode = 4; // 0=+norm*width-0.5, 1=+norm*width(符号反→整体抖), 2=raw, 3=-norm*width+0.5, 4=-norm*width(FSR4实测:符号正确), 5=零
    bool ffx12_depth_inverted = true; // 游戏深度逆方向（0=far）；FSR2 默认 0=near
    bool ffx12_decode_motion = true;  // 游戏 motion 为 R10G10B10A2 平方编码 → 解码 R16G16_FLOAT
    bool ffx12_hdr_input = true;      // 游戏 10-bit HDR 管线
    bool ffx12_auto_exposure = true;  // 2026-08-25 用户确认：原神需要自动曝光
    bool ffx12_non_linear = true;     // 非线性色彩空间（OptiScaler 实证）
    float ffx12_velocity_factor = 0.5f; // FSR2 高亮稳定性（OptiScaler 用 0.5）
    // 运动矢量量级二分：保持已验证方向，只缩放送入 FSR2 的像素量级。
    // 1.0 为现有行为；0.5/2.0 用于隔离“解码量级错误”造成的历史重投影拖影。
    float ffx12_motion_scale = 1.0f;
    float ffx12_camera_near = 0.25f;
    float ffx12_camera_far = 6000.0f;
    // 同一 Render generation 的双缓冲输出只复制首个 SDK 结果，绝不二次推进 FSR2 history。
    bool ffx12_reuse_same_generation = true;
    bool ffx12_output_mark = false;     // 输出标记（诊断：验证画面来源是否 ffx12）
    float ffx12_fov_scale = 1.0f;       // FOV 缩放（AA 重投影验证：1.0=45°）
    bool ffx12_jitter_delay = true;   // 2026-08-25 FSR4 实测：游戏双缓冲预录滞后一帧 → 用上一帧 jitter（jdelay=1）
    bool ffx12_force_reset = false;   // 诊断：每帧强制 FSR2 reset（与正常对比：相同=历史零贡献/单帧重建）
    // 原生 D3D11 纯 GPU 传输（自建 D3D12 + NT 共享句柄 + D3D11.4 fence）。
    // 默认开：GpuInteropProbe 已在本机字节级验证；不劫持游戏设备为 On12（进场前冻结根因）。
    bool ffx12_gpu_interop = true;
#endif
    bool show_osd = false;
    bool assume_phase_order = false;
    bool enable_similarity_probe = false;
    bool reset_similarity_on_recording = true;
    std::uint32_t candidate_limit_per_frame = 64;
    std::uint32_t interesting_dispatch_log_limit = 256;
    std::uint32_t interesting_dispatch_phase_gap_ms = 1500;
    std::uint32_t similarity_report_interval_ms = 2000;
    std::wstring run_label;
};

struct ResourceInfo
{
    std::uint64_t resource_key = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT view_format = DXGI_FORMAT_UNKNOWN;
    std::uint32_t bind_flags = 0;
    std::uint32_t misc_flags = 0;
};

struct BufferInfo
{
    std::uint64_t resource_key = 0;
    std::uint32_t byte_width = 0;
    std::uint32_t bind_flags = 0;
    std::uint32_t usage = 0;
    std::uint32_t binding_slot = UINT32_MAX;
    std::uint64_t last_update_hash = 0;
    std::uint32_t last_update_size = 0;
};

struct DispatchState
{
    std::array<BufferInfo, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> vs_cbs {};
    std::array<ResourceInfo, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> ps_srvs {};
    std::array<BufferInfo, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> ps_cbs {};
    std::array<ResourceInfo, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> cs_srvs {};
    std::array<ResourceInfo, D3D11_1_UAV_SLOT_COUNT> cs_uavs {};
    std::array<BufferInfo, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> cs_cbs {};
    std::array<ResourceInfo, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs {};
    ResourceInfo dsv {};
    std::uint64_t current_cs_shader = 0;
    std::uint64_t current_cs_hash = 0;
    std::size_t current_cs_size = 0;
    std::uint64_t current_vs_shader = 0;
    std::uint64_t current_vs_hash = 0;
    std::size_t current_vs_size = 0;
    std::uint64_t current_ps_shader = 0;
    std::uint64_t current_ps_hash = 0;
    std::size_t current_ps_size = 0;
    std::uint32_t viewport_width = 0;
    std::uint32_t viewport_height = 0;
    std::uint64_t frame_index = 0;
    std::uint32_t candidate_count = 0;
    std::uint32_t backbuffer_width = 0;
    std::uint32_t backbuffer_height = 0;
};

#if defined(DX11FSRBRIDGE_FINAL_SCENE_PROBE)
struct FinalSceneProbeCandidate
{
    std::uint64_t resource_key = 0;
    ResourceInfo source0 {};
    std::uint64_t pixel_shader_hash = 0;
    std::size_t pixel_shader_size = 0;
    std::uint64_t vertex_shader_hash = 0;
    std::size_t vertex_shader_size = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::uint32_t viewport_width = 0;
    std::uint32_t viewport_height = 0;
    std::uint32_t render_target_count = 0;
    std::uint32_t element_count = 0;
    bool indexed = false;
    bool backbuffer = false;
    std::uint32_t display_draw_ordinal = 0;
    std::uint32_t backbuffer_draw_ordinal = 0;
};

struct FinalSceneProbeFrame
{
    std::uint64_t frame_index = 0;
    std::uint32_t display_target_draws = 0;
    std::uint32_t backbuffer_draws = 0;
    std::array<FinalSceneProbeCandidate, 16> candidates {};
    std::uint32_t candidate_count = 0;
    std::array<FinalSceneProbeCandidate, 16> tail_candidates {};
    std::uint32_t tail_candidate_count = 0;
};

struct FinalSceneSnapshotState
{
    ID3D11Texture2D *texture = nullptr;
    ID3D11Query *completion_query = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::uint64_t queued_frame = 0;
    std::uint64_t queued_count = 0;
    std::uint64_t completed_count = 0;
    bool pending = false;
};
#endif

struct ColorSourceWrite
{
    std::uint64_t sequence = 0;
    std::string stage;
    std::uint64_t shader_hash = 0;
    std::size_t shader_size = 0;
    std::uint32_t call_x = 0;
    std::uint32_t call_y = 0;
    std::uint32_t call_z = 0;
    std::uint32_t viewport_width = 0;
    std::uint32_t viewport_height = 0;
    std::uint64_t vertex_shader_hash = 0;
    std::size_t vertex_shader_size = 0;
    std::vector<ResourceInfo> inputs;
    std::vector<BufferInfo> constant_buffers;
    std::vector<BufferInfo> vertex_constant_buffers;
};

struct OptiScalerBridgePacket
{
    std::uint64_t frame_index = 0;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    std::uint32_t render_width = 0;
    std::uint32_t render_height = 0;
    ResourceInfo color {};
    ResourceInfo motion {};
    ResourceInfo depth {};
    ResourceInfo output {};
    std::uint64_t compute_shader = 0;
    std::uint64_t compute_shader_hash = 0;
    std::wstring path = L"compute";
};

struct ShaderInfo
{
    std::uint64_t hash = 0;
    std::size_t bytecode_size = 0;
    bool reflected = false;
    UINT thread_x = 0;
    UINT thread_y = 0;
    UINT thread_z = 0;
    UINT cb_count = 0;
    UINT sampler_count = 0;
    UINT srv_2d_count = 0;
    UINT srv_3d_count = 0;
    UINT srv_other_count = 0;
    UINT uav_2d_count = 0;
    UINT uav_3d_count = 0;
    UINT uav_other_count = 0;
};

struct SimilarityStats
{
    ULONGLONG first_event_tick = 0;
    ULONGLONG last_event_tick = 0;
    std::uint64_t dispatch_count = 0;
    std::uint64_t draw_count = 0;
    std::uint64_t lowres_input_hits = 0;
    std::uint64_t fullres_output_hits = 0;
    std::uint64_t depth_input_hits = 0;
    std::uint64_t motion_input_hits = 0;
    std::uint64_t history_read_after_write_hits = 0;
    std::uint64_t temporal_chain_hits = 0;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    ULONGLONG last_report_tick = 0;
    std::unordered_map<std::uint64_t, std::uint64_t> cs_hash_counts;
    std::unordered_map<std::uint64_t, std::uint64_t> ps_hash_counts;
    std::unordered_map<std::uint64_t, std::uint64_t> ps_post_hash_counts;
    std::unordered_map<std::uint64_t, std::string> resource_last_writer;
    std::unordered_map<std::uint64_t, std::uint64_t> resource_read_after_write_counts;
    std::unordered_map<std::string, std::uint64_t> lowres_candidates;
    std::unordered_map<std::string, std::uint64_t> fullres_candidates;
    std::unordered_map<std::string, std::uint64_t> depth_candidates;
    std::unordered_map<std::string, std::uint64_t> motion_candidates;
    std::unordered_map<std::string, std::uint64_t> cs_2d_post_contexts;
    std::unordered_map<std::string, std::uint64_t> ps_post_contexts;
};

struct ModeMatch
{
    std::wstring name = L"未确定";
    std::uint32_t score = 0;
    std::uint32_t total = 0;
};

HMODULE g_module = nullptr;
Config g_config;
std::filesystem::path g_module_dir;
std::filesystem::path g_log_path;
#if defined(DX11FSRBRIDGE_FG_DXGI_DIAGNOSTICS)
std::atomic_uint64_t g_dxgi_swapchain_request_id = 0;
#endif
std::filesystem::path g_frames_path;
std::filesystem::path g_similarity_path;
std::filesystem::path g_ps_trace_path;
std::filesystem::path g_texture_trace_path;
std::mutex g_log_mutex;
#if defined(DX11FSRBRIDGE_FINAL_SCENE_PROBE)
std::mutex g_final_scene_probe_mutex;
std::unordered_set<std::uint64_t> g_final_scene_probe_backbuffers;
std::unordered_set<std::uint64_t> g_final_scene_probe_signatures;
FinalSceneProbeFrame g_final_scene_probe_frame;
std::mutex g_final_scene_snapshot_mutex;
FinalSceneSnapshotState g_final_scene_snapshot;
#endif
std::atomic_bool g_logging_enabled = false;
// 显卡路由摘要（initialize 早期产生、被 reset_log 清空）——存全局，active 后补打保证可见
static bool g_route_applied = false; // 路由已应用（防止设备补检重复应用）
// 路由信息（apply 时保存，焦点框首次成功/失败时输出，保证三行相邻）
static std::string g_route_gpu_name;
static std::string g_route_vendor_label;
static std::string g_route_sdk_path;

// OptiScaler 输出定向修复——XeSS/DLSS 的 jitter 需 ≤±0.5（像素/归一化），
// FSR 输出保持原样（互不干扰）。运行时按 OptiScaler.ini 的 Dx11Upscaler/Dx12Upscaler 判定
// （用户菜单切换会写回 INI；限频刷新）。
enum class OptiOutput
{
    Fsr,   // FSR 系（fsr21/22/31/31_12 等）——不干预
    XeSS,  // xess / xess_12
    Dlss,  // dlss
    Unknown
};
static std::atomic<OptiOutput> g_opti_output { OptiOutput::Unknown };
static std::atomic_uint64_t g_opti_output_check_tick { 0 };

static bool contains_ci(const std::wstring &hay, const wchar_t *needle); // 定义在后（classify 区）

static OptiOutput read_optiscaler_output_state()
{
    OptiOutput out = OptiOutput::Fsr; // 默认 FSR（不干预）
    const std::filesystem::path opti_ini =
        g_module_dir.parent_path() / L"OptiScaler" / L"OptiScaler.ini";
    if (!std::filesystem::exists(opti_ini))
        return out;
    wchar_t buf[64] {};
    const auto ini_path = opti_ini.c_str();
    // 当前 Bridge 的 dx11on12 输出由 Dx12Upscaler 决定（用户菜单切换写回该键）；
    // Dx11Upscaler 不参与判定。
    GetPrivateProfileStringW(L"Upscalers", L"Dx12Upscaler", L"", buf,
                             static_cast<DWORD>(std::size(buf)), ini_path);
    const std::wstring sel = buf;
    if (contains_ci(sel, L"xess"))
        out = OptiOutput::XeSS;
    else if (contains_ci(sel, L"dlss"))
        out = OptiOutput::Dlss;
    return out;
}

// 限频刷新（每 500ms——切换输出后快速跟随，避免 FSR/XeSS 参数互相污染）
static OptiOutput opti_output_current()
{
    const std::uint64_t now = GetTickCount64();
    const std::uint64_t last = g_opti_output_check_tick.load(std::memory_order_relaxed);
    if (last == 0 || now - last >= 500)
    {
        g_opti_output_check_tick.store(now, std::memory_order_relaxed);
        const OptiOutput fresh = read_optiscaler_output_state();
        g_opti_output.store(fresh, std::memory_order_relaxed);
        return fresh;
    }
    return g_opti_output.load(std::memory_order_relaxed);
}
// 显卡路由（定义于 initialize 之前；设备创建 hook 先于定义处使用，需前置声明）
static void apply_adapter_route(std::uint32_t vendor, std::uint32_t device, const std::wstring &desc,
                                const char *source);
static void route_from_d3d11_device(ID3D11Device *d3d11_device);
std::mutex g_ps_trace_mutex;
std::ofstream g_ps_trace_stream;
std::atomic_uint32_t g_ps_trace_count = 0;
std::atomic_uint64_t g_texture_trace_until_tick = 0;
std::atomic_uint32_t g_texture_trace_count = 0;
std::atomic_uint64_t g_current_ps_hash = 0;
std::atomic_uint64_t g_mode2_fast_target_ps_hash = 0;
std::atomic_uint64_t g_mode2_fast_target_ps_key = 0;
std::atomic_uint64_t g_trace_ps_cb0_key = 0;
std::mutex g_replacement_mutex;
std::vector<std::uint8_t> g_spatial_copy_bytecode;
bool g_spatial_copy_compile_attempted = false;
ID3D11Device *g_replacement_device = nullptr;
ID3D11PixelShader *g_spatial_copy_shader = nullptr;
bool g_spatial_copy_create_failed = false;
std::atomic_uint64_t g_replacement_draw_count = 0;

struct HdrSdrToneMapResources
{
    ID3D11Device *device = nullptr;
    ID3D11Texture2D *source_copy = nullptr;
    ID3D11ShaderResourceView *source_view = nullptr;
    ID3D11RenderTargetView *source_target_view = nullptr;
    ID3D11RenderTargetView *target_view = nullptr;
    ID3D11Texture2D *target_texture = nullptr;
    ID3D11VertexShader *vertex_shader = nullptr;
    ID3D11PixelShader *pixel_shader = nullptr;
    ID3D11SamplerState *sampler = nullptr;
    ID3D11Buffer *constants = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT width = 0;
    UINT height = 0;
    std::uint64_t target_key = 0;
};

std::mutex g_hdr_sdr_tone_map_mutex;
HdrSdrToneMapResources g_hdr_sdr_tone_map_resources;
struct HdrSdrToneMapDrawResources
{
    ID3D11Device *device = nullptr;
    ID3D11PixelShader *pixel_shader = nullptr;
    ID3D11Buffer *constants = nullptr;
};
HdrSdrToneMapDrawResources g_hdr_sdr_tone_map_draw_resources;
std::vector<std::uint8_t> g_hdr_sdr_tone_map_vs_bytecode;
std::vector<std::uint8_t> g_hdr_sdr_tone_map_ps_bytecode;
bool g_hdr_sdr_tone_map_shader_compile_attempted = false;
bool g_hdr_sdr_tone_map_shader_create_failed = false;
std::atomic_bool g_hdr_sdr_tone_map_logged = false;

std::mutex g_swapchain_backbuffer_mutex;
std::unordered_set<std::uint64_t> g_swapchain_backbuffer_resources;
std::mutex g_hdr_composite_probe_mutex;
std::unordered_set<std::uint64_t> g_hdr_composite_probe_signatures;
std::atomic_uint32_t g_hdr_composite_probe_count = 0;
#if defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
std::atomic_uint64_t g_fsr2_translation_dispatch_count = 0;
std::atomic_uint32_t g_fsr2_translation_failure_count = 0;
std::atomic_uint64_t g_fsr2_translation_candidate_count = 0;
std::atomic_uint64_t g_fsr2_stale_producer_fallback_count = 0;
std::atomic_uint64_t g_fsr2_late_composed_dispatch_count = 0;
std::atomic_bool g_fsr2_dx11on12_block_logged = false;
std::atomic_bool g_fsr2_output_validation_logged = false;
std::atomic_bool g_fsr2_input_textures_dumped = false;
std::atomic_bool g_fsr2_output_pair_dumped = false;
std::atomic_uint32_t g_fsr2_pre_color_capture_index = 0;
std::atomic_bool g_fsr2_pre_color_capture_key_down = false;
std::atomic_uint32_t g_fsr2_input_sequence_capture_index = 0;
std::atomic_uint32_t g_fsr2_input_sequence_frames_remaining = 0;
std::atomic_bool g_fsr2_input_sequence_key_down = false;
std::atomic_bool g_fsr2_color_producers_dumped = false;
std::atomic_bool g_fsr2_motion_producers_dumped = false;
std::atomic_bool g_fsr2_color_candidate_dumped = false;
std::atomic_bool g_fsr2_same_frame_capture_pending = false;
std::atomic_uint32_t g_fsr2_early_output_probe_frames_remaining = 0;
std::atomic_uint64_t g_fsr2_candidate_color_resource = 0;
std::atomic_uint64_t g_fsr2_candidate_producer_output_resource = 0;
std::atomic_uint64_t g_fsr2_candidate_producer_generation = 0;
std::atomic_uint64_t g_fsr2_dynamic_producer_generation = 0;
std::atomic_uint64_t g_fsr2_locked_color_producer_ps_hash = 0;
std::atomic_uint64_t g_fsr2_rejected_color_producer_count = 0;
std::atomic_uint64_t g_fsr2_candidate_sequence = 0;
std::mutex g_fsr2_candidate_color_view_mutex;
ID3D11ShaderResourceView *g_fsr2_candidate_color_view = nullptr;
struct Fsr2DynamicColorTarget
{
    std::uint64_t resource_key = 0;
    std::uint32_t render_width = 0;
    std::uint32_t render_height = 0;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
};
std::mutex g_fsr2_dynamic_color_path_mutex;
std::deque<Fsr2DynamicColorTarget> g_fsr2_dynamic_color_targets;
std::unordered_map<std::uint64_t, std::uint64_t> g_fsr2_latest_producer_write_generations;
std::unordered_map<std::uint64_t, std::uint64_t> g_fsr2_consumed_producer_generations;
std::unordered_map<std::uint64_t, bool> g_fsr2_late_path_states;
std::atomic_uint64_t g_fsr2_color_path_switch_count = 0;
std::atomic_uint64_t g_fsr2_optiscaler_config_next_poll_tick = 0;
std::atomic_uint64_t g_fsr2_optiscaler_config_last_write = 0;
std::atomic_uint32_t g_fsr2_optiscaler_config_reset_frames_remaining = 0;
std::atomic_uint64_t g_fsr2_optiscaler_log_next_poll_tick = 0;
std::atomic_uint64_t g_fsr2_optiscaler_log_last_write = 0;
std::atomic_uint64_t g_fsr2_optiscaler_log_reset_until_tick = 0;
std::optional<Fsr2DynamicColorTarget> match_fsr2_dynamic_color_producer();
struct Fsr2ColorReplayState
{
    ID3D11PixelShader *pixel_shader = nullptr;
    std::array<ID3D11ShaderResourceView *, 7> shader_resources {};
    ID3D11Buffer *constant_buffer = nullptr;
    std::array<ID3D11SamplerState *, 6> samplers {};
    UINT exposure_slot = UINT_MAX;
    std::uint64_t producer_output_resource_key = 0;
    std::uint64_t producer_generation = 0;
    std::uint32_t render_width = 0;
    std::uint32_t render_height = 0;
};
std::mutex g_fsr2_color_replay_mutex;
Fsr2ColorReplayState g_fsr2_color_replay_state;
ID3D11Device *g_fsr2_color_replay_device = nullptr;
ID3D11Texture2D *g_fsr2_color_replay_output = nullptr;
ID3D11ShaderResourceView *g_fsr2_color_replay_output_view = nullptr;
std::uint32_t g_fsr2_color_replay_output_width = 0;
std::uint32_t g_fsr2_color_replay_output_height = 0;
DXGI_FORMAT g_fsr2_color_replay_output_format = DXGI_FORMAT_UNKNOWN;
std::atomic_uint64_t g_fsr2_color_replay_count = 0;
struct Fsr2GpuTimingSlot
{
    ID3D11Query *disjoint = nullptr;
    std::array<ID3D11Query *, 5> timestamps {};
    bool pending = false;
};
std::mutex g_fsr2_gpu_timing_mutex;
ID3D11Device *g_fsr2_gpu_timing_device = nullptr;
std::array<Fsr2GpuTimingSlot, 8> g_fsr2_gpu_timing_slots {};
std::uint32_t g_fsr2_gpu_timing_cursor = 0;
std::array<double, 4> g_fsr2_gpu_timing_accumulated_ms {};
std::uint32_t g_fsr2_gpu_timing_sample_count = 0;
std::uint32_t g_fsr2_gpu_timing_unavailable_streak = 0;
ULONGLONG g_fsr2_gpu_timing_last_recovery_tick = 0;
std::atomic_bool g_fsr2_translation_recovery_requested = false;
// 上次成功接管（skip_original_draw）的 tick；用于检测"翻译空窗"（切原生档、
// 加载、过场、dispatch 失败回退）后恢复时强制重置 FSR2 历史，避免旧场景鬼影。
std::atomic<ULONGLONG> g_fsr2_last_translation_tick { 0 };
std::mutex g_fsr2_neutral_exposure_mutex;
ID3D11Device *g_fsr2_neutral_exposure_device = nullptr;
ID3D11Texture2D *g_fsr2_neutral_exposure_texture = nullptr;
ID3D11ShaderResourceView *g_fsr2_neutral_exposure_view = nullptr;
std::atomic_bool g_fsr2_transient_capture_key_down = false;
std::atomic_uint32_t g_fsr2_transient_capture_frames_remaining = 0;
std::atomic_uint32_t g_fsr2_transient_capture_session = 0;
std::atomic_uint32_t g_fsr2_transient_capture_sample = 0;
std::mutex g_fsr2_transient_capture_mutex;
thread_local std::uint32_t g_fsr2_transient_capture_current_session = 0;
thread_local std::uint32_t g_fsr2_transient_capture_current_sample = 0;
thread_local bool g_fsr2_transient_capture_snapshot = false;
thread_local bool g_fsr2_transient_capture_result_recorded = false;
#endif
thread_local bool g_internal_bridge_dispatch = false;

struct ScopedInternalBridgeDispatch
{
    ScopedInternalBridgeDispatch()
    {
        g_internal_bridge_dispatch = true;
    }

    ~ScopedInternalBridgeDispatch()
    {
        g_internal_bridge_dispatch = false;
    }

    ScopedInternalBridgeDispatch(const ScopedInternalBridgeDispatch &) = delete;
    ScopedInternalBridgeDispatch &operator=(const ScopedInternalBridgeDispatch &) = delete;
};

std::mutex g_state_mutex;
std::atomic_bool g_active { false };
DispatchState g_state;
std::mutex g_color_source_mutex;
std::unordered_map<std::uint64_t, std::deque<ColorSourceWrite>> g_color_source_writes;
std::atomic_uint64_t g_color_source_sequence = 0;
std::mutex g_hook_scan_mutex;
std::string g_last_create_hook_scan;
std::string g_last_loader_hook_scan;
std::string g_last_hdr_environment_hook_scan;
std::atomic_uint32_t g_hdr_environment_probe_call_count = 0;
std::atomic_bool g_hdr_environment_probe_suppressed_logged = false;
std::atomic_bool g_hdr_output_desc_probe_install_started = false;
std::atomic_uint32_t g_hdr_output_desc_probe_call_count = 0;
std::atomic_bool g_hdr_output_desc_probe_suppressed_logged = false;
std::mutex g_dispatch_signature_mutex;
std::unordered_map<std::string, std::uint32_t> g_dispatch_signature_counts;
ULONGLONG g_last_interesting_dispatch_tick = 0;
std::uint32_t g_dispatch_phase = 0;
std::mutex g_shader_info_mutex;
std::unordered_map<std::uint64_t, ShaderInfo> g_compute_shader_info;
std::unordered_map<std::uint64_t, ShaderInfo> g_vertex_shader_info;
std::unordered_map<std::uint64_t, ShaderInfo> g_pixel_shader_info;
std::unordered_map<std::uint64_t, ShaderInfo> g_compute_shader_info_by_hash;
std::mutex g_buffer_info_mutex;
std::unordered_map<std::uint64_t, BufferInfo> g_buffer_info;
struct MappedBufferInfo
{
    void *data = nullptr;
    std::uint32_t size = 0;
};
std::unordered_map<std::uint64_t, MappedBufferInfo> g_mapped_buffers;
std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> g_buffer_snapshots;
std::mutex g_similarity_mutex;
SimilarityStats g_similarity;
std::unordered_map<std::string, SimilarityStats> g_similarity_archives;
std::mutex g_osd_mutex;
std::wstring g_osd_text = L"Dx11FsrBridge\n正在初始化";
std::atomic_bool g_osd_running { false };
HWND g_osd_window = nullptr;
HANDLE g_osd_thread = nullptr;
std::mutex g_mode_mutex;
std::vector<std::string> g_recent_mode_features;
std::unordered_map<int, std::vector<std::string>> g_mode_samples;
std::wstring g_mode_status = L"未校准";
int g_recording_mode = 0;

using create_device_and_swapchain_fn = HRESULT(WINAPI *)(
    IDXGIAdapter *,
    D3D_DRIVER_TYPE,
    HMODULE,
    UINT,
    const D3D_FEATURE_LEVEL *,
    UINT,
    UINT,
    const DXGI_SWAP_CHAIN_DESC *,
    IDXGISwapChain **,
    ID3D11Device **,
    D3D_FEATURE_LEVEL *,
    ID3D11DeviceContext **);
using create_device_fn = HRESULT(WINAPI *)(
    IDXGIAdapter *,
    D3D_DRIVER_TYPE,
    HMODULE,
    UINT,
    const D3D_FEATURE_LEVEL *,
    UINT,
    UINT,
    ID3D11Device **,
    D3D_FEATURE_LEVEL *,
    ID3D11DeviceContext **);
using load_library_a_fn = HMODULE(WINAPI *)(LPCSTR);
using load_library_w_fn = HMODULE(WINAPI *)(LPCWSTR);
using load_library_ex_a_fn = HMODULE(WINAPI *)(LPCSTR, HANDLE, DWORD);
using load_library_ex_w_fn = HMODULE(WINAPI *)(LPCWSTR, HANDLE, DWORD);
using get_proc_address_fn = FARPROC(WINAPI *)(HMODULE, LPCSTR);
using display_config_get_device_info_fn = LONG(WINAPI *)(DISPLAYCONFIG_DEVICE_INFO_HEADER *);
using display_config_set_device_info_fn = LONG(WINAPI *)(DISPLAYCONFIG_DEVICE_INFO_HEADER *);
using output_get_desc1_fn = HRESULT(STDMETHODCALLTYPE *)(IDXGIOutput6 *, DXGI_OUTPUT_DESC1 *);
using factory_create_swap_chain_fn = HRESULT(STDMETHODCALLTYPE *)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
using factory2_create_swap_chain_for_hwnd_fn = HRESULT(STDMETHODCALLTYPE *)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);
using create_buffer_fn = HRESULT(STDMETHODCALLTYPE *)(ID3D11Device *, const D3D11_BUFFER_DESC *, const D3D11_SUBRESOURCE_DATA *, ID3D11Buffer **);
using create_texture_2d_fn = HRESULT(STDMETHODCALLTYPE *)(ID3D11Device *, const D3D11_TEXTURE2D_DESC *, const D3D11_SUBRESOURCE_DATA *, ID3D11Texture2D **);
using create_vertex_shader_fn = HRESULT(STDMETHODCALLTYPE *)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11VertexShader **);
using create_pixel_shader_fn = HRESULT(STDMETHODCALLTYPE *)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11PixelShader **);
using create_compute_shader_fn = HRESULT(STDMETHODCALLTYPE *)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11ComputeShader **);

using present_fn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT);
using vs_set_constant_buffers_fn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, ID3D11Buffer *const *);
using vs_set_shader_fn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11VertexShader *, ID3D11ClassInstance *const *, UINT);
using ps_set_shader_resources_fn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, ID3D11ShaderResourceView *const *);
using ps_set_shader_fn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11PixelShader *, ID3D11ClassInstance *const *, UINT);
using ps_set_constant_buffers_fn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, ID3D11Buffer *const *);
using cs_set_shader_resources_fn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, ID3D11ShaderResourceView *const *);
using cs_set_uavs_fn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, ID3D11UnorderedAccessView *const *, const UINT *);
using cs_set_shader_fn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11ComputeShader *, ID3D11ClassInstance *const *, UINT);
using cs_set_constant_buffers_fn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, ID3D11Buffer *const *);
using om_set_render_targets_fn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *, ID3D11DepthStencilView *);
using om_set_render_targets_and_uavs_fn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT,
    ID3D11RenderTargetView *const *, ID3D11DepthStencilView *, UINT, UINT,
    ID3D11UnorderedAccessView *const *, const UINT *);
using dispatch_fn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, UINT);
using draw_indexed_fn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, INT);
using draw_fn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT);
using map_fn = HRESULT(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE *);
using unmap_fn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT);
using rs_set_viewports_fn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, const D3D11_VIEWPORT *);
using copy_resource_fn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, ID3D11Resource *);
using copy_subresource_region_fn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT, UINT, UINT, UINT, ID3D11Resource *, UINT, const D3D11_BOX *);
using update_subresource_fn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT, const D3D11_BOX *, const void *, UINT, UINT);
using clear_rtv_fn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11RenderTargetView *, const FLOAT[4]);
using clear_dsv_fn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11DepthStencilView *, UINT, FLOAT, UINT8);
using set_fullscreen_state_fn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, BOOL, IDXGIOutput *);
using get_fullscreen_state_fn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, BOOL *, IDXGIOutput **);
using resize_buffers_fn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using resize_target_fn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, const DXGI_MODE_DESC *);
using check_color_space_support_fn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain3 *, DXGI_COLOR_SPACE_TYPE, UINT *);
using set_color_space1_fn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain3 *, DXGI_COLOR_SPACE_TYPE);
using set_hdr_metadata_fn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain4 *, DXGI_HDR_METADATA_TYPE, UINT, void *);

create_device_and_swapchain_fn g_original_create_device_and_swapchain = nullptr;
create_device_fn g_original_create_device = nullptr;
load_library_a_fn g_original_load_library_a = nullptr;
load_library_w_fn g_original_load_library_w = nullptr;
load_library_ex_a_fn g_original_load_library_ex_a = nullptr;
load_library_ex_w_fn g_original_load_library_ex_w = nullptr;
get_proc_address_fn g_original_get_proc_address = nullptr;
display_config_get_device_info_fn g_original_display_config_get_device_info = nullptr;
display_config_set_device_info_fn g_original_display_config_set_device_info = nullptr;
output_get_desc1_fn g_original_output_get_desc1 = nullptr;
factory_create_swap_chain_fn g_original_factory_create_swap_chain = nullptr;
factory2_create_swap_chain_for_hwnd_fn g_original_factory2_create_swap_chain_for_hwnd = nullptr;
create_buffer_fn g_original_create_buffer = nullptr;
create_texture_2d_fn g_original_create_texture_2d = nullptr;
create_vertex_shader_fn g_original_create_vertex_shader = nullptr;
create_pixel_shader_fn g_original_create_pixel_shader = nullptr;
create_compute_shader_fn g_original_create_compute_shader = nullptr;
present_fn g_original_present = nullptr;
std::mutex g_swapchain_present_mutex;
std::unordered_map<void *, present_fn> g_original_present_by_instance;
set_fullscreen_state_fn g_original_set_fullscreen_state = nullptr;
get_fullscreen_state_fn g_original_get_fullscreen_state = nullptr;
resize_buffers_fn g_original_resize_buffers = nullptr;
resize_target_fn g_original_resize_target = nullptr;
check_color_space_support_fn g_original_check_color_space_support = nullptr;
set_color_space1_fn g_original_set_color_space1 = nullptr;
set_hdr_metadata_fn g_original_set_hdr_metadata = nullptr;
std::mutex g_swapchain_resize_mutex;
std::unordered_map<void *, resize_buffers_fn> g_original_resize_buffers_by_instance;
#if defined(DX11FSRBRIDGE_COLOR_DIAGNOSTICS)
std::mutex g_fsr2_color_diagnostics_mutex;
std::string g_last_fsr2_color_diagnostics;
#endif
vs_set_constant_buffers_fn g_original_vs_set_constant_buffers = nullptr;
vs_set_shader_fn g_original_vs_set_shader = nullptr;
ps_set_shader_resources_fn g_original_ps_set_shader_resources = nullptr;
ps_set_shader_fn g_original_ps_set_shader = nullptr;
ps_set_constant_buffers_fn g_original_ps_set_constant_buffers = nullptr;
cs_set_shader_resources_fn g_original_cs_set_shader_resources = nullptr;
cs_set_uavs_fn g_original_cs_set_uavs = nullptr;
cs_set_shader_fn g_original_cs_set_shader = nullptr;
cs_set_constant_buffers_fn g_original_cs_set_constant_buffers = nullptr;
om_set_render_targets_fn g_original_om_set_render_targets = nullptr;
om_set_render_targets_and_uavs_fn g_original_om_set_render_targets_and_uavs = nullptr;
dispatch_fn g_original_dispatch = nullptr;
draw_indexed_fn g_original_draw_indexed = nullptr;
draw_fn g_original_draw = nullptr;
map_fn g_original_map = nullptr;
unmap_fn g_original_unmap = nullptr;
rs_set_viewports_fn g_original_rs_set_viewports = nullptr;
copy_resource_fn g_original_copy_resource = nullptr;
copy_subresource_region_fn g_original_copy_subresource_region = nullptr;
update_subresource_fn g_original_update_subresource = nullptr;
clear_rtv_fn g_original_clear_rtv = nullptr;
clear_dsv_fn g_original_clear_dsv = nullptr;

std::mutex g_vtable_mutex;
std::unordered_map<void *, void **> g_cloned_vtables;
std::unordered_map<void *, void **> g_original_vtables;
std::mutex g_context_device_hook_mutex;
std::unordered_set<std::uint64_t> g_context_device_hook_attempts;
std::atomic_uint32_t g_native_ldr_final_target_candidate_count { 0 };
std::atomic_uint32_t g_native_ldr_texture_create_observed_count { 0 };

HRESULT WINAPI hooked_create_device_and_swapchain(
    IDXGIAdapter *adapter,
    D3D_DRIVER_TYPE driver_type,
    HMODULE software,
    UINT flags,
    const D3D_FEATURE_LEVEL *feature_levels,
    UINT feature_levels_count,
    UINT sdk_version,
    const DXGI_SWAP_CHAIN_DESC *swapchain_desc,
    IDXGISwapChain **swapchain,
    ID3D11Device **device,
    D3D_FEATURE_LEVEL *feature_level,
    ID3D11DeviceContext **context);

HRESULT WINAPI hooked_create_device(
    IDXGIAdapter *adapter,
    D3D_DRIVER_TYPE driver_type,
    HMODULE software,
    UINT flags,
    const D3D_FEATURE_LEVEL *feature_levels,
    UINT feature_levels_count,
    UINT sdk_version,
    ID3D11Device **device,
    D3D_FEATURE_LEVEL *feature_level,
    ID3D11DeviceContext **context);

HMODULE WINAPI hooked_load_library_a(LPCSTR file_name);
HMODULE WINAPI hooked_load_library_w(LPCWSTR file_name);
HMODULE WINAPI hooked_load_library_ex_a(LPCSTR file_name, HANDLE file, DWORD flags);
HMODULE WINAPI hooked_load_library_ex_w(LPCWSTR file_name, HANDLE file, DWORD flags);
FARPROC WINAPI hooked_get_proc_address(HMODULE module, LPCSTR proc_name);
LONG WINAPI hooked_display_config_get_device_info(DISPLAYCONFIG_DEVICE_INFO_HEADER *request);
LONG WINAPI hooked_display_config_set_device_info(DISPLAYCONFIG_DEVICE_INFO_HEADER *request);
HRESULT STDMETHODCALLTYPE hooked_output_get_desc1(IDXGIOutput6 *output, DXGI_OUTPUT_DESC1 *desc);
HRESULT STDMETHODCALLTYPE hooked_factory_create_swap_chain(IDXGIFactory *factory, IUnknown *device, DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **swapchain);
HRESULT STDMETHODCALLTYPE hooked_factory2_create_swap_chain_for_hwnd(IDXGIFactory2 *factory, IUnknown *device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1 *desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen_desc, IDXGIOutput *restrict_to_output, IDXGISwapChain1 **swapchain);
void apply_hdr_swapchain_force(IDXGISwapChain *swapchain);
HRESULT STDMETHODCALLTYPE hooked_create_buffer(ID3D11Device *device, const D3D11_BUFFER_DESC *desc, const D3D11_SUBRESOURCE_DATA *initial_data, ID3D11Buffer **buffer);
HRESULT STDMETHODCALLTYPE hooked_create_texture_2d(ID3D11Device *device, const D3D11_TEXTURE2D_DESC *desc, const D3D11_SUBRESOURCE_DATA *initial_data, ID3D11Texture2D **texture);
HRESULT STDMETHODCALLTYPE hooked_create_vertex_shader(ID3D11Device *device, const void *shader_bytecode, SIZE_T bytecode_length, ID3D11ClassLinkage *class_linkage, ID3D11VertexShader **vertex_shader);
HRESULT STDMETHODCALLTYPE hooked_create_pixel_shader(ID3D11Device *device, const void *shader_bytecode, SIZE_T bytecode_length, ID3D11ClassLinkage *class_linkage, ID3D11PixelShader **pixel_shader);
HRESULT STDMETHODCALLTYPE hooked_create_compute_shader(ID3D11Device *device, const void *shader_bytecode, SIZE_T bytecode_length, ID3D11ClassLinkage *class_linkage, ID3D11ComputeShader **compute_shader);
void install_device_hooks(ID3D11Device *device);
void log_line(const std::string &line);
void set_osd_text(const std::wstring &text);
void start_osd();
void update_osd_from_dispatch(std::uint32_t phase, UINT group_x, UINT group_y, UINT group_z);
bool dlssg_framegen_selected();
bool dlssg_dxgi_workaround_active();
void update_swapchain_backbuffer_resources(IDXGISwapChain *swapchain);
void record_hdr_composite_candidate(UINT element_count, bool indexed);
void record_hdr_composite_target_bind(const ResourceInfo &target);

std::wstring current_process_name()
{
    wchar_t buffer[MAX_PATH] {};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(buffer).filename().wstring();
}

std::string narrow(const std::wstring &value)
{
    if (value.empty())
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::string format_string(DXGI_FORMAT format)
{
    std::ostringstream out;
    out << static_cast<std::uint32_t>(format);
    return out.str();
}

std::string hex64(std::uint64_t value)
{
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << value;
    return out.str();
}

// 只读诊断：在尝试安装任何 IL2CPP 相机 hook 前，记录目标函数的实际机器码。
// 这避免把历史版本的 RVA/序言假设直接带入运行中的渲染线程。
static void append_code_bytes(std::string &out, std::uint64_t address, std::size_t count)
{
    out += " bytes=";
    const auto *p = reinterpret_cast<const std::uint8_t *>(address);
    MEMORY_BASIC_INFORMATION mbi {};
    if (address == 0 || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        mbi.Protect == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD) != 0)
    {
        out += "unreadable";
        return;
    }
    static constexpr char k_hex[] = "0123456789ABCDEF";
    for (std::size_t i = 0; i < count; ++i)
    {
        const std::uint8_t value = p[i];
        out += k_hex[value >> 4];
        out += k_hex[value & 0x0F];
    }
}

// 诊断：staging 读回纹理，取 5 个采样点（四角+中心）或 9 点 3×3 网格（dense），
// 按格式解码追加到日志字符串。report_mvmax 时额外输出 R/G 通道 max|sgn|（motion 幅度诊断）。
// 仅诊断用（Ffx12Probe=1 时调用）；不做任何绑定/状态修改。
static void append_tex_samples(std::string &out, const char *tag,
                               ID3D11Texture2D *tex, ID3D11DeviceContext *ctx,
                               bool dense = false, bool report_mvmax = false)
{
    if (!tex || !ctx)
        return;
    // On12 shared-queue renderer（游戏 D3D11 设备是 D3D11On12）：下面的 staging
    // 读回（CreateTexture2D → CopySubresourceRegion → Flush → Map(READ)）是在游戏
    // draw 内发起的 CPU 同步等待，落在共享队列上可能永远不返回 → 游戏在进入渲染
    // 场景前整体无响应（日志精确断在 ffx12_pipeline 之后、
    // ffx12_backend_samples 之前）。采样仅为诊断，On12 路径一律跳过；
    // native-D3D11 CPU-bridge 路径（dx11on12=0）保持原行为。
    {
        bool dx11on12 = false, gpu_only = false;
        ffx12::interop_capabilities(dx11on12, gpu_only);
        if (dx11on12)
            return;
    }
    D3D11_TEXTURE2D_DESC td {};
    tex->GetDesc(&td);
    ID3D11Device *dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev)
        return;
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.BindFlags = 0;
    sd.MiscFlags = 0;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    ID3D11Texture2D *staging = nullptr;
    const HRESULT hr = dev->CreateTexture2D(&sd, nullptr, &staging);
    dev->Release();
    if (FAILED(hr) || !staging)
        return;
    ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, 0, nullptr);
    ctx->Flush();
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData)
    {
        staging->Release();
        return;
    }
    const std::size_t bpp = (td.Format == DXGI_FORMAT_R32G8X24_TYPELESS) ? 8u : 4u;
    const std::uint32_t mx = td.Width > 0 ? td.Width - 1 : 0u;
    const std::uint32_t my = td.Height > 0 ? td.Height - 1 : 0u;
    std::uint32_t pts[9][2] = {};
    int npts = 0;
    if (dense)
    {
        const std::uint32_t xs[3] = {0u, td.Width / 2, mx};
        const std::uint32_t ys[3] = {0u, td.Height / 2, my};
        for (int yy = 0; yy < 3; ++yy)
            for (int xx = 0; xx < 3; ++xx)
            {
                pts[npts][0] = xs[xx];
                pts[npts][1] = ys[yy];
                ++npts;
            }
    }
    else
    {
        pts[0][0] = 0u;            pts[0][1] = 0u;
        pts[1][0] = mx;            pts[1][1] = 0u;
        pts[2][0] = td.Width / 2;  pts[2][1] = td.Height / 2;
        pts[3][0] = 0u;            pts[3][1] = my;
        pts[4][0] = mx;            pts[4][1] = my;
        npts = 5;
    }
    out += ' ';
    out += tag;
    out += '=';
    float mvmax = 0.0f;
    for (int i = 0; i < npts; ++i)
    {
        const std::uint32_t x = pts[i][0];
        const std::uint32_t y = pts[i][1];
        const std::uint8_t *pixel =
            static_cast<const std::uint8_t *>(mapped.pData) +
            static_cast<std::size_t>(y) * mapped.RowPitch + static_cast<std::size_t>(x) * bpp;
        std::uint32_t raw = 0;
        std::memcpy(&raw, pixel, 4);
        out += "(" + std::to_string(x) + "," + std::to_string(y) + ")=";
        if (td.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS ||
            td.Format == DXGI_FORMAT_R10G10B10A2_UNORM)
        {
            const float r = static_cast<float>(raw & 0x3FFu) / 1023.0f;
            const float g = static_cast<float>((raw >> 10) & 0x3FFu) / 1023.0f;
            const float b = static_cast<float>((raw >> 20) & 0x3FFu) / 1023.0f;
            const float sr = r * 2.0f - 1.0f;
            const float sg = g * 2.0f - 1.0f;
            if (report_mvmax)
            {
                const float a = sr * sr + sg * sg;
                if (a > mvmax)
                    mvmax = a;
            }
            out += "0x" + hex64(raw) + " r=" + std::to_string(r) + " g=" + std::to_string(g) +
                " b=" + std::to_string(b) + " sgn=" + std::to_string(sr) + "," +
                std::to_string(sg) + "," + std::to_string(b * 2.0f - 1.0f);
        }
        else if (td.Format == DXGI_FORMAT_R32G8X24_TYPELESS || td.Format == DXGI_FORMAT_R32_FLOAT)
        {
            float d = 0.0f;
            std::memcpy(&d, pixel, 4);
            out += "d=" + std::to_string(d);
        }
        else
        {
            out += "0x" + hex64(raw) + " fmt=" + std::to_string(static_cast<std::uint32_t>(td.Format));
        }
    }
    if (report_mvmax)
        out += " mvmax=" + std::to_string(std::sqrt(mvmax));
    ctx->Unmap(staging, 0);
    staging->Release();
}

// 诊断：把地址处内存按 float4 追加到日志（带 VirtualQuery 保护检查）。
static void append_mem_dump(std::string &out, const char *tag, std::uint64_t addr, int f4_count)
{
    out += ' ';
    out += tag;
    out += '=';
    if (addr == 0)
    {
        out += "null";
        return;
    }
    const std::uint8_t *base = reinterpret_cast<const std::uint8_t *>(addr);
    MEMORY_BASIC_INFORMATION mbi {};
    if (!VirtualQuery(base, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || mbi.Protect == PAGE_NOACCESS ||
        (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                        PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) == 0)
    {
        out += "unreadable";
        return;
    }
    const std::uintptr_t region_end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    const std::size_t avail = std::min<std::size_t>(region_end - static_cast<std::uintptr_t>(addr),
                                                    static_cast<std::size_t>(f4_count) * 16u);
    const int n = static_cast<int>(avail / 16u);
    for (int i = 0; i < n; ++i)
    {
        if (i > 0)
            out += ";";
        const float *f = reinterpret_cast<const float *>(base + i * 16);
        out += "f4[" + std::to_string(i) + "]=" + std::to_string(f[0]) + "," + std::to_string(f[1]) + "," +
               std::to_string(f[2]) + "," + std::to_string(f[3]);
    }
}

// 诊断：把地址处内存按 u64 指针追加（找 il2cpp 对象引用/非零指针字段）。
static void append_mem_u64(std::string &out, const char *tag, std::uint64_t addr, int qword_count)
{
    out += ' ';
    out += tag;
    out += '=';
    if (addr == 0)
    {
        out += "null";
        return;
    }
    const std::uint8_t *base = reinterpret_cast<const std::uint8_t *>(addr);
    MEMORY_BASIC_INFORMATION mbi {};
    if (!VirtualQuery(base, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || mbi.Protect == PAGE_NOACCESS ||
        (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                        PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) == 0)
    {
        out += "unreadable";
        return;
    }
    const std::uintptr_t region_end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    const std::size_t avail = std::min<std::size_t>(region_end - static_cast<std::uintptr_t>(addr),
                                                    static_cast<std::size_t>(qword_count) * 8u);
    const int n = static_cast<int>(avail / 8u);
    for (int i = 0; i < n; ++i)
    {
        std::uint64_t v = 0;
        std::memcpy(&v, base + i * 8, 8);
        if (v == 0)
            continue;
        if (out.size() > 0 && out.back() != '=' && out.back() != ' ')
            out += ",";
        out += std::to_string(i) + "=0x" + hex64(v);
    }
}

// sdk234 输出防覆盖：sdk234 dispatch 写过的输出指针（try_fsr2_translation_draw 内共享，
// 同输出后续 draw 跳过，防止转译层/原生覆盖 sdk234 的结果）。
static std::uint64_t g_sdk234_output_ptr = 0;
// sdk234 上次写输出的时间（cover-skip 过期保护：超过 300ms 未写则不再跳过同输出 draw，
// 防"切视图后 sdk234 停派发 → 同输出 draw 被永久跳过 → 视图冻结"——2026-08-25 日志实证
// cover_skip count 飙到 6144 且输出不再更新）。
static std::atomic_uint64_t g_sdk234_output_tick { 0 };

#if defined(DX11FSRBRIDGE_FG_DXGI_DIAGNOSTICS)
std::string hex32(std::uint32_t value)
{
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << value;
    return out.str();
}
#endif

std::wstring widen_ascii(std::string_view value)
{
    return std::wstring(value.begin(), value.end());
}

std::wstring mode_name_for_phase(std::uint32_t phase)
{
    if (!g_config.assume_phase_order)
        return L"未校准";

    switch (phase)
    {
    case 1:
        return L"FSR2 开启";
    case 2:
        return L"FSR2 关闭";
    case 3:
        return L"SMAA 开启";
    default:
        return L"加载/未知";
    }
}

std::wstring calibrated_mode_name(int mode)
{
    switch (mode)
    {
    case 1:
        return L"FSR2 开启";
    case 2:
        return L"FSR2 关闭";
    case 3:
        return L"SMAA 开启";
    default:
        return L"未确定";
    }
}

std::string mode_label_ascii(int mode)
{
    switch (mode)
    {
    case 1:
        return "FSR_ON";
    case 2:
        return "FSR_OFF";
    case 3:
        return "SMAA";
    default:
        return "UNKNOWN";
    }
}

std::filesystem::path similarity_path_for_label(const std::string &label);
void write_similarity_report_to_path_locked(const std::filesystem::path &path, const SimilarityStats &stats, const std::string &label);
void write_similarity_diff_locked();
void reset_similarity_locked();

void add_unique_feature(std::vector<std::string> &target, const std::string &feature)
{
    if (feature.empty())
        return;
    for (const std::string &existing : target)
    {
        if (existing == feature)
            return;
    }
    target.push_back(feature);
}

void add_limited_recent_feature(const std::string &feature)
{
    if (feature.empty())
        return;
    g_recent_mode_features.push_back(feature);
    constexpr std::size_t k_max_recent_features = 256;
    if (g_recent_mode_features.size() > k_max_recent_features)
        g_recent_mode_features.erase(g_recent_mode_features.begin(), g_recent_mode_features.begin() + (g_recent_mode_features.size() - k_max_recent_features));
}

std::vector<std::string> build_mode_features_from_state(UINT group_x, UINT group_y, UINT group_z)
{
    std::vector<std::string> features;
    std::uint64_t shader_hash = 0;
    std::size_t shader_size = 0;
    std::array<ResourceInfo, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> srvs {};
    std::array<ResourceInfo, D3D11_1_UAV_SLOT_COUNT> uavs {};
    std::array<BufferInfo, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> cbs {};
    {
        std::lock_guard lock(g_state_mutex);
        shader_hash = g_state.current_cs_hash;
        shader_size = g_state.current_cs_size;
        srvs = g_state.cs_srvs;
        uavs = g_state.cs_uavs;
        cbs = g_state.cs_cbs;
    }

    if (shader_hash == 0)
        return features;

    const std::string cs = hex64(shader_hash);
    add_unique_feature(features, "cs:" + cs);
    add_unique_feature(features, "cs_size:" + cs + ":" + std::to_string(shader_size));
    add_unique_feature(features, "groups:" + cs + ":" + std::to_string(group_x) + "x" + std::to_string(group_y) + "x" + std::to_string(group_z));

    for (const BufferInfo &cb : cbs)
    {
        if (cb.resource_key == 0 || cb.byte_width == 0)
            continue;
        add_unique_feature(features, "cb_size:" + cs + ":" + std::to_string(cb.byte_width));
        if (cb.last_update_hash != 0)
            add_unique_feature(features, "cb_hash:" + cs + ":" + std::to_string(cb.byte_width) + ":" + hex64(cb.last_update_hash));
    }

    for (const ResourceInfo &srv : srvs)
    {
        if (srv.width == 0 || srv.height == 0)
            continue;
        add_unique_feature(features, "srv_fmt:" + cs + ":" + std::to_string(static_cast<std::uint32_t>(srv.format)));
    }

    for (const ResourceInfo &uav : uavs)
    {
        if (uav.width == 0 || uav.height == 0)
            continue;
        add_unique_feature(features, "uav_fmt:" + cs + ":" + std::to_string(static_cast<std::uint32_t>(uav.format)));
    }

    return features;
}

void append_features_to_mode_sample_locked(int mode, const std::vector<std::string> &features)
{
    if (mode == 0 || features.empty())
        return;

    std::vector<std::string> &sample = g_mode_samples[mode];
    for (const std::string &feature : features)
        add_unique_feature(sample, feature);
}

void toggle_recording_mode(int mode)
{
    std::wstring status;
    std::size_t sample_size = 0;
    bool started = false;
    bool stopped = false;
    bool switched = false;
    std::string archive_label = mode_label_ascii(mode);
    const std::string label = mode_label_ascii(mode);
    {
        std::lock_guard lock(g_mode_mutex);
        if (g_recording_mode == mode)
        {
            g_recording_mode = 0;
            sample_size = g_mode_samples[mode].size();
            g_mode_status = L"已停止记录 " + calibrated_mode_name(mode) + L" 样本: " + std::to_wstring(sample_size);
            stopped = true;
        }
        else
        {
            if (g_recording_mode != 0)
            {
                archive_label = mode_label_ascii(g_recording_mode);
                stopped = true;
                switched = true;
            }
            g_recording_mode = mode;
            append_features_to_mode_sample_locked(mode, g_recent_mode_features);
            sample_size = g_mode_samples[mode].size();
            started = true;
            g_mode_status = L"正在记录 " + calibrated_mode_name(mode) + L" 样本: " + std::to_wstring(sample_size);
        }
        status = g_mode_status;
    }

    if (started && g_config.reset_similarity_on_recording)
    {
        std::lock_guard lock(g_similarity_mutex);
        if (switched)
        {
            g_similarity_archives[archive_label] = g_similarity;
            write_similarity_report_to_path_locked(similarity_path_for_label(archive_label), g_similarity, archive_label);
            write_similarity_diff_locked();
            log_line("similarity_recording_saved label=" + archive_label + " dispatch=" + std::to_string(g_similarity.dispatch_count) +
                " draw=" + std::to_string(g_similarity.draw_count));
        }
        reset_similarity_locked();
        log_line("similarity_recording_reset label=" + label);
    }

    if (stopped && !switched)
    {
        std::lock_guard lock(g_similarity_mutex);
        g_similarity_archives[archive_label] = g_similarity;
        write_similarity_report_to_path_locked(similarity_path_for_label(archive_label), g_similarity, archive_label);
        write_similarity_report_to_path_locked(g_similarity_path, g_similarity, archive_label);
        write_similarity_diff_locked();
        log_line("similarity_recording_saved label=" + archive_label + " dispatch=" + std::to_string(g_similarity.dispatch_count) +
            " draw=" + std::to_string(g_similarity.draw_count));
    }

    log_line(std::string(started ? "mode_recording_started " : "mode_recording_stopped ") +
        "mode=" + narrow(calibrated_mode_name(mode)) + " features=" + std::to_string(sample_size));
    set_osd_text(L"Dx11FsrBridge OSD\n" + status);
}

void clear_mode_samples()
{
    {
        std::lock_guard lock(g_mode_mutex);
        g_mode_samples.clear();
        g_recent_mode_features.clear();
        g_recording_mode = 0;
        g_mode_status = L"已清空样本";
    }
    {
        std::lock_guard lock(g_dispatch_signature_mutex);
        g_dispatch_signature_counts.clear();
        g_last_interesting_dispatch_tick = 0;
        g_dispatch_phase = 0;
    }
    {
        std::lock_guard lock(g_similarity_mutex);
        reset_similarity_locked();
        g_similarity_archives.clear();
    }

    std::ofstream(g_frames_path, std::ios::trunc).close();
    {
        std::lock_guard lock(g_ps_trace_mutex);
        if (g_ps_trace_stream.is_open())
            g_ps_trace_stream.close();
        g_ps_trace_stream.open(g_ps_trace_path, std::ios::trunc);
    }
    g_ps_trace_count = 0;
    std::ofstream(g_similarity_path, std::ios::trunc).close();
    std::ofstream(g_module_dir / L"Dx11FsrBridge.similarity.diff.txt", std::ios::trunc).close();
    std::ofstream(similarity_path_for_label("FSR_ON"), std::ios::trunc).close();
    std::ofstream(similarity_path_for_label("FSR_OFF"), std::ios::trunc).close();
    std::ofstream(similarity_path_for_label("SMAA"), std::ios::trunc).close();

    log_line("mode_calibration_and_similarity_cleared");
    set_osd_text(L"Dx11FsrBridge OSD\n已清空全部记录");
}

void poll_mode_hotkeys()
{
    if (g_config.trace_texture_creates && (GetAsyncKeyState(g_config.texture_trace_hotkey) & 1))
    {
        const ULONGLONG now = GetTickCount64();
        g_texture_trace_count.store(0, std::memory_order_relaxed);
        g_texture_trace_until_tick.store(now + g_config.texture_trace_duration_ms, std::memory_order_relaxed);
        log_line("texture_trace_started duration_ms=" + std::to_string(g_config.texture_trace_duration_ms) +
            " main_base=" + hex64(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))));
    }
    if (GetAsyncKeyState(VK_F10) & 1)
    {
        clear_mode_samples();
        return;
    }
    if (GetAsyncKeyState(VK_F7) & 1)
        toggle_recording_mode(1);
    if (GetAsyncKeyState(VK_F8) & 1)
        toggle_recording_mode(2);
    if (GetAsyncKeyState(VK_F9) & 1)
        toggle_recording_mode(3);
}

ModeMatch classify_current_mode()
{
    std::lock_guard lock(g_mode_mutex);
    ModeMatch best {};
    if (g_recent_mode_features.empty() || g_mode_samples.empty())
    {
        best.name = L"未校准";
        best.total = static_cast<std::uint32_t>(g_recent_mode_features.size());
        return best;
    }

    std::unordered_set<std::string> recent(g_recent_mode_features.begin(), g_recent_mode_features.end());
    best.total = static_cast<std::uint32_t>(recent.size());
    std::uint32_t second_score = 0;
    int best_mode = 0;

    for (const auto &[mode, sample] : g_mode_samples)
    {
        std::uint32_t score = 0;
        for (const std::string &feature : sample)
        {
            if (recent.contains(feature))
                ++score;
        }
        if (score > best.score)
        {
            second_score = best.score;
            best.score = score;
            best_mode = mode;
        }
        else if (score > second_score)
        {
            second_score = score;
        }
    }

    if (best.score < 4 || best.score <= second_score + 1)
    {
        best.name = L"未确定";
        return best;
    }

    best.name = calibrated_mode_name(best_mode);
    return best;
}

std::uint64_t fnv1a64(const void *data, std::size_t size)
{
    constexpr std::uint64_t k_offset = 14695981039346656037ull;
    constexpr std::uint64_t k_prime = 1099511628211ull;
    std::uint64_t hash = k_offset;
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    for (std::size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= k_prime;
    }
    return hash;
}

ShaderInfo lookup_compute_shader_info(ID3D11ComputeShader *shader)
{
    if (shader == nullptr)
        return {};

    std::lock_guard lock(g_shader_info_mutex);
    const auto it = g_compute_shader_info.find(reinterpret_cast<std::uint64_t>(shader));
    if (it == g_compute_shader_info.end())
        return {};
    return it->second;
}

ShaderInfo lookup_compute_shader_info_by_hash(std::uint64_t hash)
{
    if (hash == 0)
        return {};
    std::lock_guard lock(g_shader_info_mutex);
    const auto it = g_compute_shader_info_by_hash.find(hash);
    if (it == g_compute_shader_info_by_hash.end())
        return {};
    return it->second;
}

ShaderInfo lookup_vertex_shader_info(ID3D11VertexShader *shader)
{
    if (shader == nullptr)
        return {};

    std::lock_guard lock(g_shader_info_mutex);
    const auto it = g_vertex_shader_info.find(reinterpret_cast<std::uint64_t>(shader));
    if (it == g_vertex_shader_info.end())
        return {};
    return it->second;
}

ShaderInfo lookup_pixel_shader_info(ID3D11PixelShader *shader)
{
    if (shader == nullptr)
        return {};

    std::lock_guard lock(g_shader_info_mutex);
    const auto it = g_pixel_shader_info.find(reinterpret_cast<std::uint64_t>(shader));
    if (it == g_pixel_shader_info.end())
        return {};
    return it->second;
}

ShaderInfo reflect_compute_shader(std::uint64_t hash, const void *shader_bytecode, std::size_t bytecode_length)
{
    ShaderInfo info {};
    info.hash = hash;
    info.bytecode_size = bytecode_length;
    if (shader_bytecode == nullptr || bytecode_length == 0)
        return info;

    ID3D11ShaderReflection *reflection = nullptr;
    if (FAILED(D3DReflect(shader_bytecode, bytecode_length, __uuidof(ID3D11ShaderReflection), reinterpret_cast<void **>(&reflection))) || reflection == nullptr)
        return info;

    D3D11_SHADER_DESC desc {};
    if (SUCCEEDED(reflection->GetDesc(&desc)))
    {
        info.reflected = true;
        reflection->GetThreadGroupSize(&info.thread_x, &info.thread_y, &info.thread_z);
        for (UINT i = 0; i < desc.BoundResources; ++i)
        {
            D3D11_SHADER_INPUT_BIND_DESC bind {};
            if (FAILED(reflection->GetResourceBindingDesc(i, &bind)))
                continue;

            if (bind.Type == D3D_SIT_CBUFFER)
            {
                ++info.cb_count;
                continue;
            }
            if (bind.Type == D3D_SIT_SAMPLER)
            {
                ++info.sampler_count;
                continue;
            }

            const bool is_uav = bind.Type == D3D_SIT_UAV_RWTYPED ||
                bind.Type == D3D_SIT_UAV_RWSTRUCTURED ||
                bind.Type == D3D_SIT_UAV_RWBYTEADDRESS ||
                bind.Type == D3D_SIT_UAV_APPEND_STRUCTURED ||
                bind.Type == D3D_SIT_UAV_CONSUME_STRUCTURED ||
                bind.Type == D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER;

            if (bind.Type == D3D_SIT_TEXTURE)
            {
                if (bind.Dimension == D3D_SRV_DIMENSION_TEXTURE2D || bind.Dimension == D3D_SRV_DIMENSION_TEXTURE2DARRAY)
                    ++info.srv_2d_count;
                else if (bind.Dimension == D3D_SRV_DIMENSION_TEXTURE3D)
                    ++info.srv_3d_count;
                else
                    ++info.srv_other_count;
            }
            else if (is_uav)
            {
                if (bind.Dimension == D3D_SRV_DIMENSION_TEXTURE2D || bind.Dimension == D3D_SRV_DIMENSION_TEXTURE2DARRAY)
                    ++info.uav_2d_count;
                else if (bind.Dimension == D3D_SRV_DIMENSION_TEXTURE3D)
                    ++info.uav_3d_count;
                else
                    ++info.uav_other_count;
            }
        }
    }

    reflection->Release();
    return info;
}

BufferInfo lookup_buffer_info(ID3D11Buffer *buffer)
{
    if (buffer == nullptr)
        return {};

    std::lock_guard lock(g_buffer_info_mutex);
    const auto it = g_buffer_info.find(reinterpret_cast<std::uint64_t>(buffer));
    if (it == g_buffer_info.end())
        return {};
    return it->second;
}

std::vector<std::uint8_t> lookup_buffer_snapshot(std::uint64_t resource_key)
{
    if (resource_key == 0)
        return {};
    std::lock_guard lock(g_buffer_info_mutex);
    const auto it = g_buffer_snapshots.find(resource_key);
    if (it == g_buffer_snapshots.end())
        return {};
    return it->second;
}

bool read_buffer_snapshot_bytes(std::uint64_t resource_key, std::size_t offset, void *destination, std::size_t size)
{
    if (resource_key == 0 || destination == nullptr || size == 0)
        return false;
    std::lock_guard lock(g_buffer_info_mutex);
    const auto it = g_buffer_snapshots.find(resource_key);
    if (it == g_buffer_snapshots.end() || it->second.size() < offset + size)
        return false;
    std::memcpy(destination, it->second.data() + offset, size);
    return true;
}

std::string bytes_to_hex(const std::vector<std::uint8_t> &bytes)
{
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    result.resize(bytes.size() * 2);
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        result[i * 2] = digits[bytes[i] >> 4];
        result[i * 2 + 1] = digits[bytes[i] & 0x0F];
    }
    return result;
}

void update_constant_buffer_array(std::array<BufferInfo, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> &target, UINT start_slot, UINT count, ID3D11Buffer *const *buffers)
{
    for (UINT i = 0; i < count && (start_slot + i) < target.size(); ++i)
    {
        BufferInfo info {};
        if (buffers != nullptr)
            info = lookup_buffer_info(buffers[i]);
        info.binding_slot = start_slot + i;
        target[start_slot + i] = info;
    }
}

std::vector<std::uint8_t> readback_buffer_bytes(ID3D11DeviceContext *context, std::uint64_t resource_key)
{
    if (context == nullptr || resource_key == 0)
        return {};

    auto *source = reinterpret_cast<ID3D11Buffer *>(resource_key);
    D3D11_BUFFER_DESC source_desc {};
    source->GetDesc(&source_desc);
    if (source_desc.ByteWidth == 0)
        return {};

    D3D11_BUFFER_DESC staging_desc {};
    staging_desc.ByteWidth = source_desc.ByteWidth;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ID3D11Device *device = nullptr;
    context->GetDevice(&device);
    ID3D11Buffer *staging = nullptr;
    const HRESULT create_result = device != nullptr
        ? device->CreateBuffer(&staging_desc, nullptr, &staging)
        : E_POINTER;
    if (device != nullptr)
        device->Release();
    if (FAILED(create_result) || staging == nullptr)
        return {};

    context->CopyResource(staging, source);
    D3D11_MAPPED_SUBRESOURCE mapped {};
    const HRESULT map_result = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(map_result) || mapped.pData == nullptr)
    {
        staging->Release();
        return {};
    }

    const auto *bytes = static_cast<const std::uint8_t *>(mapped.pData);
    std::vector<std::uint8_t> result(bytes, bytes + source_desc.ByteWidth);
    context->Unmap(staging, 0);
    staging->Release();
    return result;
}

void append_constant_buffer_list(std::ostringstream &out, const char *label, const std::array<BufferInfo, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> &buffers)
{
    bool wrote_any = false;
    std::size_t emitted = 0;
    for (std::size_t i = 0; i < buffers.size() && emitted < 6; ++i)
    {
        const BufferInfo &info = buffers[i];
        if (info.resource_key == 0 || info.byte_width == 0)
            continue;

        if (!wrote_any)
        {
            out << " " << label << "=";
            wrote_any = true;
        }
        else
        {
            out << ",";
        }

        out << i << ":" << info.byte_width
            << "/h" << hex64(info.last_update_hash)
            << "/u" << info.last_update_size;
        ++emitted;
    }
}

void dump_compute_shader_bytecode(std::uint64_t hash, const void *shader_bytecode, std::size_t bytecode_length)
{
    if (!g_config.dump_compute_shaders || hash == 0 || shader_bytecode == nullptr || bytecode_length == 0)
        return;

    try
    {
        const std::filesystem::path dump_dir = g_module_dir / L"Dx11FsrBridge.shaders";
        std::filesystem::create_directories(dump_dir);
        const std::filesystem::path dump_path = dump_dir / (hex64(hash) + ".cso");
        if (std::filesystem::exists(dump_path))
            return;

        std::ofstream out(dump_path, std::ios::binary);
        out.write(static_cast<const char *>(shader_bytecode), static_cast<std::streamsize>(bytecode_length));
    }
    catch (...)
    {
    }
}

void dump_pixel_shader_bytecode(std::uint64_t hash, const void *shader_bytecode, std::size_t bytecode_length)
{
    if (!g_config.dump_pixel_shaders || hash == 0 || shader_bytecode == nullptr || bytecode_length == 0)
        return;

    try
    {
        const std::filesystem::path dump_dir = g_module_dir / L"Dx11FsrBridge.pixel_shaders";
        std::filesystem::create_directories(dump_dir);
        const std::filesystem::path dump_path = dump_dir / (hex64(hash) + ".cso");
        if (std::filesystem::exists(dump_path))
            return;

        std::ofstream out(dump_path, std::ios::binary);
        out.write(static_cast<const char *>(shader_bytecode), static_cast<std::streamsize>(bytecode_length));
    }
    catch (...)
    {
    }
}

void dump_vertex_shader_bytecode(std::uint64_t hash, const void *shader_bytecode, std::size_t bytecode_length)
{
    if (!g_config.dump_pixel_shaders || hash == 0 || shader_bytecode == nullptr || bytecode_length == 0)
        return;

    try
    {
        const std::filesystem::path dump_dir = g_module_dir / L"Dx11FsrBridge.vertex_shaders";
        std::filesystem::create_directories(dump_dir);
        const std::filesystem::path dump_path = dump_dir / (hex64(hash) + ".cso");
        if (std::filesystem::exists(dump_path))
            return;

        std::ofstream out(dump_path, std::ios::binary);
        out.write(static_cast<const char *>(shader_bytecode), static_cast<std::streamsize>(bytecode_length));
    }
    catch (...)
    {
    }
}

bool is_fullres_surface(const ResourceInfo &info, std::uint32_t output_width, std::uint32_t output_height)
{
    if (info.width == 0 || info.height == 0 || output_width == 0 || output_height == 0)
        return false;
    const std::uint32_t width_delta = info.width > output_width ? info.width - output_width : output_width - info.width;
    const std::uint32_t height_delta = info.height > output_height ? info.height - output_height : output_height - info.height;
    return width_delta <= 2 && height_delta <= 2;
}

bool is_lowres_surface(const ResourceInfo &info, std::uint32_t output_width, std::uint32_t output_height)
{
    if (info.width == 0 || info.height == 0 || output_width == 0 || output_height == 0)
        return false;
    if (is_fullres_surface(info, output_width, output_height))
        return false;
    if (info.width > output_width || info.height > output_height)
        return false;
    return info.width >= output_width / 3 && info.height >= output_height / 3;
}

bool is_fullres_viewport(const DispatchState &snapshot)
{
    if (snapshot.viewport_width == 0 || snapshot.viewport_height == 0 ||
        snapshot.backbuffer_width == 0 || snapshot.backbuffer_height == 0)
        return false;

    const std::uint32_t width_delta = snapshot.viewport_width > snapshot.backbuffer_width
        ? snapshot.viewport_width - snapshot.backbuffer_width
        : snapshot.backbuffer_width - snapshot.viewport_width;
    const std::uint32_t height_delta = snapshot.viewport_height > snapshot.backbuffer_height
        ? snapshot.viewport_height - snapshot.backbuffer_height
        : snapshot.backbuffer_height - snapshot.viewport_height;
    return width_delta <= 2 && height_delta <= 2;
}

bool is_depth_like_format(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_R16_UNORM:
        return true;
    default:
        return false;
    }
}

bool is_motion_like_format(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_UNORM:
        return true;
    default:
        return false;
    }
}

std::string resource_signature(const ResourceInfo &info)
{
    std::ostringstream out;
    out << info.width << "x" << info.height
        << "/f" << static_cast<std::uint32_t>(info.format)
        << "/rh" << hex64(info.resource_key);
    return out.str();
}

std::string resource_shape_signature(const ResourceInfo &info)
{
    std::ostringstream out;
    out << info.width << "x" << info.height
        << "/f" << static_cast<std::uint32_t>(info.format);
    return out.str();
}

template <std::size_t Count>
std::string limited_resource_shapes(const std::array<ResourceInfo, Count> &resources, std::size_t limit)
{
    std::ostringstream out;
    std::size_t emitted = 0;
    for (std::size_t i = 0; i < resources.size() && emitted < limit; ++i)
    {
        const ResourceInfo &info = resources[i];
        if (info.resource_key == 0 || info.width == 0 || info.height == 0)
            continue;
        if (emitted != 0)
            out << "|";
        out << i << ":" << resource_shape_signature(info);
        ++emitted;
    }
    if (emitted == 0)
        out << "none";
    return out.str();
}

void increment_string_counter(std::unordered_map<std::string, std::uint64_t> &map, const std::string &key)
{
    if (!key.empty())
        map[key]++;
}

template <typename Map>
std::string top_entries_string(const Map &map, std::size_t limit)
{
    std::vector<std::pair<typename Map::key_type, std::uint64_t>> entries;
    entries.reserve(map.size());
    for (const auto &[key, value] : map)
        entries.push_back({ key, value });
    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
        return a.second > b.second;
    });

    std::ostringstream out;
    for (std::size_t i = 0; i < entries.size() && i < limit; ++i)
    {
        if (i != 0)
            out << ", ";
        if constexpr (std::is_integral_v<typename Map::key_type>)
            out << hex64(static_cast<std::uint64_t>(entries[i].first));
        else
            out << entries[i].first;
        out << "=" << entries[i].second;
    }
    if (entries.empty())
        out << "none";
    return out.str();
}

std::string compute_shader_reflection_string(std::uint64_t hash)
{
    const ShaderInfo info = lookup_compute_shader_info_by_hash(hash);
    if (!info.reflected)
        return "reflect=none";

    std::ostringstream out;
    out << "tg=" << info.thread_x << "x" << info.thread_y << "x" << info.thread_z
        << "/srv2d=" << info.srv_2d_count
        << "/srv3d=" << info.srv_3d_count
        << "/srvO=" << info.srv_other_count
        << "/uav2d=" << info.uav_2d_count
        << "/uav3d=" << info.uav_3d_count
        << "/uavO=" << info.uav_other_count
        << "/cb=" << info.cb_count;
    return out.str();
}

bool is_compute_shader_2d_post_candidate(std::uint64_t hash)
{
    const ShaderInfo info = lookup_compute_shader_info_by_hash(hash);
    if (!info.reflected)
        return false;
    if (info.thread_z != 1)
        return false;
    if (info.srv_3d_count != 0 || info.uav_3d_count != 0)
        return false;
    return info.srv_2d_count >= 1 && info.uav_2d_count >= 1;
}

std::string top_compute_entries_with_reflection(const std::unordered_map<std::uint64_t, std::uint64_t> &map, std::size_t limit)
{
    std::vector<std::pair<std::uint64_t, std::uint64_t>> entries;
    entries.reserve(map.size());
    for (const auto &[key, value] : map)
        entries.push_back({ key, value });
    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
        return a.second > b.second;
    });

    std::ostringstream out;
    for (std::size_t i = 0; i < entries.size() && i < limit; ++i)
    {
        if (i != 0)
            out << ", ";
        out << hex64(entries[i].first) << "=" << entries[i].second
            << "(" << compute_shader_reflection_string(entries[i].first) << ")";
    }
    if (entries.empty())
        out << "none";
    return out.str();
}

void note_resource_read(SimilarityStats &stats, const ResourceInfo &info, const char *reader)
{
    if (info.resource_key == 0)
        return;

    const auto writer_it = stats.resource_last_writer.find(info.resource_key);
    if (writer_it != stats.resource_last_writer.end())
    {
        stats.history_read_after_write_hits++;
        stats.resource_read_after_write_counts[info.resource_key]++;
        if (std::strstr(writer_it->second.c_str(), reader) == nullptr)
            stats.temporal_chain_hits++;
    }
}

void note_resource_write(SimilarityStats &stats, const ResourceInfo &info, const std::string &writer)
{
    if (info.resource_key == 0)
        return;
    stats.resource_last_writer[info.resource_key] = writer;
}

bool is_color_source_trace_surface(
    const ResourceInfo &info,
    std::uint32_t output_width,
    std::uint32_t output_height)
{
    if (info.resource_key == 0 || info.width == 0 || info.height == 0 ||
        output_width == 0 || output_height == 0)
    {
        return false;
    }
    if (info.width > output_width || info.height > output_height ||
        info.width < output_width / 3 || info.height < output_height / 3)
    {
        return false;
    }
    return !is_depth_like_format(info.format);
}

void record_color_source_call(
    const char *stage,
    std::uint32_t call_x,
    std::uint32_t call_y,
    std::uint32_t call_z)
{
    if (!g_config.fsr2_trace_color_producers || stage == nullptr)
        return;

    DispatchState snapshot {};
    {
        std::lock_guard lock(g_state_mutex);
        snapshot = g_state;
    }

    ColorSourceWrite write;
    write.sequence = g_color_source_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    write.stage = stage;
    write.call_x = call_x;
    write.call_y = call_y;
    write.call_z = call_z;
    write.viewport_width = snapshot.viewport_width;
    write.viewport_height = snapshot.viewport_height;

    std::vector<ResourceInfo> outputs;
    if (write.stage == "dispatch")
    {
        write.shader_hash = snapshot.current_cs_hash;
        write.shader_size = snapshot.current_cs_size;
        for (std::size_t slot = 0; slot < snapshot.cs_srvs.size() && slot < 32; ++slot)
        {
            if (snapshot.cs_srvs[slot].resource_key != 0)
                write.inputs.push_back(snapshot.cs_srvs[slot]);
        }
        for (const ResourceInfo &uav : snapshot.cs_uavs)
        {
            if (is_color_source_trace_surface(uav, snapshot.backbuffer_width, snapshot.backbuffer_height))
                outputs.push_back(uav);
        }
        for (const BufferInfo &buffer : snapshot.cs_cbs)
        {
            if (buffer.resource_key != 0)
                write.constant_buffers.push_back(buffer);
        }
    }
    else
    {
        write.shader_hash = snapshot.current_ps_hash;
        write.shader_size = snapshot.current_ps_size;
        write.vertex_shader_hash = snapshot.current_vs_hash;
        write.vertex_shader_size = snapshot.current_vs_size;
        for (std::size_t slot = 0; slot < snapshot.ps_srvs.size() && slot < 32; ++slot)
        {
            if (snapshot.ps_srvs[slot].resource_key != 0)
                write.inputs.push_back(snapshot.ps_srvs[slot]);
        }
        for (const ResourceInfo &rtv : snapshot.rtvs)
        {
            if (is_color_source_trace_surface(rtv, snapshot.backbuffer_width, snapshot.backbuffer_height))
                outputs.push_back(rtv);
        }
        for (const BufferInfo &buffer : snapshot.ps_cbs)
        {
            if (buffer.resource_key != 0)
                write.constant_buffers.push_back(buffer);
        }
        for (const BufferInfo &buffer : snapshot.vs_cbs)
        {
            if (buffer.resource_key != 0)
                write.vertex_constant_buffers.push_back(buffer);
        }
    }

    if (outputs.empty())
        return;

    std::lock_guard lock(g_color_source_mutex);
    for (const ResourceInfo &output : outputs)
    {
        auto &history = g_color_source_writes[output.resource_key];
        history.push_back(write);
        while (history.size() > 1024)
            history.pop_front();
    }
}

void record_color_source_copy(const ResourceInfo &destination, const ResourceInfo &source, const char *stage)
{
    if (!g_config.fsr2_trace_color_producers)
        return;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    {
        std::lock_guard lock(g_state_mutex);
        output_width = g_state.backbuffer_width;
        output_height = g_state.backbuffer_height;
    }
    if (!is_color_source_trace_surface(destination, output_width, output_height))
    {
        return;
    }

    ColorSourceWrite write;
    write.sequence = g_color_source_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    write.stage = stage != nullptr ? stage : "copy";
    if (source.resource_key != 0)
        write.inputs.push_back(source);

    std::lock_guard lock(g_color_source_mutex);
    auto &history = g_color_source_writes[destination.resource_key];
    history.push_back(std::move(write));
    while (history.size() > 1024)
        history.pop_front();
}

void write_color_source_record_json(std::ofstream &out, const ColorSourceWrite &write)
{
    out << "{\"sequence\":" << write.sequence
        << ",\"stage\":\"" << write.stage << "\""
        << ",\"shader\":\"" << hex64(write.shader_hash) << "\""
        << ",\"shader_size\":" << write.shader_size
        << ",\"vertex_shader\":\"" << hex64(write.vertex_shader_hash) << "\""
        << ",\"vertex_shader_size\":" << write.vertex_shader_size
        << ",\"call\":[" << write.call_x << "," << write.call_y << "," << write.call_z << "]"
        << ",\"viewport\":[" << write.viewport_width << "," << write.viewport_height << "]"
        << ",\"inputs\":[";
    for (std::size_t index = 0; index < write.inputs.size(); ++index)
    {
        if (index != 0)
            out << ",";
        const ResourceInfo &input = write.inputs[index];
        out << "{\"resource\":\"" << hex64(input.resource_key) << "\""
            << ",\"width\":" << input.width
            << ",\"height\":" << input.height
            << ",\"format\":" << static_cast<std::uint32_t>(input.format) << "}";
    }
    out << "],\"constant_buffers\":[";
    for (std::size_t index = 0; index < write.constant_buffers.size(); ++index)
    {
        if (index != 0)
            out << ",";
        const BufferInfo &buffer = write.constant_buffers[index];
        out << "{\"slot\":" << buffer.binding_slot
            << ",\"resource\":\"" << hex64(buffer.resource_key) << "\""
            << ",\"bytes\":" << buffer.byte_width
            << ",\"update_hash\":\"" << hex64(buffer.last_update_hash) << "\"}";
    }
    out << "],\"vertex_constant_buffers\":[";
    for (std::size_t index = 0; index < write.vertex_constant_buffers.size(); ++index)
    {
        if (index != 0)
            out << ",";
        const BufferInfo &buffer = write.vertex_constant_buffers[index];
        out << "{\"slot\":" << buffer.binding_slot
            << ",\"resource\":\"" << hex64(buffer.resource_key) << "\""
            << ",\"bytes\":" << buffer.byte_width
            << ",\"update_hash\":\"" << hex64(buffer.last_update_hash) << "\"}";
    }
    out << "]}";
}

void maybe_dump_source_history(
    ID3D11DeviceContext *context,
    std::uint64_t target_resource_key,
    const wchar_t *file_stem,
    const char *log_label,
    std::atomic_bool &dumped)
{
    if (!g_config.fsr2_trace_color_producers || target_resource_key == 0 || file_stem == nullptr ||
        log_label == nullptr ||
        (GetAsyncKeyState(VK_F6) & 0x8000) == 0 ||
        dumped.exchange(true, std::memory_order_relaxed))
    {
        return;
    }

    std::deque<ColorSourceWrite> target_writes;
    std::unordered_map<std::uint64_t, ColorSourceWrite> related_writes;
    {
        std::lock_guard lock(g_color_source_mutex);
        const auto target_it = g_color_source_writes.find(target_resource_key);
        if (target_it != g_color_source_writes.end())
            target_writes = target_it->second;
        for (const ColorSourceWrite &write : target_writes)
        {
            for (const ResourceInfo &input : write.inputs)
            {
                const auto input_it = g_color_source_writes.find(input.resource_key);
                if (input_it != g_color_source_writes.end() && !input_it->second.empty())
                    related_writes[input.resource_key] = input_it->second.back();
            }
        }
    }

    const std::filesystem::path output_path = g_module_dir / (std::wstring(file_stem) + L".json");
    std::ofstream out(output_path, std::ios::trunc);
    if (!out)
    {
        log_line(std::string("fsr2_") + log_label + "_dump_failed open=0");
        return;
    }

    out << "{\"target_resource\":\"" << hex64(target_resource_key) << "\",\"writes\":[";
    for (std::size_t index = 0; index < target_writes.size(); ++index)
    {
        if (index != 0)
            out << ",";
        write_color_source_record_json(out, target_writes[index]);
    }
    out << "],\"related_last_writes\":[";
    std::size_t related_index = 0;
    for (const auto &[resource_key, write] : related_writes)
    {
        if (related_index++ != 0)
            out << ",";
        out << "{\"resource\":\"" << hex64(resource_key) << "\",\"write\":";
        write_color_source_record_json(out, write);
        out << "}";
    }
    out << "],\"last_write_constant_buffers\":[";
    if (!target_writes.empty())
    {
        const ColorSourceWrite &last_write = target_writes.back();
        for (std::size_t index = 0; index < last_write.constant_buffers.size(); ++index)
        {
            if (index != 0)
                out << ",";
            const BufferInfo &buffer = last_write.constant_buffers[index];
            std::vector<std::uint8_t> snapshot = readback_buffer_bytes(context, buffer.resource_key);
            const char *source = "gpu_readback";
            if (snapshot.empty())
            {
                snapshot = lookup_buffer_snapshot(buffer.resource_key);
                source = "cpu_snapshot";
            }
            out << "{\"slot\":" << buffer.binding_slot
                << ",\"resource\":\"" << hex64(buffer.resource_key) << "\""
                << ",\"bytes\":" << buffer.byte_width
                << ",\"source\":\"" << source << "\""
                << ",\"data_hex\":\"" << bytes_to_hex(snapshot) << "\"}";
        }
    }
    out << "],\"last_write_vertex_constant_buffers\":[";
    if (!target_writes.empty())
    {
        const ColorSourceWrite &last_write = target_writes.back();
        for (std::size_t index = 0; index < last_write.vertex_constant_buffers.size(); ++index)
        {
            if (index != 0)
                out << ",";
            const BufferInfo &buffer = last_write.vertex_constant_buffers[index];
            std::vector<std::uint8_t> snapshot = readback_buffer_bytes(context, buffer.resource_key);
            const char *source = "gpu_readback";
            if (snapshot.empty())
            {
                snapshot = lookup_buffer_snapshot(buffer.resource_key);
                source = "cpu_snapshot";
            }
            out << "{\"slot\":" << buffer.binding_slot
                << ",\"resource\":\"" << hex64(buffer.resource_key) << "\""
                << ",\"bytes\":" << buffer.byte_width
                << ",\"source\":\"" << source << "\""
                << ",\"data_hex\":\"" << bytes_to_hex(snapshot) << "\"}";
        }
    }
    out << "]}";
    log_line(std::string("fsr2_") + log_label + "_dumped target=" + hex64(target_resource_key) +
        " writes=" + std::to_string(target_writes.size()) +
        " related=" + std::to_string(related_writes.size()));
}

void maybe_dump_color_source_history(ID3D11DeviceContext *context, std::uint64_t target_resource_key)
{
    maybe_dump_source_history(
        context,
        target_resource_key,
        L"Dx11FsrBridge.color_chain",
        "color_chain",
        g_fsr2_color_producers_dumped);
}

void maybe_dump_motion_source_history(ID3D11DeviceContext *context, std::uint64_t target_resource_key)
{
    maybe_dump_source_history(
        context,
        target_resource_key,
        L"Dx11FsrBridge.motion_chain",
        "motion_chain",
        g_fsr2_motion_producers_dumped);
}

void note_2d_post_context(SimilarityStats &stats, const DispatchState &snapshot, UINT group_x, UINT group_y, UINT group_z)
{
    if (snapshot.current_cs_hash == 0 || !is_compute_shader_2d_post_candidate(snapshot.current_cs_hash))
        return;

    std::ostringstream out;
    out << "cs=" << hex64(snapshot.current_cs_hash)
        << " groups=" << group_x << "x" << group_y << "x" << group_z
        << " out=" << snapshot.backbuffer_width << "x" << snapshot.backbuffer_height
        << " srv=" << limited_resource_shapes(snapshot.cs_srvs, 6)
        << " uav=" << limited_resource_shapes(snapshot.cs_uavs, 4)
        << " rtv=" << limited_resource_shapes(snapshot.rtvs, 4);
    increment_string_counter(stats.cs_2d_post_contexts, out.str());
}

void note_pixel_shader_post_context(SimilarityStats &stats, const DispatchState &snapshot, const char *kind, UINT element_count)
{
    if (snapshot.current_ps_hash == 0 || !is_fullres_viewport(snapshot))
        return;

    bool has_fullres_rtv = false;
    for (const ResourceInfo &rtv : snapshot.rtvs)
    {
        if (is_fullres_surface(rtv, snapshot.backbuffer_width, snapshot.backbuffer_height))
        {
            has_fullres_rtv = true;
            break;
        }
    }
    if (!has_fullres_rtv)
        return;

    bool has_lowres_srv = false;
    for (const ResourceInfo &srv : snapshot.ps_srvs)
    {
        if (is_lowres_surface(srv, snapshot.backbuffer_width, snapshot.backbuffer_height))
        {
            has_lowres_srv = true;
            break;
        }
    }

    if (element_count > 6 && !has_lowres_srv)
        return;

    stats.ps_post_hash_counts[snapshot.current_ps_hash]++;

    std::ostringstream out;
    out << "ps=" << hex64(snapshot.current_ps_hash)
        << " size=" << snapshot.current_ps_size
        << " kind=" << kind
        << " count=" << element_count
        << " vp=" << snapshot.viewport_width << "x" << snapshot.viewport_height
        << " out=" << snapshot.backbuffer_width << "x" << snapshot.backbuffer_height
        << " srv=" << limited_resource_shapes(snapshot.ps_srvs, 8)
        << " rtv=" << limited_resource_shapes(snapshot.rtvs, 4);
    if (snapshot.dsv.resource_key != 0)
        out << " dsv=" << resource_shape_signature(snapshot.dsv);
    else
        out << " dsv=none";
    increment_string_counter(stats.ps_post_contexts, out.str());
}

void note_similarity_event(SimilarityStats &stats)
{
    const ULONGLONG now = GetTickCount64();
    if (stats.first_event_tick == 0)
        stats.first_event_tick = now;
    stats.last_event_tick = now;
}

std::uint64_t similarity_duration_ms(const SimilarityStats &stats)
{
    if (stats.first_event_tick == 0 || stats.last_event_tick <= stats.first_event_tick)
        return 0;
    return stats.last_event_tick - stats.first_event_tick;
}

double similarity_rate(std::uint64_t count, const SimilarityStats &stats)
{
    const std::uint64_t duration_ms = similarity_duration_ms(stats);
    if (duration_ms == 0)
        return 0.0;
    return static_cast<double>(count) * 1000.0 / static_cast<double>(duration_ms);
}

template <std::size_t Count>
void append_resource_array_json(std::ostringstream &out, const char *name, const std::array<ResourceInfo, Count> &resources)
{
    out << "\"" << name << "\":[";
    bool first = true;
    for (std::size_t slot = 0; slot < resources.size(); ++slot)
    {
        const ResourceInfo &info = resources[slot];
        if (info.resource_key == 0)
            continue;
        if (!first)
            out << ",";
        first = false;
        out << "{\"slot\":" << slot
            << ",\"resource\":\"" << hex64(info.resource_key) << "\""
            << ",\"w\":" << info.width
            << ",\"h\":" << info.height
            << ",\"fmt\":" << static_cast<std::uint32_t>(info.format)
            << "}";
    }
    out << "]";
}

template <std::size_t Count>
void append_buffer_array_json(std::ostringstream &out, const char *name, const std::array<BufferInfo, Count> &buffers)
{
    out << "\"" << name << "\":[";
    bool first = true;
    for (std::size_t slot = 0; slot < buffers.size(); ++slot)
    {
        const BufferInfo &info = buffers[slot];
        if (info.resource_key == 0)
            continue;
        if (!first)
            out << ",";
        first = false;
        out << "{\"slot\":" << slot
            << ",\"resource\":\"" << hex64(info.resource_key) << "\""
            << ",\"bytes\":" << info.byte_width
            << ",\"update_hash\":\"" << hex64(info.last_update_hash) << "\""
            << ",\"update_size\":" << info.last_update_size;
        if (slot == 0)
        {
            const std::vector<std::uint8_t> snapshot = lookup_buffer_snapshot(info.resource_key);
            if (!snapshot.empty())
                out << ",\"data_hex\":\"" << bytes_to_hex(snapshot) << "\"";
        }
        out << "}";
    }
    out << "]";
}

void maybe_write_pixel_shader_trace(const DispatchState &snapshot, const char *kind, UINT element_count)
{
    if (!g_config.trace_pixel_shader_draws || g_config.trace_pixel_shader_hash == 0 ||
        snapshot.current_ps_hash != g_config.trace_pixel_shader_hash)
        return;

    const std::uint32_t trace_index = g_ps_trace_count.fetch_add(1);
    if (trace_index >= g_config.pixel_shader_trace_limit)
        return;

    if (snapshot.ps_cbs[0].resource_key != 0)
        g_trace_ps_cb0_key = snapshot.ps_cbs[0].resource_key;

    std::ostringstream line;
    line << "{\"index\":" << trace_index
        << ",\"tick\":" << GetTickCount64()
        << ",\"kind\":\"" << kind << "\""
        << ",\"count\":" << element_count
        << ",\"ps\":\"" << hex64(snapshot.current_ps_hash) << "\""
        << ",\"viewport\":[" << snapshot.viewport_width << "," << snapshot.viewport_height << "]"
        << ",\"output\":[" << snapshot.backbuffer_width << "," << snapshot.backbuffer_height << "]";
    line << ",";
    append_resource_array_json(line, "srvs", snapshot.ps_srvs);
    line << ",";
    append_resource_array_json(line, "rtvs", snapshot.rtvs);
    line << ",\"dsv\":";
    if (snapshot.dsv.resource_key != 0)
    {
        line << "{\"resource\":\"" << hex64(snapshot.dsv.resource_key) << "\""
            << ",\"w\":" << snapshot.dsv.width
            << ",\"h\":" << snapshot.dsv.height
            << ",\"fmt\":" << static_cast<std::uint32_t>(snapshot.dsv.format)
            << "}";
    }
    else
    {
        line << "null";
    }
    line << ",";
    append_buffer_array_json(line, "cbs", snapshot.ps_cbs);
    line << "}";

    std::lock_guard lock(g_ps_trace_mutex);
    if (!g_ps_trace_stream.is_open())
        g_ps_trace_stream.open(g_ps_trace_path, std::ios::app);
    if (g_ps_trace_stream)
    {
        g_ps_trace_stream << line.str() << "\n";
        if ((trace_index + 1) % 64 == 0 || trace_index + 1 == g_config.pixel_shader_trace_limit)
            g_ps_trace_stream.flush();
    }
}

std::string sanitize_label(std::string label)
{
    if (label.empty())
        return "UNLABELED";

    for (char &ch : label)
    {
        const bool keep = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
        if (!keep)
            ch = '_';
    }
    return label;
}

std::filesystem::path similarity_path_for_label(const std::string &label)
{
    std::wstring file = L"Dx11FsrBridge.similarity.";
    file += widen_ascii(sanitize_label(label));
    file += L".txt";
    return g_module_dir / file;
}

void write_similarity_report_to_path_locked(const std::filesystem::path &path, const SimilarityStats &stats, const std::string &label)
{
    if (!g_config.enable_similarity_probe)
        return;

    std::ofstream out(path, std::ios::trunc);
    if (!out)
        return;

    int temporal_score = 0;
    if (stats.lowres_input_hits > 0)
        temporal_score += 20;
    if (stats.fullres_output_hits > 0)
        temporal_score += 15;
    if (stats.depth_input_hits > 0)
        temporal_score += 15;
    if (stats.motion_input_hits > 0)
        temporal_score += 20;
    if (stats.history_read_after_write_hits > 5)
        temporal_score += 20;
    if (stats.cs_hash_counts.size() >= 6 || stats.ps_hash_counts.size() >= 6)
        temporal_score += 10;

    int fsr2_score = 0;
    if (stats.lowres_input_hits > 0 && stats.fullres_output_hits > 0)
        fsr2_score += 20;
    if (stats.depth_input_hits > 0)
        fsr2_score += 20;
    if (stats.motion_input_hits > 0)
        fsr2_score += 30;
    if (stats.history_read_after_write_hits > 5 && stats.temporal_chain_hits > 5)
        fsr2_score += 20;
    if (stats.cs_hash_counts.size() >= 6 || stats.ps_hash_counts.size() >= 6)
        fsr2_score += 10;

    const char *temporal_verdict = "low";
    if (temporal_score >= 75)
        temporal_verdict = "high";
    else if (temporal_score >= 45)
        temporal_verdict = "medium";

    const char *fsr2_verdict = "unconfirmed";
    if (stats.motion_input_hits > 0)
    {
        if (fsr2_score >= 75)
            fsr2_verdict = "high";
        else if (fsr2_score >= 45)
            fsr2_verdict = "medium";
        else
            fsr2_verdict = "low";
    }

    out << "Dx11FsrBridge FSR-like similarity report\n";
    out << "run_label=" << (label.empty() ? "UNLABELED" : label) << "\n";
    out << "temporal_upscale_verdict=" << temporal_verdict << "\n";
    out << "temporal_upscale_score=" << temporal_score << "/100\n";
    out << "fsr2_specific_verdict=" << fsr2_verdict << "\n";
    out << "fsr2_specific_score=" << fsr2_score << "/100\n";
    out << "note=Temporal score detects generic temporal reconstruction/upscaling. Render scale below 1.0 can trigger lowres-to-window output even when the game mode is not FSR2. FSR2-specific evidence must be mode-specific and preferably include motion-vector input.\n";
    out << "output_size=" << stats.output_width << "x" << stats.output_height << "\n";
    out << "capture_duration_ms=" << similarity_duration_ms(stats) << "\n";
    out << "dispatch_count=" << stats.dispatch_count << "\n";
    out << "draw_count=" << stats.draw_count << "\n";
    out << std::fixed << std::setprecision(2);
    out << "dispatch_per_second=" << similarity_rate(stats.dispatch_count, stats) << "\n";
    out << "draw_per_second=" << similarity_rate(stats.draw_count, stats) << "\n";
    out << std::defaultfloat;
    out << "lowres_input_hits=" << stats.lowres_input_hits << "\n";
    out << "fullres_output_hits=" << stats.fullres_output_hits << "\n";
    out << "depth_input_hits=" << stats.depth_input_hits << "\n";
    out << "motion_input_hits=" << stats.motion_input_hits << "\n";
    out << "history_read_after_write_hits=" << stats.history_read_after_write_hits << "\n";
    out << "temporal_chain_hits=" << stats.temporal_chain_hits << "\n";
    out << "unique_cs_hashes=" << stats.cs_hash_counts.size() << "\n";
    out << "unique_ps_hashes=" << stats.ps_hash_counts.size() << "\n";
    out << "top_cs=" << top_entries_string(stats.cs_hash_counts, 12) << "\n";
    out << "top_cs_reflect=" << top_compute_entries_with_reflection(stats.cs_hash_counts, 12) << "\n";
    out << "top_ps=" << top_entries_string(stats.ps_hash_counts, 12) << "\n";
    out << "top_ps_post=" << top_entries_string(stats.ps_post_hash_counts, 16) << "\n";
    out << "lowres_candidates=" << top_entries_string(stats.lowres_candidates, 10) << "\n";
    out << "fullres_candidates=" << top_entries_string(stats.fullres_candidates, 10) << "\n";
    out << "depth_candidates=" << top_entries_string(stats.depth_candidates, 10) << "\n";
    out << "motion_candidates=" << top_entries_string(stats.motion_candidates, 10) << "\n";
    out << "history_candidates=" << top_entries_string(stats.resource_read_after_write_counts, 12) << "\n";
    out << "cs_2d_post_contexts=" << top_entries_string(stats.cs_2d_post_contexts, 16) << "\n";
    out << "ps_post_contexts=" << top_entries_string(stats.ps_post_contexts, 24) << "\n";

    out << "\ninterpretation:\n";
    out << "- temporal_upscale high: the frame graph strongly looks like temporal reconstruction/upscaling, not necessarily FSR2.\n";
    out << "- fsr2_specific unconfirmed: motion vectors were not identified, so do not call it FSR2-like yet.\n";
    out << "- Compare FSR_ON/FSR_OFF/SMAA reports; FSR_ON-only deltas matter more than one absolute score.\n";
}

void write_similarity_report_locked()
{
    std::string label;
    if (!g_config.run_label.empty())
        label = sanitize_label(narrow(g_config.run_label));

    write_similarity_report_to_path_locked(g_similarity_path, g_similarity, label);
    if (!label.empty())
        write_similarity_report_to_path_locked(similarity_path_for_label(label), g_similarity, label);
}

void reset_similarity_locked()
{
    g_similarity = {};
}

template <typename Key>
std::string format_delta_key(const Key &key)
{
    if constexpr (std::is_integral_v<Key>)
        return hex64(static_cast<std::uint64_t>(key));
    else
        return key;
}

template <typename Key>
std::string positive_delta_entries_string(
    const std::unordered_map<Key, std::uint64_t> &current,
    const std::unordered_map<Key, std::uint64_t> &baseline,
    std::size_t limit)
{
    std::vector<std::pair<Key, std::uint64_t>> entries;
    for (const auto &[key, value] : current)
    {
        const auto it = baseline.find(key);
        const std::uint64_t base_value = it == baseline.end() ? 0 : it->second;
        if (value <= base_value)
            continue;

        const std::uint64_t delta = value - base_value;
        if (delta < 8 && value < base_value * 2 + 8)
            continue;
        entries.emplace_back(key, delta);
    }

    std::sort(entries.begin(), entries.end(), [](const auto &left, const auto &right) {
        return left.second > right.second;
    });

    std::ostringstream out;
    for (std::size_t i = 0; i < entries.size() && i < limit; ++i)
    {
        if (i != 0)
            out << ", ";
        out << format_delta_key(entries[i].first) << "=+" << entries[i].second;
    }
    if (entries.empty())
        out << "none";
    return out.str();
}

std::string positive_compute_delta_entries_with_reflection(
    const std::unordered_map<std::uint64_t, std::uint64_t> &current,
    const std::unordered_map<std::uint64_t, std::uint64_t> &baseline,
    std::size_t limit,
    bool only_2d_post_candidates)
{
    std::vector<std::pair<std::uint64_t, std::uint64_t>> entries;
    for (const auto &[key, value] : current)
    {
        if (only_2d_post_candidates && !is_compute_shader_2d_post_candidate(key))
            continue;

        const auto it = baseline.find(key);
        const std::uint64_t base_value = it == baseline.end() ? 0 : it->second;
        if (value <= base_value)
            continue;

        const std::uint64_t delta = value - base_value;
        if (delta < 8 && value < base_value * 2 + 8)
            continue;
        entries.emplace_back(key, delta);
    }

    std::sort(entries.begin(), entries.end(), [](const auto &left, const auto &right) {
        return left.second > right.second;
    });

    std::ostringstream out;
    for (std::size_t i = 0; i < entries.size() && i < limit; ++i)
    {
        if (i != 0)
            out << ", ";
        out << hex64(entries[i].first) << "=+" << entries[i].second
            << "(" << compute_shader_reflection_string(entries[i].first) << ")";
    }
    if (entries.empty())
        out << "none";
    return out.str();
}

template <typename Key>
std::string positive_rate_delta_entries_string(
    const std::unordered_map<Key, std::uint64_t> &current,
    const SimilarityStats &current_stats,
    const std::unordered_map<Key, std::uint64_t> &baseline,
    const SimilarityStats &baseline_stats,
    std::size_t limit)
{
    struct Entry
    {
        Key key;
        double delta_rate = 0.0;
        double current_rate = 0.0;
        double baseline_rate = 0.0;
    };

    std::vector<Entry> entries;
    for (const auto &[key, value] : current)
    {
        const auto it = baseline.find(key);
        const std::uint64_t base_value = it == baseline.end() ? 0 : it->second;
        const double current_rate = similarity_rate(value, current_stats);
        const double baseline_rate = similarity_rate(base_value, baseline_stats);
        if (current_rate <= baseline_rate)
            continue;

        const double delta_rate = current_rate - baseline_rate;
        if (delta_rate < 1.0 && current_rate < baseline_rate * 2.0 + 1.0)
            continue;
        entries.push_back({ key, delta_rate, current_rate, baseline_rate });
    }

    std::sort(entries.begin(), entries.end(), [](const Entry &left, const Entry &right) {
        return left.delta_rate > right.delta_rate;
    });

    std::ostringstream out;
    out << std::fixed << std::setprecision(2);
    for (std::size_t i = 0; i < entries.size() && i < limit; ++i)
    {
        if (i != 0)
            out << ", ";
        out << format_delta_key(entries[i].key)
            << "=+" << entries[i].delta_rate << "/s"
            << "(on=" << entries[i].current_rate << "/s base=" << entries[i].baseline_rate << "/s)";
    }
    if (entries.empty())
        out << "none";
    return out.str();
}

void write_similarity_diff_locked()
{
    const auto on_it = g_similarity_archives.find("FSR_ON");
    if (on_it == g_similarity_archives.end())
        return;

    const std::filesystem::path diff_path = g_module_dir / L"Dx11FsrBridge.similarity.diff.txt";
    std::ofstream out(diff_path, std::ios::trunc);
    if (!out)
        return;

    const SimilarityStats &on = on_it->second;
    out << "Dx11FsrBridge mode differential report\n";
    out << "note=This compares recorded hotkey windows. It is intended to separate generic render-scale upscaling from FSR_ON-specific features.\n";
    out << "note_rates=Rate-normalized fields should be preferred when recording window durations differ.\n";
    out << "fsr_on_capture_duration_ms=" << similarity_duration_ms(on) << "\n";
    out << "fsr_on_dispatch_count=" << on.dispatch_count << "\n";
    out << "fsr_on_draw_count=" << on.draw_count << "\n";

    const auto write_compare = [&](const char *baseline_label) {
        const auto base_it = g_similarity_archives.find(baseline_label);
        if (base_it == g_similarity_archives.end())
            return;

        const SimilarityStats &base = base_it->second;
        out << "\n[FSR_ON minus " << baseline_label << "]\n";
        out << "baseline_capture_duration_ms=" << similarity_duration_ms(base) << "\n";
        out << "dispatch_delta=" << static_cast<std::int64_t>(on.dispatch_count) - static_cast<std::int64_t>(base.dispatch_count) << "\n";
        out << "draw_delta=" << static_cast<std::int64_t>(on.draw_count) - static_cast<std::int64_t>(base.draw_count) << "\n";
        out << std::fixed << std::setprecision(2);
        out << "dispatch_rate_delta_per_second=" << similarity_rate(on.dispatch_count, on) - similarity_rate(base.dispatch_count, base) << "\n";
        out << "draw_rate_delta_per_second=" << similarity_rate(on.draw_count, on) - similarity_rate(base.draw_count, base) << "\n";
        out << std::defaultfloat;
        out << "lowres_input_delta=" << static_cast<std::int64_t>(on.lowres_input_hits) - static_cast<std::int64_t>(base.lowres_input_hits) << "\n";
        out << "fullres_output_delta=" << static_cast<std::int64_t>(on.fullres_output_hits) - static_cast<std::int64_t>(base.fullres_output_hits) << "\n";
        out << "depth_input_delta=" << static_cast<std::int64_t>(on.depth_input_hits) - static_cast<std::int64_t>(base.depth_input_hits) << "\n";
        out << "motion_input_delta=" << static_cast<std::int64_t>(on.motion_input_hits) - static_cast<std::int64_t>(base.motion_input_hits) << "\n";
        out << "history_read_after_write_delta=" << static_cast<std::int64_t>(on.history_read_after_write_hits) - static_cast<std::int64_t>(base.history_read_after_write_hits) << "\n";
        out << "temporal_chain_delta=" << static_cast<std::int64_t>(on.temporal_chain_hits) - static_cast<std::int64_t>(base.temporal_chain_hits) << "\n";
        out << "cs_more_in_fsr_on=" << positive_delta_entries_string(on.cs_hash_counts, base.cs_hash_counts, 16) << "\n";
        out << "cs_more_in_fsr_on_reflect=" << positive_compute_delta_entries_with_reflection(on.cs_hash_counts, base.cs_hash_counts, 16, false) << "\n";
        out << "cs_2d_post_more_in_fsr_on=" << positive_compute_delta_entries_with_reflection(on.cs_hash_counts, base.cs_hash_counts, 16, true) << "\n";
        out << "ps_more_in_fsr_on=" << positive_delta_entries_string(on.ps_hash_counts, base.ps_hash_counts, 16) << "\n";
        out << "ps_rate_more_in_fsr_on=" << positive_rate_delta_entries_string(on.ps_hash_counts, on, base.ps_hash_counts, base, 20) << "\n";
        out << "ps_post_more_in_fsr_on=" << positive_delta_entries_string(on.ps_post_hash_counts, base.ps_post_hash_counts, 20) << "\n";
        out << "ps_post_rate_more_in_fsr_on=" << positive_rate_delta_entries_string(on.ps_post_hash_counts, on, base.ps_post_hash_counts, base, 20) << "\n";
        out << "lowres_more_in_fsr_on=" << positive_delta_entries_string(on.lowres_candidates, base.lowres_candidates, 12) << "\n";
        out << "fullres_more_in_fsr_on=" << positive_delta_entries_string(on.fullres_candidates, base.fullres_candidates, 12) << "\n";
        out << "depth_more_in_fsr_on=" << positive_delta_entries_string(on.depth_candidates, base.depth_candidates, 12) << "\n";
        out << "motion_more_in_fsr_on=" << positive_delta_entries_string(on.motion_candidates, base.motion_candidates, 12) << "\n";
        out << "history_more_in_fsr_on=" << positive_delta_entries_string(on.resource_read_after_write_counts, base.resource_read_after_write_counts, 12) << "\n";
        out << "cs_2d_post_contexts_more_in_fsr_on=" << positive_delta_entries_string(on.cs_2d_post_contexts, base.cs_2d_post_contexts, 20) << "\n";
        out << "ps_post_contexts_more_in_fsr_on=" << positive_delta_entries_string(on.ps_post_contexts, base.ps_post_contexts, 30) << "\n";
    };

    write_compare("FSR_OFF");
    write_compare("SMAA");
}

void maybe_write_similarity_report_locked()
{
    const ULONGLONG now = GetTickCount64();
    if (g_similarity.last_report_tick != 0 && now - g_similarity.last_report_tick < g_config.similarity_report_interval_ms)
        return;
    g_similarity.last_report_tick = now;
    write_similarity_report_locked();
}

void record_similarity_dispatch(UINT group_x, UINT group_y, UINT group_z)
{
    if (!g_config.enable_similarity_probe)
        return;

    DispatchState snapshot {};
    {
        std::lock_guard lock(g_state_mutex);
        snapshot = g_state;
    }

    std::lock_guard lock(g_similarity_mutex);
    SimilarityStats &stats = g_similarity;
    note_similarity_event(stats);
    stats.dispatch_count++;
    stats.output_width = snapshot.backbuffer_width;
    stats.output_height = snapshot.backbuffer_height;
    if (snapshot.current_cs_hash != 0)
        stats.cs_hash_counts[snapshot.current_cs_hash]++;
    note_2d_post_context(stats, snapshot, group_x, group_y, group_z);

    const std::string writer = "cs=" + hex64(snapshot.current_cs_hash) + " dispatch=" +
        std::to_string(group_x) + "x" + std::to_string(group_y) + "x" + std::to_string(group_z);

    for (const ResourceInfo &srv : snapshot.cs_srvs)
    {
        if (srv.resource_key == 0)
            continue;
        note_resource_read(stats, srv, "cs");
        if (is_lowres_surface(srv, snapshot.backbuffer_width, snapshot.backbuffer_height))
        {
            stats.lowres_input_hits++;
            increment_string_counter(stats.lowres_candidates, resource_signature(srv));
        }
        if (is_depth_like_format(srv.format))
        {
            stats.depth_input_hits++;
            increment_string_counter(stats.depth_candidates, resource_signature(srv));
        }
        if (is_motion_like_format(srv.format) && is_lowres_surface(srv, snapshot.backbuffer_width, snapshot.backbuffer_height))
        {
            stats.motion_input_hits++;
            increment_string_counter(stats.motion_candidates, resource_signature(srv));
        }
    }

    for (const ResourceInfo &uav : snapshot.cs_uavs)
    {
        if (uav.resource_key == 0)
            continue;
        if (is_fullres_surface(uav, snapshot.backbuffer_width, snapshot.backbuffer_height))
        {
            stats.fullres_output_hits++;
            increment_string_counter(stats.fullres_candidates, resource_signature(uav));
        }
        note_resource_write(stats, uav, writer);
    }

    for (const ResourceInfo &rtv : snapshot.rtvs)
    {
        if (rtv.resource_key == 0)
            continue;
        if (is_fullres_surface(rtv, snapshot.backbuffer_width, snapshot.backbuffer_height))
        {
            stats.fullres_output_hits++;
            increment_string_counter(stats.fullres_candidates, resource_signature(rtv));
        }
        note_resource_write(stats, rtv, writer + " rtv");
    }

    if (snapshot.dsv.resource_key != 0)
    {
        stats.depth_input_hits++;
        increment_string_counter(stats.depth_candidates, resource_signature(snapshot.dsv));
    }

    maybe_write_similarity_report_locked();
}

void record_similarity_draw(const char *kind, UINT element_count)
{
    const bool trace_target = g_config.trace_pixel_shader_draws &&
        g_config.trace_pixel_shader_hash != 0 &&
        g_current_ps_hash.load(std::memory_order_relaxed) == g_config.trace_pixel_shader_hash;
    if (!g_config.enable_similarity_probe && !trace_target)
        return;

    DispatchState snapshot {};
    {
        std::lock_guard lock(g_state_mutex);
        snapshot = g_state;
    }

    if (trace_target)
        maybe_write_pixel_shader_trace(snapshot, kind, element_count);

    if (!g_config.enable_similarity_probe)
        return;

    std::lock_guard lock(g_similarity_mutex);
    SimilarityStats &stats = g_similarity;
    note_similarity_event(stats);
    stats.draw_count++;
    stats.output_width = snapshot.backbuffer_width;
    stats.output_height = snapshot.backbuffer_height;
    if (snapshot.current_ps_hash != 0)
        stats.ps_hash_counts[snapshot.current_ps_hash]++;
    note_pixel_shader_post_context(stats, snapshot, kind, element_count);

    const std::string writer = std::string(kind) + " ps=" + hex64(snapshot.current_ps_hash);

    for (const ResourceInfo &srv : snapshot.ps_srvs)
    {
        if (srv.resource_key == 0)
            continue;
        note_resource_read(stats, srv, "ps");
        if (is_lowres_surface(srv, snapshot.backbuffer_width, snapshot.backbuffer_height))
        {
            stats.lowres_input_hits++;
            increment_string_counter(stats.lowres_candidates, resource_signature(srv));
        }
        if (is_depth_like_format(srv.format))
        {
            stats.depth_input_hits++;
            increment_string_counter(stats.depth_candidates, resource_signature(srv));
        }
        if (is_motion_like_format(srv.format) && is_lowres_surface(srv, snapshot.backbuffer_width, snapshot.backbuffer_height))
        {
            stats.motion_input_hits++;
            increment_string_counter(stats.motion_candidates, resource_signature(srv));
        }
    }

    for (const ResourceInfo &rtv : snapshot.rtvs)
    {
        if (rtv.resource_key == 0)
            continue;
        if (is_fullres_surface(rtv, snapshot.backbuffer_width, snapshot.backbuffer_height))
        {
            stats.fullres_output_hits++;
            increment_string_counter(stats.fullres_candidates, resource_signature(rtv));
        }
        note_resource_write(stats, rtv, writer);
    }

    if (snapshot.dsv.resource_key != 0)
    {
        stats.depth_input_hits++;
        increment_string_counter(stats.depth_candidates, resource_signature(snapshot.dsv));
    }

    maybe_write_similarity_report_locked();
}

bool is_relevant_surface(const ResourceInfo &info, std::uint32_t output_width, std::uint32_t output_height)
{
    if (info.width == 0 || info.height == 0 || output_width == 0 || output_height == 0)
        return false;
    if (info.width > output_width || info.height > output_height)
        return false;
    return info.width >= output_width / 4 || info.height >= output_height / 4;
}

void append_resource_list(std::ostringstream &out, const char *label, const auto &resources, std::uint32_t output_width, std::uint32_t output_height)
{
    bool wrote_any = false;
    std::size_t emitted = 0;
    for (std::size_t i = 0; i < resources.size() && emitted < 6; ++i)
    {
        const ResourceInfo &info = resources[i];
        if (!is_relevant_surface(info, output_width, output_height))
            continue;

        if (!wrote_any)
        {
            out << " " << label << "=";
            wrote_any = true;
        }
        else
        {
            out << ",";
        }

        out << i << ":" << info.width << "x" << info.height << "/f" << static_cast<std::uint32_t>(info.format);
        ++emitted;
    }
}

bool should_log_interesting_dispatch(UINT group_x, UINT group_y, UINT group_z)
{
    if (group_z != 1)
        return false;

    std::lock_guard lock(g_state_mutex);
    const std::uint32_t output_width = g_state.backbuffer_width;
    const std::uint32_t output_height = g_state.backbuffer_height;
    if (output_width == 0 || output_height == 0)
        return false;

    for (const ResourceInfo &info : g_state.cs_srvs)
    {
        if (is_relevant_surface(info, output_width, output_height))
            return true;
    }
    for (const ResourceInfo &info : g_state.cs_uavs)
    {
        if (is_relevant_surface(info, output_width, output_height))
            return true;
    }

    return false;
}

void log_interesting_dispatch_details(UINT group_x, UINT group_y, UINT group_z)
{
    std::ostringstream signature;
    std::ostringstream message;
    {
        std::lock_guard lock(g_state_mutex);
        const std::uint64_t shader_identity = g_state.current_cs_hash != 0 ? g_state.current_cs_hash : g_state.current_cs_shader;
        signature << hex64(shader_identity) << "|" << group_x << "x" << group_y << "x" << group_z
                  << "|" << g_state.backbuffer_width << "x" << g_state.backbuffer_height;
        message << "dispatch_detail cs=" << hex64(g_state.current_cs_shader)
                << " cs_hash=" << hex64(g_state.current_cs_hash)
                << " cs_size=" << g_state.current_cs_size
                << " groups=" << group_x << "x" << group_y << "x" << group_z
                << " output=" << g_state.backbuffer_width << "x" << g_state.backbuffer_height;

        append_resource_list(message, "srv", g_state.cs_srvs, g_state.backbuffer_width, g_state.backbuffer_height);
        append_resource_list(message, "uav", g_state.cs_uavs, g_state.backbuffer_width, g_state.backbuffer_height);
        append_resource_list(message, "rtv", g_state.rtvs, g_state.backbuffer_width, g_state.backbuffer_height);
        append_constant_buffer_list(message, "cb", g_state.cs_cbs);

        for (const ResourceInfo &info : g_state.cs_srvs)
        {
            if (is_relevant_surface(info, g_state.backbuffer_width, g_state.backbuffer_height))
                signature << "|s:" << info.width << "x" << info.height << "/" << static_cast<std::uint32_t>(info.format);
        }
        for (const ResourceInfo &info : g_state.cs_uavs)
        {
            if (is_relevant_surface(info, g_state.backbuffer_width, g_state.backbuffer_height))
                signature << "|u:" << info.width << "x" << info.height << "/" << static_cast<std::uint32_t>(info.format);
        }
    }

    bool should_emit = false;
    bool phase_reset = false;
    std::uint32_t phase = 0;
    {
        std::lock_guard lock(g_dispatch_signature_mutex);
        const ULONGLONG now = GetTickCount64();
        if (g_last_interesting_dispatch_tick != 0 && now >= g_last_interesting_dispatch_tick &&
            now - g_last_interesting_dispatch_tick >= g_config.interesting_dispatch_phase_gap_ms)
        {
            g_dispatch_signature_counts.clear();
            ++g_dispatch_phase;
            phase_reset = true;
        }
        g_last_interesting_dispatch_tick = now;
        phase = g_dispatch_phase;

        if (g_dispatch_signature_counts.size() < g_config.interesting_dispatch_log_limit || g_dispatch_signature_counts.contains(signature.str()))
        {
            std::uint32_t &count = g_dispatch_signature_counts[signature.str()];
            if (count < 2)
            {
                ++count;
                should_emit = true;
            }
        }
    }

    if (phase_reset)
        log_line("dispatch_phase phase=" + std::to_string(phase));

    if (should_emit)
    {
        const std::string prefix = "dispatch_detail";
        std::string text = message.str();
        if (text.starts_with(prefix))
            text.insert(prefix.size(), " phase=" + std::to_string(phase));
        log_line(text);
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
        update_osd_from_dispatch(phase, group_x, group_y, group_z);
#endif
    }
}

void log_line(const std::string &line)
{
    if (!g_logging_enabled.load(std::memory_order_relaxed))
        return;
#if defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    // Ffx12FullLogging=1 时跳过白名单过滤（全量日志——排查切换渲染精度卡死等）
    if (!g_config.ffx12_full_logging)
    {
    static constexpr std::array<std::string_view, 11> error_terms {
        "failed", "failure", "error", "invalid", "mismatch", "exception",
        "unavailable", "unresolved", "unsupported", "missing", "refusing"
    };
    static constexpr std::array<std::string_view, 10> basic_terms {
        // 正式版日志精简——只保留：接管结果(ffx12_result/failed)、
        // 显卡型号(ffx12_gpu)、SDK 路径(ffx12_sdk)、FSR 实际版本(ffx12_version)、渲染精度菜单状态。
        // hook 数据状态（draw_hook_active/iat_scan/fsr2_translation_candidate/ffx12_path 等）一律不写。
        "ffx12_gpu", "ffx12_sdk", "ffx12_version", "ffx12_result", "ffx12_failed",
        "jit_norm", "jitter_px",
        "render_scale_menu hook_ready", "render_scale_menu render_scale_written",
        "fsr2_on_demand_identify_fail"
    };
    const bool startup_marker = line.starts_with("Dx11FsrBridge active");
    const bool error_message = std::any_of(error_terms.begin(), error_terms.end(),
        [&](std::string_view term) { return line.find(term) != std::string::npos; });
    const bool basic_message = line.starts_with("warning ") ||
        std::any_of(basic_terms.begin(), basic_terms.end(),
            [&](std::string_view term) { return line.find(term) != std::string::npos; });
    if (!startup_marker && !error_message && !basic_message)
        return;
    }
#endif
    std::lock_guard lock(g_log_mutex);
    static std::ofstream out(g_log_path, std::ios::app); // 常驻流：避免每次写盘开/关
    SYSTEMTIME st {};
    GetLocalTime(&st);
    char prefix[64] {};
    std::snprintf(prefix, sizeof(prefix), "%04u-%02u-%02u %02u:%02u:%02u.%03u ",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    out << prefix << line << "\n";
}

void reset_log()
{
    if (!g_logging_enabled.load(std::memory_order_relaxed))
        return;
    std::lock_guard lock(g_log_mutex);
    std::ofstream(g_log_path, std::ios::trunc).close();
}

// 焦点区域行——无条件写入（绕过 RELEASE 过滤白名单，用于分隔线/空行/关键信息框）
void log_focus_line(const std::string &line)
{
    if (!g_logging_enabled.load(std::memory_order_relaxed))
        return;
    std::lock_guard lock(g_log_mutex);
    static std::ofstream out(g_log_path, std::ios::app); // 常驻流：避免每次写盘开/关
    SYSTEMTIME st {};
    GetLocalTime(&st);
    char prefix[64] {};
    std::snprintf(prefix, sizeof(prefix), "%04u-%02u-%02u %02u:%02u:%02u.%03u ",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    out << prefix << line << "\n";
}

// 接管信息焦点框——显卡型号 / SDK 路径 / FSR 实际版本 相邻输出，
// 空行 + 分隔线制造明显焦点区域（排版参考：[ffx] Upscaler Version 风格）。
static void log_focus_block(const char *state)
{
    const std::string gpu =
        g_route_gpu_name.empty() ? std::string("unknown") : g_route_gpu_name;
    const std::string sdk =
        g_route_sdk_path.empty() ? std::string("unknown") : g_route_sdk_path;
    log_focus_line("");
    log_focus_line("====================================================");
    log_focus_line("  GPU : " + gpu + " (vendor=" + g_route_vendor_label + ")");
    log_focus_line("  SDK : " + sdk);
    log_focus_line("  FSR : FSR" + std::string(ffx12::matched_version_name()) +
                   "  [" + state + "]");
    log_focus_line("====================================================");
    log_focus_line("");
}

std::string module_path_from_address(void *address)
{
    HMODULE module = nullptr;
    if (address == nullptr || !GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCSTR>(address),
            &module))
    {
        return "unknown";
    }

    char path[MAX_PATH] {};
    const DWORD length = GetModuleFileNameA(module, path, static_cast<DWORD>(std::size(path)));
    return length != 0 ? std::string(path, length) : "unknown";
}

#if defined(DX11FSRBRIDGE_FG_DXGI_DIAGNOSTICS)
std::string describe_dxgi_swapchain_device(IUnknown *device)
{
    if (device == nullptr)
        return "null";

    std::ostringstream out;
    out << "ptr=" << hex64(reinterpret_cast<std::uintptr_t>(device));

    ID3D12CommandQueue *queue = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(&queue))) && queue != nullptr)
    {
        const D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
        ID3D12Device *d3d12_device = nullptr;
        const HRESULT device_hr = queue->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&d3d12_device));
        out << " kind=d3d12_queue type=" << static_cast<unsigned>(desc.Type)
            << " priority=" << desc.Priority
            << " flags=" << hex32(static_cast<std::uint32_t>(desc.Flags))
            << " node_mask=" << desc.NodeMask
            << " get_device_hr=" << hex32(static_cast<std::uint32_t>(device_hr));
        if (d3d12_device != nullptr)
        {
            const LUID luid = d3d12_device->GetAdapterLuid();
            out << " adapter_luid=" << hex32(static_cast<std::uint32_t>(luid.HighPart))
                << ":" << hex32(luid.LowPart);
            d3d12_device->Release();
        }
        queue->Release();
        return out.str();
    }

    ID3D12Device *d3d12_device = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D12Device), reinterpret_cast<void **>(&d3d12_device))) && d3d12_device != nullptr)
    {
        const LUID luid = d3d12_device->GetAdapterLuid();
        out << " kind=d3d12_device adapter_luid=" << hex32(static_cast<std::uint32_t>(luid.HighPart))
            << ":" << hex32(luid.LowPart);
        d3d12_device->Release();
        return out.str();
    }

    ID3D11Device *d3d11_device = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D11Device), reinterpret_cast<void **>(&d3d11_device))) && d3d11_device != nullptr)
    {
        out << " kind=d3d11_device feature_level=" << hex32(static_cast<std::uint32_t>(d3d11_device->GetFeatureLevel()))
            << " creation_flags=" << hex32(d3d11_device->GetCreationFlags());
        d3d11_device->Release();
        return out.str();
    }

    return out.str() + " kind=unknown";
}

std::string describe_dxgi_swapchain_desc(const DXGI_SWAP_CHAIN_DESC1 *desc)
{
    if (desc == nullptr)
        return "desc=null";

    std::ostringstream out;
    out << "size=" << desc->Width << "x" << desc->Height
        << " format=" << static_cast<unsigned>(desc->Format)
        << " sample=" << desc->SampleDesc.Count << "/" << desc->SampleDesc.Quality
        << " usage=" << hex32(desc->BufferUsage)
        << " buffers=" << desc->BufferCount
        << " scaling=" << static_cast<unsigned>(desc->Scaling)
        << " swap_effect=" << static_cast<unsigned>(desc->SwapEffect)
        << " alpha=" << static_cast<unsigned>(desc->AlphaMode)
        << " flags=" << hex32(desc->Flags)
        << " stereo=" << (desc->Stereo ? 1 : 0);
    return out.str();
}

std::string describe_dxgi_swapchain_desc(const DXGI_SWAP_CHAIN_DESC *desc)
{
    if (desc == nullptr)
        return "desc=null";

    std::ostringstream out;
    out << "size=" << desc->BufferDesc.Width << "x" << desc->BufferDesc.Height
        << " format=" << static_cast<unsigned>(desc->BufferDesc.Format)
        << " refresh=" << desc->BufferDesc.RefreshRate.Numerator << "/" << desc->BufferDesc.RefreshRate.Denominator
        << " sample=" << desc->SampleDesc.Count << "/" << desc->SampleDesc.Quality
        << " usage=" << hex32(desc->BufferUsage)
        << " buffers=" << desc->BufferCount
        << " windowed=" << (desc->Windowed ? 1 : 0)
        << " swap_effect=" << static_cast<unsigned>(desc->SwapEffect)
        << " flags=" << hex32(desc->Flags)
        << " output_window=" << hex64(reinterpret_cast<std::uintptr_t>(desc->OutputWindow));
    return out.str();
}

std::string describe_dxgi_fullscreen_desc(const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *desc)
{
    if (desc == nullptr)
        return "fullscreen=null";

    std::ostringstream out;
    out << "fullscreen_windowed=" << (desc->Windowed ? 1 : 0)
        << " refresh=" << desc->RefreshRate.Numerator << "/" << desc->RefreshRate.Denominator
        << " scanline=" << static_cast<unsigned>(desc->ScanlineOrdering)
        << " scaling=" << static_cast<unsigned>(desc->Scaling);
    return out.str();
}

std::string describe_dxgi_window(HWND hwnd)
{
    if (hwnd == nullptr)
        return "hwnd=null";

    RECT client {};
    RECT window {};
    GetClientRect(hwnd, &client);
    GetWindowRect(hwnd, &window);
    std::ostringstream out;
    out << "hwnd=" << hex64(reinterpret_cast<std::uintptr_t>(hwnd))
        << " client=" << (client.right - client.left) << "x" << (client.bottom - client.top)
        << " window=" << (window.right - window.left) << "x" << (window.bottom - window.top)
        << " style=" << hex64(static_cast<std::uint64_t>(GetWindowLongPtrW(hwnd, GWL_STYLE)))
        << " exstyle=" << hex64(static_cast<std::uint64_t>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)));
    return out.str();
}
#endif

const char *dxgi_format_name(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_UNKNOWN: return "UNKNOWN";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
    case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
    case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
    case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24_UINT";
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return "R32_FLOAT_X8X24_TYPELESS";
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT: return "X32_TYPELESS_G8X24_UINT";
    case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
    case DXGI_FORMAT_D16_UNORM: return "D16_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
    default: return "OTHER";
    }
}

std::string describe_dxgi_format(DXGI_FORMAT format)
{
    return std::to_string(static_cast<unsigned>(format)) + "/" + dxgi_format_name(format);
}

const char *color_space_name(DXGI_COLOR_SPACE_TYPE color_space)
{
    switch (color_space)
    {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709: return "RGB_FULL_G22_NONE_P709";
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709: return "RGB_FULL_G10_NONE_P709";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709: return "RGB_STUDIO_G22_NONE_P709";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P2020: return "RGB_STUDIO_G22_NONE_P2020";
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020: return "RGB_FULL_G2084_NONE_P2020";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020: return "RGB_STUDIO_G2084_NONE_P2020";
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020: return "RGB_FULL_G22_NONE_P2020";
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020: return "YCBCR_STUDIO_GHLG_TOPLEFT_P2020";
    case DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020: return "YCBCR_FULL_GHLG_TOPLEFT_P2020";
    default: return "OTHER";
    }
}

const char *hdr_metadata_type_name(DXGI_HDR_METADATA_TYPE type)
{
    switch (type)
    {
    case DXGI_HDR_METADATA_TYPE_NONE: return "NONE";
    case DXGI_HDR_METADATA_TYPE_HDR10: return "HDR10";
    default: return "OTHER";
    }
}

DXGI_FORMAT shader_resource_view_format(ID3D11ShaderResourceView *view)
{
    if (view == nullptr)
        return DXGI_FORMAT_UNKNOWN;
    D3D11_SHADER_RESOURCE_VIEW_DESC desc {};
    view->GetDesc(&desc);
    return desc.Format;
}

void set_osd_text(const std::wstring &text)
{
    if (!g_config.show_osd)
        return;

    {
        std::lock_guard lock(g_osd_mutex);
        g_osd_text = text;
    }

    HWND window = g_osd_window;
    if (window != nullptr)
        InvalidateRect(window, nullptr, TRUE);
}

// OSD：显示桥内 FSR 路径的生效状态（用户据此确认当前运行模式与传输，
// 悬浮文本窗口方式呈现，不干涉游戏画面——替代画面内版本标记）。
static void update_osd_sdk234(std::uint64_t count, std::uint32_t rw, std::uint32_t rh,
                              std::uint32_t dw, std::uint32_t dh)
{
    if (!g_config.show_osd)
        return;
    std::wostringstream out;
    out << L"Dx11FsrBridge OSD\n";
    out << L"FSR " << widen_ascii(ffx12::selected_version_name())
        << L" ACTIVE  disp=" << count << L"\n";
    {
        bool on12 = false, gpu_only = false;
        ffx12::interop_capabilities(on12, gpu_only);
        const bool gpu_ready = ffx12::gpu_interop_ready();
        out << L"transport=" << (on12 ? L"d3d11on12" : (gpu_ready ? L"gpu-shared(NT)" : L"cpu-staging"))
            << (gpu_only ? L"  gpu_only" : L"") << L"\n";
    }
    out << L"render=" << rw << L"x" << rh << L"  display=" << dw << L"x" << dh << L"\n";
    set_osd_text(out.str());
}

// OSD 悬浮窗跟随同进程游戏主窗口左上角，并强制置顶。
// 解决无边框/串流（GameViewer）场景独立桌面窗口被游戏画面覆盖的问题。
static void osd_anchor_to_game_window(HWND osd)
{
    if (osd == nullptr)
        return;
    HWND best = nullptr;
    for (HWND w = GetTopWindow(nullptr); w != nullptr; w = GetWindow(w, GW_HWNDNEXT))
    {
        if (!IsWindowVisible(w))
            continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid != GetCurrentProcessId())
            continue;
        wchar_t cls[64] {};
        GetClassNameW(w, cls, 63);
        if (wcscmp(cls, L"UnityWndClass") == 0)
        {
            best = w;
            break;
        }
        if (best == nullptr)
            best = w;
    }
    if (best == nullptr)
        return;
    RECT r {};
    if (!GetWindowRect(best, &r))
        return;
    SetWindowPos(osd, HWND_TOPMOST, r.left + 12, r.top + 12, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

LRESULT CALLBACK osd_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case WM_TIMER:
        poll_mode_hotkeys();
        osd_anchor_to_game_window(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps {};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rect {};
        GetClientRect(hwnd, &rect);

        HBRUSH background = CreateSolidBrush(RGB(16, 18, 20));
        FillRect(dc, &rect, background);
        DeleteObject(background);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(232, 238, 245));
        HFONT font = CreateFontW(
            18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        HFONT old_font = static_cast<HFONT>(SelectObject(dc, font));

        RECT text_rect { 12, 10, rect.right - 12, rect.bottom - 10 };
        std::wstring text;
        {
            std::lock_guard lock(g_osd_mutex);
            text = g_osd_text;
        }
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &text_rect, DT_LEFT | DT_TOP | DT_NOPREFIX | DT_WORDBREAK);

        SelectObject(dc, old_font);
        DeleteObject(font);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

DWORD WINAPI osd_thread_proc(LPVOID)
{
    const wchar_t *class_name = L"Dx11FsrBridgeOsdWindow";
    WNDCLASSEXW wc {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = osd_window_proc;
    wc.hInstance = g_module;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = class_name;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        class_name,
        L"Dx11FsrBridge OSD",
        WS_POPUP,
        24, 24, 560, 170,
        nullptr,
        nullptr,
        g_module,
        nullptr);

    if (hwnd == nullptr)
        return 0;

    g_osd_window = hwnd;
    SetLayeredWindowAttributes(hwnd, 0, 220, LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetTimer(hwnd, 1, 250, nullptr);

    MSG msg {};
    while (g_osd_running && GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_osd_window = nullptr;
    if (IsWindow(hwnd))
        DestroyWindow(hwnd);
    return 0;
}

void start_osd()
{
    if (!g_config.show_osd || g_osd_running)
        return;

    g_osd_running = true;
    g_osd_thread = CreateThread(nullptr, 0, osd_thread_proc, nullptr, 0, nullptr);
}

void update_osd_from_dispatch(std::uint32_t phase, UINT group_x, UINT group_y, UINT group_z)
{
    if (!g_config.show_osd)
        return;

    const std::vector<std::string> features = build_mode_features_from_state(group_x, group_y, group_z);
    {
        std::lock_guard lock(g_mode_mutex);
        for (const std::string &feature : features)
            add_limited_recent_feature(feature);
        if (g_recording_mode != 0)
        {
            append_features_to_mode_sample_locked(g_recording_mode, features);
            g_mode_status = L"正在记录 " + calibrated_mode_name(g_recording_mode) + L" 样本: " + std::to_wstring(g_mode_samples[g_recording_mode].size());
        }
    }
    poll_mode_hotkeys();
    const ModeMatch match = classify_current_mode();
    std::wstring mode_status;
    int recording_mode = 0;
    {
        std::lock_guard lock(g_mode_mutex);
        mode_status = g_mode_status;
        recording_mode = g_recording_mode;
    }

    std::uint64_t shader_hash = 0;
    std::size_t shader_size = 0;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    std::vector<BufferInfo> cbs;
    {
        std::lock_guard lock(g_state_mutex);
        shader_hash = g_state.current_cs_hash;
        shader_size = g_state.current_cs_size;
        output_width = g_state.backbuffer_width;
        output_height = g_state.backbuffer_height;
        for (const BufferInfo &cb : g_state.cs_cbs)
        {
            if (cb.resource_key != 0 && cb.byte_width != 0)
                cbs.push_back(cb);
            if (cbs.size() >= 3)
                break;
        }
    }

    std::wostringstream out;
    out << L"Dx11FsrBridge OSD\n";
    out << L"当前模式: " << match.name << L"    匹配=" << match.score << L"/" << match.total << L"    phase=" << phase << L"\n";
    out << L"输出分辨率: " << output_width << L"x" << output_height
        << L"    dispatch=" << group_x << L"x" << group_y << L"x" << group_z << L"\n";
    out << L"CS: " << widen_ascii(hex64(shader_hash)) << L"    size=" << shader_size << L"\n";
    out << L"CB: ";
    if (cbs.empty())
    {
        out << L"未捕获";
    }
    else
    {
        for (std::size_t i = 0; i < cbs.size(); ++i)
        {
            if (i != 0)
                out << L" | ";
            out << cbs[i].byte_width << L"/" << widen_ascii(hex64(cbs[i].last_update_hash));
        }
    }
    out << L"\n" << mode_status;
    if (recording_mode != 0)
        out << L"    REC=" << calibrated_mode_name(recording_mode);
    out << L"\nF7=FSR2开/停 F8=FSR2关/停 F9=SMAA/停 F10=清空全部";

    set_osd_text(out.str());
}

void load_config()
{
    const std::filesystem::path config_path = g_module_dir / L"Dx11FsrBridge.ini";
#if defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    g_config.enabled = GetPrivateProfileIntW(L"Dx11FsrBridge", L"Enabled", 1, config_path.c_str()) != 0;
    g_config.enable_logging = GetPrivateProfileIntW(L"Dx11FsrBridge", L"EnableLogging", 1, config_path.c_str()) != 0;
    g_logging_enabled.store(g_config.enable_logging, std::memory_order_relaxed);
    g_config.dlssg_dxgi_workaround =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"DlssgDxgiWorkaround", -1, config_path.c_str());
    g_config.hdr_swapchain_spoof =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"HdrSwapchainSpoof", 0, config_path.c_str()) != 0;
    g_config.hdr_swapchain_force =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"HdrSwapchainForce", 0, config_path.c_str()) != 0;
    g_config.hdr_environment_probe =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"HdrEnvironmentProbe", 0, config_path.c_str()) != 0;
    g_config.hdr_output_desc_probe =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"HdrOutputDescProbe", 0, config_path.c_str()) != 0;
    g_config.hdr_output_desc_spoof =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"HdrOutputDescSpoof", 0, config_path.c_str()) != 0;
    g_config.native_ldr_swapchain_unorm =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"NativeLdrSwapchainUnorm", 0, config_path.c_str()) != 0;
    g_config.dx11_on12_swapchain =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Dx11On12Swapchain", 0, config_path.c_str()) != 0;
    g_config.native_ldr_final_target_unorm =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"NativeLdrFinalTargetUnorm", 0, config_path.c_str()) != 0;
    g_config.hdr_sdr_tone_map =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"HdrSdrToneMap", 0, config_path.c_str()) != 0;
    g_config.hdr_sdr_tone_map_pq_input =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"HdrSdrToneMapPqInput", 1, config_path.c_str()) != 0;
    g_config.hdr_sdr_tone_map_paper_white = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(GetPrivateProfileIntW(
            L"Dx11FsrBridge", L"HdrSdrToneMapPaperWhite", 80, config_path.c_str())),
        1u,
        1000u);
    g_config.hdr_sdr_tone_map_peak = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(GetPrivateProfileIntW(
            L"Dx11FsrBridge", L"HdrSdrToneMapPeak", 100, config_path.c_str())),
        1u,
        10000u);
    g_config.hdr_composite_probe =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"HdrCompositeProbe", 0, config_path.c_str()) != 0;
    g_config.hdr_composite_probe_limit = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(GetPrivateProfileIntW(
            L"Dx11FsrBridge", L"HdrCompositeProbeLimit", 32, config_path.c_str())),
        1u,
        512u);
    g_config.capture_metadata_only =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"CaptureMetadataOnly", 0, config_path.c_str()) != 0;
#if defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
    g_config.enable_fsr2_get_proc_address_shim =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"EnableFsr2GetProcAddressShim", 1, config_path.c_str()) != 0;
    g_config.fsr2_translation_mode = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2TranslationMode", 2, config_path.c_str()));
    g_config.fsr2_mode2_on_demand_state =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2Mode2OnDemandState", 1, config_path.c_str()) != 0;
    g_config.fsr2_motion_vectors_jittered =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2MotionVectorsJittered", 0, config_path.c_str()) != 0;
    g_config.fsr2_positive_motion_vector_scale =
        // 2026-08-25（AA 修复，依据游戏 accumulate 反汇编）：游戏原生重投影 = UV − mv_g
        // （mv_g=+sign·4d² 正向、UV 空间）；FSR2 期望 historyUV = UV + scale·mv_sample。
        // 我们的解码 mv_sample = −mv_g（反向），故 scale 必须为 +renderSize（cb scale=+1）
        // → 有效运动 = −mv_g ✓ 与游戏一致。旧默认 0（−renderSize）把运动再翻转回 +mv_g
        // → 历史重投影方向全反 → 运动中历史每帧被拒/错位 → AA 退化为单帧放大（锯齿）。
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2MotionVectorScaleMode", 1, config_path.c_str()) == 1;
    g_config.fsr2_use_reactive_mask =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2UseReactiveMask", 1, config_path.c_str()) != 0;
    g_config.fsr2_use_transparency_mask =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2UseTransparencyMask", 0, config_path.c_str()) != 0;
    g_config.fsr2_jitter_mode = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2JitterMode", 3, config_path.c_str()));
    g_config.fsr2_hdr10_pq_color =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2Hdr10PqColor", 0, config_path.c_str()) != 0;
    g_config.fsr2_use_native_exposure =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2UseNativeExposure", 1, config_path.c_str()) != 0;
    g_config.fsr2_fast_metadata_copy =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2FastMetadataCopy", 1, config_path.c_str()) != 0;
    g_config.fsr2_compact_linear_output =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2CompactLinearOutput", 1, config_path.c_str()) != 0;
    g_config.fsr2_lock_color_producer_shader =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2LockColorProducerShader", 1, config_path.c_str()) != 0;
    g_config.fsr2_reset_on_color_path_change =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2ResetOnColorPathChange", 1, config_path.c_str()) != 0;
    g_config.block_dx11_on12_upscalers =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"BlockDx11On12Upscalers", 0, config_path.c_str()) != 0;
    g_config.fsr2_family_skip =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2FamilySkip", 0, config_path.c_str()) != 0;
    g_config.fsr2_family_expire_ms = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(GetPrivateProfileIntW(
            L"Dx11FsrBridge", L"Fsr2FamilyExpireMs", 500, config_path.c_str())),
        100u,
        5000u);
    g_config.fsr2_il2cpp_hook =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2Il2CppHook", 0, config_path.c_str()) != 0;
    g_config.fsr2_il2cpp_skip_render =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2Il2CppSkipRender", 0, config_path.c_str()) != 0;
    g_config.ffx12_probe_camera =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12ProbeCamera", 0, config_path.c_str()) != 0;
    g_config.ffx12_camera_hook =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12CameraHook", 0, config_path.c_str()) != 0;
    g_config.ffx12_projection_hook =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12ProjectionHook", 0, config_path.c_str()) != 0;
    const auto read_hex_rva = [&](const wchar_t *key, std::uint32_t fallback) {
        wchar_t buf[32] {};
        GetPrivateProfileStringW(L"Dx11FsrBridge", key, L"", buf, static_cast<DWORD>(std::size(buf)),
                                 config_path.c_str());
        const std::wstring text(buf);
        if (text.empty())
            return fallback;
        try
        {
            return static_cast<std::uint32_t>(std::stoul(text, nullptr, 0));
        }
        catch (...)
        {
            return fallback;
        }
    };
    g_config.fsr2_il2cpp_render_rva = read_hex_rva(L"Fsr2Il2CppRenderRva", 0x06B59670);
    g_config.fsr2_il2cpp_ucb_rva = read_hex_rva(L"Fsr2Il2CppUcbRva", 0x06B59600);
    g_config.ffx12_camera_rva = read_hex_rva(L"Ffx12CameraRva", 0x06B558D0);
    g_config.ffx12_projection_setter_rva =
        read_hex_rva(L"Ffx12ProjectionSetterRva", 0x013DC510);
    g_config.ffx12 =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12", 0, config_path.c_str()) != 0;
    g_config.ffx12_probe =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12Probe", 0, config_path.c_str()) != 0;
    g_config.optiscaler_bridge_probe =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"OptiScalerBridgeProbe", 0, config_path.c_str()) != 0;
    g_config.ffx12_jitter_mode = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12JitterMode", 4, config_path.c_str()));
    g_config.ffx12_depth_inverted =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12DepthInverted", 1, config_path.c_str()) != 0;
    g_config.ffx12_decode_motion =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12MotionDecode", 1, config_path.c_str()) != 0;
    g_config.ffx12_jitter_delay =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12JitterDelay", 0, config_path.c_str()) != 0;
    g_config.ffx12_force_reset =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12ForceReset", 0, config_path.c_str()) != 0;
    g_config.ffx12_hdr_input =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12Hdr", 1, config_path.c_str()) != 0;
    g_config.ffx12_auto_exposure =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12AutoExposure", 1, config_path.c_str()) != 0;
    g_config.ffx12_non_linear =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12NonLinear", 1, config_path.c_str()) != 0;
    ffx12::set_use_pq_chain(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12PqChain", 0, config_path.c_str()) != 0);
    g_config.ffx12_velocity_factor = std::clamp<float>(
        static_cast<float>(GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12VelocityFactor", 50, config_path.c_str())) / 100.0f,
        0.0f, 1.0f);
    {
        wchar_t motion_scale_buf[32] {};
        GetPrivateProfileStringW(L"Dx11FsrBridge", L"Ffx12MotionScale", L"1.0",
                                 motion_scale_buf, static_cast<DWORD>(std::size(motion_scale_buf)),
                                 config_path.c_str());
    g_config.ffx12_motion_scale = std::clamp<float>(
            std::wcstof(motion_scale_buf, nullptr), 0.0f, 4.0f);
    }
    g_config.ffx12_reuse_same_generation =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12ReuseSameGeneration", 0,
                              config_path.c_str()) != 0;
    g_config.ffx12_output_mark =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12OutputMark", 0, config_path.c_str()) != 0;
    ffx12::set_output_mark(g_config.ffx12_output_mark);
    {
        wchar_t fov_buf[32] {};
        GetPrivateProfileStringW(L"Dx11FsrBridge", L"Ffx12FovScale", L"1.0", fov_buf,
                                 static_cast<DWORD>(std::size(fov_buf)), config_path.c_str());
        g_config.ffx12_fov_scale = std::clamp<float>(std::wcstof(fov_buf, nullptr), 0.1f, 3.0f);
    }
    {
        wchar_t near_buf[32] {}, far_buf[32] {};
        GetPrivateProfileStringW(L"Dx11FsrBridge", L"Ffx12CameraNear", L"0.25", near_buf,
                                 static_cast<DWORD>(std::size(near_buf)), config_path.c_str());
        GetPrivateProfileStringW(L"Dx11FsrBridge", L"Ffx12CameraFar", L"6000.0", far_buf,
                                 static_cast<DWORD>(std::size(far_buf)), config_path.c_str());
        g_config.ffx12_camera_near = std::clamp<float>(std::wcstof(near_buf, nullptr), 0.001f, 100.0f);
        g_config.ffx12_camera_far = std::clamp<float>(std::wcstof(far_buf, nullptr), 10.0f, 100000.0f);
    }
    ffx12::set_motion_decode_test(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12DecodeTest", 0, config_path.c_str()) != 0);
    ffx12::set_decode_test(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12DecodeTest", 0, config_path.c_str()) != 0);
    ffx12::set_motion_decode_test(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12MotionDecodeTest", 0, config_path.c_str()) != 0);
    ffx12::set_motion_deadzone(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12MotionDeadzone", 0, config_path.c_str()) != 0);
    ffx12::set_debug_layer(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12DebugLayer", 0, config_path.c_str()) != 0);
    ffx12::set_depth_inverted(g_config.ffx12_depth_inverted);
    ffx12::set_decode_motion(g_config.ffx12_decode_motion);
    ffx12::set_motion_vectors_jittered(g_config.fsr2_motion_vectors_jittered);
    g_config.ffx12_gpu_interop =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12GpuInterop", 1, config_path.c_str()) != 0;
    ffx12::set_gpu_interop(g_config.ffx12_gpu_interop);
    ffx12::set_hdr_input(g_config.ffx12_hdr_input);
    ffx12::set_auto_exposure(g_config.ffx12_auto_exposure);
    ffx12::set_non_linear(g_config.ffx12_non_linear);
    ffx12::set_velocity_factor(g_config.ffx12_velocity_factor);
    ffx12::set_dump_frames(static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12DumpFrames", 0, config_path.c_str())));
    {
        wchar_t ver_buf[32] {};
        // 默认请求最高版本前缀（4.x）——provider 的 GET_VERSIONS 按 GPU
        // 能力过滤，降级链 4→3→2 自动兜底；无需按显卡分类强制降级。
        GetPrivateProfileStringW(L"Dx11FsrBridge", L"Ffx12Version", L"ffx12-fsr4.x", ver_buf,
                                 static_cast<DWORD>(std::size(ver_buf)), config_path.c_str());
        ffx12::set_sdk_version(narrow(ver_buf).c_str());
    }
    // 诊断 shader dump 也必须在 RELEASE 分支读取（非 RELEASE 分支的读取不生效）
    g_config.dump_pixel_shaders = GetPrivateProfileIntW(L"Dx11FsrBridge", L"DumpPixelShaders", 0, config_path.c_str()) != 0;
    {
        wchar_t buf[520] {};
        GetPrivateProfileStringW(L"Dx11FsrBridge", L"Ffx12DllPath", L"", buf,
                                 static_cast<DWORD>(std::size(buf)), config_path.c_str());
        g_config.ffx12_dll_path = buf;
        if (g_config.ffx12_dll_path.empty())
            // 默认指向 Bridge 目录自身副本（不再依赖 OptiScaler 目录布局——
            // OptiScaler 更新后其 SDK DLL 移入子目录，外路径失配导致 LoadLibrary 失败 → 黑屏）。
            // 正式版 SDK 统一放上一级 payload\AMD（Bridge 目录只留桥本体）。
            g_config.ffx12_dll_path = g_module_dir.parent_path() / L"AMD" / L"amd_fidelityfx_upscaler_dx12.dll";
        ffx12::set_sdk_dll_path(g_config.ffx12_dll_path.c_str());
    }
    g_config.ffx12_fail_closed =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12FailClosed", 0, config_path.c_str()) != 0;
    g_config.ffx12_full_logging =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12FullLogging", 0, config_path.c_str()) != 0;
#endif
#else
    g_config.enabled = GetPrivateProfileIntW(L"Dx11FsrBridge", L"Enabled", 1, config_path.c_str()) != 0;
    g_config.enable_logging = GetPrivateProfileIntW(L"Dx11FsrBridge", L"EnableLogging", 0, config_path.c_str()) != 0;
    g_logging_enabled.store(g_config.enable_logging, std::memory_order_relaxed);
    g_config.target_process_id = GetPrivateProfileIntW(L"Dx11FsrBridge", L"TargetProcessId", 0, config_path.c_str());
    wchar_t name_buffer[260] {};
    GetPrivateProfileStringW(L"Dx11FsrBridge", L"TargetProcessName", L"", name_buffer, static_cast<DWORD>(std::size(name_buffer)), config_path.c_str());
    g_config.target_process_name = name_buffer;
    g_config.log_all_dispatch = GetPrivateProfileIntW(L"Dx11FsrBridge", L"LogAllDispatch", 0, config_path.c_str()) != 0;
    g_config.log_resource_ops = GetPrivateProfileIntW(L"Dx11FsrBridge", L"LogResourceOps", 0, config_path.c_str()) != 0;
    g_config.log_loader_activity = GetPrivateProfileIntW(L"Dx11FsrBridge", L"LogLoaderActivity", 0, config_path.c_str()) != 0;
    g_config.log_interesting_dispatch_details = GetPrivateProfileIntW(L"Dx11FsrBridge", L"LogInterestingDispatchDetails", 0, config_path.c_str()) != 0;
    g_config.hook_present = GetPrivateProfileIntW(L"Dx11FsrBridge", L"HookPresent", 0, config_path.c_str()) != 0;
    g_config.final_scene_probe = GetPrivateProfileIntW(L"Dx11FsrBridge", L"FinalSceneProbe", 0, config_path.c_str()) != 0;
    g_config.final_scene_probe_limit = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(GetPrivateProfileIntW(L"Dx11FsrBridge", L"FinalSceneProbeLimit", 6, config_path.c_str())),
        1u,
        16u);
    g_config.final_scene_probe_signature_limit = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(GetPrivateProfileIntW(L"Dx11FsrBridge", L"FinalSceneProbeSignatureLimit", 128, config_path.c_str())),
        1u,
        4096u);
    g_config.final_scene_snapshot =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"FinalSceneSnapshot", 0, config_path.c_str()) != 0;
    g_config.final_scene_snapshot_interval_frames = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(GetPrivateProfileIntW(
            L"Dx11FsrBridge", L"FinalSceneSnapshotIntervalFrames", 240, config_path.c_str())),
        1u,
        3600u);
    g_config.final_scene_optifg_input =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"FinalSceneOptiFgInput", 0, config_path.c_str()) != 0;
    g_config.dlssg_dxgi_workaround = GetPrivateProfileIntW(L"Dx11FsrBridge", L"DlssgDxgiWorkaround", -1, config_path.c_str());
    g_config.hdr_swapchain_spoof =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"HdrSwapchainSpoof", 0, config_path.c_str()) != 0;
    g_config.hdr_swapchain_force =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"HdrSwapchainForce", 0, config_path.c_str()) != 0;
    g_config.hdr_environment_probe =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"HdrEnvironmentProbe", 0, config_path.c_str()) != 0;
    g_config.hdr_output_desc_probe =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"HdrOutputDescProbe", 0, config_path.c_str()) != 0;
    g_config.hdr_output_desc_spoof =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"HdrOutputDescSpoof", 0, config_path.c_str()) != 0;
    g_config.native_ldr_swapchain_unorm =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"NativeLdrSwapchainUnorm", 0, config_path.c_str()) != 0;
    g_config.dx11_on12_swapchain =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Dx11On12Swapchain", 0, config_path.c_str()) != 0;
    g_config.native_ldr_final_target_unorm =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"NativeLdrFinalTargetUnorm", 0, config_path.c_str()) != 0;
    g_config.hdr_sdr_tone_map =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"HdrSdrToneMap", 0, config_path.c_str()) != 0;
    g_config.hdr_sdr_tone_map_pq_input =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"HdrSdrToneMapPqInput", 1, config_path.c_str()) != 0;
    g_config.hdr_sdr_tone_map_paper_white = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(GetPrivateProfileIntW(
            L"Dx11FsrBridge", L"HdrSdrToneMapPaperWhite", 80, config_path.c_str())),
        1u,
        1000u);
    g_config.hdr_sdr_tone_map_peak = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(GetPrivateProfileIntW(
            L"Dx11FsrBridge", L"HdrSdrToneMapPeak", 100, config_path.c_str())),
        1u,
        10000u);
    g_config.hdr_composite_probe =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"HdrCompositeProbe", 0, config_path.c_str()) != 0;
    g_config.hdr_composite_probe_limit = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(GetPrivateProfileIntW(
            L"Dx11FsrBridge", L"HdrCompositeProbeLimit", 32, config_path.c_str())),
        1u,
        512u);
    g_config.capture_metadata_only = GetPrivateProfileIntW(L"Dx11FsrBridge", L"CaptureMetadataOnly", 1, config_path.c_str()) != 0;
    g_config.dump_compute_shaders = GetPrivateProfileIntW(L"Dx11FsrBridge", L"DumpComputeShaders", 0, config_path.c_str()) != 0;
    g_config.dump_pixel_shaders = GetPrivateProfileIntW(L"Dx11FsrBridge", L"DumpPixelShaders", 0, config_path.c_str()) != 0;
    g_config.trace_pixel_shader_draws = GetPrivateProfileIntW(L"Dx11FsrBridge", L"TracePixelShaderDraws", 0, config_path.c_str()) != 0;
    g_config.trace_texture_creates = GetPrivateProfileIntW(L"Dx11FsrBridge", L"TraceTextureCreates", 0, config_path.c_str()) != 0;
    g_config.texture_trace_hotkey = static_cast<std::uint32_t>(GetPrivateProfileIntW(L"Dx11FsrBridge", L"TextureTraceHotkey", VK_F11, config_path.c_str()));
    g_config.texture_trace_duration_ms = std::max<std::uint32_t>(1000u,
        static_cast<std::uint32_t>(GetPrivateProfileIntW(L"Dx11FsrBridge", L"TextureTraceDurationMs", 10000, config_path.c_str())));
    g_config.texture_trace_limit = std::max<std::uint32_t>(1u,
        static_cast<std::uint32_t>(GetPrivateProfileIntW(L"Dx11FsrBridge", L"TextureTraceLimit", 128, config_path.c_str())));
    wchar_t trace_hash_buffer[64] {};
    GetPrivateProfileStringW(L"Dx11FsrBridge", L"TracePixelShaderHash", L"78057A29AF6C2D99", trace_hash_buffer, static_cast<DWORD>(std::size(trace_hash_buffer)), config_path.c_str());
    wchar_t *trace_hash_end = nullptr;
    g_config.trace_pixel_shader_hash = std::wcstoull(trace_hash_buffer, &trace_hash_end, 16);
    g_config.pixel_shader_trace_limit = static_cast<std::uint32_t>(GetPrivateProfileIntW(L"Dx11FsrBridge", L"PixelShaderTraceLimit", 512, config_path.c_str()));
    wchar_t target_hash_buffer[64] {};
    GetPrivateProfileStringW(L"Dx11FsrBridge", L"TargetPixelShaderHash", L"78057A29AF6C2D99", target_hash_buffer, static_cast<DWORD>(std::size(target_hash_buffer)), config_path.c_str());
    wchar_t *target_hash_end = nullptr;
    g_config.target_pixel_shader_hash = std::wcstoull(target_hash_buffer, &target_hash_end, 16);
    g_config.pixel_shader_replacement_mode = static_cast<std::uint32_t>(GetPrivateProfileIntW(L"Dx11FsrBridge", L"PixelShaderReplacementMode", 0, config_path.c_str()));
#if defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
    g_config.enable_fsr2_get_proc_address_shim = GetPrivateProfileIntW(L"Dx11FsrBridge", L"EnableFsr2GetProcAddressShim", 0, config_path.c_str()) != 0;
    g_config.fsr2_translation_mode = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2TranslationMode", 0, config_path.c_str()));
    g_config.fsr2_fast_state_tracking =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2FastStateTracking", 0, config_path.c_str()) != 0;
    g_config.fsr2_mode2_on_demand_state =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2Mode2OnDemandState", 1, config_path.c_str()) != 0;
    g_config.fsr2_output_validation_target = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2OutputValidationTarget", 0, config_path.c_str()));
    g_config.fsr2_motion_vectors_jittered =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2MotionVectorsJittered", 0, config_path.c_str()) != 0;
    g_config.fsr2_positive_motion_vector_scale =
        // 同上：+renderSize 为游戏原生运动约定下的正确符号（见 sdk234 配置块注释）。
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2MotionVectorScaleMode", 1, config_path.c_str()) == 1;
    g_config.fsr2_use_reactive_mask =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2UseReactiveMask", 0, config_path.c_str()) != 0;
    g_config.fsr2_use_transparency_mask =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2UseTransparencyMask", 0, config_path.c_str()) != 0;
    g_config.fsr2_jitter_mode = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2JitterMode", 0, config_path.c_str()));
    g_config.fsr2_dump_input_textures = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2DumpInputTextures", 0, config_path.c_str()));
    g_config.fsr2_compare_output_capture =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2CompareOutputCapture", 0, config_path.c_str()) != 0;
    g_config.fsr2_sharpness_percent = std::min<std::uint32_t>(
        100u,
        static_cast<std::uint32_t>(GetPrivateProfileIntW(
            L"Dx11FsrBridge", L"Fsr2SharpnessPercent", 0, config_path.c_str())));
    g_config.fsr2_hdr10_pq_color =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2Hdr10PqColor", 0, config_path.c_str()) != 0;
    g_config.fsr2_use_native_exposure =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2UseNativeExposure", 1, config_path.c_str()) != 0;
    g_config.fsr2_fast_metadata_copy =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2FastMetadataCopy", 0, config_path.c_str()) != 0;
    g_config.fsr2_compact_linear_output =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2CompactLinearOutput", 0, config_path.c_str()) != 0;
    g_config.fsr2_lock_color_producer_shader =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2LockColorProducerShader", 1, config_path.c_str()) != 0;
    g_config.fsr2_gpu_timing =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2GpuTiming", 0, config_path.c_str()) != 0;
    g_config.fsr2_reset_on_color_path_change =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2ResetOnColorPathChange", 0, config_path.c_str()) != 0;
    g_config.fsr2_reset_on_optiscaler_config_change =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2ResetOnOptiScalerConfigChange", 0, config_path.c_str()) != 0;
    g_config.fsr2_optiscaler_config_reset_frames = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(GetPrivateProfileIntW(
            L"Dx11FsrBridge", L"Fsr2OptiScalerConfigResetFrames", 4, config_path.c_str())),
        1u,
        16u);
    g_config.fsr2_reset_on_optiscaler_log_change =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2ResetOnOptiScalerLogChange", 0, config_path.c_str()) != 0;
    g_config.fsr2_optiscaler_log_reset_duration_ms = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(GetPrivateProfileIntW(
            L"Dx11FsrBridge", L"Fsr2OptiScalerLogResetDurationMs", 4000, config_path.c_str())),
        250u,
        10000u);
    g_config.fsr2_auto_recover_upscaler_ms = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2AutoRecoverUpscalerMs", 0, config_path.c_str()));
    g_config.fsr2_trace_color_producers =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2TraceColorProducers", 0, config_path.c_str()) != 0;
    g_config.fsr2_early_output_probe =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2EarlyOutputProbe", 0, config_path.c_str()) != 0;
    g_config.fsr2_early_output_probe_frames = static_cast<std::uint32_t>(std::max<UINT>(
        1u,
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2EarlyOutputProbeFrames", 60, config_path.c_str())));
    g_config.block_dx11_on12_upscalers =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"BlockDx11On12Upscalers", 1, config_path.c_str()) != 0;
#endif
    g_config.show_osd = GetPrivateProfileIntW(L"Dx11FsrBridge", L"ShowOSD", 0, config_path.c_str()) != 0;
    g_config.assume_phase_order = GetPrivateProfileIntW(L"Dx11FsrBridge", L"AssumePhaseOrder", 0, config_path.c_str()) != 0;
    g_config.enable_similarity_probe = GetPrivateProfileIntW(L"Dx11FsrBridge", L"EnableSimilarityProbe", 0, config_path.c_str()) != 0;
    g_config.reset_similarity_on_recording = GetPrivateProfileIntW(L"Dx11FsrBridge", L"ResetSimilarityOnRecording", 1, config_path.c_str()) != 0;
    g_config.candidate_limit_per_frame = static_cast<std::uint32_t>(GetPrivateProfileIntW(L"Dx11FsrBridge", L"CandidateLimitPerFrame", 64, config_path.c_str()));
    g_config.interesting_dispatch_log_limit = static_cast<std::uint32_t>(GetPrivateProfileIntW(L"Dx11FsrBridge", L"InterestingDispatchLogLimit", 256, config_path.c_str()));
    g_config.interesting_dispatch_phase_gap_ms = static_cast<std::uint32_t>(GetPrivateProfileIntW(L"Dx11FsrBridge", L"InterestingDispatchPhaseGapMs", 1500, config_path.c_str()));
    g_config.similarity_report_interval_ms = static_cast<std::uint32_t>(GetPrivateProfileIntW(L"Dx11FsrBridge", L"SimilarityReportIntervalMs", 2000, config_path.c_str()));
    wchar_t label_buffer[128] {};
    GetPrivateProfileStringW(L"Dx11FsrBridge", L"RunLabel", L"", label_buffer, static_cast<DWORD>(std::size(label_buffer)), config_path.c_str());
    g_config.run_label = label_buffer;
#endif
}

bool process_matches()
{
    if (!g_config.enabled)
        return false;
    if (g_config.target_process_id != 0 && GetCurrentProcessId() != g_config.target_process_id)
        return false;
    if (!g_config.target_process_name.empty())
    {
        std::wstring current = current_process_name();
        if (_wcsicmp(current.c_str(), g_config.target_process_name.c_str()) != 0)
            return false;
    }
    return true;
}

bool read_resource_info(ID3D11View *view, const wchar_t *kind, ResourceInfo &out_info); // 定义见下（cached 版本在其后调用）

bool read_resource_info(ID3D11View *view, const wchar_t *kind, ResourceInfo &out_info)
{
    out_info = {};
    if (view == nullptr)
        return false;

    ID3D11Resource *resource = nullptr;
    view->GetResource(&resource);
    if (resource == nullptr)
        return false;

    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    resource->GetType(&dimension);
    if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
    {
        ID3D11Texture2D *texture = nullptr;
        if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&texture))) && texture != nullptr)
        {
            D3D11_TEXTURE2D_DESC desc {};
            texture->GetDesc(&desc);
            out_info.resource_key = reinterpret_cast<std::uint64_t>(resource);
            out_info.width = desc.Width;
            out_info.height = desc.Height;
            out_info.format = desc.Format;
            out_info.bind_flags = desc.BindFlags;
            out_info.misc_flags = desc.MiscFlags;

            ID3D11ShaderResourceView *srv = nullptr;
            if (SUCCEEDED(view->QueryInterface(__uuidof(ID3D11ShaderResourceView), reinterpret_cast<void **>(&srv))) && srv != nullptr)
            {
                D3D11_SHADER_RESOURCE_VIEW_DESC view_desc {};
                srv->GetDesc(&view_desc);
                out_info.view_format = view_desc.Format;
                srv->Release();
            }
            else
            {
                ID3D11RenderTargetView *rtv = nullptr;
                if (SUCCEEDED(view->QueryInterface(__uuidof(ID3D11RenderTargetView), reinterpret_cast<void **>(&rtv))) && rtv != nullptr)
                {
                    D3D11_RENDER_TARGET_VIEW_DESC view_desc {};
                    rtv->GetDesc(&view_desc);
                    out_info.view_format = view_desc.Format;
                    rtv->Release();
                }
            }
            texture->Release();
            resource->Release();
            return true;
        }
    }

    resource->Release();
    return false;
}

bool read_resource_info_from_resource(ID3D11Resource *resource, const wchar_t *kind, ResourceInfo &out_info)
{
    out_info = {};
    if (resource == nullptr)
        return false;

    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    resource->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        return false;

    ID3D11Texture2D *texture = nullptr;
    if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&texture))) || texture == nullptr)
        return false;

    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    out_info.resource_key = reinterpret_cast<std::uint64_t>(resource);
    out_info.width = desc.Width;
    out_info.height = desc.Height;
    out_info.format = desc.Format;
    texture->Release();
    return true;
}

#if defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
std::uint32_t format_bytes_per_pixel(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
        return 8;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
        return 16;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        return 8;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
        return 4;
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT:
        return 2;
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
        return 1;
    default:
        return 0;
    }
}

bool dump_fsr2_input_texture(
    ID3D11DeviceContext *context,
    ID3D11ShaderResourceView *view,
    UINT slot,
    const std::wstring &file_stem = {})
{
    if (context == nullptr || view == nullptr)
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC view_desc {};
    view->GetDesc(&view_desc);
    if (view_desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D)
    {
        log_line("fsr2_input_dump_unsupported slot=" + std::to_string(slot) +
            " view_dimension=" + std::to_string(static_cast<std::uint32_t>(view_desc.ViewDimension)));
        return false;
    }

    ID3D11Resource *resource = nullptr;
    view->GetResource(&resource);
    if (resource == nullptr)
        return false;

    ID3D11Texture2D *texture = nullptr;
    const HRESULT query_result = resource->QueryInterface(
        __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&texture));
    resource->Release();
    if (FAILED(query_result) || texture == nullptr)
        return false;

    D3D11_TEXTURE2D_DESC source_desc {};
    texture->GetDesc(&source_desc);
    const UINT source_mip = view_desc.Texture2D.MostDetailedMip;
    const std::uint32_t width = std::max(1u, source_desc.Width >> source_mip);
    const std::uint32_t height = std::max(1u, source_desc.Height >> source_mip);
    const std::uint32_t bytes_per_pixel = format_bytes_per_pixel(source_desc.Format);
    if (bytes_per_pixel == 0 || source_desc.SampleDesc.Count != 1)
    {
        log_line("fsr2_input_dump_unsupported slot=" + std::to_string(slot) +
            " resource_format=" + std::to_string(static_cast<std::uint32_t>(source_desc.Format)) +
            " view_format=" + std::to_string(static_cast<std::uint32_t>(view_desc.Format)) +
            " samples=" + std::to_string(source_desc.SampleDesc.Count));
        texture->Release();
        return false;
    }

    D3D11_TEXTURE2D_DESC staging_desc {};
    staging_desc.Width = width;
    staging_desc.Height = height;
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.Format = source_desc.Format;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ID3D11Device *device = nullptr;
    context->GetDevice(&device);
    ID3D11Texture2D *staging = nullptr;
    HRESULT result = device != nullptr ? device->CreateTexture2D(&staging_desc, nullptr, &staging) : E_POINTER;
    if (device != nullptr)
        device->Release();
    if (FAILED(result) || staging == nullptr)
    {
        log_line("fsr2_input_dump_create_failed slot=" + std::to_string(slot) +
            " hr=" + std::to_string(static_cast<long>(result)));
        texture->Release();
        return false;
    }

    const UINT source_subresource = D3D11CalcSubresource(source_mip, 0, source_desc.MipLevels);
    context->CopySubresourceRegion(staging, 0, 0, 0, 0, texture, source_subresource, nullptr);
    texture->Release();

    D3D11_MAPPED_SUBRESOURCE mapped {};
    result = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result))
    {
        log_line("fsr2_input_dump_map_failed slot=" + std::to_string(slot) +
            " hr=" + std::to_string(static_cast<long>(result)));
        staging->Release();
        return false;
    }

    const std::filesystem::path dump_dir = g_module_dir / L"Dx11FsrBridge.inputs";
    std::error_code directory_error;
    std::filesystem::create_directories(dump_dir, directory_error);
    const std::wstring output_stem = file_stem.empty()
        ? L"t" + std::to_wstring(slot)
        : file_stem;
    const std::filesystem::path raw_path = dump_dir / (output_stem + L".raw");
    std::ofstream raw(raw_path, std::ios::binary | std::ios::trunc);
    const std::size_t row_bytes = static_cast<std::size_t>(width) * bytes_per_pixel;
    if (raw)
    {
        for (std::uint32_t y = 0; y < height; ++y)
        {
            const auto *row = static_cast<const std::uint8_t *>(mapped.pData) +
                static_cast<std::size_t>(y) * mapped.RowPitch;
            raw.write(reinterpret_cast<const char *>(row), row_bytes);
        }
    }
    context->Unmap(staging, 0);
    staging->Release();

    const std::filesystem::path json_path = dump_dir / (output_stem + L".json");
    std::ofstream metadata(json_path, std::ios::trunc);
    if (metadata)
    {
        metadata << "{\"name\":\"" << narrow(output_stem) << "\""
            << ",\"slot\":" << slot
            << ",\"width\":" << width
            << ",\"height\":" << height
            << ",\"resource_format\":" << static_cast<std::uint32_t>(source_desc.Format)
            << ",\"view_format\":" << static_cast<std::uint32_t>(view_desc.Format)
            << ",\"bytes_per_pixel\":" << bytes_per_pixel
            << ",\"row_bytes\":" << row_bytes << "}";
    }

    log_line("fsr2_input_dumped slot=" + std::to_string(slot) +
        " name=" + narrow(output_stem) +
        " size=" + std::to_string(width) + "x" + std::to_string(height) +
        " resource_format=" + std::to_string(static_cast<std::uint32_t>(source_desc.Format)) +
        " view_format=" + std::to_string(static_cast<std::uint32_t>(view_desc.Format)) +
        " bytes_per_pixel=" + std::to_string(bytes_per_pixel));
    return raw.good();
}

bool dump_fsr2_render_target_texture(
    ID3D11DeviceContext *context,
    ID3D11RenderTargetView *render_target,
    UINT slot,
    const std::wstring &file_stem = {})
{
    if (context == nullptr || render_target == nullptr)
        return false;

    D3D11_RENDER_TARGET_VIEW_DESC render_target_desc {};
    render_target->GetDesc(&render_target_desc);
    if (render_target_desc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D)
    {
        log_line("fsr2_output_dump_unsupported slot=" + std::to_string(slot) +
            " view_dimension=" +
            std::to_string(static_cast<std::uint32_t>(render_target_desc.ViewDimension)));
        return false;
    }

    ID3D11Resource *resource = nullptr;
    render_target->GetResource(&resource);
    if (resource == nullptr)
        return false;

    ID3D11Texture2D *texture = nullptr;
    const HRESULT query_result = resource->QueryInterface(
        __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&texture));
    resource->Release();
    if (FAILED(query_result) || texture == nullptr)
        return false;

    D3D11_TEXTURE2D_DESC texture_desc {};
    texture->GetDesc(&texture_desc);
    D3D11_SHADER_RESOURCE_VIEW_DESC shader_resource_desc {};
    shader_resource_desc.Format = render_target_desc.Format;
    shader_resource_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    shader_resource_desc.Texture2D.MostDetailedMip = render_target_desc.Texture2D.MipSlice;
    shader_resource_desc.Texture2D.MipLevels = 1;

    ID3D11Device *device = nullptr;
    context->GetDevice(&device);
    ID3D11ShaderResourceView *view = nullptr;
    const HRESULT create_result = device != nullptr
        ? device->CreateShaderResourceView(texture, &shader_resource_desc, &view)
        : E_POINTER;
    if (device != nullptr)
        device->Release();
    texture->Release();
    if (FAILED(create_result) || view == nullptr)
    {
        log_line("fsr2_output_dump_view_failed slot=" + std::to_string(slot) +
            " hr=" + std::to_string(static_cast<long>(create_result)));
        return false;
    }

    const bool dumped = dump_fsr2_input_texture(context, view, slot, file_stem);
    view->Release();
    return dumped;
}

bool write_fsr2_constant_buffer_dump(
    const std::uint8_t *data,
    std::size_t size,
    std::uint64_t resource_key,
    const char *source)
{
    const std::filesystem::path dump_dir = g_module_dir / L"Dx11FsrBridge.inputs";
    std::error_code directory_error;
    std::filesystem::create_directories(dump_dir, directory_error);

    const std::filesystem::path raw_path = dump_dir / L"cb0.raw";
    std::ofstream raw(raw_path, std::ios::binary | std::ios::trunc);
    if (raw)
        raw.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));

    const std::filesystem::path json_path = dump_dir / L"cb0.json";
    std::ofstream metadata(json_path, std::ios::trunc);
    if (metadata)
    {
        metadata << "{\"byte_count\":" << size
            << ",\"float_count\":" << size / sizeof(float)
            << ",\"source\":\"" << source << "\"}";
    }

    log_line("fsr2_input_dumped_cb0 bytes=" + std::to_string(size) +
        " resource=" + hex64(resource_key) + " source=" + source);
    return raw.good();
}

bool dump_fsr2_constant_buffer(std::uint64_t resource_key)
{
    const std::vector<std::uint8_t> snapshot = lookup_buffer_snapshot(resource_key);
    if (snapshot.empty())
    {
        log_line("fsr2_input_dump_cb0_missing resource=" + hex64(resource_key));
        return false;
    }
    return write_fsr2_constant_buffer_dump(
        snapshot.data(), snapshot.size(), resource_key, "cpu_snapshot");
}

bool dump_fsr2_bound_constant_buffer(
    ID3D11DeviceContext *context,
    ID3D11Buffer *constant_buffer)
{
    if (context == nullptr || constant_buffer == nullptr)
        return false;

    D3D11_BUFFER_DESC source_desc {};
    constant_buffer->GetDesc(&source_desc);

    D3D11_BUFFER_DESC staging_desc {};
    staging_desc.ByteWidth = source_desc.ByteWidth;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ID3D11Device *device = nullptr;
    context->GetDevice(&device);
    ID3D11Buffer *staging = nullptr;
    HRESULT result = device != nullptr ? device->CreateBuffer(&staging_desc, nullptr, &staging) : E_POINTER;
    if (device != nullptr)
        device->Release();
    if (FAILED(result) || staging == nullptr)
    {
        log_line("fsr2_input_dump_cb0_create_failed bytes=" + std::to_string(source_desc.ByteWidth) +
            " hr=" + std::to_string(static_cast<long>(result)));
        return false;
    }

    context->CopyResource(staging, constant_buffer);
    D3D11_MAPPED_SUBRESOURCE mapped {};
    result = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result))
    {
        log_line("fsr2_input_dump_cb0_map_failed bytes=" + std::to_string(source_desc.ByteWidth) +
            " hr=" + std::to_string(static_cast<long>(result)));
        staging->Release();
        return false;
    }

    const std::uint64_t resource_key = reinterpret_cast<std::uint64_t>(constant_buffer);
    const bool dumped = write_fsr2_constant_buffer_dump(
        static_cast<const std::uint8_t *>(mapped.pData),
        source_desc.ByteWidth,
        resource_key,
        "gpu_readback");
    context->Unmap(staging, 0);
    staging->Release();
    return dumped;
}

void maybe_dump_color_candidate_inputs(ID3D11DeviceContext *context, UINT element_count)
{
    if (context == nullptr || element_count != 3 || (GetAsyncKeyState(VK_F6) & 0x8000) == 0)
    {
        return;
    }
    const std::optional<Fsr2DynamicColorTarget> target = match_fsr2_dynamic_color_producer();
    if (!target ||
        g_fsr2_candidate_producer_output_resource.load(std::memory_order_acquire) != target->resource_key ||
        g_fsr2_color_candidate_dumped.exchange(true, std::memory_order_relaxed))
    {
        return;
    }

    std::array<ID3D11ShaderResourceView *, 7> views {};
    context->PSGetShaderResources(0, static_cast<UINT>(views.size()), views.data());
    ResourceInfo candidate_color {};
    read_resource_info(views[0], L"fsr2_color_candidate", candidate_color);
    if (views[0] != nullptr)
    {
        views[0]->AddRef();
        std::lock_guard lock(g_fsr2_candidate_color_view_mutex);
        if (g_fsr2_candidate_color_view != nullptr)
            g_fsr2_candidate_color_view->Release();
        g_fsr2_candidate_color_view = views[0];
    }
    for (UINT slot = 0; slot < views.size(); ++slot)
    {
        dump_fsr2_input_texture(context, views[slot], slot);
        if (views[slot] != nullptr)
            views[slot]->Release();
    }

    ID3D11Buffer *constant_buffer = nullptr;
    context->PSGetConstantBuffers(0, 1, &constant_buffer);
    if (constant_buffer != nullptr)
    {
        if (!dump_fsr2_bound_constant_buffer(context, constant_buffer))
        {
            const BufferInfo info = lookup_buffer_info(constant_buffer);
            dump_fsr2_constant_buffer(info.resource_key);
        }
        constant_buffer->Release();
    }
    else
    {
        log_line("fsr2_input_dump_cb0_unbound");
    }
    g_fsr2_candidate_color_resource.store(candidate_color.resource_key, std::memory_order_relaxed);
    g_fsr2_candidate_sequence.store(g_color_source_sequence.load(std::memory_order_relaxed), std::memory_order_relaxed);
    g_fsr2_same_frame_capture_pending.store(true, std::memory_order_release);
    g_fsr2_early_output_probe_frames_remaining.store(
        g_config.fsr2_early_output_probe ? g_config.fsr2_early_output_probe_frames : 0,
        std::memory_order_release);
    maybe_dump_color_source_history(context, candidate_color.resource_key);
    log_line("fsr2_color_candidate_dumped shader=" +
        hex64(g_current_ps_hash.load(std::memory_order_relaxed)) +
        " output=" + hex64(target->resource_key));
}
#endif

template <typename ViewType>
void update_view_array(std::array<ResourceInfo, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> &target, UINT start_slot, UINT count, ViewType *const *views, const wchar_t *kind)
{
    for (UINT i = 0; i < count && (start_slot + i) < target.size(); ++i)
    {
        ResourceInfo info {};
        if (views != nullptr)
            read_resource_info(views[i], kind, info);
        target[start_slot + i] = info;
    }
}

void update_uav_array(std::array<ResourceInfo, D3D11_1_UAV_SLOT_COUNT> &target, UINT start_slot, UINT count, ID3D11UnorderedAccessView *const *views)
{
    for (UINT i = 0; i < count && (start_slot + i) < target.size(); ++i)
    {
        ResourceInfo info {};
        if (views != nullptr)
            read_resource_info(views[i], L"uav", info);
        target[start_slot + i] = info;
    }
}

void write_candidate_packet(const OptiScalerBridgePacket &packet, UINT group_x, UINT group_y, UINT group_z)
{
    std::ofstream out(g_frames_path, std::ios::app);
    out << "{";
    out << "\"frame\":" << packet.frame_index << ",";
    out << "\"path\":\"" << narrow(packet.path) << "\",";
    out << "\"dispatch\":[" << group_x << "," << group_y << "," << group_z << "],";
    out << "\"render_size\":[" << packet.render_width << "," << packet.render_height << "],";
    out << "\"output_size\":[" << packet.output_width << "," << packet.output_height << "],";
    out << "\"compute_shader\":\"" << hex64(packet.compute_shader) << "\",";
    out << "\"color\":{\"w\":" << packet.color.width << ",\"h\":" << packet.color.height << ",\"fmt\":" << static_cast<std::uint32_t>(packet.color.format) << "},";
    out << "\"motion\":{\"w\":" << packet.motion.width << ",\"h\":" << packet.motion.height << ",\"fmt\":" << static_cast<std::uint32_t>(packet.motion.format) << "},";
    out << "\"depth\":{\"w\":" << packet.depth.width << ",\"h\":" << packet.depth.height << ",\"fmt\":" << static_cast<std::uint32_t>(packet.depth.format) << "},";
    out << "\"output\":{\"w\":" << packet.output.width << ",\"h\":" << packet.output.height << ",\"fmt\":" << static_cast<std::uint32_t>(packet.output.format) << "}";
    out << "}\n";
}

std::optional<OptiScalerBridgePacket> build_dispatch_candidate(UINT group_x, UINT group_y, UINT group_z)
{
    if (!g_config.optiscaler_bridge_probe)
        return std::nullopt;
    std::lock_guard lock(g_state_mutex);

    if (g_state.candidate_count >= g_config.candidate_limit_per_frame)
        return std::nullopt;
    if (g_state.backbuffer_width == 0 || g_state.backbuffer_height == 0)
        return std::nullopt;

    OptiScalerBridgePacket packet {};
    packet.frame_index = g_state.frame_index;
    packet.output_width = g_state.backbuffer_width;
    packet.output_height = g_state.backbuffer_height;
    packet.compute_shader = g_state.current_cs_shader;
    packet.compute_shader_hash = g_state.current_cs_hash;

    for (const ResourceInfo &uav : g_state.cs_uavs)
    {
        if (uav.width == g_state.backbuffer_width && uav.height == g_state.backbuffer_height)
        {
            packet.output = uav;
            break;
        }
    }

    for (const ResourceInfo &srv : g_state.cs_srvs)
    {
        if (srv.width == 0 || srv.height == 0)
            continue;
        if (packet.render_width == 0 && srv.width <= g_state.backbuffer_width && srv.height <= g_state.backbuffer_height)
        {
            packet.render_width = srv.width;
            packet.render_height = srv.height;
            packet.color = srv;
            continue;
        }

        const auto fmt = static_cast<std::uint32_t>(srv.format);
        if (packet.motion.width == 0 && (fmt == DXGI_FORMAT_R8G8_UNORM || fmt == DXGI_FORMAT_R16G16_FLOAT))
        {
            packet.motion = srv;
            continue;
        }

        if (packet.depth.width == 0 && (fmt == DXGI_FORMAT_R32_FLOAT || fmt == DXGI_FORMAT_R24_UNORM_X8_TYPELESS || fmt == DXGI_FORMAT_R16_UNORM))
            packet.depth = srv;
    }

    if (packet.output.width == 0 || packet.color.width == 0)
        return std::nullopt;
    if (packet.output.width == packet.color.width && packet.output.height == packet.color.height)
        return std::nullopt;

    g_state.candidate_count++;
    write_candidate_packet(packet, group_x, group_y, group_z);
    return packet;
}

bool hook_iat_unchecked(HMODULE module, const char *import_name, const char *function_name, void *replacement, void **original)
{
    const auto *base = reinterpret_cast<const std::uint8_t *>(module);
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        return false;

    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;
    const std::size_t image_size = nt->OptionalHeader.SizeOfImage;
    const std::size_t nt_offset = static_cast<std::size_t>(dos->e_lfanew);
    if (image_size < sizeof(IMAGE_DOS_HEADER) || nt_offset > image_size - sizeof(IMAGE_NT_HEADERS))
        return false;

    const IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0 || dir.VirtualAddress >= image_size ||
        dir.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR) || dir.Size > image_size - dir.VirtualAddress)
        return false;

    auto *descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(const_cast<std::uint8_t *>(base) + dir.VirtualAddress);
    const std::size_t descriptor_count = dir.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    for (std::size_t descriptor_index = 0; descriptor_index < descriptor_count && descriptor[descriptor_index].Name != 0; ++descriptor_index)
    {
        auto &current_descriptor = descriptor[descriptor_index];
        if (current_descriptor.Name >= image_size || current_descriptor.FirstThunk >= image_size)
            continue;
        const char *name = reinterpret_cast<const char *>(base + current_descriptor.Name);
        if (_stricmp(name, import_name) != 0)
            continue;

        auto *lookup = reinterpret_cast<IMAGE_THUNK_DATA *>(
            const_cast<std::uint8_t *>(base) + (current_descriptor.OriginalFirstThunk != 0 ? current_descriptor.OriginalFirstThunk : current_descriptor.FirstThunk));
        auto *iat = reinterpret_cast<IMAGE_THUNK_DATA *>(const_cast<std::uint8_t *>(base) + current_descriptor.FirstThunk);
        const std::size_t lookup_offset = reinterpret_cast<const std::uint8_t *>(lookup) - base;
        const std::size_t iat_offset = reinterpret_cast<const std::uint8_t *>(iat) - base;
        if (lookup_offset >= image_size || iat_offset >= image_size)
            continue;
        const std::size_t thunk_count = std::min(
            (image_size - lookup_offset) / sizeof(IMAGE_THUNK_DATA),
            (image_size - iat_offset) / sizeof(IMAGE_THUNK_DATA));
        for (std::size_t thunk_index = 0; thunk_index < thunk_count && lookup[thunk_index].u1.AddressOfData != 0; ++thunk_index)
        {
            if (IMAGE_SNAP_BY_ORDINAL(lookup[thunk_index].u1.Ordinal))
                continue;

            if (lookup[thunk_index].u1.AddressOfData >= image_size - sizeof(IMAGE_IMPORT_BY_NAME))
                continue;
            auto *import = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(
                const_cast<std::uint8_t *>(base) + lookup[thunk_index].u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char *>(import->Name), function_name) != 0)
                continue;

            DWORD old_protect = 0;
            if (!VirtualProtect(&iat[thunk_index].u1.Function, sizeof(std::uintptr_t), PAGE_READWRITE, &old_protect))
                return false;

            void *current = reinterpret_cast<void *>(iat[thunk_index].u1.Function);
            if (current == replacement)
            {
                VirtualProtect(&iat[thunk_index].u1.Function, sizeof(std::uintptr_t), old_protect, &old_protect);
                return true;
            }

            if (original != nullptr && *original == nullptr)
                *original = current;
            iat[thunk_index].u1.Function = reinterpret_cast<ULONGLONG>(replacement);
            VirtualProtect(&iat[thunk_index].u1.Function, sizeof(std::uintptr_t), old_protect, &old_protect);
            return true;
        }
    }

    return false;
}

bool hook_iat(HMODULE module, const char *import_name, const char *function_name, void *replacement, void **original)
{
    if (module == nullptr)
        return false;

    MEMORY_BASIC_INFORMATION memory {};
    if (VirtualQuery(module, &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE || memory.AllocationBase != module)
    {
        return false;
    }

    __try
    {
        return hook_iat_unchecked(module, import_name, function_name, replacement, original);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

std::vector<HMODULE> enumerate_process_modules()
{
    std::vector<HMODULE> modules;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
        return modules;

    MODULEENTRY32W entry {};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry))
    {
        do
        {
            modules.push_back(entry.hModule);
        }
        while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return modules;
}

void capture_runtime_snapshot_if_requested()
{
    if ((GetAsyncKeyState(VK_F12) & 1) == 0)
        return;

    if (g_config.trace_texture_creates)
    {
        const ULONGLONG now = GetTickCount64();
        g_texture_trace_count.store(0, std::memory_order_relaxed);
        g_texture_trace_until_tick.store(now + g_config.texture_trace_duration_ms, std::memory_order_relaxed);
        log_line("texture_trace_started source=F12 duration_ms=" + std::to_string(g_config.texture_trace_duration_ms) +
            " main_base=" + hex64(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))));
    }

    DispatchState snapshot {};
    {
        std::lock_guard lock(g_state_mutex);
        snapshot = g_state;
    }

    std::size_t dynamic_color_targets = 0;
    std::size_t latest_producer_generations = 0;
    std::size_t consumed_producer_generations = 0;
    std::size_t late_path_states = 0;
#if defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
    {
        std::lock_guard lock(g_fsr2_dynamic_color_path_mutex);
        dynamic_color_targets = g_fsr2_dynamic_color_targets.size();
        latest_producer_generations = g_fsr2_latest_producer_write_generations.size();
        consumed_producer_generations = g_fsr2_consumed_producer_generations.size();
        late_path_states = g_fsr2_late_path_states.size();
    }
#endif

    SYSTEMTIME st {};
    GetLocalTime(&st);
    const std::filesystem::path path = g_module_dir / L"Dx11FsrBridge.runtime.snapshots.log";
    std::ofstream out(path, std::ios::app);
    out << "snapshot_begin time="
        << st.wYear << "-" << st.wMonth << "-" << st.wDay << "T"
        << st.wHour << ":" << st.wMinute << ":" << st.wSecond << "." << st.wMilliseconds
        << " pid=" << GetCurrentProcessId() << " tick=" << GetTickCount64() << "\n";
    out << "bridge active=" << g_active.load(std::memory_order_relaxed)
        << " translation_mode=" << g_config.fsr2_translation_mode
        << " fast_state_tracking=" << g_config.fsr2_fast_state_tracking
        << " reactive_mask=" << g_config.fsr2_use_reactive_mask
        << " transparency_mask=" << g_config.fsr2_use_transparency_mask
        << " jitter_mode=" << g_config.fsr2_jitter_mode
        << " native_exposure=" << g_config.fsr2_use_native_exposure
        << " compact_output=" << g_config.fsr2_compact_linear_output
        << " lock_color_producer=" << g_config.fsr2_lock_color_producer_shader
        << " reset_on_color_change=" << g_config.fsr2_reset_on_color_path_change << "\n";
    out << "render frame=" << snapshot.frame_index
        << " output=" << snapshot.backbuffer_width << "x" << snapshot.backbuffer_height
        << " viewport=" << snapshot.viewport_width << "x" << snapshot.viewport_height
        << " candidates=" << snapshot.candidate_count
        << " cs=" << hex64(snapshot.current_cs_hash)
        << " ps=" << hex64(snapshot.current_ps_hash)
        << " vs=" << hex64(snapshot.current_vs_hash) << "\n";
#if defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
    out << "translation dispatches=" << g_fsr2_translation_dispatch_count.load(std::memory_order_relaxed)
        << " failures=" << g_fsr2_translation_failure_count.load(std::memory_order_relaxed)
        << " late_composed=" << g_fsr2_late_composed_dispatch_count.load(std::memory_order_relaxed)
        << " stale_fallback=" << g_fsr2_stale_producer_fallback_count.load(std::memory_order_relaxed)
        << " color_replays=" << g_fsr2_color_replay_count.load(std::memory_order_relaxed)
        << " color_path_switches=" << g_fsr2_color_path_switch_count.load(std::memory_order_relaxed)
        << " rejected_producers=" << g_fsr2_rejected_color_producer_count.load(std::memory_order_relaxed)
        << " locked_producer=" << hex64(g_fsr2_locked_color_producer_ps_hash.load(std::memory_order_relaxed))
        << " dynamic_targets=" << dynamic_color_targets
        << " producer_generations=" << latest_producer_generations
        << " consumed_generations=" << consumed_producer_generations
        << " late_path_states=" << late_path_states << "\n";
#endif
    std::string create_scan_copy;
    std::string loader_scan_copy;
    {
        std::lock_guard hook_scan_lock(g_hook_scan_mutex);
        create_scan_copy = g_last_create_hook_scan;
        loader_scan_copy = g_last_loader_hook_scan;
    }
    out << "hooks create_scan=" << create_scan_copy
        << " loader_scan=" << loader_scan_copy << "\n";

    const std::vector<HMODULE> modules = enumerate_process_modules();
    out << "modules count=" << modules.size() << "\n";
    for (HMODULE module : modules)
    {
        wchar_t module_path[MAX_PATH] {};
        const DWORD length = GetModuleFileNameW(module, module_path, MAX_PATH);
        if (length != 0)
            out << "module base=" << hex64(reinterpret_cast<std::uintptr_t>(module))
                << " path=" << narrow(std::wstring(module_path, module_path + length)) << "\n";
    }
    out << "snapshot_end\n\n";
    out.flush();
}

bool is_d3d11_module(HMODULE module)
{
    if (module == nullptr)
        return false;

    wchar_t path[MAX_PATH] {};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (length == 0)
        return false;

    const std::wstring file_name = std::filesystem::path(std::wstring(path, path + length)).filename().wstring();
    return _wcsicmp(file_name.c_str(), L"d3d11.dll") == 0;
}

bool is_user32_module(HMODULE module)
{
    if (module == nullptr)
        return false;

    wchar_t path[MAX_PATH] {};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (length == 0)
        return false;

    const std::wstring file_name = std::filesystem::path(std::wstring(path, path + length)).filename().wstring();
    return _wcsicmp(file_name.c_str(), L"user32.dll") == 0;
}

void install_hdr_environment_probe_for_loaded_modules()
{
    if (!g_config.hdr_environment_probe && !g_config.hdr_output_desc_spoof)
        return;

    std::size_t get_hook_count = 0;
    std::size_t set_hook_count = 0;
    for (HMODULE module : enumerate_process_modules())
    {
        if (hook_iat(module, "USER32.dll", "DisplayConfigGetDeviceInfo",
                reinterpret_cast<void *>(&hooked_display_config_get_device_info),
                reinterpret_cast<void **>(&g_original_display_config_get_device_info)))
        {
            ++get_hook_count;
        }
        if (hook_iat(module, "USER32.dll", "DisplayConfigSetDeviceInfo",
                reinterpret_cast<void *>(&hooked_display_config_set_device_info),
                reinterpret_cast<void **>(&g_original_display_config_set_device_info)))
        {
            ++set_hook_count;
        }
    }

    const std::string summary = "hdr_environment_probe get_iat_hooks=" + std::to_string(get_hook_count) +
        " set_iat_hooks=" + std::to_string(set_hook_count);
    bool summary_changed = false;
    {
        std::lock_guard lock(g_hook_scan_mutex);
        if (summary != g_last_hdr_environment_hook_scan)
        {
            g_last_hdr_environment_hook_scan = summary;
            summary_changed = true;
        }
    }
    if (summary_changed)
        log_line(summary);
}

HRESULT STDMETHODCALLTYPE hooked_output_get_desc1(IDXGIOutput6 *output, DXGI_OUTPUT_DESC1 *desc)
{
    const HRESULT result = g_original_output_get_desc1 != nullptr
        ? g_original_output_get_desc1(output, desc)
        : E_FAIL;

    const bool spoof = g_config.hdr_output_desc_spoof && SUCCEEDED(result) && desc != nullptr;
    const DXGI_COLOR_SPACE_TYPE physical_color_space = spoof ? desc->ColorSpace : DXGI_COLOR_SPACE_CUSTOM;
    if (spoof)
        desc->ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;

    if (!g_config.hdr_output_desc_probe && !spoof)
        return result;

    constexpr std::uint32_t k_log_limit = 32;
    const std::uint32_t call_index = g_hdr_output_desc_probe_call_count.fetch_add(1, std::memory_order_relaxed);
    if (call_index >= k_log_limit)
    {
        if (!g_hdr_output_desc_probe_suppressed_logged.exchange(true, std::memory_order_relaxed))
            log_line("hdr_output_desc_probe query log limit reached");
        return result;
    }

    std::ostringstream out;
    out << "hdr_output_desc_probe result=" << hex64(static_cast<std::uint32_t>(result));
    if (SUCCEEDED(result) && desc != nullptr)
    {
        out << " color_space=" << static_cast<std::uint32_t>(desc->ColorSpace)
            << " bits_per_color=" << desc->BitsPerColor
            << " min_luminance=" << desc->MinLuminance
            << " max_luminance=" << desc->MaxLuminance
            << " max_full_frame_luminance=" << desc->MaxFullFrameLuminance;
        if (spoof)
            out << " physical_color_space=" << static_cast<std::uint32_t>(physical_color_space)
                << " spoof=1";
    }
    else
    {
        out << " response=unavailable";
    }
    log_line(out.str());
    return result;
}

void install_hdr_output_desc_probe_from_adapter(IDXGIAdapter *adapter)
{
    if ((!g_config.hdr_output_desc_probe && !g_config.hdr_output_desc_spoof) || adapter == nullptr)
        return;

    bool expected = false;
    if (!g_hdr_output_desc_probe_install_started.compare_exchange_strong(expected, true, std::memory_order_relaxed))
        return;

    IDXGIOutput *output = nullptr;
    const HRESULT enum_result = adapter->EnumOutputs(0, &output);
    if (FAILED(enum_result) || output == nullptr)
    {
        log_line("hdr_output_desc_probe install_failed stage=EnumOutputs hr=" +
            hex64(static_cast<std::uint32_t>(enum_result)));
        return;
    }

    IDXGIOutput6 *output6 = nullptr;
    const HRESULT query_result = output->QueryInterface(
        __uuidof(IDXGIOutput6), reinterpret_cast<void **>(&output6));
    output->Release();
    if (FAILED(query_result) || output6 == nullptr)
    {
        log_line("hdr_output_desc_probe install_failed stage=QueryInterface hr=" +
            hex64(static_cast<std::uint32_t>(query_result)));
        return;
    }

    void **const vtable = *reinterpret_cast<void ***>(output6);
    g_original_output_get_desc1 = reinterpret_cast<output_get_desc1_fn>(vtable[k_idx_output6_get_desc1]);
    output6->Release();
    if (g_original_output_get_desc1 == nullptr)
    {
        log_line("hdr_output_desc_probe install_failed stage=vtable");
        return;
    }

    LONG status = DetourTransactionBegin();
    if (status == NO_ERROR)
        status = DetourUpdateThread(GetCurrentThread());
    if (status == NO_ERROR)
        status = DetourAttach(reinterpret_cast<PVOID *>(&g_original_output_get_desc1),
            reinterpret_cast<PVOID>(&hooked_output_get_desc1));
    if (status == NO_ERROR)
        status = DetourTransactionCommit();
    else
        DetourTransactionAbort();

    if (status == NO_ERROR)
    {
        log_line("hdr_output_desc_probe installed method=IDXGIOutput6.GetDesc1");
    }
    else
    {
        log_line("hdr_output_desc_probe install_failed stage=Detours error=" + std::to_string(status));
        g_original_output_get_desc1 = nullptr;
    }
}

void install_create_hooks_for_loaded_modules()
{
    std::size_t swapchain_hooks = 0;
    std::size_t device_hooks = 0;

    for (HMODULE module : enumerate_process_modules())
    {
        if (hook_iat(module, "d3d11.dll", "D3D11CreateDeviceAndSwapChain",
                reinterpret_cast<void *>(&hooked_create_device_and_swapchain),
                reinterpret_cast<void **>(&g_original_create_device_and_swapchain)))
        {
            ++swapchain_hooks;
        }

        if (hook_iat(module, "d3d11.dll", "D3D11CreateDevice",
                reinterpret_cast<void *>(&hooked_create_device),
                reinterpret_cast<void **>(&g_original_create_device)))
        {
            ++device_hooks;
        }
    }

    const std::string summary = "iat_scan create_device_and_swapchain_hooks=" + std::to_string(swapchain_hooks) +
        " create_device_hooks=" + std::to_string(device_hooks);
    bool summary_changed = false;
    {
        std::lock_guard lock(g_hook_scan_mutex);
        if (summary != g_last_create_hook_scan)
        {
            g_last_create_hook_scan = summary;
            summary_changed = true;
        }
    }
    if (summary_changed)
    {
        log_line(summary);
        if (swapchain_hooks == 0 && device_hooks == 0)
        {
            log_line("warning no d3d11 create import hooks found; possible reasons: already-created device, GetProcAddress path, or module loaded later");
        }
    }
}

void install_loader_hooks_for_loaded_modules()
{
    std::size_t load_library_a_hooks = 0;
    std::size_t load_library_w_hooks = 0;
    std::size_t load_library_ex_a_hooks = 0;
    std::size_t load_library_ex_w_hooks = 0;
    std::size_t get_proc_address_hooks = 0;

    for (HMODULE module : enumerate_process_modules())
    {
        if (hook_iat(module, "KERNEL32.dll", "LoadLibraryA",
                reinterpret_cast<void *>(&hooked_load_library_a),
                reinterpret_cast<void **>(&g_original_load_library_a)))
        {
            ++load_library_a_hooks;
        }
        if (hook_iat(module, "KERNEL32.dll", "LoadLibraryW",
                reinterpret_cast<void *>(&hooked_load_library_w),
                reinterpret_cast<void **>(&g_original_load_library_w)))
        {
            ++load_library_w_hooks;
        }
        if (hook_iat(module, "KERNEL32.dll", "LoadLibraryExA",
                reinterpret_cast<void *>(&hooked_load_library_ex_a),
                reinterpret_cast<void **>(&g_original_load_library_ex_a)))
        {
            ++load_library_ex_a_hooks;
        }
        if (hook_iat(module, "KERNEL32.dll", "LoadLibraryExW",
                reinterpret_cast<void *>(&hooked_load_library_ex_w),
                reinterpret_cast<void **>(&g_original_load_library_ex_w)))
        {
            ++load_library_ex_w_hooks;
        }
        if (hook_iat(module, "KERNEL32.dll", "GetProcAddress",
                reinterpret_cast<void *>(&hooked_get_proc_address),
                reinterpret_cast<void **>(&g_original_get_proc_address)))
        {
            ++get_proc_address_hooks;
        }
    }

    const std::string summary = "iat_scan loader_hooks"
        " loadlibrarya=" + std::to_string(load_library_a_hooks) +
        " loadlibraryw=" + std::to_string(load_library_w_hooks) +
        " loadlibraryexa=" + std::to_string(load_library_ex_a_hooks) +
        " loadlibraryexw=" + std::to_string(load_library_ex_w_hooks) +
        " getprocaddress=" + std::to_string(get_proc_address_hooks);
    bool summary_changed = false;
    {
        std::lock_guard lock(g_hook_scan_mutex);
        if (summary != g_last_loader_hook_scan)
        {
            g_last_loader_hook_scan = summary;
            summary_changed = true;
        }
    }
    if (summary_changed && g_config.log_loader_activity)
        log_line(summary);
}

bool clone_and_patch_vtable(void *instance, std::size_t method_count, const std::vector<std::pair<std::size_t, void *>> &patches)
{
    if (instance == nullptr)
        return false;

    std::lock_guard lock(g_vtable_mutex);
    if (g_cloned_vtables.contains(instance))
        return true;

    void ***object = reinterpret_cast<void ***>(instance);
    void **source = *object;
    void **clone = reinterpret_cast<void **>(VirtualAlloc(nullptr, sizeof(void *) * method_count, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (clone == nullptr)
        return false;

    std::memcpy(clone, source, sizeof(void *) * method_count);
    for (const auto &[index, value] : patches)
        clone[index] = value;

    *object = clone;
    g_cloned_vtables[instance] = clone;
    g_original_vtables[instance] = source;
    return true;
}

std::size_t context_vtable_size(ID3D11DeviceContext *context)
{
    if (context == nullptr)
        return k_context_vtable_size;

    ID3D11DeviceContext4 *context4 = nullptr;
    const HRESULT result = context->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void **>(&context4));
    if (FAILED(result) || context4 == nullptr)
        return k_context_vtable_size;

    const bool shared_instance = static_cast<ID3D11DeviceContext *>(context4) == context;
    context4->Release();
    return shared_instance ? k_context4_vtable_size : k_context_vtable_size;
}

bool set_cloned_vtable_enabled(void *instance, bool enabled)
{
    if (instance == nullptr)
        return false;

    std::lock_guard lock(g_vtable_mutex);
    const auto cloned = g_cloned_vtables.find(instance);
    const auto original = g_original_vtables.find(instance);
    if (cloned == g_cloned_vtables.end() || original == g_original_vtables.end())
        return false;

    void ***object = reinterpret_cast<void ***>(instance);
    *object = enabled ? cloned->second : original->second;
    return true;
}

class ScopedContextVtableBypass
{
  public:
    explicit ScopedContextVtableBypass(ID3D11DeviceContext *context)
        : context_(context), active_(set_cloned_vtable_enabled(context, false))
    {
    }

    ~ScopedContextVtableBypass()
    {
        if (active_)
            set_cloned_vtable_enabled(context_, true);
    }

    ScopedContextVtableBypass(const ScopedContextVtableBypass &) = delete;
    ScopedContextVtableBypass &operator=(const ScopedContextVtableBypass &) = delete;

  private:
    ID3D11DeviceContext *context_ = nullptr;
    bool active_ = false;
};

#if defined(DX11FSRBRIDGE_FINAL_SCENE_PROBE)
void update_final_scene_probe_backbuffers(IDXGISwapChain *swapchain)
{
    if ((!g_config.final_scene_probe && !g_config.final_scene_snapshot && !g_config.final_scene_optifg_input) ||
        swapchain == nullptr)
        return;

    DXGI_SWAP_CHAIN_DESC desc {};
    if (FAILED(swapchain->GetDesc(&desc)) || desc.BufferCount == 0)
        return;

    std::unordered_set<std::uint64_t> backbuffers;
    for (UINT index = 0; index < desc.BufferCount; ++index)
    {
        ID3D11Texture2D *buffer = nullptr;
        if (SUCCEEDED(swapchain->GetBuffer(index, IID_PPV_ARGS(&buffer))) && buffer != nullptr)
        {
            backbuffers.insert(reinterpret_cast<std::uint64_t>(buffer));
            buffer->Release();
        }
    }

    std::lock_guard lock(g_final_scene_probe_mutex);
    g_final_scene_probe_backbuffers = std::move(backbuffers);
}

bool matches_final_scene_boundary(
    UINT element_count,
    bool indexed,
    std::uint64_t &frame_index,
    bool apply_snapshot_interval)
{
    if ((!g_config.final_scene_snapshot && !g_config.final_scene_optifg_input) || !indexed ||
        element_count != 3 || g_internal_bridge_dispatch)
        return false;

    ResourceInfo target {};
    std::uint64_t pixel_shader_hash = 0;
    std::uint64_t vertex_shader_hash = 0;
    std::uint32_t viewport_width = 0;
    std::uint32_t viewport_height = 0;
    {
        std::lock_guard lock(g_state_mutex);
        target = g_state.rtvs[0];
        pixel_shader_hash = g_state.current_ps_hash;
        vertex_shader_hash = g_state.current_vs_hash;
        viewport_width = g_state.viewport_width;
        viewport_height = g_state.viewport_height;
        frame_index = g_state.frame_index + 1;
    }

    constexpr std::uint64_t k_final_scene_ps = 0x773B7BD7A2C6971Full;
    constexpr std::uint64_t k_final_scene_vs = 0x778E7E69BB2060F5ull;
    bool is_backbuffer = false;
    {
        std::lock_guard lock(g_final_scene_probe_mutex);
        is_backbuffer = g_final_scene_probe_backbuffers.contains(target.resource_key);
    }

    const bool matches = target.resource_key != 0 && pixel_shader_hash == k_final_scene_ps &&
        vertex_shader_hash == k_final_scene_vs && target.width != 0 && target.height != 0 &&
        target.width == viewport_width && target.height == viewport_height && is_backbuffer &&
        (!apply_snapshot_interval ||
            (g_config.final_scene_snapshot &&
                frame_index % g_config.final_scene_snapshot_interval_frames == 0));
    if (!matches)
    {
        // The OptiFG handoff is opt-in; emit one diagnostic only when the known scene shader is observed.
        static std::atomic_bool logged_expected_shader_mismatch { false };
        if (!apply_snapshot_interval && pixel_shader_hash == k_final_scene_ps &&
            vertex_shader_hash == k_final_scene_vs &&
            !logged_expected_shader_mismatch.exchange(true, std::memory_order_relaxed))
        {
            log_line("final_scene_optifg_boundary_rejected target=" + hex64(target.resource_key) +
                " backbuffer=" + std::to_string(is_backbuffer ? 1 : 0) +
                " size=" + std::to_string(target.width) + "x" + std::to_string(target.height) +
                " viewport=" + std::to_string(viewport_width) + "x" + std::to_string(viewport_height));
        }
        return false;
    }

    return true;
}

bool final_scene_snapshot_desc_matches(const D3D11_TEXTURE2D_DESC &desc)
{
    return g_final_scene_snapshot.texture != nullptr &&
        g_final_scene_snapshot.width == desc.Width &&
        g_final_scene_snapshot.height == desc.Height &&
        g_final_scene_snapshot.format == desc.Format;
}

void queue_final_scene_snapshot(ID3D11DeviceContext *context, std::uint64_t frame_index)
{
    if (context == nullptr)
        return;

    ID3D11RenderTargetView *render_target = nullptr;
    context->OMGetRenderTargets(1, &render_target, nullptr);
    if (render_target == nullptr)
        return;

    ID3D11Resource *resource = nullptr;
    render_target->GetResource(&resource);
    render_target->Release();
    ID3D11Texture2D *source = nullptr;
    const HRESULT source_result = resource != nullptr
        ? resource->QueryInterface(IID_PPV_ARGS(&source))
        : E_POINTER;
    if (resource != nullptr)
        resource->Release();
    if (FAILED(source_result) || source == nullptr)
    {
        log_line("final_scene_snapshot_source_failed hr=" +
            std::to_string(static_cast<std::uint32_t>(source_result)));
        return;
    }

    D3D11_TEXTURE2D_DESC source_desc {};
    source->GetDesc(&source_desc);
    if (source_desc.Width == 0 || source_desc.Height == 0)
    {
        source->Release();
        return;
    }

    std::lock_guard lock(g_final_scene_snapshot_mutex);
    if (g_final_scene_snapshot.pending)
    {
        source->Release();
        return;
    }

    if (!final_scene_snapshot_desc_matches(source_desc))
    {
        if (g_final_scene_snapshot.texture != nullptr)
        {
            g_final_scene_snapshot.texture->Release();
            g_final_scene_snapshot.texture = nullptr;
        }
        if (g_final_scene_snapshot.completion_query != nullptr)
        {
            g_final_scene_snapshot.completion_query->Release();
            g_final_scene_snapshot.completion_query = nullptr;
        }

        D3D11_TEXTURE2D_DESC snapshot_desc = source_desc;
        snapshot_desc.BindFlags = 0;
        snapshot_desc.CPUAccessFlags = 0;
        snapshot_desc.MiscFlags = 0;
        snapshot_desc.Usage = D3D11_USAGE_DEFAULT;
        ID3D11Device *device = nullptr;
        context->GetDevice(&device);
        const HRESULT texture_result = device != nullptr
            ? device->CreateTexture2D(&snapshot_desc, nullptr, &g_final_scene_snapshot.texture)
            : E_POINTER;
        if (SUCCEEDED(texture_result) && device != nullptr)
        {
            const D3D11_QUERY_DESC query_desc { D3D11_QUERY_EVENT, 0 };
            const HRESULT query_result = device->CreateQuery(&query_desc, &g_final_scene_snapshot.completion_query);
            if (FAILED(query_result))
            {
                g_final_scene_snapshot.texture->Release();
                g_final_scene_snapshot.texture = nullptr;
                log_line("final_scene_snapshot_query_create_failed hr=" +
                    std::to_string(static_cast<std::uint32_t>(query_result)));
            }
        }
        if (device != nullptr)
            device->Release();
        if (FAILED(texture_result) || g_final_scene_snapshot.texture == nullptr ||
            g_final_scene_snapshot.completion_query == nullptr)
        {
            log_line("final_scene_snapshot_texture_create_failed hr=" +
                std::to_string(static_cast<std::uint32_t>(texture_result)));
            source->Release();
            return;
        }
        g_final_scene_snapshot.width = source_desc.Width;
        g_final_scene_snapshot.height = source_desc.Height;
        g_final_scene_snapshot.format = source_desc.Format;
        log_line("final_scene_snapshot_resources_created size=" +
            std::to_string(source_desc.Width) + "x" + std::to_string(source_desc.Height) +
            " format=" + std::to_string(static_cast<std::uint32_t>(source_desc.Format)));
    }

    {
        ScopedInternalBridgeDispatch internal_dispatch_scope;
        ScopedContextVtableBypass context_vtable_bypass(context);
        context->CopyResource(g_final_scene_snapshot.texture, source);
        context->End(g_final_scene_snapshot.completion_query);
    }
    source->Release();

    g_final_scene_snapshot.queued_frame = frame_index;
    g_final_scene_snapshot.queued_count++;
    g_final_scene_snapshot.pending = true;
    log_line("final_scene_snapshot_queued frame=" + std::to_string(frame_index) +
        " count=" + std::to_string(g_final_scene_snapshot.queued_count) +
        " source=post_scene_pre_ui");
}

void submit_final_scene_to_optiscaler(ID3D11DeviceContext *context, std::uint64_t frame_index)
{
    if (!g_config.final_scene_optifg_input || context == nullptr)
        return;

    using submit_final_scene_fn = BOOL(WINAPI *)(ID3D11Resource *, UINT64);
    static submit_final_scene_fn submit = nullptr;
    static HMODULE module = nullptr;
    static std::atomic_bool missing_export_logged { false };
    static std::atomic_uint64_t accepted_count { 0 };

    if (module == nullptr)
    {
        module = GetModuleHandleW(L"OptiScaler.dll");
        if (module != nullptr)
            submit = reinterpret_cast<submit_final_scene_fn>(
                GetProcAddress(module, "OptiScalerSubmitFinalSceneD3D11"));
    }
    if (submit == nullptr)
    {
        if (!missing_export_logged.exchange(true, std::memory_order_relaxed))
            log_line("final_scene_optifg_input_unavailable export=OptiScalerSubmitFinalSceneD3D11");
        return;
    }

    ID3D11RenderTargetView *render_target = nullptr;
    context->OMGetRenderTargets(1, &render_target, nullptr);
    if (render_target == nullptr)
        return;
    ID3D11Resource *scene = nullptr;
    render_target->GetResource(&scene);
    render_target->Release();
    if (scene == nullptr)
        return;

    const BOOL accepted = submit(scene, frame_index);
    scene->Release();
    if (accepted)
    {
        const std::uint64_t count = accepted_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count == 1 || count % 240 == 0)
        {
            log_line("final_scene_optifg_input_accepted frame=" + std::to_string(frame_index) +
                " count=" + std::to_string(count));
        }
    }
}

void poll_final_scene_snapshot(IDXGISwapChain *swapchain)
{
    if (!g_config.final_scene_snapshot || swapchain == nullptr)
        return;

    {
        std::lock_guard lock(g_final_scene_snapshot_mutex);
        if (!g_final_scene_snapshot.pending || g_final_scene_snapshot.completion_query == nullptr)
            return;
    }

    ID3D11Device *device = nullptr;
    if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
        return;
    ID3D11DeviceContext *context = nullptr;
    device->GetImmediateContext(&context);
    device->Release();
    if (context == nullptr)
        return;

    std::lock_guard lock(g_final_scene_snapshot_mutex);
    if (g_final_scene_snapshot.pending && g_final_scene_snapshot.completion_query != nullptr &&
        context->GetData(g_final_scene_snapshot.completion_query, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK)
    {
        g_final_scene_snapshot.pending = false;
        g_final_scene_snapshot.completed_count++;
        log_line("final_scene_snapshot_complete frame=" +
            std::to_string(g_final_scene_snapshot.queued_frame) +
            " queued=" + std::to_string(g_final_scene_snapshot.queued_count) +
            " completed=" + std::to_string(g_final_scene_snapshot.completed_count));
    }
    context->Release();
}

void flush_final_scene_probe(std::uint64_t frame_index)
{
    if (!g_config.final_scene_probe)
        return;

    FinalSceneProbeFrame frame {};
    bool should_log = false;
    {
        std::lock_guard lock(g_final_scene_probe_mutex);
        if (g_final_scene_probe_frame.frame_index != frame_index)
            return;
        frame = g_final_scene_probe_frame;
        g_final_scene_probe_frame = {};

        std::uint64_t signature = 1469598103934665603ull;
        const auto mix_signature = [&](std::uint64_t value)
        {
            signature ^= value;
            signature *= 1099511628211ull;
        };
        mix_signature(frame.display_target_draws);
        mix_signature(frame.backbuffer_draws);
        mix_signature(frame.candidate_count);
        mix_signature(frame.tail_candidate_count);
        const auto mix_candidate = [&](const FinalSceneProbeCandidate &candidate)
        {
            mix_signature(candidate.pixel_shader_hash);
            mix_signature(candidate.pixel_shader_size);
            mix_signature(candidate.vertex_shader_hash);
            mix_signature(candidate.vertex_shader_size);
            mix_signature(candidate.source0.width);
            mix_signature(candidate.source0.height);
            mix_signature(static_cast<std::uint32_t>(candidate.source0.format));
            mix_signature(candidate.width);
            mix_signature(candidate.height);
            mix_signature(static_cast<std::uint32_t>(candidate.format));
            mix_signature(candidate.viewport_width);
            mix_signature(candidate.viewport_height);
            mix_signature(candidate.render_target_count);
            mix_signature(candidate.element_count);
            mix_signature(candidate.indexed ? 1u : 0u);
            mix_signature(candidate.backbuffer ? 1u : 0u);
            mix_signature(candidate.display_draw_ordinal);
            mix_signature(candidate.backbuffer_draw_ordinal);
        };
        for (std::uint32_t index = 0; index < frame.candidate_count; ++index)
        {
            mix_candidate(frame.candidates[index]);
        }
        if (frame.display_target_draws > frame.candidate_count)
        {
            for (std::uint32_t index = 0; index < frame.tail_candidate_count; ++index)
                mix_candidate(frame.tail_candidates[index]);
        }
        if (g_final_scene_probe_signatures.size() < g_config.final_scene_probe_signature_limit)
            should_log = g_final_scene_probe_signatures.insert(signature).second;
    }

    if (!should_log)
        return;

    std::ostringstream out;
    out << "final_scene_probe frame=" << frame.frame_index
        << " display_target_draws=" << frame.display_target_draws
        << " backbuffer_draws=" << frame.backbuffer_draws
        << " recorded=" << frame.candidate_count
        << " tail_recorded=" << frame.tail_candidate_count;
    const auto append_candidate = [&](const char *label, std::uint32_t index, const FinalSceneProbeCandidate &candidate)
    {
        out << " " << label << index
            << "={draw=" << (candidate.indexed ? "indexed" : "nonindexed")
            << ",elements=" << candidate.element_count
            << ",target=" << hex64(candidate.resource_key)
            << ",srv0=" << hex64(candidate.source0.resource_key)
            << ",srv0_size=" << candidate.source0.width << "x" << candidate.source0.height
            << ",srv0_format=" << format_string(candidate.source0.format)
            << ",ps=" << hex64(candidate.pixel_shader_hash)
            << ",ps_size=" << candidate.pixel_shader_size
            << ",vs=" << hex64(candidate.vertex_shader_hash)
            << ",vs_size=" << candidate.vertex_shader_size
            << ",size=" << candidate.width << "x" << candidate.height
            << ",format=" << format_string(candidate.format)
            << ",viewport=" << candidate.viewport_width << "x" << candidate.viewport_height
            << ",rtvs=" << candidate.render_target_count
            << ",backbuffer=" << (candidate.backbuffer ? 1 : 0)
            << ",display_order=" << candidate.display_draw_ordinal
            << ",backbuffer_order=" << candidate.backbuffer_draw_ordinal << "}";
    };
    for (std::uint32_t index = 0; index < frame.candidate_count; ++index)
    {
        append_candidate("candidate", index, frame.candidates[index]);
    }
    if (frame.display_target_draws > frame.candidate_count)
    {
        for (std::uint32_t index = 0; index < frame.tail_candidate_count; ++index)
            append_candidate("tail", index, frame.tail_candidates[index]);
    }
    log_line(out.str());
}

void record_final_scene_probe_draw(UINT element_count, bool indexed)
{
    if (!g_config.final_scene_probe || g_internal_bridge_dispatch)
        return;

    FinalSceneProbeCandidate candidate {};
    std::uint64_t frame_index = 0;
    {
        std::lock_guard state_lock(g_state_mutex);
        const ResourceInfo &target = g_state.rtvs[0];
        if (target.resource_key == 0 || target.width == 0 || target.height == 0 ||
            g_state.backbuffer_width == 0 || g_state.backbuffer_height == 0 ||
            target.width != g_state.backbuffer_width || target.height != g_state.backbuffer_height)
        {
            return;
        }

        candidate.resource_key = target.resource_key;
        candidate.source0 = g_state.ps_srvs[0];
        candidate.pixel_shader_hash = g_state.current_ps_hash;
        candidate.pixel_shader_size = g_state.current_ps_size;
        candidate.vertex_shader_hash = g_state.current_vs_hash;
        candidate.vertex_shader_size = g_state.current_vs_size;
        candidate.width = target.width;
        candidate.height = target.height;
        candidate.format = target.format;
        candidate.viewport_width = g_state.viewport_width;
        candidate.viewport_height = g_state.viewport_height;
        candidate.element_count = element_count;
        candidate.indexed = indexed;
        for (const ResourceInfo &rtv : g_state.rtvs)
        {
            if (rtv.resource_key != 0)
                ++candidate.render_target_count;
        }
        frame_index = g_state.frame_index + 1;
    }

    std::lock_guard probe_lock(g_final_scene_probe_mutex);
    candidate.backbuffer = g_final_scene_probe_backbuffers.contains(candidate.resource_key);
    if (g_final_scene_probe_frame.frame_index != frame_index)
        g_final_scene_probe_frame = { .frame_index = frame_index };

    candidate.display_draw_ordinal = ++g_final_scene_probe_frame.display_target_draws;
    if (candidate.backbuffer)
        candidate.backbuffer_draw_ordinal = ++g_final_scene_probe_frame.backbuffer_draws;
    if (g_final_scene_probe_frame.candidate_count < g_config.final_scene_probe_limit)
        g_final_scene_probe_frame.candidates[g_final_scene_probe_frame.candidate_count++] = candidate;
    if (g_final_scene_probe_frame.tail_candidate_count < g_config.final_scene_probe_limit)
    {
        g_final_scene_probe_frame.tail_candidates[g_final_scene_probe_frame.tail_candidate_count++] = candidate;
    }
    else
    {
        std::move(g_final_scene_probe_frame.tail_candidates.begin() + 1,
            g_final_scene_probe_frame.tail_candidates.begin() + g_config.final_scene_probe_limit,
            g_final_scene_probe_frame.tail_candidates.begin());
        g_final_scene_probe_frame.tail_candidates[g_config.final_scene_probe_limit - 1] = candidate;
    }
}
#endif

bool is_known_swapchain_backbuffer(std::uint64_t resource_key)
{
    if (resource_key == 0)
        return false;
    std::lock_guard lock(g_swapchain_backbuffer_mutex);
    return g_swapchain_backbuffer_resources.contains(resource_key);
}

void record_hdr_composite_candidate(UINT element_count, bool indexed)
{
    if (!g_config.hdr_composite_probe || g_internal_bridge_dispatch)
        return;

    ResourceInfo target {};
    ResourceInfo source {};
    std::uint64_t pixel_shader_hash = 0;
    std::uint64_t vertex_shader_hash = 0;
    std::uint32_t viewport_width = 0;
    std::uint32_t viewport_height = 0;
    std::uint32_t backbuffer_width = 0;
    std::uint32_t backbuffer_height = 0;
    std::uint64_t frame_index = 0;
    {
        std::lock_guard lock(g_state_mutex);
        target = g_state.rtvs[0];
        source = g_state.ps_srvs[0];
        pixel_shader_hash = g_state.current_ps_hash;
        vertex_shader_hash = g_state.current_vs_hash;
        viewport_width = g_state.viewport_width;
        viewport_height = g_state.viewport_height;
        backbuffer_width = g_state.backbuffer_width;
        backbuffer_height = g_state.backbuffer_height;
        frame_index = g_state.frame_index;
    }

    if (target.resource_key == 0 || target.width == 0 || target.height == 0 ||
        (target.format != DXGI_FORMAT_R10G10B10A2_UNORM &&
            target.format != DXGI_FORMAT_R8G8B8A8_UNORM &&
            target.format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) ||
        target.width != backbuffer_width || target.height != backbuffer_height)
        return;

    const bool full_resolution_source = source.resource_key != 0 && source.resource_key != target.resource_key &&
        source.width == target.width && source.height == target.height;
    const bool full_resolution_viewport = viewport_width == target.width && viewport_height == target.height;

    std::uint64_t signature = pixel_shader_hash;
    signature ^= vertex_shader_hash + 0x9E3779B97F4A7C15ull + (signature << 6) + (signature >> 2);
    signature ^= static_cast<std::uint64_t>(element_count) << 32 | (indexed ? 1u : 0u);
    signature ^= source.resource_key + 0x9E3779B97F4A7C15ull + (signature << 6) + (signature >> 2);
    signature ^= static_cast<std::uint64_t>(source.format) << 32 | static_cast<std::uint32_t>(target.format);
    {
        std::lock_guard lock(g_hdr_composite_probe_mutex);
        if (g_hdr_composite_probe_signatures.size() >= g_config.hdr_composite_probe_limit ||
            !g_hdr_composite_probe_signatures.insert(signature).second)
        {
            return;
        }
    }

    const std::uint32_t index = g_hdr_composite_probe_count.fetch_add(1, std::memory_order_relaxed) + 1;
    log_line("hdr_composite_candidate index=" + std::to_string(index) +
        " frame=" + std::to_string(frame_index) +
        " draw=" + std::string(indexed ? "indexed" : "nonindexed") +
        " elements=" + std::to_string(element_count) +
        " ps=" + hex64(pixel_shader_hash) +
        " vs=" + hex64(vertex_shader_hash) +
        " source=" + hex64(source.resource_key) +
        " source_size=" + std::to_string(source.width) + "x" + std::to_string(source.height) +
        " source_format=" + describe_dxgi_format(source.format) +
        " source_view_format=" + describe_dxgi_format(source.view_format) +
        " source_bind_flags=" + hex64(source.bind_flags) +
        " source_misc_flags=" + hex64(source.misc_flags) +
        " target=" + hex64(target.resource_key) +
        " target_format=" + describe_dxgi_format(target.format) +
        " target_view_format=" + describe_dxgi_format(target.view_format) +
        " target_bind_flags=" + hex64(target.bind_flags) +
        " size=" + std::to_string(target.width) + "x" + std::to_string(target.height) +
        " full_source=" + std::to_string(full_resolution_source ? 1 : 0) +
        " full_viewport=" + std::to_string(full_resolution_viewport ? 1 : 0));
}

void record_hdr_composite_copy(const ResourceInfo &destination, const ResourceInfo &source, const char *kind)
{
    if (!g_config.hdr_composite_probe || !is_known_swapchain_backbuffer(destination.resource_key))
        return;

    std::uint64_t signature = destination.resource_key;
    signature ^= source.resource_key + 0x9E3779B97F4A7C15ull + (signature << 6) + (signature >> 2);
    signature ^= static_cast<std::uint64_t>(destination.format) << 32 | static_cast<std::uint32_t>(source.format);
    {
        std::lock_guard lock(g_hdr_composite_probe_mutex);
        if (g_hdr_composite_probe_signatures.size() >= g_config.hdr_composite_probe_limit ||
            !g_hdr_composite_probe_signatures.insert(signature).second)
        {
            return;
        }
    }

    const std::uint32_t index = g_hdr_composite_probe_count.fetch_add(1, std::memory_order_relaxed) + 1;
    log_line("hdr_composite_copy index=" + std::to_string(index) +
        " kind=" + kind +
        " source=" + hex64(source.resource_key) +
        " source_size=" + std::to_string(source.width) + "x" + std::to_string(source.height) +
        " source_format=" + describe_dxgi_format(source.format) +
        " target_format=" + describe_dxgi_format(destination.format) +
        " size=" + std::to_string(destination.width) + "x" + std::to_string(destination.height));
}

void record_hdr_composite_target_bind(const ResourceInfo &target)
{
    if (!g_config.hdr_composite_probe || target.resource_key == 0 || target.width == 0 || target.height == 0)
        return;

    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    {
        std::lock_guard lock(g_state_mutex);
        output_width = g_state.backbuffer_width;
        output_height = g_state.backbuffer_height;
    }
    if (target.width != output_width || target.height != output_height)
        return;

    std::uint64_t signature = target.resource_key;
    signature ^= static_cast<std::uint64_t>(target.format) << 32;
    {
        std::lock_guard lock(g_hdr_composite_probe_mutex);
        if (g_hdr_composite_probe_signatures.size() >= g_config.hdr_composite_probe_limit ||
            !g_hdr_composite_probe_signatures.insert(signature).second)
        {
            return;
        }
    }

    const std::uint32_t index = g_hdr_composite_probe_count.fetch_add(1, std::memory_order_relaxed) + 1;
    log_line("hdr_composite_target_bind index=" + std::to_string(index) +
        " target=" + hex64(target.resource_key) +
        " format=" + describe_dxgi_format(target.format) +
        " size=" + std::to_string(target.width) + "x" + std::to_string(target.height) +
        " swapchain_backbuffer=" + std::to_string(is_known_swapchain_backbuffer(target.resource_key) ? 1 : 0));
}

bool is_hdr_sdr_tone_map_composite_draw(UINT element_count)
{
    if (!g_config.hdr_sdr_tone_map || !g_config.hdr_swapchain_spoof || element_count != 3 ||
        g_internal_bridge_dispatch)
    {
        return false;
    }

    ResourceInfo target {};
    ResourceInfo source {};
    std::uint32_t viewport_width = 0;
    std::uint32_t viewport_height = 0;
    std::uint32_t backbuffer_width = 0;
    std::uint32_t backbuffer_height = 0;
    {
        std::lock_guard lock(g_state_mutex);
        target = g_state.rtvs[0];
        source = g_state.ps_srvs[0];
        viewport_width = g_state.viewport_width;
        viewport_height = g_state.viewport_height;
        backbuffer_width = g_state.backbuffer_width;
        backbuffer_height = g_state.backbuffer_height;
    }

    return target.format == DXGI_FORMAT_R10G10B10A2_UNORM &&
        target.width == backbuffer_width && target.height == backbuffer_height &&
        source.resource_key != 0 && source.resource_key != target.resource_key &&
        source.format == DXGI_FORMAT_R8G8B8A8_TYPELESS &&
        source.width == target.width && source.height == target.height &&
        viewport_width == target.width && viewport_height == target.height;
}

bool hdr_sdr_tone_map_format_supported(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

void release_hdr_sdr_tone_map_resources_locked()
{
    auto release = [](auto *&resource)
    {
        if (resource != nullptr)
        {
            resource->Release();
            resource = nullptr;
        }
    };
    release(g_hdr_sdr_tone_map_resources.source_copy);
    release(g_hdr_sdr_tone_map_resources.source_view);
    release(g_hdr_sdr_tone_map_resources.source_target_view);
    release(g_hdr_sdr_tone_map_resources.target_view);
    release(g_hdr_sdr_tone_map_resources.target_texture);
    release(g_hdr_sdr_tone_map_resources.vertex_shader);
    release(g_hdr_sdr_tone_map_resources.pixel_shader);
    release(g_hdr_sdr_tone_map_resources.sampler);
    release(g_hdr_sdr_tone_map_resources.constants);
    release(g_hdr_sdr_tone_map_resources.device);
    g_hdr_sdr_tone_map_resources = {};
    g_hdr_sdr_tone_map_shader_create_failed = false;
}

bool compile_hdr_sdr_tone_map_shaders_locked()
{
    if (g_hdr_sdr_tone_map_shader_compile_attempted)
        return !g_hdr_sdr_tone_map_vs_bytecode.empty() && !g_hdr_sdr_tone_map_ps_bytecode.empty();

    g_hdr_sdr_tone_map_shader_compile_attempted = true;
    static constexpr char vertex_source[] = R"(
struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOutput main(uint vertex_id : SV_VertexID)
{
    VertexOutput output;
    const float2 position = float2((vertex_id << 1) & 2, vertex_id & 2);
    output.uv = position;
    output.position = float4(position * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
)";
    static constexpr char pixel_source[] = R"(
cbuffer ToneMapConstants : register(b0)
{
    float paper_white_nits;
    float peak_nits;
    float pq_input;
    float padding;
};

Texture2D<float4> SourceColor : register(t0);
SamplerState LinearSampler : register(s0);

float3 pq_to_nits(float3 value)
{
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    const float3 powered = pow(max(value, 0.0), 1.0 / m2);
    return pow(max(powered - c1, 0.0) / max(c2 - c3 * powered, 1e-5), 1.0 / m1) * 10000.0;
}

float3 bt2020_to_bt709(float3 value)
{
    return float3(
        dot(value, float3(1.660491, -0.587641, -0.072850)),
        dot(value, float3(-0.124551, 1.132900, -0.008349)),
        dot(value, float3(-0.018151, -0.100579, 1.118730)));
}

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    const float3 sampled = max(SourceColor.SampleLevel(LinearSampler, uv, 0.0).rgb, 0.0);
    float3 scene_linear = sampled;
    if (pq_input > 0.5)
    {
        // The spoofed SDR path still carries the game's HDR10 signal in a
        // 10-bit UNORM target: decode ST.2084, convert its Rec.2020 gamut,
        // then normalize against the SDR paper white before encoding gamma.
        const float3 scene_nits = pq_to_nits(sampled);
        // The engine advertises a P2020 swapchain, but the copied composite
        // already carries display-referred RGB primaries. Treating it as
        // P2020 here applies a second gamut conversion and desaturates the
        // SDR result, so retain the composite's native RGB basis.
        scene_linear = scene_nits / max(paper_white_nits, 1.0);

        // Compress against the configured content peak. This leaves headroom
        // above scene paper white instead of hard-clipping the whole HDR
        // range at that anchor.
        const float white_point = max(peak_nits / max(paper_white_nits, 1.0), 1.0);
        const float white_point_sq = white_point * white_point;
        const float scene_luma = dot(scene_linear, float3(0.2126, 0.7152, 0.0722));
        const float mapped_luma = scene_luma * (1.0 + scene_luma / white_point_sq) /
            (1.0 + scene_luma);
        const float chroma_scale = mapped_luma / max(scene_luma, 1e-5);
        scene_linear *= chroma_scale;
    }
    return float4(pow(saturate(scene_linear), 1.0 / 2.2), 1.0);
}
)";

    const auto compile = [](const char *source, const char *name, const char *entry, const char *target,
        std::vector<std::uint8_t> &output) -> bool
    {
        ID3DBlob *bytecode = nullptr;
        ID3DBlob *errors = nullptr;
        const HRESULT result = D3DCompile(
            source,
            std::strlen(source),
            name,
            nullptr,
            nullptr,
            entry,
            target,
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0,
            &bytecode,
            &errors);
        if (FAILED(result) || bytecode == nullptr)
        {
            std::string message = std::string("hdr_sdr_tone_map_shader_compile_failed stage=") + name +
                " hr=" + std::to_string(static_cast<long>(result));
            if (errors != nullptr && errors->GetBufferPointer() != nullptr)
                message += " error=" + std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize());
            log_line(message);
            if (errors != nullptr)
                errors->Release();
            if (bytecode != nullptr)
                bytecode->Release();
            return false;
        }
        const auto *begin = static_cast<const std::uint8_t *>(bytecode->GetBufferPointer());
        output.assign(begin, begin + bytecode->GetBufferSize());
        if (errors != nullptr)
            errors->Release();
        bytecode->Release();
        return true;
    };

    return compile(vertex_source, "Dx11FsrBridgeHdrSdrToneMapVS", "main", "vs_5_0", g_hdr_sdr_tone_map_vs_bytecode) &&
        compile(pixel_source, "Dx11FsrBridgeHdrSdrToneMapPS", "main", "ps_5_0", g_hdr_sdr_tone_map_ps_bytecode);
}

void release_hdr_sdr_tone_map_draw_resources_locked()
{
    if (g_hdr_sdr_tone_map_draw_resources.pixel_shader != nullptr)
    {
        g_hdr_sdr_tone_map_draw_resources.pixel_shader->Release();
        g_hdr_sdr_tone_map_draw_resources.pixel_shader = nullptr;
    }
    if (g_hdr_sdr_tone_map_draw_resources.constants != nullptr)
    {
        g_hdr_sdr_tone_map_draw_resources.constants->Release();
        g_hdr_sdr_tone_map_draw_resources.constants = nullptr;
    }
    if (g_hdr_sdr_tone_map_draw_resources.device != nullptr)
    {
        g_hdr_sdr_tone_map_draw_resources.device->Release();
        g_hdr_sdr_tone_map_draw_resources.device = nullptr;
    }
}

bool acquire_hdr_sdr_tone_map_draw_resources(
    ID3D11DeviceContext *context,
    ID3D11PixelShader **pixel_shader,
    ID3D11Buffer **constants)
{
    if (pixel_shader == nullptr || constants == nullptr || context == nullptr)
        return false;
    *pixel_shader = nullptr;
    *constants = nullptr;

    ID3D11Device *device = nullptr;
    context->GetDevice(&device);
    if (device == nullptr)
        return false;

    std::lock_guard lock(g_hdr_sdr_tone_map_mutex);
    auto &resources = g_hdr_sdr_tone_map_draw_resources;
    if (resources.device != device)
    {
        release_hdr_sdr_tone_map_draw_resources_locked();
        resources.device = device;
        device = nullptr;
    }
    if (device != nullptr)
        device->Release();

    if (resources.pixel_shader == nullptr || resources.constants == nullptr)
    {
        if (!compile_hdr_sdr_tone_map_shaders_locked())
            return false;
        const HRESULT shader_result = g_original_create_pixel_shader != nullptr
            ? g_original_create_pixel_shader(
                resources.device,
                g_hdr_sdr_tone_map_ps_bytecode.data(),
                g_hdr_sdr_tone_map_ps_bytecode.size(),
                nullptr,
                &resources.pixel_shader)
            : resources.device->CreatePixelShader(
                g_hdr_sdr_tone_map_ps_bytecode.data(),
                g_hdr_sdr_tone_map_ps_bytecode.size(),
                nullptr,
                &resources.pixel_shader);
        if (FAILED(shader_result) || resources.pixel_shader == nullptr)
        {
            log_line("hdr_sdr_tone_map_draw_shader_create_failed hr=" +
                std::to_string(static_cast<long>(shader_result)));
            release_hdr_sdr_tone_map_draw_resources_locked();
            return false;
        }

        D3D11_BUFFER_DESC buffer_desc {};
        buffer_desc.ByteWidth = 16;
        buffer_desc.Usage = D3D11_USAGE_DEFAULT;
        buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        const HRESULT buffer_result = g_original_create_buffer != nullptr
            ? g_original_create_buffer(resources.device, &buffer_desc, nullptr, &resources.constants)
            : resources.device->CreateBuffer(&buffer_desc, nullptr, &resources.constants);
        if (FAILED(buffer_result) || resources.constants == nullptr)
        {
            log_line("hdr_sdr_tone_map_draw_constants_create_failed hr=" +
                std::to_string(static_cast<long>(buffer_result)));
            release_hdr_sdr_tone_map_draw_resources_locked();
            return false;
        }
        log_line("hdr_sdr_tone_map_draw_resources_ready");
    }

    resources.pixel_shader->AddRef();
    resources.constants->AddRef();
    *pixel_shader = resources.pixel_shader;
    *constants = resources.constants;
    return true;
}

bool ensure_hdr_sdr_tone_map_resources_locked(
    ID3D11Device *device,
    ID3D11Texture2D *target_texture,
    ID3D11RenderTargetView *target_view)
{
    if (device == nullptr || target_texture == nullptr || target_view == nullptr)
        return false;

    D3D11_TEXTURE2D_DESC target_desc {};
    target_texture->GetDesc(&target_desc);
    if (target_desc.Width == 0 || target_desc.Height == 0 || target_desc.SampleDesc.Count != 1 ||
        !hdr_sdr_tone_map_format_supported(target_desc.Format))
        return false;

    const std::uint64_t target_key = reinterpret_cast<std::uint64_t>(target_texture);
    auto &resources = g_hdr_sdr_tone_map_resources;
    const bool device_changed = resources.device != device;
    const bool size_changed = resources.width != target_desc.Width || resources.height != target_desc.Height ||
        resources.format != target_desc.Format;
    if (device_changed || size_changed)
    {
        release_hdr_sdr_tone_map_resources_locked();
        device->AddRef();
        resources.device = device;
        resources.width = target_desc.Width;
        resources.height = target_desc.Height;
        resources.format = target_desc.Format;

        if (!compile_hdr_sdr_tone_map_shaders_locked())
            return false;

        D3D11_TEXTURE2D_DESC source_desc = target_desc;
        source_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        source_desc.CPUAccessFlags = 0;
        source_desc.MiscFlags = 0;
        source_desc.Usage = D3D11_USAGE_DEFAULT;
        HRESULT result = device->CreateTexture2D(&source_desc, nullptr, &resources.source_copy);
        if (FAILED(result) || resources.source_copy == nullptr)
        {
            log_line("hdr_sdr_tone_map_source_create_failed hr=" + std::to_string(static_cast<long>(result)) +
                " format=" + describe_dxgi_format(target_desc.Format));
            release_hdr_sdr_tone_map_resources_locked();
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC view_desc {};
        view_desc.Format = target_desc.Format;
        view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view_desc.Texture2D.MostDetailedMip = 0;
        view_desc.Texture2D.MipLevels = 1;
        result = device->CreateShaderResourceView(resources.source_copy, &view_desc, &resources.source_view);
        if (FAILED(result) || resources.source_view == nullptr)
        {
            log_line("hdr_sdr_tone_map_srv_create_failed hr=" + std::to_string(static_cast<long>(result)));
            release_hdr_sdr_tone_map_resources_locked();
            return false;
        }

        D3D11_RENDER_TARGET_VIEW_DESC target_view_desc {};
        target_view_desc.Format = target_desc.Format;
        target_view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        target_view_desc.Texture2D.MipSlice = 0;
        result = device->CreateRenderTargetView(resources.source_copy, &target_view_desc, &resources.source_target_view);
        if (FAILED(result) || resources.source_target_view == nullptr)
        {
            log_line("hdr_sdr_tone_map_intermediate_rtv_create_failed hr=" + std::to_string(static_cast<long>(result)));
            release_hdr_sdr_tone_map_resources_locked();
            return false;
        }

        result = device->CreateVertexShader(
            g_hdr_sdr_tone_map_vs_bytecode.data(), g_hdr_sdr_tone_map_vs_bytecode.size(), nullptr,
            &resources.vertex_shader);
        if (SUCCEEDED(result))
        {
            result = device->CreatePixelShader(
                g_hdr_sdr_tone_map_ps_bytecode.data(), g_hdr_sdr_tone_map_ps_bytecode.size(), nullptr,
                &resources.pixel_shader);
        }
        if (FAILED(result) || resources.vertex_shader == nullptr || resources.pixel_shader == nullptr)
        {
            log_line("hdr_sdr_tone_map_shader_create_failed hr=" + std::to_string(static_cast<long>(result)));
            release_hdr_sdr_tone_map_resources_locked();
            return false;
        }

        D3D11_SAMPLER_DESC sampler_desc {};
        sampler_desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.MinLOD = 0.0f;
        sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
        result = device->CreateSamplerState(&sampler_desc, &resources.sampler);
        if (SUCCEEDED(result))
        {
            D3D11_BUFFER_DESC buffer_desc {};
            buffer_desc.ByteWidth = 16;
            buffer_desc.Usage = D3D11_USAGE_DEFAULT;
            buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            result = device->CreateBuffer(&buffer_desc, nullptr, &resources.constants);
        }
        if (FAILED(result) || resources.sampler == nullptr || resources.constants == nullptr)
        {
            log_line("hdr_sdr_tone_map_state_create_failed hr=" + std::to_string(static_cast<long>(result)));
            release_hdr_sdr_tone_map_resources_locked();
            return false;
        }

        log_line("hdr_sdr_tone_map_resources_created size=" +
            std::to_string(target_desc.Width) + "x" + std::to_string(target_desc.Height) +
            " format=" + describe_dxgi_format(target_desc.Format));
    }

    if (resources.target_key != target_key)
    {
        if (resources.target_view != nullptr)
            resources.target_view->Release();
        if (resources.target_texture != nullptr)
            resources.target_texture->Release();
        target_texture->AddRef();
        target_view->AddRef();
        resources.target_texture = target_texture;
        resources.target_view = target_view;
        resources.target_key = target_key;
    }
    return resources.source_copy != nullptr && resources.source_view != nullptr &&
        resources.target_view != nullptr && resources.vertex_shader != nullptr &&
        resources.pixel_shader != nullptr && resources.sampler != nullptr && resources.constants != nullptr;
}

void tone_map_hdr_backbuffer_to_sdr(IDXGISwapChain *swapchain, std::uint64_t frame_index)
{
    if (!g_config.hdr_sdr_tone_map || !g_config.hdr_swapchain_spoof || swapchain == nullptr || g_internal_bridge_dispatch)
        return;

    ID3D11Device *device = nullptr;
    if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
        return;
    ID3D11DeviceContext *context = nullptr;
    device->GetImmediateContext(&context);
    if (context == nullptr)
    {
        device->Release();
        return;
    }

    ID3D11RenderTargetView *bound_target = nullptr;
    context->OMGetRenderTargets(1, &bound_target, nullptr);
    if (bound_target == nullptr)
    {
        context->Release();
        device->Release();
        return;
    }
    ID3D11Resource *target_resource = nullptr;
    bound_target->GetResource(&target_resource);
    ID3D11Texture2D *target_texture = nullptr;
    if (target_resource != nullptr)
        target_resource->QueryInterface(IID_PPV_ARGS(&target_texture));
    if (target_resource != nullptr)
        target_resource->Release();
    if (target_texture == nullptr)
    {
        bound_target->Release();
        context->Release();
        device->Release();
        return;
    }

    DXGI_SWAP_CHAIN_DESC swapchain_desc {};
    bool is_swapchain_target = SUCCEEDED(swapchain->GetDesc(&swapchain_desc));
    if (is_swapchain_target)
    {
        is_swapchain_target = false;
        for (UINT index = 0; index < swapchain_desc.BufferCount; ++index)
        {
            ID3D11Texture2D *buffer = nullptr;
            if (SUCCEEDED(swapchain->GetBuffer(index, IID_PPV_ARGS(&buffer))) && buffer != nullptr)
            {
                is_swapchain_target = buffer == target_texture;
                buffer->Release();
                if (is_swapchain_target)
                    break;
            }
        }
    }
    if (!is_swapchain_target)
    {
        target_texture->Release();
        bound_target->Release();
        context->Release();
        device->Release();
        return;
    }

    std::lock_guard tone_map_lock(g_hdr_sdr_tone_map_mutex);
    if (!ensure_hdr_sdr_tone_map_resources_locked(device, target_texture, bound_target))
    {
        target_texture->Release();
        bound_target->Release();
        context->Release();
        device->Release();
        return;
    }

    auto &resources = g_hdr_sdr_tone_map_resources;
    ID3D11RenderTargetView *saved_render_targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] {};
    ID3D11DepthStencilView *saved_depth_stencil = nullptr;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, saved_render_targets, &saved_depth_stencil);

    D3D11_VIEWPORT saved_viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] {};
    UINT saved_viewport_count = static_cast<UINT>(std::size(saved_viewports));
    context->RSGetViewports(&saved_viewport_count, saved_viewports);

    ID3D11InputLayout *saved_input_layout = nullptr;
    context->IAGetInputLayout(&saved_input_layout);
    D3D11_PRIMITIVE_TOPOLOGY saved_topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    context->IAGetPrimitiveTopology(&saved_topology);
    ID3D11VertexShader *saved_vertex_shader = nullptr;
    std::array<ID3D11ClassInstance *, 256> saved_vertex_instances {};
    UINT saved_vertex_instance_count = static_cast<UINT>(saved_vertex_instances.size());
    context->VSGetShader(&saved_vertex_shader, saved_vertex_instances.data(), &saved_vertex_instance_count);
    ID3D11PixelShader *saved_pixel_shader = nullptr;
    std::array<ID3D11ClassInstance *, 256> saved_pixel_instances {};
    UINT saved_pixel_instance_count = static_cast<UINT>(saved_pixel_instances.size());
    context->PSGetShader(&saved_pixel_shader, saved_pixel_instances.data(), &saved_pixel_instance_count);
    ID3D11ShaderResourceView *saved_source_view = nullptr;
    context->PSGetShaderResources(0, 1, &saved_source_view);
    ID3D11SamplerState *saved_sampler = nullptr;
    context->PSGetSamplers(0, 1, &saved_sampler);
    ID3D11Buffer *saved_constant_buffer = nullptr;
    context->PSGetConstantBuffers(0, 1, &saved_constant_buffer);
    ID3D11BlendState *saved_blend_state = nullptr;
    FLOAT saved_blend_factor[4] {};
    UINT saved_sample_mask = 0;
    context->OMGetBlendState(&saved_blend_state, saved_blend_factor, &saved_sample_mask);
    ID3D11DepthStencilState *saved_depth_state = nullptr;
    UINT saved_stencil_ref = 0;
    context->OMGetDepthStencilState(&saved_depth_state, &saved_stencil_ref);
    ID3D11RasterizerState *saved_rasterizer_state = nullptr;
    context->RSGetState(&saved_rasterizer_state);

    struct ToneMapConstants
    {
        float paper_white;
        float peak;
        float pq_input;
        float padding;
    } constants {
        static_cast<float>(g_config.hdr_sdr_tone_map_paper_white),
        static_cast<float>(std::max(g_config.hdr_sdr_tone_map_peak, g_config.hdr_sdr_tone_map_paper_white)),
        g_config.hdr_sdr_tone_map_pq_input ? 1.0f : 0.0f,
        0.0f,
    };

    {
        ScopedInternalBridgeDispatch internal_dispatch_scope;
        ScopedContextVtableBypass context_vtable_bypass(context);
        if (g_original_om_set_render_targets != nullptr)
            g_original_om_set_render_targets(context, 0, nullptr, nullptr);
        else
            context->OMSetRenderTargets(0, nullptr, nullptr);
        if (g_original_copy_resource != nullptr)
            g_original_copy_resource(context, resources.source_copy, target_texture);
        else
            context->CopyResource(resources.source_copy, target_texture);
        context->UpdateSubresource(resources.constants, 0, nullptr, &constants, 0, 0);
        if (g_original_om_set_render_targets != nullptr)
            g_original_om_set_render_targets(context, 1, &resources.target_view, nullptr);
        else
            context->OMSetRenderTargets(1, &resources.target_view, nullptr);
        const D3D11_VIEWPORT viewport {
            0.0f, 0.0f,
            static_cast<float>(resources.width), static_cast<float>(resources.height),
            0.0f, 1.0f,
        };
        if (g_original_rs_set_viewports != nullptr)
            g_original_rs_set_viewports(context, 1, &viewport);
        else
            context->RSSetViewports(1, &viewport);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (g_original_vs_set_shader != nullptr)
            g_original_vs_set_shader(context, resources.vertex_shader, nullptr, 0);
        else
            context->VSSetShader(resources.vertex_shader, nullptr, 0);
        if (g_original_ps_set_shader != nullptr)
            g_original_ps_set_shader(context, resources.pixel_shader, nullptr, 0);
        else
            context->PSSetShader(resources.pixel_shader, nullptr, 0);
        if (g_original_ps_set_shader_resources != nullptr)
            g_original_ps_set_shader_resources(context, 0, 1, &resources.source_view);
        else
            context->PSSetShaderResources(0, 1, &resources.source_view);
        context->PSSetSamplers(0, 1, &resources.sampler);
        context->PSSetConstantBuffers(0, 1, &resources.constants);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
        context->OMSetDepthStencilState(nullptr, 0);
        context->RSSetState(nullptr);
        if (g_original_draw != nullptr)
            g_original_draw(context, 3, 0);
        else
            context->Draw(3, 0);
        ID3D11ShaderResourceView *null_view = nullptr;
        if (g_original_ps_set_shader_resources != nullptr)
            g_original_ps_set_shader_resources(context, 0, 1, &null_view);
        else
            context->PSSetShaderResources(0, 1, &null_view);
        if (g_original_om_set_render_targets != nullptr)
            g_original_om_set_render_targets(context, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                saved_render_targets, saved_depth_stencil);
        else
            context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                saved_render_targets, saved_depth_stencil);
        if (saved_viewport_count != 0)
        {
            if (g_original_rs_set_viewports != nullptr)
                g_original_rs_set_viewports(context, saved_viewport_count, saved_viewports);
            else
                context->RSSetViewports(saved_viewport_count, saved_viewports);
        }
        context->IASetInputLayout(saved_input_layout);
        context->IASetPrimitiveTopology(saved_topology);
        if (g_original_vs_set_shader != nullptr)
            g_original_vs_set_shader(context, saved_vertex_shader, saved_vertex_instances.data(), saved_vertex_instance_count);
        else
            context->VSSetShader(saved_vertex_shader, saved_vertex_instances.data(), saved_vertex_instance_count);
        if (g_original_ps_set_shader != nullptr)
            g_original_ps_set_shader(context, saved_pixel_shader, saved_pixel_instances.data(), saved_pixel_instance_count);
        else
            context->PSSetShader(saved_pixel_shader, saved_pixel_instances.data(), saved_pixel_instance_count);
        if (g_original_ps_set_shader_resources != nullptr)
            g_original_ps_set_shader_resources(context, 0, 1, &saved_source_view);
        else
            context->PSSetShaderResources(0, 1, &saved_source_view);
        context->PSSetSamplers(0, 1, &saved_sampler);
        context->PSSetConstantBuffers(0, 1, &saved_constant_buffer);
        context->OMSetBlendState(saved_blend_state, saved_blend_factor, saved_sample_mask);
        context->OMSetDepthStencilState(saved_depth_state, saved_stencil_ref);
        context->RSSetState(saved_rasterizer_state);
    }

    if (saved_input_layout != nullptr)
        saved_input_layout->Release();
    if (saved_vertex_shader != nullptr)
        saved_vertex_shader->Release();
    for (ID3D11ClassInstance *instance : saved_vertex_instances)
    {
        if (instance != nullptr)
            instance->Release();
    }
    if (saved_pixel_shader != nullptr)
        saved_pixel_shader->Release();
    for (ID3D11ClassInstance *instance : saved_pixel_instances)
    {
        if (instance != nullptr)
            instance->Release();
    }
    if (saved_source_view != nullptr)
        saved_source_view->Release();
    if (saved_sampler != nullptr)
        saved_sampler->Release();
    if (saved_constant_buffer != nullptr)
        saved_constant_buffer->Release();
    if (saved_blend_state != nullptr)
        saved_blend_state->Release();
    if (saved_depth_state != nullptr)
        saved_depth_state->Release();
    if (saved_rasterizer_state != nullptr)
        saved_rasterizer_state->Release();
    for (ID3D11RenderTargetView *render_target : saved_render_targets)
    {
        if (render_target != nullptr)
            render_target->Release();
    }
    if (saved_depth_stencil != nullptr)
        saved_depth_stencil->Release();

    if (!g_hdr_sdr_tone_map_logged.exchange(true, std::memory_order_relaxed))
    {
        log_line("hdr_sdr_tone_map_active frame=" + std::to_string(frame_index) +
            " paper_white=" + std::to_string(g_config.hdr_sdr_tone_map_paper_white) +
            " peak=" + std::to_string(g_config.hdr_sdr_tone_map_peak) +
            " pq_input=" + std::to_string(g_config.hdr_sdr_tone_map_pq_input ? 1 : 0));
    }

    target_texture->Release();
    bound_target->Release();
    context->Release();
    device->Release();
}

HRESULT STDMETHODCALLTYPE hooked_present(IDXGISwapChain *swapchain, UINT sync_interval, UINT flags)
{
#if defined(DX11FSRBRIDGE_SERVER_DEBUG_RUNTIME) && defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
    static std::uint32_t last_fsr2_query_mask = UINT32_MAX;
    const std::uint32_t fsr2_query_mask = fsr2_get_proc_address_shim_query_mask();
    if (fsr2_query_mask != last_fsr2_query_mask)
    {
        last_fsr2_query_mask = fsr2_query_mask;
        log_line("server_debug_fsr2_queries mask=" + hex64(fsr2_query_mask) +
            " create=" + std::to_string((fsr2_query_mask & (1u << 0)) != 0) +
            " dispatch=" + std::to_string((fsr2_query_mask & (1u << 1)) != 0) +
            " destroy=" + std::to_string((fsr2_query_mask & (1u << 2)) != 0) +
            " ratio=" + std::to_string((fsr2_query_mask & (1u << 3)) != 0) +
            " resolution=" + std::to_string((fsr2_query_mask & (1u << 4)) != 0) +
            " jitter=" + std::to_string((fsr2_query_mask & (1u << 5)) != 0));
    }
#endif
#if defined(DX11FSRBRIDGE_FINAL_SCENE_PROBE)
    update_final_scene_probe_backbuffers(swapchain);
#endif
    DXGI_SWAP_CHAIN_DESC desc {};
    const bool desc_available = SUCCEEDED(swapchain->GetDesc(&desc));
    std::uint64_t frame_index = 0;
    std::uint32_t backbuffer_width = 0;
    std::uint32_t backbuffer_height = 0;
    {
        std::lock_guard lock(g_state_mutex);
        if (desc_available)
        {
            g_state.backbuffer_width = desc.BufferDesc.Width;
            g_state.backbuffer_height = desc.BufferDesc.Height;
            g_state.frame_index++;
            g_state.candidate_count = 0;
        }
        frame_index = g_state.frame_index;
        backbuffer_width = g_state.backbuffer_width;
        backbuffer_height = g_state.backbuffer_height;
    }

#if defined(DX11FSRBRIDGE_FINAL_SCENE_PROBE)
    flush_final_scene_probe(frame_index);
    poll_final_scene_snapshot(swapchain);
#endif

    if (g_logging_enabled.load(std::memory_order_relaxed))
        log_line("present frame=" + std::to_string(frame_index) +
            " size=" + std::to_string(backbuffer_width) + "x" + std::to_string(backbuffer_height));
#if defined(DX11FSRBRIDGE_RELEASE_RUNTIME) && defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
    static std::atomic_uint64_t last_runtime_status_tick { 0 };
    const ULONGLONG now = GetTickCount64();
    std::uint64_t last_tick = last_runtime_status_tick.load(std::memory_order_relaxed);
    if (now - last_tick >= 5000 &&
        last_runtime_status_tick.compare_exchange_strong(last_tick, now, std::memory_order_relaxed))
    {
        log_line("fsr2_runtime_status frame=" + std::to_string(frame_index) +
            " candidates=" + std::to_string(g_fsr2_translation_candidate_count.load(std::memory_order_relaxed)) +
            " dispatches=" + std::to_string(g_fsr2_translation_dispatch_count.load(std::memory_order_relaxed)) +
            " failures=" + std::to_string(g_fsr2_translation_failure_count.load(std::memory_order_relaxed))); // ：旧 shim 已移除
    }
#endif
    tone_map_hdr_backbuffer_to_sdr(swapchain, frame_index);
    present_fn original_present = g_original_present;
    {
        std::lock_guard lock(g_swapchain_present_mutex);
        const auto it = g_original_present_by_instance.find(swapchain);
        if (it != g_original_present_by_instance.end())
            original_present = it->second;
    }
    return original_present != nullptr ? original_present(swapchain, sync_interval, flags) : E_FAIL;
}

HRESULT STDMETHODCALLTYPE hooked_set_fullscreen_state(IDXGISwapChain *swapchain, BOOL fullscreen, IDXGIOutput *target)
{
    if (!fullscreen && dlssg_dxgi_workaround_active())
    {
        static std::atomic_uint64_t skip_count = 0;
        const std::uint64_t count = skip_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count == 1 || count % 16 == 0)
        {
            log_line("dlssg_dxgi_workaround skip_set_fullscreen_state fullscreen=0 count=" +
                std::to_string(count) + " target=" + hex64(reinterpret_cast<std::uint64_t>(target)));
        }
        return S_OK;
    }

    return g_original_set_fullscreen_state != nullptr
        ? g_original_set_fullscreen_state(swapchain, fullscreen, target)
        : DXGI_ERROR_INVALID_CALL;
}

HRESULT STDMETHODCALLTYPE hooked_get_fullscreen_state(IDXGISwapChain *swapchain, BOOL *fullscreen, IDXGIOutput **target)
{
    return g_original_get_fullscreen_state != nullptr
        ? g_original_get_fullscreen_state(swapchain, fullscreen, target)
        : DXGI_ERROR_INVALID_CALL;
}

HRESULT STDMETHODCALLTYPE hooked_resize_buffers(
    IDXGISwapChain *swapchain,
    UINT buffer_count,
    UINT width,
    UINT height,
    DXGI_FORMAT format,
    UINT flags)
{
    DXGI_FORMAT effective_format = format;
    if (g_config.native_ldr_swapchain_unorm && format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
    {
        effective_format = DXGI_FORMAT_R8G8B8A8_UNORM;
        log_line("native_ldr_resize_buffers_format from=29/R8G8B8A8_UNORM_SRGB to=28/R8G8B8A8_UNORM");
    }
    if (dlssg_dxgi_workaround_active() && swapchain != nullptr)
    {
        DXGI_SWAP_CHAIN_DESC current {};
        if (SUCCEEDED(swapchain->GetDesc(&current)))
        {
            const bool unchanged =
                (buffer_count == 0 || buffer_count == current.BufferCount) &&
                (width == 0 || width == current.BufferDesc.Width) &&
                (height == 0 || height == current.BufferDesc.Height) &&
                (format == DXGI_FORMAT_UNKNOWN || format == current.BufferDesc.Format) &&
                flags == current.Flags;
            if (unchanged)
            {
                static std::atomic_uint64_t skip_count = 0;
                const std::uint64_t count = skip_count.fetch_add(1, std::memory_order_relaxed) + 1;
                if (count == 1 || count % 16 == 0)
                {
                    log_line("dlssg_dxgi_workaround skip_noop_resize_buffers count=" +
                        std::to_string(count) + " request=" +
                        std::to_string(buffer_count) + "/" + std::to_string(width) + "x" +
                        std::to_string(height) + " fmt=" + std::to_string(static_cast<UINT>(format)) +
                        " flags=" + std::to_string(flags));
                }
                return S_OK;
            }
        }
    }

    resize_buffers_fn original_resize = nullptr;
    {
        std::lock_guard lock(g_swapchain_resize_mutex);
        const auto it = g_original_resize_buffers_by_instance.find(swapchain);
        if (it != g_original_resize_buffers_by_instance.end())
            original_resize = it->second;
    }
    if (original_resize == nullptr)
        original_resize = g_original_resize_buffers;

    const HRESULT result = original_resize != nullptr
        ? original_resize(swapchain, buffer_count, width, height, effective_format, flags)
        : DXGI_ERROR_INVALID_CALL;
    if (SUCCEEDED(result))
    {
        update_swapchain_backbuffer_resources(swapchain);
        apply_hdr_swapchain_force(swapchain);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE hooked_resize_target(IDXGISwapChain *swapchain, const DXGI_MODE_DESC *target_parameters)
{
    if (dlssg_dxgi_workaround_active())
    {
        static std::atomic_uint64_t skip_count = 0;
        const std::uint64_t count = skip_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count == 1 || count % 16 == 0)
        {
            std::string detail = "null";
            if (target_parameters != nullptr)
            {
                detail = std::to_string(target_parameters->Width) + "x" +
                    std::to_string(target_parameters->Height) + " fmt=" +
                    std::to_string(static_cast<UINT>(target_parameters->Format));
            }
            log_line("dlssg_dxgi_workaround skip_resize_target count=" +
                std::to_string(count) + " target=" + detail);
        }
        return S_OK;
    }

    return g_original_resize_target != nullptr
        ? g_original_resize_target(swapchain, target_parameters)
        : DXGI_ERROR_INVALID_CALL;
}

bool hdr_swapchain_spoof_active()
{
    return g_config.hdr_swapchain_spoof;
}

bool hdr_swapchain_force_active()
{
    return g_config.hdr_swapchain_force;
}

bool is_hdr10_color_space(DXGI_COLOR_SPACE_TYPE color_space)
{
    return color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
        color_space == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020;
}

HRESULT STDMETHODCALLTYPE hooked_check_color_space_support(
    IDXGISwapChain3 *swapchain,
    DXGI_COLOR_SPACE_TYPE color_space,
    UINT *support)
{
    void *const caller = _ReturnAddress();
    const HRESULT physical_hr = g_original_check_color_space_support != nullptr
        ? g_original_check_color_space_support(swapchain, color_space, support)
        : DXGI_ERROR_INVALID_CALL;
    const bool spoof = hdr_swapchain_spoof_active() && is_hdr10_color_space(color_space);
    if (spoof && support != nullptr)
        *support |= DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT;

#if defined(DX11FSRBRIDGE_COLOR_DIAGNOSTICS)
    const bool should_log = true;
#else
    const bool should_log = spoof;
#endif
    if (should_log)
    {
        log_line("dxgi_color_check swapchain=" + hex64(reinterpret_cast<std::uintptr_t>(swapchain)) +
            " caller=" + module_path_from_address(caller) +
            " requested=" + std::to_string(static_cast<unsigned>(color_space)) + "/" + color_space_name(color_space) +
            " physical_hr=" + hex64(static_cast<std::uint32_t>(physical_hr)) +
            " support=" + (support != nullptr ? hex64(*support) : std::string("null")) +
            " spoof=" + std::to_string(spoof ? 1 : 0));
    }
    return spoof ? S_OK : physical_hr;
}

HRESULT STDMETHODCALLTYPE hooked_set_color_space1(
    IDXGISwapChain3 *swapchain,
    DXGI_COLOR_SPACE_TYPE color_space)
{
    void *const caller = _ReturnAddress();
    const bool spoof = hdr_swapchain_spoof_active() && is_hdr10_color_space(color_space);
    const DXGI_COLOR_SPACE_TYPE physical_color_space = spoof
        ? DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
        : color_space;
    const HRESULT physical_hr = g_original_set_color_space1 != nullptr
        ? g_original_set_color_space1(swapchain, physical_color_space)
        : DXGI_ERROR_INVALID_CALL;

#if defined(DX11FSRBRIDGE_COLOR_DIAGNOSTICS)
    const bool should_log = true;
#else
    const bool should_log = spoof;
#endif
    if (should_log)
    {
        log_line("dxgi_color_set swapchain=" + hex64(reinterpret_cast<std::uintptr_t>(swapchain)) +
            " caller=" + module_path_from_address(caller) +
            " requested=" + std::to_string(static_cast<unsigned>(color_space)) + "/" + color_space_name(color_space) +
            " physical=" + std::to_string(static_cast<unsigned>(physical_color_space)) + "/" + color_space_name(physical_color_space) +
            " physical_hr=" + hex64(static_cast<std::uint32_t>(physical_hr)) +
            " spoof=" + std::to_string(spoof ? 1 : 0));
    }
    return spoof ? S_OK : physical_hr;
}

HRESULT STDMETHODCALLTYPE hooked_set_hdr_metadata(
    IDXGISwapChain4 *swapchain,
    DXGI_HDR_METADATA_TYPE type,
    UINT size,
    void *metadata)
{
    void *const caller = _ReturnAddress();
    const bool spoof = hdr_swapchain_spoof_active() && type == DXGI_HDR_METADATA_TYPE_HDR10;
    const HRESULT physical_hr = spoof
        ? S_OK
        : (g_original_set_hdr_metadata != nullptr
            ? g_original_set_hdr_metadata(swapchain, type, size, metadata)
            : DXGI_ERROR_INVALID_CALL);

    std::ostringstream message;
    message << "dxgi_hdr_metadata swapchain=" << hex64(reinterpret_cast<std::uintptr_t>(swapchain))
        << " caller=" << module_path_from_address(caller)
        << " type=" << static_cast<unsigned>(type) << "/" << hdr_metadata_type_name(type)
        << " size=" << size
        << " physical_hr=" << hex64(static_cast<std::uint32_t>(physical_hr))
        << " spoof=" << (spoof ? 1 : 0);
    if (type == DXGI_HDR_METADATA_TYPE_HDR10 && metadata != nullptr && size >= sizeof(DXGI_HDR_METADATA_HDR10))
    {
        const auto &hdr10 = *static_cast<const DXGI_HDR_METADATA_HDR10 *>(metadata);
        message << " red=" << hdr10.RedPrimary[0] << "," << hdr10.RedPrimary[1]
            << " green=" << hdr10.GreenPrimary[0] << "," << hdr10.GreenPrimary[1]
            << " blue=" << hdr10.BluePrimary[0] << "," << hdr10.BluePrimary[1]
            << " white=" << hdr10.WhitePoint[0] << "," << hdr10.WhitePoint[1]
            << " max_mastering=" << hdr10.MaxMasteringLuminance
            << " min_mastering=" << hdr10.MinMasteringLuminance
            << " max_cll=" << hdr10.MaxContentLightLevel
            << " max_fall=" << hdr10.MaxFrameAverageLightLevel;
    }
#if defined(DX11FSRBRIDGE_COLOR_DIAGNOSTICS)
    const bool should_log = true;
#else
    const bool should_log = spoof;
#endif
    if (should_log)
        log_line(message.str());
    return spoof ? S_OK : physical_hr;
}

void apply_hdr_swapchain_force(IDXGISwapChain *swapchain)
{
    if (!hdr_swapchain_force_active() || swapchain == nullptr)
        return;

    IDXGISwapChain3 *swapchain3 = nullptr;
    if (FAILED(swapchain->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void **>(&swapchain3))) ||
        swapchain3 == nullptr)
    {
        log_line("dxgi_hdr_force unavailable reason=no_IDXGISwapChain3");
        return;
    }

    constexpr DXGI_COLOR_SPACE_TYPE requested_color_space = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    UINT support = 0;
    const HRESULT check_hr = swapchain3->CheckColorSpaceSupport(requested_color_space, &support);
    HRESULT set_hr = S_FALSE;
    if (SUCCEEDED(check_hr) && (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0)
        set_hr = swapchain3->SetColorSpace1(requested_color_space);

    log_line("dxgi_hdr_force requested=" + std::to_string(static_cast<unsigned>(requested_color_space)) + "/" +
        color_space_name(requested_color_space) +
        " check_hr=" + hex64(static_cast<std::uint32_t>(check_hr)) +
        " support=" + hex64(support) +
        " set_hr=" + hex64(static_cast<std::uint32_t>(set_hr)));
    swapchain3->Release();
}

void STDMETHODCALLTYPE hooked_ps_set_shader_resources(ID3D11DeviceContext *context, UINT start_slot, UINT count, ID3D11ShaderResourceView *const *views)
{
#if defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    if (g_config.fsr2_translation_mode == 2 && g_config.fsr2_mode2_on_demand_state)
    {
        g_original_ps_set_shader_resources(context, start_slot, count, views);
        return;
    }
    if (g_config.fsr2_fast_state_tracking && g_config.fsr2_translation_mode == 2 &&
        g_mode2_fast_target_ps_hash.load(std::memory_order_relaxed) != 0)
    {
        g_original_ps_set_shader_resources(context, start_slot, count, views);
        return;
    }
    constexpr UINT tracked_slot_count = 7;
    if (start_slot < tracked_slot_count)
    {
        const UINT tracked_count = std::min(count, tracked_slot_count - start_slot);
        std::lock_guard lock(g_state_mutex);
        update_view_array(g_state.ps_srvs, start_slot, tracked_count, views, L"ps_srv");
    }
#else
    {
        std::lock_guard lock(g_state_mutex);
        update_view_array(g_state.ps_srvs, start_slot, count, views, L"ps_srv");
    }
#endif
    g_original_ps_set_shader_resources(context, start_slot, count, views);
}

void STDMETHODCALLTYPE hooked_vs_set_shader(ID3D11DeviceContext *context, ID3D11VertexShader *shader, ID3D11ClassInstance *const *class_instances, UINT class_instances_count)
{
#if defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    if (g_config.fsr2_translation_mode == 2 && g_config.fsr2_mode2_on_demand_state)
    {
        g_original_vs_set_shader(context, shader, class_instances, class_instances_count);
        return;
    }
#endif
    const ShaderInfo shader_info = lookup_vertex_shader_info(shader);
    {
        std::lock_guard lock(g_state_mutex);
        g_state.current_vs_shader = reinterpret_cast<std::uint64_t>(shader);
        g_state.current_vs_hash = shader_info.hash;
        g_state.current_vs_size = shader_info.bytecode_size;
    }
    g_original_vs_set_shader(context, shader, class_instances, class_instances_count);
}

void STDMETHODCALLTYPE hooked_vs_set_constant_buffers(ID3D11DeviceContext *context, UINT start_slot, UINT count, ID3D11Buffer *const *buffers)
{
#if defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    if (g_config.fsr2_translation_mode == 2 && g_config.fsr2_mode2_on_demand_state)
    {
        g_original_vs_set_constant_buffers(context, start_slot, count, buffers);
        return;
    }
#endif
    {
        std::lock_guard lock(g_state_mutex);
        update_constant_buffer_array(g_state.vs_cbs, start_slot, count, buffers);
    }
    g_original_vs_set_constant_buffers(context, start_slot, count, buffers);
}

void STDMETHODCALLTYPE hooked_ps_set_shader(ID3D11DeviceContext *context, ID3D11PixelShader *shader, ID3D11ClassInstance *const *class_instances, UINT class_instances_count)
{
#if defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    if (g_config.fsr2_translation_mode == 2 && g_config.fsr2_mode2_on_demand_state)
    {
        g_original_ps_set_shader(context, shader, class_instances, class_instances_count);
        return;
    }
    const std::uint64_t fast_target_hash = g_mode2_fast_target_ps_hash.load(std::memory_order_relaxed);
    if (g_config.fsr2_fast_state_tracking && g_config.fsr2_translation_mode == 2 && fast_target_hash != 0)
    {
        const std::uint64_t shader_key = reinterpret_cast<std::uint64_t>(shader);
        const std::uint64_t target_key = g_mode2_fast_target_ps_key.load(std::memory_order_relaxed);
        g_current_ps_hash.store(shader_key == target_key ? fast_target_hash : 0, std::memory_order_relaxed);
        g_original_ps_set_shader(context, shader, class_instances, class_instances_count);
        return;
    }
#endif
    const ShaderInfo shader_info = lookup_pixel_shader_info(shader);
    g_current_ps_hash.store(shader_info.hash, std::memory_order_relaxed);
    {
        std::lock_guard lock(g_state_mutex);
        g_state.current_ps_shader = reinterpret_cast<std::uint64_t>(shader);
        g_state.current_ps_hash = shader_info.hash;
        g_state.current_ps_size = shader_info.bytecode_size;
    }
    g_original_ps_set_shader(context, shader, class_instances, class_instances_count);
}

void STDMETHODCALLTYPE hooked_ps_set_constant_buffers(ID3D11DeviceContext *context, UINT start_slot, UINT count, ID3D11Buffer *const *buffers)
{
#if defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    if (g_config.fsr2_translation_mode == 2 && g_config.fsr2_mode2_on_demand_state)
    {
        g_original_ps_set_constant_buffers(context, start_slot, count, buffers);
        return;
    }
    if (g_config.fsr2_fast_state_tracking && g_config.fsr2_translation_mode == 2 &&
        g_mode2_fast_target_ps_hash.load(std::memory_order_relaxed) != 0)
    {
        g_original_ps_set_constant_buffers(context, start_slot, count, buffers);
        return;
    }
    if (start_slot == 0 && count != 0)
    {
        std::lock_guard lock(g_state_mutex);
        update_constant_buffer_array(g_state.ps_cbs, 0, 1, buffers);
    }
#else
    {
        std::lock_guard lock(g_state_mutex);
        update_constant_buffer_array(g_state.ps_cbs, start_slot, count, buffers);
    }
#endif
    g_original_ps_set_constant_buffers(context, start_slot, count, buffers);
}

void STDMETHODCALLTYPE hooked_cs_set_shader_resources(ID3D11DeviceContext *context, UINT start_slot, UINT count, ID3D11ShaderResourceView *const *views)
{
#if defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    if (g_config.fsr2_translation_mode == 2 && g_config.fsr2_mode2_on_demand_state)
    {
        g_original_cs_set_shader_resources(context, start_slot, count, views);
        return;
    }
#endif
    {
        std::lock_guard lock(g_state_mutex);
        update_view_array(g_state.cs_srvs, start_slot, count, views, L"cs_srv");
    }
    g_original_cs_set_shader_resources(context, start_slot, count, views);
}

void STDMETHODCALLTYPE hooked_cs_set_uavs(ID3D11DeviceContext *context, UINT start_slot, UINT count, ID3D11UnorderedAccessView *const *views, const UINT *initial_counts)
{
#if defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    if (g_config.fsr2_translation_mode == 2 && g_config.fsr2_mode2_on_demand_state)
    {
        g_original_cs_set_uavs(context, start_slot, count, views, initial_counts);
        return;
    }
#endif
    {
        std::lock_guard lock(g_state_mutex);
        update_uav_array(g_state.cs_uavs, start_slot, count, views);
    }
    g_original_cs_set_uavs(context, start_slot, count, views, initial_counts);
}

void STDMETHODCALLTYPE hooked_cs_set_shader(ID3D11DeviceContext *context, ID3D11ComputeShader *shader, ID3D11ClassInstance *const *class_instances, UINT class_instances_count)
{
#if defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    if (g_config.fsr2_translation_mode == 2 && g_config.fsr2_mode2_on_demand_state)
    {
        g_original_cs_set_shader(context, shader, class_instances, class_instances_count);
        return;
    }
#endif
    const ShaderInfo shader_info = lookup_compute_shader_info(shader);
    {
        std::lock_guard lock(g_state_mutex);
        g_state.current_cs_shader = reinterpret_cast<std::uint64_t>(shader);
        g_state.current_cs_hash = shader_info.hash;
        g_state.current_cs_size = shader_info.bytecode_size;
    }
    g_original_cs_set_shader(context, shader, class_instances, class_instances_count);
}

void STDMETHODCALLTYPE hooked_cs_set_constant_buffers(ID3D11DeviceContext *context, UINT start_slot, UINT count, ID3D11Buffer *const *buffers)
{
#if defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    if (g_config.fsr2_translation_mode == 2 && g_config.fsr2_mode2_on_demand_state)
    {
        g_original_cs_set_constant_buffers(context, start_slot, count, buffers);
        return;
    }
#endif
    {
        std::lock_guard lock(g_state_mutex);
        update_constant_buffer_array(g_state.cs_cbs, start_slot, count, buffers);
    }
    g_original_cs_set_constant_buffers(context, start_slot, count, buffers);
}

void ensure_context_device_texture_hook(ID3D11DeviceContext *context)
{
    if (!g_config.native_ldr_final_target_unorm || context == nullptr)
        return;

    ID3D11Device *device = nullptr;
    context->GetDevice(&device);
    if (device == nullptr)
        return;

    const std::uint64_t device_key = reinterpret_cast<std::uint64_t>(device);
    bool first_attempt = false;
    {
        std::lock_guard lock(g_context_device_hook_mutex);
        first_attempt = g_context_device_hook_attempts.insert(device_key).second;
    }
    if (first_attempt)
    {
        void **device_vtable = *reinterpret_cast<void ***>(device);
        log_line("device_create_texture_entry device=" + hex64(device_key) +
            " entry=" + hex64(reinterpret_cast<std::uintptr_t>(device_vtable[k_idx_device_create_texture_2d])) +
            " expected=" + hex64(reinterpret_cast<std::uintptr_t>(&hooked_create_texture_2d)));
        install_device_hooks(device);
        log_line("context_device_texture_hook device=" + hex64(device_key));
    }
    device->Release();
}

void STDMETHODCALLTYPE hooked_om_set_render_targets(ID3D11DeviceContext *context, UINT count, ID3D11RenderTargetView *const *rtvs, ID3D11DepthStencilView *dsv)
{
    ensure_context_device_texture_hook(context);
#if defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    if (g_config.fsr2_translation_mode == 2 && g_config.fsr2_mode2_on_demand_state)
    {
        g_original_om_set_render_targets(context, count, rtvs, dsv);
        return;
    }
    if (g_config.fsr2_fast_state_tracking && g_config.fsr2_translation_mode == 2 &&
        g_mode2_fast_target_ps_hash.load(std::memory_order_relaxed) != 0)
    {
        g_original_om_set_render_targets(context, count, rtvs, dsv);
        return;
    }
#endif
    ResourceInfo first_target {};
    {
        std::lock_guard lock(g_state_mutex);
        for (std::size_t i = 0; i < g_state.rtvs.size(); ++i)
            g_state.rtvs[i] = {};
#if defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
        for (UINT i = 0; i < count && i < 2; ++i)
            read_resource_info(rtvs[i], L"rtv", g_state.rtvs[i]);
#else
        for (UINT i = 0; i < count && i < g_state.rtvs.size(); ++i)
            read_resource_info(rtvs[i], L"rtv", g_state.rtvs[i]);
        read_resource_info(dsv, L"dsv", g_state.dsv);
#endif
        first_target = g_state.rtvs[0];
    }
    record_hdr_composite_target_bind(first_target);
    g_original_om_set_render_targets(context, count, rtvs, dsv);
}

void STDMETHODCALLTYPE hooked_om_set_render_targets_and_uavs(
    ID3D11DeviceContext *context,
    UINT render_target_count,
    ID3D11RenderTargetView *const *render_targets,
    ID3D11DepthStencilView *depth_stencil,
    UINT uav_start_slot,
    UINT uav_count,
    ID3D11UnorderedAccessView *const *unordered_access_views,
    const UINT *uav_initial_counts)
{
    ensure_context_device_texture_hook(context);
    if (g_config.dx11_on12_swapchain)
    {
        ResourceInfo first_target {};
        {
            std::lock_guard lock(g_state_mutex);
            for (std::size_t i = 0; i < g_state.rtvs.size(); ++i)
                g_state.rtvs[i] = {};
            for (UINT i = 0; i < render_target_count && i < g_state.rtvs.size(); ++i)
                read_resource_info(render_targets[i], L"rtv_uav", g_state.rtvs[i]);
            read_resource_info(depth_stencil, L"dsv_uav", g_state.dsv);
            first_target = g_state.rtvs[0];
        }
        record_hdr_composite_target_bind(first_target);
    }
    g_original_om_set_render_targets_and_uavs(
        context,
        render_target_count,
        render_targets,
        depth_stencil,
        uav_start_slot,
        uav_count,
        unordered_access_views,
        uav_initial_counts);
}

void STDMETHODCALLTYPE hooked_dispatch(ID3D11DeviceContext *context, UINT group_x, UINT group_y, UINT group_z)
{
    if (g_internal_bridge_dispatch)
    {
        g_original_dispatch(context, group_x, group_y, group_z);
        return;
    }

    record_color_source_call("dispatch", group_x, group_y, group_z);

    if (g_config.log_all_dispatch)
        log_line("dispatch groups=" + std::to_string(group_x) + "x" + std::to_string(group_y) + "x" + std::to_string(group_z));

    if (g_config.log_interesting_dispatch_details && should_log_interesting_dispatch(group_x, group_y, group_z))
        log_interesting_dispatch_details(group_x, group_y, group_z);

    if (const auto candidate = build_dispatch_candidate(group_x, group_y, group_z))
    {
        log_line("fsr_candidate frame=" + std::to_string(candidate->frame_index) +
            " render=" + std::to_string(candidate->render_width) + "x" + std::to_string(candidate->render_height) +
            " output=" + std::to_string(candidate->output_width) + "x" + std::to_string(candidate->output_height) +
            " cs=" + hex64(candidate->compute_shader) +
            " color=" + hex64(candidate->color.resource_key) +
            " motion=" + hex64(candidate->motion.resource_key) +
            " depth=" + hex64(candidate->depth.resource_key) +
            " out=" + hex64(candidate->output.resource_key));
    }

#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    record_similarity_dispatch(group_x, group_y, group_z);
#endif
    g_original_dispatch(context, group_x, group_y, group_z);
}

struct TargetUpscalerDrawInfo
{
    std::uint32_t render_width = 0;
    std::uint32_t render_height = 0;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    std::uint64_t constant_buffer_key = 0;
    std::uint64_t color_resource_key = 0;
    std::uint64_t motion_resource_key = 0;
    std::uint32_t color_srv_slot = 0;
    std::uint32_t depth_srv_slot = 2;
    std::uint32_t motion_srv_slot = 3;
    std::uint32_t transparency_srv_slot = UINT_MAX;
    std::uint32_t output_rtv_slot = 1;
};

// 接管路径指纹缓存（正缓存）：缓存"识别成功"的资源特征指纹 → 布局。
// 键 = 特征值（尺寸/格式/槽位组合，非指针）——UI 切换/视图地址复用无关。
// 命中：按缓存布局读少量视图验证指纹 → 直接构造 info（省 9 视图完整读取）。
struct UpscalerPathCacheEntry
{
    std::uint32_t color_slot = 0;
    std::uint32_t depth_slot = 2;
    std::uint32_t motion_slot = 3;
    std::uint32_t transparency_slot = UINT_MAX;
    std::uint32_t output_rtv_slot = 1;
    std::uint32_t output_w = 0;
    std::uint32_t output_h = 0;
    std::uint32_t output_fmt = 0;
    std::uint64_t last_hit_tick = 0;
};
std::mutex g_upscaler_path_cache_mutex;
std::unordered_map<std::uint64_t, UpscalerPathCacheEntry> g_upscaler_path_cache;

std::uint64_t upscaler_path_fingerprint(
    const std::array<ResourceInfo, 7> &inputs,
    const std::array<ResourceInfo, 2> &outputs,
    const TargetUpscalerDrawInfo &info)
{
    std::uint64_t h = 14695981039346656037ull;
    const auto mix = [&h](std::uint64_t v) { h ^= v; h *= 1099511628211ull; };
    const auto mix_info = [&mix](const ResourceInfo &r)
    {
        mix(static_cast<std::uint64_t>(r.width));
        mix(static_cast<std::uint64_t>(r.height));
        mix(static_cast<std::uint64_t>(r.format));
    };
    mix(static_cast<std::uint64_t>(info.color_srv_slot));
    mix(static_cast<std::uint64_t>(info.motion_srv_slot));
    mix(static_cast<std::uint64_t>(info.depth_srv_slot));
    mix(static_cast<std::uint64_t>(info.output_rtv_slot));
    mix_info(inputs[info.color_srv_slot]);
    mix_info(inputs[info.motion_srv_slot]);
    mix_info(inputs[info.depth_srv_slot]);
    mix_info(outputs[0]);
    mix_info(outputs[1]);
    return h;
}

// 快速路径：正缓存命中（output 特征匹配 + 按缓存布局读 3 SRV 指纹验证）→ 直接构造 info。
std::optional<TargetUpscalerDrawInfo> try_upscaler_path_cache_fast(
    ID3D11RenderTargetView *const *render_targets,
    ID3D11ShaderResourceView *const *shader_resources)
{
    ResourceInfo out0 {};
    read_resource_info(render_targets[0], L"fsr2_path_out", out0);
    if (out0.resource_key == 0)
        return std::nullopt;

    std::lock_guard lock(g_upscaler_path_cache_mutex);
    for (auto &[fingerprint, entry] : g_upscaler_path_cache)
    {
        (void)fingerprint;
        if (entry.output_w != out0.width || entry.output_h != out0.height ||
            entry.output_fmt != static_cast<std::uint32_t>(out0.format))
            continue; // output 段不匹配：跳过（避免按旧布局读 SRV）

        ResourceInfo color {}, motion {}, depth {};
        read_resource_info(shader_resources[entry.color_slot], L"fsr2_path_color", color);
        read_resource_info(shader_resources[entry.motion_slot], L"fsr2_path_motion", motion);
        read_resource_info(shader_resources[entry.depth_slot], L"fsr2_path_depth", depth);
        if (color.resource_key == 0 || motion.resource_key == 0 || depth.resource_key == 0)
            continue;

        ResourceInfo out1 {};
        if (entry.output_rtv_slot < 2)
            read_resource_info(render_targets[entry.output_rtv_slot], L"fsr2_path_out1", out1);

        std::uint64_t h = 14695981039346656037ull;
        const auto mix = [&h](std::uint64_t v) { h ^= v; h *= 1099511628211ull; };
        const auto mix_info = [&mix](const ResourceInfo &r)
        {
            mix(static_cast<std::uint64_t>(r.width));
            mix(static_cast<std::uint64_t>(r.height));
            mix(static_cast<std::uint64_t>(r.format));
        };
        mix(static_cast<std::uint64_t>(entry.color_slot));
        mix(static_cast<std::uint64_t>(entry.motion_slot));
        mix(static_cast<std::uint64_t>(entry.depth_slot));
        mix(static_cast<std::uint64_t>(entry.output_rtv_slot));
        mix_info(color);
        mix_info(motion);
        mix_info(depth);
        mix_info(out0);
        mix_info(out1);
        if (h != fingerprint)
            continue;

        TargetUpscalerDrawInfo info;
        info.render_width = color.width;
        info.render_height = color.height;
        info.output_width = out0.width;
        info.output_height = out0.height;
        info.color_srv_slot = entry.color_slot;
        info.depth_srv_slot = entry.depth_slot;
        info.motion_srv_slot = entry.motion_slot;
        info.transparency_srv_slot = entry.transparency_slot;
        info.output_rtv_slot = entry.output_rtv_slot;
        info.color_resource_key = color.resource_key;
        info.motion_resource_key = motion.resource_key;
        entry.last_hit_tick = GetTickCount64();
        return info;
    }
    return std::nullopt;
}

// cb0 注册（jitter map/unmap 快照链自愈）——完整识别与快速路径共用
void register_cb0_for_jitter(ID3D11Buffer *constant_buffer)
{
    if (constant_buffer == nullptr)
        return;
    D3D11_BUFFER_DESC desc {};
    constant_buffer->GetDesc(&desc);
    const std::uint64_t key = reinterpret_cast<std::uint64_t>(constant_buffer);
    g_trace_ps_cb0_key.store(key, std::memory_order_relaxed);
    if (key != 0 && desc.ByteWidth != 0)
    {
        std::lock_guard lock(g_buffer_info_mutex);
        BufferInfo &registered = g_buffer_info[key];
        if (registered.resource_key == 0)
        {
            registered.resource_key = key;
            registered.byte_width = desc.ByteWidth;
            registered.bind_flags = desc.BindFlags;
            registered.usage = desc.Usage;
            log_line("mode2_on_demand_cb0_registered key=" + hex64(key) +
                " bytes=" + std::to_string(desc.ByteWidth));
        }
    }
}

DXGI_FORMAT resource_view_or_format(const ResourceInfo &info)
{
    return info.view_format != DXGI_FORMAT_UNKNOWN ? info.view_format : info.format;
}

bool fsr_feature_depth(const ResourceInfo &info)
{
    switch (resource_view_or_format(info))
    {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return true;
    default:
        return info.format == DXGI_FORMAT_R32G8X24_TYPELESS;
    }
}

bool fsr_feature_color(const ResourceInfo &info)
{
    switch (resource_view_or_format(info))
    {
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return true;
    default:
        return false;
    }
}

bool fsr_feature_motion(const ResourceInfo &info)
{
    switch (resource_view_or_format(info))
    {
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM: // Genshin's square-encoded MV surface
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return true;
    default:
        return false;
    }
}

bool fsr_feature_transparency(const ResourceInfo &info)
{
    switch (resource_view_or_format(info))
    {
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
        return true;
    default:
        return false;
    }
}

template <std::size_t SrvCount, std::size_t RtvCount>
std::optional<TargetUpscalerDrawInfo> identify_target_upscaler_resources(
    const std::array<ResourceInfo, SrvCount> &srvs,
    const std::array<ResourceInfo, RtvCount> &rtvs,
    std::uint32_t viewport_width, std::uint32_t viewport_height,
    std::uint32_t cb0_bytes, std::uint64_t cb0_key)
{
    if (cb0_bytes < 464 || viewport_width == 0 || viewport_height == 0)
        return std::nullopt;

    int output_slot = -1;
    int output_score = -1;
    for (std::size_t i = 0; i < RtvCount; ++i)
    {
        const ResourceInfo &candidate = rtvs[i];
        if (candidate.resource_key == 0 || candidate.width != viewport_width ||
            candidate.height != viewport_height || !fsr_feature_color(candidate))
            continue;
        int score = 1;
        const DXGI_FORMAT format = resource_view_or_format(candidate);
        if (format == DXGI_FORMAT_R10G10B10A2_UNORM || format == DXGI_FORMAT_R11G11B10_FLOAT ||
            format == DXGI_FORMAT_R16G16B16A16_FLOAT)
            score += 4;
        if (score > output_score)
        {
            output_slot = static_cast<int>(i);
            output_score = score;
        }
    }
    if (output_slot < 0)
        return std::nullopt;
    const ResourceInfo &output = rtvs[output_slot];

    int weight_count = 0;
    int history_count = 0;
    for (const ResourceInfo &srv : srvs)
    {
        weight_count += srv.resource_key != 0 && srv.width == 16 && srv.height == 16 ? 1 : 0;
        history_count += srv.resource_key != 0 && srv.width == output.width && srv.height == output.height ? 1 : 0;
    }
    if (weight_count == 0 || history_count < 2)
        return std::nullopt;

    int depth_slot = -1;
    for (std::size_t i = 0; i < SrvCount; ++i)
    {
        if (srvs[i].resource_key != 0 && fsr_feature_depth(srvs[i]))
        {
            depth_slot = static_cast<int>(i);
            break;
        }
    }
    if (depth_slot < 0)
        return std::nullopt;
    const ResourceInfo &depth = srvs[depth_slot];

    int color_slot = -1, motion_slot = -1, transparency_slot = -1;
    int color_score = -1, motion_score = -1, transparency_score = -1;
    for (std::size_t i = 0; i < SrvCount; ++i)
    {
        const ResourceInfo &candidate = srvs[i];
        if (candidate.resource_key == 0 || candidate.width != depth.width ||
            candidate.height != depth.height || static_cast<int>(i) == depth_slot ||
            (candidate.width == 16 && candidate.height == 16))
            continue;
        if (fsr_feature_color(candidate))
        {
            int score = 1;
            const DXGI_FORMAT format = resource_view_or_format(candidate);
            if (format == DXGI_FORMAT_R11G11B10_FLOAT || format == DXGI_FORMAT_R16G16B16A16_FLOAT)
                score += 4;
            if (i == 0) score += 2; // known layout is only a tie-breaker, never a requirement
            if (score > color_score) { color_score = score; color_slot = static_cast<int>(i); }
        }
        if (fsr_feature_motion(candidate))
        {
            int score = 1;
            const DXGI_FORMAT format = resource_view_or_format(candidate);
            if (format == DXGI_FORMAT_R16G16_FLOAT || format == DXGI_FORMAT_R16G16_SNORM ||
                format == DXGI_FORMAT_R32G32_FLOAT) score += 4;
            if (i == 3) score += 2; // tie-breaker for the observed game layout
            if (score > motion_score) { motion_score = score; motion_slot = static_cast<int>(i); }
        }
        if (fsr_feature_transparency(candidate))
        {
            int score = 1;
            if (i == 4) score += 2; // observed layout only breaks ties
            if (score > transparency_score)
            {
                transparency_score = score;
                transparency_slot = static_cast<int>(i);
            }
        }
    }
    if (color_slot < 0 || motion_slot < 0 || color_slot == motion_slot)
        return std::nullopt;
    const ResourceInfo &color = srvs[color_slot];
    if (color.width > output.width || color.height > output.height)
        return std::nullopt;

    return TargetUpscalerDrawInfo {
        color.width, color.height, output.width, output.height, cb0_key,
        color.resource_key, srvs[motion_slot].resource_key,
        static_cast<std::uint32_t>(color_slot), static_cast<std::uint32_t>(depth_slot),
        static_cast<std::uint32_t>(motion_slot),
        transparency_slot >= 0 ? static_cast<std::uint32_t>(transparency_slot) : UINT_MAX,
        static_cast<std::uint32_t>(output_slot)};
}

std::optional<TargetUpscalerDrawInfo> inspect_target_upscaler_draw(UINT element_count)
{
    if (element_count != 3)
        return std::nullopt;
#if defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    const std::uint64_t fast_target_hash = g_mode2_fast_target_ps_hash.load(std::memory_order_relaxed);
    if (g_config.fsr2_fast_state_tracking && g_config.fsr2_translation_mode == 2 && fast_target_hash != 0 &&
        g_current_ps_hash.load(std::memory_order_relaxed) != fast_target_hash)
    {
        return std::nullopt;
    }
#endif

    std::lock_guard lock(g_state_mutex);
    if (g_state.current_ps_hash == 0)
        return std::nullopt;

    const auto identified = identify_target_upscaler_resources(
        g_state.ps_srvs, g_state.rtvs, g_state.viewport_width, g_state.viewport_height,
        g_state.ps_cbs[0].byte_width, g_state.ps_cbs[0].resource_key);
    if (!identified)
        return std::nullopt;

    if (g_state.current_ps_hash != g_config.target_pixel_shader_hash)
    {
        static std::atomic_bool fallback_shader_logged { false };
        if (!fallback_shader_logged.load(std::memory_order_relaxed) &&
            !fallback_shader_logged.exchange(true, std::memory_order_relaxed))
        {
            log_line("target_upscaler_signature_fallback ps=" + hex64(g_state.current_ps_hash) +
                " configured=" + hex64(g_config.target_pixel_shader_hash));
        }
    }

    g_trace_ps_cb0_key.store(g_state.ps_cbs[0].resource_key, std::memory_order_relaxed);
#if defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    if (g_config.fsr2_fast_state_tracking && g_config.fsr2_translation_mode == 2 && fast_target_hash == 0)
    {
        g_mode2_fast_target_ps_hash.store(g_state.current_ps_hash, std::memory_order_relaxed);
        g_mode2_fast_target_ps_key.store(g_state.current_ps_shader, std::memory_order_relaxed);
        log_line("mode2_fast_target_learned ps=" + hex64(g_state.current_ps_hash));
    }
#endif

    return identified;
}

#if defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
// mode 2 的按需识别路径：不依赖 Set 钩子维护的 g_state 镜像，在候选 draw 现场直接查询
// 状态并做完整签名校验。两段式：先用"双 RTV"预筛掉绝大多数全屏三角形（TAAU 签名要求
// 同时绑定 output_metadata 与 output_color 两个 RTV），再对剩余候选做全量内省。
// 不做任何 shader hash 过滤，因此技能等使用不同 shader 的 TAAU 路径同样能被识别。
std::optional<TargetUpscalerDrawInfo> inspect_target_upscaler_draw_on_demand(
    ID3D11DeviceContext *context,
    UINT element_count)
{
    if (context == nullptr || element_count != 3)
        return std::nullopt;

    // 诊断：候选 draw（3 顶点）识别失败限量记录（5s 一次），定位 UI 切换后桥接掉线环节
    static std::atomic_uint64_t on_demand_fail_tick { 0 };
    const auto log_fail = [](std::string &&detail) {
        const ULONGLONG now = GetTickCount64();
        ULONGLONG last = on_demand_fail_tick.load(std::memory_order_relaxed);
        if (now - last < 5000 ||
            !on_demand_fail_tick.compare_exchange_strong(last, now, std::memory_order_relaxed))
            return;
        log_line("fsr2_on_demand_identify_fail " + detail);
    };

    std::array<ID3D11RenderTargetView *, 2> render_targets {};
    context->OMGetRenderTargets(static_cast<UINT>(render_targets.size()), render_targets.data(), nullptr);
    if (render_targets[0] == nullptr || render_targets[1] == nullptr)
    {
        log_fail(std::string("stage=prescreen rtv0=") + (render_targets[0] != nullptr ? "1" : "0") +
            " rtv1=" + (render_targets[1] != nullptr ? "1" : "0"));
        for (ID3D11RenderTargetView *render_target : render_targets)
        {
            if (render_target != nullptr)
                render_target->Release();
        }
        return std::nullopt;
    }

    std::array<ID3D11ShaderResourceView *, 7> shader_resources {};
    ID3D11Buffer *constant_buffer = nullptr;
    context->PSGetShaderResources(0, static_cast<UINT>(shader_resources.size()), shader_resources.data());
    context->PSGetConstantBuffers(0, 1, &constant_buffer);

    // 快速路径：正缓存命中直接构造接管（省 9 视图完整读取——只读 RTV[0] + 布局 3 SRV 验证指纹）
    if (const auto fast_info = try_upscaler_path_cache_fast(render_targets.data(), shader_resources.data()))
    {
        register_cb0_for_jitter(constant_buffer);
        for (ID3D11ShaderResourceView *view : shader_resources)
        {
            if (view != nullptr)
                view->Release();
        }
        for (ID3D11RenderTargetView *render_target : render_targets)
        {
            if (render_target != nullptr)
                render_target->Release();
        }
        if (constant_buffer != nullptr)
            constant_buffer->Release();
        return fast_info;
    }

    D3D11_VIEWPORT viewport {};
    UINT viewport_count = 1;
    context->RSGetViewports(&viewport_count, &viewport);

    std::array<ResourceInfo, 7> inputs {};
    std::array<ResourceInfo, 2> outputs {};
    for (std::size_t index = 0; index < shader_resources.size(); ++index)
        read_resource_info(shader_resources[index], L"fsr2_on_demand_srv", inputs[index]);
    for (std::size_t index = 0; index < render_targets.size(); ++index)
        read_resource_info(render_targets[index], L"fsr2_on_demand_rtv", outputs[index]);

    D3D11_BUFFER_DESC constant_buffer_description {};
    if (constant_buffer != nullptr)
        constant_buffer->GetDesc(&constant_buffer_description);
    const std::uint64_t constant_buffer_key =
        reinterpret_cast<std::uint64_t>(constant_buffer);

    for (ID3D11ShaderResourceView *view : shader_resources)
    {
        if (view != nullptr)
            view->Release();
    }
    for (ID3D11RenderTargetView *render_target : render_targets)
        render_target->Release();
    if (constant_buffer != nullptr)
        constant_buffer->Release();

    const auto identified = identify_target_upscaler_resources(
        inputs, outputs,
        viewport_count != 0 ? static_cast<std::uint32_t>(viewport.Width) : 0,
        viewport_count != 0 ? static_cast<std::uint32_t>(viewport.Height) : 0,
        constant_buffer_description.ByteWidth, constant_buffer_key);
    if (!identified)
    {
        std::ostringstream detail;
        detail << "stage=identify"
               << " out0=" << outputs[0].width << "x" << outputs[0].height
               << " fmt=" << static_cast<std::uint32_t>(outputs[0].format)
               << " out1=" << outputs[1].width << "x" << outputs[1].height
               << " srv0=" << inputs[0].width << "x" << inputs[0].height
               << " srv1=" << inputs[1].width << "x" << inputs[1].height
               << " vp=" << static_cast<std::uint32_t>(viewport.Width) << "x"
               << static_cast<std::uint32_t>(viewport.Height);
        log_fail(detail.str());
        return std::nullopt;
    }

    g_trace_ps_cb0_key.store(constant_buffer_key, std::memory_order_relaxed);
    register_cb0_for_jitter(constant_buffer);

    // 正缓存写入（仅识别成功路径——值语义指纹，UI 切换/地址复用无关）
    {
        std::lock_guard lock(g_upscaler_path_cache_mutex);
        UpscalerPathCacheEntry entry;
        entry.color_slot = identified->color_srv_slot;
        entry.depth_slot = identified->depth_srv_slot;
        entry.motion_slot = identified->motion_srv_slot;
        entry.transparency_slot = identified->transparency_srv_slot;
        entry.output_rtv_slot = identified->output_rtv_slot;
        entry.output_w = outputs[0].width;
        entry.output_h = outputs[0].height;
        entry.output_fmt = static_cast<std::uint32_t>(outputs[0].format);
        entry.last_hit_tick = GetTickCount64();
        const std::uint64_t fingerprint = upscaler_path_fingerprint(inputs, outputs, *identified);
        if (g_upscaler_path_cache.size() >= 8)
        {
            auto oldest = g_upscaler_path_cache.begin();
            for (auto it = g_upscaler_path_cache.begin(); it != g_upscaler_path_cache.end(); ++it)
                if (it->second.last_hit_tick < oldest->second.last_hit_tick)
                    oldest = it;
            g_upscaler_path_cache.erase(oldest);
        }
        g_upscaler_path_cache[fingerprint] = entry;
    }

    static std::atomic_bool identified_logged { false };
    if (!identified_logged.load(std::memory_order_relaxed) &&
        !identified_logged.exchange(true, std::memory_order_relaxed))
    {
        log_line("mode2_on_demand_target_identified render=" +
            std::to_string(identified->render_width) + "x" + std::to_string(identified->render_height) +
            " output=" + std::to_string(identified->output_width) + "x" + std::to_string(identified->output_height) +
            " cb0=" + hex64(constant_buffer_key));
    }

    return identified;
}
#endif

std::optional<TargetUpscalerDrawInfo> inspect_target_upscaler_draw(
    ID3D11DeviceContext *context,
    UINT element_count)
{
#if defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
    if (g_config.fsr2_translation_mode == 2 && g_config.fsr2_mode2_on_demand_state)
        return inspect_target_upscaler_draw_on_demand(context, element_count);
#endif
#if defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    const std::uint64_t fast_target_hash = g_mode2_fast_target_ps_hash.load(std::memory_order_relaxed);
    if (g_config.fsr2_fast_state_tracking && g_config.fsr2_translation_mode == 2 && fast_target_hash != 0)
    {
        if (context == nullptr || element_count != 3 ||
            g_current_ps_hash.load(std::memory_order_relaxed) != fast_target_hash)
        {
            return std::nullopt;
        }

        std::array<ID3D11ShaderResourceView *, 7> shader_resources {};
        std::array<ID3D11RenderTargetView *, 2> render_targets {};
        ID3D11Buffer *constant_buffer = nullptr;
        context->PSGetShaderResources(0, static_cast<UINT>(shader_resources.size()), shader_resources.data());
        context->OMGetRenderTargets(static_cast<UINT>(render_targets.size()), render_targets.data(), nullptr);
        context->PSGetConstantBuffers(0, 1, &constant_buffer);

        D3D11_VIEWPORT viewport {};
        UINT viewport_count = 1;
        context->RSGetViewports(&viewport_count, &viewport);

        std::array<ResourceInfo, 7> inputs {};
        std::array<ResourceInfo, 2> outputs {};
        for (std::size_t index = 0; index < shader_resources.size(); ++index)
            read_resource_info(shader_resources[index], L"fsr2_fast_srv", inputs[index]);
        for (std::size_t index = 0; index < render_targets.size(); ++index)
            read_resource_info(render_targets[index], L"fsr2_fast_rtv", outputs[index]);

        D3D11_BUFFER_DESC constant_buffer_description {};
        if (constant_buffer != nullptr)
            constant_buffer->GetDesc(&constant_buffer_description);
        const std::uint64_t constant_buffer_key =
            reinterpret_cast<std::uint64_t>(constant_buffer);

        for (ID3D11ShaderResourceView *view : shader_resources)
        {
            if (view != nullptr)
                view->Release();
        }
        for (ID3D11RenderTargetView *render_target : render_targets)
        {
            if (render_target != nullptr)
                render_target->Release();
        }
        if (constant_buffer != nullptr)
            constant_buffer->Release();

        const auto identified = identify_target_upscaler_resources(
            inputs, outputs,
            viewport_count != 0 ? static_cast<std::uint32_t>(viewport.Width) : 0,
            viewport_count != 0 ? static_cast<std::uint32_t>(viewport.Height) : 0,
            constant_buffer_description.ByteWidth, constant_buffer_key);
        if (!identified)
            return std::nullopt;

        g_trace_ps_cb0_key.store(constant_buffer_key, std::memory_order_relaxed);
        return identified;
    }
#endif
    return inspect_target_upscaler_draw(element_count);
}

void maybe_dump_target_color_chain(ID3D11DeviceContext *context, UINT element_count)
{
    if (!g_config.fsr2_trace_color_producers)
        return;
    const auto draw_info = inspect_target_upscaler_draw(element_count);
    if (draw_info)
    {
        maybe_dump_color_source_history(context, draw_info->color_resource_key);
        maybe_dump_motion_source_history(context, draw_info->motion_resource_key);
    }
}

std::wstring lower_wstring(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character)
        {
            return static_cast<wchar_t>(std::towlower(character));
        });
    return value;
}

std::vector<std::filesystem::path> optiscaler_ini_candidates()
{
    std::vector<std::filesystem::path> paths;
    auto add_path = [&](const std::filesystem::path &path)
    {
        if (path.empty())
            return;
        if (std::find(paths.begin(), paths.end(), path) == paths.end())
            paths.push_back(path);
    };

    wchar_t process_path[MAX_PATH] {};
    const DWORD length = GetModuleFileNameW(nullptr, process_path, static_cast<DWORD>(std::size(process_path)));
    if (length != 0)
        add_path(std::filesystem::path(std::wstring(process_path, process_path + length)).parent_path() / L"OptiScaler.ini");

    add_path(g_module_dir / L"OptiScaler.ini");
    if (!g_module_dir.empty())
    {
        add_path(g_module_dir.parent_path() / L"OptiScaler.ini");
        add_path(g_module_dir.parent_path() / L"OptiScaler" / L"OptiScaler.ini");
    }

    return paths;
}

bool optiscaler_framegen_value_is_dlssg(const std::wstring &value)
{
    const std::wstring lower = lower_wstring(value);
    return lower == L"dlssg" || lower == L"dlssgwithnvngx";
}

bool dlssg_framegen_selected()
{
    static std::mutex mutex;
    static ULONGLONG last_check_tick = 0;
    static bool cached_result = false;
    static bool logged_active = false;
    const ULONGLONG now = GetTickCount64();

    std::lock_guard lock(mutex);
    if (now - last_check_tick < 250)
        return cached_result;
    last_check_tick = now;

    cached_result = false;
    for (const std::filesystem::path &ini_path : optiscaler_ini_candidates())
    {
        std::error_code error;
        if (!std::filesystem::exists(ini_path, error))
            continue;

        wchar_t output_buffer[64] {};
        GetPrivateProfileStringW(
            L"FrameGen",
            L"FGOutput",
            L"",
            output_buffer,
            static_cast<DWORD>(std::size(output_buffer)),
            ini_path.c_str());

        const std::wstring output = output_buffer;
        if (!optiscaler_framegen_value_is_dlssg(output))
            continue;

        cached_result = true;
        if (!logged_active)
        {
            logged_active = true;
            log_line("dlssg_dxgi_workaround auto_enabled optiscaler_ini=" +
                narrow(ini_path.wstring()) + " fg_output=" + narrow(output));
        }
        break;
    }

    return cached_result;
}

bool dlssg_dxgi_workaround_active()
{
    if (g_config.dlssg_dxgi_workaround > 0)
        return true;
    if (g_config.dlssg_dxgi_workaround == 0)
        return false;
    return dlssg_framegen_selected();
}

#if defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
void maybe_dump_same_frame_fsr2_inputs(ID3D11DeviceContext *context, UINT element_count)
{
    if (context == nullptr || !g_fsr2_same_frame_capture_pending.load(std::memory_order_acquire))
        return;

    const auto draw_info = inspect_target_upscaler_draw(element_count);
    if (!draw_info || !g_fsr2_same_frame_capture_pending.exchange(false, std::memory_order_acq_rel))
        return;

    std::array<ID3D11ShaderResourceView *, 5> views {};
    context->PSGetShaderResources(0, static_cast<UINT>(views.size()), views.data());
    for (UINT slot = 2; slot <= 4; ++slot)
        dump_fsr2_input_texture(context, views[slot], slot);

    ID3D11Buffer *constant_buffer = nullptr;
    context->PSGetConstantBuffers(0, 1, &constant_buffer);
    if (constant_buffer != nullptr)
    {
        if (!dump_fsr2_bound_constant_buffer(context, constant_buffer))
            dump_fsr2_constant_buffer(draw_info->constant_buffer_key);
        constant_buffer->Release();
    }

    for (ID3D11ShaderResourceView *view : views)
    {
        if (view != nullptr)
            view->Release();
    }

    log_line("fsr2_same_frame_inputs_dumped raw_color=" +
        hex64(g_fsr2_candidate_color_resource.load(std::memory_order_relaxed)) +
        " late_color=" + hex64(draw_info->color_resource_key) +
        " sequence_begin=" + std::to_string(g_fsr2_candidate_sequence.load(std::memory_order_relaxed)) +
        " sequence_end=" + std::to_string(g_color_source_sequence.load(std::memory_order_relaxed)));
}

// 返回 nullopt 表示"jitter 不可用"（快照缺失或数据非法），调用方必须区分
// 该情形与真实零抖动：用 (0,0) 或坏数据 dispatch 会让 FSR2 历史累积错位。
std::optional<std::pair<float, float>> target_jitter_pixels(const TargetUpscalerDrawInfo &draw_info)
{
    constexpr std::size_t jitter_offset = 28 * sizeof(float) * 4;
    float normalized_jitter[2] {};
    if (!read_buffer_snapshot_bytes(
            draw_info.constant_buffer_key, jitter_offset, normalized_jitter, sizeof(normalized_jitter)))
        return std::nullopt;
    if (!std::isfinite(normalized_jitter[0]) || !std::isfinite(normalized_jitter[1]))
        return std::nullopt;

    float jitter_x = normalized_jitter[0] * static_cast<float>(draw_info.render_width) - 0.5f;
    float jitter_y = normalized_jitter[1] * static_cast<float>(draw_info.render_height) - 0.5f;
    // TAA jitter 换算成像素后必然落在亚像素范围（约 ±0.5px）内；超出说明 cb0
    // 偏移 448 处并非 jitter（识别到布局不同的 pass 或快照内容错位），此时放弃
    // 本帧翻译比把数百像素的"jitter"喂给 FSR2 安全得多。
    constexpr float k_max_jitter_pixels = 0.6f;
    if (std::abs(jitter_x) > k_max_jitter_pixels || std::abs(jitter_y) > k_max_jitter_pixels)
        return std::nullopt;
    switch (g_config.fsr2_jitter_mode)
    {
    case 1:
        jitter_x = -jitter_x;
        break;
    case 2:
        jitter_y = -jitter_y;
        break;
    case 3:
        jitter_x = -jitter_x;
        jitter_y = -jitter_y;
        break;
    case 4:
        jitter_x = 0.0f;
        jitter_y = 0.0f;
        break;
    default:
        break;
    }
    return std::pair<float, float> { jitter_x, jitter_y };
}

bool unsafe_dx11_on12_backend_selected()
{
    if (!g_config.block_dx11_on12_upscalers)
        return false;

    static std::mutex mutex;
    static ULONGLONG last_check_tick = 0;
    static bool cached_result = false;
    const ULONGLONG now = GetTickCount64();
    std::lock_guard lock(mutex);
    if (now - last_check_tick < 250)
        return cached_result;
    last_check_tick = now;

    wchar_t process_path[MAX_PATH] {};
    const DWORD length = GetModuleFileNameW(nullptr, process_path, static_cast<DWORD>(std::size(process_path)));
    if (length == 0)
    {
        cached_result = false;
        return false;
    }

    const std::filesystem::path ini_path =
        std::filesystem::path(std::wstring(process_path, process_path + length)).parent_path() / L"OptiScaler.ini";
    wchar_t backend_buffer[64] {};
    GetPrivateProfileStringW(
        L"Upscalers",
        L"Dx11Upscaler",
        L"",
        backend_buffer,
        static_cast<DWORD>(std::size(backend_buffer)),
        ini_path.c_str());
    std::wstring backend = backend_buffer;
    std::transform(backend.begin(), backend.end(), backend.begin(), [](wchar_t value)
        {
            return static_cast<wchar_t>(std::towlower(value));
        });
    cached_result = backend.ends_with(L"_12") || backend.find(L"on12") != std::wstring::npos;
    return cached_result;
}

ID3D11ShaderResourceView *acquire_fsr2_candidate_color_view(
    std::uint64_t producer_output_resource_key = 0,
    std::uint64_t producer_generation = 0)
{
    std::lock_guard lock(g_fsr2_candidate_color_view_mutex);
    if (producer_output_resource_key != 0 &&
        g_fsr2_candidate_producer_output_resource.load(std::memory_order_acquire) !=
            producer_output_resource_key)
    {
        return nullptr;
    }
    if (producer_generation != 0 &&
        g_fsr2_candidate_producer_generation.load(std::memory_order_acquire) != producer_generation)
    {
        return nullptr;
    }
    if (g_fsr2_candidate_color_view != nullptr)
        g_fsr2_candidate_color_view->AddRef();
    return g_fsr2_candidate_color_view;
}

bool is_linear_scene_color_format(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_R11G11B10_FLOAT ||
        format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
        format == DXGI_FORMAT_R32G32B32A32_FLOAT;
}

bool is_single_channel_float_format(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_R16_FLOAT || format == DXGI_FORMAT_R32_FLOAT;
}

void release_fsr2_color_replay_state(Fsr2ColorReplayState &state);
bool acquire_fsr2_color_replay_state(Fsr2ColorReplayState &state);

void invalidate_fsr2_dynamic_color_path()
{
    g_fsr2_locked_color_producer_ps_hash.store(0, std::memory_order_relaxed);
    {
        std::lock_guard lock(g_fsr2_candidate_color_view_mutex);
        if (g_fsr2_candidate_color_view != nullptr)
            g_fsr2_candidate_color_view->Release();
        g_fsr2_candidate_color_view = nullptr;
    }
    g_fsr2_candidate_color_resource.store(0, std::memory_order_relaxed);
    g_fsr2_candidate_producer_output_resource.store(0, std::memory_order_release);
    g_fsr2_candidate_producer_generation.store(0, std::memory_order_release);
    {
        std::lock_guard lock(g_fsr2_color_replay_mutex);
        release_fsr2_color_replay_state(g_fsr2_color_replay_state);
    }
    {
        std::lock_guard lock(g_fsr2_dynamic_color_path_mutex);
        g_fsr2_latest_producer_write_generations.clear();
        g_fsr2_consumed_producer_generations.clear();
        g_fsr2_late_path_states.clear();
    }
}

void observe_fsr2_dynamic_color_target(const TargetUpscalerDrawInfo &draw_info)
{
    bool dimensions_changed = false;
    {
        std::lock_guard lock(g_fsr2_dynamic_color_path_mutex);
        const auto existing = std::find_if(
            g_fsr2_dynamic_color_targets.begin(),
            g_fsr2_dynamic_color_targets.end(),
            [&](const Fsr2DynamicColorTarget &target)
            {
                return target.resource_key == draw_info.color_resource_key;
            });
        if (existing != g_fsr2_dynamic_color_targets.end())
        {
            dimensions_changed = existing->render_width != draw_info.render_width ||
                existing->render_height != draw_info.render_height ||
                existing->output_width != draw_info.output_width ||
                existing->output_height != draw_info.output_height;
            g_fsr2_dynamic_color_targets.erase(existing);
        }
        g_fsr2_dynamic_color_targets.push_back(Fsr2DynamicColorTarget {
            draw_info.color_resource_key,
            draw_info.render_width,
            draw_info.render_height,
            draw_info.output_width,
            draw_info.output_height,
        });
        while (g_fsr2_dynamic_color_targets.size() > 8)
            g_fsr2_dynamic_color_targets.pop_front();
    }

    if (dimensions_changed)
    {
        invalidate_fsr2_dynamic_color_path();
        log_line("fsr2_dynamic_color_path_invalidated reason=dimensions_changed render=" +
            std::to_string(draw_info.render_width) + "x" + std::to_string(draw_info.render_height) +
            " output=" + std::to_string(draw_info.output_width) + "x" +
            std::to_string(draw_info.output_height));
    }
}

std::optional<Fsr2DynamicColorTarget> match_fsr2_dynamic_color_producer()
{
    DispatchState snapshot {};
    {
        std::lock_guard lock(g_state_mutex);
        snapshot = g_state;
    }

    std::lock_guard lock(g_fsr2_dynamic_color_path_mutex);
    for (const ResourceInfo &render_target : snapshot.rtvs)
    {
        if (render_target.resource_key == 0)
            continue;
        const auto target = std::find_if(
            g_fsr2_dynamic_color_targets.rbegin(),
            g_fsr2_dynamic_color_targets.rend(),
            [&](const Fsr2DynamicColorTarget &candidate)
            {
                return candidate.resource_key == render_target.resource_key &&
                    candidate.render_width == render_target.width &&
                    candidate.render_height == render_target.height;
            });
        if (target != g_fsr2_dynamic_color_targets.rend())
            return *target;
    }
    return std::nullopt;
}

std::uint64_t note_fsr2_dynamic_producer_write(std::uint64_t producer_output_resource_key)
{
    const std::uint64_t generation =
        g_fsr2_dynamic_producer_generation.fetch_add(1, std::memory_order_relaxed) + 1;
    std::lock_guard lock(g_fsr2_dynamic_color_path_mutex);
    g_fsr2_latest_producer_write_generations[producer_output_resource_key] = generation;
    return generation;
}

struct Fsr2FreshProducerPath
{
    bool has_fresh_write = false;
    bool has_fresh_linear_color = false;
    std::uint64_t linear_generation = 0;
};

Fsr2FreshProducerPath consume_fsr2_fresh_producer_path(
    std::uint64_t producer_output_resource_key)
{
    if (producer_output_resource_key == 0)
        return {};

    std::lock_guard lock(g_fsr2_dynamic_color_path_mutex);
    const auto latest = g_fsr2_latest_producer_write_generations.find(producer_output_resource_key);
    if (latest == g_fsr2_latest_producer_write_generations.end())
        return {};
    std::uint64_t &consumed = g_fsr2_consumed_producer_generations[producer_output_resource_key];
    if (latest->second <= consumed)
        return {};

    Fsr2FreshProducerPath path;
    path.has_fresh_write = true;
    const std::uint64_t linear_generation =
        g_fsr2_candidate_producer_generation.load(std::memory_order_acquire);
    path.has_fresh_linear_color =
        g_fsr2_candidate_producer_output_resource.load(std::memory_order_acquire) ==
            producer_output_resource_key &&
        linear_generation > consumed && linear_generation <= latest->second;
    path.linear_generation = path.has_fresh_linear_color ? linear_generation : 0;
    consumed = latest->second;
    return path;
}

bool update_fsr2_late_path_state(std::uint64_t producer_output_resource_key, bool late_path)
{
    std::lock_guard lock(g_fsr2_dynamic_color_path_mutex);
    const auto existing = g_fsr2_late_path_states.find(producer_output_resource_key);
    const bool changed = existing == g_fsr2_late_path_states.end() || existing->second != late_path;
    g_fsr2_late_path_states[producer_output_resource_key] = late_path;
    return changed;
}

UINT infer_fsr2_exposure_slot(const std::array<ID3D11ShaderResourceView *, 7> &shader_resources)
{
    UINT best_slot = UINT_MAX;
    std::uint64_t best_score = UINT64_MAX;
    for (UINT slot = 1; slot < shader_resources.size(); ++slot)
    {
        ResourceInfo info {};
        if (!read_resource_info(shader_resources[slot], L"fsr2_dynamic_exposure", info) ||
            info.width == 0 || info.height == 0 || info.width > 4 || info.height > 4 ||
            is_depth_like_format(info.format))
        {
            continue;
        }
        const std::uint64_t area = static_cast<std::uint64_t>(info.width) * info.height;
        const std::uint64_t score = area * 2 + (is_single_channel_float_format(info.format) ? 0 : 1);
        if (score < best_score)
        {
            best_score = score;
            best_slot = slot;
        }
    }
    return best_slot;
}

void poll_fsr2_transient_capture_hotkey()
{
    const bool key_down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    const bool was_down = g_fsr2_transient_capture_key_down.exchange(key_down, std::memory_order_relaxed);
    if (!key_down || was_down)
        return;

    const std::uint32_t session =
        g_fsr2_transient_capture_session.fetch_add(1, std::memory_order_relaxed) + 1;
    g_fsr2_transient_capture_sample.store(0, std::memory_order_relaxed);
    g_fsr2_transient_capture_frames_remaining.store(90, std::memory_order_release);
    log_line("fsr2_transient_capture_started session=" + std::to_string(session) +
        " samples=90 snapshot_interval=10 hotkey=F11");
}

std::filesystem::path fsr2_transient_capture_path(std::uint32_t session)
{
    const std::filesystem::path directory = g_module_dir / L"Dx11FsrBridge.inputs";
    std::error_code directory_error;
    std::filesystem::create_directories(directory, directory_error);
    return directory / (L"transient_" + std::to_wstring(GetCurrentProcessId()) + L"_s" +
        std::to_wstring(session) + L".jsonl");
}

void write_fsr2_transient_resource(std::ofstream &out, const ResourceInfo &info)
{
    out << "{\"key\":\"" << hex64(info.resource_key) << "\""
        << ",\"width\":" << info.width
        << ",\"height\":" << info.height
        << ",\"format\":" << static_cast<std::uint32_t>(info.format) << "}";
}

std::string escape_fsr2_transient_json(std::string_view value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value)
    {
        if (character == '\\' || character == '"')
            escaped.push_back('\\');
        if (character == '\n')
        {
            escaped += "\\n";
            continue;
        }
        if (character == '\r')
            continue;
        escaped.push_back(character);
    }
    return escaped;
}

void record_fsr2_transient_producer(
    const Fsr2DynamicColorTarget &target,
    const ResourceInfo &input,
    bool accepted,
    const char *reason,
    UINT exposure_slot)
{
    if (g_fsr2_transient_capture_frames_remaining.load(std::memory_order_acquire) == 0)
        return;
    const std::uint32_t session = g_fsr2_transient_capture_session.load(std::memory_order_relaxed);
    std::lock_guard lock(g_fsr2_transient_capture_mutex);
    std::ofstream out(fsr2_transient_capture_path(session), std::ios::app);
    if (!out)
        return;
    out << "{\"event\":\"producer\",\"session\":" << session
        << ",\"tick_ms\":" << GetTickCount64()
        << ",\"ps\":\"" << hex64(g_current_ps_hash.load(std::memory_order_relaxed)) << "\""
        << ",\"output\":\"" << hex64(target.resource_key) << "\""
        << ",\"input\":";
    write_fsr2_transient_resource(out, input);
    out << ",\"accepted\":" << (accepted ? 1 : 0)
        << ",\"reason\":\"" << (reason != nullptr ? reason : "") << "\""
        << ",\"exposure_slot\":";
    if (exposure_slot == UINT_MAX)
        out << "null";
    else
        out << exposure_slot;
    out << "}\n";
}

bool begin_fsr2_transient_capture(
    ID3D11DeviceContext *context,
    const TargetUpscalerDrawInfo &draw_info)
{
    g_fsr2_transient_capture_current_session = 0;
    g_fsr2_transient_capture_current_sample = 0;
    g_fsr2_transient_capture_snapshot = false;
    g_fsr2_transient_capture_result_recorded = false;

    std::uint32_t remaining =
        g_fsr2_transient_capture_frames_remaining.load(std::memory_order_acquire);
    while (remaining != 0 &&
        !g_fsr2_transient_capture_frames_remaining.compare_exchange_weak(
            remaining, remaining - 1, std::memory_order_acq_rel, std::memory_order_relaxed))
    {
    }
    if (remaining == 0)
        return false;

    const std::uint32_t session = g_fsr2_transient_capture_session.load(std::memory_order_relaxed);
    const std::uint32_t sample =
        g_fsr2_transient_capture_sample.fetch_add(1, std::memory_order_relaxed) + 1;
    g_fsr2_transient_capture_current_session = session;
    g_fsr2_transient_capture_current_sample = sample;
    g_fsr2_transient_capture_snapshot = sample == 1 || sample % 10 == 0 || remaining == 1;

    DispatchState snapshot {};
    {
        std::lock_guard lock(g_state_mutex);
        snapshot = g_state;
    }
    Fsr2ColorReplayState replay_state;
    const bool replay_available = acquire_fsr2_color_replay_state(replay_state);
    std::deque<Fsr2DynamicColorTarget> targets;
    {
        std::lock_guard lock(g_fsr2_dynamic_color_path_mutex);
        targets = g_fsr2_dynamic_color_targets;
    }

    {
        std::lock_guard lock(g_fsr2_transient_capture_mutex);
        std::ofstream out(fsr2_transient_capture_path(session), std::ios::app);
        if (out)
        {
            out << "{\"event\":\"target\",\"session\":" << session
                << ",\"sample\":" << sample
                << ",\"tick_ms\":" << GetTickCount64()
                << ",\"game_frame\":" << snapshot.frame_index
                << ",\"ps\":\"" << hex64(snapshot.current_ps_hash) << "\""
                << ",\"render_size\":[" << draw_info.render_width << "," << draw_info.render_height << "]"
                << ",\"output_size\":[" << draw_info.output_width << "," << draw_info.output_height << "]"
                << ",\"target_color\":\"" << hex64(draw_info.color_resource_key) << "\""
                << ",\"candidate_color\":\""
                << hex64(g_fsr2_candidate_color_resource.load(std::memory_order_relaxed)) << "\""
                << ",\"candidate_output\":\""
                << hex64(g_fsr2_candidate_producer_output_resource.load(std::memory_order_acquire)) << "\""
                << ",\"replay_available\":" << (replay_available ? 1 : 0);
            if (replay_available)
            {
                out << ",\"replay_output\":\"" << hex64(replay_state.producer_output_resource_key) << "\""
                    << ",\"replay_render_size\":[" << replay_state.render_width << ","
                    << replay_state.render_height << "]"
                    << ",\"exposure_slot\":";
                if (replay_state.exposure_slot == UINT_MAX)
                    out << "null";
                else
                    out << replay_state.exposure_slot;
            }
            out << ",\"srvs\":[";
            for (std::size_t slot = 0; slot < 7; ++slot)
            {
                if (slot != 0)
                    out << ",";
                write_fsr2_transient_resource(out, snapshot.ps_srvs[slot]);
            }
            out << "],\"rtvs\":[";
            for (std::size_t slot = 0; slot < 2; ++slot)
            {
                if (slot != 0)
                    out << ",";
                write_fsr2_transient_resource(out, snapshot.rtvs[slot]);
            }
            out << "],\"learned_targets\":[";
            for (std::size_t index = 0; index < targets.size(); ++index)
            {
                if (index != 0)
                    out << ",";
                out << "{\"key\":\"" << hex64(targets[index].resource_key) << "\""
                    << ",\"render_size\":[" << targets[index].render_width << ","
                    << targets[index].render_height << "]"
                    << ",\"output_size\":[" << targets[index].output_width << ","
                    << targets[index].output_height << "]}";
            }
            out << "]}\n";
        }
    }
    if (replay_available)
        release_fsr2_color_replay_state(replay_state);

    if (g_fsr2_transient_capture_snapshot && context != nullptr)
    {
        std::wostringstream stem;
        stem << L"transient_" << GetCurrentProcessId() << L"_s" << session << L"_"
            << std::setw(3) << std::setfill(L'0') << sample;
        ID3D11ShaderResourceView *early_color = acquire_fsr2_candidate_color_view(draw_info.color_resource_key);
        dump_fsr2_input_texture(context, early_color, 0, stem.str() + L"_early");
        if (early_color != nullptr)
            early_color->Release();
        ID3D11ShaderResourceView *late_color = nullptr;
        context->PSGetShaderResources(0, 1, &late_color);
        dump_fsr2_input_texture(context, late_color, 0, stem.str() + L"_late");
        if (late_color != nullptr)
            late_color->Release();
    }

    if (remaining == 1)
        log_line("fsr2_transient_capture_target_sequence_complete session=" + std::to_string(session));
    return true;
}

void record_fsr2_transient_capture_result(
    bool dispatch_succeeded,
    bool hook_entry_detected,
    bool color_replayed,
    bool skip_original_draw,
    std::uint32_t error_code,
    const std::string &error)
{
    if (g_fsr2_transient_capture_current_session == 0)
        return;
    std::lock_guard lock(g_fsr2_transient_capture_mutex);
    std::ofstream out(
        fsr2_transient_capture_path(g_fsr2_transient_capture_current_session), std::ios::app);
    if (out)
    {
        out << "{\"event\":\"result\",\"session\":" << g_fsr2_transient_capture_current_session
            << ",\"sample\":" << g_fsr2_transient_capture_current_sample
            << ",\"tick_ms\":" << GetTickCount64()
            << ",\"dispatch_succeeded\":" << (dispatch_succeeded ? 1 : 0)
            << ",\"hook_entry_detected\":" << (hook_entry_detected ? 1 : 0)
            << ",\"color_replayed\":" << (color_replayed ? 1 : 0)
            << ",\"skip_original_draw\":" << (skip_original_draw ? 1 : 0)
            << ",\"error_code\":" << error_code
            << ",\"error\":\"" << escape_fsr2_transient_json(error) << "\"}\n";
    }
    g_fsr2_transient_capture_result_recorded = true;
}

void finish_fsr2_transient_capture_fallback()
{
    if (g_fsr2_transient_capture_current_session != 0 &&
        !g_fsr2_transient_capture_result_recorded)
    {
        record_fsr2_transient_capture_result(false, false, false, false, 0, "fallback_before_dispatch");
    }
    g_fsr2_transient_capture_current_session = 0;
    g_fsr2_transient_capture_current_sample = 0;
    g_fsr2_transient_capture_snapshot = false;
    g_fsr2_transient_capture_result_recorded = false;
}

void release_fsr2_color_replay_state(Fsr2ColorReplayState &state)
{
    if (state.pixel_shader != nullptr)
        state.pixel_shader->Release();
    for (ID3D11ShaderResourceView *view : state.shader_resources)
    {
        if (view != nullptr)
            view->Release();
    }
    if (state.constant_buffer != nullptr)
        state.constant_buffer->Release();
    for (ID3D11SamplerState *sampler : state.samplers)
    {
        if (sampler != nullptr)
            sampler->Release();
    }
    state = {};
}

bool acquire_fsr2_color_replay_state(Fsr2ColorReplayState &state)
{
    std::lock_guard lock(g_fsr2_color_replay_mutex);
    if (g_fsr2_color_replay_state.pixel_shader == nullptr ||
        g_fsr2_color_replay_state.constant_buffer == nullptr)
    {
        return false;
    }

    state = g_fsr2_color_replay_state;
    state.pixel_shader->AddRef();
    for (ID3D11ShaderResourceView *view : state.shader_resources)
    {
        if (view != nullptr)
            view->AddRef();
    }
    state.constant_buffer->AddRef();
    for (ID3D11SamplerState *sampler : state.samplers)
    {
        if (sampler != nullptr)
            sampler->AddRef();
    }
    return true;
}

ID3D11ShaderResourceView *acquire_fsr2_color_replay_exposure_view(
    std::uint64_t producer_output_resource_key,
    std::uint64_t producer_generation,
    std::uint32_t render_width,
    std::uint32_t render_height)
{
    std::lock_guard lock(g_fsr2_color_replay_mutex);
    if (g_fsr2_color_replay_state.producer_output_resource_key != producer_output_resource_key ||
        g_fsr2_color_replay_state.producer_generation != producer_generation ||
        g_fsr2_color_replay_state.render_width != render_width ||
        g_fsr2_color_replay_state.render_height != render_height ||
        g_fsr2_color_replay_state.exposure_slot >= g_fsr2_color_replay_state.shader_resources.size())
    {
        return nullptr;
    }
    const UINT exposure_slot = g_fsr2_color_replay_state.exposure_slot;
    ID3D11ShaderResourceView *exposure = g_fsr2_color_replay_state.shader_resources[exposure_slot];
    if (exposure != nullptr)
        exposure->AddRef();
    return exposure;
}

bool acquire_fsr2_color_replay_output(
    ID3D11DeviceContext *context,
    std::uint32_t width,
    std::uint32_t height,
    DXGI_FORMAT format,
    ID3D11Resource **output,
    ID3D11ShaderResourceView **output_view)
{
    if (context == nullptr || output == nullptr || output_view == nullptr)
        return false;

    *output = nullptr;
    *output_view = nullptr;
    ID3D11Device *device = nullptr;
    context->GetDevice(&device);
    if (device == nullptr)
        return false;

    std::lock_guard lock(g_fsr2_color_replay_mutex);
    const bool recreate =
        g_fsr2_color_replay_device != device || g_fsr2_color_replay_output == nullptr ||
        g_fsr2_color_replay_output_width != width || g_fsr2_color_replay_output_height != height ||
        g_fsr2_color_replay_output_format != format;
    if (recreate)
    {
        if (g_fsr2_color_replay_output_view != nullptr)
            g_fsr2_color_replay_output_view->Release();
        if (g_fsr2_color_replay_output != nullptr)
            g_fsr2_color_replay_output->Release();
        if (g_fsr2_color_replay_device != nullptr)
            g_fsr2_color_replay_device->Release();
        g_fsr2_color_replay_device = device;
        g_fsr2_color_replay_output = nullptr;
        g_fsr2_color_replay_output_view = nullptr;
        g_fsr2_color_replay_output_width = 0;
        g_fsr2_color_replay_output_height = 0;
        g_fsr2_color_replay_output_format = DXGI_FORMAT_UNKNOWN;

        D3D11_TEXTURE2D_DESC description {};
        description.Width = width;
        description.Height = height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = format;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        HRESULT result = device->CreateTexture2D(&description, nullptr, &g_fsr2_color_replay_output);
        if (SUCCEEDED(result))
        {
            result = device->CreateShaderResourceView(
                g_fsr2_color_replay_output, nullptr, &g_fsr2_color_replay_output_view);
        }
        if (FAILED(result) || g_fsr2_color_replay_output == nullptr ||
            g_fsr2_color_replay_output_view == nullptr)
        {
            if (g_fsr2_color_replay_output_view != nullptr)
            {
                g_fsr2_color_replay_output_view->Release();
                g_fsr2_color_replay_output_view = nullptr;
            }
            if (g_fsr2_color_replay_output != nullptr)
            {
                g_fsr2_color_replay_output->Release();
                g_fsr2_color_replay_output = nullptr;
            }
            log_line("fsr2_color_replay_output_create_failed hr=" +
                std::to_string(static_cast<long>(result)));
            return false;
        }
        g_fsr2_color_replay_output_width = width;
        g_fsr2_color_replay_output_height = height;
        g_fsr2_color_replay_output_format = format;
        log_line("fsr2_color_replay_output_created size=" + std::to_string(width) + "x" +
            std::to_string(height) + " format=" + std::to_string(static_cast<std::uint32_t>(format)));
    }
    else
    {
        device->Release();
    }

    g_fsr2_color_replay_output->AddRef();
    g_fsr2_color_replay_output_view->AddRef();
    *output = g_fsr2_color_replay_output;
    *output_view = g_fsr2_color_replay_output_view;
    return true;
}

ID3D11ShaderResourceView *acquire_fsr2_neutral_exposure_view(ID3D11DeviceContext *context)
{
    if (context == nullptr)
        return nullptr;
    ID3D11Device *device = nullptr;
    context->GetDevice(&device);
    if (device == nullptr)
        return nullptr;

    std::lock_guard lock(g_fsr2_neutral_exposure_mutex);
    if (g_fsr2_neutral_exposure_device != device || g_fsr2_neutral_exposure_view == nullptr)
    {
        if (g_fsr2_neutral_exposure_view != nullptr)
            g_fsr2_neutral_exposure_view->Release();
        if (g_fsr2_neutral_exposure_texture != nullptr)
            g_fsr2_neutral_exposure_texture->Release();
        if (g_fsr2_neutral_exposure_device != nullptr)
            g_fsr2_neutral_exposure_device->Release();
        g_fsr2_neutral_exposure_device = device;
        g_fsr2_neutral_exposure_texture = nullptr;
        g_fsr2_neutral_exposure_view = nullptr;

        D3D11_TEXTURE2D_DESC description {};
        description.Width = 1;
        description.Height = 1;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R32_FLOAT;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const float neutral_exposure = 1.0f;
        D3D11_SUBRESOURCE_DATA initial_data {};
        initial_data.pSysMem = &neutral_exposure;
        initial_data.SysMemPitch = sizeof(neutral_exposure);
        HRESULT result = device->CreateTexture2D(
            &description, &initial_data, &g_fsr2_neutral_exposure_texture);
        if (SUCCEEDED(result))
        {
            result = device->CreateShaderResourceView(
                g_fsr2_neutral_exposure_texture, nullptr, &g_fsr2_neutral_exposure_view);
        }
        if (FAILED(result) || g_fsr2_neutral_exposure_view == nullptr)
        {
            if (g_fsr2_neutral_exposure_view != nullptr)
            {
                g_fsr2_neutral_exposure_view->Release();
                g_fsr2_neutral_exposure_view = nullptr;
            }
            if (g_fsr2_neutral_exposure_texture != nullptr)
            {
                g_fsr2_neutral_exposure_texture->Release();
                g_fsr2_neutral_exposure_texture = nullptr;
            }
            log_line("fsr2_neutral_exposure_create_failed hr=" +
                std::to_string(static_cast<long>(result)));
            return nullptr;
        }
        log_line("fsr2_neutral_exposure_created value=1");
    }
    else
    {
        device->Release();
    }

    g_fsr2_neutral_exposure_view->AddRef();
    return g_fsr2_neutral_exposure_view;
}

void maybe_track_fsr2_color_candidate(ID3D11DeviceContext *context, UINT element_count)
{
    if (g_config.fsr2_translation_mode < 3 || context == nullptr || element_count != 3)
        return;

    const std::optional<Fsr2DynamicColorTarget> target = match_fsr2_dynamic_color_producer();
    if (!target)
        return;
    const std::uint64_t producer_write_generation =
        note_fsr2_dynamic_producer_write(target->resource_key);

    std::array<ID3D11ShaderResourceView *, 7> shader_resources {};
    const UINT resource_count = g_config.fsr2_translation_mode >= 4
        ? static_cast<UINT>(shader_resources.size())
        : 1u;
    context->PSGetShaderResources(0, resource_count, shader_resources.data());
    if (shader_resources[0] == nullptr)
    {
        for (ID3D11ShaderResourceView *view : shader_resources)
        {
            if (view != nullptr)
                view->Release();
        }
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
        record_fsr2_transient_producer(*target, {}, false, "t0_unbound", UINT_MAX);
#endif
        return;
    }

    ResourceInfo candidate_color {};
    if (!read_resource_info(shader_resources[0], L"fsr2_color_candidate", candidate_color) ||
        candidate_color.width != target->render_width ||
        candidate_color.height != target->render_height ||
        !is_linear_scene_color_format(candidate_color.format))
    {
        for (ID3D11ShaderResourceView *view : shader_resources)
        {
            if (view != nullptr)
                view->Release();
        }
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
        record_fsr2_transient_producer(*target, candidate_color, false, "non_linear_or_size_mismatch", UINT_MAX);
#endif
        return;
    }
    if (g_config.fsr2_translation_mode >= 4 && g_config.fsr2_lock_color_producer_shader)
    {
        const std::uint64_t producer_shader_hash =
            g_current_ps_hash.load(std::memory_order_relaxed);
        std::uint64_t locked_shader_hash =
            g_fsr2_locked_color_producer_ps_hash.load(std::memory_order_relaxed);
        if (locked_shader_hash == 0 && producer_shader_hash != 0)
        {
            g_fsr2_locked_color_producer_ps_hash.compare_exchange_strong(
                locked_shader_hash,
                producer_shader_hash,
                std::memory_order_relaxed);
            locked_shader_hash = g_fsr2_locked_color_producer_ps_hash.load(std::memory_order_relaxed);
            if (locked_shader_hash == producer_shader_hash)
                log_line("fsr2_color_producer_shader_locked ps=" + hex64(producer_shader_hash));
        }
        if (locked_shader_hash != 0 && producer_shader_hash != locked_shader_hash)
        {
            for (ID3D11ShaderResourceView *view : shader_resources)
            {
                if (view != nullptr)
                    view->Release();
            }
            const std::uint64_t rejected_count =
                g_fsr2_rejected_color_producer_count.fetch_add(1, std::memory_order_relaxed) + 1;
            if (rejected_count <= 8 || rejected_count % 1024 == 0)
            {
                log_line("fsr2_color_producer_shader_rejected count=" +
                    std::to_string(rejected_count) + " ps=" + hex64(producer_shader_hash) +
                    " locked=" + hex64(locked_shader_hash));
            }
            return;
        }
    }
    shader_resources[0]->AddRef();
    {
        std::lock_guard lock(g_fsr2_candidate_color_view_mutex);
        if (g_fsr2_candidate_color_view != nullptr)
            g_fsr2_candidate_color_view->Release();
        g_fsr2_candidate_color_view = shader_resources[0];
    }
    const std::uint64_t producer_generation = producer_write_generation;
    g_fsr2_candidate_color_resource.store(candidate_color.resource_key, std::memory_order_relaxed);
    g_fsr2_candidate_producer_output_resource.store(target->resource_key, std::memory_order_release);
    g_fsr2_candidate_producer_generation.store(producer_generation, std::memory_order_release);

    const UINT inferred_exposure_slot = g_config.fsr2_translation_mode >= 4
        ? infer_fsr2_exposure_slot(shader_resources)
        : UINT_MAX;
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    record_fsr2_transient_producer(
        *target, candidate_color, true, "matched", inferred_exposure_slot);
#endif

    if (g_config.fsr2_translation_mode >= 4)
    {
        Fsr2ColorReplayState state;
        context->PSGetShader(&state.pixel_shader, nullptr, nullptr);
        state.shader_resources = shader_resources;
        state.exposure_slot = inferred_exposure_slot;
        state.producer_output_resource_key = target->resource_key;
        state.producer_generation = producer_generation;
        state.render_width = target->render_width;
        state.render_height = target->render_height;
        context->PSGetConstantBuffers(0, 1, &state.constant_buffer);
        context->PSGetSamplers(0, static_cast<UINT>(state.samplers.size()), state.samplers.data());
        if (state.pixel_shader == nullptr || state.constant_buffer == nullptr)
        {
            release_fsr2_color_replay_state(state);
            return;
        }
        {
            std::lock_guard lock(g_fsr2_color_replay_mutex);
            release_fsr2_color_replay_state(g_fsr2_color_replay_state);
            g_fsr2_color_replay_state = state;
        }
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
        static std::atomic_uint64_t last_logged_output { 0 };
        const std::uint64_t previous_output = last_logged_output.exchange(
            target->resource_key, std::memory_order_relaxed);
        if (previous_output != target->resource_key)
        {
            log_line("fsr2_dynamic_color_path_learned ps=" +
                hex64(g_current_ps_hash.load(std::memory_order_relaxed)) +
                " output=" + hex64(target->resource_key) +
                " input=" + hex64(candidate_color.resource_key) +
                " render=" + std::to_string(target->render_width) + "x" +
                std::to_string(target->render_height) +
                " exposure_slot=" +
                (state.exposure_slot == UINT_MAX ? std::string("none") : std::to_string(state.exposure_slot)));
        }
#endif
    }
    else
    {
        for (ID3D11ShaderResourceView *view : shader_resources)
        {
            if (view != nullptr)
                view->Release();
        }
    }
}


template <typename DrawCall>
bool replay_fsr2_color_processing(
    ID3D11DeviceContext *context,
    ID3D11ShaderResourceView *fsr_output_view,
    ID3D11RenderTargetView *final_render_target,
    const std::array<ID3D11RenderTargetView *, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> &restore_render_targets,
    ID3D11DepthStencilView *restore_depth_stencil,
    std::uint64_t producer_output_resource_key,
    std::uint64_t producer_generation,
    std::uint32_t render_width,
    std::uint32_t render_height,
    std::uint32_t output_width,
    std::uint32_t output_height,
    DrawCall &&draw_call)
{
    if (context == nullptr || fsr_output_view == nullptr || final_render_target == nullptr)
        return false;

    Fsr2ColorReplayState replay_state;
    if (!acquire_fsr2_color_replay_state(replay_state))
        return false;
    if (replay_state.producer_output_resource_key != producer_output_resource_key ||
        replay_state.producer_generation != producer_generation ||
        replay_state.render_width != render_width || replay_state.render_height != render_height)
    {
        release_fsr2_color_replay_state(replay_state);
        return false;
    }

    ID3D11PixelShader *restore_pixel_shader = nullptr;
    std::array<ID3D11ShaderResourceView *, 7> restore_shader_resources {};
    ID3D11Buffer *restore_constant_buffer = nullptr;
    std::array<ID3D11SamplerState *, 6> restore_samplers {};
    std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> restore_viewports {};
    UINT restore_viewport_count = static_cast<UINT>(restore_viewports.size());
    context->PSGetShader(&restore_pixel_shader, nullptr, nullptr);
    context->PSGetShaderResources(
        0, static_cast<UINT>(restore_shader_resources.size()), restore_shader_resources.data());
    context->PSGetConstantBuffers(0, 1, &restore_constant_buffer);
    context->PSGetSamplers(0, static_cast<UINT>(restore_samplers.size()), restore_samplers.data());
    context->RSGetViewports(&restore_viewport_count, restore_viewports.data());

    std::array<ID3D11ShaderResourceView *, 7> replay_shader_resources = replay_state.shader_resources;
    replay_shader_resources[0] = fsr_output_view;
    D3D11_VIEWPORT replay_viewport {};
    replay_viewport.Width = static_cast<float>(output_width);
    replay_viewport.Height = static_cast<float>(output_height);
    replay_viewport.MinDepth = 0.0f;
    replay_viewport.MaxDepth = 1.0f;

    if (g_original_om_set_render_targets != nullptr)
        g_original_om_set_render_targets(context, 1, &final_render_target, nullptr);
    else
        context->OMSetRenderTargets(1, &final_render_target, nullptr);
    if (g_original_rs_set_viewports != nullptr)
        g_original_rs_set_viewports(context, 1, &replay_viewport);
    else
        context->RSSetViewports(1, &replay_viewport);
    if (g_original_ps_set_shader != nullptr)
        g_original_ps_set_shader(context, replay_state.pixel_shader, nullptr, 0);
    else
        context->PSSetShader(replay_state.pixel_shader, nullptr, 0);
    if (g_original_ps_set_constant_buffers != nullptr)
        g_original_ps_set_constant_buffers(context, 0, 1, &replay_state.constant_buffer);
    else
        context->PSSetConstantBuffers(0, 1, &replay_state.constant_buffer);
    context->PSSetSamplers(0, static_cast<UINT>(replay_state.samplers.size()), replay_state.samplers.data());
    if (g_original_ps_set_shader_resources != nullptr)
    {
        g_original_ps_set_shader_resources(
            context, 0, static_cast<UINT>(replay_shader_resources.size()), replay_shader_resources.data());
    }
    else
    {
        context->PSSetShaderResources(
            0, static_cast<UINT>(replay_shader_resources.size()), replay_shader_resources.data());
    }

    std::forward<DrawCall>(draw_call)();

    std::array<ID3D11ShaderResourceView *, 7> null_shader_resources {};
    if (g_original_ps_set_shader_resources != nullptr)
        g_original_ps_set_shader_resources(
            context, 0, static_cast<UINT>(null_shader_resources.size()), null_shader_resources.data());
    else
        context->PSSetShaderResources(
            0, static_cast<UINT>(null_shader_resources.size()), null_shader_resources.data());
    if (g_original_om_set_render_targets != nullptr)
    {
        g_original_om_set_render_targets(
            context,
            static_cast<UINT>(restore_render_targets.size()),
            restore_render_targets.data(),
            restore_depth_stencil);
    }
    else
    {
        context->OMSetRenderTargets(
            static_cast<UINT>(restore_render_targets.size()),
            restore_render_targets.data(),
            restore_depth_stencil);
    }
    if (g_original_rs_set_viewports != nullptr)
        g_original_rs_set_viewports(context, restore_viewport_count, restore_viewports.data());
    else
        context->RSSetViewports(restore_viewport_count, restore_viewports.data());
    if (g_original_ps_set_shader != nullptr)
        g_original_ps_set_shader(context, restore_pixel_shader, nullptr, 0);
    else
        context->PSSetShader(restore_pixel_shader, nullptr, 0);
    if (g_original_ps_set_constant_buffers != nullptr)
        g_original_ps_set_constant_buffers(context, 0, 1, &restore_constant_buffer);
    else
        context->PSSetConstantBuffers(0, 1, &restore_constant_buffer);
    context->PSSetSamplers(0, static_cast<UINT>(restore_samplers.size()), restore_samplers.data());
    if (g_original_ps_set_shader_resources != nullptr)
    {
        g_original_ps_set_shader_resources(
            context, 0, static_cast<UINT>(restore_shader_resources.size()), restore_shader_resources.data());
    }
    else
    {
        context->PSSetShaderResources(
            0, static_cast<UINT>(restore_shader_resources.size()), restore_shader_resources.data());
    }

    if (restore_pixel_shader != nullptr)
        restore_pixel_shader->Release();
    for (ID3D11ShaderResourceView *view : restore_shader_resources)
    {
        if (view != nullptr)
            view->Release();
    }
    if (restore_constant_buffer != nullptr)
        restore_constant_buffer->Release();
    for (ID3D11SamplerState *sampler : restore_samplers)
    {
        if (sampler != nullptr)
            sampler->Release();
    }
    release_fsr2_color_replay_state(replay_state);

#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    const std::uint64_t replay_index = g_fsr2_color_replay_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (replay_index == 1 || replay_index % 1024 == 0)
        log_line("fsr2_color_replay_succeeded count=" + std::to_string(replay_index));
#endif
    return true;
}

bool copy_fsr2_history_metadata(
    ID3D11DeviceContext *context,
    ID3D11ShaderResourceView *history_metadata_view,
    ID3D11RenderTargetView *output_metadata_view,
    const std::array<ID3D11RenderTargetView *, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> &restore_render_targets,
    ID3D11DepthStencilView *restore_depth_stencil)
{
    if (context == nullptr || history_metadata_view == nullptr || output_metadata_view == nullptr)
        return false;

    ResourceInfo history_info {};
    ResourceInfo output_info {};
    if (!read_resource_info(history_metadata_view, L"fsr2_history_metadata", history_info) ||
        !read_resource_info(output_metadata_view, L"fsr2_output_metadata", output_info) ||
        history_info.width != output_info.width || history_info.height != output_info.height ||
        history_info.format != output_info.format)
    {
        return false;
    }

    ID3D11Resource *history_resource = nullptr;
    ID3D11Resource *output_resource = nullptr;
    history_metadata_view->GetResource(&history_resource);
    output_metadata_view->GetResource(&output_resource);
    if (history_resource == nullptr || output_resource == nullptr || history_resource == output_resource)
    {
        if (history_resource != nullptr)
            history_resource->Release();
        if (output_resource != nullptr)
            output_resource->Release();
        return false;
    }

    ID3D11ShaderResourceView *null_view = nullptr;
    if (g_original_ps_set_shader_resources != nullptr)
        g_original_ps_set_shader_resources(context, 5, 1, &null_view);
    else
        context->PSSetShaderResources(5, 1, &null_view);
    if (g_original_om_set_render_targets != nullptr)
        g_original_om_set_render_targets(context, 0, nullptr, nullptr);
    else
        context->OMSetRenderTargets(0, nullptr, nullptr);

    if (g_original_copy_resource != nullptr)
        g_original_copy_resource(context, output_resource, history_resource);
    else
        context->CopyResource(output_resource, history_resource);

    if (g_original_ps_set_shader_resources != nullptr)
        g_original_ps_set_shader_resources(context, 5, 1, &history_metadata_view);
    else
        context->PSSetShaderResources(5, 1, &history_metadata_view);
    if (g_original_om_set_render_targets != nullptr)
    {
        g_original_om_set_render_targets(
            context,
            static_cast<UINT>(restore_render_targets.size()),
            restore_render_targets.data(),
            restore_depth_stencil);
    }
    else
    {
        context->OMSetRenderTargets(
            static_cast<UINT>(restore_render_targets.size()),
            restore_render_targets.data(),
            restore_depth_stencil);
    }

    history_resource->Release();
    output_resource->Release();
    return true;
}

void release_fsr2_gpu_timing_queries()
{
    for (Fsr2GpuTimingSlot &slot : g_fsr2_gpu_timing_slots)
    {
        if (slot.disjoint != nullptr)
            slot.disjoint->Release();
        for (ID3D11Query *query : slot.timestamps)
        {
            if (query != nullptr)
                query->Release();
        }
        slot = {};
    }
    if (g_fsr2_gpu_timing_device != nullptr)
        g_fsr2_gpu_timing_device->Release();
    g_fsr2_gpu_timing_device = nullptr;
    g_fsr2_gpu_timing_cursor = 0;
    g_fsr2_gpu_timing_accumulated_ms = {};
    g_fsr2_gpu_timing_sample_count = 0;
    g_fsr2_gpu_timing_unavailable_streak = 0;
}

bool collect_fsr2_gpu_timing_slot(ID3D11DeviceContext *context, Fsr2GpuTimingSlot &slot)
{
    if (!slot.pending)
        return true;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint {};
    if (context->GetData(slot.disjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
        return false;
    std::array<UINT64, 5> timestamps {};
    for (std::size_t index = 0; index < timestamps.size(); ++index)
    {
        if (context->GetData(
                slot.timestamps[index],
                &timestamps[index],
                sizeof(timestamps[index]),
                D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
        {
            return false;
        }
    }
    slot.pending = false;
    if (disjoint.Disjoint || disjoint.Frequency == 0)
        return true;

    for (std::size_t stage = 0; stage < g_fsr2_gpu_timing_accumulated_ms.size(); ++stage)
    {
        g_fsr2_gpu_timing_accumulated_ms[stage] +=
            static_cast<double>(timestamps[stage + 1] - timestamps[stage]) * 1000.0 /
            static_cast<double>(disjoint.Frequency);
    }
    ++g_fsr2_gpu_timing_sample_count;
    if (g_fsr2_gpu_timing_sample_count >= 120)
    {
        const double divisor = static_cast<double>(g_fsr2_gpu_timing_sample_count);
        const double upscaler_average_ms = g_fsr2_gpu_timing_accumulated_ms[1] / divisor;
        log_line("fsr2_gpu_timing samples=" + std::to_string(g_fsr2_gpu_timing_sample_count) +
            " prepare_ms=" + std::to_string(g_fsr2_gpu_timing_accumulated_ms[0] / divisor) +
            " upscaler_ms=" + std::to_string(upscaler_average_ms) +
            " metadata_ms=" + std::to_string(g_fsr2_gpu_timing_accumulated_ms[2] / divisor) +
            " color_replay_ms=" + std::to_string(g_fsr2_gpu_timing_accumulated_ms[3] / divisor) +
            " total_ms=" + std::to_string(
                (g_fsr2_gpu_timing_accumulated_ms[0] + g_fsr2_gpu_timing_accumulated_ms[1] +
                    g_fsr2_gpu_timing_accumulated_ms[2] + g_fsr2_gpu_timing_accumulated_ms[3]) /
                divisor));
        const ULONGLONG now = GetTickCount64();
        if (g_config.fsr2_auto_recover_upscaler_ms > 0 &&
            upscaler_average_ms >= static_cast<double>(g_config.fsr2_auto_recover_upscaler_ms) &&
            (g_fsr2_gpu_timing_last_recovery_tick == 0 ||
                now - g_fsr2_gpu_timing_last_recovery_tick >= 10000))
        {
            g_fsr2_gpu_timing_last_recovery_tick = now;
            g_fsr2_translation_recovery_requested.store(true, std::memory_order_release);
            log_line("fsr2_upscaler_stall_detected upscaler_ms=" +
                std::to_string(upscaler_average_ms) + " recovery=requested");
        }
        g_fsr2_gpu_timing_accumulated_ms = {};
        g_fsr2_gpu_timing_sample_count = 0;
    }
    return true;
}

Fsr2GpuTimingSlot *begin_fsr2_gpu_timing(ID3D11DeviceContext *context)
{
    if (!g_config.fsr2_gpu_timing || context == nullptr)
        return nullptr;
    ID3D11Device *device = nullptr;
    context->GetDevice(&device);
    if (device == nullptr)
        return nullptr;

    std::lock_guard lock(g_fsr2_gpu_timing_mutex);
    if (g_fsr2_gpu_timing_device != device)
    {
        release_fsr2_gpu_timing_queries();
        g_fsr2_gpu_timing_device = device;
        D3D11_QUERY_DESC disjoint_description { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
        D3D11_QUERY_DESC timestamp_description { D3D11_QUERY_TIMESTAMP, 0 };
        for (Fsr2GpuTimingSlot &slot : g_fsr2_gpu_timing_slots)
        {
            if (FAILED(device->CreateQuery(&disjoint_description, &slot.disjoint)))
            {
                release_fsr2_gpu_timing_queries();
                return nullptr;
            }
            for (ID3D11Query *&query : slot.timestamps)
            {
                if (FAILED(device->CreateQuery(&timestamp_description, &query)))
                {
                    release_fsr2_gpu_timing_queries();
                    return nullptr;
                }
            }
        }
    }
    else
    {
        device->Release();
    }

    Fsr2GpuTimingSlot &slot =
        g_fsr2_gpu_timing_slots[g_fsr2_gpu_timing_cursor++ % g_fsr2_gpu_timing_slots.size()];
    if (!collect_fsr2_gpu_timing_slot(context, slot))
    {
        ++g_fsr2_gpu_timing_unavailable_streak;
        if (g_fsr2_gpu_timing_unavailable_streak == 120 ||
            g_fsr2_gpu_timing_unavailable_streak % 600 == 0)
        {
            log_line("fsr2_gpu_queue_backlog unavailable_queries=" +
                std::to_string(g_fsr2_gpu_timing_unavailable_streak) +
                " ring_size=" + std::to_string(g_fsr2_gpu_timing_slots.size()));
        }
        return nullptr;
    }
    if (g_fsr2_gpu_timing_unavailable_streak >= 120)
    {
        log_line("fsr2_gpu_queue_recovered unavailable_queries=" +
            std::to_string(g_fsr2_gpu_timing_unavailable_streak));
    }
    g_fsr2_gpu_timing_unavailable_streak = 0;
    context->Begin(slot.disjoint);
    context->End(slot.timestamps[0]);
    return &slot;
}

void end_fsr2_gpu_timing(ID3D11DeviceContext *context, Fsr2GpuTimingSlot *slot)
{
    if (context == nullptr || slot == nullptr)
        return;
    context->End(slot->timestamps[4]);
    context->End(slot->disjoint);
    slot->pending = true;
}

bool consume_fsr2_optiscaler_config_reset()
{
    if (!g_config.fsr2_reset_on_optiscaler_config_change)
        return false;

    const ULONGLONG now = GetTickCount64();
    std::uint64_t next_poll = g_fsr2_optiscaler_config_next_poll_tick.load(std::memory_order_relaxed);
    if (now >= next_poll &&
        g_fsr2_optiscaler_config_next_poll_tick.compare_exchange_strong(
            next_poll,
            now + 250,
            std::memory_order_relaxed))
    {
        WIN32_FILE_ATTRIBUTE_DATA attributes {};
        const std::filesystem::path optiscaler_config =
            g_module_dir.parent_path() / L"OptiScaler" / L"OptiScaler.ini";
        if (GetFileAttributesExW(
                optiscaler_config.c_str(),
                GetFileExInfoStandard,
                &attributes) != 0)
        {
            ULARGE_INTEGER last_write {};
            last_write.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
            last_write.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
            const std::uint64_t previous_write = g_fsr2_optiscaler_config_last_write.exchange(
                last_write.QuadPart,
                std::memory_order_relaxed);
            if (previous_write != 0 && previous_write != last_write.QuadPart)
            {
                g_fsr2_optiscaler_config_reset_frames_remaining.store(
                    g_config.fsr2_optiscaler_config_reset_frames,
                    std::memory_order_release);
                log_line("fsr2_optiscaler_config_changed reset_frames=" +
                    std::to_string(g_config.fsr2_optiscaler_config_reset_frames));
            }
        }
    }

    std::uint32_t remaining =
        g_fsr2_optiscaler_config_reset_frames_remaining.load(std::memory_order_acquire);
    while (remaining != 0)
    {
        if (g_fsr2_optiscaler_config_reset_frames_remaining.compare_exchange_weak(
                remaining,
                remaining - 1,
                std::memory_order_acq_rel))
        {
            return true;
        }
    }
    return false;
}

bool should_reset_for_optiscaler_log_activity()
{
    if (!g_config.fsr2_reset_on_optiscaler_log_change)
        return false;

    const ULONGLONG now = GetTickCount64();
    std::uint64_t next_poll = g_fsr2_optiscaler_log_next_poll_tick.load(std::memory_order_relaxed);
    if (now >= next_poll &&
        g_fsr2_optiscaler_log_next_poll_tick.compare_exchange_strong(
            next_poll,
            now + 100,
            std::memory_order_relaxed))
    {
        WIN32_FILE_ATTRIBUTE_DATA attributes {};
        const std::filesystem::path optiscaler_log =
            g_module_dir.parent_path() / L"OptiScaler" / L"OptiScaler.log";
        if (GetFileAttributesExW(optiscaler_log.c_str(), GetFileExInfoStandard, &attributes) != 0)
        {
            ULARGE_INTEGER last_write {};
            last_write.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
            last_write.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
            const std::uint64_t previous_write = g_fsr2_optiscaler_log_last_write.exchange(
                last_write.QuadPart,
                std::memory_order_relaxed);
            if (previous_write != 0 && previous_write != last_write.QuadPart)
            {
                const ULONGLONG reset_until = now + g_config.fsr2_optiscaler_log_reset_duration_ms;
                g_fsr2_optiscaler_log_reset_until_tick.store(reset_until, std::memory_order_release);
                log_line("fsr2_optiscaler_log_activity reset_duration_ms=" +
                    std::to_string(g_config.fsr2_optiscaler_log_reset_duration_ms));
            }
        }
    }

    return now < g_fsr2_optiscaler_log_reset_until_tick.load(std::memory_order_acquire);
}

// SEH 保护的进程内浮点转储（读取游戏内存；仅用于诊断，指针无效时安全返回 0）。
static std::size_t read_game_floats_seh(std::uint64_t ptr, float *out, std::size_t count)
{
    if (ptr == 0)
        return 0;
    __try
    {
        std::memcpy(out, reinterpret_cast<const void *>(ptr), count * sizeof(float));
        return count;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

template <typename DrawCall>
bool try_fsr2_translation_draw(
    ID3D11DeviceContext *context,
    UINT element_count,
    const std::optional<TargetUpscalerDrawInfo> &inspected_draw_info,
    DrawCall &&draw_call)
{
    const std::uint32_t translation_mode = g_config.fsr2_translation_mode;
    if (translation_mode == 0 || context == nullptr)
        return false;

    // 旧翻译层上下文重置（upscaler_stall 恢复）已移除——新架构仅 ffx12 链路。

    if (unsafe_dx11_on12_backend_selected())
    {
        if (!g_fsr2_dx11on12_block_logged.load(std::memory_order_relaxed) &&
            !g_fsr2_dx11on12_block_logged.exchange(true, std::memory_order_relaxed))
            log_line("fsr2_translation_blocked unsafe_dx11_on12_backend=1 fallback=original_draw");
        return false;
    }
    if (g_fsr2_dx11on12_block_logged.load(std::memory_order_relaxed))
        g_fsr2_dx11on12_block_logged.store(false, std::memory_order_relaxed);

    // 两个调用点（hooked_draw / hooked_draw_indexed）都已在本帧对该 draw inspect
    // 过一次并传入结果；这里不再回退重查，避免按需查询模式下的重复内省。
    const std::optional<TargetUpscalerDrawInfo> &draw_info = inspected_draw_info;
    if (!draw_info)
        return false;
    if (draw_info->render_width >= draw_info->output_width &&
        draw_info->render_height >= draw_info->output_height)
    {
        return false;
    }

    const std::uint64_t candidate_index =
        g_fsr2_translation_candidate_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (candidate_index == 1)
    {
        log_line("fsr2_translation_candidate render=" +
            std::to_string(draw_info->render_width) + "x" + std::to_string(draw_info->render_height) +
            " output=" + std::to_string(draw_info->output_width) + "x" + std::to_string(draw_info->output_height) +
            " mode=" + std::to_string(translation_mode));
    }

    // ---- Phase 2: 调用点驱动的 FSR2 2.3.4 后端（不经 OptiScaler、不经 cb0 jitter） ----
    if (g_config.ffx12 && il2cpp_callsite::active())
    {
        // 2026-08-25（多实例修复）：状态按实例隔离。旧实现 = 全局代次门控 + 全局参数：
        // UI/次实例的 Render 会抬升全局代次并覆写全局参数 → 主 accumulate draw 偶发
        // 用错实例（jitter/frame/context key）或 gate 提前命中 → 原生帧漏出（无蓝框）+ 闪烁。
        // 现在：hook 侧按实例存参数/代次；draw→实例 用"输出关联"（该实例写过的双缓冲
        // 输出指针 r1a/r1b，确定性；实例重建后输出纹理复用，按"最新 Render 代次"裁决）
        // + "尺寸+最新代次"bootstrap；每实例代次门控（每 Render 一次派发）。
        struct Sdk234InstState
        {
            std::uint64_t instance = 0;
            std::uint64_t last_gen = 0;           // 已消费代次
            // 仅诊断：Render generation 与游戏 frame_index 不是同一概念。若同一
            // frame_index 被多个新 generation 推进 SDK history，记录事实但不改变
            // dispatch 行为，避免在未证实前把正常多视图渲染误判为重复帧。
            std::uint32_t last_dispatched_frame = 0;
            std::uint64_t last_dispatched_frame_gen = 0;
            std::uint64_t duplicate_frame_count = 0;
            std::uint64_t last_dispatch_tick = 0; // 接管空窗检测
            std::uint64_t last_frame_tick = 0;    // 帧间隔测量
            // 帧时间步（）：高精度 + 游戏帧号门控——
            // 同帧多次 dispatch（alternate 双缓冲/多视图）共享同一时间步；
            // 不同帧间 dt = QPC 实测真实帧间隔。
            std::uint64_t last_frame_index = 0;
            std::uint64_t last_dispatch_us = 0;
            float last_dt_ms = 16.7f;
            std::uint64_t last_gap_log_tick = 0;
            std::uint64_t out_a = 0, out_b = 0;   // 该实例写过的输出（双缓冲）
            std::uint64_t out_a_tick = 0, out_b_tick = 0; // 各输出上次派发时间（每输出 gate）
            ID3D11Texture2D *last_sdk_output = nullptr; // 当前 Render generation 的首个 SDK 输出（持有引用）
            std::uint64_t last_sdk_output_gen = 0;
            std::uint64_t last_repair_gen = 0; // 每个 canonical generation 至多修复一次 alternate
            // direct-only 故障恢复：失败 token 绝不能在 TTL 内反复推进或复用旧输出；
            // 下一枚有效 token 必须以 reset 重新建立 history。
            bool reset_next = false;
            std::uint64_t invalidated_gen = 0;
            std::uint64_t invalidation_count = 0;
            float prev_jx = 0.0f, prev_jy = 0.0f; // jdelay 上一帧 jitter
            // 2026-08-25（AA 修复）：cb0 jitter 槽位自适应——不同视图/着色器变体的 jitter
            // 可能在不同槽位（[28].xy / [27].zw / [26].zw，probe 实证布局）。探测两帧后
            // **锁定**首个逐帧变化的槽（每帧重选会弹跳混入恒定槽零抖动帧 → 破坏累积相位）。
            float jit_probe[3][2] {}; // 首帧各槽探测值
            int jit_state = 0;        // 0=首帧探测, 1=已锁定
            bool jit_locked = false;
            int jit_slot = 0;
        };
        static Sdk234InstState sdk234_insts[8] {};
        static std::size_t sdk234_inst_count = 0;
        static std::atomic_uint64_t sdk234_dispatch_count { 0 };
        static std::atomic_uint64_t sdk234_inst_log_tick { 0 };

        // 1) 当前 draw 的输出指针（draw→实例 输出关联；与 cover-skip 同源同值）
        ID3D11RenderTargetView *bind_rtvs[4] {};
        ID3D11DepthStencilView *bind_dsv = nullptr;
        context->OMGetRenderTargets(4, bind_rtvs, &bind_dsv);
        ID3D11Resource *draw_out_res = nullptr;
        const UINT draw_output_slot = draw_info->output_rtv_slot;
        if (draw_output_slot < std::size(bind_rtvs) && bind_rtvs[draw_output_slot] != nullptr)
            bind_rtvs[draw_output_slot]->GetResource(&draw_out_res);
        for (int i = 0; i < 4; ++i)
            if (bind_rtvs[i] != nullptr)
                bind_rtvs[i]->Release();
        if (bind_dsv != nullptr)
            bind_dsv->Release();
        const std::uint64_t draw_out_ptr = reinterpret_cast<std::uint64_t>(draw_out_res);
        if (draw_out_res != nullptr)
            draw_out_res->Release();

        // 2) draw→实例 匹配
        il2cpp_callsite::CapturedParams call_params {};
        std::uint64_t call_gen = 0;
        std::uint64_t match_inst = 0;
        // A draw may legitimately use the alternate FSR output buffer without
        // entering Render again.  Keep this association separate from a fresh
        // token: it is only eligible for a bounded canonical-output repair,
        // never for another temporal dispatch.
        std::uint64_t output_associated_inst = 0;
        bool output_association_ambiguous = false;
        {
            std::uint64_t insts[8] {};
            std::size_t inst_n = 0;
            il2cpp_callsite::known_instances(insts, 8, inst_n);
            // A) 输出关联只作为候选提示。候选必须仍有未消费的 Render
            // token；旧实例虽然可能复用同一 output 指针，但没有 pending
            // token 时绝不能再次认领当前 draw。
            {
                std::uint64_t best_gen = 0;
                for (std::size_t i = 0; i < inst_n; ++i)
                {
                    bool out_match = false;
                    for (std::size_t j = 0; j < sdk234_inst_count; ++j)
                    {
                        if (sdk234_insts[j].instance == insts[i] &&
                            ((sdk234_insts[j].out_a != 0 && sdk234_insts[j].out_a == draw_out_ptr) ||
                             (sdk234_insts[j].out_b != 0 && sdk234_insts[j].out_b == draw_out_ptr)))
                        {
                            out_match = true;
                            break;
                        }
                    }
                    if (!out_match)
                        continue;
                    if (output_associated_inst == 0)
                    {
                        output_associated_inst = insts[i];
                    }
                    else if (output_associated_inst != insts[i])
                    {
                        output_association_ambiguous = true;
                    }
                    il2cpp_callsite::RenderToken token {};
                    if (!il2cpp_callsite::latest_pending_render_token_for(
                            insts[i], draw_info->render_width, draw_info->render_height, token))
                        continue;
                    if (token.generation > best_gen)
                    {
                        best_gen = token.generation;
                        match_inst = insts[i];
                        call_gen = token.generation;
                        call_params = token.params;
                    }
                }
            }
            if (output_association_ambiguous)
                output_associated_inst = 0;
            // B) bootstrap：仅从尚未消费的 token 选取最新 Render。不能再
            // 用每实例的 latest snapshot 猜测，否则切界面后旧实例会永久
            // 把已消费 generation 认领为当前 draw。
            if (match_inst == 0)
            {
                std::uint64_t best_gen = 0;
                for (std::size_t i = 0; i < inst_n; ++i)
                {
                    il2cpp_callsite::RenderToken token {};
                    if (!il2cpp_callsite::latest_pending_render_token_for(
                            insts[i], draw_info->render_width, draw_info->render_height, token))
                        continue;
                    if (token.generation > best_gen)
                    {
                        best_gen = token.generation;
                        match_inst = insts[i];
                        call_gen = token.generation;
                        call_params = token.params;
                    }
                }
            }
            // No pending token means this is not a new temporal input.  It
            // can nevertheless be the paired write to an alternate output
            // buffer immediately after the current SDK dispatch.
            if (match_inst == 0 && output_associated_inst != 0)
                match_inst = output_associated_inst;
        }

        // 3) 每实例状态槽
        Sdk234InstState *st = nullptr;
        if (match_inst != 0)
        {
            std::size_t slot = sdk234_inst_count;
            for (std::size_t j = 0; j < sdk234_inst_count; ++j)
            {
                if (sdk234_insts[j].instance == match_inst)
                {
                    slot = j;
                    break;
                }
            }
            if (slot == sdk234_inst_count)
            {
                if (sdk234_inst_count < 8)
                {
                    slot = sdk234_inst_count++;
                }
                else
                {
                    std::size_t oldest = 0;
                    for (std::size_t j = 1; j < 8; ++j)
                        if (sdk234_insts[j].last_dispatch_tick < sdk234_insts[oldest].last_dispatch_tick)
                            oldest = j;
                    slot = oldest;
                }
                sdk234_insts[slot] = Sdk234InstState {};
                sdk234_insts[slot].instance = match_inst;
                log_line("ffx12_instance_new inst=" + hex64(match_inst & 0xFFFFFFFFull) +
                    " frame=" + std::to_string(call_params.frame_index) +
                    " render=" + std::to_string(call_params.render_w) + "x" +
                    std::to_string(call_params.render_h));
            }
            st = &sdk234_insts[slot];
        }

        // 该路径保持 direct-only：失败也不能让原生 FSR2 覆盖输出。不过不能保留
        // 旧 token/旧 canonical output，否则下一 draw 会重试同一历史或复制陈旧帧。
        auto invalidate_direct_attempt = [&](const char *reason)
        {
            if (st == nullptr || call_gen == 0)
                return;
            st->reset_next = true;
            st->invalidated_gen = call_gen;
            ++st->invalidation_count;
            st->last_dispatch_tick = 0;
            st->last_frame_tick = 0;
            // 帧时间基准一并失效：失败后的下一次 dispatch 重新以 16.7 兜底起步
            st->last_dispatch_us = 0;
            st->last_frame_index = 0;
            st->out_a = st->out_b = 0;
            st->out_a_tick = st->out_b_tick = 0;
            if (st->last_sdk_output != nullptr)
            {
                st->last_sdk_output->Release();
                st->last_sdk_output = nullptr;
            }
            st->last_sdk_output_gen = 0;
            const bool consumed = il2cpp_callsite::consume_render_token_for(match_inst, call_gen);
            log_line("ffx12_result rc=" + std::string(reason) +
                " gen=" + std::to_string(call_gen) +
                " inst=" + hex64(match_inst & 0xFFFFFFFFull) +
                " frame=" + std::to_string(call_params.frame_index) +
                " token_consumed=" + std::to_string(consumed ? 1 : 0) +
                " action=invalidate_reset");
        };

        // 4) 每实例 gate：只允许新 Render generation 推进 temporal history。
        //    hooked_draw/hooked_draw_indexed 双入口的第二次进入 = 同代次 → 由下方 cover-skip
        //    （同输出跳过）拦截。
        //    旧的按输出 5ms 兜底会把同一个 generation 的旧 color/depth/motion 再次 dispatch。
        //    这等价于错误地额外推进一次 FSR history，会形成方向性残影；无法证明新帧时
        //    必须回退原生路径，不能用时间阈值猜测。
        const std::uint64_t sdk234_now = GetTickCount64();
        const bool sdk234_gen_fresh = st != nullptr && call_gen != 0 && call_gen != st->last_gen;
        const bool sdk234_duplicate_game_frame = sdk234_gen_fresh &&
            st->last_dispatched_frame_gen != 0 &&
            call_params.frame_index == st->last_dispatched_frame;
        if (sdk234_duplicate_game_frame)
        {
            ++st->duplicate_frame_count;
            log_line("ffx12_duplicate_render_frame inst=" +
                hex64(match_inst & 0xFFFFFFFFull) +
                " frame=" + std::to_string(call_params.frame_index) +
                " gen=" + std::to_string(call_gen) +
                " previous_gen=" + std::to_string(st->last_dispatched_frame_gen) +
                " out=" + hex64(draw_out_ptr) +
                " count=" + std::to_string(st->duplicate_frame_count));
        }
        // 仅允许“无 token + 输出明确属于该实例双缓冲 + canonical 输出确实对应
        // 最近已消费 generation”的 paired draw 做 repair。不能只凭时间窗猜测，
        // 否则普通 draw 会把上一帧复制到当前缓冲并造成旧帧闪烁。
        const bool output_belongs_to_instance = st != nullptr && draw_out_ptr != 0 &&
            ((st->out_a != 0 && st->out_a == draw_out_ptr) ||
             (st->out_b != 0 && st->out_b == draw_out_ptr));
        const bool sdk234_output_duplicate = st != nullptr && call_gen == 0 &&
            output_associated_inst == match_inst && output_belongs_to_instance &&
            st->last_sdk_output != nullptr && st->last_gen != 0 &&
            st->last_sdk_output_gen == st->last_gen && st->last_dispatch_tick != 0 &&
            st->last_repair_gen != st->last_gen &&
            sdk234_now - st->last_dispatch_tick <= 50;
        if (match_inst != 0 && st != nullptr && (sdk234_gen_fresh || sdk234_output_duplicate))
        {
            // 记录 sdk234 拦截的 draw 的 ps hash（确认是否游戏 accumulate 0x78057A29AF6C2D99）
            {
                static std::atomic_uint64_t sdk234_enter_count { 0 };
                const std::uint64_t ec = sdk234_enter_count.fetch_add(1, std::memory_order_relaxed) + 1;
                if (ec <= 4 || ec % 1024 == 0)
                {
                    std::uint64_t ps_h = 0;
                    {
                        std::lock_guard lock(g_state_mutex);
                        ps_h = g_state.current_ps_hash;
                    }
                    log_line("ffx12_enter count=" + std::to_string(ec) +
                        " ps_hash=" + hex64(ps_h) +
                        " inst=" + hex64(match_inst & 0xFFFFFFFFull) +
                        " frame=" + std::to_string(call_params.frame_index));
                }
            }
            // 现场查询当前 accumulate draw 的绑定。角色和尺寸均由 draw 特征识别决定；
            // 不假定 t0/t2/t3 或 RTV1，也不把窗口尺寸当作 render size。
            ID3D11ShaderResourceView *bound_srvs[8] {};
            context->PSGetShaderResources(0, 8, bound_srvs);
            ID3D11RenderTargetView *bound_rtvs[4] {};
            ID3D11DepthStencilView *bound_dsv = nullptr;
            context->OMGetRenderTargets(4, bound_rtvs, &bound_dsv);
            ID3D11Texture2D *color_tex = nullptr;
            ID3D11Texture2D *depth_tex = nullptr;
            ID3D11Texture2D *motion_tex = nullptr;
            ID3D11Texture2D *transparency_tex = nullptr;
            ID3D11Texture2D *output_tex = nullptr;
            ID3D11Resource *sdk234_out_res = nullptr;
            ID3D11Resource *res = nullptr;
            const UINT color_slot = draw_info->color_srv_slot;
            const UINT depth_slot = draw_info->depth_srv_slot;
            const UINT motion_slot = draw_info->motion_srv_slot;
            const UINT transparency_slot = draw_info->transparency_srv_slot;
            const UINT output_slot = draw_info->output_rtv_slot;
            if (color_slot < std::size(bound_srvs) && bound_srvs[color_slot] &&
                (bound_srvs[color_slot]->GetResource(&res), res))
            {
                res->QueryInterface(IID_PPV_ARGS(&color_tex));
                res->Release();
            }
            if (depth_slot < std::size(bound_srvs) && bound_srvs[depth_slot] &&
                (bound_srvs[depth_slot]->GetResource(&res), res))
            {
                res->QueryInterface(IID_PPV_ARGS(&depth_tex));
                res->Release();
            }
            if (motion_slot < std::size(bound_srvs) && bound_srvs[motion_slot] &&
                (bound_srvs[motion_slot]->GetResource(&res), res))
            {
                res->QueryInterface(IID_PPV_ARGS(&motion_tex));
                res->Release();
            }
            if (transparency_slot < std::size(bound_srvs) && bound_srvs[transparency_slot] &&
                (bound_srvs[transparency_slot]->GetResource(&res), res))
            {
                res->QueryInterface(IID_PPV_ARGS(&transparency_tex));
                res->Release();
            }
            if (output_slot < std::size(bound_rtvs) && bound_rtvs[output_slot] &&
                (bound_rtvs[output_slot]->GetResource(&res), res))
            {
                sdk234_out_res = res; // 记录 GetResource 原始指针（防覆盖比较用，与转译层 output 同一来源）
                res->QueryInterface(IID_PPV_ARGS(&output_tex));
                res->Release();
            }
            for (int i = 0; i < 8; ++i)
                if (bound_srvs[i]) bound_srvs[i]->Release();
            for (int i = 0; i < 4; ++i)
                if (bound_rtvs[i]) bound_rtvs[i]->Release();
            if (bound_dsv) bound_dsv->Release();
            // The un-tokened paired draw must not run the native upscaler and
            // must not advance FSR history a second time.  Refresh only its
            // alternate output from the SDK result produced moments ago; this
            // prevents a stale buffer from flashing between two valid frames.
            if (sdk234_output_duplicate && output_tex != nullptr)
            {
                D3D11_TEXTURE2D_DESC source_desc {};
                D3D11_TEXTURE2D_DESC target_desc {};
                st->last_sdk_output->GetDesc(&source_desc);
                output_tex->GetDesc(&target_desc);
                const bool copy_compatible = source_desc.Width == target_desc.Width &&
                    source_desc.Height == target_desc.Height && source_desc.Format == target_desc.Format &&
                    source_desc.MipLevels == target_desc.MipLevels &&
                    source_desc.ArraySize == target_desc.ArraySize &&
                    source_desc.SampleDesc.Count == target_desc.SampleDesc.Count &&
                    source_desc.SampleDesc.Quality == target_desc.SampleDesc.Quality;
                if (copy_compatible)
                {
                    const bool same_output = st->last_sdk_output == output_tex;
                    if (!same_output)
                        context->CopyResource(output_tex, st->last_sdk_output);
                    static std::atomic_uint64_t sdk234_output_repair_count { 0 };
                    const std::uint64_t repair_count =
                        sdk234_output_repair_count.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (repair_count <= 8 || repair_count % 1024 == 0)
                    {
                        log_line("ffx12_output_repair count=" + std::to_string(repair_count) +
                            " inst=" + hex64(match_inst & 0xFFFFFFFFull) +
                            " age_ms=" + std::to_string(sdk234_now - st->last_dispatch_tick) +
                            " same_output=" + std::to_string(same_output ? 1 : 0) +
                            " src=" + hex64(reinterpret_cast<std::uint64_t>(st->last_sdk_output)) +
                            " dst=" + hex64(reinterpret_cast<std::uint64_t>(output_tex)));
                    }
                    st->last_repair_gen = st->last_gen;
                    g_sdk234_output_ptr = reinterpret_cast<std::uint64_t>(sdk234_out_res);
                    g_sdk234_output_tick.store(GetTickCount64(), std::memory_order_relaxed);
                    if (st->out_a == 0 || st->out_a == draw_out_ptr)
                    {
                        st->out_a = draw_out_ptr;
                        st->out_a_tick = GetTickCount64();
                    }
                    else if (st->out_b == 0 || st->out_b == draw_out_ptr)
                    {
                        st->out_b = draw_out_ptr;
                        st->out_b_tick = GetTickCount64();
                    }
                    else
                    {
                        st->out_b = st->out_a;
                        st->out_b_tick = st->out_a_tick;
                        st->out_a = draw_out_ptr;
                        st->out_a_tick = GetTickCount64();
                    }
                    if (color_tex) color_tex->Release();
                    if (depth_tex) depth_tex->Release();
                    if (motion_tex) motion_tex->Release();
                    if (transparency_tex) transparency_tex->Release();
                    output_tex->Release();
                    return true;
                }
            }
            if (!sdk234_output_duplicate && color_tex && depth_tex && motion_tex && output_tex)
            {
                // 双缓冲/双入口可能在同一游戏 Render generation 再次命中 accumulate。
                // 旧逻辑在每输出超过 5ms 时再次 ffxDispatch，导致一套 color/depth/motion
                // 连续推进同一 FSR2 history 两次，形成多帧叠影。这里将首个 SDK 结果复制到
                // 第二输出，仍拦截原生 draw，但不再推进 temporal history。
                if (!sdk234_gen_fresh && g_config.ffx12_reuse_same_generation &&
                    st->last_sdk_output != nullptr && st->last_sdk_output_gen == call_gen)
                {
                    D3D11_TEXTURE2D_DESC prior_desc {};
                    D3D11_TEXTURE2D_DESC target_desc {};
                    st->last_sdk_output->GetDesc(&prior_desc);
                    output_tex->GetDesc(&target_desc);
                    const bool copy_compatible = prior_desc.Width == target_desc.Width &&
                        prior_desc.Height == target_desc.Height && prior_desc.Format == target_desc.Format &&
                        prior_desc.MipLevels == target_desc.MipLevels &&
                        prior_desc.ArraySize == target_desc.ArraySize &&
                        prior_desc.SampleDesc.Count == target_desc.SampleDesc.Count &&
                        prior_desc.SampleDesc.Quality == target_desc.SampleDesc.Quality;
                    if (copy_compatible)
                    {
                        const bool same_output = st->last_sdk_output == output_tex;
                        if (!same_output)
                            context->CopyResource(output_tex, st->last_sdk_output);
                        static std::atomic_uint64_t sdk234_reuse_count { 0 };
                        const std::uint64_t rc = sdk234_reuse_count.fetch_add(1, std::memory_order_relaxed) + 1;
                        if (rc <= 8 || rc % 1024 == 0)
                        {
                            log_line("ffx12_reuse_generation count=" + std::to_string(rc) +
                                " inst=" + hex64(match_inst & 0xFFFFFFFFull) +
                                " frame=" + std::to_string(call_params.frame_index) +
                                " same_output=" + std::to_string(same_output ? 1 : 0) +
                                " src=" + hex64(reinterpret_cast<std::uint64_t>(st->last_sdk_output)) +
                                " dst=" + hex64(reinterpret_cast<std::uint64_t>(output_tex)));
                        }
                        g_sdk234_output_ptr = reinterpret_cast<std::uint64_t>(sdk234_out_res);
                        g_sdk234_output_tick.store(GetTickCount64(), std::memory_order_relaxed);
                        if (st->out_a == 0 || st->out_a == draw_out_ptr)
                        {
                            st->out_a = draw_out_ptr;
                            st->out_a_tick = GetTickCount64();
                        }
                        else if (st->out_b == 0 || st->out_b == draw_out_ptr)
                        {
                            st->out_b = draw_out_ptr;
                            st->out_b_tick = GetTickCount64();
                        }
                        else
                        {
                            st->out_b = st->out_a;
                            st->out_b_tick = st->out_a_tick;
                            st->out_a = draw_out_ptr;
                            st->out_a_tick = GetTickCount64();
                        }
                        color_tex->Release();
                        depth_tex->Release();
                        motion_tex->Release();
                        if (transparency_tex) transparency_tex->Release();
                        output_tex->Release();
                        return true;
                    }
                }
                // 2026-08-24（）：切断原生 FSR2，全部实例接管，只允许 FSR2.3.4 SDK。
                // 每实例独立 FSR2 context（后端按 instance_key 隔离历史）；实例重建 = 新 key = 隐式 reset。
                // 实例切换日志（低频）：记录实例集合变化供诊断。
                {
                    const std::uint64_t now_tick = GetTickCount64();
                    const std::uint64_t last_log = sdk234_inst_log_tick.load(std::memory_order_relaxed);
                    if (last_log == 0 || now_tick - last_log > 10000)
                    {
                        sdk234_inst_log_tick.store(now_tick, std::memory_order_relaxed);
                        log_line("ffx12_instance inst=" +
                            hex64(call_params.instance & 0xFFFFFFFFull) +
                            " frame=" + std::to_string(call_params.frame_index) +
                            " render=" + std::to_string(call_params.render_w) + "x" +
                            std::to_string(call_params.render_h));
                    }
                }
                {
                // 绑定深度（slot2 SRV）实测内容全 0（bits3x3 全 00000000）——
                // 非游戏真实深度缓冲。改用当前 draw 的 DSV 资源（场景深度缓冲本体）：
                // 只要 accumulate 时 DSV 仍绑定则优先替代，否则回退原绑定纹理。
                {
                    ID3D11DepthStencilView *cur_dsv = nullptr;
                    ID3D11RenderTargetView *cur_rtvs[4] = {};
                    context->OMGetRenderTargets(4, cur_rtvs, &cur_dsv);
                    for (auto *r : cur_rtvs)
                        if (r) r->Release();
                    if (cur_dsv != nullptr)
                    {
                        ID3D11Resource *dsv_res = nullptr;
                        cur_dsv->GetResource(&dsv_res);
                        if (dsv_res != nullptr)
                        {
                            ID3D11Texture2D *dsv_tex = nullptr;
                            if (SUCCEEDED(dsv_res->QueryInterface(__uuidof(ID3D11Texture2D),
                                                                  reinterpret_cast<void **>(&dsv_tex))))
                            {
                                D3D11_TEXTURE2D_DESC dtd {};
                                dsv_tex->GetDesc(&dtd);
                                if (dtd.Format != DXGI_FORMAT_UNKNOWN)
                                {
                                    static std::atomic_uint64_t dsv_switch_log { 0 };
                                    const std::uint64_t dsl =
                                        dsv_switch_log.fetch_add(1, std::memory_order_relaxed) + 1;
                                    const std::uint32_t dsv_fmt =
                                        static_cast<std::uint32_t>(dtd.Format);
                                    if (depth_tex != dsv_tex)
                                    {
                                        dsv_tex->AddRef(); // depth_tex 接管一个引用
                                        depth_tex->Release();
                                        depth_tex = dsv_tex;
                                        if (dsl <= 4 || dsl % 256 == 0)
                                            log_line("ffx12_depth_dsv switch fmt=" +
                                                std::to_string(dsv_fmt) +
                                                " size=" + std::to_string(dtd.Width) + "x" +
                                                std::to_string(dtd.Height));
                                    }
                                }
                                dsv_tex->Release();
                            }
                            dsv_res->Release();
                        }
                        cur_dsv->Release();
                    }
                }
                ffx12::FrameInput sdk_in {};
                sdk_in.color = color_tex;
                sdk_in.depth = depth_tex;
                sdk_in.motion = motion_tex;
                sdk_in.transparency = transparency_tex;
                sdk_in.output_target = output_tex;
                sdk_in.reset = g_config.ffx12_force_reset || st->reset_next;
                sdk_in.use_reactive_mask = g_config.fsr2_use_reactive_mask;
                sdk_in.use_transparency_mask = g_config.fsr2_use_transparency_mask;
                // 接管空窗检测（2026-08-24 实证）：UI 全屏/场景切换由另一实例或原生接管，
                // 距上次 dispatch 超 500ms（= 其他路径接管期）→ 恢复时强制 FSR2 reset，清历史残留。
                {
                    const std::uint64_t now_tick = GetTickCount64();
                    if (st->last_dispatch_tick != 0 && now_tick - st->last_dispatch_tick > 500)
                    {
                        sdk_in.reset = true;
                        if (st->last_gap_log_tick == 0 || now_tick - st->last_gap_log_tick > 5000)
                        {
                            st->last_gap_log_tick = now_tick;
                            log_line("ffx12_reset reason=takeover_gap ms=" +
                                std::to_string(now_tick - st->last_dispatch_tick) +
                                " inst=" + hex64(match_inst & 0xFFFFFFFFull));
                        }
                    }
                    st->last_dispatch_tick = now_tick; // 本帧尝试接管即刷新（无论成败，防连续触发）
                }
                sdk_in.render_w = draw_info->render_width;
                sdk_in.render_h = draw_info->render_height;
                sdk_in.display_w = draw_info->output_width;
                sdk_in.display_h = draw_info->output_height;
                if (call_params.render_w != sdk_in.render_w || call_params.render_h != sdk_in.render_h ||
                    call_params.display_w != sdk_in.display_w || call_params.display_h != sdk_in.display_h)
                {
                    static std::atomic_uint64_t resource_dimension_mismatch_count { 0 };
                    const std::uint64_t mismatch_count =
                        resource_dimension_mismatch_count.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (mismatch_count <= 8 || mismatch_count % 1024 == 0)
                    {
                        log_line("ffx12_resource_dimensions count=" + std::to_string(mismatch_count) +
                            " render=" + std::to_string(sdk_in.render_w) + "x" + std::to_string(sdk_in.render_h) +
                            " output=" + std::to_string(sdk_in.display_w) + "x" + std::to_string(sdk_in.display_h) +
                            " call_render=" + std::to_string(call_params.render_w) + "x" + std::to_string(call_params.render_h) +
                            " call_output=" + std::to_string(call_params.display_w) + "x" + std::to_string(call_params.display_h));
                    }
                }
                sdk_in.jitter_x = call_params.jitter_x;
                sdk_in.jitter_y = call_params.jitter_y;
                // Render token 与当前 accumulate draw 已严格配对。当前日志证明 Render
                // jitter 每帧变化，而 cb0[28].xy 在当前 draw 中落后一个 token：frame 3
                // 的 Render jitter 会在下一 draw 的 cb0 才出现。若把它当作当前 jitter，
                // color/motion/history 相位会错一帧，运动时便持续拒绝 history。优先使用
                // 当前 Render jitter；只有字段无效时才以 cb0 为兼容回退。
                float jit_src_x = call_params.jitter_x;
                float jit_src_y = call_params.jitter_y;
                const bool render_jitter_valid = std::isfinite(jit_src_x) && std::isfinite(jit_src_y) &&
                    jit_src_x >= 0.0f && jit_src_x < 1.0f && jit_src_y >= 0.0f && jit_src_y < 1.0f;
                const char *jit_src_name = "render";
                // 直接 staging 读回当前 draw 的 cb0，三候选槽自适应选"逐帧变化"的 jitter：
                //   槽0 = cb0[28].xy（第一视图实证逐帧变化 = halton）
                //   槽1 = cb0[27].zw、槽2 = cb0[26].zw（其他着色器变体可能在此）
                // 选变化槽：避免某视图槽位冻结导致 jitter 冻结 → 无 AA（第 16 轮 0x25FCE520 实证）。
                // 都不变 = 该视图无相机抖动 → 无 AA 可累积（游戏原生同此）。
                if (!render_jitter_valid)
                {
                    ID3D11Buffer *bound_cb = nullptr;
                    context->PSGetConstantBuffers(0, 1, &bound_cb);
                    if (bound_cb != nullptr)
                    {
                        D3D11_BUFFER_DESC bd {};
                        bound_cb->GetDesc(&bd);
                        if (bd.ByteWidth >= 464 && (bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0)
                        {
                            ID3D11Device *dev = nullptr;
                            context->GetDevice(&dev);
                            if (dev != nullptr)
                            {
                                D3D11_BUFFER_DESC sd {};
                                sd.ByteWidth = bd.ByteWidth;
                                sd.Usage = D3D11_USAGE_STAGING;
                                sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                                ID3D11Buffer *staging_cb = nullptr;
                                if (SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &staging_cb)) &&
                                    staging_cb != nullptr)
                                {
                                    context->CopyResource(staging_cb, bound_cb);
                                    context->Flush();
                                    D3D11_MAPPED_SUBRESOURCE m {};
                                    if (SUCCEEDED(context->Map(staging_cb, 0, D3D11_MAP_READ, 0, &m)) &&
                                        m.pData != nullptr)
                                    {
                                        const float *f = static_cast<const float *>(m.pData);
                                        // 候选槽（归一化 jitter ∈ (0,1)；0 视为未写入，≥1 视为脏数据）
                                        float cb[3][2] {};
                                        bool ok[3] = {false, false, false};
                                        const int idx[3][2] = {{112, 113}, {110, 111}, {106, 107}};
                                        for (int s = 0; s < 3; ++s)
                                        {
                                            const float a = f[idx[s][0]], b = f[idx[s][1]];
                                            if ((a != 0.0f || b != 0.0f) && a < 1.0f && b < 1.0f)
                                            {
                                                cb[s][0] = a;
                                                cb[s][1] = b;
                                                ok[s] = true;
                                            }
                                        }
                                        // 锁定式槽位选择：首帧记录探测值；次帧起选首个变化的槽并锁定
                                        if (st->jit_state == 0)
                                        {
                                            for (int s = 0; s < 3; ++s)
                                                if (ok[s])
                                                {
                                                    st->jit_probe[s][0] = cb[s][0];
                                                    st->jit_probe[s][1] = cb[s][1];
                                                }
                                            st->jit_state = 1;
                                        }
                                        else if (!st->jit_locked)
                                        {
                                            for (int s = 0; s < 3; ++s)
                                            {
                                                if (ok[s] &&
                                                    (cb[s][0] != st->jit_probe[s][0] || cb[s][1] != st->jit_probe[s][1]))
                                                {
                                                    st->jit_slot = s;
                                                    break;
                                                }
                                            }
                                            st->jit_locked = true; // 全冻结也锁定（默认槽 0 = 无抖动视图）
                                            static std::atomic_uint64_t jit_slot_log_count { 0 };
                                            const std::uint64_t jc = jit_slot_log_count.fetch_add(1, std::memory_order_relaxed) + 1;
                                            if (jc <= 16)
                                            {
                                                log_line("ffx12_jit_slot inst=" + hex64(match_inst & 0xFFFFFFFFull) +
                                                    " slot=" + std::to_string(st->jit_slot) +
                                                    " v=(" + std::to_string(cb[st->jit_slot][0]) + "," +
                                                    std::to_string(cb[st->jit_slot][1]) + ")");
                                            }
                                        }
                                        const int pick = st->jit_locked ? st->jit_slot : 0;
                                        if (ok[pick])
                                        {
                                            jit_src_x = cb[pick][0];
                                            jit_src_y = cb[pick][1];
                                            jit_src_name = pick == 0 ? "cb0_fallback" :
                                                (pick == 1 ? "cb27_fallback" : "cb26_fallback");
                                        }
                                        else
                                        {
                                            // 锁定槽失效：本帧回退任一有效槽（锁定保持不变）
                                            for (int s = 0; s < 3; ++s)
                                            {
                                                if (ok[s])
                                                {
                                                    jit_src_x = cb[s][0];
                                                    jit_src_y = cb[s][1];
                                                    jit_src_name = s == 0 ? "cb0_fallback" :
                                                        (s == 1 ? "cb27_fallback" : "cb26_fallback");
                                                    break;
                                                }
                                            }
                                        }
                                        context->Unmap(staging_cb, 0);
                                    }
                                    staging_cb->Release();
                                }
                                dev->Release();
                            }
                        }
                        bound_cb->Release();
                    }
                }
                const float use_jx = g_config.ffx12_jitter_delay ? st->prev_jx : jit_src_x;
                const float use_jy = g_config.ffx12_jitter_delay ? st->prev_jy : jit_src_y;
                switch (g_config.ffx12_jitter_mode)
                {
                    case 1: // +norm×render（无 −0.5）
                        sdk_in.jitter_x = use_jx * static_cast<float>(sdk_in.render_w);
                        sdk_in.jitter_y = use_jy * static_cast<float>(sdk_in.render_h);
                        break;
                    case 2: // 原样归一化（≈0，等效关 jitter）
                        break;
                    case 3: // −norm×render + 0.5（模式 0 的 X/Y 翻转，翻译层 Fsr2JitterMode=3 风格）
                        sdk_in.jitter_x = -(use_jx * static_cast<float>(sdk_in.render_w)) + 0.5f;
                        sdk_in.jitter_y = -(use_jy * static_cast<float>(sdk_in.render_h)) + 0.5f;
                        break;
                    case 4: // −norm×render（游戏反汇编约定：采样 = pixel − jitter；默认）
                        sdk_in.jitter_x = -(use_jx * static_cast<float>(sdk_in.render_w));
                        sdk_in.jitter_y = -(use_jy * static_cast<float>(sdk_in.render_h));
                        break;
                    case 5: // 零 jitter（诊断）
                        sdk_in.jitter_x = 0.0f;
                        sdk_in.jitter_y = 0.0f;
                        break;
                    default: // 0: +norm×render − 0.5（旧默认，A/B 保留）
                        sdk_in.jitter_x = use_jx * static_cast<float>(sdk_in.render_w) - 0.5f;
                        sdk_in.jitter_y = use_jy * static_cast<float>(sdk_in.render_h) - 0.5f;
                        break;
                }
                st->prev_jx = jit_src_x;
                st->prev_jy = jit_src_y;
                // OptiScaler 输出定向修复——XeSS/DLSS 的 jitter 需 ≤±0.5
                // （xessD3D12Execute 报 Invalid Argument：jitterOffset 超界）。仅 XeSS/DLSS
                // 激活时把像素 jitter 夹紧到 ±0.5（子像素），FSR 输出保持原样（互不干扰）。
                const OptiOutput opti_out = opti_output_current();
                const bool xess_or_dlss = opti_out == OptiOutput::XeSS || opti_out == OptiOutput::Dlss;
                // XeSS/DLSS 定向——jitter 夹紧 ±0.5（xessD3D12Execute 校验，
                // 静态 AA 生效）；motion 方向/缩放/depth 归一化均实测无效果（OptiScaler 的
                // XeSS 输出链路内部限制）→ 全部回退默认。FSR 输出完全不读取此分支（参数隔离）。
                ffx12::set_motion_flip(1.0f);
                ffx12::set_depth_scale(1.0f);
                if (xess_or_dlss)
                {
                    // XeSS/DLSS 定向——jitter 需 ≤±0.5（xessD3D12Execute 校验）。
                    // FSR 输出完全不读取此分支（参数隔离：opti_out 判定限频 500ms，切换后快速跟随）。
                    auto clamp_half = [](float v) -> float {
                        if (v > 0.5f) return 0.5f;
                        if (v < -0.5f) return -0.5f;
                        return v;
                    };
                    sdk_in.jitter_x = clamp_half(sdk_in.jitter_x);
                    sdk_in.jitter_y = clamp_half(sdk_in.jitter_y);
                }
                // motion scale：FSR2 约定 = ±render 尺寸，符号 = 游戏原生运动方向约定。
                // 2026-08-25 依据游戏 accumulate 反汇编（history_uv = UV − mv_g, mv_g=正向）：
                // FSR2 需要反向运动 → 本桥 −sign 解码已反向 → scale 必须 +renderSize（Fsr2MotionVectorScaleMode=1）。
                const float mv_sign = g_config.fsr2_positive_motion_vector_scale ? 1.0f : -1.0f;
                // motionScale：FSR2/DLSS/XeSS 均按 render 尺寸（输入分辨率像素；display 尺寸已试无变化）。
                sdk_in.motion_scale_x = mv_sign * static_cast<float>(sdk_in.render_w) *
                                        g_config.ffx12_motion_scale;
                sdk_in.motion_scale_y = mv_sign * static_cast<float>(sdk_in.render_h) *
                                        g_config.ffx12_motion_scale;
                // 相机参数：先用 OptiScaler 路径已验证值（原生值待 PRE cb0 探测）。
                // FovScale 配置：AA 重投影依赖 FOV，缩放可快速验证 FOV 是否为 AA 失效元凶。
                sdk_in.camera_near = g_config.ffx12_camera_near;
                sdk_in.camera_far = g_config.ffx12_camera_far;
                sdk_in.camera_fov_vertical =
                    0.7853981634f * g_config.ffx12_fov_scale;
                // 帧时间：FSR 语义 = 游戏帧时长（运动矢量按帧归一化的时间步）。
                // 旧实现按"两次 dispatch"的 GetTickCount64 墙钟间隔：同帧多 dispatch
                // 得 ~0ms（clamp 1），跨帧又受 15.6ms 分辨率量化（1/15/47/100 跳变），
                // 稳定段 dt 从未等于帧时长 → 运动时 temporal 权重错 → 动态 AA 失效。
                // 现按游戏帧号变化用 QPC 高精度计时；同帧重复 dispatch 沿用上次 dt
                // （同一时间步），并只在帧号变化时推进计时基准。
                {
                    LARGE_INTEGER qpc_freq {};
                    LARGE_INTEGER qpc_now {};
                    QueryPerformanceFrequency(&qpc_freq);
                    QueryPerformanceCounter(&qpc_now);
                    const std::uint64_t now_us =
                        qpc_freq.QuadPart != 0 ? static_cast<std::uint64_t>(qpc_now.QuadPart) *
                                                     1000000ull / static_cast<std::uint64_t>(qpc_freq.QuadPart)
                                               : 0ull;
                    const std::uint64_t now_frame = call_params.frame_index;
                    if (st->last_dispatch_us == 0)
                    {
                        sdk_in.frame_time_delta_ms = 16.7f; // 首帧兜底
                    }
                    else if (now_frame != st->last_frame_index)
                    {
                        const std::uint64_t delta_us =
                            now_us > st->last_dispatch_us ? now_us - st->last_dispatch_us : 0ull;
                        sdk_in.frame_time_delta_ms =
                            delta_us != 0 ? static_cast<float>(delta_us) / 1000.0f : 16.7f;
                    }
                    // 同帧多 dispatch：沿用上次 dt（时间步不变）
                    sdk_in.frame_time_delta_ms = std::clamp(sdk_in.frame_time_delta_ms, 1.0f, 100.0f);
                    if (st->last_dispatch_us == 0 || now_frame != st->last_frame_index)
                    {
                        st->last_dispatch_us = now_us;
                        st->last_frame_index = now_frame;
                    }
                    st->last_dt_ms = sdk_in.frame_time_delta_ms;
                    st->last_frame_tick = GetTickCount64();
                }

                // ---- 一次性原生参数探测（诊断用，Ffx12Probe=1 时启用） ----
                if (g_config.ffx12_probe)
                {
                static std::atomic_uint32_t sdk234_probe_round { 0 };
                static std::atomic_bool sdk234_cb0_done { false };
                const std::uint32_t probe_round = sdk234_probe_round.fetch_add(1, std::memory_order_relaxed) + 1;
                const bool probe_full = probe_round == 1;
                const bool probe_cb0 = !sdk234_cb0_done.load(std::memory_order_relaxed) && probe_round <= 32;
                if (probe_full || probe_cb0)
                {
                    std::string probe;
                    if (probe_full)
                        probe = "ffx12_probe";
                    else
                        probe = "ffx12_probe_cb0";
                    if (probe_full)
                    {
                        ID3D11PixelShader *ps = nullptr;
                        context->PSGetShader(&ps, nullptr, nullptr);
                        if (ps)
                        {
                            const ShaderInfo si = lookup_pixel_shader_info(ps);
                            probe += " ps_hash=" + hex64(si.hash);
                            ps->Release();
                        }
                        ID3D11ShaderResourceView *psrvs[8] = {};
                        context->PSGetShaderResources(0, 8, psrvs);
                        for (int i = 0; i < 8; ++i)
                        {
                            if (!psrvs[i])
                                continue;
                            ID3D11Resource *res = nullptr;
                            psrvs[i]->GetResource(&res);
                            if (res)
                            {
                                ID3D11Texture2D *tex = nullptr;
                                if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D),
                                                                  reinterpret_cast<void **>(&tex))))
                                {
                                    D3D11_TEXTURE2D_DESC td {};
                                    tex->GetDesc(&td);
                                    probe += " s" + std::to_string(i) + "=" + std::to_string(td.Format) + "x" +
                                        std::to_string(td.Width) + "x" + std::to_string(td.Height);
                                    tex->Release();
                                }
                                res->Release();
                            }
                            psrvs[i]->Release();
                        }
                        ID3D11RenderTargetView *prtvs[4] = {};
                        ID3D11DepthStencilView *pdsv = nullptr;
                        context->OMGetRenderTargets(4, prtvs, &pdsv);
                        for (int i = 0; i < 4; ++i)
                        {
                            if (!prtvs[i])
                                continue;
                            ID3D11Resource *res = nullptr;
                            prtvs[i]->GetResource(&res);
                            if (res)
                            {
                                ID3D11Texture2D *tex = nullptr;
                                if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D),
                                                                  reinterpret_cast<void **>(&tex))))
                                {
                                    D3D11_TEXTURE2D_DESC td {};
                                    tex->GetDesc(&td);
                                    probe += " r" + std::to_string(i) + "=" + std::to_string(td.Format) + "x" +
                                        std::to_string(td.Width) + "x" + std::to_string(td.Height);
                                    tex->Release();
                                }
                                res->Release();
                            }
                            prtvs[i]->Release();
                        }
                        if (pdsv)
                            pdsv->Release();
                    }
                    std::uint8_t cb0[512] {};
                    std::size_t cb0_size = 0;
                    {
                        ID3D11Buffer *bound_cb = nullptr;
                        context->PSGetConstantBuffers(0, 1, &bound_cb);
                        if (bound_cb)
                        {
                            const std::uint64_t cb_key = reinterpret_cast<std::uint64_t>(bound_cb);
                            g_trace_ps_cb0_key.store(cb_key, std::memory_order_relaxed);
                            const auto snap_it = g_buffer_snapshots.find(cb_key);
                            if (snap_it != g_buffer_snapshots.end())
                            {
                                cb0_size = snap_it->second.size();
                                std::memcpy(cb0, snap_it->second.data(),
                                            cb0_size < sizeof(cb0) ? cb0_size : sizeof(cb0));
                            }
                            bound_cb->Release();
                        }
                    }
                    probe += " cb0_size=" + std::to_string(cb0_size);
                    if (cb0_size > 0)
                    {
                        sdk234_cb0_done.store(true, std::memory_order_relaxed);
                        probe += " cb0=";
                        const std::size_t floats = cb0_size / 4;
                        for (std::size_t i = 0; i < floats; ++i)
                        {
                            probe += std::to_string(reinterpret_cast<const float *>(cb0)[i]);
                            if (i + 1 < floats)
                                probe += ",";
                        }
                    }
                    log_line(probe);
                }
                // ---- 输入内容采样（前 3 轮 + 每 64 轮；motion 3×3 网格 + mvmax） ----
                if (probe_round <= 3 || probe_round % 64 == 0)
                {
                    std::string samples = "ffx12_samples round=" + std::to_string(probe_round);
                    append_tex_samples(samples, "color", color_tex, context);
                    append_tex_samples(samples, "depth", depth_tex, context);
                    append_tex_samples(samples, "motion", motion_tex, context, true, true);
                    bool last_reset = false;
                    std::uint64_t ctx_recreates = 0;
                    ffx12::debug_state(last_reset, ctx_recreates);
                    samples += " reset=" + std::to_string(last_reset ? 1 : 0) +
                        " recreates=" + std::to_string(ctx_recreates);
                    log_line(samples);
                }
                // ---- cb0 逐帧跟踪（前 32 轮）：jitter/frame/instance 与 cb0 tail 的对应 ----
                if (probe_round <= 32)
                {
                    std::string track = "ffx12_cb0_track round=" + std::to_string(probe_round) +
                        " inst=" + hex64(call_params.instance & 0xFFFFFFFFull) +
                        " frame=" + std::to_string(call_params.frame_index) +
                        " jit_norm=" + std::to_string(call_params.jitter_x) + "," +
                        std::to_string(call_params.jitter_y);
                    ID3D11Buffer *bound_cb = nullptr;
                    context->PSGetConstantBuffers(0, 1, &bound_cb);
                    if (bound_cb)
                    {
                        const std::uint64_t cb_key = reinterpret_cast<std::uint64_t>(bound_cb);
                        g_trace_ps_cb0_key.store(cb_key, std::memory_order_relaxed);
                        const auto snap_it = g_buffer_snapshots.find(cb_key);
                        if (snap_it != g_buffer_snapshots.end() && snap_it->second.size() >= 464)
                        {
                            const float *f = reinterpret_cast<const float *>(snap_it->second.data());
                            track += " cb0_96_115=";
                            for (int i = 96; i < 116; ++i)
                            {
                                track += std::to_string(f[i]);
                                if (i < 115)
                                    track += ",";
                            }
                        }
                        else
                        {
                            track += " cb0_snap_missing";
                        }
                        bound_cb->Release();
                    }
                    log_line(track);
                }
                // ---- 一次性实例/上下文内存 dump（前 2 轮；找原生 near/far/fov/exposure 等参数） ----
                if (probe_round <= 2)
                {
                    std::string mem = "ffx12_mem_dump round=" + std::to_string(probe_round);
                    append_mem_dump(mem, "inst", call_params.instance, 32);
                    append_mem_u64(mem, "inst_ptr", call_params.instance, 32);
                    append_mem_dump(mem, "ctx", call_params.context, 16);
                    append_mem_u64(mem, "ctx_ptr", call_params.context, 32);
                    log_line(mem);
                    // 原生参数全量转储（日志截断放不下；第 1 轮写文件，供离线分析 fov/near/far）：
                    // 实例前 8KB + 上下文前 4KB + cb0 全量（496B）
                    if (probe_round == 1)
                    {
                        FILE *f = nullptr;
                        if (fopen_s(&f, "sdk234_native_dump.bin", "wb") == 0 && f)
                        {
                            if (call_params.instance)
                                fwrite(reinterpret_cast<const void *>(call_params.instance), 1, 8192, f);
                            if (call_params.context)
                                fwrite(reinterpret_cast<const void *>(call_params.context), 1, 4096, f);
                            // cb0 全量（含实例字段的 render/display/jitter 段）：快照钩子可能缺失，
                            // 用 staging 直读兜底（CopyResource + Map）
                            {
                                ID3D11Buffer *bound_cb = nullptr;
                                context->PSGetConstantBuffers(0, 1, &bound_cb);
                                if (bound_cb)
                                {
                                    D3D11_BUFFER_DESC bd {};
                                    bound_cb->GetDesc(&bd);
                                    const UINT cb_size = bd.ByteWidth > 4096 ? 4096 : bd.ByteWidth;
                                    D3D11_BUFFER_DESC sd {};
                                    sd.ByteWidth = cb_size;
                                    sd.Usage = D3D11_USAGE_STAGING;
                                    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                                    ID3D11Buffer *staging_cb = nullptr;
                                    ID3D11Device *dev = nullptr;
                                    context->GetDevice(&dev);
                                    if (dev &&
                                        SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &staging_cb)) &&
                                        staging_cb != nullptr)
                                    {
                                        context->CopyResource(staging_cb, bound_cb);
                                        context->Flush();
                                        D3D11_MAPPED_SUBRESOURCE m {};
                                        if (SUCCEEDED(context->Map(staging_cb, 0,
                                                                   D3D11_MAP_READ, 0, &m)) &&
                                            m.pData)
                                        {
                                            fwrite(m.pData, 1, cb_size, f);
                                            context->Unmap(staging_cb, 0);
                                        }
                                        staging_cb->Release();
                                    }
                                    if (dev)
                                        dev->Release();
                                    bound_cb->Release();
                                }
                            }
                            fclose(f);
                            log_line("ffx12_native_dump written=sdk234_native_dump.bin");
                        }
                    }
                }
                } // if (g_config.ffx12_probe)

                if (ffx12::dispatch(sdk_in, context, call_params.instance))
                {
                    // 仅成功输出后才消费 generation；失败不得让下一次重试拿到旧 history。
                    st->last_gen = call_gen;
                    st->last_dispatched_frame = call_params.frame_index;
                    st->last_dispatched_frame_gen = call_gen;
                    st->reset_next = false;
                    if (!il2cpp_callsite::consume_render_token_for(match_inst, call_gen))
                    {
                        log_line("ffx12_token_consume_miss inst=" +
                            hex64(match_inst & 0xFFFFFFFFull) +
                            " gen=" + std::to_string(call_gen));
                    }
                    const std::uint64_t dcount =
                        sdk234_dispatch_count.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (g_config.ffx12_probe || dcount <= 8 || dcount % 1024 == 0)
                    {
                        log_line("ffx12_result rc=DISPATCH_OK" +
                            std::string(" gen=") + std::to_string(call_gen) +
                            " inst=" + hex64(match_inst & 0xFFFFFFFFull) +
                            " frame=" + std::to_string(call_params.frame_index) +
                            " reset=" + std::to_string(sdk_in.reset ? 1 : 0) +
                            " ffx_rc=" + std::to_string(ffx12::last_ffx_dispatch_return_code()) +
                            " in=" + hex64(reinterpret_cast<std::uint64_t>(color_tex)) + "/" +
                            hex64(reinterpret_cast<std::uint64_t>(depth_tex)) + "/" +
                            hex64(reinterpret_cast<std::uint64_t>(motion_tex)) +
                            " out=" + hex64(draw_out_ptr));
                    }
                    // 首次成功时上报实际 FSR 版本（含降级结果）
                    {
                        static std::atomic_bool focus_logged { false };
                        if (!focus_logged.exchange(true, std::memory_order_relaxed))
                            log_focus_block("OK"); // 首次接管成功：显卡/SDK/FSR版本 焦点框
                    }
                    if (dcount == 1)
                    {
                        std::uint64_t d11_luid = 0, d12_luid = 0;
                        ffx12::adapter_luids(d11_luid, d12_luid);
                        log_line("ffx12_adapter d11_luid=" + hex64(d11_luid) +
                            " d12_luid=" + hex64(d12_luid) +
                            " match=" + std::to_string(d11_luid == d12_luid ? 1 : 0) +
                            " (DX11 render + D3D12 FSR2 共享纹理互操作，无 swapchain 转译)");
                    }
                    if (dcount == 1 || dcount % 15 == 0)
                    {
                        update_osd_sdk234(dcount, sdk_in.render_w, sdk_in.render_h,
                                          sdk_in.display_w, sdk_in.display_h);
                    }
                    if (dcount == 1 || dcount % 1024 == 0)
                    {
                        std::wstring sdk_msgs;
                        ffx12::get_sdk_messages(sdk_msgs);
                        HMODULE amdxc = GetModuleHandleW(L"amdxc64.dll");
                        log_line("ffx12_path count=" + std::to_string(dcount) +
                            " version=" + ffx12::selected_version_name() +
                            " chain=pq_decode->ffx_dispatch->pq_encode" +
                            " out=" + hex64(reinterpret_cast<std::uint64_t>(output_tex)) +
                            " render=" + std::to_string(sdk_in.render_w) + "x" +
                            std::to_string(sdk_in.render_h) +
                            " display=" + std::to_string(sdk_in.display_w) + "x" +
                            std::to_string(sdk_in.display_h) +
                            " flags=hdr,deptinv,autoexp" +
                            " amdxc64=" + (amdxc != nullptr ? std::string("loaded") : std::string("missing")) +
                            " sdk_msgs=" + (sdk_msgs.empty() ? std::string("none") : narrow(sdk_msgs)));
                    }
                    if (dcount == 1 || dcount % 1024 == 0)
                    {
                        float dp[16] {};
                        ffx12::debug_pixels(dp);
                        log_line("ffx12_pipeline count=" + std::to_string(dcount) +
                            " pqdec=" + std::to_string(dp[0]) + "," + std::to_string(dp[1]) + "," + std::to_string(dp[2]) +
                            " ffxout=" + std::to_string(dp[4]) + "," + std::to_string(dp[5]) + "," + std::to_string(dp[6]) +
                            " pqenc=" + std::to_string(dp[8]) + "," + std::to_string(dp[9]) + "," + std::to_string(dp[10]) +
                            " mvdec=" + std::to_string(dp[12]) + "," + std::to_string(dp[13]) +
                            " (pqdec=PQ解码输出; ffxout=ffxDispatch输出; pqenc=PQ编码输出; mvdec=motion解码)");
                    }
                    if (dcount == 1 || dcount % 1024 == 0)
                    {
                        bool on12 = false, gpu_only = false;
                        ffx12::interop_capabilities(on12, gpu_only);
                        log_line("ffx12_transport count=" + std::to_string(dcount) +
                            " on12_device=" + std::to_string(on12 ? 1 : 0) +
                            " gpu_only=" + std::to_string(gpu_only ? 1 : 0) +
                            " gpu_interop_ready=" +
                            std::to_string(ffx12::gpu_interop_ready() ? 1 : 0));
                    }
                    if (dcount == 1 || dcount % 1024 == 0)
                    {
                        // 读回 ffx12 后端输出与游戏 rtv1 的 5 点采样：
                        // 判断后端是否有画面内容（四角+中央），定位"空转"还是"显示链未用"
                        std::string bd = "backend";
                        append_tex_samples(bd, "", ffx12::debug_output_texture(), context);
                        std::string r1 = "rtv1";
                        append_tex_samples(r1, "", output_tex, context);
                        log_line("ffx12_backend_samples count=" + std::to_string(dcount) + bd);
                        log_line("ffx12_rtv1_samples count=" + std::to_string(dcount) + r1);
                    }
                    if (dcount == 1 || dcount % 1024 == 0)
                    {
                        // E1+链中点：D3D12 自有输出 5 点 + 链中点二分（金丝雀/解码/派发）。
                        // enc p0=左上（mark=1 时应红 0x3FF00000/0xFF0000）；motion_cvt 非 0=执行正常；
                        // color_linear 非 0=解码+共享输入读正常；output_linear 非 0=ffxDispatch 正常。
                        std::uint32_t os_raw[5] {};
                        std::uint32_t os_w = 0, os_h = 0;
                        bool os_valid = false;
                        ffx12::get_output_samples(os_raw, os_w, os_h, os_valid);
                        ffx12::ChainSampleData cs {};
                        ffx12::get_chain_samples(cs);
                        log_line("ffx12_out_samples count=" + std::to_string(dcount) +
                            " enc_valid=" + std::to_string(os_valid ? 1 : 0) +
                            " dims=" + std::to_string(os_w) + "x" + std::to_string(os_h) +
                            " enc_p0=0x" + hex64(os_raw[0]) + " p1=0x" + hex64(os_raw[1]) +
                            " p2=0x" + hex64(os_raw[2]) + " p3=0x" + hex64(os_raw[3]) +
                            " p4=0x" + hex64(os_raw[4]) +
                            " canary=0x" + hex64(cs.canary) +
                            " co=0x" + hex64(cs.color_own) +
                            " dz=" + std::to_string(cs.depth_own) +
                            " mo=0x" + hex64(cs.motion_own) +
                            " mv_cvt=" + std::to_string(cs.motion_cvt[0]) + "," +
                            std::to_string(cs.motion_cvt[1]) +
                            " cl=" + std::to_string(cs.color_linear[0]) + "," +
                            std::to_string(cs.color_linear[1]) + "," +
                            std::to_string(cs.color_linear[2]) +
                            " ol=" + std::to_string(cs.output_linear[0]) + "," +
                            std::to_string(cs.output_linear[1]) + "," +
                            std::to_string(cs.output_linear[2]) +
                            " (canary=执行+UAV+readback全通; co/dz/mo=自有输入送达; mv_cvt=运动解码; cl=PQ解码; ol=ffxDispatch; enc=PQ编码)");
                    }
                    if (il2cpp_callsite::active() && call_params.context != 0)
                    {
                        // 2026-08-26：游戏 FSR2 上下文结构体转储（零补丁，SEH 保护）。
                        // 前 32 个 float 实测全零（相机常量在更深偏移）→ 转储 128 个 float（512B）；
                        // 只记录一次（首捕），避免每帧刷屏。
                        static std::atomic_bool ctx_logged { false };
                        if (!ctx_logged.exchange(true, std::memory_order_relaxed))
                        {
                            float ctx_f[128] {};
                            const std::size_t ctx_n =
                                read_game_floats_seh(call_params.context, ctx_f, 128);
                            std::string ctx_line = "ffx12_context ptr=0x" +
                                hex64(call_params.context) + " n=" + std::to_string(ctx_n);
                            for (std::size_t ci = 0; ci < ctx_n; ++ci)
                                ctx_line += " " + std::to_string(ctx_f[ci]);
                            log_line(ctx_line);
                        }
                    }
                    if (dcount <= 8 || dcount % 1024 == 0)
                    {
                        std::uint32_t f_color = 0, f_depth = 0, f_motion = 0, f_output = 0;
                        ffx12::input_formats(f_color, f_depth, f_motion, f_output);
                        bool dbg_reset = false;
                        std::uint64_t dbg_recreates = 0;
                        ffx12::debug_state(dbg_reset, dbg_recreates);
                        log_line("ffx12_dispatch count=" + std::to_string(dcount) +
                            " inst=" + hex64(call_params.instance & 0xFFFFFFFFull) +
                            " render=" + std::to_string(sdk_in.render_w) + "x" +
                            std::to_string(sdk_in.render_h) +
                            " output=" + std::to_string(sdk_in.display_w) + "x" +
                            std::to_string(sdk_in.display_h) +
                            " jitter_px=" + std::to_string(sdk_in.jitter_x) + "," +
                            std::to_string(sdk_in.jitter_y) +
                            " jm=" + std::to_string(g_config.ffx12_jitter_mode) +
                            " jdelay=" + std::to_string(g_config.ffx12_jitter_delay ? 1 : 0) +
                            " jit_src=" + jit_src_name +
                            " frame=" + std::to_string(call_params.frame_index) +
                            " mvscale=" + std::to_string(sdk_in.motion_scale_x) + "," +
                            std::to_string(sdk_in.motion_scale_y) +
                            " mvfactor=" + std::to_string(g_config.ffx12_motion_scale) +
                            " slots=" + std::to_string(color_slot) + "/" +
                            std::to_string(depth_slot) + "/" + std::to_string(motion_slot) +
                            "/" + std::to_string(output_slot) +
                            " fmt=" + std::to_string(f_color) + "/" + std::to_string(f_depth) +
                            "/" + std::to_string(f_motion) + "/" + std::to_string(f_output) +
                            " reset=" + std::to_string(dbg_reset ? 1 : 0) +
                            " recreates=" + std::to_string(dbg_recreates) +
                            " depth_inv=" + std::to_string(g_config.ffx12_depth_inverted ? 1 : 0) +
                            " mv_decode=" + std::to_string(g_config.ffx12_decode_motion ? 1 : 0) +
                            " reactive=" + std::to_string(sdk_in.use_reactive_mask ? 1 : 0) +
                            " transparency=" + std::to_string(sdk_in.use_transparency_mask ? 1 : 0) +
                            " hdr=" + std::to_string(g_config.ffx12_hdr_input ? 1 : 0) +
                            " autoexp=" + std::to_string(g_config.ffx12_auto_exposure ? 1 : 0) +
                            " nonlin=" + std::to_string(g_config.ffx12_non_linear ? 1 : 0) +
                            " vf=" + std::to_string(g_config.ffx12_velocity_factor) +
                            " cam=" + std::to_string(g_config.ffx12_camera_near) + "," +
                            std::to_string(g_config.ffx12_camera_far) + "," +
                            std::to_string(sdk_in.camera_fov_vertical) +
                            " dt_ms=" + std::to_string(sdk_in.frame_time_delta_ms) +
                            " version=" + ffx12::selected_version_name());
                        if (g_config.ffx12_camera_hook)
                        {
                            float camera_values[64] {};
                            const std::size_t camera_count =
                                il2cpp_callsite::camera_floats(camera_values, std::size(camera_values));
                            std::string camera_line = "ffx12_camera_capture count=" +
                                std::to_string(dcount) + " frame=" +
                                std::to_string(call_params.frame_index) + " generation=" +
                                std::to_string(il2cpp_callsite::camera_generation()) + " n=" +
                                std::to_string(camera_count);
                            for (std::size_t i = 0; i < camera_count; ++i)
                                camera_line += " " + std::to_string(camera_values[i]);
                            log_line(camera_line);
                        }
                        if (g_config.ffx12_projection_hook)
                        {
                            float projection[16] {};
                            std::uint64_t projection_camera = 0;
                            const std::size_t projection_count = il2cpp_callsite::projection_matrix(
                                projection, std::size(projection), &projection_camera);
                            std::string projection_line = "ffx12_projection_capture count=" +
                                std::to_string(dcount) + " frame=" +
                                std::to_string(call_params.frame_index) + " generation=" +
                                std::to_string(il2cpp_callsite::projection_generation()) + " camera=" +
                                hex64(projection_camera) + " n=" + std::to_string(projection_count);
                            for (std::size_t i = 0; i < projection_count; ++i)
                                projection_line += " " + std::to_string(projection[i]);
                            log_line(projection_line);
                        }
                    }
                    fsr2_family_takeover::notify_accumulate_result(true, GetTickCount64());
                    if (st->last_sdk_output != nullptr)
                        st->last_sdk_output->Release();
                    st->last_sdk_output = output_tex;
                    st->last_sdk_output->AddRef();
                    st->last_sdk_output_gen = call_gen;
                    // 记录 sdk234 写过的输出（GetResource 原始指针，防覆盖：同输出后续 draw 跳过）
                    g_sdk234_output_ptr = reinterpret_cast<std::uint64_t>(sdk234_out_res);
                    g_sdk234_output_tick.store(GetTickCount64(), std::memory_order_relaxed);
                    // 输出关联（draw→实例 确定性匹配）：双缓冲交替输出都记录（含各自派发时间）
                    if (st->out_a == 0 || st->out_a == draw_out_ptr)
                    {
                        st->out_a = draw_out_ptr;
                        st->out_a_tick = GetTickCount64();
                    }
                    else if (st->out_b == 0 || st->out_b == draw_out_ptr)
                    {
                        st->out_b = draw_out_ptr;
                        st->out_b_tick = GetTickCount64();
                    }
                    else
                    {
                        st->out_b = st->out_a;
                        st->out_b_tick = st->out_a_tick;
                        st->out_a = draw_out_ptr;
                        st->out_a_tick = GetTickCount64();
                    }
                    color_tex->Release();
                    depth_tex->Release();
                    motion_tex->Release();
                    if (transparency_tex) transparency_tex->Release();
                    output_tex->Release();
                    return true;
                } // if (ffx12::dispatch(...))
                else
                {
                    // sdk234 dispatch 失败：立即转储失败痕迹（P0-3：rc/阶段记在 sdk_msgs，
                    // 若不在此转储则被吞——旧版仅在后续成功 %1024 派发时才会读 sdk_msgs）。
                    static std::atomic_uint64_t sdk234_fail_count { 0 };
                    const std::uint64_t fc = sdk234_fail_count.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (fc == 1)
                        log_focus_block("FAILED"); // 首次失败也输出焦点框（原因由 ffx12_failed 行补充）
                    if (fc <= 8 || fc % 256 == 0)
                    {
                        std::wstring msgs;
                        ffx12::get_sdk_messages(msgs);
                        log_line("ffx12_failed count=" + std::to_string(fc) +
                            " inst=" + hex64(call_params.instance & 0xFFFFFFFFull) +
                            " gen=" + std::to_string(call_gen) +
                            " frame=" + std::to_string(call_params.frame_index) +
                            " render=" + std::to_string(call_params.render_w) + "x" +
                            std::to_string(call_params.render_h) +
                            " ffx_rc=" + std::to_string(ffx12::last_ffx_dispatch_return_code()) +
                            " sdk_msgs=" + (msgs.empty() ? std::string("none") : narrow(msgs)));
                    }
                    invalidate_direct_attempt("DISPATCH_FAILED");
                    // 旧翻译层已停用——sdk234 未接管时直接放行
                    // （新架构仅 ffx12 一条链路；OptiScaler 后续以 ffx12 为基底重新接入）。
                    // Ffx12FailClosed=1 时禁止回退原生（跳过原始 draw，
                    // 画面显式异常暴露故障，用于 SDK 缺失/路由错误等测试）。
                    if (g_config.ffx12_fail_closed)
                        return true;
                    return false;
                }
                }
            }
            else
            {
                // 纹理获取失败诊断：区分"实例未锁"与"绑定采集不到纹理"（后者 sdk234 无法接管）
                static std::atomic_uint64_t sdk234_null_count { 0 };
                const std::uint64_t nc = sdk234_null_count.fetch_add(1, std::memory_order_relaxed) + 1;
                if (nc <= 4 || nc % 256 == 0)
                {
                        log_line("ffx12_textures_null count=" + std::to_string(nc) +
                            " inst=" + hex64(call_params.instance & 0xFFFFFFFFull) +
                            " gen=" + std::to_string(call_gen) +
                        " color=" + std::to_string(color_tex ? 1 : 0) +
                        " depth=" + std::to_string(depth_tex ? 1 : 0) +
                        " motion=" + std::to_string(motion_tex ? 1 : 0) +
                        " output=" + std::to_string(output_tex ? 1 : 0) +
                            " frame=" + std::to_string(call_params.frame_index));
                }
                invalidate_direct_attempt("RESOURCES_INVALID");
            }
            if (color_tex) color_tex->Release();
            if (depth_tex) depth_tex->Release();
            if (motion_tex) motion_tex->Release();
            if (transparency_tex) transparency_tex->Release();
            if (output_tex) output_tex->Release();
            // 失败由 invalidate_direct_attempt 终结旧 token；函数末的 direct-only gate
            // 继续截断原生 accumulate draw，等待下一枚有效 token 以 reset 重建历史。
        }
        // 诊断：sdk234 未接管原因（低频）。双入口第二次进入（同代次、刚派发过）不算异常，过滤。
        {
            static std::atomic_uint64_t sdk234_skip_count { 0 };
            static std::atomic_uint64_t sdk234_skip_next { 1 };
            const bool second_entry_like =
                st != nullptr && st->last_dispatch_tick != 0 &&
                sdk234_now - st->last_dispatch_tick <= 50;
            if (!second_entry_like)
            {
                const std::uint64_t sc = sdk234_skip_count.fetch_add(1, std::memory_order_relaxed) + 1;
                if (sc == 1 || sc == sdk234_skip_next.load(std::memory_order_relaxed))
                {
                    sdk234_skip_next.store(sc * 8 + 1, std::memory_order_relaxed);
                    const std::string reason = match_inst == 0 ? "NO_TOKEN" : "GATE_STALE";
                    const std::uint64_t ago_ms =
                        st != nullptr && st->last_dispatch_tick != 0
                            ? sdk234_now - st->last_dispatch_tick : 0;
                    // 已知实例代次/帧状态：判断"实例 Render 未被捕获"（:g 远离 glob）还是
                    // "代次在推进但 bridge 门控异常"（:g 接近 glob 却仍 gate_stale）。
                    std::string insts_state;
                    {
                        std::uint64_t insts[8] {};
                        std::size_t inst_n = 0;
                        il2cpp_callsite::known_instances(insts, 8, inst_n);
                        for (std::size_t i = 0; i < inst_n; ++i)
                        {
                            il2cpp_callsite::CapturedParams ip {};
                            std::uint64_t ig = 0;
                            il2cpp_callsite::last_params_for(insts[i], ip, ig);
                            if (i > 0)
                                insts_state += ",";
                            insts_state += hex64(insts[i] & 0xFFFFFFFFull) +
                                ":g" + std::to_string(ig) +
                                ":f" + std::to_string(ip.frame_index);
                        }
                    }
                    log_line("ffx12_skip count=" + std::to_string(sc) +
                        " rc=" + reason +
                        " inst=" + hex64(match_inst & 0xFFFFFFFFull) +
                        " out=" + hex64(draw_out_ptr) +
                        " sdk234_out=" + hex64(g_sdk234_output_ptr) +
                        " ago_ms=" + std::to_string(ago_ms) +
                        " render=" + std::to_string(draw_info->render_width) + "x" +
                        std::to_string(draw_info->render_height) +
                        " glob=" + std::to_string(il2cpp_callsite::params_generation()) +
                        " insts=" + insts_state);
                }
            }
        }
    }

    // Once a target FSR2 accumulate draw is identified, keep the native path
    // blocked.  Pending Render tokens above make the update decision from the
    // actual Render event rather than a stale output association.
    if (g_config.ffx12 && il2cpp_callsite::active())
        return true;

    // 在获取任何 COM 资源之前先取 jitter：不可用时本帧直接回退原生 TAAU
    // （原生路径的 jitter 天然正确），等待 cb0 快照链在后续帧自愈。
    const std::optional<std::pair<float, float>> jitter_pixels = target_jitter_pixels(*draw_info);
    if (!jitter_pixels)
    {
        static std::atomic_uint64_t jitter_unavailable_count { 0 };
        const std::uint64_t miss_count =
            jitter_unavailable_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (miss_count <= 8 || miss_count % 1024 == 0)
        {
            log_line("fsr2_jitter_unavailable count=" + std::to_string(miss_count) +
                " cb0=" + hex64(draw_info->constant_buffer_key) + " fallback=original_draw");
        }
        return false;
    }

    Fsr2FreshProducerPath producer_path;
    std::uint64_t producer_generation = 0;
    if (translation_mode >= 3)
    {
        producer_path = consume_fsr2_fresh_producer_path(draw_info->color_resource_key);
        if (!producer_path.has_fresh_write)
        {
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
            const std::uint64_t fallback_count =
                g_fsr2_stale_producer_fallback_count.fetch_add(1, std::memory_order_relaxed) + 1;
            if (fallback_count == 1 || fallback_count % 1024 == 0)
            {
                log_line("fsr2_translation_fallback no_fresh_producer count=" +
                    std::to_string(fallback_count) + " target=" +
                    hex64(draw_info->color_resource_key));
            }
            record_fsr2_transient_capture_result(
                false, false, false, false, 0, "no_fresh_producer");
#endif
            return false;
        }
        producer_generation = producer_path.linear_generation;
    }
    const bool use_late_composed_color =
        translation_mode >= 3 && !producer_path.has_fresh_linear_color;
    const bool color_path_changed = translation_mode >= 3 &&
        update_fsr2_late_path_state(draw_info->color_resource_key, use_late_composed_color);
    if (color_path_changed)
    {
        const std::uint64_t switch_count =
            g_fsr2_color_path_switch_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (switch_count <= 8 || switch_count % 256 == 0)
        {
            log_line("fsr2_color_path_switch count=" + std::to_string(switch_count) +
                " path=" + (use_late_composed_color ? std::string("late") : std::string("early")) +
                " reset=" +
                    (g_config.fsr2_reset_on_color_path_change ? std::string("1") : std::string("0")));
        }
    }
    if (use_late_composed_color)
    {
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
        const std::uint64_t late_dispatch_count =
            g_fsr2_late_composed_dispatch_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (late_dispatch_count == 1 || late_dispatch_count % 1024 == 0)
        {
            log_line("fsr2_translation_path late_composed count=" +
                std::to_string(late_dispatch_count) + " target=" +
                hex64(draw_info->color_resource_key));
        }
#endif
    }

    std::array<ID3D11ShaderResourceView *, 7> views {};
    std::array<ID3D11RenderTargetView *, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> render_targets {};
    ID3D11DepthStencilView *depth_stencil = nullptr;
    ID3D11Resource *output = nullptr;
    context->PSGetShaderResources(0, static_cast<UINT>(views.size()), views.data());
    context->OMGetRenderTargets(
        static_cast<UINT>(render_targets.size()),
        render_targets.data(),
        &depth_stencil);
    static std::atomic_uint64_t last_depth_state { 0 };
    ResourceInfo depth_info {};
    const bool depth_resource_valid =
        views[2] != nullptr && read_resource_info(views[2], L"fsr2_depth_guard", depth_info);
    // Fsr2TranslationLayer describes the depth SRV as R32_FLOAT.  Do not
    // reinterpret D24/D16 or colour resources as R32: cinematic passes can
    // temporarily bind a different depth target and otherwise produce a
    // water-like depth/history distortion.
    // Genshin's real depth buffer is a D32F_S8-style resource
    // (R32G8X24_TYPELESS) read through its float plane
    // (R32_FLOAT_X8X24_TYPELESS).  That float plane holds genuine R32 depth
    // data, not a reinterpreted D24/D16/colour buffer, so it must be accepted
    // or the guard rejects every frame and super-resolution never activates.
    const bool depth_format_compatible = depth_resource_valid &&
        (depth_info.format == DXGI_FORMAT_R32_FLOAT ||
            depth_info.view_format == DXGI_FORMAT_R32_FLOAT ||
            (depth_info.format == DXGI_FORMAT_R32_TYPELESS &&
                depth_info.view_format == DXGI_FORMAT_R32_FLOAT) ||
            (depth_info.format == DXGI_FORMAT_R32G8X24_TYPELESS &&
                depth_info.view_format == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS));
    // Compute the depth path change up front.  A depth change is a legitimate
    // scene/recreation signal: the motion-mask lock below is re-established on
    // this signal, and the FSR2 history is reset later exactly as before.
    const std::uint64_t depth_state =
        depth_resource_valid
            ? depth_info.resource_key ^ (static_cast<std::uint64_t>(depth_info.format) << 32) ^
                (static_cast<std::uint64_t>(depth_info.width) << 16) ^ depth_info.height
            : 0;
    const std::uint64_t previous_depth_state =
        depth_format_compatible
            ? last_depth_state.exchange(depth_state, std::memory_order_relaxed)
            : last_depth_state.load(std::memory_order_relaxed);
    const bool depth_path_changed =
        depth_format_compatible && previous_depth_state != 0 && previous_depth_state != depth_state;

    // Motion-mask lock.  During normal rendering views[3] is the real motion
    // buffer (whose .z channel is used as the FSR2 reactive mask).  The
    // scripted in-game event rebinds views[3] to a different local-mask-like
    // resource while the real depth stays unchanged.  Lock the stable main
    // motion key and fall back to native TAAU while a different resource is
    // bound, so the mask never reaches FSR2.  A depth change re-establishes
    // the lock, and a color/motion role swap after the event resumes FSR2
    // immediately.
    static std::atomic_uint64_t main_motion_key { 0 };
    static std::atomic_uint32_t main_motion_stable_frames { 0 };
    static std::atomic_uint64_t main_color_key { 0 };
    static std::atomic_uint64_t alt_motion_key { 0 };
    static std::atomic_uint64_t alt_motion_since_tick { 0 };
    static std::atomic_uint64_t motion_mask_skip_logged_key { 0 };
    static std::atomic_uint32_t motion_mask_skip_active { 0 };
    constexpr std::uint32_t k_motion_lock_frames = 30;
    constexpr ULONGLONG k_motion_relock_ms = 5000; // UI/事件切换后视图恢复的 relock 安全网（原 60s 过长——UI 关闭后场景 motion 重建为新资源时持续 fallback 60s）
    ResourceInfo motion_guard_info {};
    ResourceInfo color_guard_info {};
    const bool motion_guard_valid =
        views[3] != nullptr && read_resource_info(views[3], L"fsr2_motion_mask_guard", motion_guard_info);
    const bool color_guard_valid =
        views[0] != nullptr && read_resource_info(views[0], L"fsr2_motion_mask_guard", color_guard_info);
    const std::uint64_t current_motion_key = motion_guard_valid ? motion_guard_info.resource_key : 0;
    const std::uint64_t current_color_key = color_guard_valid ? color_guard_info.resource_key : 0;
    bool motion_mask_skip = false;
    bool motion_mask_resume = false;
    if (depth_format_compatible && motion_guard_valid && color_guard_valid)
    {
        if (depth_path_changed)
        {
            // Depth changed: legitimate scene/recreation.  Re-establish the
            // motion baseline from scratch.
            main_motion_key.store(0, std::memory_order_relaxed);
            main_motion_stable_frames.store(0, std::memory_order_relaxed);
            main_color_key.store(0, std::memory_order_relaxed);
            alt_motion_key.store(0, std::memory_order_relaxed);
            alt_motion_since_tick.store(0, std::memory_order_relaxed);
            motion_mask_skip_logged_key.store(0, std::memory_order_relaxed);
        }
        const std::uint64_t locked_motion_key = main_motion_key.load(std::memory_order_relaxed);
        const std::uint64_t locked_color_key = main_color_key.load(std::memory_order_relaxed);
        if (locked_motion_key == 0 ||
            main_motion_stable_frames.load(std::memory_order_relaxed) < k_motion_lock_frames)
        {
            // Still establishing the main motion key.
            if (current_motion_key == locked_motion_key)
                main_motion_stable_frames.fetch_add(1, std::memory_order_relaxed);
            else
            {
                main_motion_key.store(current_motion_key, std::memory_order_relaxed);
                main_motion_stable_frames.store(1, std::memory_order_relaxed);
            }
            main_color_key.store(current_color_key, std::memory_order_relaxed);
            alt_motion_key.store(0, std::memory_order_relaxed);
            alt_motion_since_tick.store(0, std::memory_order_relaxed);
        }
        else if (current_motion_key == locked_motion_key)
        {
            // Normal rendering: track the current color key as the stable
            // color baseline for role-swap detection.
            main_color_key.store(current_color_key, std::memory_order_relaxed);
            alt_motion_key.store(0, std::memory_order_relaxed);
            alt_motion_since_tick.store(0, std::memory_order_relaxed);
            motion_mask_skip_logged_key.store(0, std::memory_order_relaxed);
        }
        else if (current_motion_key == locked_color_key && locked_color_key != 0)
        {
            // The old color and motion resources swapped roles after the
            // scripted event (observed: old motion becomes color, old color
            // becomes motion).  Accept the new stable assignment.
            main_motion_key.store(current_motion_key, std::memory_order_relaxed);
            main_motion_stable_frames.store(k_motion_lock_frames, std::memory_order_relaxed);
            main_color_key.store(current_color_key, std::memory_order_relaxed);
            alt_motion_key.store(0, std::memory_order_relaxed);
            alt_motion_since_tick.store(0, std::memory_order_relaxed);
            motion_mask_skip_logged_key.store(0, std::memory_order_relaxed);
            log_line("fsr2_motion_mask_guard role_swap new_motion=" + hex64(current_motion_key) +
                " new_color=" + hex64(current_color_key) + " resume=1");
        }
        else
        {
            // Transient motion-like mask bound while the main depth/motion is
            // locked.  Fall back to native TAAU for this frame.
            motion_mask_skip = true;
            const ULONGLONG now_tick = GetTickCount64();
            const std::uint64_t previous_alt =
                alt_motion_key.exchange(current_motion_key, std::memory_order_relaxed);
            if (previous_alt != current_motion_key)
                alt_motion_since_tick.store(now_tick, std::memory_order_relaxed);
            const ULONGLONG alt_since = alt_motion_since_tick.load(std::memory_order_relaxed);
            if (alt_since != 0 && now_tick - alt_since >= k_motion_relock_ms)
            {
                // Safety net: a motion resource that persists this long without
                // any depth change is probably a legitimate recreation rather
                // than a scripted mask.  Re-lock instead of disabling FSR2
                // forever.
                main_motion_key.store(current_motion_key, std::memory_order_relaxed);
                main_motion_stable_frames.store(k_motion_lock_frames, std::memory_order_relaxed);
                main_color_key.store(current_color_key, std::memory_order_relaxed);
                alt_motion_key.store(0, std::memory_order_relaxed);
                alt_motion_since_tick.store(0, std::memory_order_relaxed);
                motion_mask_skip = false;
                log_line("fsr2_motion_mask_guard relock new_motion=" + hex64(current_motion_key) +
                    " new_color=" + hex64(current_color_key) + " reason=stable_timeout");
            }
            else if (motion_mask_skip_logged_key.exchange(current_motion_key, std::memory_order_relaxed) !=
                current_motion_key)
            {
                log_line("fsr2_motion_mask_guard skip mask=" + hex64(current_motion_key) +
                    " main=" + hex64(locked_motion_key) +
                    " main_color=" + hex64(locked_color_key) +
                    " fallback=original_draw");
            }
        }
        const bool skip_active = motion_mask_skip_active.exchange(
            motion_mask_skip ? 1u : 0u, std::memory_order_relaxed) != 0;
        motion_mask_resume = skip_active && !motion_mask_skip;
    }
    if (views[0] == nullptr || !depth_format_compatible || motion_mask_skip || views[3] == nullptr ||
        (g_config.fsr2_use_reactive_mask && views[4] == nullptr) || render_targets[1] == nullptr)
    {
        if (!depth_format_compatible)
            last_depth_state.store(UINT64_MAX, std::memory_order_relaxed);
        if (views[2] != nullptr && !depth_format_compatible)
        {
            static std::atomic_uint32_t last_rejected_depth_format { UINT_MAX };
            const std::uint32_t rejected_format = static_cast<std::uint32_t>(depth_info.view_format != DXGI_FORMAT_UNKNOWN
                ? depth_info.view_format : depth_info.format);
            if (last_rejected_depth_format.exchange(rejected_format, std::memory_order_relaxed) != rejected_format)
            {
                log_line("fsr2_depth_guard rejected resource_format=" +
                    describe_dxgi_format(depth_info.format) +
                    " view_format=" + describe_dxgi_format(depth_info.view_format) +
                    " reason=not_r32_float");
            }
        }
        for (ID3D11ShaderResourceView *view : views)
        {
            if (view != nullptr)
                view->Release();
        }
        for (ID3D11RenderTargetView *render_target : render_targets)
        {
            if (render_target != nullptr)
                render_target->Release();
        }
        if (depth_stencil != nullptr)
            depth_stencil->Release();
        return false;
    }
    if (depth_path_changed)
    {
        log_line("fsr2_depth_guard resource_changed previous=" + hex64(previous_depth_state) +
            " current=" + hex64(depth_state) + " reset=1");
    }
    render_targets[1]->GetResource(&output);

    // ---- sdk234 精确防覆盖 ----
    // 本 draw 的输出与 sdk234 刚写过的输出相同（同一 r1）时，转译层/原生会覆盖 sdk234 的结果。
    // 只跳过"同输出"的 draw（精确判断，不黑屏）；不同输出的 draw 正常放行。
    {
        static std::atomic_uint64_t sdk234_cover_count { 0 };
        const std::uint64_t sdk234_out_tick = g_sdk234_output_tick.load(std::memory_order_relaxed);
        const std::uint64_t sdk234_out_age_ms =
            sdk234_out_tick != 0 ? GetTickCount64() - sdk234_out_tick : UINT64_MAX;
        // 过期保护：仅当 sdk234 在 300ms 内刚写过该输出才跳过（同帧双入口拦截）；
        // 超时 = sdk234 已停止派发该输出（视图切走/匹配失败/代次停滞）→ 放行原生/OptiScaler，
        // 避免该视图永久冻结（2026-08-25 日志实证 cover_skip 6144 次后输出不再更新）。
        if (g_config.ffx12 && il2cpp_callsite::active() && g_sdk234_output_ptr != 0 &&
            sdk234_out_age_ms < 300 &&
            output != nullptr && reinterpret_cast<std::uint64_t>(output) == g_sdk234_output_ptr)
        {
            const std::uint64_t cc = sdk234_cover_count.fetch_add(1, std::memory_order_relaxed) + 1;
            if (cc <= 4 || cc % 1024 == 0)
            {
                log_line("ffx12_cover_skip output=" + hex64(reinterpret_cast<std::uint64_t>(output)) +
                    " count=" + std::to_string(cc) +
                    " age_ms=" + std::to_string(sdk234_out_age_ms));
            }
            for (ID3D11ShaderResourceView *view : views)
            {
                if (view != nullptr)
                    view->Release();
            }
            for (ID3D11RenderTargetView *render_target : render_targets)
            {
                if (render_target != nullptr)
                    render_target->Release();
            }
            if (depth_stencil != nullptr)
                depth_stencil->Release();
            output->Release();
            return true;
        }
    }

    // 旧翻译层执行段停用（旧 OptiScaler+Bridge 方案隔离，文本保留供参考）。

    return false; // ：旧翻译层停用后直接放行（新架构仅 ffx12 链路）
}
#endif

bool compile_spatial_copy_bytecode_locked()
{
    if (g_spatial_copy_compile_attempted)
        return !g_spatial_copy_bytecode.empty();

    g_spatial_copy_compile_attempted = true;
    static constexpr char source[] = R"(
cbuffer ExistingConstants : register(b0)
{
    float4 constants[31];
};

Texture2D<float4> CurrentColor : register(t0);
SamplerState LinearSampler : register(s0);

struct SpatialOutput
{
    float2 metadata : SV_Target0;
    float4 color : SV_Target1;
};

SpatialOutput main(float4 position : SV_Position)
{
    SpatialOutput output;
    const float2 outputSize = constants[27].xy;
    const float2 uv = position.xy / outputSize;
    output.metadata = float2(0.0, 0.0);
    output.color = CurrentColor.SampleLevel(LinearSampler, uv, 0.0);
    return output;
}
)";

    ID3DBlob *bytecode = nullptr;
    ID3DBlob *errors = nullptr;
    const HRESULT hr = D3DCompile(
        source,
        sizeof(source) - 1,
        "Dx11FsrBridgeSpatialCopy",
        nullptr,
        nullptr,
        "main",
        "ps_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &bytecode,
        &errors);

    if (FAILED(hr) || bytecode == nullptr)
    {
        std::string message = "pixel_shader_replacement_compile_failed hr=" + std::to_string(static_cast<long>(hr));
        if (errors != nullptr && errors->GetBufferPointer() != nullptr)
            message += " error=" + std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize());
        log_line(message);
        if (errors != nullptr)
            errors->Release();
        if (bytecode != nullptr)
            bytecode->Release();
        return false;
    }

    const auto *begin = static_cast<const std::uint8_t *>(bytecode->GetBufferPointer());
    g_spatial_copy_bytecode.assign(begin, begin + bytecode->GetBufferSize());
    if (errors != nullptr)
        errors->Release();
    bytecode->Release();
    return true;
}

ID3D11PixelShader *acquire_spatial_copy_shader(ID3D11DeviceContext *context)
{
    ID3D11Device *device = nullptr;
    context->GetDevice(&device);
    if (device == nullptr)
        return nullptr;

    std::lock_guard lock(g_replacement_mutex);
    if (g_replacement_device != device)
    {
        if (g_spatial_copy_shader != nullptr)
        {
            g_spatial_copy_shader->Release();
            g_spatial_copy_shader = nullptr;
        }
        if (g_replacement_device != nullptr)
            g_replacement_device->Release();
        g_replacement_device = device;
        g_spatial_copy_create_failed = false;
    }
    else
    {
        device->Release();
    }

    if (g_spatial_copy_shader == nullptr && !g_spatial_copy_create_failed)
    {
        if (!compile_spatial_copy_bytecode_locked())
            return nullptr;

        const HRESULT hr = g_original_create_pixel_shader != nullptr
            ? g_original_create_pixel_shader(
                g_replacement_device,
                g_spatial_copy_bytecode.data(),
                g_spatial_copy_bytecode.size(),
                nullptr,
                &g_spatial_copy_shader)
            : g_replacement_device->CreatePixelShader(
                g_spatial_copy_bytecode.data(),
                g_spatial_copy_bytecode.size(),
                nullptr,
                &g_spatial_copy_shader);
        if (FAILED(hr) || g_spatial_copy_shader == nullptr)
        {
            g_spatial_copy_create_failed = true;
            log_line("pixel_shader_replacement_create_failed hr=" + std::to_string(static_cast<long>(hr)));
            return nullptr;
        }
        log_line("pixel_shader_replacement_ready mode=spatial_copy target=" + hex64(g_config.target_pixel_shader_hash));
    }

    if (g_spatial_copy_shader != nullptr)
        g_spatial_copy_shader->AddRef();
    return g_spatial_copy_shader;
}

struct PixelShaderRestoreState
{
    ID3D11PixelShader *shader = nullptr;
    std::array<ID3D11ClassInstance *, 256> class_instances {};
    UINT class_instance_count = 0;
};

bool begin_spatial_copy_draw(ID3D11DeviceContext *context, PixelShaderRestoreState &restore_state)
{
    ID3D11PixelShader *replacement = acquire_spatial_copy_shader(context);
    if (replacement == nullptr)
        return false;

    restore_state.class_instance_count = static_cast<UINT>(restore_state.class_instances.size());
    context->PSGetShader(
        &restore_state.shader,
        restore_state.class_instances.data(),
        &restore_state.class_instance_count);
    if (restore_state.shader == nullptr)
    {
        for (UINT i = 0; i < restore_state.class_instance_count; ++i)
        {
            if (restore_state.class_instances[i] != nullptr)
                restore_state.class_instances[i]->Release();
        }
        replacement->Release();
        return false;
    }

    g_original_ps_set_shader(context, replacement, nullptr, 0);
    replacement->Release();
    if (g_replacement_draw_count.fetch_add(1, std::memory_order_relaxed) == 0)
        log_line("pixel_shader_replacement_active mode=spatial_copy");
    return true;
}

void end_spatial_copy_draw(ID3D11DeviceContext *context, PixelShaderRestoreState &restore_state)
{
    g_original_ps_set_shader(
        context,
        restore_state.shader,
        restore_state.class_instances.data(),
        restore_state.class_instance_count);
    restore_state.shader->Release();
    for (UINT i = 0; i < restore_state.class_instance_count; ++i)
    {
        if (restore_state.class_instances[i] != nullptr)
            restore_state.class_instances[i]->Release();
    }
}

template <typename DrawCall>
bool try_hdr_sdr_tone_map_draw(ID3D11DeviceContext *context, UINT element_count, DrawCall &&draw_call)
{
    if (!is_hdr_sdr_tone_map_composite_draw(element_count))
        return false;

    ID3D11RenderTargetView *original_target = nullptr;
    context->OMGetRenderTargets(1, &original_target, nullptr);
    if (original_target == nullptr)
        return false;
    ID3D11Resource *target_resource = nullptr;
    ID3D11Texture2D *target_texture = nullptr;
    original_target->GetResource(&target_resource);
    if (target_resource != nullptr)
        target_resource->QueryInterface(IID_PPV_ARGS(&target_texture));
    if (target_resource != nullptr)
        target_resource->Release();
    if (target_texture == nullptr)
    {
        original_target->Release();
        return false;
    }

    ID3D11Device *device = nullptr;
    context->GetDevice(&device);
    std::lock_guard tone_map_lock(g_hdr_sdr_tone_map_mutex);
    if (device == nullptr || !ensure_hdr_sdr_tone_map_resources_locked(device, target_texture, original_target))
    {
        if (device != nullptr)
            device->Release();
        target_texture->Release();
        original_target->Release();
        return false;
    }
    device->Release();
    auto &resources = g_hdr_sdr_tone_map_resources;

    ID3D11RenderTargetView *saved_rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] {};
    ID3D11DepthStencilView *saved_dsv = nullptr;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, saved_rtvs, &saved_dsv);
    D3D11_VIEWPORT saved_viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] {};
    UINT saved_viewport_count = static_cast<UINT>(std::size(saved_viewports));
    context->RSGetViewports(&saved_viewport_count, saved_viewports);
    ID3D11InputLayout *saved_layout = nullptr;
    context->IAGetInputLayout(&saved_layout);
    D3D11_PRIMITIVE_TOPOLOGY saved_topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    context->IAGetPrimitiveTopology(&saved_topology);
    ID3D11VertexShader *saved_vs = nullptr;
    context->VSGetShader(&saved_vs, nullptr, nullptr);
    ID3D11PixelShader *saved_ps = nullptr;
    context->PSGetShader(&saved_ps, nullptr, nullptr);
    ID3D11ShaderResourceView *saved_srv0 = nullptr;
    context->PSGetShaderResources(0, 1, &saved_srv0);
    ID3D11SamplerState *saved_sampler = nullptr;
    context->PSGetSamplers(0, 1, &saved_sampler);
    ID3D11Buffer *saved_cb0 = nullptr;
    context->PSGetConstantBuffers(0, 1, &saved_cb0);
    ID3D11BlendState *saved_blend = nullptr;
    FLOAT saved_blend_factor[4] {};
    UINT saved_sample_mask = 0;
    context->OMGetBlendState(&saved_blend, saved_blend_factor, &saved_sample_mask);
    ID3D11DepthStencilState *saved_depth = nullptr;
    UINT saved_stencil_ref = 0;
    context->OMGetDepthStencilState(&saved_depth, &saved_stencil_ref);
    ID3D11RasterizerState *saved_rasterizer = nullptr;
    context->RSGetState(&saved_rasterizer);

    const D3D11_VIEWPORT viewport { 0.0f, 0.0f, static_cast<float>(resources.width),
        static_cast<float>(resources.height), 0.0f, 1.0f };
    struct ToneMapConstants
    {
        float paper_white;
        float peak;
        float pq_input;
        float padding;
    } constants {
        static_cast<float>(g_config.hdr_sdr_tone_map_paper_white),
        static_cast<float>(std::max(g_config.hdr_sdr_tone_map_peak, g_config.hdr_sdr_tone_map_paper_white)),
        g_config.hdr_sdr_tone_map_pq_input ? 1.0f : 0.0f,
        0.0f,
    };
    {
        ScopedInternalBridgeDispatch internal_dispatch_scope;
        ScopedContextVtableBypass context_vtable_bypass(context);
        context->OMSetRenderTargets(1, &resources.source_target_view, nullptr);
        context->RSSetViewports(1, &viewport);
        std::forward<DrawCall>(draw_call)();

        context->OMSetRenderTargets(1, &original_target, nullptr);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(resources.vertex_shader, nullptr, 0);
        context->PSSetShader(resources.pixel_shader, nullptr, 0);
        context->PSSetShaderResources(0, 1, &resources.source_view);
        context->PSSetSamplers(0, 1, &resources.sampler);
        context->UpdateSubresource(resources.constants, 0, nullptr, &constants, 0, 0);
        context->PSSetConstantBuffers(0, 1, &resources.constants);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
        context->OMSetDepthStencilState(nullptr, 0);
        context->RSSetState(nullptr);
        context->Draw(3, 0);

        context->PSSetShaderResources(0, 1, &saved_srv0);
        context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, saved_rtvs, saved_dsv);
        if (saved_viewport_count != 0)
            context->RSSetViewports(saved_viewport_count, saved_viewports);
        context->IASetInputLayout(saved_layout);
        context->IASetPrimitiveTopology(saved_topology);
        context->VSSetShader(saved_vs, nullptr, 0);
        context->PSSetShader(saved_ps, nullptr, 0);
        context->PSSetSamplers(0, 1, &saved_sampler);
        context->PSSetConstantBuffers(0, 1, &saved_cb0);
        context->OMSetBlendState(saved_blend, saved_blend_factor, saved_sample_mask);
        context->OMSetDepthStencilState(saved_depth, saved_stencil_ref);
        context->RSSetState(saved_rasterizer);
    }

    for (auto *rtv : saved_rtvs) if (rtv != nullptr) rtv->Release();
    if (saved_dsv != nullptr) saved_dsv->Release();
    if (saved_layout != nullptr) saved_layout->Release();
    if (saved_vs != nullptr) saved_vs->Release();
    if (saved_ps != nullptr) saved_ps->Release();
    if (saved_srv0 != nullptr) saved_srv0->Release();
    if (saved_sampler != nullptr) saved_sampler->Release();
    if (saved_cb0 != nullptr) saved_cb0->Release();
    if (saved_blend != nullptr) saved_blend->Release();
    if (saved_depth != nullptr) saved_depth->Release();
    if (saved_rasterizer != nullptr) saved_rasterizer->Release();
    target_texture->Release();
    original_target->Release();

    static std::atomic_uint64_t applied_count { 0 };
    const auto count = applied_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1 || count % 1024 == 0)
        log_line("hdr_sdr_tone_map_offscreen_applied count=" + std::to_string(count) +
            " paper_white=" + std::to_string(g_config.hdr_sdr_tone_map_paper_white) +
            " peak=" + std::to_string(g_config.hdr_sdr_tone_map_peak) +
            " pq_input=" + std::to_string(g_config.hdr_sdr_tone_map_pq_input ? 1 : 0));
    return true;
}

template <typename DrawCall>
bool try_spatial_copy_draw(ID3D11DeviceContext *context, UINT element_count, DrawCall &&draw_call)
{
    if (g_config.pixel_shader_replacement_mode != 1 || !inspect_target_upscaler_draw(element_count))
        return false;

    PixelShaderRestoreState restore_state;
    if (!begin_spatial_copy_draw(context, restore_state))
        return false;

    std::forward<DrawCall>(draw_call)();
    end_spatial_copy_draw(context, restore_state);
    return true;
}

void STDMETHODCALLTYPE hooked_draw_indexed(ID3D11DeviceContext *context, UINT index_count, UINT start_index_location, INT base_vertex_location)
{
    // passthrough 机制已整体移除（实测让 OptiScaler 丢失 FFX 输入识别）。
    // 所有显卡统一桥直连；OptiScaler 共存时并行（各自独立链路，实测无冲突；
    // N/Intel 上 OptiScaler 用于提供 DLSS/XeSS，不依赖桥让路）。
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    capture_runtime_snapshot_if_requested();
#endif
    static std::atomic_bool hook_logged { false };
    if (!hook_logged.load(std::memory_order_relaxed) &&
        !hook_logged.exchange(true, std::memory_order_relaxed))
        log_line("draw_indexed_hook_active");
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    record_color_source_call("draw_indexed", index_count, 0, 0);
    maybe_dump_target_color_chain(context, index_count);
#endif
#if defined(DX11FSRBRIDGE_FINAL_SCENE_PROBE)
    record_final_scene_probe_draw(index_count, true);
#endif
    record_hdr_composite_candidate(index_count, true);
#if defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    poll_fsr2_transient_capture_hotkey();
#endif
    // Phase 1：FSR2 合成族预处理 pass 跳过（仅当上一帧累积 pass 被桥成功替换且未超时）
    if (g_config.fsr2_family_skip && index_count == 3)
    {
        ID3D11PixelShader *family_ps = nullptr;
        context->PSGetShader(&family_ps, nullptr, nullptr);
        const std::uint64_t family_ps_hash = family_ps ? lookup_pixel_shader_info(family_ps).hash : 0;
        if (family_ps)
            family_ps->Release();
        if (family_ps_hash != 0 &&
            fsr2_family_takeover::should_skip_pre(family_ps_hash, GetTickCount64(), g_config.fsr2_family_expire_ms))
        {
            static std::atomic_uint64_t family_skip_log_count { 0 };
            const std::uint64_t log_count = family_skip_log_count.fetch_add(1, std::memory_order_relaxed) + 1;
            if (log_count == 1 || log_count % 1024 == 0)
                log_line("fsr2_family_skip_draw hash=" + hex64(family_ps_hash) +
                    " total=" + std::to_string(fsr2_family_takeover::skipped_count()));
            return;
        }
    }
    const auto target_draw_info = inspect_target_upscaler_draw(context, index_count);
    if (target_draw_info && g_config.fsr2_translation_mode >= 3)
        observe_fsr2_dynamic_color_target(*target_draw_info);
    maybe_track_fsr2_color_candidate(context, index_count);
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    if (target_draw_info)
        begin_fsr2_transient_capture(context, *target_draw_info);
    maybe_dump_color_candidate_inputs(context, index_count);
    maybe_dump_same_frame_fsr2_inputs(context, index_count);
    // 旧翻译层 probe 停用（旧方案隔离）
#endif
#endif
#if defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
    const bool fsr2_translation_handled = try_fsr2_translation_draw(context, index_count, target_draw_info, [&]
        {
            g_original_draw_indexed(context, index_count, start_index_location, base_vertex_location);
        });
    if (g_config.fsr2_family_skip && target_draw_info)
    {
        static std::atomic_uint64_t family_notify_count { 0 };
        const std::uint64_t notify_seq = family_notify_count.fetch_add(1, std::memory_order_relaxed);
        if (notify_seq < 16)
            log_line("fsr2_family_notify handled=" + std::to_string(fsr2_translation_handled ? 1 : 0) +
                " render=" + std::to_string(target_draw_info->render_width) + "x" +
                std::to_string(target_draw_info->render_height) +
                (il2cpp_callsite::active()
                    ? " il2cpp_render_calls=" + std::to_string(il2cpp_callsite::render_call_count())
                    : std::string()));
        fsr2_family_takeover::notify_accumulate_result(fsr2_translation_handled, GetTickCount64());
    }
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    finish_fsr2_transient_capture_fallback();
#endif
    if (fsr2_translation_handled)
        return;
#endif

    if (try_hdr_sdr_tone_map_draw(context, index_count, [&]
        {
            g_original_draw_indexed(context, index_count, start_index_location, base_vertex_location);
        }))
    {
        return;
    }

#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    if (try_spatial_copy_draw(context, index_count, [&]
        {
            g_original_draw_indexed(context, index_count, start_index_location, base_vertex_location);
        }))
        return;

    if (g_config.enable_similarity_probe ||
        (g_config.trace_pixel_shader_draws && g_current_ps_hash.load(std::memory_order_relaxed) == g_config.trace_pixel_shader_hash))
        record_similarity_draw("draw_indexed", index_count);
#endif
#if defined(DX11FSRBRIDGE_FINAL_SCENE_PROBE)
    bool final_scene_snapshot_boundary = false;
    std::uint64_t final_scene_snapshot_frame = 0;
    bool final_scene_optifg_boundary = false;
    std::uint64_t final_scene_optifg_frame = 0;
    final_scene_snapshot_boundary =
        matches_final_scene_boundary(index_count, true, final_scene_snapshot_frame, true);
    final_scene_optifg_boundary =
        matches_final_scene_boundary(index_count, true, final_scene_optifg_frame, false);
#endif
    g_original_draw_indexed(context, index_count, start_index_location, base_vertex_location);
#if defined(DX11FSRBRIDGE_FINAL_SCENE_PROBE)
    if (final_scene_optifg_boundary)
        submit_final_scene_to_optiscaler(context, final_scene_optifg_frame);
    if (final_scene_snapshot_boundary)
        queue_final_scene_snapshot(context, final_scene_snapshot_frame);
#endif
}

void STDMETHODCALLTYPE hooked_draw(ID3D11DeviceContext *context, UINT vertex_count, UINT start_vertex_location)
{
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    capture_runtime_snapshot_if_requested();
#endif
    static std::atomic_bool hook_logged { false };
    if (!hook_logged.load(std::memory_order_relaxed) &&
        !hook_logged.exchange(true, std::memory_order_relaxed))
        log_line("draw_hook_active");
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    record_color_source_call("draw", vertex_count, 0, 0);
    maybe_dump_target_color_chain(context, vertex_count);
#endif
#if defined(DX11FSRBRIDGE_FINAL_SCENE_PROBE)
    record_final_scene_probe_draw(vertex_count, false);
#endif
    record_hdr_composite_candidate(vertex_count, false);
#if defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    poll_fsr2_transient_capture_hotkey();
#endif
    // Phase 1：FSR2 合成族预处理 pass 跳过（同上）
    if (g_config.fsr2_family_skip && vertex_count == 3)
    {
        ID3D11PixelShader *family_ps = nullptr;
        context->PSGetShader(&family_ps, nullptr, nullptr);
        const std::uint64_t family_ps_hash = family_ps ? lookup_pixel_shader_info(family_ps).hash : 0;
        if (family_ps)
            family_ps->Release();
        if (family_ps_hash != 0 &&
            fsr2_family_takeover::should_skip_pre(family_ps_hash, GetTickCount64(), g_config.fsr2_family_expire_ms))
        {
            static std::atomic_uint64_t family_skip_log_count { 0 };
            const std::uint64_t log_count = family_skip_log_count.fetch_add(1, std::memory_order_relaxed) + 1;
            if (log_count == 1 || log_count % 1024 == 0)
                log_line("fsr2_family_skip_draw hash=" + hex64(family_ps_hash) +
                    " total=" + std::to_string(fsr2_family_takeover::skipped_count()));
            // 一次性 PRE-pass cb0 探测（诊断用，Ffx12Probe=1 时启用）
            if (g_config.ffx12_probe)
            {
                static std::atomic_int pre_cb0_probe_round { 0 };
                static std::atomic_bool pre_cb0_done { false };
                const int pre_round = pre_cb0_probe_round.fetch_add(1, std::memory_order_relaxed) + 1;
                if (!pre_cb0_done.load(std::memory_order_relaxed) && pre_round <= 96)
                {
                ID3D11Buffer *pre_cb = nullptr;
                context->PSGetConstantBuffers(0, 1, &pre_cb);
                if (pre_cb)
                {
                    const std::uint64_t pre_key = reinterpret_cast<std::uint64_t>(pre_cb);
                    g_trace_ps_cb0_key.store(pre_key, std::memory_order_relaxed);
                    const auto pre_it = g_buffer_snapshots.find(pre_key);
                    if (pre_it != g_buffer_snapshots.end() && pre_it->second.size() > 496)
                    {
                        std::string probe = "ffx12_pre_cb0 size=" +
                            std::to_string(pre_it->second.size());
                        const std::size_t floats = pre_it->second.size() / 4;
                        probe += " cb0=";
                        const auto *fb = reinterpret_cast<const float *>(pre_it->second.data());
                        for (std::size_t i = 0; i < floats; ++i)
                        {
                            probe += std::to_string(fb[i]);
                            if (i + 1 < floats)
                                probe += ",";
                        }
                        log_line(probe);
                        pre_cb0_done.store(true, std::memory_order_relaxed);
                    }
                    pre_cb->Release();
                }
                }
            }
            return;
        }
    }
    const auto target_draw_info = inspect_target_upscaler_draw(context, vertex_count);
    if (target_draw_info && g_config.fsr2_translation_mode >= 3)
        observe_fsr2_dynamic_color_target(*target_draw_info);
    maybe_track_fsr2_color_candidate(context, vertex_count);
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    if (target_draw_info)
        begin_fsr2_transient_capture(context, *target_draw_info);
    maybe_dump_color_candidate_inputs(context, vertex_count);
    maybe_dump_same_frame_fsr2_inputs(context, vertex_count);
    // 旧翻译层 probe 停用（旧方案隔离）
#endif
#endif
#if defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
    const bool fsr2_translation_handled = try_fsr2_translation_draw(context, vertex_count, target_draw_info, [&]
        {
            g_original_draw(context, vertex_count, start_vertex_location);
        });
    if (g_config.fsr2_family_skip && target_draw_info)
        fsr2_family_takeover::notify_accumulate_result(fsr2_translation_handled, GetTickCount64());
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    finish_fsr2_transient_capture_fallback();
#endif
    if (fsr2_translation_handled)
        return;
#endif

    if (try_hdr_sdr_tone_map_draw(context, vertex_count, [&]
        {
            g_original_draw(context, vertex_count, start_vertex_location);
        }))
    {
        return;
    }

#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    if (try_spatial_copy_draw(context, vertex_count, [&]
        {
            g_original_draw(context, vertex_count, start_vertex_location);
        }))
        return;

    if (g_config.enable_similarity_probe ||
        (g_config.trace_pixel_shader_draws && g_current_ps_hash.load(std::memory_order_relaxed) == g_config.trace_pixel_shader_hash))
        record_similarity_draw("draw", vertex_count);
#endif
#if defined(DX11FSRBRIDGE_FINAL_SCENE_PROBE)
    bool final_scene_snapshot_boundary = false;
    std::uint64_t final_scene_snapshot_frame = 0;
    bool final_scene_optifg_boundary = false;
    std::uint64_t final_scene_optifg_frame = 0;
    final_scene_snapshot_boundary =
        matches_final_scene_boundary(vertex_count, false, final_scene_snapshot_frame, true);
    final_scene_optifg_boundary =
        matches_final_scene_boundary(vertex_count, false, final_scene_optifg_frame, false);
#endif
    g_original_draw(context, vertex_count, start_vertex_location);
#if defined(DX11FSRBRIDGE_FINAL_SCENE_PROBE)
    if (final_scene_optifg_boundary)
        submit_final_scene_to_optiscaler(context, final_scene_optifg_frame);
    if (final_scene_snapshot_boundary)
        queue_final_scene_snapshot(context, final_scene_snapshot_frame);
#endif
}

HRESULT STDMETHODCALLTYPE hooked_map(ID3D11DeviceContext *context, ID3D11Resource *resource, UINT subresource, D3D11_MAP map_type, UINT map_flags, D3D11_MAPPED_SUBRESOURCE *mapped)
{
    const HRESULT hr = g_original_map(context, resource, subresource, map_type, map_flags, mapped);
    if (SUCCEEDED(hr) && resource != nullptr && mapped != nullptr && mapped->pData != nullptr)
    {
        const auto key = reinterpret_cast<std::uint64_t>(resource);
        // 仅跟踪 trace 目标 cb0（性能：游戏每帧大量 Map/Unmap，全量跟踪导致 3 FPS）
        if (key != g_trace_ps_cb0_key.load(std::memory_order_relaxed))
            return hr;
        std::lock_guard lock(g_buffer_info_mutex);
        const auto it = g_buffer_info.find(key);
        if (it != g_buffer_info.end() && it->second.byte_width != 0)
            g_mapped_buffers[key] = { mapped->pData, it->second.byte_width };
    }
    return hr;
}

void STDMETHODCALLTYPE hooked_unmap(ID3D11DeviceContext *context, ID3D11Resource *resource, UINT subresource)
{
    if (resource != nullptr)
    {
        const auto key = reinterpret_cast<std::uint64_t>(resource);
        if (key != g_trace_ps_cb0_key.load(std::memory_order_relaxed))
        {
            g_original_unmap(context, resource, subresource);
            return;
        }
        std::lock_guard lock(g_buffer_info_mutex);
        const auto mapped_it = g_mapped_buffers.find(key);
        if (mapped_it != g_mapped_buffers.end() && mapped_it->second.data != nullptr && mapped_it->second.size != 0)
        {
            const auto *bytes = static_cast<const std::uint8_t *>(mapped_it->second.data);
            g_buffer_snapshots[key].assign(bytes, bytes + mapped_it->second.size);
            const auto info_it = g_buffer_info.find(key);
            if (info_it != g_buffer_info.end())
            {
                info_it->second.last_update_size = mapped_it->second.size;
                info_it->second.last_update_hash = fnv1a64(bytes, mapped_it->second.size);
            }
            g_mapped_buffers.erase(mapped_it);
        }
    }
    g_original_unmap(context, resource, subresource);
}

void STDMETHODCALLTYPE hooked_rs_set_viewports(ID3D11DeviceContext *context, UINT count, const D3D11_VIEWPORT *viewports)
{
#if defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    if (g_config.fsr2_translation_mode == 2 && g_config.fsr2_mode2_on_demand_state)
    {
        g_original_rs_set_viewports(context, count, viewports);
        return;
    }
    if (g_config.fsr2_fast_state_tracking && g_config.fsr2_translation_mode == 2 &&
        g_mode2_fast_target_ps_hash.load(std::memory_order_relaxed) != 0)
    {
        g_original_rs_set_viewports(context, count, viewports);
        return;
    }
#endif
    {
        std::lock_guard lock(g_state_mutex);
        if (count != 0 && viewports != nullptr)
        {
            g_state.viewport_width = static_cast<std::uint32_t>(viewports[0].Width);
            g_state.viewport_height = static_cast<std::uint32_t>(viewports[0].Height);
        }
        else
        {
            g_state.viewport_width = 0;
            g_state.viewport_height = 0;
        }
    }
    g_original_rs_set_viewports(context, count, viewports);
}

void STDMETHODCALLTYPE hooked_copy_resource(ID3D11DeviceContext *context, ID3D11Resource *dst, ID3D11Resource *src)
{
#if defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    // Release 只为 sdk234 输出链追踪 CopyResource。未命中时不查询资源描述，保持原样透传。
    const std::uint64_t sdk_output_fast = g_sdk234_output_ptr;
    const std::uint64_t sdk_tick_fast = g_sdk234_output_tick.load(std::memory_order_relaxed);
    const bool sdk_copy_candidate = g_config.ffx12 && sdk_output_fast != 0 &&
        sdk_tick_fast != 0 && GetTickCount64() - sdk_tick_fast < 300 &&
        (reinterpret_cast<std::uint64_t>(dst) == sdk_output_fast ||
         reinterpret_cast<std::uint64_t>(src) == sdk_output_fast);
    if (!sdk_copy_candidate)
    {
        g_original_copy_resource(context, dst, src);
        return;
    }
#endif
    ResourceInfo dst_info {};
    ResourceInfo src_info {};
    read_resource_info_from_resource(dst, L"copy_dst", dst_info);
    read_resource_info_from_resource(src, L"copy_src", src_info);
    // 只跟踪 SDK 刚写过的输出参与的复制，判定它是被后续链覆盖（dst）还是
    // 正常转交给显示链（src）。限量记录，避免全量 D3D11 CopyResource 日志扰动时序。
    const std::uint64_t sdk_output = g_sdk234_output_ptr;
    const std::uint64_t sdk_tick = g_sdk234_output_tick.load(std::memory_order_relaxed);
    const std::uint64_t sdk_age_ms = sdk_tick ? GetTickCount64() - sdk_tick : UINT64_MAX;
    if (g_config.ffx12 && sdk_output != 0 && sdk_age_ms < 300 &&
        (reinterpret_cast<std::uint64_t>(dst) == sdk_output ||
         reinterpret_cast<std::uint64_t>(src) == sdk_output))
    {
        static std::atomic_uint32_t sdk234_copy_chain_logs { 0 };
        const std::uint32_t n = sdk234_copy_chain_logs.fetch_add(1, std::memory_order_relaxed);
        if (n < 64)
        {
            const char *role = reinterpret_cast<std::uint64_t>(src) == sdk_output ? "src" : "dst";
            log_line("ffx12_copy_chain role=" + std::string(role) +
                " age_ms=" + std::to_string(sdk_age_ms) +
                " dst=" + hex64(dst_info.resource_key) +
                " src=" + hex64(src_info.resource_key) +
                " dst_dims=" + std::to_string(dst_info.width) + "x" + std::to_string(dst_info.height) +
                " src_dims=" + std::to_string(src_info.width) + "x" + std::to_string(src_info.height));
        }
    }
    if (g_config.log_resource_ops)
        log_line("copy_resource dst=" + hex64(dst_info.resource_key) + " src=" + hex64(src_info.resource_key));
    record_hdr_composite_copy(dst_info, src_info, "copy_resource");
    record_color_source_copy(dst_info, src_info, "copy_resource");
    g_original_copy_resource(context, dst, src);
}

void STDMETHODCALLTYPE hooked_copy_subresource_region(ID3D11DeviceContext *context, ID3D11Resource *dst, UINT dst_subresource, UINT dst_x, UINT dst_y, UINT dst_z, ID3D11Resource *src, UINT src_subresource, const D3D11_BOX *src_box)
{
#if defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    // Release：全部内省消费点默认关闭时直接透传（同 copy_resource 快路径模式）。
    if (!g_config.log_resource_ops && !g_config.hdr_composite_probe && !g_config.fsr2_trace_color_producers)
    {
        g_original_copy_subresource_region(context, dst, dst_subresource, dst_x, dst_y, dst_z, src, src_subresource, src_box);
        return;
    }
#endif
    ResourceInfo dst_info {};
    ResourceInfo src_info {};
    read_resource_info_from_resource(dst, L"copy_dst", dst_info);
    read_resource_info_from_resource(src, L"copy_src", src_info);
    if (g_config.log_resource_ops)
        log_line("copy_subresource dst=" + hex64(dst_info.resource_key) + " src=" + hex64(src_info.resource_key) +
            " dst_sub=" + std::to_string(dst_subresource) + " src_sub=" + std::to_string(src_subresource));
    record_hdr_composite_copy(dst_info, src_info, "copy_subresource");
    record_color_source_copy(dst_info, src_info, "copy_subresource");
    g_original_copy_subresource_region(context, dst, dst_subresource, dst_x, dst_y, dst_z, src, src_subresource, src_box);
}

void STDMETHODCALLTYPE hooked_update_subresource(ID3D11DeviceContext *context, ID3D11Resource *dst, UINT dst_subresource, const D3D11_BOX *dst_box, const void *src_data, UINT src_row_pitch, UINT src_depth_pitch)
{
    if (dst != nullptr && src_data != nullptr)
    {
        const auto key = reinterpret_cast<std::uint64_t>(dst);
        // 仅跟踪 trace 目标 cb0（性能：全量跟踪导致 3 FPS；多视图新鲜度改由 sdk234 直接读回解决）
#if defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
        if (key != g_trace_ps_cb0_key.load(std::memory_order_relaxed))
        {
            g_original_update_subresource(context, dst, dst_subresource, dst_box, src_data, src_row_pitch, src_depth_pitch);
            return;
        }
#endif
        std::lock_guard lock(g_buffer_info_mutex);
        const auto it = g_buffer_info.find(key);
        if (it != g_buffer_info.end())
        {
            std::uint32_t update_size = it->second.byte_width;
            if (dst_box != nullptr && dst_box->right > dst_box->left)
                update_size = dst_box->right - dst_box->left;
            it->second.last_update_size = update_size;
            it->second.last_update_hash = fnv1a64(src_data, update_size);
            const auto *bytes = static_cast<const std::uint8_t *>(src_data);
            g_buffer_snapshots[key].assign(bytes, bytes + update_size);
        }
    }

    g_original_update_subresource(context, dst, dst_subresource, dst_box, src_data, src_row_pitch, src_depth_pitch);
}

void STDMETHODCALLTYPE hooked_clear_rtv(ID3D11DeviceContext *context, ID3D11RenderTargetView *rtv, const FLOAT color[4])
{
    if (g_config.dx11_on12_swapchain)
    {
        ResourceInfo info {};
        read_resource_info(rtv, L"rtv", info);
        if (g_config.log_resource_ops)
            log_line("clear_rtv res=" + hex64(info.resource_key) + " size=" + std::to_string(info.width) + "x" + std::to_string(info.height) +
                " fmt=" + format_string(info.format) + " color=(" +
                std::to_string(color[0]) + "," + std::to_string(color[1]) + "," + std::to_string(color[2]) + "," + std::to_string(color[3]) + ")");
        record_color_source_copy(info, {}, "clear_rtv");
    }
    g_original_clear_rtv(context, rtv, color);
}

void STDMETHODCALLTYPE hooked_clear_dsv(ID3D11DeviceContext *context, ID3D11DepthStencilView *dsv, UINT flags, FLOAT depth, UINT8 stencil)
{
    if (g_config.dx11_on12_swapchain)
    {
        ResourceInfo info {};
        read_resource_info(dsv, L"dsv", info);
        if (g_config.log_resource_ops)
            log_line("clear_dsv res=" + hex64(info.resource_key) + " size=" + std::to_string(info.width) + "x" + std::to_string(info.height) +
                " fmt=" + format_string(info.format) + " flags=" + std::to_string(flags) +
                " depth=" + std::to_string(depth) + " stencil=" + std::to_string(stencil));
    }
    g_original_clear_dsv(context, dsv, flags, depth, stencil);
}

void install_context_hooks(ID3D11DeviceContext *context)
{
    if (context == nullptr)
        return;

    void **vtable = *reinterpret_cast<void ***>(context);
    if (g_original_vs_set_constant_buffers == nullptr)
        g_original_vs_set_constant_buffers = reinterpret_cast<vs_set_constant_buffers_fn>(vtable[k_idx_vs_set_constant_buffers]);
    if (g_original_vs_set_shader == nullptr)
        g_original_vs_set_shader = reinterpret_cast<vs_set_shader_fn>(vtable[k_idx_vs_set_shader]);
    if (g_original_ps_set_shader_resources == nullptr)
        g_original_ps_set_shader_resources = reinterpret_cast<ps_set_shader_resources_fn>(vtable[k_idx_ps_set_shader_resources]);
    if (g_original_ps_set_shader == nullptr)
        g_original_ps_set_shader = reinterpret_cast<ps_set_shader_fn>(vtable[k_idx_ps_set_shader]);
    if (g_original_ps_set_constant_buffers == nullptr)
        g_original_ps_set_constant_buffers = reinterpret_cast<ps_set_constant_buffers_fn>(vtable[k_idx_ps_set_constant_buffers]);
    if (g_original_cs_set_shader_resources == nullptr)
        g_original_cs_set_shader_resources = reinterpret_cast<cs_set_shader_resources_fn>(vtable[k_idx_cs_set_shader_resources]);
    if (g_original_cs_set_uavs == nullptr)
        g_original_cs_set_uavs = reinterpret_cast<cs_set_uavs_fn>(vtable[k_idx_cs_set_uavs]);
    if (g_original_cs_set_shader == nullptr)
        g_original_cs_set_shader = reinterpret_cast<cs_set_shader_fn>(vtable[k_idx_cs_set_shader]);
    if (g_original_om_set_render_targets == nullptr)
        g_original_om_set_render_targets = reinterpret_cast<om_set_render_targets_fn>(vtable[k_idx_om_set_render_targets]);
    if (g_original_om_set_render_targets_and_uavs == nullptr)
    {
        g_original_om_set_render_targets_and_uavs =
            reinterpret_cast<om_set_render_targets_and_uavs_fn>(vtable[k_idx_om_set_render_targets_and_uavs]);
    }
    if (g_original_dispatch == nullptr)
        g_original_dispatch = reinterpret_cast<dispatch_fn>(vtable[k_idx_dispatch]);
    if (g_original_draw_indexed == nullptr)
        g_original_draw_indexed = reinterpret_cast<draw_indexed_fn>(vtable[k_idx_draw_indexed]);
    if (g_original_draw == nullptr)
        g_original_draw = reinterpret_cast<draw_fn>(vtable[k_idx_draw]);
    if (g_original_map == nullptr)
        g_original_map = reinterpret_cast<map_fn>(vtable[k_idx_map]);
    if (g_original_unmap == nullptr)
        g_original_unmap = reinterpret_cast<unmap_fn>(vtable[k_idx_unmap]);
    if (g_original_rs_set_viewports == nullptr)
        g_original_rs_set_viewports = reinterpret_cast<rs_set_viewports_fn>(vtable[k_idx_rs_set_viewports]);
    if (g_original_copy_subresource_region == nullptr)
        g_original_copy_subresource_region = reinterpret_cast<copy_subresource_region_fn>(vtable[k_idx_copy_subresource_region]);
    if (g_original_copy_resource == nullptr)
        g_original_copy_resource = reinterpret_cast<copy_resource_fn>(vtable[k_idx_copy_resource]);
    if (g_original_update_subresource == nullptr)
        g_original_update_subresource = reinterpret_cast<update_subresource_fn>(vtable[k_idx_update_subresource]);
    if (g_original_cs_set_constant_buffers == nullptr)
        g_original_cs_set_constant_buffers = reinterpret_cast<cs_set_constant_buffers_fn>(vtable[k_idx_cs_set_constant_buffers]);
    if (g_original_clear_rtv == nullptr)
        g_original_clear_rtv = reinterpret_cast<clear_rtv_fn>(vtable[k_idx_clear_rtv]);
    if (g_original_clear_dsv == nullptr)
        g_original_clear_dsv = reinterpret_cast<clear_dsv_fn>(vtable[k_idx_clear_dsv]);

    std::vector<std::pair<std::size_t, void *>> patches {
        { k_idx_ps_set_shader_resources, reinterpret_cast<void *>(&hooked_ps_set_shader_resources) },
        { k_idx_ps_set_shader, reinterpret_cast<void *>(&hooked_ps_set_shader) },
        { k_idx_ps_set_constant_buffers, reinterpret_cast<void *>(&hooked_ps_set_constant_buffers) },
        { k_idx_om_set_render_targets, reinterpret_cast<void *>(&hooked_om_set_render_targets) },
        { k_idx_om_set_render_targets_and_uavs, reinterpret_cast<void *>(&hooked_om_set_render_targets_and_uavs) },
        { k_idx_draw_indexed, reinterpret_cast<void *>(&hooked_draw_indexed) },
        { k_idx_draw, reinterpret_cast<void *>(&hooked_draw) },
        { k_idx_map, reinterpret_cast<void *>(&hooked_map) },
        { k_idx_unmap, reinterpret_cast<void *>(&hooked_unmap) },
        { k_idx_rs_set_viewports, reinterpret_cast<void *>(&hooked_rs_set_viewports) },
        { k_idx_copy_resource, reinterpret_cast<void *>(&hooked_copy_resource) },
        { k_idx_update_subresource, reinterpret_cast<void *>(&hooked_update_subresource) },
    };
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    patches.insert(patches.end(), {
        { k_idx_vs_set_constant_buffers, reinterpret_cast<void *>(&hooked_vs_set_constant_buffers) },
        { k_idx_vs_set_shader, reinterpret_cast<void *>(&hooked_vs_set_shader) },
        { k_idx_cs_set_shader_resources, reinterpret_cast<void *>(&hooked_cs_set_shader_resources) },
        { k_idx_cs_set_uavs, reinterpret_cast<void *>(&hooked_cs_set_uavs) },
        { k_idx_cs_set_shader, reinterpret_cast<void *>(&hooked_cs_set_shader) },
        { k_idx_cs_set_constant_buffers, reinterpret_cast<void *>(&hooked_cs_set_constant_buffers) },
        { k_idx_dispatch, reinterpret_cast<void *>(&hooked_dispatch) },
        { k_idx_copy_subresource_region, reinterpret_cast<void *>(&hooked_copy_subresource_region) },
        { k_idx_clear_rtv, reinterpret_cast<void *>(&hooked_clear_rtv) },
        { k_idx_clear_dsv, reinterpret_cast<void *>(&hooked_clear_dsv) },
    });
#endif
    clone_and_patch_vtable(context, context_vtable_size(context), patches);
}

HRESULT STDMETHODCALLTYPE hooked_create_buffer(ID3D11Device *device, const D3D11_BUFFER_DESC *desc, const D3D11_SUBRESOURCE_DATA *initial_data, ID3D11Buffer **buffer)
{
    const HRESULT hr = g_original_create_buffer(device, desc, initial_data, buffer);
    // Only constant buffers are tracked: every consumer of g_buffer_info /
    // g_buffer_snapshots (cb0 signature check, jitter readback, diagnostics)
    // operates on constant buffers, and snapshotting vertex/index/structured
    // buffers would retain their full contents for the process lifetime.
    if (SUCCEEDED(hr) && desc != nullptr && buffer != nullptr && *buffer != nullptr &&
        (desc->BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0)
    {
        BufferInfo info {};
        info.resource_key = reinterpret_cast<std::uint64_t>(*buffer);
        info.byte_width = desc->ByteWidth;
        info.bind_flags = desc->BindFlags;
        info.usage = desc->Usage;
        if (initial_data != nullptr && initial_data->pSysMem != nullptr && desc->ByteWidth != 0)
        {
            info.last_update_size = desc->ByteWidth;
            info.last_update_hash = fnv1a64(initial_data->pSysMem, desc->ByteWidth);
        }

        std::lock_guard lock(g_buffer_info_mutex);
        g_buffer_info[info.resource_key] = info;
        if (initial_data != nullptr && initial_data->pSysMem != nullptr && desc->ByteWidth != 0)
        {
            const auto *bytes = static_cast<const std::uint8_t *>(initial_data->pSysMem);
            g_buffer_snapshots[info.resource_key] = std::vector<std::uint8_t>(bytes, bytes + desc->ByteWidth);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hooked_create_texture_2d(ID3D11Device *device, const D3D11_TEXTURE2D_DESC *desc, const D3D11_SUBRESOURCE_DATA *initial_data, ID3D11Texture2D **texture)
{
    if (g_config.native_ldr_final_target_unorm && desc != nullptr)
    {
        const std::uint32_t observed_index = g_native_ldr_texture_create_observed_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (observed_index <= 16)
        {
            log_line(std::string("native_ldr_texture_create_observed index=") + std::to_string(observed_index) +
                " device=" + hex64(reinterpret_cast<std::uintptr_t>(device)) +
                " format=" + describe_dxgi_format(desc->Format) +
                " size=" + std::to_string(desc->Width) + "x" + std::to_string(desc->Height) +
                " bind=" + hex64(desc->BindFlags));
        }
    }
    D3D11_TEXTURE2D_DESC effective_texture_desc {};
    const D3D11_TEXTURE2D_DESC *effective_desc = desc;
    const bool final_target_bind_shape = desc != nullptr &&
        (desc->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE)) ==
            (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE) &&
        desc->Width >= 1280 && desc->Height >= 720;
    if (g_config.native_ldr_final_target_unorm && desc != nullptr &&
        desc->Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
        final_target_bind_shape)
    {
        std::uint32_t output_width = 0;
        std::uint32_t output_height = 0;
        {
            std::lock_guard lock(g_state_mutex);
            output_width = g_state.backbuffer_width;
            output_height = g_state.backbuffer_height;
        }
        const bool matches_known_output = output_width != 0 && output_height != 0 &&
            desc->Width == output_width && desc->Height == output_height;
        const std::uint32_t candidate_index = g_native_ldr_final_target_candidate_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (candidate_index <= 32)
        {
            log_line(std::string("native_ldr_final_target_candidate index=") + std::to_string(candidate_index) +
                " device=" + hex64(reinterpret_cast<std::uintptr_t>(device)) +
                " format=29/R8G8B8A8_UNORM_SRGB size=" + std::to_string(desc->Width) + "x" + std::to_string(desc->Height) +
                " bind=" + hex64(desc->BindFlags) +
                " known_output=" + std::to_string(matches_known_output ? 1 : 0));
        }
        if (matches_known_output || (output_width == 0 && output_height == 0))
        {
            effective_texture_desc = *desc;
            effective_texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            effective_desc = &effective_texture_desc;
            log_line(std::string("native_ldr_final_target_format from=29/R8G8B8A8_UNORM_SRGB to=28/R8G8B8A8_UNORM") +
                " size=" + std::to_string(desc->Width) + "x" + std::to_string(desc->Height) +
                " bind=0x" + hex64(desc->BindFlags));
        }
    }

    const ULONGLONG trace_until = g_texture_trace_until_tick.load(std::memory_order_relaxed);
    if (desc != nullptr && trace_until >= GetTickCount64() && desc->Width >= 512 && desc->Height >= 288)
    {
        const std::uint32_t trace_index = g_texture_trace_count.fetch_add(1, std::memory_order_relaxed);
        if (trace_index < g_config.texture_trace_limit)
        {
            void *frames[6] {};
            const USHORT frame_count = CaptureStackBackTrace(1, static_cast<DWORD>(std::size(frames)), frames, nullptr);
            std::ostringstream out;
            out << "texture_create index=" << trace_index
                << " size=" << desc->Width << "x" << desc->Height
                << " mip=" << desc->MipLevels
                << " array=" << desc->ArraySize
                << " format=" << static_cast<std::uint32_t>(desc->Format)
                << " sample=" << desc->SampleDesc.Count
                << " usage=" << static_cast<std::uint32_t>(desc->Usage)
                << " bind=0x" << std::hex << desc->BindFlags
                << " misc=0x" << desc->MiscFlags
                << " stack=";
            for (USHORT frame_index = 0; frame_index < frame_count; ++frame_index)
            {
                if (frame_index != 0)
                    out << ',';
                out << hex64(reinterpret_cast<std::uintptr_t>(frames[frame_index]));
            }
            log_line(out.str());
        }
    }
    const HRESULT result = g_original_create_texture_2d(device, effective_desc, initial_data, texture);
    if (SUCCEEDED(result) && effective_desc != desc && texture != nullptr && *texture != nullptr)
    {
        log_line(std::string("native_ldr_final_target_created device=") +
            hex64(reinterpret_cast<std::uintptr_t>(device)) +
            " resource=" + hex64(reinterpret_cast<std::uintptr_t>(*texture)) +
            " requested_format=29/R8G8B8A8_UNORM_SRGB created_format=28/R8G8B8A8_UNORM" +
            " size=" + std::to_string(desc->Width) + "x" + std::to_string(desc->Height));
    }
    return result;
}

HRESULT STDMETHODCALLTYPE hooked_create_pixel_shader(ID3D11Device *device, const void *shader_bytecode, SIZE_T bytecode_length, ID3D11ClassLinkage *class_linkage, ID3D11PixelShader **pixel_shader)
{
    const std::uint64_t bytecode_hash = shader_bytecode != nullptr && bytecode_length != 0
        ? fnv1a64(shader_bytecode, static_cast<std::size_t>(bytecode_length))
        : 0;

    const HRESULT hr = g_original_create_pixel_shader(device, shader_bytecode, bytecode_length, class_linkage, pixel_shader);
    if (SUCCEEDED(hr) && pixel_shader != nullptr && *pixel_shader != nullptr)
    {
        const auto key = reinterpret_cast<std::uint64_t>(*pixel_shader);
#if defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
        const std::uint64_t fast_target_hash = g_mode2_fast_target_ps_hash.load(std::memory_order_relaxed);
        if (g_config.fsr2_fast_state_tracking && fast_target_hash != 0 && bytecode_hash == fast_target_hash)
            g_mode2_fast_target_ps_key.store(key, std::memory_order_relaxed);
#endif
        {
            std::lock_guard lock(g_shader_info_mutex);
            g_pixel_shader_info[key] = { bytecode_hash, static_cast<std::size_t>(bytecode_length) };
        }
        dump_pixel_shader_bytecode(bytecode_hash, shader_bytecode, static_cast<std::size_t>(bytecode_length));
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hooked_create_vertex_shader(ID3D11Device *device, const void *shader_bytecode, SIZE_T bytecode_length, ID3D11ClassLinkage *class_linkage, ID3D11VertexShader **vertex_shader)
{
    const std::uint64_t bytecode_hash = shader_bytecode != nullptr && bytecode_length != 0
        ? fnv1a64(shader_bytecode, static_cast<std::size_t>(bytecode_length))
        : 0;

    const HRESULT hr = g_original_create_vertex_shader(device, shader_bytecode, bytecode_length, class_linkage, vertex_shader);
    if (SUCCEEDED(hr) && vertex_shader != nullptr && *vertex_shader != nullptr)
    {
        const auto key = reinterpret_cast<std::uint64_t>(*vertex_shader);
        {
            std::lock_guard lock(g_shader_info_mutex);
            g_vertex_shader_info[key] = { bytecode_hash, static_cast<std::size_t>(bytecode_length) };
        }
        dump_vertex_shader_bytecode(bytecode_hash, shader_bytecode, static_cast<std::size_t>(bytecode_length));
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hooked_create_compute_shader(ID3D11Device *device, const void *shader_bytecode, SIZE_T bytecode_length, ID3D11ClassLinkage *class_linkage, ID3D11ComputeShader **compute_shader)
{
    const std::uint64_t bytecode_hash = shader_bytecode != nullptr && bytecode_length != 0
        ? fnv1a64(shader_bytecode, static_cast<std::size_t>(bytecode_length))
        : 0;

    const HRESULT hr = g_original_create_compute_shader(device, shader_bytecode, bytecode_length, class_linkage, compute_shader);
    if (SUCCEEDED(hr) && compute_shader != nullptr && *compute_shader != nullptr)
    {
        const auto key = reinterpret_cast<std::uint64_t>(*compute_shader);
        ShaderInfo info = reflect_compute_shader(bytecode_hash, shader_bytecode, static_cast<std::size_t>(bytecode_length));
        {
            std::lock_guard lock(g_shader_info_mutex);
            g_compute_shader_info[key] = info;
            if (bytecode_hash != 0)
                g_compute_shader_info_by_hash[bytecode_hash] = info;
        }
        dump_compute_shader_bytecode(bytecode_hash, shader_bytecode, static_cast<std::size_t>(bytecode_length));
    }
    return hr;
}

void install_device_hooks(ID3D11Device *device)
{
    if (device == nullptr)
        return;

    void **vtable = *reinterpret_cast<void ***>(device);
    if (g_original_create_buffer == nullptr)
        g_original_create_buffer = reinterpret_cast<create_buffer_fn>(vtable[k_idx_device_create_buffer]);
    if (g_original_create_texture_2d == nullptr)
        g_original_create_texture_2d = reinterpret_cast<create_texture_2d_fn>(vtable[k_idx_device_create_texture_2d]);
    if (g_original_create_vertex_shader == nullptr)
        g_original_create_vertex_shader = reinterpret_cast<create_vertex_shader_fn>(vtable[k_idx_device_create_vertex_shader]);
    if (g_original_create_pixel_shader == nullptr)
        g_original_create_pixel_shader = reinterpret_cast<create_pixel_shader_fn>(vtable[k_idx_device_create_pixel_shader]);
    if (g_original_create_compute_shader == nullptr)
        g_original_create_compute_shader = reinterpret_cast<create_compute_shader_fn>(vtable[k_idx_device_create_compute_shader]);

    std::vector<std::pair<std::size_t, void *>> patches {
        { k_idx_device_create_buffer, reinterpret_cast<void *>(&hooked_create_buffer) },
        { k_idx_device_create_pixel_shader, reinterpret_cast<void *>(&hooked_create_pixel_shader) },
    };
    if (g_config.trace_texture_creates || g_config.native_ldr_final_target_unorm)
    {
        patches.emplace_back(k_idx_device_create_texture_2d, reinterpret_cast<void *>(&hooked_create_texture_2d));
    }
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    patches.insert(patches.end(), {
        { k_idx_device_create_vertex_shader, reinterpret_cast<void *>(&hooked_create_vertex_shader) },
        { k_idx_device_create_compute_shader, reinterpret_cast<void *>(&hooked_create_compute_shader) },
    });
#endif
    clone_and_patch_vtable(device, k_device_vtable_size, patches);

    if (g_config.trace_texture_creates || g_config.native_ldr_final_target_unorm)
    {
        const std::vector<std::pair<std::size_t, void *>> texture_patches {
            { k_idx_device_create_texture_2d, reinterpret_cast<void *>(&hooked_create_texture_2d) },
        };
        auto patch_extended_interface = [&](REFIID interface_id, const char *interface_name)
        {
            IUnknown *extended = nullptr;
            if (FAILED(device->QueryInterface(interface_id, reinterpret_cast<void **>(&extended))) || extended == nullptr)
                return;
            const auto interface_key = reinterpret_cast<std::uint64_t>(extended);
            clone_and_patch_vtable(extended, k_device_vtable_size, texture_patches);
            log_line(std::string("texture_device_interface_hook interface=") + interface_name +
                " instance=" + hex64(interface_key));
            extended->Release();
        };
        patch_extended_interface(__uuidof(ID3D11Device1), "ID3D11Device1");
        patch_extended_interface(__uuidof(ID3D11Device2), "ID3D11Device2");
        patch_extended_interface(__uuidof(ID3D11Device3), "ID3D11Device3");
        patch_extended_interface(__uuidof(ID3D11Device4), "ID3D11Device4");
        patch_extended_interface(__uuidof(ID3D11Device5), "ID3D11Device5");
    }
}

void update_swapchain_backbuffer_resources(IDXGISwapChain *swapchain)
{
    if ((!g_config.hdr_composite_probe && !g_config.hdr_sdr_tone_map) || swapchain == nullptr)
        return;

    DXGI_SWAP_CHAIN_DESC desc {};
    if (FAILED(swapchain->GetDesc(&desc)) || desc.BufferCount == 0)
        return;

    std::unordered_set<std::uint64_t> resources;
    for (UINT index = 0; index < desc.BufferCount; ++index)
    {
        ID3D11Texture2D *buffer = nullptr;
        if (SUCCEEDED(swapchain->GetBuffer(index, IID_PPV_ARGS(&buffer))) && buffer != nullptr)
        {
            resources.insert(reinterpret_cast<std::uint64_t>(static_cast<ID3D11Resource *>(buffer)));
            buffer->Release();
        }
    }
    if (resources.empty())
        return;

    {
        std::lock_guard lock(g_swapchain_backbuffer_mutex);
        g_swapchain_backbuffer_resources.insert(resources.begin(), resources.end());
    }
    std::string resource_keys;
    for (const std::uint64_t resource_key : resources)
    {
        if (!resource_keys.empty())
            resource_keys += ",";
        resource_keys += hex64(resource_key);
    }
    log_line("hdr_composite_backbuffers_registered count=" + std::to_string(resources.size()) +
        " resources=" + resource_keys +
        " format=" + describe_dxgi_format(desc.BufferDesc.Format) +
        " size=" + std::to_string(desc.BufferDesc.Width) + "x" + std::to_string(desc.BufferDesc.Height));
}

void install_swapchain_hooks(IDXGISwapChain *swapchain)
{
    if (swapchain == nullptr)
        return;

    update_swapchain_backbuffer_resources(swapchain);

    const bool should_hook_present = g_config.hook_present || g_config.final_scene_probe ||
        g_config.final_scene_snapshot || g_config.final_scene_optifg_input;
    const bool should_hook_dlssg_dxgi = dlssg_dxgi_workaround_active();
    const bool should_hook_general_swapchain_controls = should_hook_dlssg_dxgi || hdr_swapchain_force_active();
    const bool should_hook_native_ldr_resize = g_config.native_ldr_swapchain_unorm;
    const bool should_hook_swapchain_controls = should_hook_general_swapchain_controls || should_hook_native_ldr_resize;
    const bool should_hook_color = k_color_diagnostics_enabled || hdr_swapchain_spoof_active() ||
        hdr_swapchain_force_active();
    if (!should_hook_present && !should_hook_swapchain_controls && !should_hook_color)
        return;

    void *hook_instance = swapchain;
    std::size_t hook_method_count = k_swapchain_vtable_size;
    IDXGISwapChain4 *swapchain4 = nullptr;
    IDXGISwapChain3 *swapchain3 = nullptr;
    bool supports_hdr_metadata = false;
    if (should_hook_color)
    {
        if (SUCCEEDED(swapchain->QueryInterface(__uuidof(IDXGISwapChain4), reinterpret_cast<void **>(&swapchain4))) && swapchain4 != nullptr)
        {
            hook_instance = swapchain4;
            supports_hdr_metadata = true;
        }
        else if (SUCCEEDED(swapchain->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void **>(&swapchain3))) && swapchain3 != nullptr)
        {
            hook_instance = swapchain3;
            hook_method_count = 39;
        }
        else
        {
            log_line("dxgi_color_hooks_unavailable swapchain=" +
                hex64(reinterpret_cast<std::uintptr_t>(swapchain)) + " reason=no_IDXGISwapChain3");
            if (!should_hook_present && !should_hook_swapchain_controls)
                return;
        }
    }

    void **vtable = *reinterpret_cast<void ***>(hook_instance);
    if (should_hook_present)
    {
        const auto original_present = reinterpret_cast<present_fn>(vtable[k_idx_present]);
        if (g_original_present == nullptr)
            g_original_present = original_present;
        std::lock_guard lock(g_swapchain_present_mutex);
        g_original_present_by_instance.try_emplace(hook_instance, original_present);
        g_original_present_by_instance.try_emplace(swapchain, original_present);
    }
    if (should_hook_general_swapchain_controls && g_original_set_fullscreen_state == nullptr)
        g_original_set_fullscreen_state = reinterpret_cast<set_fullscreen_state_fn>(vtable[k_idx_set_fullscreen_state]);
    if (should_hook_general_swapchain_controls && g_original_get_fullscreen_state == nullptr)
        g_original_get_fullscreen_state = reinterpret_cast<get_fullscreen_state_fn>(vtable[k_idx_get_fullscreen_state]);
    if (should_hook_general_swapchain_controls && g_original_resize_buffers == nullptr)
        g_original_resize_buffers = reinterpret_cast<resize_buffers_fn>(vtable[k_idx_resize_buffers]);
    if (should_hook_native_ldr_resize)
    {
        std::lock_guard lock(g_swapchain_resize_mutex);
        g_original_resize_buffers_by_instance.try_emplace(hook_instance, reinterpret_cast<resize_buffers_fn>(vtable[k_idx_resize_buffers]));
    }
    if (should_hook_general_swapchain_controls && g_original_resize_target == nullptr)
        g_original_resize_target = reinterpret_cast<resize_target_fn>(vtable[k_idx_resize_target]);
    const bool has_color_interface = swapchain4 != nullptr || swapchain3 != nullptr;
    if (has_color_interface && g_original_check_color_space_support == nullptr)
        g_original_check_color_space_support = reinterpret_cast<check_color_space_support_fn>(vtable[k_idx_check_color_space_support]);
    if (has_color_interface && g_original_set_color_space1 == nullptr)
        g_original_set_color_space1 = reinterpret_cast<set_color_space1_fn>(vtable[k_idx_set_color_space1]);
    if (supports_hdr_metadata && g_original_set_hdr_metadata == nullptr)
        g_original_set_hdr_metadata = reinterpret_cast<set_hdr_metadata_fn>(vtable[k_idx_set_hdr_metadata]);
    std::vector<std::pair<std::size_t, void *>> patches;
    if (should_hook_present)
        patches.emplace_back(k_idx_present, reinterpret_cast<void *>(&hooked_present));
    if (should_hook_general_swapchain_controls)
    {
        patches.emplace_back(k_idx_set_fullscreen_state, reinterpret_cast<void *>(&hooked_set_fullscreen_state));
        patches.emplace_back(k_idx_get_fullscreen_state, reinterpret_cast<void *>(&hooked_get_fullscreen_state));
        patches.emplace_back(k_idx_resize_buffers, reinterpret_cast<void *>(&hooked_resize_buffers));
        patches.emplace_back(k_idx_resize_target, reinterpret_cast<void *>(&hooked_resize_target));
    }
    else if (should_hook_native_ldr_resize)
    {
        patches.emplace_back(k_idx_resize_buffers, reinterpret_cast<void *>(&hooked_resize_buffers));
    }
    if (should_hook_color && has_color_interface)
    {
        patches.emplace_back(k_idx_check_color_space_support, reinterpret_cast<void *>(&hooked_check_color_space_support));
        patches.emplace_back(k_idx_set_color_space1, reinterpret_cast<void *>(&hooked_set_color_space1));
        if (supports_hdr_metadata)
            patches.emplace_back(k_idx_set_hdr_metadata, reinterpret_cast<void *>(&hooked_set_hdr_metadata));

        DXGI_SWAP_CHAIN_DESC desc {};
        if (SUCCEEDED(swapchain->GetDesc(&desc)))
        {
            log_line("dxgi_color_hooks_active swapchain=" +
                hex64(reinterpret_cast<std::uintptr_t>(hook_instance)) +
                " format=" + describe_dxgi_format(desc.BufferDesc.Format) +
                " size=" + std::to_string(desc.BufferDesc.Width) + "x" + std::to_string(desc.BufferDesc.Height) +
                " swapchain4=" + std::to_string(supports_hdr_metadata ? 1 : 0));
        }
    }
    clone_and_patch_vtable(hook_instance, hook_method_count, patches);
    if (swapchain4 != nullptr)
        swapchain4->Release();
    if (swapchain3 != nullptr)
        swapchain3->Release();
}

void set_output_size(UINT width, UINT height, const char *source)
{
    if (width == 0 || height == 0)
        return;

    bool changed = false;
    {
        std::lock_guard lock(g_state_mutex);
        changed = g_state.backbuffer_width != width || g_state.backbuffer_height != height;
        g_state.backbuffer_width = width;
        g_state.backbuffer_height = height;
    }

    if (changed)
        log_line(std::string(source) + " output_size=" + std::to_string(width) + "x" + std::to_string(height));
}

void install_factory_hooks(IDXGIFactory *factory)
{
    if (factory == nullptr)
        return;

    void **vtable = *reinterpret_cast<void ***>(factory);
    if (g_original_factory_create_swap_chain == nullptr)
        g_original_factory_create_swap_chain = reinterpret_cast<factory_create_swap_chain_fn>(vtable[k_idx_factory_create_swap_chain]);

    clone_and_patch_vtable(factory, k_factory_vtable_size, {
        { k_idx_factory_create_swap_chain, reinterpret_cast<void *>(&hooked_factory_create_swap_chain) },
    });
}

void install_factory2_hooks(IDXGIFactory2 *factory)
{
    if (factory == nullptr)
        return;

    void **vtable = *reinterpret_cast<void ***>(factory);
    if (g_original_factory2_create_swap_chain_for_hwnd == nullptr)
        g_original_factory2_create_swap_chain_for_hwnd = reinterpret_cast<factory2_create_swap_chain_for_hwnd_fn>(vtable[k_idx_factory2_create_swap_chain_for_hwnd]);

    clone_and_patch_vtable(factory, k_factory2_vtable_size, {
        { k_idx_factory_create_swap_chain, reinterpret_cast<void *>(&hooked_factory_create_swap_chain) },
        { k_idx_factory2_create_swap_chain_for_hwnd, reinterpret_cast<void *>(&hooked_factory2_create_swap_chain_for_hwnd) },
    });
}

void install_factory_hooks_from_device(ID3D11Device *device)
{
    if (device == nullptr)
        return;

    IDXGIDevice *dxgi_device = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi_device))) || dxgi_device == nullptr)
        return;

    IDXGIAdapter *adapter = nullptr;
    if (FAILED(dxgi_device->GetAdapter(&adapter)) || adapter == nullptr)
    {
        dxgi_device->Release();
        return;
    }

    IDXGIFactory2 *factory2 = nullptr;
    if (SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void **>(&factory2))) && factory2 != nullptr)
    {
        install_factory2_hooks(factory2);
        factory2->Release();
    }

    IDXGIFactory *factory = nullptr;
    if (SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void **>(&factory))) && factory != nullptr)
    {
        install_factory_hooks(factory);
        factory->Release();
    }

    install_hdr_output_desc_probe_from_adapter(adapter);
    adapter->Release();
    dxgi_device->Release();
}

HRESULT WINAPI hooked_create_device_and_swapchain(
    IDXGIAdapter *adapter,
    D3D_DRIVER_TYPE driver_type,
    HMODULE software,
    UINT flags,
    const D3D_FEATURE_LEVEL *feature_levels,
    UINT feature_levels_count,
    UINT sdk_version,
    const DXGI_SWAP_CHAIN_DESC *swapchain_desc,
    IDXGISwapChain **swapchain,
    ID3D11Device **device,
    D3D_FEATURE_LEVEL *feature_level,
    ID3D11DeviceContext **context)
{
    // 旧 On12 引导移除（旧方案隔离）。
    DXGI_SWAP_CHAIN_DESC effective_swapchain_desc {};
    const DXGI_SWAP_CHAIN_DESC *effective_desc = swapchain_desc;
    if (g_config.native_ldr_swapchain_unorm && swapchain_desc != nullptr &&
        swapchain_desc->BufferDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
    {
        effective_swapchain_desc = *swapchain_desc;
        effective_swapchain_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        effective_desc = &effective_swapchain_desc;
        log_line("native_ldr_swapchain_format from=29/R8G8B8A8_UNORM_SRGB to=28/R8G8B8A8_UNORM api=CreateDeviceAndSwapChain");
    }

    const HRESULT hr = g_original_create_device_and_swapchain(
        adapter,
        driver_type,
        software,
        flags,
        feature_levels,
        feature_levels_count,
        sdk_version,
        effective_desc,
        swapchain,
        device,
        feature_level,
        context);

    if (SUCCEEDED(hr))
    {
        install_device_hooks(device != nullptr ? *device : nullptr);
        install_factory_hooks_from_device(device != nullptr ? *device : nullptr);
        install_context_hooks(context != nullptr ? *context : nullptr);
        route_from_d3d11_device(device != nullptr ? *device : nullptr); // ：设备级路由补检
        if (swapchain != nullptr && *swapchain != nullptr)
        {
            DXGI_SWAP_CHAIN_DESC created_desc {};
            if (SUCCEEDED((*swapchain)->GetDesc(&created_desc)))
                set_output_size(created_desc.BufferDesc.Width, created_desc.BufferDesc.Height, "CreateDeviceAndSwapChain");
            if (g_config.hook_present || g_config.final_scene_probe || g_config.final_scene_snapshot ||
                g_config.final_scene_optifg_input || dlssg_dxgi_workaround_active() ||
                k_color_diagnostics_enabled || hdr_swapchain_spoof_active() || hdr_swapchain_force_active())
                install_swapchain_hooks(*swapchain);
        }
        log_line("hooked D3D11CreateDeviceAndSwapChain");
    }
    return hr;
}

HRESULT WINAPI hooked_create_device(
    IDXGIAdapter *adapter,
    D3D_DRIVER_TYPE driver_type,
    HMODULE software,
    UINT flags,
    const D3D_FEATURE_LEVEL *feature_levels,
    UINT feature_levels_count,
    UINT sdk_version,
    ID3D11Device **device,
    D3D_FEATURE_LEVEL *feature_level,
    ID3D11DeviceContext **context)
{
    // 旧 On12 引导移除（旧方案隔离）。
    const HRESULT hr = g_original_create_device(
        adapter,
        driver_type,
        software,
        flags,
        feature_levels,
        feature_levels_count,
        sdk_version,
        device,
        feature_level,
        context);

    if (SUCCEEDED(hr))
    {
        install_device_hooks(device != nullptr ? *device : nullptr);
        install_factory_hooks_from_device(device != nullptr ? *device : nullptr);
        install_context_hooks(context != nullptr ? *context : nullptr);
        route_from_d3d11_device(device != nullptr ? *device : nullptr); // ：设备级路由补检
        log_line("hooked D3D11CreateDevice");
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hooked_factory_create_swap_chain(IDXGIFactory *factory, IUnknown *device, DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **swapchain)
{
#if defined(DX11FSRBRIDGE_FG_DXGI_DIAGNOSTICS)
    const std::uint64_t request_id = g_dxgi_swapchain_request_id.fetch_add(1, std::memory_order_relaxed) + 1;
    void *const caller = _ReturnAddress();
    log_line("dxgi_swapchain_request id=" + std::to_string(request_id) +
        " api=CreateSwapChain caller=" + module_path_from_address(caller) +
        " factory=" + hex64(reinterpret_cast<std::uintptr_t>(factory)) +
        " device={" + describe_dxgi_swapchain_device(device) + "} " +
        describe_dxgi_swapchain_desc(desc));
#endif

    DXGI_SWAP_CHAIN_DESC effective_swapchain_desc {};
    DXGI_SWAP_CHAIN_DESC *effective_desc = desc;
    if (g_config.native_ldr_swapchain_unorm && desc != nullptr &&
        desc->BufferDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
    {
        effective_swapchain_desc = *desc;
        effective_swapchain_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        effective_desc = &effective_swapchain_desc;
        log_line("native_ldr_swapchain_format from=29/R8G8B8A8_UNORM_SRGB to=28/R8G8B8A8_UNORM api=CreateSwapChain");
    }

    HRESULT hr = g_original_factory_create_swap_chain(factory, device, effective_desc, swapchain);
#if defined(DX11FSRBRIDGE_FG_DXGI_DIAGNOSTICS)
    log_line("dxgi_swapchain_result id=" + std::to_string(request_id) +
        " api=CreateSwapChain hr=" + hex32(static_cast<std::uint32_t>(hr)) +
        " swapchain=" + hex64(reinterpret_cast<std::uintptr_t>(swapchain != nullptr ? *swapchain : nullptr)));
#endif
    if (SUCCEEDED(hr))
    {
        if (desc != nullptr)
            set_output_size(desc->BufferDesc.Width, desc->BufferDesc.Height, "CreateSwapChain");
        if (g_config.hook_present || g_config.final_scene_probe || g_config.final_scene_snapshot ||
            g_config.final_scene_optifg_input || g_config.hdr_composite_probe || dlssg_dxgi_workaround_active() ||
            k_color_diagnostics_enabled || hdr_swapchain_spoof_active() || hdr_swapchain_force_active())
            install_swapchain_hooks(swapchain != nullptr ? *swapchain : nullptr);
        if (desc != nullptr)
            log_line("hooked CreateSwapChain size=" + std::to_string(desc->BufferDesc.Width) + "x" + std::to_string(desc->BufferDesc.Height));
        else
            log_line("hooked CreateSwapChain");
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hooked_factory2_create_swap_chain_for_hwnd(IDXGIFactory2 *factory, IUnknown *device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1 *desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen_desc, IDXGIOutput *restrict_to_output, IDXGISwapChain1 **swapchain)
{
#if defined(DX11FSRBRIDGE_FG_DXGI_DIAGNOSTICS)
    const std::uint64_t request_id = g_dxgi_swapchain_request_id.fetch_add(1, std::memory_order_relaxed) + 1;
    void *const caller = _ReturnAddress();
    log_line("dxgi_swapchain_request id=" + std::to_string(request_id) +
        " api=CreateSwapChainForHwnd caller=" + module_path_from_address(caller) +
        " factory=" + hex64(reinterpret_cast<std::uintptr_t>(factory)) +
        " restrict_output=" + hex64(reinterpret_cast<std::uintptr_t>(restrict_to_output)) +
        " device={" + describe_dxgi_swapchain_device(device) + "} " +
        describe_dxgi_swapchain_desc(desc) + " " +
        describe_dxgi_fullscreen_desc(fullscreen_desc) + " " +
        describe_dxgi_window(hwnd));
#endif

    DXGI_SWAP_CHAIN_DESC1 effective_swapchain_desc {};
    const DXGI_SWAP_CHAIN_DESC1 *effective_desc = desc;
    if (g_config.native_ldr_swapchain_unorm && desc != nullptr &&
        desc->Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
    {
        effective_swapchain_desc = *desc;
        effective_swapchain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        effective_desc = &effective_swapchain_desc;
        log_line("native_ldr_swapchain_format from=29/R8G8B8A8_UNORM_SRGB to=28/R8G8B8A8_UNORM api=CreateSwapChainForHwnd");
    }

    HRESULT hr = g_original_factory2_create_swap_chain_for_hwnd(factory, device, hwnd, effective_desc, fullscreen_desc, restrict_to_output, swapchain);
#if defined(DX11FSRBRIDGE_FG_DXGI_DIAGNOSTICS)
    log_line("dxgi_swapchain_result id=" + std::to_string(request_id) +
        " api=CreateSwapChainForHwnd hr=" + hex32(static_cast<std::uint32_t>(hr)) +
        " swapchain=" + hex64(reinterpret_cast<std::uintptr_t>(swapchain != nullptr ? *swapchain : nullptr)));
#endif
    if (SUCCEEDED(hr))
    {
        if (desc != nullptr)
            set_output_size(desc->Width, desc->Height, "CreateSwapChainForHwnd");
        if (g_config.hook_present || g_config.final_scene_probe || g_config.final_scene_snapshot ||
            g_config.final_scene_optifg_input || g_config.hdr_composite_probe || dlssg_dxgi_workaround_active() ||
            k_color_diagnostics_enabled || hdr_swapchain_spoof_active() || hdr_swapchain_force_active())
            install_swapchain_hooks(swapchain != nullptr ? *swapchain : nullptr);
        if (desc != nullptr)
            log_line("hooked CreateSwapChainForHwnd size=" + std::to_string(desc->Width) + "x" + std::to_string(desc->Height));
        else
            log_line("hooked CreateSwapChainForHwnd");
    }
    return hr;
}

void on_module_activity(const char *source, HMODULE module)
{
    if (module != nullptr && is_d3d11_module(module))
        log_line(std::string(source) + " loaded d3d11.dll");

    install_create_hooks_for_loaded_modules();
    install_loader_hooks_for_loaded_modules();
    install_hdr_environment_probe_for_loaded_modules();
#if defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
    // 旧 FSR2 shim 查询日志已移除（旧 OptiScaler 接入）。
#endif
}

HMODULE WINAPI hooked_load_library_a(LPCSTR file_name)
{
    const HMODULE module = g_original_load_library_a(file_name);
    on_module_activity("LoadLibraryA", module);
    return module;
}

HMODULE WINAPI hooked_load_library_w(LPCWSTR file_name)
{
    const HMODULE module = g_original_load_library_w(file_name);
    on_module_activity("LoadLibraryW", module);
    return module;
}

HMODULE WINAPI hooked_load_library_ex_a(LPCSTR file_name, HANDLE file, DWORD flags)
{
    const HMODULE module = g_original_load_library_ex_a(file_name, file, flags);
    on_module_activity("LoadLibraryExA", module);
    return module;
}

HMODULE WINAPI hooked_load_library_ex_w(LPCWSTR file_name, HANDLE file, DWORD flags)
{
    const HMODULE module = g_original_load_library_ex_w(file_name, file, flags);
    on_module_activity("LoadLibraryExW", module);
    return module;
}

LONG WINAPI hooked_display_config_get_device_info(DISPLAYCONFIG_DEVICE_INFO_HEADER *request)
{
    const LONG result = g_original_display_config_get_device_info != nullptr
        ? g_original_display_config_get_device_info(request)
        : ERROR_PROC_NOT_FOUND;

    if (!g_config.hdr_environment_probe || request == nullptr)
        return result;

    constexpr std::uint32_t k_advanced_color_info_2 = 15;
    const std::uint32_t query_type = static_cast<std::uint32_t>(request->type);
    if (query_type != DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO && query_type != k_advanced_color_info_2)
        return result;

    constexpr std::uint32_t k_log_limit = 32;
    const std::uint32_t call_index = g_hdr_environment_probe_call_count.fetch_add(1, std::memory_order_relaxed);
    if (call_index >= k_log_limit)
    {
        if (!g_hdr_environment_probe_suppressed_logged.exchange(true, std::memory_order_relaxed))
            log_line("hdr_environment_probe query log limit reached");
        return result;
    }

    std::ostringstream out;
    out << "hdr_environment_probe query=" << query_type
        << " result=" << result
        << " adapter=" << request->adapterId.HighPart << ":" << request->adapterId.LowPart
        << " target=" << request->id;

    if (result != ERROR_SUCCESS || request->size < sizeof(DISPLAYCONFIG_DEVICE_INFO_HEADER) + sizeof(std::uint32_t))
    {
        out << " response=unavailable size=" << request->size;
        log_line(out.str());
        return result;
    }

    const auto *flags = reinterpret_cast<const std::uint32_t *>(
        reinterpret_cast<const std::uint8_t *>(request) + sizeof(DISPLAYCONFIG_DEVICE_INFO_HEADER));
    const std::uint32_t value = *flags;
    if (query_type == DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO)
    {
        out << " advanced_supported=" << ((value & (1u << 0)) != 0 ? 1 : 0)
            << " advanced_enabled=" << ((value & (1u << 1)) != 0 ? 1 : 0)
            << " policy_disabled=" << ((value & (1u << 3)) != 0 ? 1 : 0);
    }
    else
    {
        out << " advanced_supported=" << ((value & (1u << 0)) != 0 ? 1 : 0)
            << " advanced_active=" << ((value & (1u << 1)) != 0 ? 1 : 0)
            << " hdr_supported=" << ((value & (1u << 4)) != 0 ? 1 : 0)
            << " hdr_user_enabled=" << ((value & (1u << 5)) != 0 ? 1 : 0);
    }
    log_line(out.str());
    return result;
}

LONG WINAPI hooked_display_config_set_device_info(DISPLAYCONFIG_DEVICE_INFO_HEADER *request)
{
    constexpr std::uint32_t k_set_advanced_color_state = 10;
    constexpr std::uint32_t k_set_hdr_state = 16;
    const std::uint32_t request_type = request != nullptr
        ? static_cast<std::uint32_t>(request->type)
        : UINT32_MAX;
    const bool is_hdr_state_request = request_type == k_set_advanced_color_state ||
        request_type == k_set_hdr_state;

    if (g_config.hdr_output_desc_spoof && is_hdr_state_request)
    {
        std::uint32_t requested_enabled = 0;
        if (request->size >= sizeof(DISPLAYCONFIG_DEVICE_INFO_HEADER) + sizeof(std::uint32_t))
        {
            const auto *value = reinterpret_cast<const std::uint32_t *>(
                reinterpret_cast<const std::uint8_t *>(request) + sizeof(DISPLAYCONFIG_DEVICE_INFO_HEADER));
            requested_enabled = *value & 1u;
        }
        log_line("hdr_output_desc_spoof blocked_system_hdr_change query=" +
            std::to_string(request_type) + " enabled=" + std::to_string(requested_enabled));
        return ERROR_SUCCESS;
    }

    return g_original_display_config_set_device_info != nullptr
        ? g_original_display_config_set_device_info(request)
        : ERROR_PROC_NOT_FOUND;
}

FARPROC WINAPI hooked_get_proc_address(HMODULE module, LPCSTR proc_name)
{
    FARPROC address = g_original_get_proc_address(module, proc_name);
    if (address == nullptr || proc_name == nullptr)
        return address;

    if (is_d3d11_module(module))
    {
        if (std::strcmp(proc_name, "D3D11CreateDeviceAndSwapChain") == 0)
        {
            if (g_original_create_device_and_swapchain == nullptr)
                g_original_create_device_and_swapchain = reinterpret_cast<create_device_and_swapchain_fn>(address);
            log_line("GetProcAddress intercepted D3D11CreateDeviceAndSwapChain");
            return reinterpret_cast<FARPROC>(&hooked_create_device_and_swapchain);
        }

        if (std::strcmp(proc_name, "D3D11CreateDevice") == 0)
        {
            if (g_original_create_device == nullptr)
                g_original_create_device = reinterpret_cast<create_device_fn>(address);
            log_line("GetProcAddress intercepted D3D11CreateDevice");
            return reinterpret_cast<FARPROC>(&hooked_create_device);
        }
    }

    if ((g_config.hdr_environment_probe || g_config.hdr_output_desc_spoof) && is_user32_module(module) &&
        std::strcmp(proc_name, "DisplayConfigGetDeviceInfo") == 0)
    {
        if (g_original_display_config_get_device_info == nullptr)
            g_original_display_config_get_device_info = reinterpret_cast<display_config_get_device_info_fn>(address);
        log_line("GetProcAddress intercepted DisplayConfigGetDeviceInfo");
        return reinterpret_cast<FARPROC>(&hooked_display_config_get_device_info);
    }

    if (g_config.hdr_output_desc_spoof && is_user32_module(module) &&
        std::strcmp(proc_name, "DisplayConfigSetDeviceInfo") == 0)
    {
        if (g_original_display_config_set_device_info == nullptr)
            g_original_display_config_set_device_info = reinterpret_cast<display_config_set_device_info_fn>(address);
        log_line("GetProcAddress intercepted DisplayConfigSetDeviceInfo");
        return reinterpret_cast<FARPROC>(&hooked_display_config_set_device_info);
    }

    return address;
}

// 显卡路由统一应用点（DllMain attach 早期 + D3D11 设备创建后补检）。
// 早期 CreateDXGIFactory1/EnumAdapters1 在 loader lock 内可能失败（vendor=0），此时不标记
// applied，留给设备创建 hook 用 IDXGIDevice::GetAdapter 补检——保证 RDNA2 4.0.2c 路由可靠生效。
// passthrough 机制已整体移除（实测让 OptiScaler 丢失 FFX 输入识别）——
// 不再做 GPU 架构分类与 402c 双路径——所有显卡统一默认 provider
// （Ffx12DllPath，payload\AMD\amd_fidelityfx_upscaler_dx12.dll），provider 的
// ffxQueryDescGetVersions 按 GPU 能力返回最高支持版本，降级链 4→3→2 兜底；
// RDNA2 等需要 4.0.2c 的用户自行替换 SDK 文件（见 ini 备注）。
static bool contains_ci(const std::wstring &hay, const wchar_t *needle)
{
    const std::size_t n = std::wcslen(needle);
    if (n == 0 || hay.size() < n)
        return false;
    for (std::size_t i = 0; i + n <= hay.size(); ++i)
    {
        bool match = true;
        for (std::size_t j = 0; j < n; ++j)
        {
            if (std::towlower(hay[i + j]) != std::towlower(needle[j]))
            {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

static void apply_adapter_route(std::uint32_t vendor, std::uint32_t device, const std::wstring &desc,
                                const char *source)
{
    // 不再做显卡型号识别与 402c 双路径——所有显卡统一走默认 provider
    // （Ffx12DllPath，默认 payload\AMD\amd_fidelityfx_upscaler_dx12.dll）。
    // provider 的 ffxQueryDescGetVersions 按 GPU 能力返回最高支持版本，降级链 4→3→2 兜底；
    // RDNA2 等需要 4.0.2c 的用户自行替换 SDK 文件（见 ini 备注）。
    // 本函数仅保留焦点框的显卡/SDK 记录。
    (void)device;
    (void)source;
    if (g_route_applied)
        return;
    if (vendor == 0)
        return; // 检测失败：留给后续补检（设备创建后）
    char vendor_hex[16] {};
    std::snprintf(vendor_hex, sizeof(vendor_hex), "0x%04X", vendor);
    g_route_vendor_label = vendor_hex;
    g_route_gpu_name = narrow(desc.c_str());
    g_route_sdk_path = narrow(g_config.ffx12_dll_path.c_str());
    g_route_applied = true;
}

// D3D11 设备就绪后的可靠补检（IDXGIDevice::GetAdapter 不受 loader lock 影响）
static void route_from_d3d11_device(ID3D11Device *d3d11_device)
{
    if (g_route_applied || d3d11_device == nullptr)
        return;
    IDXGIDevice *dxgi_device = nullptr;
    if (FAILED(d3d11_device->QueryInterface(__uuidof(IDXGIDevice),
                                            reinterpret_cast<void **>(&dxgi_device))) ||
        dxgi_device == nullptr)
        return;
    IDXGIAdapter *adapter = nullptr;
    std::uint32_t vendor = 0, device_id = 0;
    std::wstring desc_text;
    if (SUCCEEDED(dxgi_device->GetAdapter(&adapter)) && adapter != nullptr)
    {
        DXGI_ADAPTER_DESC desc {};
        if (SUCCEEDED(adapter->GetDesc(&desc)))
        {
            vendor = desc.VendorId;
            device_id = desc.DeviceId;
            desc_text = desc.Description;
        }
        adapter->Release();
    }
    dxgi_device->Release();
    if (vendor != 0)
        apply_adapter_route(vendor, device_id, desc_text, "device");
}

void initialize()
{
    wchar_t module_path[MAX_PATH] {};
    DWORD length = GetModuleFileNameW(g_module, module_path, MAX_PATH);
    g_module_dir = std::filesystem::path(std::wstring(module_path, module_path + length)).parent_path();
    g_log_path = g_module_dir / L"Dx11FsrBridge.log";
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    g_frames_path = g_module_dir / L"Dx11FsrBridge.frames.jsonl";
    g_similarity_path = g_module_dir / L"Dx11FsrBridge.similarity.txt";
    g_ps_trace_path = g_module_dir / L"Dx11FsrBridge.ps_trace.jsonl";
#endif
    load_config();

    // 不再做显卡型号识别/402c 双路径——所有显卡统一 preload 默认 provider
    // （Ffx12DllPath，默认 payload\AMD\amd_fidelityfx_upscaler_dx12.dll），
    // provider 的 ffxQueryDescGetVersions 按 GPU 能力返回最高支持版本，降级链 4→3→2 兜底。
    // preload 保证进程内"标准名"模块唯一 = 实际使用的 provider → OptiScaler 的 FFX 输入
    // hook（LdrLoadDll 按名合并）拿到桥的模块 → 识别。
    // 本模块 attach 阶段 OptiScaler 尚未注入其 loader hook，此时 LoadLibrary 不被劫持。
    {
        ffx12::preload(g_config.ffx12_dll_path.c_str()); // 唯一标准名 = 默认 provider
    }

    if (!process_matches())
        return;

    reset_log();

#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    if (g_config.trace_pixel_shader_draws)
    {
        std::lock_guard lock(g_ps_trace_mutex);
        if (g_ps_trace_stream.is_open())
            g_ps_trace_stream.close();
        g_ps_trace_stream.open(g_ps_trace_path, std::ios::trunc);
    }
    g_ps_trace_count = 0;
    g_trace_ps_cb0_key = 0;
#endif

    g_active = true;
    log_line("Dx11FsrBridge active pid=" + std::to_string(GetCurrentProcessId()));
#if defined(DX11FSRBRIDGE_SERVER_DEBUG_RUNTIME)
    wchar_t process_path[MAX_PATH] {};
    const DWORD process_path_length = GetModuleFileNameW(nullptr, process_path, static_cast<DWORD>(std::size(process_path)));
    HMODULE process_module = GetModuleHandleW(nullptr);
    std::uint32_t pe_timestamp = 0;
    std::uint32_t image_size = 0;
    if (process_module != nullptr)
    {
        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(process_module);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE)
        {
            const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(
                reinterpret_cast<const std::uint8_t *>(process_module) + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE)
            {
                pe_timestamp = nt->FileHeader.TimeDateStamp;
                image_size = nt->OptionalHeader.SizeOfImage;
            }
        }
    }
    log_line("build_profile=server_debug process=" +
        narrow(std::wstring(process_path, process_path + process_path_length)) +
        " pe_timestamp=" + hex64(pe_timestamp) +
        " image_size=" + std::to_string(image_size) +
        " target_process_filter=disabled");
#endif
#if defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
    // FSR2 GetProcAddress shim（旧 OptiScaler 接入）已移除。
    if (g_config.fsr2_il2cpp_hook)
    {
        il2cpp_callsite::Config hook_cfg;
        hook_cfg.skip_render = g_config.fsr2_il2cpp_skip_render;
        hook_cfg.render_rva = g_config.fsr2_il2cpp_render_rva;
        hook_cfg.update_cmd_buffer_rva = g_config.fsr2_il2cpp_ucb_rva;
        hook_cfg.capture_camera = g_config.ffx12_probe_camera;
        hook_cfg.camera_rva = g_config.ffx12_camera_rva;
        hook_cfg.capture_projection = g_config.ffx12_projection_hook;
        hook_cfg.projection_setter_rva = g_config.ffx12_projection_setter_rva;
        const std::uint64_t exe_base = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
        log_line("ffx12_camera_config probe=" +
            std::to_string(g_config.ffx12_probe_camera ? 1 : 0) +
            " hook=" + std::to_string(g_config.ffx12_camera_hook ? 1 : 0) +
            " rva=" + hex64(g_config.ffx12_camera_rva) +
            " projection_hook=" + std::to_string(g_config.ffx12_projection_hook ? 1 : 0) +
            " projection_setter=" + hex64(g_config.ffx12_projection_setter_rva));
        if (g_config.ffx12_probe_camera)
        {
            std::string camera_target = "ffx12_camera_target rva=" +
                hex64(hook_cfg.camera_rva) + " va=" + hex64(exe_base + hook_cfg.camera_rva);
            append_code_bytes(camera_target, exe_base + hook_cfg.camera_rva, 16);
            log_line(camera_target);
        }
        if (il2cpp_callsite::install(exe_base, hook_cfg))
            log_line("fsr2_il2cpp_hook_installed mode=" +
                (g_config.fsr2_il2cpp_skip_render ? std::string("skip") : std::string("observe")) +
                " render_va=" + hex64(exe_base + hook_cfg.render_rva));
        else
            log_line("fsr2_il2cpp_hook_failed render_rva=0x" + hex64(hook_cfg.render_rva) +
                " skip=" + std::to_string(g_config.fsr2_il2cpp_skip_render ? 1 : 0) +
                " fallback=draw_family_skip");
        if (g_config.ffx12_camera_hook)
        {
            if (il2cpp_callsite::install_camera(exe_base, hook_cfg))
                log_line("ffx12_camera_hook_installed va=" +
                    hex64(exe_base + hook_cfg.camera_rva));
            else
                log_line("ffx12_camera_hook_rejected rva=" +
                    hex64(hook_cfg.camera_rva));
        }
        if (g_config.ffx12_projection_hook)
        {
            if (il2cpp_callsite::install_projection_setter(exe_base, hook_cfg))
                log_line("ffx12_projection_hook_installed va=" +
                    hex64(exe_base + hook_cfg.projection_setter_rva));
            else
                log_line("ffx12_projection_hook_rejected rva=" +
                    hex64(hook_cfg.projection_setter_rva));
        }
    }
#endif
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    // Release 构建同样需要 OSD（show_osd 配置控制）：此前被 Release 条件编译切掉，
    // 导致 OSD 悬浮窗从未启动（"无可见 OSD"根因）。以下两行移到 #endif 之后。
#endif
    start_osd();
    set_osd_text(L"Dx11FsrBridge OSD\n等待 DX11 dispatch 数据");

    log_line(std::string("d3d11_loaded=") + (GetModuleHandleW(L"d3d11.dll") != nullptr ? "1" : "0"));
    install_create_hooks_for_loaded_modules();
    install_loader_hooks_for_loaded_modules();
    install_hdr_environment_probe_for_loaded_modules();
}

void initialize_once()
{
    std::call_once(g_initialize_once, []() { initialize(); });
}
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);
        initialize_once();
        if (g_active)
            initialize_render_scale_menu(module, &log_line);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        il2cpp_callsite::shutdown();
        {
            std::lock_guard lock(g_ps_trace_mutex);
            if (g_ps_trace_stream.is_open())
            {
                g_ps_trace_stream.flush();
                g_ps_trace_stream.close();
            }
        }
        g_osd_running = false;
        if (g_osd_window != nullptr)
            PostMessageW(g_osd_window, WM_CLOSE, 0, 0);
    }
    return TRUE;
}
