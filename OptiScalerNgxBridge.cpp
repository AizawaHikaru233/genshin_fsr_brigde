#include "OptiScalerNgxBridge.h"

#include <TlHelp32.h>

#include <nvsdk_ngx.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <initializer_list>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace
{
constexpr std::array<std::string_view, 9> k_relevant_exports {
    "NVSDK_NGX_D3D11_Init_with_ProjectID",
    "NVSDK_NGX_D3D11_Init_ProjectID",
    "NVSDK_NGX_D3D11_Init",
    "NVSDK_NGX_D3D11_GetCapabilityParameters",
    "NVSDK_NGX_D3D11_CreateFeature",
    "NVSDK_NGX_D3D11_EvaluateFeature",
    "NVSDK_NGX_D3D11_ReleaseFeature",
    "NVSDK_NGX_D3D11_DestroyParameters",
    "NVSDK_NGX_D3D11_Shutdown1",
};

constexpr std::string_view k_shutdown_fallback = "NVSDK_NGX_D3D11_Shutdown";

struct ModuleRecord
{
    HMODULE module = nullptr;
    std::size_t image_size = 0;
    std::wstring path;
};

struct ExportInspection
{
    std::unordered_map<std::string, void *> addresses;
    std::size_t relevant_count = 0;
};

std::string narrow_utf8(const std::wstring &value)
{
    if (value.empty())
        return {};

    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};

    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::string pointer_text(const void *value)
{
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << reinterpret_cast<std::uintptr_t>(value);
    return out.str();
}

std::vector<ModuleRecord> enumerate_modules()
{
    std::vector<ModuleRecord> modules;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
        return modules;

    MODULEENTRY32W entry {};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry))
    {
        do
        {
            modules.push_back(ModuleRecord {
                entry.hModule,
                static_cast<std::size_t>(entry.modBaseSize),
                entry.szExePath,
            });
        }
        while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return modules;
}

bool range_is_valid(std::size_t image_size, std::uint32_t rva, std::size_t byte_count)
{
    const std::size_t offset = rva;
    return offset <= image_size && byte_count <= image_size - offset;
}

template <typename T>
const T *rva_pointer(const std::uint8_t *base, std::size_t image_size, std::uint32_t rva, std::size_t count = 1)
{
    if (count > static_cast<std::size_t>(-1) / sizeof(T))
        return nullptr;
    const std::size_t byte_count = sizeof(T) * count;
    if (!range_is_valid(image_size, rva, byte_count))
        return nullptr;
    return reinterpret_cast<const T *>(base + rva);
}

std::string_view image_string(const std::uint8_t *base, std::size_t image_size, std::uint32_t rva)
{
    if (!range_is_valid(image_size, rva, 1))
        return {};

    const char *start = reinterpret_cast<const char *>(base + rva);
    const std::size_t maximum = image_size - rva;
    const void *end = std::memchr(start, '\0', maximum);
    if (end == nullptr)
        return {};
    return std::string_view(start, static_cast<const char *>(end) - start);
}

bool is_relevant_export(std::string_view name)
{
    return std::find(k_relevant_exports.begin(), k_relevant_exports.end(), name) != k_relevant_exports.end() ||
        name == k_shutdown_fallback;
}

ExportInspection inspect_exports(const ModuleRecord &module)
{
    ExportInspection result;
    if (module.module == nullptr || module.image_size < sizeof(IMAGE_DOS_HEADER))
        return result;

    const auto *base = reinterpret_cast<const std::uint8_t *>(module.module);
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        return result;

    const std::size_t nt_offset = static_cast<std::size_t>(dos->e_lfanew);
    if (nt_offset > module.image_size || sizeof(IMAGE_NT_HEADERS) > module.image_size - nt_offset)
        return result;

    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + nt_offset);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return result;
    if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT)
        return result;

    const IMAGE_DATA_DIRECTORY &directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (directory.VirtualAddress == 0 || directory.Size < sizeof(IMAGE_EXPORT_DIRECTORY))
        return result;

    const auto *exports = rva_pointer<IMAGE_EXPORT_DIRECTORY>(base, module.image_size, directory.VirtualAddress);
    if (exports == nullptr || exports->NumberOfNames == 0 || exports->NumberOfFunctions == 0)
        return result;

    const auto *name_rvas = rva_pointer<DWORD>(base, module.image_size, exports->AddressOfNames, exports->NumberOfNames);
    const auto *ordinals = rva_pointer<WORD>(base, module.image_size, exports->AddressOfNameOrdinals, exports->NumberOfNames);
    const auto *function_rvas = rva_pointer<DWORD>(base, module.image_size, exports->AddressOfFunctions, exports->NumberOfFunctions);
    if (name_rvas == nullptr || ordinals == nullptr || function_rvas == nullptr)
        return result;

    for (DWORD index = 0; index < exports->NumberOfNames; ++index)
    {
        const std::string_view name = image_string(base, module.image_size, name_rvas[index]);
        if (!is_relevant_export(name))
            continue;

        ++result.relevant_count;
        const WORD ordinal = ordinals[index];
        if (ordinal >= exports->NumberOfFunctions)
            continue;

        const DWORD function_rva = function_rvas[ordinal];
        const std::uint64_t export_start = directory.VirtualAddress;
        const std::uint64_t export_end = export_start + directory.Size;
        if (function_rva == 0 || (function_rva >= export_start && function_rva < export_end) ||
            !range_is_valid(module.image_size, function_rva, 1))
        {
            continue;
        }

        result.addresses.emplace(std::string(name), const_cast<std::uint8_t *>(base + function_rva));
    }

    return result;
}

