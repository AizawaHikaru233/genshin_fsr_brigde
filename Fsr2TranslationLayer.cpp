#include <fsr2/ffx_fsr2.h>

#include "Fsr2TranslationLayer.h"

#include <Windows.h>
#include <d3dcompiler.h>
#include <detours/detours.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace
{
using get_proc_address_fn = decltype(&GetProcAddress);
using fsr2_context_create_fn = decltype(&ffxFsr2ContextCreate);
using fsr2_context_dispatch_fn = decltype(&ffxFsr2ContextDispatch);
using fsr2_context_destroy_fn = decltype(&ffxFsr2ContextDestroy);

get_proc_address_fn g_original_get_proc_address = nullptr;
bool g_get_proc_address_shim_installed = false;
std::atomic_uint32_t g_query_mask = 0;
fsr2_context_create_fn g_translation_context_create = nullptr;
fsr2_context_dispatch_fn g_translation_context_dispatch = nullptr;
fsr2_context_destroy_fn g_translation_context_destroy = nullptr;

std::mutex g_translation_mutex;
FfxFsr2Context g_translation_context {};
ID3D11Device *g_translation_device = nullptr;
ID3D11ComputeShader *g_input_prepare_shader = nullptr;
ID3D11ComputeShader *g_input_prepare_pq_shader = nullptr;
ID3D11ComputeShader *g_input_prepare_direct_color_shader = nullptr;
ID3D11ComputeShader *g_output_encode_shader = nullptr;
ID3D11Texture2D *g_prepared_color = nullptr;
ID3D11Texture2D *g_prepared_motion = nullptr;
ID3D11Texture2D *g_prepared_reactive = nullptr;
ID3D11Texture2D *g_prepared_transparency = nullptr;
ID3D11Texture2D *g_prepared_exposure = nullptr;
ID3D11Texture2D *g_linear_output = nullptr;
ID3D11UnorderedAccessView *g_prepared_color_uav = nullptr;
ID3D11UnorderedAccessView *g_prepared_motion_uav = nullptr;
ID3D11UnorderedAccessView *g_prepared_reactive_uav = nullptr;
ID3D11UnorderedAccessView *g_prepared_transparency_uav = nullptr;
ID3D11UnorderedAccessView *g_prepared_exposure_uav = nullptr;
ID3D11ShaderResourceView *g_linear_output_srv = nullptr;
std::uint32_t g_render_width = 0;
std::uint32_t g_render_height = 0;
std::uint32_t g_output_width = 0;
std::uint32_t g_output_height = 0;
bool g_translation_context_created = false;
bool g_reset_next_dispatch = true;
bool g_auto_exposure = true;
bool g_hdr10_pq_color = false;
bool g_hook_entry_detected = false;
LARGE_INTEGER g_last_dispatch_counter {};

template <typename Interface>
void safe_release(Interface *&value)
{
    if (value != nullptr)
    {
        value->Release();
        value = nullptr;
    }
}

void release_translation_locked()
{
    if (g_translation_context_created && g_translation_context_destroy != nullptr)
        g_translation_context_destroy(&g_translation_context);
    g_translation_context_created = false;
    g_translation_context = {};

    safe_release(g_prepared_color_uav);
    safe_release(g_prepared_motion_uav);
    safe_release(g_prepared_reactive_uav);
    safe_release(g_prepared_transparency_uav);
    safe_release(g_prepared_exposure_uav);
    safe_release(g_linear_output_srv);
    safe_release(g_prepared_color);
    safe_release(g_prepared_motion);
    safe_release(g_prepared_reactive);
    safe_release(g_prepared_transparency);
    safe_release(g_prepared_exposure);
    safe_release(g_linear_output);
    safe_release(g_input_prepare_shader);
    safe_release(g_input_prepare_pq_shader);
    safe_release(g_input_prepare_direct_color_shader);
    safe_release(g_output_encode_shader);
    safe_release(g_translation_device);

    g_render_width = 0;
    g_render_height = 0;
    g_output_width = 0;
    g_output_height = 0;
    g_auto_exposure = true;
    g_hdr10_pq_color = false;
    g_reset_next_dispatch = true;
    g_last_dispatch_counter = {};
}

bool export_entry_is_detoured(const void *address)
{
    if (address == nullptr)
        return false;
    const auto *bytes = static_cast<const std::uint8_t *>(address);
    return bytes[0] == 0xE9 || bytes[0] == 0xEB ||
        (bytes[0] == 0xFF && bytes[1] == 0x25) ||
        (bytes[0] == 0x48 && bytes[1] == 0xB8);
}

bool resolve_translation_exports_locked(std::string &error)
{
    if (g_translation_context_create != nullptr && g_translation_context_dispatch != nullptr &&
        g_translation_context_destroy != nullptr)
    {
        return true;
    }

    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ffxFsr2ContextCreate),
            &module) ||
        module == nullptr)
    {
        error = "failed to resolve Bridge module handle";
        return false;
    }

    const get_proc_address_fn resolver =
        g_original_get_proc_address != nullptr ? g_original_get_proc_address : &GetProcAddress;
    g_translation_context_create = reinterpret_cast<fsr2_context_create_fn>(
        resolver(module, "ffxFsr2ContextCreate"));
    g_translation_context_dispatch = reinterpret_cast<fsr2_context_dispatch_fn>(
        resolver(module, "ffxFsr2ContextDispatch"));
    g_translation_context_destroy = reinterpret_cast<fsr2_context_destroy_fn>(
        resolver(module, "ffxFsr2ContextDestroy"));
    if (g_translation_context_create == nullptr || g_translation_context_dispatch == nullptr ||
        g_translation_context_destroy == nullptr)
    {
        error = "failed to resolve standard FSR2 exports from Bridge module";
        return false;
    }
    return true;
}

