#include <Windows.h>
#include <TlHelp32.h>
#include <d3d11.h>
#include <d3d11_3.h>
#include <d3dcompiler.h>
#if defined(DX11FSRBRIDGE_FG_DXGI_DIAGNOSTICS)
#include <d3d12.h>
#endif
#include <dxgi.h>
#include <dxgi1_2.h>
#if defined(DX11FSRBRIDGE_FG_DXGI_DIAGNOSTICS)
#include <intrin.h>
#endif

#include "Fsr31Bridge.h"
#if defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
#include "Fsr2TranslationLayer.h"
#endif
#if defined(DX11FSRBRIDGE_ENABLE_OPTISCALER_NGX_EXPERIMENTAL)
#include "OptiScalerNgxBridge.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cctype>
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
constexpr std::size_t k_idx_factory_create_swap_chain = 10;
constexpr std::size_t k_idx_factory2_create_swap_chain_for_hwnd = 15;

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
    int dlssg_dxgi_workaround = -1;
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
    bool enable_fsr31_context_probe = false;
    bool enable_fsr31_input_probe = false;
#if defined(DX11FSRBRIDGE_ENABLE_OPTISCALER_NGX_EXPERIMENTAL)
    bool enable_optiscaler_ngx_probe = false;
    bool enable_optiscaler_ngx_init_probe = false;
    bool enable_optiscaler_ngx_capability_probe = false;
    bool enable_optiscaler_ngx_delayed_init_probe = false;
#endif
#if defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
    bool enable_fsr2_get_proc_address_shim = false;
    std::uint32_t fsr2_translation_mode = 0;
    bool fsr2_fast_state_tracking = false;
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
    std::wstring kind;
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
std::atomic_bool g_logging_enabled = false;
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
#if defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
std::atomic_uint64_t g_fsr2_translation_dispatch_count = 0;
std::atomic_uint32_t g_fsr2_translation_failure_count = 0;
std::atomic_uint64_t g_mode2_last_takeover_tick = 0;
std::atomic_uint64_t g_mode2_native_bypass_start_tick = 0;
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
std::atomic_bool g_fsr31_probe_complete = false;
std::atomic_bool g_fsr31_input_probe_complete = false;
#if defined(DX11FSRBRIDGE_ENABLE_OPTISCALER_NGX_EXPERIMENTAL)
std::atomic_bool g_optiscaler_delayed_init_probe_started = false;
#endif
thread_local bool g_internal_bridge_dispatch = false;
std::mutex g_state_mutex;
std::atomic_bool g_active { false };
DispatchState g_state;
std::mutex g_color_source_mutex;
std::unordered_map<std::uint64_t, std::deque<ColorSourceWrite>> g_color_source_writes;
std::atomic_uint64_t g_color_source_sequence = 0;
std::string g_last_create_hook_scan;
std::string g_last_loader_hook_scan;
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

create_device_and_swapchain_fn g_original_create_device_and_swapchain = nullptr;
create_device_fn g_original_create_device = nullptr;
load_library_a_fn g_original_load_library_a = nullptr;
load_library_w_fn g_original_load_library_w = nullptr;
load_library_ex_a_fn g_original_load_library_ex_a = nullptr;
load_library_ex_w_fn g_original_load_library_ex_w = nullptr;
get_proc_address_fn g_original_get_proc_address = nullptr;
factory_create_swap_chain_fn g_original_factory_create_swap_chain = nullptr;
factory2_create_swap_chain_for_hwnd_fn g_original_factory2_create_swap_chain_for_hwnd = nullptr;
create_buffer_fn g_original_create_buffer = nullptr;
create_texture_2d_fn g_original_create_texture_2d = nullptr;
create_vertex_shader_fn g_original_create_vertex_shader = nullptr;
create_pixel_shader_fn g_original_create_pixel_shader = nullptr;
create_compute_shader_fn g_original_create_compute_shader = nullptr;
present_fn g_original_present = nullptr;
set_fullscreen_state_fn g_original_set_fullscreen_state = nullptr;
get_fullscreen_state_fn g_original_get_fullscreen_state = nullptr;
resize_buffers_fn g_original_resize_buffers = nullptr;
resize_target_fn g_original_resize_target = nullptr;
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

std::unordered_map<void *, void **> g_cloned_vtables;
std::unordered_map<void *, void **> g_original_vtables;

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
HRESULT STDMETHODCALLTYPE hooked_factory_create_swap_chain(IDXGIFactory *factory, IUnknown *device, DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **swapchain);
HRESULT STDMETHODCALLTYPE hooked_factory2_create_swap_chain_for_hwnd(IDXGIFactory2 *factory, IUnknown *device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1 *desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen_desc, IDXGIOutput *restrict_to_output, IDXGISwapChain1 **swapchain);
HRESULT STDMETHODCALLTYPE hooked_create_buffer(ID3D11Device *device, const D3D11_BUFFER_DESC *desc, const D3D11_SUBRESOURCE_DATA *initial_data, ID3D11Buffer **buffer);
HRESULT STDMETHODCALLTYPE hooked_create_texture_2d(ID3D11Device *device, const D3D11_TEXTURE2D_DESC *desc, const D3D11_SUBRESOURCE_DATA *initial_data, ID3D11Texture2D **texture);
HRESULT STDMETHODCALLTYPE hooked_create_vertex_shader(ID3D11Device *device, const void *shader_bytecode, SIZE_T bytecode_length, ID3D11ClassLinkage *class_linkage, ID3D11VertexShader **vertex_shader);
HRESULT STDMETHODCALLTYPE hooked_create_pixel_shader(ID3D11Device *device, const void *shader_bytecode, SIZE_T bytecode_length, ID3D11ClassLinkage *class_linkage, ID3D11PixelShader **pixel_shader);
HRESULT STDMETHODCALLTYPE hooked_create_compute_shader(ID3D11Device *device, const void *shader_bytecode, SIZE_T bytecode_length, ID3D11ClassLinkage *class_linkage, ID3D11ComputeShader **compute_shader);
void log_line(const std::string &line);
void set_osd_text(const std::wstring &text);
void start_osd();
void update_osd_from_dispatch(std::uint32_t phase, UINT group_x, UINT group_y, UINT group_z);
bool dlssg_framegen_selected();
bool dlssg_dxgi_workaround_active();

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
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    {
        std::lock_guard lock(g_state_mutex);
        output_width = g_state.backbuffer_width;
        output_height = g_state.backbuffer_height;
    }
    if (!g_config.fsr2_trace_color_producers ||
        !is_color_source_trace_surface(destination, output_width, output_height))
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
        update_osd_from_dispatch(phase, group_x, group_y, group_z);
    }
}

void log_line(const std::string &line)
{
    if (!g_logging_enabled.load(std::memory_order_relaxed))
        return;
#if defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    std::string lowered = line;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char value)
        { return static_cast<char>(std::tolower(value)); });
    static constexpr std::array<std::string_view, 12> error_terms {
        "failed",
        "failure",
        "error",
        "rejected",
        "unsupported",
        "missing",
        "blocked",
        "unavailable",
        "disabled",
        "exception",
        "mismatch",
        "invalid",
    };
    const bool is_error = std::any_of(error_terms.begin(), error_terms.end(), [&](std::string_view term)
        { return lowered.find(term) != std::string::npos; });
    if (!is_error)
        return;
#endif
    std::lock_guard lock(g_log_mutex);
    std::ofstream out(g_log_path, std::ios::app);
    SYSTEMTIME st {};
    GetLocalTime(&st);
    char prefix[64] {};
    std::snprintf(prefix, sizeof(prefix), "%04u-%02u-%02u %02u:%02u:%02u.%03u ",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    out << prefix << line << "\n";
}

#if defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
void truncate_log_file(const std::filesystem::path &path)
{
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
}

void reset_release_log_files()
{
    const std::filesystem::path payload_directory = g_module_dir.parent_path();
    truncate_log_file(g_log_path);
    truncate_log_file(payload_directory / L"OptiScaler" / L"OptiScaler.log");
    truncate_log_file(payload_directory / L"AntiPlayerMosaic.log");
}
#endif

#if defined(DX11FSRBRIDGE_FG_DXGI_DIAGNOSTICS)
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

LRESULT CALLBACK osd_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case WM_TIMER:
        poll_mode_hotkeys();
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
    g_config.dlssg_dxgi_workaround = GetPrivateProfileIntW(L"Dx11FsrBridge", L"DlssgDxgiWorkaround", -1, config_path.c_str());
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
    g_config.enable_fsr31_context_probe = GetPrivateProfileIntW(L"Dx11FsrBridge", L"EnableFsr31ContextProbe", 0, config_path.c_str()) != 0;
    g_config.enable_fsr31_input_probe = GetPrivateProfileIntW(L"Dx11FsrBridge", L"EnableFsr31InputProbe", 0, config_path.c_str()) != 0;
#if defined(DX11FSRBRIDGE_ENABLE_OPTISCALER_NGX_EXPERIMENTAL)
    g_config.enable_optiscaler_ngx_probe = GetPrivateProfileIntW(L"Dx11FsrBridge", L"EnableOptiScalerNgxProbe", 0, config_path.c_str()) != 0;
    g_config.enable_optiscaler_ngx_init_probe = GetPrivateProfileIntW(L"Dx11FsrBridge", L"EnableOptiScalerNgxInitProbe", 0, config_path.c_str()) != 0;
    g_config.enable_optiscaler_ngx_capability_probe = GetPrivateProfileIntW(L"Dx11FsrBridge", L"EnableOptiScalerNgxCapabilityProbe", 0, config_path.c_str()) != 0;
    g_config.enable_optiscaler_ngx_delayed_init_probe = GetPrivateProfileIntW(L"Dx11FsrBridge", L"EnableOptiScalerNgxDelayedInitProbe", 0, config_path.c_str()) != 0;
