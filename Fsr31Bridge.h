#pragma once

#include <d3d11.h>

#include <cstdint>
#include <mutex>
#include <string>

#if defined(DX11FSRBRIDGE_ENABLE_FSR31_EXPERIMENTAL)
#include <fsr31/dx11/ffx_dx11.h>
#include <fsr31/ffx_fsr3upscaler.h>
#endif

class Fsr31Bridge
{
public:
    enum class EnsureResult
    {
        ready,
        created,
        failed,
    };

    EnsureResult ensure_context(
        ID3D11Device *device,
        std::uint32_t render_width,
        std::uint32_t render_height,
        std::uint32_t output_width,
        std::uint32_t output_height);

    bool prepare_inputs(
        ID3D11DeviceContext *context,
        ID3D11ShaderResourceView *color,
        ID3D11ShaderResourceView *depth,
        ID3D11ShaderResourceView *motion);

    std::string last_error() const;

private:
    bool ensure_input_resources_locked();
    bool ensure_input_prepare_shader_locked();
    bool ensure_deferred_context_locked();
    void release_input_resources_locked();
    void release_context_locked();

    mutable std::mutex mutex_;
#if defined(DX11FSRBRIDGE_ENABLE_FSR31_EXPERIMENTAL)
    Fsr31::FfxFsr3UpscalerContext context_ {};
    Fsr31::FfxFsr3UpscalerContextDescription description_ {};
#endif
    ID3D11Device *device_ = nullptr;
    void *scratch_buffer_ = nullptr;
    bool initialized_ = false;
    bool failed_ = false;
    std::uint32_t render_width_ = 0;
    std::uint32_t render_height_ = 0;
    std::uint32_t output_width_ = 0;
    std::uint32_t output_height_ = 0;
    ID3D11ComputeShader *input_prepare_shader_ = nullptr;
    ID3D11DeviceContext *deferred_context_ = nullptr;
    ID3D11Texture2D *prepared_color_ = nullptr;
    ID3D11Texture2D *prepared_depth_ = nullptr;
    ID3D11Texture2D *prepared_motion_ = nullptr;
    ID3D11UnorderedAccessView *prepared_color_uav_ = nullptr;
    ID3D11UnorderedAccessView *prepared_depth_uav_ = nullptr;
    ID3D11UnorderedAccessView *prepared_motion_uav_ = nullptr;
    std::string last_error_;
};