bool create_input_prepare_shader_locked(std::string &error)
{
    static constexpr char source[] = R"(
Texture2D<float4> InputColor : register(t0);
Texture2D<float> InputDepth : register(t1);
Texture2D<float4> InputMotion : register(t2);
Texture2D<float> InputFlags : register(t3);
Texture2D<float4> InputExposure : register(t4);

RWTexture2D<float4> PreparedColor : register(u0);
RWTexture2D<float> PreparedDepth : register(u1);
RWTexture2D<float2> PreparedMotion : register(u2);
RWTexture2D<float> PreparedReactive : register(u3);
RWTexture2D<float> PreparedExposure : register(u4);
RWTexture2D<float> PreparedTransparency : register(u5);

float3 PqToLinear(float3 value)
{
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    const float3 powered = pow(max(value, 0.0), 1.0 / m2);
    return pow(max(powered - c1, 0.0) / max(c2 - c3 * powered, 1e-6), 1.0 / m1);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (all(dispatchThreadId.xy == 0))
        PreparedExposure[uint2(0, 0)] = InputExposure.Load(int3(0, 0, 0)).x;

    uint width;
    uint height;
    InputMotion.GetDimensions(width, height);
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
        return;

    const int3 location = int3(dispatchThreadId.xy, 0);
    const float4 encodedMotion = InputMotion.Load(location);
    const float2 centeredMotion = encodedMotion.xy - (127.0 / 255.0);
    const float2 motionMagnitude = abs(centeredMotion) * 2.0;
 #if !defined(DX11FSRBRIDGE_DIRECT_COLOR)
    float4 color = InputColor.Load(location);
 #if defined(DX11FSRBRIDGE_HDR10_PQ)
    color.rgb = PqToLinear(color.rgb);
 #endif
    PreparedColor[dispatchThreadId.xy] = color;
 #endif
    PreparedMotion[dispatchThreadId.xy] = sign(centeredMotion) * motionMagnitude * motionMagnitude;
    PreparedReactive[dispatchThreadId.xy] = encodedMotion.z;
    PreparedTransparency[dispatchThreadId.xy] = InputFlags.Load(location) > 0.0 ? 1.0 : 0.0;
}
)";

    static constexpr char output_source[] = R"(
Texture2D<float4> LinearOutput : register(t0);
RWTexture2D<float4> EncodedOutput : register(u0);

