#pragma once

#include <d3d11.h>

#include <cstdint>
#include <string>

bool install_fsr2_get_proc_address_shim(std::string &error);
std::uint32_t fsr2_get_proc_address_shim_query_mask();

struct Fsr2TranslationFrame
{
    ID3D11DeviceContext *context = nullptr;
    ID3D11ShaderResourceView *color = nullptr;
    ID3D11ShaderResourceView *depth = nullptr;
    ID3D11ShaderResourceView *motion = nullptr;
    ID3D11ShaderResourceView *flags = nullptr;
    ID3D11ShaderResourceView *exposure = nullptr;
    ID3D11Resource *output = nullptr;
    ID3D11Query *gpu_timestamp_after_prepare = nullptr;
    std::uint32_t render_width = 0;
    std::uint32_t render_height = 0;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    float jitter_x = 0.0f;
    float jitter_y = 0.0f;
    bool motion_vectors_jittered = false;
    bool positive_motion_vector_scale = false;
    bool use_reactive_mask = false;
    bool use_transparency_mask = false;
    bool enable_sharpening = false;
    float sharpness = 0.0f;
    bool hdr10_pq_color = false;
    bool use_direct_linear_color = false;
    bool reset = false;
};

struct Fsr2TranslationOutcome
{
    bool succeeded = false;
    bool context_created = false;
    bool hook_entry_detected = false;
    bool inputs_prepared = false;
    std::uint32_t error_code = 0;
    std::string error;
};

Fsr2TranslationOutcome dispatch_fsr2_translation(const Fsr2TranslationFrame &frame);
void reset_fsr2_translation_context();