void *find_address(const ExportInspection &inspection, std::string_view name)
{
    const auto iterator = inspection.addresses.find(std::string(name));
    return iterator != inspection.addresses.end() ? iterator->second : nullptr;
}

std::pair<void *, std::string> find_first_address(
    const ExportInspection &inspection,
    std::initializer_list<std::string_view> names)
{
    for (const std::string_view name : names)
    {
        if (void *address = find_address(inspection, name))
            return { address, std::string(name) };
    }
    return {};
}
}

OptiScalerNgxBridge::ScanResult OptiScalerNgxBridge::scan_loaded_modules()
{
    std::lock_guard lock(mutex_);

    ScanResult result;
    result.exports_ready = exports_.module != nullptr;
    if (result.exports_ready)
        return result;

    for (const ModuleRecord &module : enumerate_modules())
    {
        const std::uintptr_t module_key = reinterpret_cast<std::uintptr_t>(module.module);
        if (!scanned_modules_.insert(module_key).second)
            continue;

        ++result.newly_scanned_modules;
        const ExportInspection inspection = inspect_exports(module);
        if (inspection.relevant_count == 0)
            continue;

        const auto [initialize, initializer_name] = find_first_address(inspection, {
            "NVSDK_NGX_D3D11_Init_with_ProjectID",
            "NVSDK_NGX_D3D11_Init_ProjectID",
            "NVSDK_NGX_D3D11_Init",
        });
        const auto [shutdown, shutdown_name] = find_first_address(inspection, {
            "NVSDK_NGX_D3D11_Shutdown1",
            "NVSDK_NGX_D3D11_Shutdown",
        });

        ResolvedExports candidate;
        candidate.module = module.module;
        candidate.initialize = initialize;
        candidate.get_capability_parameters = find_address(inspection, "NVSDK_NGX_D3D11_GetCapabilityParameters");
        candidate.create_feature = find_address(inspection, "NVSDK_NGX_D3D11_CreateFeature");
        candidate.evaluate_feature = find_address(inspection, "NVSDK_NGX_D3D11_EvaluateFeature");
        candidate.release_feature = find_address(inspection, "NVSDK_NGX_D3D11_ReleaseFeature");
        candidate.destroy_parameters = find_address(inspection, "NVSDK_NGX_D3D11_DestroyParameters");
        candidate.shutdown = shutdown;
        candidate.initializer_name = initializer_name;
        candidate.shutdown_name = shutdown_name;

        const std::array<void *, 7> required {
            candidate.initialize,
            candidate.get_capability_parameters,
            candidate.create_feature,
            candidate.evaluate_feature,
            candidate.release_feature,
            candidate.destroy_parameters,
            candidate.shutdown,
        };
        constexpr std::array<std::string_view, 7> required_labels {
            "initializer",
            "GetCapabilityParameters",
            "CreateFeature",
            "EvaluateFeature",
            "ReleaseFeature",
            "DestroyParameters",
            "shutdown",
        };
        const std::size_t resolved_count = static_cast<std::size_t>(std::count_if(
            required.begin(), required.end(), [](const void *address) { return address != nullptr; }));

        std::string missing;
        for (std::size_t index = 0; index < required.size(); ++index)
        {
            if (required[index] != nullptr)
                continue;
            if (!missing.empty())
                missing += ',';
            missing += required_labels[index];
        }

        const std::string path = narrow_utf8(module.path);
        result.messages.push_back(
            "optiscaler_ngx_candidate module=\"" + path + "\" base=" + pointer_text(module.module) +
            " resolved=" + std::to_string(resolved_count) + "/7 missing=" + (missing.empty() ? "none" : missing));

        if (resolved_count != required.size())
            continue;

        exports_ = std::move(candidate);
        result.exports_ready = true;
        result.messages.push_back(
            "optiscaler_ngx_exports_ready module=\"" + path + "\" initializer=" + exports_.initializer_name +
            " shutdown=" + exports_.shutdown_name);
        break;
    }

    return result;
}

