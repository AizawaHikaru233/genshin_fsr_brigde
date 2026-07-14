#include "Fsr31Bridge.h"

#include <d3dcompiler.h>

#include <cstdlib>

namespace
{
std::string error_message(const char *operation, Fsr31::FfxErrorCode code)
{
    return std::string(operation) + " failed code=" + std::to_string(static_cast<int>(code));
}

template <typename Interface>
void safe_release(Interface *&value)
{
    if (value != nullptr)
    {
        value->Release();
        value = nullptr;
    }
}
}

Fsr31Bridge::EnsureResult Fsr31Bridge::ensure_context(
    ID3D11Device *device,
    std::uint32_t render_width,
    std::uint32_t render_height,
    std::uint32_t output_width,
    std::uint32_t output_height)
{
    if (device == nullptr || render_width == 0 || render_height == 0 || output_width == 0 || output_height == 0)
        return EnsureResult::failed;

    std::lock_guard lock(mutex_);
    const bool configuration_matches =
        device_ == device &&
        render_width_ == render_width && render_height_ == render_height &&
        output_width_ == output_width && output_height_ == output_height;
    if (initialized_ && configuration_matches)
        return EnsureResult::ready;
    if (failed_ && configuration_matches)
        return EnsureResult::failed;

    release_context_locked();
    device->AddRef();
    device_ = device;
    render_width_ = render_width;
    render_height_ = render_height;
    output_width_ = output_width;
    output_height_ = output_height;

    const std::size_t scratch_size = Fsr31::ffxGetScratchMemorySizeDX11(1);
    scratch_buffer_ = std::calloc(scratch_size, 1);
    if (scratch_buffer_ == nullptr)
    {
        failed_ = true;
        last_error_ = "ffx scratch allocation failed";
        return EnsureResult::failed;
    }

    description_ = {};
    const Fsr31::FfxErrorCode interface_result = Fsr31::ffxGetInterfaceDX11(
        &description_.backendInterface,
        Fsr31::ffxGetDeviceDX11_Fsr31(device_),
        scratch_buffer_,
        scratch_size,
        1);
    if (interface_result != Fsr31::FFX_OK)
    {
        failed_ = true;
        last_error_ = error_message("ffxGetInterfaceDX11", interface_result);
        return EnsureResult::failed;
    }

    description_.flags = Fsr31::FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
    description_.maxRenderSize = { render_width_, render_height_ };
    description_.maxUpscaleSize = { output_width_, output_height_ };
    description_.fpMessage = nullptr;

    const Fsr31::FfxErrorCode create_result =
        Fsr31::ffxFsr3UpscalerContextCreate(&context_, &description_);
    if (create_result != Fsr31::FFX_OK)
    {
        failed_ = true;
        last_error_ = error_message("ffxFsr3UpscalerContextCreate", create_result);
        return EnsureResult::failed;
    }

    initialized_ = true;
    failed_ = false;
    last_error_.clear();
    return EnsureResult::created;
}

bool Fsr31Bridge::prepare_inputs(
    ID3D11DeviceContext *context,
    ID3D11ShaderResourceView *color,
    ID3D11ShaderResourceView *depth,
    ID3D11ShaderResourceView *motion)
{
    if (context == nullptr || color == nullptr || depth == nullptr || motion == nullptr)
        return false;

    std::lock_guard lock(mutex_);
    if (!initialized_ || !ensure_deferred_context_locked() ||
        !ensure_input_prepare_shader_locked() || !ensure_input_resources_locked())
        return false;

    ID3D11ShaderResourceView *input_srvs[] { color, depth, motion };
    ID3D11UnorderedAccessView *output_uavs[] {
        prepared_color_uav_,
        prepared_depth_uav_,
        prepared_motion_uav_,
    };
    UINT initial_counts[] { UINT_MAX, UINT_MAX, UINT_MAX };
    deferred_context_->ClearState();
    deferred_context_->CSSetShader(input_prepare_shader_, nullptr, 0);
    deferred_context_->CSSetShaderResources(0, 3, input_srvs);
    deferred_context_->CSSetUnorderedAccessViews(0, 3, output_uavs, initial_counts);
    deferred_context_->Dispatch((render_width_ + 7) / 8, (render_height_ + 7) / 8, 1);

    ID3D11ShaderResourceView *null_srvs[] { nullptr, nullptr, nullptr };
    ID3D11UnorderedAccessView *null_uavs[] { nullptr, nullptr, nullptr };
    deferred_context_->CSSetShaderResources(0, 3, null_srvs);
    deferred_context_->CSSetUnorderedAccessViews(0, 3, null_uavs, initial_counts);

    ID3D11CommandList *command_list = nullptr;
    const HRESULT finish_result = deferred_context_->FinishCommandList(FALSE, &command_list);
    deferred_context_->ClearState();
    if (FAILED(finish_result) || command_list == nullptr)
    {
        last_error_ = "input prepare FinishCommandList failed=" + std::to_string(static_cast<long>(finish_result));
        safe_release(command_list);
        return false;
    }

    context->ExecuteCommandList(command_list, TRUE);
    command_list->Release();

    const HRESULT device_status = device_->GetDeviceRemovedReason();
    if (FAILED(device_status))
    {
        last_error_ = "input prepare device error=" + std::to_string(static_cast<long>(device_status));
        return false;
    }
    return true;
}

