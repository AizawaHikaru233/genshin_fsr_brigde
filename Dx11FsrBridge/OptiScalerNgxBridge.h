#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

struct ID3D11Device;

class OptiScalerNgxBridge
{
public:
    struct ScanResult
    {
        std::size_t newly_scanned_modules = 0;
        bool exports_ready = false;
        std::vector<std::string> messages;
    };

    struct InitializationProbeResult
    {
        bool attempted = false;
        bool succeeded = false;
        std::uint32_t initialize_result = 0;
        std::uint32_t capability_result = 0;
        std::uint32_t destroy_parameters_result = 0;
        std::string initializer_name;
    };

    struct CapabilityProbeResult
    {
        bool attempted = false;
        bool succeeded = false;
        std::uint32_t capability_result = 0;
        std::uint32_t destroy_parameters_result = 0;
    };

    ScanResult scan_loaded_modules();
    InitializationProbeResult probe_initialization(ID3D11Device *device, const std::wstring &application_data_path);
    CapabilityProbeResult probe_capability_parameters();

private:
    struct ResolvedExports
    {
        HMODULE module = nullptr;
        void *initialize = nullptr;
        void *get_capability_parameters = nullptr;
        void *create_feature = nullptr;
        void *evaluate_feature = nullptr;
        void *release_feature = nullptr;
        void *destroy_parameters = nullptr;
        void *shutdown = nullptr;
        std::string initializer_name;
        std::string shutdown_name;
    };

    std::mutex mutex_;
    std::unordered_set<std::uintptr_t> scanned_modules_;
    ResolvedExports exports_;
    bool initialization_attempted_ = false;
    bool capability_probe_attempted_ = false;
};