OptiScalerNgxBridge::InitializationProbeResult OptiScalerNgxBridge::probe_initialization(
    ID3D11Device *device,
    const std::wstring &application_data_path)
{
    std::lock_guard lock(mutex_);

    InitializationProbeResult result;
    if (initialization_attempted_ || exports_.module == nullptr || device == nullptr)
        return result;

    initialization_attempted_ = true;
    result.attempted = true;
    result.initializer_name = exports_.initializer_name;

    NVSDK_NGX_Result initialize_result = NVSDK_NGX_Result_Fail;
    if (exports_.initializer_name == "NVSDK_NGX_D3D11_Init_with_ProjectID")
    {
        using initialize_fn = NVSDK_NGX_Result(NVSDK_CONV *)(
            const char *,
            NVSDK_NGX_EngineType,
            const char *,
            const wchar_t *,
            ID3D11Device *,
            const NVSDK_NGX_FeatureCommonInfo *,
            NVSDK_NGX_Version);
        const auto initialize = reinterpret_cast<initialize_fn>(exports_.initialize);
        initialize_result = initialize(
            "Dx11FsrBridge",
            NVSDK_NGX_ENGINE_TYPE_CUSTOM,
            "0.1.0",
            application_data_path.c_str(),
            device,
            nullptr,
            NVSDK_NGX_Version_API);
    }
    else if (exports_.initializer_name == "NVSDK_NGX_D3D11_Init_ProjectID")
    {
        using initialize_fn = NVSDK_NGX_Result(NVSDK_CONV *)(
            const char *,
            NVSDK_NGX_EngineType,
            const char *,
            const wchar_t *,
            ID3D11Device *,
            NVSDK_NGX_Version,
            const NVSDK_NGX_FeatureCommonInfo *);
        const auto initialize = reinterpret_cast<initialize_fn>(exports_.initialize);
        initialize_result = initialize(
            "Dx11FsrBridge",
            NVSDK_NGX_ENGINE_TYPE_CUSTOM,
            "0.1.0",
            application_data_path.c_str(),
            device,
            NVSDK_NGX_Version_API,
            nullptr);
    }
    else
    {
        using initialize_fn = NVSDK_NGX_Result(NVSDK_CONV *)(
            unsigned long long,
            const wchar_t *,
            ID3D11Device *,
            const NVSDK_NGX_FeatureCommonInfo *,
            NVSDK_NGX_Version);
        const auto initialize = reinterpret_cast<initialize_fn>(exports_.initialize);
        initialize_result = initialize(
            0x1337,
            application_data_path.c_str(),
            device,
            nullptr,
            NVSDK_NGX_Version_API);
    }

    result.initialize_result = static_cast<std::uint32_t>(initialize_result);
    if (initialize_result != NVSDK_NGX_Result_Success)
        return result;

    using get_capability_parameters_fn = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Parameter **);
    using destroy_parameters_fn = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Parameter *);
    const auto get_capability_parameters =
        reinterpret_cast<get_capability_parameters_fn>(exports_.get_capability_parameters);
    const auto destroy_parameters = reinterpret_cast<destroy_parameters_fn>(exports_.destroy_parameters);

    NVSDK_NGX_Parameter *parameters = nullptr;
    const NVSDK_NGX_Result capability_result = get_capability_parameters(&parameters);
    result.capability_result = static_cast<std::uint32_t>(capability_result);
    if (capability_result != NVSDK_NGX_Result_Success || parameters == nullptr)
        return result;

    const NVSDK_NGX_Result destroy_result = destroy_parameters(parameters);
    result.destroy_parameters_result = static_cast<std::uint32_t>(destroy_result);
    result.succeeded = destroy_result == NVSDK_NGX_Result_Success;
    return result;
}

OptiScalerNgxBridge::CapabilityProbeResult OptiScalerNgxBridge::probe_capability_parameters()
{
    std::lock_guard lock(mutex_);

    CapabilityProbeResult result;
    if (capability_probe_attempted_ || exports_.module == nullptr)
        return result;

    capability_probe_attempted_ = true;
    result.attempted = true;

    using get_capability_parameters_fn = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Parameter **);
    using destroy_parameters_fn = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Parameter *);
    const auto get_capability_parameters =
        reinterpret_cast<get_capability_parameters_fn>(exports_.get_capability_parameters);
    const auto destroy_parameters = reinterpret_cast<destroy_parameters_fn>(exports_.destroy_parameters);

    NVSDK_NGX_Parameter *parameters = nullptr;
    const NVSDK_NGX_Result capability_result = get_capability_parameters(&parameters);
    result.capability_result = static_cast<std::uint32_t>(capability_result);
    if (capability_result != NVSDK_NGX_Result_Success || parameters == nullptr)
        return result;

    const NVSDK_NGX_Result destroy_result = destroy_parameters(parameters);
    result.destroy_parameters_result = static_cast<std::uint32_t>(destroy_result);
    result.succeeded = destroy_result == NVSDK_NGX_Result_Success;
    return result;
}