#endif
#if defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
    g_config.enable_fsr2_get_proc_address_shim = GetPrivateProfileIntW(L"Dx11FsrBridge", L"EnableFsr2GetProcAddressShim", 0, config_path.c_str()) != 0;
    g_config.fsr2_translation_mode = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2TranslationMode", 0, config_path.c_str()));
    g_config.fsr2_fast_state_tracking =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2FastStateTracking", 0, config_path.c_str()) != 0;
    g_config.fsr2_output_validation_target = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2OutputValidationTarget", 0, config_path.c_str()));
    g_config.fsr2_motion_vectors_jittered =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2MotionVectorsJittered", 0, config_path.c_str()) != 0;
    g_config.fsr2_positive_motion_vector_scale =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2MotionVectorScaleMode", 0, config_path.c_str()) == 1;
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
#if defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
    // This independent test build has one supported route: replace the native
    // 78057 TAAU draw in place and submit its first-hand inputs to OptiScaler.
    // It intentionally does not depend on a deployment INI or retain the
    // producer/late-color modes from the former mode 4 implementation.
    g_config.enable_logging = true;
    g_logging_enabled.store(true, std::memory_order_relaxed);
    g_config.enable_fsr2_get_proc_address_shim = true;
    g_config.fsr2_translation_mode = 2;
    g_config.fsr2_fast_state_tracking = false;
    g_config.fsr2_output_validation_target = 0;
    g_config.fsr2_motion_vectors_jittered = false;
    g_config.fsr2_positive_motion_vector_scale = false;
    g_config.fsr2_use_reactive_mask = true;
    g_config.fsr2_use_transparency_mask = false;
    g_config.fsr2_jitter_mode = 3;
    g_config.fsr2_dump_input_textures = 0;
    g_config.fsr2_compare_output_capture = false;
    g_config.fsr2_sharpness_percent = 0;
    g_config.fsr2_hdr10_pq_color = false;
    g_config.fsr2_use_native_exposure = false;
    g_config.fsr2_fast_metadata_copy = true;
    g_config.fsr2_gpu_timing = false;
    g_config.fsr2_reset_on_color_path_change = false;
    g_config.fsr2_reset_on_optiscaler_config_change = false;
    g_config.fsr2_reset_on_optiscaler_log_change = false;
    g_config.fsr2_auto_recover_upscaler_ms = 0;
    g_config.fsr2_trace_color_producers = false;
    g_config.fsr2_early_output_probe = false;
    g_config.block_dx11_on12_upscalers = false;
    g_config.show_osd = false;
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
            out_info.kind = kind;
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
    out_info.kind = kind;
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
    out << "hooks create_scan=" << g_last_create_hook_scan
        << " loader_scan=" << g_last_loader_hook_scan << "\n";

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
    if (summary != g_last_create_hook_scan)
    {
        g_last_create_hook_scan = summary;
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
    if (summary != g_last_loader_hook_scan)
    {
        g_last_loader_hook_scan = summary;
        if (g_config.log_loader_activity)
            log_line(summary);
    }
}

bool clone_and_patch_vtable(void *instance, std::size_t method_count, const std::vector<std::pair<std::size_t, void *>> &patches)
{
    if (instance == nullptr)
        return false;

    std::lock_guard lock(g_state_mutex);
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

    std::lock_guard lock(g_state_mutex);
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
    DXGI_SWAP_CHAIN_DESC desc {};
    if (SUCCEEDED(swapchain->GetDesc(&desc)))
    {
        std::lock_guard lock(g_state_mutex);
        g_state.backbuffer_width = desc.BufferDesc.Width;
        g_state.backbuffer_height = desc.BufferDesc.Height;
        g_state.frame_index++;
        g_state.candidate_count = 0;
    }

    static std::atomic_bool first_present_logged { false };
    if (!first_present_logged.exchange(true, std::memory_order_relaxed))
    {
        std::string message = "first_present size=" + std::to_string(g_state.backbuffer_width) + "x" +
            std::to_string(g_state.backbuffer_height);
#if defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
        message += " fsr2_query_mask=" + hex64(fsr2_get_proc_address_shim_query_mask());
#endif
        log_line(message);
    }
    return g_original_present(swapchain, sync_interval, flags);
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

    return g_original_resize_buffers != nullptr
        ? g_original_resize_buffers(swapchain, buffer_count, width, height, format, flags)
        : DXGI_ERROR_INVALID_CALL;
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

void STDMETHODCALLTYPE hooked_ps_set_shader_resources(ID3D11DeviceContext *context, UINT start_slot, UINT count, ID3D11ShaderResourceView *const *views)
{
#if defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
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
    {
        std::lock_guard lock(g_state_mutex);
        update_constant_buffer_array(g_state.vs_cbs, start_slot, count, buffers);
    }
    g_original_vs_set_constant_buffers(context, start_slot, count, buffers);
}

void STDMETHODCALLTYPE hooked_ps_set_shader(ID3D11DeviceContext *context, ID3D11PixelShader *shader, ID3D11ClassInstance *const *class_instances, UINT class_instances_count)
{
#if defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
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
    {
        std::lock_guard lock(g_state_mutex);
        update_view_array(g_state.cs_srvs, start_slot, count, views, L"cs_srv");
    }
    g_original_cs_set_shader_resources(context, start_slot, count, views);
}

void STDMETHODCALLTYPE hooked_cs_set_uavs(ID3D11DeviceContext *context, UINT start_slot, UINT count, ID3D11UnorderedAccessView *const *views, const UINT *initial_counts)
{
    {
        std::lock_guard lock(g_state_mutex);
        update_uav_array(g_state.cs_uavs, start_slot, count, views);
    }
    g_original_cs_set_uavs(context, start_slot, count, views, initial_counts);
}

void STDMETHODCALLTYPE hooked_cs_set_shader(ID3D11DeviceContext *context, ID3D11ComputeShader *shader, ID3D11ClassInstance *const *class_instances, UINT class_instances_count)
{
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
    {
        std::lock_guard lock(g_state_mutex);
        update_constant_buffer_array(g_state.cs_cbs, start_slot, count, buffers);
    }
    g_original_cs_set_constant_buffers(context, start_slot, count, buffers);
}

void STDMETHODCALLTYPE hooked_om_set_render_targets(ID3D11DeviceContext *context, UINT count, ID3D11RenderTargetView *const *rtvs, ID3D11DepthStencilView *dsv)
{
#if defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    if (g_config.fsr2_fast_state_tracking && g_config.fsr2_translation_mode == 2 &&
        g_mode2_fast_target_ps_hash.load(std::memory_order_relaxed) != 0)
    {
        g_original_om_set_render_targets(context, count, rtvs, dsv);
        return;
    }
#endif
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
    }
    g_original_om_set_render_targets(context, count, rtvs, dsv);
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

    record_similarity_dispatch(group_x, group_y, group_z);
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
    DXGI_FORMAT color_format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT output_color_format = DXGI_FORMAT_UNKNOWN;
};

enum class Mode2ColorContract : std::uint32_t
{
    unsupported = 0,
    sdr_srgb = 1,
    hdr10_pq = 2,
};

