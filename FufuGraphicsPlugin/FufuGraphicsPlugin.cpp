#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace
{
HMODULE g_module = nullptr;
std::filesystem::path g_module_directory;
std::filesystem::path g_log_path;
std::mutex g_log_mutex;

constexpr std::array<const char *, 6> k_fsr2_exports {
    "ffxFsr2ContextCreate",
    "ffxFsr2ContextDispatch",
    "ffxFsr2ContextDestroy",
    "ffxFsr2GetUpscaleRatioFromQualityMode",
    "ffxFsr2GetRenderResolutionFromQualityMode",
    "ffxFsr2GetJitterPhaseCount",
};

struct BootstrapConfig
{
    bool enable_bridge = true;
    bool enable_optiscaler = true;
    bool enable_reshade = true;
    std::filesystem::path bridge_path;
    std::filesystem::path optiscaler_path;
    std::filesystem::path reshade_path;
    std::wstring trigger_module = L"WINTRUST.dll";
    DWORD timeout_ms = 30000;
    DWORD poll_interval_ms = 2;
    bool reject_game_directory_proxies = true;
};

std::string wide_to_utf8(const std::wstring_view value)
{
    if (value.empty())
        return {};
    const int length = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};
    std::string output(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), output.data(), length, nullptr, nullptr);
    return output;
}

std::wstring utf8_to_wide(const std::string_view value)
{
    if (value.empty())
        return {};
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring output(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), output.data(), length);
    return output;
}

std::wstring trim(std::wstring value)
{
    const auto is_space = [](const wchar_t c) { return std::iswspace(c) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"')
        value = value.substr(1, value.size() - 2);
    return value;
}

std::wstring lower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return value;
}