float3 LinearToPq(float3 value)
{
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    const float3 powered = pow(max(value, 0.0), m1);
    return pow((c1 + c2 * powered) / (1.0 + c3 * powered), m2);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width;
    uint height;
    LinearOutput.GetDimensions(width, height);
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
        return;

    const float4 linearColor = LinearOutput.Load(int3(dispatchThreadId.xy, 0));
    EncodedOutput[dispatchThreadId.xy] = float4(LinearToPq(linearColor.rgb), linearColor.a);
}
)";

    const auto compile_shader = [&](const char *shader_source,
                                    std::size_t shader_source_size,
                                    const char *shader_name,
                                    const D3D_SHADER_MACRO *macros,
                                    ID3D11ComputeShader **shader)
    {
        ID3DBlob *bytecode = nullptr;
        ID3DBlob *errors = nullptr;
        const HRESULT compile_result = D3DCompile(
            shader_source,
            shader_source_size,
            shader_name,
            macros,
            nullptr,
            "main",
            "cs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0,
            &bytecode,
            &errors);
        if (FAILED(compile_result) || bytecode == nullptr)
        {
            error = std::string(shader_name) + " compile failed=" + std::to_string(static_cast<long>(compile_result));
            if (errors != nullptr && errors->GetBufferPointer() != nullptr)
                error += " message=" + std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize());
            safe_release(errors);
            safe_release(bytecode);
            return false;
        }

        const HRESULT create_result = g_translation_device->CreateComputeShader(
            bytecode->GetBufferPointer(),
            bytecode->GetBufferSize(),
            nullptr,
            shader);
        safe_release(errors);
        safe_release(bytecode);
        if (FAILED(create_result) || *shader == nullptr)
        {
            error = std::string(shader_name) + " creation failed=" + std::to_string(static_cast<long>(create_result));
            return false;
        }
        return true;
    };

    static constexpr D3D_SHADER_MACRO hdr10_macros[] {
        { "DX11FSRBRIDGE_HDR10_PQ", "1" },
        { nullptr, nullptr },
    };
    static constexpr D3D_SHADER_MACRO direct_color_macros[] {
        { "DX11FSRBRIDGE_DIRECT_COLOR", "1" },
        { nullptr, nullptr },
    };
    if (!compile_shader(
            source,
            sizeof(source) - 1,
            "Dx11FsrBridgeFsr2PrepareInputs",
            nullptr,
            &g_input_prepare_shader))
    {
        return false;
    }
    if (!compile_shader(
            source,
            sizeof(source) - 1,
            "Dx11FsrBridgeFsr2PrepareDirectColorInputs",
            direct_color_macros,
            &g_input_prepare_direct_color_shader))
    {
        return false;
    }
    if (!compile_shader(
            source,
            sizeof(source) - 1,
            "Dx11FsrBridgeFsr2PreparePqInputs",
            hdr10_macros,
            &g_input_prepare_pq_shader))
    {
        return false;
    }
    if (!compile_shader(
            output_source,
            sizeof(output_source) - 1,
            "Dx11FsrBridgeFsr2EncodeOutput",
            nullptr,
            &g_output_encode_shader))
    {
        return false;
    }
    return true;
}

bool create_prepared_resources_locked(std::string &error)
{
    auto create_texture = [&](std::uint32_t width,
                              std::uint32_t height,
                              DXGI_FORMAT format,
                              ID3D11Texture2D **texture,
                              ID3D11UnorderedAccessView **uav)
    {
        D3D11_TEXTURE2D_DESC description {};
        description.Width = width;
        description.Height = height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = format;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        HRESULT result = g_translation_device->CreateTexture2D(&description, nullptr, texture);
        if (SUCCEEDED(result))
            result = g_translation_device->CreateUnorderedAccessView(*texture, nullptr, uav);
        return result;
    };

    HRESULT result = create_texture(
        g_render_width,
        g_render_height,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        &g_prepared_color,
        &g_prepared_color_uav);
    if (SUCCEEDED(result))
        result = create_texture(
            g_render_width,
            g_render_height,
            DXGI_FORMAT_R16G16_FLOAT,
            &g_prepared_motion,
            &g_prepared_motion_uav);
    if (SUCCEEDED(result))
        result = create_texture(
            g_render_width,
            g_render_height,
            DXGI_FORMAT_R8_UNORM,
            &g_prepared_reactive,
            &g_prepared_reactive_uav);
    if (SUCCEEDED(result))
        result = create_texture(
            1,
            1,
            DXGI_FORMAT_R32_FLOAT,
            &g_prepared_exposure,
            &g_prepared_exposure_uav);
    if (SUCCEEDED(result))
        result = create_texture(
            g_render_width,
            g_render_height,
            DXGI_FORMAT_R8_UNORM,
            &g_prepared_transparency,
            &g_prepared_transparency_uav);
    if (SUCCEEDED(result))
    {
        ID3D11UnorderedAccessView *linear_output_uav = nullptr;
        result = create_texture(
            g_output_width,
            g_output_height,
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            &g_linear_output,
            &linear_output_uav);
        safe_release(linear_output_uav);
        if (SUCCEEDED(result))
            result = g_translation_device->CreateShaderResourceView(g_linear_output, nullptr, &g_linear_output_srv);
    }
    if (FAILED(result))
    {
        error = "input prepare resource creation failed=" + std::to_string(static_cast<long>(result));
        return false;
    }
    return true;
}