bool Fsr31Bridge::ensure_deferred_context_locked()
{
    if (deferred_context_ != nullptr)
        return true;

    const HRESULT result = device_->CreateDeferredContext(0, &deferred_context_);
    if (FAILED(result) || deferred_context_ == nullptr)
    {
        last_error_ = "CreateDeferredContext failed=" + std::to_string(static_cast<long>(result));
        return false;
    }
    return true;
}

std::string Fsr31Bridge::last_error() const
{
    std::lock_guard lock(mutex_);
    return last_error_;
}

bool Fsr31Bridge::ensure_input_prepare_shader_locked()
{
    if (input_prepare_shader_ != nullptr)
        return true;

    static constexpr char source[] = R"(
Texture2D<float4> InputColor : register(t0);
Texture2D<float> InputDepth : register(t1);
Texture2D<float4> InputMotion : register(t2);

RWTexture2D<float4> PreparedColor : register(u0);
RWTexture2D<float> PreparedDepth : register(u1);
RWTexture2D<float2> PreparedMotion : register(u2);

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width;
    uint height;
    InputColor.GetDimensions(width, height);
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
        return;

    const int3 location = int3(dispatchThreadId.xy, 0);
    const float4 encodedMotion = InputMotion.Load(location);
    const float2 centeredMotion = encodedMotion.yz - (127.0 / 255.0);
    const float2 motionMagnitude = abs(centeredMotion) * 2.0;

    PreparedColor[dispatchThreadId.xy] = InputColor.Load(location);
    PreparedDepth[dispatchThreadId.xy] = InputDepth.Load(location);
    PreparedMotion[dispatchThreadId.xy] = sign(centeredMotion) * motionMagnitude * motionMagnitude;
}
)";

    ID3DBlob *bytecode = nullptr;
    ID3DBlob *errors = nullptr;
    const HRESULT compile_result = D3DCompile(
        source,
        sizeof(source) - 1,
        "Dx11FsrBridgePrepareInputs",
        nullptr,
        nullptr,
        "main",
        "cs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &bytecode,
        &errors);
    if (FAILED(compile_result) || bytecode == nullptr)
    {
        last_error_ = "input prepare compile failed=" + std::to_string(static_cast<long>(compile_result));
        if (errors != nullptr && errors->GetBufferPointer() != nullptr)
            last_error_ += " error=" + std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize());
        safe_release(errors);
        safe_release(bytecode);
        return false;
    }

    const HRESULT create_result = device_->CreateComputeShader(
        bytecode->GetBufferPointer(),
        bytecode->GetBufferSize(),
        nullptr,
        &input_prepare_shader_);
    safe_release(errors);
    safe_release(bytecode);
    if (FAILED(create_result) || input_prepare_shader_ == nullptr)
    {
        last_error_ = "input prepare shader creation failed=" + std::to_string(static_cast<long>(create_result));
        return false;
    }
    return true;
}

bool Fsr31Bridge::ensure_input_resources_locked()
{
    if (prepared_color_ != nullptr && prepared_depth_ != nullptr && prepared_motion_ != nullptr)
        return true;

    release_input_resources_locked();
    auto create_texture = [&](DXGI_FORMAT format, ID3D11Texture2D **texture, ID3D11UnorderedAccessView **uav)
    {
        D3D11_TEXTURE2D_DESC description {};
        description.Width = render_width_;
        description.Height = render_height_;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = format;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        HRESULT result = device_->CreateTexture2D(&description, nullptr, texture);
        if (SUCCEEDED(result))
            result = device_->CreateUnorderedAccessView(*texture, nullptr, uav);
        return result;
    };

    HRESULT result = create_texture(DXGI_FORMAT_R16G16B16A16_FLOAT, &prepared_color_, &prepared_color_uav_);
    if (SUCCEEDED(result))
        result = create_texture(DXGI_FORMAT_R32_FLOAT, &prepared_depth_, &prepared_depth_uav_);
    if (SUCCEEDED(result))
        result = create_texture(DXGI_FORMAT_R16G16_FLOAT, &prepared_motion_, &prepared_motion_uav_);
    if (FAILED(result))
    {
        last_error_ = "input prepare resource creation failed=" + std::to_string(static_cast<long>(result));
        release_input_resources_locked();
        return false;
    }
    return true;
}

void Fsr31Bridge::release_input_resources_locked()
{
    safe_release(prepared_color_uav_);
    safe_release(prepared_depth_uav_);
    safe_release(prepared_motion_uav_);
    safe_release(prepared_color_);
    safe_release(prepared_depth_);
    safe_release(prepared_motion_);
}

void Fsr31Bridge::release_context_locked()
{
    release_input_resources_locked();
    safe_release(input_prepare_shader_);
    safe_release(deferred_context_);
    if (initialized_)
        Fsr31::ffxFsr3UpscalerContextDestroy(&context_);
    initialized_ = false;
    failed_ = false;
    context_ = {};
    description_ = {};

    if (scratch_buffer_ != nullptr)
    {
        std::free(scratch_buffer_);
        scratch_buffer_ = nullptr;
    }
    if (device_ != nullptr)
    {
        device_->Release();
        device_ = nullptr;
    }
}