void write_log(const std::string &message)
{
    std::lock_guard lock(g_log_mutex);
    if (g_log_path.empty())
        return;

    SYSTEMTIME time {};
    GetLocalTime(&time);
    char prefix[96] {};
    _snprintf_s(
        prefix,
        _TRUNCATE,
        "%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu tid=%lu ",
        time.wYear,
        time.wMonth,
        time.wDay,
        time.wHour,
        time.wMinute,
        time.wSecond,
        time.wMilliseconds,
        GetCurrentProcessId(),
        GetCurrentThreadId());

    const std::string line = std::string(prefix) + message + "\r\n";
    HANDLE file = CreateFileW(
        g_log_path.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
}

void reset_log()
{
    HANDLE file = CreateFileW(
        g_log_path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
}

std::filesystem::path module_path(const HMODULE module)
{
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return {};
    return std::filesystem::path(std::wstring(buffer.data(), length));
}

bool file_exists(const std::filesystem::path &path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::filesystem::path absolute_from(const std::filesystem::path &base, const std::filesystem::path &path)
{
    const std::filesystem::path candidate = path.is_absolute() ? path : base / path;
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetFullPathNameW(candidate.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (length == 0 || length >= buffer.size())
        return candidate;
    return std::filesystem::path(std::wstring(buffer.data(), length));
}

std::unordered_map<std::wstring, std::wstring> read_paths_file(const std::filesystem::path &path)
{
    std::unordered_map<std::wstring, std::wstring> values;
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return values;

    LARGE_INTEGER size {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 1024 * 1024)
    {
        CloseHandle(file);
        return values;
    }
    std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL read_ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(file);
    if (!read_ok)
        return values;
    bytes.resize(read);
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF)
    {
        bytes.erase(0, 3);
    }

    std::wstring text = utf8_to_wide(bytes);
    std::size_t position = 0;
    while (position <= text.size())
    {
        const std::size_t end = text.find(L'\n', position);
        std::wstring line = trim(text.substr(position, end == std::wstring::npos ? end : end - position));
        if (!line.empty() && line.front() != L'#' && line.front() != L';')
        {
            const std::size_t separator = line.find(L'=');
            if (separator != std::wstring::npos)
            {
                std::wstring key = lower(trim(line.substr(0, separator)));
                std::wstring value = trim(line.substr(separator + 1));
                if (!key.empty())
                    values[key] = value;
            }
        }
        if (end == std::wstring::npos)
            break;
        position = end + 1;
    }
    return values;
}

std::unordered_map<std::wstring, std::wstring> read_fufu_settings(const std::filesystem::path &path)
{
    std::unordered_map<std::wstring, std::wstring> values;
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return values;

    LARGE_INTEGER size {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 1024 * 1024)
    {
        CloseHandle(file);
        return values;
    }
    std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL read_ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(file);
    if (!read_ok)
        return values;
    bytes.resize(read);
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF)
    {
        bytes.erase(0, 3);
    }

    const std::wstring text = utf8_to_wide(bytes);
    std::wstring section;
    std::size_t position = 0;
    while (position <= text.size())
    {
        const std::size_t end = text.find(L'\n', position);
        std::wstring line = trim(text.substr(position, end == std::wstring::npos ? end : end - position));
        if (!line.empty() && line.front() != L'#' && line.front() != L';')
        {
            if (line.size() >= 2 && line.front() == L'[' && line.back() == L']')
            {
                section = lower(trim(line.substr(1, line.size() - 2)));
            }
            else if (!section.empty() && section != L"general")
            {
                const std::size_t separator = line.find(L'=');
                if (separator != std::wstring::npos)
                {
                    const std::wstring key = lower(trim(line.substr(0, separator)));
                    if (key == L"value")
                        values[section] = trim(line.substr(separator + 1));
                }
            }
        }
        if (end == std::wstring::npos)
            break;
        position = end + 1;
    }
    return values;
}

DWORD parse_dword(const std::wstring &value, const DWORD fallback, const DWORD minimum, const DWORD maximum)
{
    wchar_t *end = nullptr;
    const unsigned long parsed = std::wcstoul(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != L'\0' || parsed < minimum || parsed > maximum)
        return fallback;
    return static_cast<DWORD>(parsed);
}

bool parse_bool(const std::wstring &value, const bool fallback)
{
    const std::wstring normalized = lower(trim(value));
    if (normalized == L"true" || normalized == L"1" || normalized == L"yes")
        return true;
    if (normalized == L"false" || normalized == L"0" || normalized == L"no")
        return false;
    return fallback;
}

std::filesystem::path first_existing(const std::vector<std::filesystem::path> &candidates)
{
    for (const auto &candidate : candidates)
    {
        const auto absolute = absolute_from(g_module_directory, candidate);
        if (file_exists(absolute))
            return absolute;
    }
    return {};
}

BootstrapConfig load_config()
{
    BootstrapConfig config;
    const std::filesystem::path config_path = g_module_directory / L"FufuBridgeBootstrap.paths";
    const auto values = read_paths_file(config_path);
    const auto find = [&values](const wchar_t *key) -> const std::wstring * {
        const auto iterator = values.find(lower(key));
        return iterator == values.end() ? nullptr : &iterator->second;
    };

    if (const auto *value = find(L"BridgePath"))
        config.bridge_path = absolute_from(g_module_directory, *value);
    if (const auto *value = find(L"OptiScalerPath"))
        config.optiscaler_path = absolute_from(g_module_directory, *value);
    if (const auto *value = find(L"TriggerModule"))
        config.trigger_module = *value;
    if (const auto *value = find(L"TimeoutMs"))
        config.timeout_ms = parse_dword(*value, config.timeout_ms, 1000, 120000);
    if (const auto *value = find(L"PollIntervalMs"))
        config.poll_interval_ms = parse_dword(*value, config.poll_interval_ms, 1, 1000);
    if (const auto *value = find(L"RejectGameDirectoryProxies"))
        config.reject_game_directory_proxies = parse_bool(*value, true);

    const auto fufu_values = read_fufu_settings(g_module_directory / L"config.ini");
    const auto find_fufu = [&fufu_values](const wchar_t *section) -> const std::wstring * {
        const auto iterator = fufu_values.find(lower(section));
        return iterator == fufu_values.end() ? nullptr : &iterator->second;
    };
    if (const auto *value = find_fufu(L"EnableBridge"))
        config.enable_bridge = parse_bool(*value, config.enable_bridge);
    if (const auto *value = find_fufu(L"EnableOptiScaler"))
        config.enable_optiscaler = parse_bool(*value, config.enable_optiscaler);
    if (const auto *value = find_fufu(L"EnableReShade"))
        config.enable_reshade = parse_bool(*value, config.enable_reshade);
    if (const auto *value = find_fufu(L"BridgePath"); value != nullptr && !trim(*value).empty())
        config.bridge_path = absolute_from(g_module_directory, trim(*value));
    if (const auto *value = find_fufu(L"OptiScalerPath"); value != nullptr && !trim(*value).empty())
        config.optiscaler_path = absolute_from(g_module_directory, trim(*value));
    if (const auto *value = find_fufu(L"ReShadePath"); value != nullptr && !trim(*value).empty())
        config.reshade_path = absolute_from(g_module_directory, trim(*value));
    if (const auto *value = find_fufu(L"TriggerModule"))
        config.trigger_module = trim(*value);
    if (const auto *value = find_fufu(L"TimeoutMs"))
        config.timeout_ms = parse_dword(*value, config.timeout_ms, 1000, 120000);
    if (const auto *value = find_fufu(L"RejectGameDirectoryProxies"))
        config.reject_game_directory_proxies = parse_bool(*value, config.reject_game_directory_proxies);

    if (config.bridge_path.empty())
    {
        config.bridge_path = first_existing({
            L"..\\..\\FSRGraphicsPayload\\Bridge\\Dx11FsrBridge.dll",
            L"Dx11FsrBridge.dll",
            L"Bridge\\Dx11FsrBridge.dll",
            L"..\\Bridge\\Dx11FsrBridge.dll",
            L"payload\\Bridge\\Dx11FsrBridge.dll",
            L"..\\payload\\Bridge\\Dx11FsrBridge.dll",
        });
    }
    if (config.optiscaler_path.empty())
    {
        config.optiscaler_path = first_existing({
            L"..\\..\\FSRGraphicsPayload\\OptiScaler\\OptiScaler.dll",
            L"OptiScaler.dll",
            L"OptiScaler\\OptiScaler.dll",
            L"..\\OptiScaler\\OptiScaler.dll",
            L"payload\\OptiScaler\\OptiScaler.dll",
            L"..\\payload\\OptiScaler\\OptiScaler.dll",
        });
    }
    if (config.reshade_path.empty())
    {
        config.reshade_path = first_existing({
            L"..\\..\\FSRGraphicsPayload\\ReShade\\ReShade64.dll",
            L"ReShade64.dll",
            L"ReShade\\ReShade64.dll",
            L"..\\ReShade\\ReShade64.dll",
            L"payload\\ReShade\\ReShade64.dll",
            L"..\\payload\\ReShade\\ReShade64.dll",
        });
    }
    return config;
}

bool is_target_process()
{
    const std::wstring filename = lower(module_path(nullptr).filename().wstring());
    return filename == L"yuanshen.exe" || filename == L"genshinimpact.exe";
}

bool wait_for_loader_trigger(const BootstrapConfig &config)
{
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < config.timeout_ms)
    {
        if (GetModuleHandleW(L"kernelbase.dll") != nullptr &&
            !config.trigger_module.empty() && GetModuleHandleW(config.trigger_module.c_str()) != nullptr)
        {
            write_log("process_loader_ready trigger=" + wide_to_utf8(config.trigger_module));
            return true;
        }
        Sleep(config.poll_interval_ms);
    }
    write_log("process_loader_timeout trigger=" + wide_to_utf8(config.trigger_module) +
        " timeout_ms=" + std::to_string(config.timeout_ms));
    return false;
}

bool has_game_directory_proxy()
{
    const std::filesystem::path executable_directory = module_path(nullptr).parent_path();
    const std::array<std::wstring, 3> proxy_names { L"dbghelp.dll", L"dxgi.dll", L"d3d11.dll" };
    for (const auto &name : proxy_names)
    {
        const std::filesystem::path proxy_path = executable_directory / name;
        if (file_exists(proxy_path))
        {
            write_log("proxy_file_collision path=" + wide_to_utf8(proxy_path.wstring()));
            return true;
        }
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;

    MODULEENTRY32W entry {};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Module32FirstW(snapshot, &entry))
    {
        do
        {
            const std::wstring name = lower(entry.szModule);
            if (std::find(proxy_names.begin(), proxy_names.end(), name) == proxy_names.end())
                continue;
            const std::filesystem::path path(entry.szExePath);
            if (lower(path.parent_path().wstring()) == lower(executable_directory.wstring()))
            {
                write_log("proxy_collision module=" + wide_to_utf8(path.wstring()));
                found = true;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

HMODULE load_module(const char *label, const std::filesystem::path &path)
{
    write_log(std::string(label) + "_load_begin path=" + wide_to_utf8(path.wstring()));
    SetLastError(ERROR_SUCCESS);
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr)
    {
        write_log(std::string(label) + "_load_failed win32=" + std::to_string(GetLastError()));
        return nullptr;
    }
    write_log(std::string(label) + "_load_success module=0x" + [&]() {
        char address[32] {};
        _snprintf_s(address, _TRUNCATE, "%llX", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(module)));
        return std::string(address);
    }());
    return module;
}

struct EnvironmentVariableBackup
{
    bool existed = false;
    std::wstring value;
};

EnvironmentVariableBackup set_environment_temporarily(const wchar_t *name, const std::wstring &value)
{
    EnvironmentVariableBackup backup;
    SetLastError(ERROR_SUCCESS);
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required != 0)
    {
        std::vector<wchar_t> buffer(required);
        const DWORD length = GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length < buffer.size())
        {
            backup.existed = true;
            backup.value.assign(buffer.data(), length);
        }
    }
    else if (GetLastError() != ERROR_ENVVAR_NOT_FOUND)
    {
        backup.existed = true;
    }

    SetEnvironmentVariableW(name, value.c_str());
    return backup;
}

void restore_environment(const wchar_t *name, const EnvironmentVariableBackup &backup)
{
    SetEnvironmentVariableW(name, backup.existed ? backup.value.c_str() : nullptr);
}

bool verify_bridge_shim(const HMODULE bridge)
{
    using get_proc_address_fn = decltype(&GetProcAddress);
    HMODULE kernel_base = GetModuleHandleW(L"kernelbase.dll");
    if (kernel_base == nullptr)
    {
        write_log("fsr2_shim_verify_failed reason=kernelbase_missing");
        return false;
    }
    const auto kernel_base_get_proc_address = reinterpret_cast<get_proc_address_fn>(
        GetProcAddress(kernel_base, "GetProcAddress"));
    if (kernel_base_get_proc_address == nullptr)
    {
        write_log("fsr2_shim_verify_failed reason=kernelbase_get_proc_address_missing");
        return false;
    }

    HMODULE executable = GetModuleHandleW(nullptr);
    std::size_t matched = 0;
    for (const char *name : k_fsr2_exports)
    {
        FARPROC bridge_export = kernel_base_get_proc_address(bridge, name);
        FARPROC executable_export = kernel_base_get_proc_address(executable, name);
        const bool match = bridge_export != nullptr && executable_export == bridge_export;
        write_log(std::string("shim_query name=") + name + " match=" + (match ? "1" : "0"));
        if (match)
            ++matched;
    }
    write_log("fsr2_shim_matches=" + std::to_string(matched) + "/" + std::to_string(k_fsr2_exports.size()));
    return matched == k_fsr2_exports.size();
}

bool export_entry_is_detoured(const FARPROC address)
{
    if (address == nullptr)
        return false;
    MEMORY_BASIC_INFORMATION information {};
    if (VirtualQuery(reinterpret_cast<const void *>(address), &information, sizeof(information)) == 0)
        return false;
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(address);
    return bytes[0] == 0xE9 || bytes[0] == 0xEB ||
        (bytes[0] == 0xFF && bytes[1] == 0x25) ||
        (bytes[0] == 0x48 && bytes[1] == 0xB8);
}

bool verify_optiscaler_hooks(const HMODULE bridge)
{
    const bool create_hooked = export_entry_is_detoured(GetProcAddress(bridge, "ffxFsr2ContextCreate"));
    const bool dispatch_hooked = export_entry_is_detoured(GetProcAddress(bridge, "ffxFsr2ContextDispatch"));
    const bool destroy_hooked = export_entry_is_detoured(GetProcAddress(bridge, "ffxFsr2ContextDestroy"));
    write_log(std::string("optiscaler_fsr_hooks create=") + (create_hooked ? "1" : "0") +
        " dispatch=" + (dispatch_hooked ? "1" : "0") +
        " destroy=" + (destroy_hooked ? "1" : "0"));
    return create_hooked && dispatch_hooked;
}

DWORD WINAPI bootstrap_thread(void *)
{
    const auto self_path = module_path(g_module);
    g_module_directory = self_path.parent_path();
    g_log_path = g_module_directory / L"FSR-Bridge-Plugin.log";
    reset_log();
    write_log("plugin_loaded version=0.2.1 path=" + wide_to_utf8(self_path.wstring()));

    if (!is_target_process())
    {
        write_log("unsupported_process path=" + wide_to_utf8(module_path(nullptr).wstring()));
        return 1;
    }

    const BootstrapConfig config = load_config();
    write_log(std::string("settings bridge=") + (config.enable_bridge ? "1" : "0") +
        " optiscaler=" + (config.enable_optiscaler ? "1" : "0") +
        " reshade=" + (config.enable_reshade ? "1" : "0"));
    if (config.enable_optiscaler && !config.enable_bridge)
    {
        write_log("plugin_stopped reason=optiscaler_requires_bridge");
        return 2;
    }
    if (config.enable_bridge)
        write_log("bridge_path=" + wide_to_utf8(config.bridge_path.wstring()));
    if (config.enable_optiscaler)
        write_log("optiscaler_path=" + wide_to_utf8(config.optiscaler_path.wstring()));
    if (config.enable_reshade)
        write_log("reshade_path=" + wide_to_utf8(config.reshade_path.wstring()));
    if (config.enable_bridge && (config.bridge_path.empty() || !file_exists(config.bridge_path)))
    {
        write_log("bridge_path_invalid");
        return 3;
    }
    if (config.enable_optiscaler && (config.optiscaler_path.empty() || !file_exists(config.optiscaler_path)))
    {
        write_log("optiscaler_path_invalid");
        return 4;
    }
    if (config.enable_reshade && (config.reshade_path.empty() || !file_exists(config.reshade_path)))
    {
        write_log("reshade_path_invalid");
        return 5;
    }
    if (!config.enable_bridge && !config.enable_optiscaler && !config.enable_reshade)
    {
        write_log("plugin_success no_components_enabled");
        return 0;
    }
    if (!wait_for_loader_trigger(config))
        return 6;
    if (config.reject_game_directory_proxies && has_game_directory_proxy())
    {
        write_log("plugin_stopped reason=game_directory_proxy_loaded");
        return 7;
    }

    HMODULE bridge = nullptr;
    if (config.enable_bridge)
    {
        if (GetModuleHandleW(L"Dx11FsrBridge.dll") != nullptr)
        {
            write_log("plugin_stopped reason=bridge_already_loaded");
            return 8;
        }
        bridge = load_module("bridge", config.bridge_path);
        if (bridge == nullptr)
            return 9;
        if (!verify_bridge_shim(bridge))
        {
            write_log("plugin_stopped reason=fsr2_shim_not_ready");
            return 10;
        }
    }

    if (config.enable_optiscaler)
    {
        if (GetModuleHandleW(L"OptiScaler.dll") != nullptr)
        {
            write_log("plugin_stopped reason=optiscaler_already_loaded");
            return 11;
        }
        HMODULE optiscaler = load_module("optiscaler", config.optiscaler_path);
        if (optiscaler == nullptr)
            return 12;
        if (!verify_optiscaler_hooks(bridge))
        {
            write_log("plugin_stopped reason=optiscaler_fsr_hooks_missing");
            return 13;
        }
    }

    if (config.enable_reshade)
    {
        if (GetModuleHandleW(L"ReShade64.dll") != nullptr)
        {
            write_log("plugin_stopped reason=reshade_already_loaded");
            return 14;
        }
        const std::filesystem::path reshade_directory = config.reshade_path.parent_path();
        write_log("reshade_base_path_override=" + wide_to_utf8(reshade_directory.wstring()));
        const EnvironmentVariableBackup base_path_backup =
            set_environment_temporarily(L"RESHADE_BASE_PATH_OVERRIDE", reshade_directory.wstring());
        HMODULE reshade = load_module("reshade", config.reshade_path);
        restore_environment(L"RESHADE_BASE_PATH_OVERRIDE", base_path_backup);
        if (reshade == nullptr)
            return 15;
    }

    write_log(std::string("plugin_success bridge=") + (config.enable_bridge ? "1" : "0") +
        " optiscaler=" + (config.enable_optiscaler ? "1" : "0") +
        " reshade=" + (config.enable_reshade ? "1" : "0") +
        " order=bridge,optiscaler,reshade");
    return 0;
}
} // namespace

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, bootstrap_thread, nullptr, 0, nullptr);
        if (thread != nullptr)
            CloseHandle(thread);
    }
    return TRUE;
}