bool ensure_translation_locked(const Fsr2TranslationFrame &frame, bool &created, std::string &error)
{
    if (!resolve_translation_exports_locked(error))
        return false;

    ID3D11Device *device = nullptr;
    frame.context->GetDevice(&device);
    if (device == nullptr)
    {
        error = "D3D11 device is unavailable";
        return false;
    }

    const bool matches =
        g_translation_context_created && g_translation_device == device &&
        g_render_width == frame.render_width && g_render_height == frame.render_height &&
        g_output_width == frame.output_width && g_output_height == frame.output_height &&
        g_auto_exposure == (frame.exposure == nullptr) &&
        g_hdr10_pq_color == frame.hdr10_pq_color;
    if (matches)
    {
        device->Release();
        return true;
    }

    const bool display_contract_changed =
        g_translation_context_created && g_hdr10_pq_color != frame.hdr10_pq_color;
    if (display_contract_changed)
        frame.context->Flush();
    release_translation_locked();
    g_translation_device = device;
    g_render_width = frame.render_width;
    g_render_height = frame.render_height;
    g_output_width = frame.output_width;
    g_output_height = frame.output_height;
    g_auto_exposure = frame.exposure == nullptr;
    g_hdr10_pq_color = frame.hdr10_pq_color;

    if (!create_input_prepare_shader_locked(error) || !create_prepared_resources_locked(error))
    {
        release_translation_locked();
        return false;
    }

    FfxFsr2ContextDescription description {};
    description.flags = FFX_FSR2_ENABLE_DEPTH_INVERTED;
    if (frame.hdr10_pq_color)
        description.flags |= FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE;
    if (g_auto_exposure)
        description.flags |= FFX_FSR2_ENABLE_AUTO_EXPOSURE;
    if (frame.motion_vectors_jittered)
        description.flags |= FFX_FSR2_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
    description.maxRenderSize = { frame.render_width, frame.render_height };
    description.displaySize = { frame.output_width, frame.output_height };
    description.device = g_translation_device;

    const FfxErrorCode create_result = g_translation_context_create(&g_translation_context, &description);
    if (create_result != FFX_OK)
    {
        error = "ffxFsr2ContextCreate failed=" + std::to_string(static_cast<std::uint32_t>(create_result));
        release_translation_locked();
        return false;
    }

    g_translation_context_created = true;
    g_reset_next_dispatch = true;
    created = true;
    return true;
}