bool is_mode2_sdr_format(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
        format == DXGI_FORMAT_R8G8B8A8_UNORM ||
        format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
        format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
        format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

bool is_mode2_hdr10_format(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_R10G10B10A2_TYPELESS ||
        format == DXGI_FORMAT_R10G10B10A2_UNORM;
}

Mode2ColorContract classify_mode2_color_contract(const TargetUpscalerDrawInfo &draw_info)
{
    if (is_mode2_sdr_format(draw_info.color_format) &&
        is_mode2_sdr_format(draw_info.output_color_format))
    {
        return Mode2ColorContract::sdr_srgb;
    }
    if (is_mode2_hdr10_format(draw_info.color_format) &&
        is_mode2_hdr10_format(draw_info.output_color_format))
    {
        return Mode2ColorContract::hdr10_pq;
    }
    return Mode2ColorContract::unsupported;
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

    const ResourceInfo &color = g_state.ps_srvs[0];
    const ResourceInfo &weights = g_state.ps_srvs[1];
    const ResourceInfo &depth = g_state.ps_srvs[2];
    const ResourceInfo &motion = g_state.ps_srvs[3];
    const ResourceInfo &flags = g_state.ps_srvs[4];
    const ResourceInfo &history_metadata = g_state.ps_srvs[5];
    const ResourceInfo &history_color = g_state.ps_srvs[6];
    const ResourceInfo &output_metadata = g_state.rtvs[0];
    const ResourceInfo &output_color = g_state.rtvs[1];

    if (color.resource_key == 0 || weights.resource_key == 0 || depth.resource_key == 0 ||
        motion.resource_key == 0 || flags.resource_key == 0 || history_metadata.resource_key == 0 ||
        history_color.resource_key == 0 || output_metadata.resource_key == 0 || output_color.resource_key == 0)
        return std::nullopt;

    const bool input_dimensions_match =
        depth.width == color.width && depth.height == color.height &&
        motion.width == color.width && motion.height == color.height &&
        flags.width == color.width && flags.height == color.height;
    const bool output_dimensions_match =
        output_metadata.width == output_color.width && output_metadata.height == output_color.height &&
        history_metadata.width == output_color.width && history_metadata.height == output_color.height &&
        history_color.width == output_color.width && history_color.height == output_color.height &&
        g_state.viewport_width == output_color.width && g_state.viewport_height == output_color.height;

    if (!input_dimensions_match || !output_dimensions_match ||
        weights.width != 16 || weights.height != 16 ||
        color.width > output_color.width || color.height > output_color.height ||
        g_state.ps_cbs[0].byte_width < 464)
        return std::nullopt;

    if (g_state.current_ps_hash != g_config.target_pixel_shader_hash)
    {
        static std::atomic_bool fallback_shader_logged { false };
        if (!fallback_shader_logged.exchange(true, std::memory_order_relaxed))
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

    return TargetUpscalerDrawInfo {
        color.width,
        color.height,
        output_color.width,
        output_color.height,
        g_state.ps_cbs[0].resource_key,
        color.resource_key,
        motion.resource_key,
        color.format,
        output_color.format,
    };
}

std::optional<TargetUpscalerDrawInfo> inspect_target_upscaler_draw(
    ID3D11DeviceContext *context,
    UINT element_count)
{
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

        const ResourceInfo &color = inputs[0];
        const ResourceInfo &weights = inputs[1];
        const ResourceInfo &depth = inputs[2];
        const ResourceInfo &motion = inputs[3];
        const ResourceInfo &flags = inputs[4];
        const ResourceInfo &history_metadata = inputs[5];
        const ResourceInfo &history_color = inputs[6];
        const ResourceInfo &output_metadata = outputs[0];
        const ResourceInfo &output_color = outputs[1];
        const bool resources_present =
            color.resource_key != 0 && weights.resource_key != 0 && depth.resource_key != 0 &&
            motion.resource_key != 0 && flags.resource_key != 0 && history_metadata.resource_key != 0 &&
            history_color.resource_key != 0 && output_metadata.resource_key != 0 &&
            output_color.resource_key != 0;
        const bool input_dimensions_match =
            depth.width == color.width && depth.height == color.height &&
            motion.width == color.width && motion.height == color.height &&
            flags.width == color.width && flags.height == color.height;
        const bool output_dimensions_match =
            output_metadata.width == output_color.width && output_metadata.height == output_color.height &&
            history_metadata.width == output_color.width && history_metadata.height == output_color.height &&
            history_color.width == output_color.width && history_color.height == output_color.height &&
            viewport_count != 0 && static_cast<std::uint32_t>(viewport.Width) == output_color.width &&
            static_cast<std::uint32_t>(viewport.Height) == output_color.height;
        if (!resources_present || !input_dimensions_match || !output_dimensions_match ||
            weights.width != 16 || weights.height != 16 ||
            color.width > output_color.width || color.height > output_color.height ||
            constant_buffer_description.ByteWidth < 464)
        {
            return std::nullopt;
        }

        g_trace_ps_cb0_key.store(constant_buffer_key, std::memory_order_relaxed);
        return TargetUpscalerDrawInfo {
            color.width,
            color.height,
            output_color.width,
            output_color.height,
            constant_buffer_key,
            color.resource_key,
            motion.resource_key,
            color.format,
            output_color.format,
        };
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

std::pair<float, float> target_jitter_pixels(const TargetUpscalerDrawInfo &draw_info)
{
    constexpr std::size_t jitter_offset = 28 * sizeof(float) * 4;
    const std::vector<std::uint8_t> snapshot = lookup_buffer_snapshot(draw_info.constant_buffer_key);
    if (snapshot.size() < jitter_offset + sizeof(float) * 2)
        return {};

    float normalized_jitter[2] {};
    std::memcpy(normalized_jitter, snapshot.data() + jitter_offset, sizeof(normalized_jitter));
    if (!std::isfinite(normalized_jitter[0]) || !std::isfinite(normalized_jitter[1]) ||
        std::abs(normalized_jitter[0]) > 1.0f || std::abs(normalized_jitter[1]) > 1.0f)
    {
        return {};
    }

    float jitter_x = normalized_jitter[0] * static_cast<float>(draw_info.render_width) - 0.5f;
    float jitter_y = normalized_jitter[1] * static_cast<float>(draw_info.render_height) - 0.5f;
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
    return { jitter_x, jitter_y };
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

void maybe_dispatch_early_output_probe(ID3D11DeviceContext *context, UINT element_count)
{
    if (context == nullptr || g_fsr2_early_output_probe_frames_remaining.load(std::memory_order_acquire) == 0)
        return;

    const auto draw_info = inspect_target_upscaler_draw(element_count);
    if (!draw_info)
        return;

    std::uint32_t remaining_before = g_fsr2_early_output_probe_frames_remaining.load(std::memory_order_relaxed);
    while (remaining_before != 0 &&
        !g_fsr2_early_output_probe_frames_remaining.compare_exchange_weak(
            remaining_before,
            remaining_before - 1,
            std::memory_order_acq_rel,
            std::memory_order_relaxed))
    {
    }
    if (remaining_before == 0)
        return;

    const std::uint32_t probe_frame = g_config.fsr2_early_output_probe_frames - remaining_before + 1;
    const bool final_probe_frame = remaining_before == 1;

    if (unsafe_dx11_on12_backend_selected())
    {
        g_fsr2_early_output_probe_frames_remaining.store(0, std::memory_order_relaxed);
        log_line("fsr2_early_output_probe_blocked unsafe_dx11_on12_backend=1");
        return;
    }

    ID3D11ShaderResourceView *raw_color = acquire_fsr2_candidate_color_view();
    std::array<ID3D11ShaderResourceView *, 5> views {};
    std::array<ID3D11RenderTargetView *, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> render_targets {};
    ID3D11DepthStencilView *depth_stencil = nullptr;
    context->PSGetShaderResources(0, static_cast<UINT>(views.size()), views.data());
    context->OMGetRenderTargets(static_cast<UINT>(render_targets.size()), render_targets.data(), &depth_stencil);

    ID3D11Device *device = nullptr;
    context->GetDevice(&device);
    D3D11_TEXTURE2D_DESC output_desc {};
    output_desc.Width = draw_info->output_width;
    output_desc.Height = draw_info->output_height;
    output_desc.MipLevels = 1;
    output_desc.ArraySize = 1;
    output_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    output_desc.SampleDesc.Count = 1;
    output_desc.Usage = D3D11_USAGE_DEFAULT;
    output_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ID3D11Texture2D *probe_output = nullptr;
    HRESULT create_result = device != nullptr ? device->CreateTexture2D(&output_desc, nullptr, &probe_output) : E_POINTER;
    if (device != nullptr)
        device->Release();

    if (raw_color == nullptr || views[2] == nullptr || views[3] == nullptr ||
        FAILED(create_result) || probe_output == nullptr)
    {
        log_line("fsr2_early_output_probe_create_failed hr=" +
            std::to_string(static_cast<long>(create_result)));
    }
    else
    {
        const auto [jitter_x, jitter_y] = target_jitter_pixels(*draw_info);
        Fsr2TranslationFrame frame;
        frame.context = context;
        frame.color = raw_color;
        frame.depth = views[2];
        frame.motion = views[3];
        frame.output = probe_output;
        frame.render_width = draw_info->render_width;
        frame.render_height = draw_info->render_height;
        frame.output_width = draw_info->output_width;
        frame.output_height = draw_info->output_height;
        frame.jitter_x = jitter_x;
        frame.jitter_y = jitter_y;
        frame.motion_vectors_jittered = g_config.fsr2_motion_vectors_jittered;
        frame.positive_motion_vector_scale = g_config.fsr2_positive_motion_vector_scale;

        if (g_original_om_set_render_targets != nullptr)
            g_original_om_set_render_targets(context, 0, nullptr, nullptr);
        else
            context->OMSetRenderTargets(0, nullptr, nullptr);

        g_internal_bridge_dispatch = true;
        Fsr2TranslationOutcome outcome;
        {
            ScopedContextVtableBypass context_vtable_bypass(context);
            outcome = dispatch_fsr2_translation(frame);
        }
        g_internal_bridge_dispatch = false;

        if (g_original_om_set_render_targets != nullptr)
            g_original_om_set_render_targets(
                context,
                static_cast<UINT>(render_targets.size()),
                render_targets.data(),
                depth_stencil);
        else
            context->OMSetRenderTargets(
                static_cast<UINT>(render_targets.size()),
                render_targets.data(),
                depth_stencil);

        if (outcome.succeeded && final_probe_frame)
        {
            ID3D11ShaderResourceView *probe_view = nullptr;
            ID3D11Device *output_device = nullptr;
            probe_output->GetDevice(&output_device);
            const HRESULT actual_view_result = output_device != nullptr
                ? output_device->CreateShaderResourceView(probe_output, nullptr, &probe_view)
                : E_POINTER;
            if (output_device != nullptr)
                output_device->Release();
            if (SUCCEEDED(actual_view_result) && probe_view != nullptr)
            {
                dump_fsr2_input_texture(context, probe_view, 7);
                probe_view->Release();
            }
        }

        if (probe_frame == 1 || final_probe_frame || !outcome.succeeded)
        {
            log_line("fsr2_early_output_probe_frame index=" + std::to_string(probe_frame) +
                " total=" + std::to_string(g_config.fsr2_early_output_probe_frames) +
                " succeeded=" + std::to_string(outcome.succeeded ? 1 : 0) +
                " hook=" + std::to_string(outcome.hook_entry_detected ? 1 : 0) +
                " code=" + std::to_string(outcome.error_code) +
                " error=" + outcome.error);
        }
    }

    if (raw_color != nullptr)
        raw_color->Release();
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
    if (probe_output != nullptr)
        probe_output->Release();
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

    if (g_fsr2_translation_recovery_requested.exchange(false, std::memory_order_acq_rel))
    {
        reset_fsr2_translation_context();
        log_line("fsr2_translation_context_reset reason=upscaler_stall");
    }

    if (unsafe_dx11_on12_backend_selected())
    {
        if (!g_fsr2_dx11on12_block_logged.exchange(true, std::memory_order_relaxed))
            log_line("fsr2_translation_blocked unsafe_dx11_on12_backend=1 fallback=original_draw");
        return false;
    }
    g_fsr2_dx11on12_block_logged.store(false, std::memory_order_relaxed);

    const auto draw_info = inspected_draw_info
        ? inspected_draw_info
        : inspect_target_upscaler_draw(context, element_count);
    if (!draw_info)
        return false;
    const bool native_upscaling_enabled =
        draw_info->render_width < draw_info->output_width &&
        draw_info->render_height < draw_info->output_height;
    if (!native_upscaling_enabled)
    {
        const std::uint64_t now_tick = GetTickCount64();
        std::uint64_t no_bypass_started = 0;
        g_mode2_native_bypass_start_tick.compare_exchange_strong(
            no_bypass_started,
            now_tick,
            std::memory_order_acq_rel);
        static std::atomic_bool initial_bypass_logged { false };
        if (!initial_bypass_logged.exchange(true, std::memory_order_relaxed))
        {
            log_line("mode2_bypass route=native_taau reason=not_upscaling render=" +
                std::to_string(draw_info->render_width) + "x" +
                    std::to_string(draw_info->render_height) +
                " output=" + std::to_string(draw_info->output_width) + "x" +
                    std::to_string(draw_info->output_height) +
                " context_policy=keep_resident");
        }
        return false;
    }

    const Mode2ColorContract color_contract = classify_mode2_color_contract(*draw_info);
    if (color_contract == Mode2ColorContract::unsupported)
    {
        const std::uint64_t now_tick = GetTickCount64();
        std::uint64_t no_bypass_started = 0;
        g_mode2_native_bypass_start_tick.compare_exchange_strong(
            no_bypass_started,
            now_tick,
            std::memory_order_acq_rel);
        static std::atomic_uint64_t rejected_contracts { 0 };
        const std::uint64_t count = rejected_contracts.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count == 1 || count % 600 == 0)
        {
            log_line("mode2_target_rejected reason=unsupported_color_contract count=" +
                std::to_string(count) +
                " input_format=" + format_string(draw_info->color_format) +
                " output_format=" + format_string(draw_info->output_color_format) +
                " native_draw_skipped=1");
        }
        return true;
    }
    const bool hdr10_pq = color_contract == Mode2ColorContract::hdr10_pq;
    const std::uint64_t now_tick = GetTickCount64();
    const std::uint64_t previous_takeover_tick =
        g_mode2_last_takeover_tick.exchange(now_tick, std::memory_order_acq_rel);
    const std::uint64_t native_bypass_start_tick =
        g_mode2_native_bypass_start_tick.exchange(0, std::memory_order_acq_rel);
    const bool entering_takeover = previous_takeover_tick == 0;
    const bool resume_after_native_bypass =
        native_bypass_start_tick != 0 && now_tick >= native_bypass_start_tick + 100;
    const bool takeover_history_gap =
        previous_takeover_tick != 0 && now_tick >= previous_takeover_tick + 1000;
    if (entering_takeover)
    {
        log_line("mode2_route_switch route=optiscaler reason=native_upscaling history_reset=1");
    }
    else if (resume_after_native_bypass)
    {
        log_line("mode2_history_reset reason=resume_after_native_bypass bypass_ms=" +
            std::to_string(now_tick - native_bypass_start_tick) +
            " context_policy=keep_resident");
    }
    else if (takeover_history_gap)
    {
        log_line("mode2_history_reset reason=takeover_gap gap_ms=" +
            std::to_string(now_tick - previous_takeover_tick));
    }
    static std::atomic_uint32_t last_color_contract { 0 };
    const std::uint32_t current_color_contract = static_cast<std::uint32_t>(color_contract);
    const std::uint32_t previous_color_contract =
        last_color_contract.exchange(current_color_contract, std::memory_order_acq_rel);
    const bool display_contract_changed =
        previous_color_contract != 0 && previous_color_contract != current_color_contract;
    if (previous_color_contract != current_color_contract)
    {
        log_line("mode2_color_contract path=" +
            std::string(hdr10_pq ? "hdr10_pq" : "sdr_srgb") +
            " input_format=" + format_string(draw_info->color_format) +
            " output_format=" + format_string(draw_info->output_color_format) +
            " render=" + std::to_string(draw_info->render_width) + "x" +
                std::to_string(draw_info->render_height) +
            " output=" + std::to_string(draw_info->output_width) + "x" +
                std::to_string(draw_info->output_height) +
            " contract_switch=" + std::to_string(display_contract_changed ? 1 : 0) +
            " shim_query_mask=" + hex64(fsr2_get_proc_address_shim_query_mask()));
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
    if (views[0] == nullptr || views[2] == nullptr || views[3] == nullptr ||
        (g_config.fsr2_use_reactive_mask && views[4] == nullptr) || render_targets[1] == nullptr)
    {
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
        static std::atomic_bool missing_binding_logged { false };
        if (!missing_binding_logged.exchange(true, std::memory_order_relaxed))
            log_line("mode2_dispatch_failed reason=missing_native_binding native_draw_skipped=1");
        return true;
    }
    render_targets[1]->GetResource(&output);

    ID3D11ShaderResourceView *translation_color = views[0];
    ID3D11ShaderResourceView *early_color = nullptr;
    ID3D11ShaderResourceView *exposure = nullptr;
    ID3D11Resource *color_replay_output = nullptr;
    ID3D11ShaderResourceView *color_replay_output_view = nullptr;
    ID3D11Resource *translation_output = output;
    if (translation_mode >= 3 && !use_late_composed_color)
    {
        early_color = acquire_fsr2_candidate_color_view(
            draw_info->color_resource_key, producer_generation);
        ResourceInfo early_color_info {};
        const bool early_color_matches =
            read_resource_info(early_color, L"fsr2_early_color", early_color_info) &&
            early_color_info.width == draw_info->render_width &&
            early_color_info.height == draw_info->render_height;
        if (!early_color_matches)
        {
            if (output != nullptr)
                output->Release();
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
        translation_color = early_color;
    }
    if (translation_mode >= 4 && !use_late_composed_color)
    {
        exposure = acquire_fsr2_color_replay_exposure_view(
            draw_info->color_resource_key,
            producer_generation,
            draw_info->render_width,
            draw_info->render_height);
        if (g_config.fsr2_use_native_exposure && exposure == nullptr)
        {
            if (output != nullptr)
                output->Release();
            if (early_color != nullptr)
                early_color->Release();
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
    }
    if (translation_mode >= 4 && use_late_composed_color)
    {
        exposure = acquire_fsr2_neutral_exposure_view(context);
        if (exposure == nullptr)
        {
            if (output != nullptr)
                output->Release();
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
    }
    if (translation_mode >= 4 && !use_late_composed_color)
    {
        if (!acquire_fsr2_color_replay_output(
                context,
                draw_info->output_width,
                draw_info->output_height,
                g_config.fsr2_compact_linear_output
                    ? DXGI_FORMAT_R11G11B10_FLOAT
                    : DXGI_FORMAT_R16G16B16A16_FLOAT,
                &color_replay_output,
                &color_replay_output_view))
        {
            if (output != nullptr)
                output->Release();
            if (early_color != nullptr)
                early_color->Release();
            if (exposure != nullptr)
                exposure->Release();
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
        translation_output = color_replay_output;
    }

#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    const bool input_dump_requested =
        g_config.fsr2_dump_input_textures == 1 ||
        (g_config.fsr2_dump_input_textures >= 2 && (GetAsyncKeyState(VK_F6) & 0x8000) != 0);
    if (input_dump_requested &&
        !g_fsr2_input_textures_dumped.exchange(true, std::memory_order_relaxed))
    {
        for (UINT slot = 0; slot < views.size(); ++slot)
            dump_fsr2_input_texture(context, views[slot], slot);
        dump_fsr2_constant_buffer(draw_info->constant_buffer_key);
    }
#endif

    const auto [jitter_x, jitter_y] = target_jitter_pixels(*draw_info);
    Fsr2TranslationFrame frame;
    frame.context = context;
    frame.color = translation_color;
    frame.depth = views[2];
    frame.motion = views[3];
    frame.flags = views[4];
    frame.exposure = use_late_composed_color
        ? exposure
        : (g_config.fsr2_use_native_exposure ? exposure : nullptr);
    frame.output = translation_output;
    frame.render_width = draw_info->render_width;
    frame.render_height = draw_info->render_height;
    frame.output_width = draw_info->output_width;
    frame.output_height = draw_info->output_height;
    frame.jitter_x = jitter_x;
    frame.jitter_y = jitter_y;
    frame.motion_vectors_jittered = g_config.fsr2_motion_vectors_jittered;
    frame.positive_motion_vector_scale = g_config.fsr2_positive_motion_vector_scale;
    frame.use_reactive_mask = g_config.fsr2_use_reactive_mask;
    frame.use_transparency_mask = g_config.fsr2_use_transparency_mask;
    frame.enable_sharpening = g_config.fsr2_sharpness_percent > 0;
    frame.sharpness = static_cast<float>(g_config.fsr2_sharpness_percent) / 100.0f;
    frame.hdr10_pq_color = hdr10_pq;
    frame.use_direct_linear_color = !hdr10_pq;
    const bool optiscaler_config_reset = consume_fsr2_optiscaler_config_reset();
    const bool optiscaler_log_reset = should_reset_for_optiscaler_log_activity();
    frame.reset =
        entering_takeover ||
        resume_after_native_bypass ||
        takeover_history_gap ||
        display_contract_changed ||
        (color_path_changed && g_config.fsr2_reset_on_color_path_change) ||
        optiscaler_config_reset ||
        optiscaler_log_reset;
    Fsr2GpuTimingSlot *gpu_timing_slot = begin_fsr2_gpu_timing(context);
    frame.gpu_timestamp_after_prepare =
        gpu_timing_slot != nullptr ? gpu_timing_slot->timestamps[1] : nullptr;

#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    const bool input_sequence_key_down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    const bool input_sequence_was_down = g_fsr2_input_sequence_key_down.exchange(
        input_sequence_key_down,
        std::memory_order_relaxed);
    if (input_sequence_key_down && !input_sequence_was_down)
    {
        g_fsr2_input_sequence_frames_remaining.store(8, std::memory_order_release);
        log_line("fsr2_input_sequence_capture_started frames=8 hotkey=F7");
    }
    std::uint32_t input_sequence_remaining =
        g_fsr2_input_sequence_frames_remaining.load(std::memory_order_acquire);
    if (input_sequence_remaining > 0 &&
        g_fsr2_input_sequence_frames_remaining.compare_exchange_strong(
            input_sequence_remaining,
            input_sequence_remaining - 1,
            std::memory_order_acq_rel))
    {
        const std::uint32_t capture_index =
            g_fsr2_input_sequence_capture_index.fetch_add(1, std::memory_order_relaxed) + 1;
        std::wostringstream capture_stem;
        capture_stem << L"fsr_input_" << GetCurrentProcessId() << L"_"
            << std::setw(4) << std::setfill(L'0') << capture_index;
        const bool captured = dump_fsr2_input_texture(
            context,
            translation_color,
            0,
            capture_stem.str());
        const std::filesystem::path metadata_path =
            g_module_dir / L"Dx11FsrBridge.inputs" / (capture_stem.str() + L".frame.json");
        std::ofstream metadata(metadata_path, std::ios::trunc);
        if (metadata)
        {
            metadata << "{\"capture_index\":" << capture_index
                << ",\"jitter_pixels\":[" << jitter_x << "," << jitter_y << "]"
                << ",\"jitter_mode\":" << g_config.fsr2_jitter_mode
                << ",\"render_size\":[" << draw_info->render_width << "," << draw_info->render_height << "]"
                << ",\"output_size\":[" << draw_info->output_width << "," << draw_info->output_height << "]} ";
        }
        log_line("fsr2_input_sequence_capture index=" + std::to_string(capture_index) +
            " captured=" + std::to_string(captured ? 1 : 0) +
            " remaining=" + std::to_string(input_sequence_remaining - 1) +
            " jitter=" + std::to_string(jitter_x) + "," + std::to_string(jitter_y));
    }
#endif

    static constexpr float rtv0_validation_color[] { 0.0f, 1.0f, 0.0f, 1.0f };
    static constexpr float rtv1_validation_color[] { 1.0f, 0.0f, 1.0f, 1.0f };
    const auto clear_validation_target = [&](ID3D11RenderTargetView *render_target, const float color[4])
    {
        if (render_target == nullptr)
            return;
        if (g_original_clear_rtv != nullptr)
            g_original_clear_rtv(context, render_target, color);
        else
            context->ClearRenderTargetView(render_target, color);
    };

    if ((g_config.fsr2_output_validation_target & 4u) != 0)
    {
        clear_validation_target(render_targets[1], rtv1_validation_color);
        if (!g_fsr2_output_validation_logged.exchange(true, std::memory_order_relaxed))
        {
            log_line("fsr2_output_validation_active target=" +
                std::to_string(g_config.fsr2_output_validation_target) +
                " phase=pre_dispatch" +
                " rtv1=" + hex64(reinterpret_cast<std::uint64_t>(render_targets[1])) +
                " output_resource=" + hex64(reinterpret_cast<std::uint64_t>(output)));
        }
    }

    if (g_original_om_set_render_targets != nullptr)
        g_original_om_set_render_targets(context, 0, nullptr, nullptr);
    else
        context->OMSetRenderTargets(0, nullptr, nullptr);

    g_internal_bridge_dispatch = true;
    Fsr2TranslationOutcome outcome;
    {
        ScopedContextVtableBypass context_vtable_bypass(context);
        outcome = dispatch_fsr2_translation(frame);
    }
    g_internal_bridge_dispatch = false;
    if (gpu_timing_slot != nullptr)
    {
        if (!outcome.inputs_prepared)
            context->End(gpu_timing_slot->timestamps[1]);
        context->End(gpu_timing_slot->timestamps[2]);
    }

    if (g_original_om_set_render_targets != nullptr)
    {
        g_original_om_set_render_targets(
            context,
            static_cast<UINT>(render_targets.size()),
            render_targets.data(),
            depth_stencil);
    }
    else
    {
        context->OMSetRenderTargets(
            static_cast<UINT>(render_targets.size()),
            render_targets.data(),
            depth_stencil);
    }

    bool color_replayed = false;
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    const bool pre_color_capture_key_down = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    const bool pre_color_capture_was_down = g_fsr2_pre_color_capture_key_down.exchange(
        pre_color_capture_key_down,
        std::memory_order_relaxed);
    const bool pre_color_capture_requested =
        pre_color_capture_key_down && !pre_color_capture_was_down;
    if (outcome.succeeded && outcome.hook_entry_detected && color_replay_output_view != nullptr &&
        pre_color_capture_requested)
    {
        const std::uint32_t capture_index =
            g_fsr2_pre_color_capture_index.fetch_add(1, std::memory_order_relaxed) + 1;
        std::wostringstream capture_stem;
        capture_stem << L"pre_color_" << GetCurrentProcessId() << L"_"
            << std::setw(4) << std::setfill(L'0') << capture_index;
        const bool captured = dump_fsr2_input_texture(
            context,
            color_replay_output_view,
            7,
            capture_stem.str());
        log_line("fsr2_pre_color_capture index=" + std::to_string(capture_index) +
            " captured=" + std::to_string(captured ? 1 : 0) +
            " hotkey=F5 stage=after_optiscaler_before_color_replay");
    }
    if (g_fsr2_transient_capture_snapshot && color_replay_output_view != nullptr)
    {
        std::wostringstream stem;
        stem << L"transient_" << GetCurrentProcessId() << L"_s"
            << g_fsr2_transient_capture_current_session << L"_"
            << std::setw(3) << std::setfill(L'0') << g_fsr2_transient_capture_current_sample
            << L"_opti";
        dump_fsr2_input_texture(context, color_replay_output_view, 7, stem.str());
    }
#endif
    bool metadata_updated = false;
    if (outcome.succeeded && outcome.hook_entry_detected && g_config.fsr2_fast_metadata_copy &&
        translation_mode >= 2 && render_targets[0] != nullptr)
    {
        metadata_updated = copy_fsr2_history_metadata(
            context,
            views[5],
            render_targets[0],
            render_targets,
            depth_stencil);
        if (!metadata_updated)
        {
            static std::atomic_bool failure_logged { false };
            if (!failure_logged.exchange(true, std::memory_order_relaxed))
                log_line("fsr2_fast_metadata_copy_failed native_draw_skipped=1");
        }
    }
    if (outcome.succeeded && outcome.hook_entry_detected && !metadata_updated &&
        translation_mode >= 4 && render_targets[0] != nullptr)
    {
        if (g_original_om_set_render_targets != nullptr)
            g_original_om_set_render_targets(context, 1, render_targets.data(), depth_stencil);
        else
            context->OMSetRenderTargets(1, render_targets.data(), depth_stencil);
        std::forward<DrawCall>(draw_call)();
        if (g_original_om_set_render_targets != nullptr)
        {
            g_original_om_set_render_targets(
                context,
                static_cast<UINT>(render_targets.size()),
                render_targets.data(),
                depth_stencil);
        }
        else
        {
            context->OMSetRenderTargets(
                static_cast<UINT>(render_targets.size()),
                render_targets.data(),
                depth_stencil);
        }
    }
    if (gpu_timing_slot != nullptr)
        context->End(gpu_timing_slot->timestamps[3]);
    if (outcome.succeeded && outcome.hook_entry_detected && translation_mode >= 4 &&
        !use_late_composed_color)
    {
        color_replayed = replay_fsr2_color_processing(
            context,
            color_replay_output_view,
            render_targets[1],
            render_targets,
            depth_stencil,
            draw_info->color_resource_key,
            producer_generation,
            draw_info->render_width,
            draw_info->render_height,
            draw_info->output_width,
            draw_info->output_height,
            std::forward<DrawCall>(draw_call));
    }
    end_fsr2_gpu_timing(context, gpu_timing_slot);
    const bool skip_original_draw = translation_mode == 2 ||
        (outcome.succeeded && outcome.hook_entry_detected &&
            (use_late_composed_color || color_replayed));
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    if (g_fsr2_transient_capture_snapshot && render_targets[1] != nullptr)
    {
        std::wostringstream stem;
        stem << L"transient_" << GetCurrentProcessId() << L"_s"
            << g_fsr2_transient_capture_current_session << L"_"
            << std::setw(3) << std::setfill(L'0') << g_fsr2_transient_capture_current_sample
            << L"_final";
        dump_fsr2_render_target_texture(context, render_targets[1], 8, stem.str());
    }
    record_fsr2_transient_capture_result(
        outcome.succeeded,
        outcome.hook_entry_detected,
        color_replayed,
        skip_original_draw,
        outcome.error_code,
        outcome.error);
    const bool compare_output_capture =
        g_config.fsr2_compare_output_capture && color_replayed &&
        (GetAsyncKeyState(VK_F6) & 0x8000) != 0 &&
        !g_fsr2_output_pair_dumped.exchange(true, std::memory_order_relaxed);
    if (compare_output_capture)
    {
        const bool bridge_dumped = dump_fsr2_render_target_texture(context, render_targets[1], 8);
        std::forward<DrawCall>(draw_call)();
        const bool original_dumped = dump_fsr2_render_target_texture(context, render_targets[1], 9);
        log_line("fsr2_output_pair_dumped bridge=" + std::to_string(bridge_dumped ? 1 : 0) +
            " original=" + std::to_string(original_dumped ? 1 : 0) +
            " bridge_slot=8 original_slot=9");
    }
#endif
    if (outcome.succeeded && (g_config.fsr2_output_validation_target & 3u) != 0)
    {
        if ((g_config.fsr2_output_validation_target & 1u) != 0)
            clear_validation_target(render_targets[1], rtv1_validation_color);
        if ((g_config.fsr2_output_validation_target & 2u) != 0)
            clear_validation_target(render_targets[0], rtv0_validation_color);

        if (!g_fsr2_output_validation_logged.exchange(true, std::memory_order_relaxed))
        {
            log_line("fsr2_output_validation_active target=" +
                std::to_string(g_config.fsr2_output_validation_target) +
                " phase=post_dispatch" +
                " rtv0=" + hex64(reinterpret_cast<std::uint64_t>(render_targets[0])) +
                " rtv1=" + hex64(reinterpret_cast<std::uint64_t>(render_targets[1])) +
                " output_resource=" + hex64(reinterpret_cast<std::uint64_t>(output)));
        }
    }

    if (output != nullptr)
        output->Release();
    if (color_replay_output != nullptr)
        color_replay_output->Release();
    if (color_replay_output_view != nullptr)
        color_replay_output_view->Release();
    if (early_color != nullptr)
        early_color->Release();
    if (exposure != nullptr)
        exposure->Release();
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

    if (outcome.context_created)
    {
        log_line("fsr2_translation_context_created render=" +
            std::to_string(draw_info->render_width) + "x" + std::to_string(draw_info->render_height) +
            " output=" + std::to_string(draw_info->output_width) + "x" + std::to_string(draw_info->output_height) +
            " detoured=" + (outcome.hook_entry_detected ? std::string("1") : std::string("0")));
    }
    if (!outcome.succeeded)
    {
        const std::uint32_t failure_index = g_fsr2_translation_failure_count.fetch_add(1, std::memory_order_relaxed);
        if (failure_index < 8)
        {
            log_line("fsr2_translation_dispatch_failed code=" + std::to_string(outcome.error_code) +
                " error=" + outcome.error);
        }
        return translation_mode == 2;
    }

#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    const std::uint64_t dispatch_index =
        g_fsr2_translation_dispatch_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (dispatch_index == 1 || dispatch_index % 1024 == 0)
    {
        log_line("fsr2_translation_dispatch_succeeded count=" + std::to_string(dispatch_index) +
            " mode=" + std::to_string(translation_mode) +
            " input_source=" + (use_late_composed_color
                ? std::string("late_composed")
                : (translation_mode >= 3 ? std::string("early_raw") : std::string("late_color"))) +
            " color_replayed=" + (color_replayed ? std::string("1") : std::string("0")) +
            " native_exposure=" + (frame.exposure != nullptr ? std::string("1") : std::string("0")) +
            " skip_original_draw=" + (skip_original_draw ? std::string("1") : std::string("0")) +
            " hook_entry_detected=" + (outcome.hook_entry_detected ? std::string("1") : std::string("0")) +
            " render=" + std::to_string(draw_info->render_width) + "x" +
                std::to_string(draw_info->render_height) +
            " output=" + std::to_string(draw_info->output_width) + "x" +
                std::to_string(draw_info->output_height) +
            " output_resource=" + hex64(reinterpret_cast<std::uint64_t>(output)) +
            " jittered_mv=" + (g_config.fsr2_motion_vectors_jittered ? std::string("1") : std::string("0")) +
            " mv_scale_mode=" + (g_config.fsr2_positive_motion_vector_scale ? std::string("1") : std::string("0")) +
            " reactive_mask=" + (g_config.fsr2_use_reactive_mask ? std::string("1") : std::string("0")) +
            " transparency_mask=" + (g_config.fsr2_use_transparency_mask ? std::string("1") : std::string("0")) +
            " sharpness=" + std::to_string(frame.sharpness) +
            " hdr10_pq_color=" +
                (g_config.fsr2_hdr10_pq_color ? std::string("1") : std::string("0")) +
            " jitter_mode=" + std::to_string(g_config.fsr2_jitter_mode) +
            " jitter=" + std::to_string(jitter_x) + "," + std::to_string(jitter_y));
    }
#endif
    static std::atomic_bool first_mode2_dispatch_logged { false };
    if (translation_mode == 2 &&
        !first_mode2_dispatch_logged.exchange(true, std::memory_order_relaxed))
    {
        log_line("mode2_dispatch_succeeded opti_detoured=" +
            std::to_string(outcome.hook_entry_detected ? 1 : 0) +
            " path=" + std::string(hdr10_pq ? "hdr10_pq" : "sdr_srgb") +
            " metadata_updated=" + std::to_string(metadata_updated ? 1 : 0) +
            " native_draw_skipped=1");
    }
    return skip_original_draw;
}
#endif

Fsr31Bridge &fsr31_bridge()
{
    static Fsr31Bridge *bridge = new Fsr31Bridge();
    return *bridge;
}

#if defined(DX11FSRBRIDGE_ENABLE_OPTISCALER_NGX_EXPERIMENTAL)
OptiScalerNgxBridge &optiscaler_ngx_bridge()
{
    static OptiScalerNgxBridge *bridge = new OptiScalerNgxBridge();
    return *bridge;
}

void scan_optiscaler_ngx_exports(const char *source, bool always_log_summary)
{
    if (!g_config.enable_optiscaler_ngx_probe)
        return;

    const OptiScalerNgxBridge::ScanResult scan = optiscaler_ngx_bridge().scan_loaded_modules();
    for (const std::string &message : scan.messages)
        log_line(message);

    if (always_log_summary || (scan.newly_scanned_modules != 0 && !scan.exports_ready && g_config.log_loader_activity))
    {
        log_line(std::string("optiscaler_ngx_probe_scan source=") + source +
            " newly_scanned=" + std::to_string(scan.newly_scanned_modules) +
            " ready=" + (scan.exports_ready ? std::string("1") : std::string("0")));
    }
}

std::string hex32(std::uint32_t value)
{
    char buffer[16] {};
    std::snprintf(buffer, sizeof(buffer), "0x%08X", value);
    return buffer;
}

void maybe_probe_optiscaler_ngx_initialization(ID3D11Device *device)
{
    if (!g_config.enable_optiscaler_ngx_init_probe || device == nullptr)
        return;

    const OptiScalerNgxBridge::InitializationProbeResult result =
        optiscaler_ngx_bridge().probe_initialization(device, g_module_dir.wstring());
    if (!result.attempted)
        return;

    log_line("optiscaler_ngx_init_probe initializer=" + result.initializer_name +
        " init=" + hex32(result.initialize_result) +
        " capability=" + hex32(result.capability_result) +
        " destroy=" + hex32(result.destroy_parameters_result) +
        " success=" + (result.succeeded ? std::string("1") : std::string("0")));
}

void maybe_probe_optiscaler_ngx_capabilities(UINT element_count)
{
    if (!g_config.enable_optiscaler_ngx_capability_probe || !inspect_target_upscaler_draw(element_count))
        return;

    const OptiScalerNgxBridge::CapabilityProbeResult result =
        optiscaler_ngx_bridge().probe_capability_parameters();
    if (!result.attempted)
        return;

    log_line("optiscaler_ngx_capability_probe capability=" + hex32(result.capability_result) +
        " destroy=" + hex32(result.destroy_parameters_result) +
        " success=" + (result.succeeded ? std::string("1") : std::string("0")));
}

void maybe_probe_optiscaler_ngx_delayed_initialization(ID3D11DeviceContext *context, UINT element_count)
{
    if (!g_config.enable_optiscaler_ngx_delayed_init_probe || context == nullptr ||
        !inspect_target_upscaler_draw(element_count))
    {
        return;
    }
    if (g_optiscaler_delayed_init_probe_started.exchange(true, std::memory_order_relaxed))
        return;

    ID3D11Device *device = nullptr;
    context->GetDevice(&device);
    if (device == nullptr)
        return;

    log_line("optiscaler_ngx_delayed_init_probe_begin trigger=target_upscaler_draw");
    const OptiScalerNgxBridge::InitializationProbeResult result =
        optiscaler_ngx_bridge().probe_initialization(device, g_module_dir.wstring());
    device->Release();
    if (!result.attempted)
        return;

    log_line("optiscaler_ngx_delayed_init_probe initializer=" + result.initializer_name +
        " init=" + hex32(result.initialize_result) +
        " capability=" + hex32(result.capability_result) +
        " destroy=" + hex32(result.destroy_parameters_result) +
        " success=" + (result.succeeded ? std::string("1") : std::string("0")));
}
#endif

void maybe_probe_fsr31_context(ID3D11DeviceContext *context, UINT element_count)
{
    if (!g_config.enable_fsr31_context_probe || g_fsr31_probe_complete.load(std::memory_order_relaxed))
        return;

    const auto draw_info = inspect_target_upscaler_draw(element_count);
    if (!draw_info)
        return;

    ID3D11Device *device = nullptr;
    context->GetDevice(&device);
    if (device == nullptr)
        return;

    const Fsr31Bridge::EnsureResult result = fsr31_bridge().ensure_context(
        device,
        draw_info->render_width,
        draw_info->render_height,
        draw_info->output_width,
        draw_info->output_height);
    device->Release();

    if (result == Fsr31Bridge::EnsureResult::created)
    {
        log_line("fsr31_context_probe_success render=" +
            std::to_string(draw_info->render_width) + "x" + std::to_string(draw_info->render_height) +
            " output=" + std::to_string(draw_info->output_width) + "x" + std::to_string(draw_info->output_height));
        g_fsr31_probe_complete.store(true, std::memory_order_relaxed);
    }
    else if (result == Fsr31Bridge::EnsureResult::failed)
    {
        log_line("fsr31_context_probe_failed error=" + fsr31_bridge().last_error());
        g_fsr31_probe_complete.store(true, std::memory_order_relaxed);
    }
}

void maybe_probe_fsr31_inputs(ID3D11DeviceContext *context, UINT element_count)
{
    if (!g_config.enable_fsr31_input_probe || g_fsr31_input_probe_complete.load(std::memory_order_relaxed))
        return;

    const auto draw_info = inspect_target_upscaler_draw(element_count);
    if (!draw_info)
        return;

    ID3D11Device *device = nullptr;
    context->GetDevice(&device);
    if (device == nullptr)
        return;

    const Fsr31Bridge::EnsureResult context_result = fsr31_bridge().ensure_context(
        device,
        draw_info->render_width,
        draw_info->render_height,
        draw_info->output_width,
        draw_info->output_height);
    device->Release();
    if (context_result == Fsr31Bridge::EnsureResult::failed)
    {
        log_line("fsr31_input_probe_failed error=" + fsr31_bridge().last_error());
        g_fsr31_input_probe_complete.store(true, std::memory_order_relaxed);
        return;
    }

    std::array<ID3D11ShaderResourceView *, 4> views {};
    context->PSGetShaderResources(0, static_cast<UINT>(views.size()), views.data());
    g_internal_bridge_dispatch = true;
    const bool prepared = fsr31_bridge().prepare_inputs(context, views[0], views[2], views[3]);
    g_internal_bridge_dispatch = false;
    for (ID3D11ShaderResourceView *view : views)
    {
        if (view != nullptr)
            view->Release();
    }

    if (prepared)
        log_line("fsr31_input_probe_success color=rgba16f depth=r32f motion=rg16f encoding=signed_square_uv");
    else
        log_line("fsr31_input_probe_failed error=" + fsr31_bridge().last_error());
    g_fsr31_input_probe_complete.store(true, std::memory_order_relaxed);
}

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
    capture_runtime_snapshot_if_requested();
    static std::atomic_bool hook_logged { false };
    if (!hook_logged.exchange(true, std::memory_order_relaxed))
        log_line("draw_indexed_hook_active");
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    record_color_source_call("draw_indexed", index_count, 0, 0);
    maybe_dump_target_color_chain(context, index_count);
#endif
#if defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    poll_fsr2_transient_capture_hotkey();
#endif
    const auto target_draw_info = inspect_target_upscaler_draw(context, index_count);
    if (target_draw_info && g_config.fsr2_translation_mode >= 3)
        observe_fsr2_dynamic_color_target(*target_draw_info);
    maybe_track_fsr2_color_candidate(context, index_count);
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    if (target_draw_info)
        begin_fsr2_transient_capture(context, *target_draw_info);
    maybe_dump_color_candidate_inputs(context, index_count);
    maybe_dump_same_frame_fsr2_inputs(context, index_count);
    maybe_dispatch_early_output_probe(context, index_count);
#endif
#endif
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
#if defined(DX11FSRBRIDGE_ENABLE_OPTISCALER_NGX_EXPERIMENTAL)
    maybe_probe_optiscaler_ngx_delayed_initialization(context, index_count);
    maybe_probe_optiscaler_ngx_capabilities(index_count);
#endif
    maybe_probe_fsr31_context(context, index_count);
    maybe_probe_fsr31_inputs(context, index_count);
#endif

#if defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
    const bool fsr2_translation_handled = try_fsr2_translation_draw(context, index_count, target_draw_info, [&]
        {
            g_original_draw_indexed(context, index_count, start_index_location, base_vertex_location);
        });
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    finish_fsr2_transient_capture_fallback();
#endif
    if (fsr2_translation_handled)
        return;
#endif

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
    g_original_draw_indexed(context, index_count, start_index_location, base_vertex_location);
}

void STDMETHODCALLTYPE hooked_draw(ID3D11DeviceContext *context, UINT vertex_count, UINT start_vertex_location)
{
    capture_runtime_snapshot_if_requested();
    static std::atomic_bool hook_logged { false };
    if (!hook_logged.exchange(true, std::memory_order_relaxed))
        log_line("draw_hook_active");
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    record_color_source_call("draw", vertex_count, 0, 0);
    maybe_dump_target_color_chain(context, vertex_count);
#endif
#if defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    poll_fsr2_transient_capture_hotkey();
#endif
    const auto target_draw_info = inspect_target_upscaler_draw(context, vertex_count);
    if (target_draw_info && g_config.fsr2_translation_mode >= 3)
        observe_fsr2_dynamic_color_target(*target_draw_info);
    maybe_track_fsr2_color_candidate(context, vertex_count);
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    if (target_draw_info)
        begin_fsr2_transient_capture(context, *target_draw_info);
    maybe_dump_color_candidate_inputs(context, vertex_count);
    maybe_dump_same_frame_fsr2_inputs(context, vertex_count);
    maybe_dispatch_early_output_probe(context, vertex_count);
#endif
#endif
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
#if defined(DX11FSRBRIDGE_ENABLE_OPTISCALER_NGX_EXPERIMENTAL)
    maybe_probe_optiscaler_ngx_delayed_initialization(context, vertex_count);
    maybe_probe_optiscaler_ngx_capabilities(vertex_count);
#endif
    maybe_probe_fsr31_context(context, vertex_count);
    maybe_probe_fsr31_inputs(context, vertex_count);
#endif

#if defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
    const bool fsr2_translation_handled = try_fsr2_translation_draw(context, vertex_count, target_draw_info, [&]
        {
            g_original_draw(context, vertex_count, start_vertex_location);
        });
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    finish_fsr2_transient_capture_fallback();
#endif
    if (fsr2_translation_handled)
        return;
#endif

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
    g_original_draw(context, vertex_count, start_vertex_location);
}

HRESULT STDMETHODCALLTYPE hooked_map(ID3D11DeviceContext *context, ID3D11Resource *resource, UINT subresource, D3D11_MAP map_type, UINT map_flags, D3D11_MAPPED_SUBRESOURCE *mapped)
{
    const HRESULT hr = g_original_map(context, resource, subresource, map_type, map_flags, mapped);
    if (SUCCEEDED(hr) && resource != nullptr && mapped != nullptr && mapped->pData != nullptr)
    {
        const auto key = reinterpret_cast<std::uint64_t>(resource);
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
            g_buffer_snapshots[key] = std::vector<std::uint8_t>(bytes, bytes + mapped_it->second.size);
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
    ResourceInfo dst_info {};
    ResourceInfo src_info {};
    read_resource_info_from_resource(dst, L"copy_dst", dst_info);
    read_resource_info_from_resource(src, L"copy_src", src_info);
    if (g_config.log_resource_ops)
        log_line("copy_resource dst=" + hex64(dst_info.resource_key) + " src=" + hex64(src_info.resource_key));
    record_color_source_copy(dst_info, src_info, "copy_resource");
    g_original_copy_resource(context, dst, src);
}

void STDMETHODCALLTYPE hooked_copy_subresource_region(ID3D11DeviceContext *context, ID3D11Resource *dst, UINT dst_subresource, UINT dst_x, UINT dst_y, UINT dst_z, ID3D11Resource *src, UINT src_subresource, const D3D11_BOX *src_box)
{
    ResourceInfo dst_info {};
    ResourceInfo src_info {};
    read_resource_info_from_resource(dst, L"copy_dst", dst_info);
    read_resource_info_from_resource(src, L"copy_src", src_info);
    if (g_config.log_resource_ops)
        log_line("copy_subresource dst=" + hex64(dst_info.resource_key) + " src=" + hex64(src_info.resource_key) +
            " dst_sub=" + std::to_string(dst_subresource) + " src_sub=" + std::to_string(src_subresource));
    record_color_source_copy(dst_info, src_info, "copy_subresource");
    g_original_copy_subresource_region(context, dst, dst_subresource, dst_x, dst_y, dst_z, src, src_subresource, src_box);
}

void STDMETHODCALLTYPE hooked_update_subresource(ID3D11DeviceContext *context, ID3D11Resource *dst, UINT dst_subresource, const D3D11_BOX *dst_box, const void *src_data, UINT src_row_pitch, UINT src_depth_pitch)
{
    if (dst != nullptr && src_data != nullptr)
    {
        const auto key = reinterpret_cast<std::uint64_t>(dst);
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
            g_buffer_snapshots[key] = std::vector<std::uint8_t>(bytes, bytes + update_size);
        }
    }

    g_original_update_subresource(context, dst, dst_subresource, dst_box, src_data, src_row_pitch, src_depth_pitch);
}

void STDMETHODCALLTYPE hooked_clear_rtv(ID3D11DeviceContext *context, ID3D11RenderTargetView *rtv, const FLOAT color[4])
{
    ResourceInfo info {};
    read_resource_info(rtv, L"rtv", info);
    if (g_config.log_resource_ops)
        log_line("clear_rtv res=" + hex64(info.resource_key) + " size=" + std::to_string(info.width) + "x" + std::to_string(info.height) +
            " fmt=" + format_string(info.format) + " color=(" +
            std::to_string(color[0]) + "," + std::to_string(color[1]) + "," + std::to_string(color[2]) + "," + std::to_string(color[3]) + ")");
    record_color_source_copy(info, {}, "clear_rtv");
    g_original_clear_rtv(context, rtv, color);
}

void STDMETHODCALLTYPE hooked_clear_dsv(ID3D11DeviceContext *context, ID3D11DepthStencilView *dsv, UINT flags, FLOAT depth, UINT8 stencil)
{
    ResourceInfo info {};
    read_resource_info(dsv, L"dsv", info);
    if (g_config.log_resource_ops)
        log_line("clear_dsv res=" + hex64(info.resource_key) + " size=" + std::to_string(info.width) + "x" + std::to_string(info.height) +
            " fmt=" + format_string(info.format) + " flags=" + std::to_string(flags) +
            " depth=" + std::to_string(depth) + " stencil=" + std::to_string(stencil));
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
        { k_idx_draw_indexed, reinterpret_cast<void *>(&hooked_draw_indexed) },
        { k_idx_draw, reinterpret_cast<void *>(&hooked_draw) },
        { k_idx_map, reinterpret_cast<void *>(&hooked_map) },
        { k_idx_unmap, reinterpret_cast<void *>(&hooked_unmap) },
        { k_idx_rs_set_viewports, reinterpret_cast<void *>(&hooked_rs_set_viewports) },
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
        { k_idx_copy_resource, reinterpret_cast<void *>(&hooked_copy_resource) },
        { k_idx_clear_rtv, reinterpret_cast<void *>(&hooked_clear_rtv) },
        { k_idx_clear_dsv, reinterpret_cast<void *>(&hooked_clear_dsv) },
    });
#endif
    clone_and_patch_vtable(context, context_vtable_size(context), patches);
}

HRESULT STDMETHODCALLTYPE hooked_create_buffer(ID3D11Device *device, const D3D11_BUFFER_DESC *desc, const D3D11_SUBRESOURCE_DATA *initial_data, ID3D11Buffer **buffer)
{
    const HRESULT hr = g_original_create_buffer(device, desc, initial_data, buffer);
    if (SUCCEEDED(hr) && desc != nullptr && buffer != nullptr && *buffer != nullptr)
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
    return g_original_create_texture_2d(device, desc, initial_data, texture);
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
    if (g_config.trace_texture_creates)
    {
        patches.emplace_back(k_idx_device_create_texture_2d, reinterpret_cast<void *>(&hooked_create_texture_2d));
        log_line("texture_create_hook_active");
    }
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    patches.insert(patches.end(), {
        { k_idx_device_create_vertex_shader, reinterpret_cast<void *>(&hooked_create_vertex_shader) },
        { k_idx_device_create_compute_shader, reinterpret_cast<void *>(&hooked_create_compute_shader) },
    });
#endif
    clone_and_patch_vtable(device, k_device_vtable_size, patches);
}

void install_swapchain_hooks(IDXGISwapChain *swapchain)
{
    if (swapchain == nullptr)
        return;

    const bool should_hook_present = g_config.hook_present;
    const bool should_hook_dlssg_dxgi = dlssg_dxgi_workaround_active();
    if (!should_hook_present && !should_hook_dlssg_dxgi)
        return;

    void **vtable = *reinterpret_cast<void ***>(swapchain);
    if (should_hook_present && g_original_present == nullptr)
        g_original_present = reinterpret_cast<present_fn>(vtable[k_idx_present]);
    if (should_hook_dlssg_dxgi && g_original_set_fullscreen_state == nullptr)
        g_original_set_fullscreen_state = reinterpret_cast<set_fullscreen_state_fn>(vtable[k_idx_set_fullscreen_state]);
    if (should_hook_dlssg_dxgi && g_original_get_fullscreen_state == nullptr)
        g_original_get_fullscreen_state = reinterpret_cast<get_fullscreen_state_fn>(vtable[k_idx_get_fullscreen_state]);
    if (should_hook_dlssg_dxgi && g_original_resize_buffers == nullptr)
        g_original_resize_buffers = reinterpret_cast<resize_buffers_fn>(vtable[k_idx_resize_buffers]);
    if (should_hook_dlssg_dxgi && g_original_resize_target == nullptr)
        g_original_resize_target = reinterpret_cast<resize_target_fn>(vtable[k_idx_resize_target]);
    std::vector<std::pair<std::size_t, void *>> patches;
    if (should_hook_present)
        patches.emplace_back(k_idx_present, reinterpret_cast<void *>(&hooked_present));
    if (should_hook_dlssg_dxgi)
    {
        patches.emplace_back(k_idx_set_fullscreen_state, reinterpret_cast<void *>(&hooked_set_fullscreen_state));
        patches.emplace_back(k_idx_get_fullscreen_state, reinterpret_cast<void *>(&hooked_get_fullscreen_state));
        patches.emplace_back(k_idx_resize_buffers, reinterpret_cast<void *>(&hooked_resize_buffers));
        patches.emplace_back(k_idx_resize_target, reinterpret_cast<void *>(&hooked_resize_target));
    }
    clone_and_patch_vtable(swapchain, k_swapchain_vtable_size, patches);
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
    const HRESULT hr = g_original_create_device_and_swapchain(
        adapter,
        driver_type,
        software,
        flags,
        feature_levels,
        feature_levels_count,
        sdk_version,
        swapchain_desc,
        swapchain,
        device,
        feature_level,
        context);

    if (SUCCEEDED(hr))
    {
        install_device_hooks(device != nullptr ? *device : nullptr);
        install_factory_hooks_from_device(device != nullptr ? *device : nullptr);
        install_context_hooks(context != nullptr ? *context : nullptr);
        if (swapchain != nullptr && *swapchain != nullptr)
        {
            DXGI_SWAP_CHAIN_DESC created_desc {};
            if (SUCCEEDED((*swapchain)->GetDesc(&created_desc)))
                set_output_size(created_desc.BufferDesc.Width, created_desc.BufferDesc.Height, "CreateDeviceAndSwapChain");
            if (g_config.hook_present || dlssg_dxgi_workaround_active())
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

    const HRESULT hr = g_original_factory_create_swap_chain(factory, device, desc, swapchain);
#if defined(DX11FSRBRIDGE_FG_DXGI_DIAGNOSTICS)
    log_line("dxgi_swapchain_result id=" + std::to_string(request_id) +
        " api=CreateSwapChain hr=" + hex32(static_cast<std::uint32_t>(hr)) +
        " swapchain=" + hex64(reinterpret_cast<std::uintptr_t>(swapchain != nullptr ? *swapchain : nullptr)));
#endif
    if (SUCCEEDED(hr))
    {
        if (desc != nullptr)
            set_output_size(desc->BufferDesc.Width, desc->BufferDesc.Height, "CreateSwapChain");
        if (g_config.hook_present || dlssg_dxgi_workaround_active())
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

    const HRESULT hr = g_original_factory2_create_swap_chain_for_hwnd(factory, device, hwnd, desc, fullscreen_desc, restrict_to_output, swapchain);
#if defined(DX11FSRBRIDGE_FG_DXGI_DIAGNOSTICS)
    log_line("dxgi_swapchain_result id=" + std::to_string(request_id) +
        " api=CreateSwapChainForHwnd hr=" + hex32(static_cast<std::uint32_t>(hr)) +
        " swapchain=" + hex64(reinterpret_cast<std::uintptr_t>(swapchain != nullptr ? *swapchain : nullptr)));
#endif
    if (SUCCEEDED(hr))
    {
        if (desc != nullptr)
            set_output_size(desc->Width, desc->Height, "CreateSwapChainForHwnd");
        if (g_config.hook_present || dlssg_dxgi_workaround_active())
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
#if defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
    static std::uint32_t last_fsr2_query_mask = 0;
    const std::uint32_t fsr2_query_mask = fsr2_get_proc_address_shim_query_mask();
    if (fsr2_query_mask != last_fsr2_query_mask)
    {
        last_fsr2_query_mask = fsr2_query_mask;
        log_line("fsr2_get_proc_address_shim_queries mask=" + hex64(fsr2_query_mask));
    }
#endif
#if defined(DX11FSRBRIDGE_ENABLE_OPTISCALER_NGX_EXPERIMENTAL)
    scan_optiscaler_ngx_exports(source, false);
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

    return address;
}

void initialize()
{
    wchar_t module_path[MAX_PATH] {};
    DWORD length = GetModuleFileNameW(g_module, module_path, MAX_PATH);
    g_module_dir = std::filesystem::path(std::wstring(module_path, module_path + length)).parent_path();
    g_log_path = g_module_dir / L"Dx11FsrBridge.log";
    g_frames_path = g_module_dir / L"Dx11FsrBridge.frames.jsonl";
    g_similarity_path = g_module_dir / L"Dx11FsrBridge.similarity.txt";
    g_ps_trace_path = g_module_dir / L"Dx11FsrBridge.ps_trace.jsonl";
#if defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    reset_release_log_files();
#endif
    load_config();

    if (!process_matches())
        return;

    if (g_config.trace_pixel_shader_draws)
    {
        std::lock_guard lock(g_ps_trace_mutex);
        if (g_ps_trace_stream.is_open())
            g_ps_trace_stream.close();
        g_ps_trace_stream.open(g_ps_trace_path, std::ios::trunc);
    }
    g_ps_trace_count = 0;
    g_trace_ps_cb0_key = 0;

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
    if (g_config.enable_fsr2_get_proc_address_shim)
    {
        std::string error;
        if (install_fsr2_get_proc_address_shim(error))
            log_line("fsr2_get_proc_address_shim_ready exports=6");
        else
            log_line("fsr2_get_proc_address_shim_failed error=" + error);
    }
#endif
    start_osd();
    set_osd_text(L"Dx11FsrBridge OSD\n等待 DX11 dispatch 数据");

    log_line(std::string("d3d11_loaded=") + (GetModuleHandleW(L"d3d11.dll") != nullptr ? "1" : "0"));
    install_create_hooks_for_loaded_modules();
    install_loader_hooks_for_loaded_modules();
#if defined(DX11FSRBRIDGE_ENABLE_OPTISCALER_NGX_EXPERIMENTAL)
    scan_optiscaler_ngx_exports("initialize", true);
#endif
}

void initialize_once()
{
    std::call_once(g_initialize_once, []() { initialize(); });
}
}

#if defined(DX11FSRBRIDGE_ASI)
extern "C" __declspec(dllexport) void InitializeASI()
{
    // OptiScaler calls this optional entry point immediately after loading an
    // ASI.  DllMain already calls initialize_once(), so this is intentionally
    // idempotent and also works with generic ASI loaders.
    initialize_once();
}
#endif

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);
        initialize_once();
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
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