bool prepare_inputs_locked(const Fsr2TranslationFrame &frame, std::string &error)
{
    ID3D11ComputeShader *previous_shader = nullptr;
    ID3D11ClassInstance *previous_class_instances[256] {};
    UINT previous_class_instance_count = static_cast<UINT>(std::size(previous_class_instances));
    ID3D11ShaderResourceView *previous_srvs[5] {};
    ID3D11UnorderedAccessView *previous_uavs[6] {};
    frame.context->CSGetShader(
        &previous_shader,
        previous_class_instances,
        &previous_class_instance_count);
    frame.context->CSGetShaderResources(0, 5, previous_srvs);
    frame.context->CSGetUnorderedAccessViews(0, 6, previous_uavs);

    ID3D11ShaderResourceView *input_srvs[] {
        frame.color,
        frame.depth,
        frame.motion,
        frame.flags,
        frame.exposure,
    };
    ID3D11UnorderedAccessView *output_uavs[] {
        g_prepared_color_uav,
        nullptr,
        g_prepared_motion_uav,
        g_prepared_reactive_uav,
        g_prepared_exposure_uav,
        g_prepared_transparency_uav,
    };
    UINT initial_counts[] { UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX };
    ID3D11ComputeShader *prepare_shader = g_input_prepare_shader;
    if (frame.hdr10_pq_color)
        prepare_shader = g_input_prepare_pq_shader;
    else if (frame.use_direct_linear_color)
        prepare_shader = g_input_prepare_direct_color_shader;
    frame.context->CSSetShader(prepare_shader, nullptr, 0);
    frame.context->CSSetShaderResources(0, 5, input_srvs);
    frame.context->CSSetUnorderedAccessViews(0, 6, output_uavs, initial_counts);
    frame.context->Dispatch((g_render_width + 7) / 8, (g_render_height + 7) / 8, 1);

    ID3D11ShaderResourceView *null_srvs[] { nullptr, nullptr, nullptr, nullptr, nullptr };
    ID3D11UnorderedAccessView *null_uavs[] { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    frame.context->CSSetShaderResources(0, 5, null_srvs);
    frame.context->CSSetUnorderedAccessViews(0, 6, null_uavs, initial_counts);
    frame.context->CSSetShaderResources(0, 5, previous_srvs);
    frame.context->CSSetUnorderedAccessViews(0, 6, previous_uavs, initial_counts);
    frame.context->CSSetShader(
        previous_shader,
        previous_class_instances,
        previous_class_instance_count);

    safe_release(previous_shader);
    for (ID3D11ClassInstance *class_instance : previous_class_instances)
    {
        if (class_instance != nullptr)
            class_instance->Release();
    }
    for (ID3D11ShaderResourceView *view : previous_srvs)
    {
        if (view != nullptr)
            view->Release();
    }
    for (ID3D11UnorderedAccessView *view : previous_uavs)
    {
        if (view != nullptr)
            view->Release();
    }

#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    const HRESULT device_status = g_translation_device->GetDeviceRemovedReason();
    if (FAILED(device_status))
    {
        error = "input prepare device error=" + std::to_string(static_cast<long>(device_status));
        return false;
    }
#endif
    return true;
}

bool encode_hdr10_output_locked(const Fsr2TranslationFrame &frame, std::string &error)
{
    if (!frame.hdr10_pq_color)
        return true;
    if (g_output_encode_shader == nullptr || g_linear_output_srv == nullptr || frame.output == nullptr)
    {
        error = "HDR10 output encoder is unavailable";
        return false;
    }

    ID3D11Texture2D *output_texture = nullptr;
    const HRESULT query_result = frame.output->QueryInterface(
        __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&output_texture));
    if (FAILED(query_result) || output_texture == nullptr)
    {
        error = "HDR10 output texture query failed=" + std::to_string(static_cast<long>(query_result));
        return false;
    }

    D3D11_TEXTURE2D_DESC output_description {};
    output_texture->GetDesc(&output_description);
    D3D11_UNORDERED_ACCESS_VIEW_DESC output_uav_description {};
    output_uav_description.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    output_uav_description.Texture2D.MipSlice = 0;
    switch (output_description.Format)
    {
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        output_uav_description.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        break;
    default:
        error = "unsupported HDR10 output format=" +
            std::to_string(static_cast<std::uint32_t>(output_description.Format));
        output_texture->Release();
        return false;
    }

    ID3D11UnorderedAccessView *output_uav = nullptr;
    const HRESULT uav_result = g_translation_device->CreateUnorderedAccessView(
        output_texture, &output_uav_description, &output_uav);
    output_texture->Release();
    if (FAILED(uav_result) || output_uav == nullptr)
    {
        error = "HDR10 output UAV creation failed=" + std::to_string(static_cast<long>(uav_result));
        return false;
    }

    ID3D11ComputeShader *previous_shader = nullptr;
    ID3D11ClassInstance *previous_class_instances[256] {};
    UINT previous_class_instance_count = static_cast<UINT>(std::size(previous_class_instances));
    ID3D11ShaderResourceView *previous_srv = nullptr;
    ID3D11UnorderedAccessView *previous_uav = nullptr;
    frame.context->CSGetShader(
        &previous_shader,
        previous_class_instances,
        &previous_class_instance_count);
    frame.context->CSGetShaderResources(0, 1, &previous_srv);
    frame.context->CSGetUnorderedAccessViews(0, 1, &previous_uav);

    UINT initial_count = UINT_MAX;
    frame.context->CSSetShader(g_output_encode_shader, nullptr, 0);
    frame.context->CSSetShaderResources(0, 1, &g_linear_output_srv);
    frame.context->CSSetUnorderedAccessViews(0, 1, &output_uav, &initial_count);
    frame.context->Dispatch((g_output_width + 7) / 8, (g_output_height + 7) / 8, 1);

    ID3D11ShaderResourceView *null_srv = nullptr;
    ID3D11UnorderedAccessView *null_uav = nullptr;
    frame.context->CSSetShaderResources(0, 1, &null_srv);
    frame.context->CSSetUnorderedAccessViews(0, 1, &null_uav, &initial_count);
    frame.context->CSSetShaderResources(0, 1, &previous_srv);
    frame.context->CSSetUnorderedAccessViews(0, 1, &previous_uav, &initial_count);
    frame.context->CSSetShader(
        previous_shader,
        previous_class_instances,
        previous_class_instance_count);

    safe_release(output_uav);
    safe_release(previous_shader);
    safe_release(previous_srv);
    safe_release(previous_uav);
    for (ID3D11ClassInstance *class_instance : previous_class_instances)
    {
        if (class_instance != nullptr)
            class_instance->Release();
    }

#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    const HRESULT device_status = g_translation_device->GetDeviceRemovedReason();
    if (FAILED(device_status))
    {
        error = "HDR10 output encode device error=" + std::to_string(static_cast<long>(device_status));
        return false;
    }
#endif
    return true;
}

FfxResource make_resource(
    ID3D11Resource *resource,
    FfxSurfaceFormat format,
    std::uint32_t width,
    std::uint32_t height,
    FfxResourceStates state,
    bool is_depth)
{
    FfxResource result {};
    result.resource = resource;
    result.description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
    result.description.format = format;
    result.description.width = width;
    result.description.height = height;
    result.description.depth = 1;
    result.description.mipCount = 1;
    result.description.flags = FFX_RESOURCE_FLAGS_NONE;
    result.state = state;
    result.isDepth = is_depth;
    return result;
}

float frame_time_delta_ms()
{
    static const LARGE_INTEGER frequency = []
    {
        LARGE_INTEGER value {};
        QueryPerformanceFrequency(&value);
        return value;
    }();
    LARGE_INTEGER now {};
    QueryPerformanceCounter(&now);
    float delta = 16.6667f;
    if (g_last_dispatch_counter.QuadPart != 0 && frequency.QuadPart != 0)
    {
        delta = static_cast<float>(
            static_cast<double>(now.QuadPart - g_last_dispatch_counter.QuadPart) * 1000.0 /
            static_cast<double>(frequency.QuadPart));
        delta = std::clamp(delta, 1.0f, 100.0f);
    }
    g_last_dispatch_counter = now;
    return delta;
}

FARPROC fsr2_export_address(const char *name)
{
    if (name == nullptr)
        return nullptr;
    if (std::strcmp(name, "ffxFsr2ContextCreate") == 0)
    {
        g_query_mask.fetch_or(1u << 0, std::memory_order_relaxed);
        return reinterpret_cast<FARPROC>(&ffxFsr2ContextCreate);
    }
    if (std::strcmp(name, "ffxFsr2ContextDispatch") == 0)
    {
        g_query_mask.fetch_or(1u << 1, std::memory_order_relaxed);
        return reinterpret_cast<FARPROC>(&ffxFsr2ContextDispatch);
    }
    if (std::strcmp(name, "ffxFsr2ContextDestroy") == 0)
    {
        g_query_mask.fetch_or(1u << 2, std::memory_order_relaxed);
        return reinterpret_cast<FARPROC>(&ffxFsr2ContextDestroy);
    }
    if (std::strcmp(name, "ffxFsr2GetUpscaleRatioFromQualityMode") == 0)
    {
        g_query_mask.fetch_or(1u << 3, std::memory_order_relaxed);
        return reinterpret_cast<FARPROC>(&ffxFsr2GetUpscaleRatioFromQualityMode);
    }
    if (std::strcmp(name, "ffxFsr2GetRenderResolutionFromQualityMode") == 0)
    {
        g_query_mask.fetch_or(1u << 4, std::memory_order_relaxed);
        return reinterpret_cast<FARPROC>(&ffxFsr2GetRenderResolutionFromQualityMode);
    }
    if (std::strcmp(name, "ffxFsr2GetJitterPhaseCount") == 0)
    {
        g_query_mask.fetch_or(1u << 5, std::memory_order_relaxed);
        return reinterpret_cast<FARPROC>(&ffxFsr2GetJitterPhaseCount);
    }
    return nullptr;
}

FARPROC WINAPI hooked_get_proc_address(HMODULE module, LPCSTR proc_name)
{
    if (module == GetModuleHandleW(nullptr) && reinterpret_cast<std::uintptr_t>(proc_name) > 0xFFFF)
    {
        if (FARPROC address = fsr2_export_address(proc_name))
            return address;
    }
    return g_original_get_proc_address(module, proc_name);
}
}

bool install_fsr2_get_proc_address_shim(std::string &error)
{
    if (g_get_proc_address_shim_installed)
        return true;

    HMODULE kernel_base = GetModuleHandleW(L"kernelbase.dll");
    if (kernel_base == nullptr)
    {
        error = "kernelbase.dll is not loaded";
        return false;
    }

    g_original_get_proc_address = reinterpret_cast<get_proc_address_fn>(
        GetProcAddress(kernel_base, "GetProcAddress"));
    if (g_original_get_proc_address == nullptr)
    {
        error = "KERNELBASE!GetProcAddress was not found";
        return false;
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    const LONG attach_result = DetourAttach(
        reinterpret_cast<PVOID *>(&g_original_get_proc_address),
        reinterpret_cast<PVOID>(&hooked_get_proc_address));
    if (attach_result != NO_ERROR)
    {
        DetourTransactionAbort();
        error = "DetourAttach failed error=" + std::to_string(attach_result);
        return false;
    }

    const LONG commit_result = DetourTransactionCommit();
    if (commit_result != NO_ERROR)
    {
        error = "DetourTransactionCommit failed error=" + std::to_string(commit_result);
        return false;
    }

    g_get_proc_address_shim_installed = true;
    return true;
}

std::uint32_t fsr2_get_proc_address_shim_query_mask()
{
    return g_query_mask.load(std::memory_order_relaxed);
}

Fsr2TranslationOutcome dispatch_fsr2_translation(const Fsr2TranslationFrame &frame)
{
    Fsr2TranslationOutcome outcome;
    if (frame.context == nullptr || frame.color == nullptr || frame.depth == nullptr || frame.motion == nullptr ||
        frame.output == nullptr || frame.render_width == 0 || frame.render_height == 0 ||
        frame.output_width == 0 || frame.output_height == 0)
    {
        outcome.error = "invalid FSR2 translation frame";
        outcome.error_code = static_cast<std::uint32_t>(FFX_ERROR_INVALID_ARGUMENT);
        return outcome;
    }

    std::lock_guard lock(g_translation_mutex);
    if (!ensure_translation_locked(frame, outcome.context_created, outcome.error))
    {
        outcome.error_code = static_cast<std::uint32_t>(FFX_ERROR_BACKEND_API_ERROR);
        return outcome;
    }
    if (!g_hook_entry_detected)
    {
        g_hook_entry_detected =
            export_entry_is_detoured(reinterpret_cast<const void *>(g_translation_context_create)) &&
            export_entry_is_detoured(reinterpret_cast<const void *>(g_translation_context_dispatch));
    }
    outcome.hook_entry_detected = g_hook_entry_detected;
    if (!prepare_inputs_locked(frame, outcome.error))
    {
        outcome.error_code = static_cast<std::uint32_t>(FFX_ERROR_BACKEND_API_ERROR);
        return outcome;
    }
    if (frame.gpu_timestamp_after_prepare != nullptr)
        frame.context->End(frame.gpu_timestamp_after_prepare);
    outcome.inputs_prepared = true;

    FfxFsr2DispatchDescription description {};
    description.commandList = frame.context;
    ID3D11Resource *direct_color_resource = nullptr;
    if (frame.use_direct_linear_color)
        frame.color->GetResource(&direct_color_resource);
    if (frame.use_direct_linear_color && direct_color_resource == nullptr)
    {
        outcome.error = "direct color resource is unavailable";
        outcome.error_code = static_cast<std::uint32_t>(FFX_ERROR_INVALID_ARGUMENT);
        return outcome;
    }
    description.color = make_resource(
        frame.use_direct_linear_color ? direct_color_resource : g_prepared_color,
        frame.use_direct_linear_color
            ? FFX_SURFACE_FORMAT_R11G11B10_FLOAT
            : FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT,
        frame.render_width,
        frame.render_height,
        FFX_RESOURCE_STATE_COMPUTE_READ,
        false);
    ID3D11Resource *direct_depth_resource = nullptr;
    frame.depth->GetResource(&direct_depth_resource);
    if (direct_depth_resource == nullptr)
    {
        safe_release(direct_color_resource);
        outcome.error = "direct depth resource is unavailable";
        outcome.error_code = static_cast<std::uint32_t>(FFX_ERROR_INVALID_ARGUMENT);
        return outcome;
    }
    description.depth = make_resource(
        direct_depth_resource,
        FFX_SURFACE_FORMAT_R32_FLOAT,
        frame.render_width,
        frame.render_height,
        FFX_RESOURCE_STATE_COMPUTE_READ,
        true);
    description.motionVectors = make_resource(
        g_prepared_motion,
        FFX_SURFACE_FORMAT_R16G16_FLOAT,
        frame.render_width,
        frame.render_height,
        FFX_RESOURCE_STATE_COMPUTE_READ,
        false);
    if (frame.exposure != nullptr)
    {
        description.exposure = make_resource(
            g_prepared_exposure,
            FFX_SURFACE_FORMAT_R32_FLOAT,
            1,
            1,
            FFX_RESOURCE_STATE_COMPUTE_READ,
            false);
    }
    if (frame.use_reactive_mask)
    {
        description.reactive = make_resource(
            g_prepared_reactive,
            FFX_SURFACE_FORMAT_R8_UNORM,
            frame.render_width,
            frame.render_height,
            FFX_RESOURCE_STATE_COMPUTE_READ,
            false);
    }
    if (frame.use_transparency_mask)
    {
        description.transparencyAndComposition = make_resource(
            g_prepared_transparency,
            FFX_SURFACE_FORMAT_R8_UNORM,
            frame.render_width,
            frame.render_height,
            FFX_RESOURCE_STATE_COMPUTE_READ,
            false);
    }
    ID3D11Resource *translation_output = frame.hdr10_pq_color ? g_linear_output : frame.output;
    const FfxSurfaceFormat translation_output_format =
        frame.hdr10_pq_color ? FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT : FFX_SURFACE_FORMAT_UNKNOWN;
    description.output = make_resource(
        translation_output,
        translation_output_format,
        frame.output_width,
        frame.output_height,
        FFX_RESOURCE_STATE_UNORDERED_ACCESS,
        false);
    description.jitterOffset = { frame.jitter_x, frame.jitter_y };
    const float motion_vector_scale_sign = frame.positive_motion_vector_scale ? 1.0f : -1.0f;
    description.motionVectorScale = {
        motion_vector_scale_sign * static_cast<float>(frame.render_width),
        motion_vector_scale_sign * static_cast<float>(frame.render_height),
    };
    description.renderSize = { frame.render_width, frame.render_height };
    description.enableSharpening = frame.enable_sharpening;
    description.sharpness = std::clamp(frame.sharpness, 0.0f, 1.0f);
    description.frameTimeDelta = frame_time_delta_ms();
    description.preExposure = 1.0f;
    description.reset = g_reset_next_dispatch || frame.reset;
    description.cameraNear = 0.25f;
    description.cameraFar = 6000.0f;
    description.cameraFovAngleVertical = 0.7853981634f;
    description.viewSpaceToMetersFactor = 1.0f;

    const FfxErrorCode dispatch_result = g_translation_context_dispatch(&g_translation_context, &description);
    safe_release(direct_color_resource);
    safe_release(direct_depth_resource);
    outcome.error_code = static_cast<std::uint32_t>(dispatch_result);
    if (dispatch_result != FFX_OK)
    {
        outcome.error = "ffxFsr2ContextDispatch failed=" + std::to_string(outcome.error_code);
        return outcome;
    }
    if (!encode_hdr10_output_locked(frame, outcome.error))
    {
        outcome.error_code = static_cast<std::uint32_t>(FFX_ERROR_BACKEND_API_ERROR);
        return outcome;
    }

    g_reset_next_dispatch = false;
    outcome.succeeded = true;
    return outcome;
}

void reset_fsr2_translation_context()
{
    std::lock_guard lock(g_translation_mutex);
    release_translation_locked();
}

FfxErrorCode ffxFsr2ContextCreate(
    FfxFsr2Context *context,
    const FfxFsr2ContextDescription *context_description)
{
    if (context == nullptr || context_description == nullptr || context_description->device == nullptr)
        return FFX_ERROR_INVALID_ARGUMENT;

    std::memset(context, 0, sizeof(*context));
    return FFX_OK;
}

FfxErrorCode ffxFsr2ContextDispatch(
    FfxFsr2Context *context,
    const FfxFsr2DispatchDescription *dispatch_description)
{
    if (context == nullptr || dispatch_description == nullptr || dispatch_description->commandList == nullptr)
        return FFX_ERROR_INVALID_ARGUMENT;
    return FFX_OK;
}

FfxErrorCode ffxFsr2ContextDestroy(FfxFsr2Context *context)
{
    if (context == nullptr)
        return FFX_ERROR_INVALID_ARGUMENT;

    std::memset(context, 0, sizeof(*context));
    return FFX_OK;
}

float ffxFsr2GetUpscaleRatioFromQualityMode(FfxFsr2QualityMode quality_mode)
{
    switch (quality_mode)
    {
    case FFX_FSR2_QUALITY_MODE_QUALITY:
        return 1.5f;
    case FFX_FSR2_QUALITY_MODE_BALANCED:
        return 1.7f;
    case FFX_FSR2_QUALITY_MODE_PERFORMANCE:
        return 2.0f;
    case FFX_FSR2_QUALITY_MODE_ULTRA_PERFORMANCE:
        return 3.0f;
    default:
        return 1.0f;
    }
}

FfxErrorCode ffxFsr2GetRenderResolutionFromQualityMode(
    std::uint32_t *render_width,
    std::uint32_t *render_height,
    std::uint32_t display_width,
    std::uint32_t display_height,
    FfxFsr2QualityMode quality_mode)
{
    if (render_width == nullptr || render_height == nullptr)
        return FFX_ERROR_INVALID_ARGUMENT;

    const float ratio = ffxFsr2GetUpscaleRatioFromQualityMode(quality_mode);
    *render_width = static_cast<std::uint32_t>(static_cast<float>(display_width) / ratio);
    *render_height = static_cast<std::uint32_t>(static_cast<float>(display_height) / ratio);
    return FFX_OK;
}

std::int32_t ffxFsr2GetJitterPhaseCount(
    std::int32_t render_width,
    std::int32_t display_width)
{
    if (render_width <= 0 || display_width <= 0)
        return 0;

    const float ratio = static_cast<float>(display_width) / static_cast<float>(render_width);
    return static_cast<std::int32_t>(std::ceil(8.0f * ratio * ratio));
}
