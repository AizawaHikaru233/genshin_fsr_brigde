#include <windows.h>
#include <tlhelp32.h>
#include <dxgi.h>

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
    bool reset_configurations = false;
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
    static HANDLE cached_handle = INVALID_HANDLE_VALUE;
    if (cached_handle == INVALID_HANDLE_VALUE)
    {
        cached_handle = CreateFileW(
            g_log_path.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (cached_handle == INVALID_HANDLE_VALUE)
            return;
    }
    DWORD written = 0;
    WriteFile(cached_handle, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
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
    wchar_t buffer[32768];
    const DWORD length = GetModuleFileNameW(module, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer))
        return {};
    return std::filesystem::path(std::wstring(buffer, length));
}

bool file_exists(const std::filesystem::path &path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::filesystem::path absolute_from(const std::filesystem::path &base, const std::filesystem::path &path)
{
    const std::filesystem::path candidate = path.is_absolute() ? path : base / path;
    wchar_t buffer[32768];
    const DWORD length = GetFullPathNameW(candidate.c_str(), static_cast<DWORD>(std::size(buffer)), buffer, nullptr);
    if (length == 0 || length >= std::size(buffer))
        return candidate;
    return std::filesystem::path(std::wstring(buffer, length));
}

std::string trim_ascii(std::string value)
{
    const auto is_space = [](const unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
    return value;
}

std::string lower_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool read_utf8_file(const std::filesystem::path &path, std::string &bytes)
{
    bytes.clear();
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 4 * 1024 * 1024)
    {
        CloseHandle(file);
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = bytes.empty() || ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(file);
    if (!ok)
    {
        bytes.clear();
        return false;
    }
    bytes.resize(read);
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF)
    {
        bytes.erase(0, 3);
    }
    return true;
}

bool write_utf8_file(const std::filesystem::path &path, const std::string &bytes)
{
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const BOOL ok = bytes.empty() || WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
    return ok && written == bytes.size();
}

std::vector<std::string> split_ini_lines(const std::string &text)
{
    std::vector<std::string> lines;
    std::size_t position = 0;
    while (position <= text.size())
    {
        const std::size_t end = text.find('\n', position);
        std::string line = text.substr(position, end == std::string::npos ? end : end - position);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(std::move(line));
        if (end == std::string::npos)
            break;
        position = end + 1;
    }
    if (!lines.empty() && lines.back().empty())
        lines.pop_back();
    return lines;
}

bool ini_has_key_utf8(const std::filesystem::path &path, const std::string &section, const std::string &key)
{
    std::string bytes;
    if (!read_utf8_file(path, bytes))
        return false;
    const std::string wanted_section = lower_ascii(section);
    const std::string wanted_key = lower_ascii(key);
    std::string current_section;
    for (const std::string &raw_line : split_ini_lines(bytes))
    {
        const std::string line = trim_ascii(raw_line);
        if (line.size() >= 2 && line.front() == '[' && line.back() == ']')
        {
            current_section = lower_ascii(trim_ascii(line.substr(1, line.size() - 2)));
            continue;
        }
        if (current_section != wanted_section || line.empty() || line.front() == ';' || line.front() == '#')
            continue;
        const std::size_t separator = line.find('=');
        if (separator != std::string::npos &&
            lower_ascii(trim_ascii(line.substr(0, separator))) == wanted_key)
            return true;
    }
    return false;
}

bool set_ini_value_utf8(
    const std::filesystem::path &path,
    const std::string &section,
    const std::string &key,
    const std::string &value)
{
    std::string bytes;
    if (!read_utf8_file(path, bytes))
        return false;
    std::vector<std::string> lines = split_ini_lines(bytes);
    const std::string wanted_section = lower_ascii(section);
    const std::string wanted_key = lower_ascii(key);
    std::size_t section_begin = lines.size();
    std::size_t section_end = lines.size();
    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        const std::string line = trim_ascii(lines[index]);
        if (line.size() < 2 || line.front() != '[' || line.back() != ']')
            continue;
        const std::string current = lower_ascii(trim_ascii(line.substr(1, line.size() - 2)));
        if (section_begin != lines.size())
        {
            section_end = index;
            break;
        }
        if (current == wanted_section)
            section_begin = index;
    }

    if (section_begin == lines.size())
    {
        if (!lines.empty() && !lines.back().empty())
            lines.push_back({});
        lines.push_back("[" + section + "]");
        lines.push_back(key + "=" + value);
    }
    else
    {
        std::size_t key_index = lines.size();
        for (std::size_t index = section_begin + 1; index < section_end; ++index)
        {
            const std::string line = trim_ascii(lines[index]);
            if (line.empty() || line.front() == ';' || line.front() == '#')
                continue;
            const std::size_t separator = line.find('=');
            if (separator != std::string::npos &&
                lower_ascii(trim_ascii(line.substr(0, separator))) == wanted_key)
            {
                key_index = index;
                break;
            }
        }
        const std::string replacement = key + "=" + value;
        if (key_index != lines.size())
        {
            const std::string current = trim_ascii(lines[key_index]);
            const std::size_t eq = current.find('=');
            if (eq != std::string::npos && trim_ascii(current.substr(eq + 1)) == value)
                return true; // 值未变化：不重写文件
            lines[key_index] = replacement;
        }
        else
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(section_end), replacement);
    }

    std::string output;
    for (const auto &line : lines)
        output += line + "\r\n";
    return write_utf8_file(path, output);
}

bool copy_file_replace(const std::filesystem::path &source, const std::filesystem::path &destination)
{
    return file_exists(source) && CopyFileW(source.c_str(), destination.c_str(), FALSE) != FALSE;
}

bool prepare_reshade_game_configuration(const BootstrapConfig &config)
{
    const std::filesystem::path game_directory = module_path(nullptr).parent_path();
    const std::filesystem::path reshade_directory = config.reshade_path.parent_path();
    const std::filesystem::path default_directory = g_module_directory / L"payload" / L"default_config";
    const std::filesystem::path game_ini = game_directory / L"ReShade.ini";
    const std::filesystem::path game_preset = game_directory / L"ReShadePreset.ini";
    const std::filesystem::path legacy_ini = reshade_directory / L"ReShade.ini";
    const std::filesystem::path legacy_preset = reshade_directory / L"ReShadePreset.ini";
    const bool redirect_config = ini_has_key_utf8(game_ini, "INSTALL", "BasePath");

    if (!file_exists(game_ini) || redirect_config)
    {
        const std::filesystem::path source = file_exists(legacy_ini)
            ? legacy_ini : default_directory / L"ReShade.ini";
        if (!copy_file_replace(source, game_ini))
        {
            write_log("reshade_game_config_failed reason=ini_copy source=" + wide_to_utf8(source.wstring()));
            return false;
        }
    }
    if (!file_exists(game_preset))
    {
        const std::filesystem::path source = file_exists(legacy_preset)
            ? legacy_preset : default_directory / L"ReShadePreset.ini";
        if (!copy_file_replace(source, game_preset))
        {
            write_log("reshade_game_config_failed reason=preset_copy source=" + wide_to_utf8(source.wstring()));
            return false;
        }
    }

    const std::filesystem::path shader_root = reshade_directory / L"reshade-shaders";
    const std::filesystem::path screenshots = game_directory / L"Screenshots";
    CreateDirectoryW(screenshots.c_str(), nullptr);
    const bool configured =
        set_ini_value_utf8(game_ini, "ADDON", "AddonPath",
            wide_to_utf8((shader_root / L"Addons").wstring())) &&
        set_ini_value_utf8(game_ini, "GENERAL", "EffectSearchPaths",
            wide_to_utf8((shader_root / L"Shaders").wstring())) &&
        set_ini_value_utf8(game_ini, "GENERAL", "TextureSearchPaths",
            wide_to_utf8((shader_root / L"Textures").wstring())) &&
        set_ini_value_utf8(game_ini, "GENERAL", "PresetPath", ".\\ReShadePreset.ini") &&
        set_ini_value_utf8(game_ini, "SCREENSHOT", "SavePath", ".\\Screenshots");
    if (!configured)
    {
        write_log("reshade_game_config_failed reason=ini_update path=" + wide_to_utf8(game_ini.wstring()));
        return false;
    }

    write_log("reshade_game_config_ready ini=" + wide_to_utf8(game_ini.wstring()) +
        " log_directory=" + wide_to_utf8(game_directory.wstring()) +
        " effects=" + wide_to_utf8(shader_root.wstring()));
    return true;
}

std::string read_policy_value(
    const std::filesystem::path &path,
    const wchar_t *key,
    const wchar_t *fallback)
{
    wchar_t value[64] {};
    GetPrivateProfileStringW(L"FSR4Policy", key, fallback, value, static_cast<DWORD>(std::size(value)), path.c_str());
    return wide_to_utf8(value);
}

struct DetectedFsr4Policy
{
    bool supported = false;
    bool prefer_fp8 = false;
    std::wstring gpu_name;
};

// 系列级匹配：在名称中找 marker（如 "RTX"），跳过空格后要求以某个系列前缀开头
// 且后随数字。这样 "RTX 4070 Ti SUPER"、"RTX 4090D"、"RX 7600M XT" 等任意后缀
// 变体都能命中，而 "RTX 3500 Ada"（35 非 30）、"RX 590"（5 非 9）不会误配。
bool name_contains_series(
    const std::wstring &name,
    const wchar_t *marker,
    std::initializer_list<const wchar_t *> series_prefixes)
{
    const std::wstring normalized_name = lower(name);
    const std::wstring normalized_marker = lower(marker);
    const std::size_t marker_length = normalized_marker.size();
    std::size_t position = normalized_name.find(normalized_marker);
    while (position != std::wstring::npos)
    {
        if (position == 0 || !iswalnum(normalized_name[position - 1]))
        {
            std::size_t cursor = position + marker_length;
            while (cursor < normalized_name.size() && normalized_name[cursor] == L' ')
                ++cursor;
            for (const wchar_t *prefix : series_prefixes)
            {
                const std::wstring normalized_prefix = lower(prefix);
                const std::size_t prefix_length = normalized_prefix.size();
                if (normalized_name.compare(cursor, prefix_length, normalized_prefix) == 0 &&
                    cursor + prefix_length < normalized_name.size() &&
                    normalized_name[cursor + prefix_length] >= L'0' &&
                    normalized_name[cursor + prefix_length] <= L'9')
                {
                    return true;
                }
            }
        }
        position = normalized_name.find(normalized_marker, position + marker_length);
    }
    return false;
}

// 增加按 Device ID 精确分类（核显名字通常只是
// "AMD Radeon(TM) Graphics"，不含型号，名字匹配识别不到——Device ID 才是权威）。
enum class Fsr4GpuClass { Unsupported, Fp8, Int8 };

// AMD（VEN_1002）FSR4 分类：RDNA4 独显 → FP8；RDNA3/3.5 独显与核显 → INT8。
// RDNA2 暂不处理（FSR4.0.2c 文件在线获取渠道尚未就绪）。
// Device ID 来源于 GPU 硬件 ID 汇总表（AMD RDNA2-4）。
static Fsr4GpuClass classify_amd_fsr4(std::uint32_t device_id)
{
    static constexpr std::uint32_t fp8_ids[] = {
        0x7550, 0x7551, 0x7590, // RDNA4 Navi 48/44: RX 9070/9070 XT/9060 XT/9050, AI PRO R9700
    };
    static constexpr std::uint32_t int8_ids[] = {
        // RDNA3 dGPU (RX 7000, Navi 3x)
        0x744C, 0x7448, 0x7449, 0x744A, 0x744B, 0x745E, 0x7460, 0x7461,
        0x7470, 0x747E, 0x73F0, 0x7480, 0x7481, 0x7483, 0x7487, 0x748B,
        0x7489, 0x7499, 0x749F,
        // RDNA3 iGPU (740M/760M/780M)
        0x15BF, 0x15C8, 0x164F, 0x1900, 0x1901,
        // RDNA3.5 iGPU (840M/860M/880M/890M/8050S/8060S)
        0x150E, 0x1586, 0x1114, 0x1902,
    };
    for (const std::uint32_t id : fp8_ids)
        if (device_id == id)
            return Fsr4GpuClass::Fp8;
    for (const std::uint32_t id : int8_ids)
        if (device_id == id)
            return Fsr4GpuClass::Int8;
    return Fsr4GpuClass::Unsupported;
}

// NVIDIA（VEN_10DE）16-50 系（GTX 16 + RTX 20/30/40/50）→ INT8。
// Device ID 来源于 GPU 硬件 ID 汇总表（NVIDIA RTX 16-50 系）。
static bool is_nvidia_int8_device(std::uint32_t device_id)
{
    static constexpr std::uint32_t ids[] = {
        // Turing: RTX 20 / GTX 16
        0x1E02, 0x1E03, 0x1E04, 0x1E07, 0x1E81, 0x1E82, 0x1E84, 0x1E87, 0x1E89,
        0x1EC2, 0x1EC7, 0x1F02, 0x1F03, 0x1F06, 0x1F07, 0x1F08, 0x1F42, 0x1F47,
        0x2182, 0x2184, 0x2187, 0x2188, 0x21C4, 0x1F82, 0x1F83,
        0x1E90, 0x1ED0, 0x1E91, 0x1ED1, 0x1E93, 0x1ED3, 0x1F10, 0x1F50, 0x1F54,
        0x1F11, 0x1F15, 0x1F51, 0x1F55, 0x1F12, 0x1F14,
        0x2191, 0x21D1, 0x2192, 0x1F91, 0x1F92, 0x1F94, 0x1F96, 0x1F99, 0x1F9D,
        0x1F95, 0x1FD9, 0x1FDD, 0x1F97, 0x1F98, 0x1F9C, 0x1F9F, 0x1FA0,
        0x1E30, 0x1FB0, 0x1FF0, 0x1FB1, 0x1FB2, 0x1FF2, 0x1FB6, 0x1FBA, 0x1FB7,
        0x1FBB, 0x1FBC, 0x1FB8, 0x1FB9, 0x1FF9,
        // Ampere: RTX 30
        0x2203, 0x2204, 0x2205, 0x2206, 0x2207, 0x2208, 0x220A, 0x2216,
        0x2414, 0x2482, 0x2484, 0x2486, 0x2487, 0x2488, 0x2489, 0x248C, 0x248D, 0x248E,
        0x24C7, 0x24C8, 0x24C9, 0x2501, 0x2503, 0x2509, 0x2544, 0x2508,
        0x2582, 0x2583, 0x2584, 0x25A7, 0x25A9, 0x25AD, 0x25ED, 0x25A6, 0x25AA,
        0x2420, 0x2460, 0x249C, 0x24DC, 0x249D, 0x24DD, 0x2520, 0x2521, 0x2560, 0x2561,
        0x2523, 0x2563, 0x25A0, 0x25E0, 0x25A2, 0x25A5, 0x25E2, 0x25E5, 0x25AB, 0x25AC, 0x25EC,
        0x2230, 0x2231, 0x2232, 0x2233, 0x24B0, 0x2531, 0x2571, 0x25B0, 0x25B2, 0x25B6,
        0x2438, 0x24B6, 0x24B7, 0x24B8, 0x24B9, 0x24BB, 0x24BA, 0x25B8, 0x25B9, 0x25BA,
        0x25BB, 0x25BD, 0x25BC,
        // Ada: RTX 40
        0x2684, 0x2685, 0x2702, 0x2704, 0x2705, 0x2709, 0x2782, 0x2783, 0x2786, 0x2788,
        0x2803, 0x2805, 0x2808, 0x2882, 0x2717, 0x2757, 0x27A0, 0x27E0,
        0x2820, 0x2860, 0x28A0, 0x28E0, 0x28A1, 0x28E1, 0x2822, 0x28A3, 0x28E3,
        0x26B1, 0x26B2, 0x26B3, 0x27B0, 0x27B1, 0x27B2, 0x28B0,
        0x2730, 0x27BA, 0x27BB, 0x28B8, 0x28B9, 0x28BA, 0x28BB,
        // Blackwell: RTX 50
        0x2B85, 0x2B87, 0x2B8C, 0x2C02, 0x2C05, 0x2F04, 0x2F06, 0x2D04, 0x2D05, 0x2D83,
        0x2C18, 0x2C58, 0x2C19, 0x2C59, 0x2F18, 0x2F58, 0x2D18, 0x2D58, 0x2D19, 0x2D59,
        0x2D98, 0x2DD8, 0x2BB1, 0x2BB4,
    };
    for (const std::uint32_t id : ids)
        if (device_id == id)
            return true;
    return false;
}

// NVIDIA RTX（20/30/40/50，不含 GTX 16）：DLSS Runtime 只对 RTX 部署。
static bool is_nvidia_rtx_device(std::uint32_t device_id)
{
    // GTX 16（Turing TU116/TU117，含 MX450/MX550）不算 RTX。
    static constexpr std::uint32_t gtx16_ids[] = {
        0x2182, 0x2184, 0x2187, 0x2188, 0x21C4, 0x1F82, 0x1F83,
        0x2191, 0x21D1, 0x2192, 0x1F91, 0x1F92, 0x1F94, 0x1F96, 0x1F99, 0x1F9D,
        0x1F95, 0x1FD9, 0x1FDD, 0x1F97, 0x1F98, 0x1F9C, 0x1F9F, 0x1FA0,
    };
    for (const std::uint32_t id : gtx16_ids)
        if (device_id == id)
            return false;
    return is_nvidia_int8_device(device_id);
}

// Intel Arc 独显（VEN_8086）：Alchemist 0x5600-0x56FF + Battlemage 0xE200-0xE2FF。
static bool is_intel_arc_device(std::uint32_t device_id)
{
    return (device_id >= 0x5600 && device_id <= 0x56FF) ||
           (device_id >= 0xE200 && device_id <= 0xE2FF);
}

DetectedFsr4Policy detect_fsr4_gpu_policy()
{
    DetectedFsr4Policy best {};
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) ||
        factory == nullptr)
    {
        return best;
    }

    for (UINT adapter_index = 0;; ++adapter_index)
    {
        IDXGIAdapter1 *adapter = nullptr;
        if (factory->EnumAdapters1(adapter_index, &adapter) != S_OK || adapter == nullptr)
            break;
        DXGI_ADAPTER_DESC1 desc {};
        const HRESULT desc_result = adapter->GetDesc1(&desc);
        adapter->Release();
        if (FAILED(desc_result) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
            continue;

        const std::wstring name = desc.Description;
        bool fp8 = false;
        bool int8 = false;
        // Device ID 精确分类优先（核显/独显都覆盖），名字匹配仅作变体兜底。
        if (desc.VendorId == 0x10DE)
        {
            int8 = is_nvidia_int8_device(desc.DeviceId);
        }
        else if (desc.VendorId == 0x1002)
        {
            const Fsr4GpuClass cls = classify_amd_fsr4(desc.DeviceId);
            fp8 = (cls == Fsr4GpuClass::Fp8);
            int8 = (cls == Fsr4GpuClass::Int8);
        }
        else if (desc.VendorId == 0x8086)
        {
            int8 = is_intel_arc_device(desc.DeviceId) ||
                   lower(name).find(L"arc") != std::wstring::npos;
        }

        // 名字兜底：Device ID 未命中时按系列前缀宽松匹配（型号变体/未收录 ID）。
        if (!fp8 && !int8)
        {
            const std::wstring normalized_name = lower(name);
            if (desc.VendorId == 0x10DE || normalized_name.find(L"nvidia") != std::wstring::npos ||
                normalized_name.find(L"geforce") != std::wstring::npos)
            {
                int8 = name_contains_series(name, L"RTX", { L"20", L"30", L"40", L"50" }) ||
                    name_contains_series(name, L"GTX", { L"16" });
            }
            else if (desc.VendorId == 0x1002 || normalized_name.find(L"amd") != std::wstring::npos ||
                normalized_name.find(L"radeon") != std::wstring::npos)
            {
                if (name_contains_series(name, L"RX", { L"9" }) || name_contains_series(name, L"PRO", { L"W9" }))
                    fp8 = true;
                else if (name_contains_series(name, L"RX", { L"7" }) || name_contains_series(name, L"PRO", { L"W7" }))
                    int8 = true;
            }
        }

        if (!fp8 && !int8)
            continue;
        if (!best.supported || (fp8 && !best.prefer_fp8))
        {
            best.supported = true;
            best.prefer_fp8 = fp8;
            best.gpu_name = name;
        }
        if (best.prefer_fp8)
            break;
    }

    factory->Release();
    return best;
}

// DLSS Runtime 仅在 RTX 上自动补齐。GTX 16 可以运行 FSR4 INT8，
// 但不能以此推断它需要或支持 DLSS，因此不复用 FSR4 的策略判断。
bool has_nvidia_rtx_adapter()
{
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) ||
        factory == nullptr)
    {
        return false;
    }

    bool has_rtx = false;
    for (UINT adapter_index = 0;; ++adapter_index)
    {
        IDXGIAdapter1 *adapter = nullptr;
        if (factory->EnumAdapters1(adapter_index, &adapter) != S_OK || adapter == nullptr)
            break;

        DXGI_ADAPTER_DESC1 desc {};
        const HRESULT desc_result = adapter->GetDesc1(&desc);
        adapter->Release();
        if (FAILED(desc_result) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 ||
            desc.VendorId != 0x10DE)
        {
            continue;
        }

        // RTX 精确判定：Device ID 属 RTX 20/30/40/50（排除 GTX 16）。
        // 非 RTX 的 NVIDIA（GTX 16）不部署 DLSS Runtime。
        if (is_nvidia_rtx_device(desc.DeviceId))
        {
            has_rtx = true;
            break;
        }
        // 名字兜底（未收录 Device ID 的变体）。
        if (lower(desc.Description).find(L"rtx") != std::wstring::npos)
        {
            has_rtx = true;
            break;
        }
    }

    factory->Release();
    return has_rtx;
}

bool apply_optiscaler_managed_settings(
    const std::filesystem::path &ini_path,
    const std::filesystem::path &optiscaler_directory)
{
    const std::filesystem::path policy_path = g_module_directory / L"FSR4Policy.ini";
    std::string fsr4_update = read_policy_value(policy_path, L"Fsr4Update", L"auto");
    std::string upscaler_index = read_policy_value(policy_path, L"UpscalerIndex", L"auto");
    std::string force_int8 = read_policy_value(policy_path, L"Fsr4ForceEnableInt8", L"auto");

    // 策略值为 auto（安装脚本未识别型号或策略文件缺失）时，在此按 PCI 厂商 +
    // 系列前缀宽松匹配解析：NVIDIA RTX 20/30/40/50、AMD RX 7/RX 9 与 RDNA3/3.5
    // 核显、Intel Arc 全部放行。非 auto 的显式值（安装脚本命中或用户手改）优先。
    if (fsr4_update == "auto" || upscaler_index == "auto" || force_int8 == "auto")
    {
        const DetectedFsr4Policy detected = detect_fsr4_gpu_policy();
        if (detected.supported)
        {
            if (fsr4_update == "auto")
                fsr4_update = "true";
            if (upscaler_index == "auto")
                upscaler_index = "0";
            if (force_int8 == "auto")
                force_int8 = detected.prefer_fp8 ? "false" : "true";
            write_log("fsr4_policy_auto_resolved gpu=" + wide_to_utf8(detected.gpu_name) +
                " mode=" + (detected.prefer_fp8 ? "fp8" : "int8"));
        }
        else
        {
            write_log("fsr4_policy_auto_unresolved keeping_auto_defaults");
        }
    }

    // 与 FPS 安装脚本（Configure.ps1 Set-OptiScalerManagedSettings）保持同一收敛集：
    // FSR4 策略 + OptiDllPath + Log 三项 + 帧生成版型（芙芙场景固定非帧生成）。
    // Upscalers/Inputs/Plugins 等其余设置由 default_config 模板自足，不再由脚本覆写。
    const struct ManagedSetting
    {
        const char *section;
        const char *key;
        std::string value;
    } settings[] {
        { "FSR", "UpscalerIndex", upscaler_index },
        { "FSR", "Fsr4Update", fsr4_update },
        { "FSR", "Fsr4ForceEnableInt8", force_int8 },
        // FSR4 uses the verified non-linear input path on every GPU.
        // This setting is inert for DLSS/XeSS and other backends.
        { "FSR", "FsrNonLinearColorSpace", "true" },
        { "Libraries", "OptiDllPath", wide_to_utf8(optiscaler_directory.wstring()) },
        { "Log", "LogToFile", "true" },
        { "Log", "LogLevel", "2" },
        { "Log", "LogFileName", "OptiScaler.log" },
        { "FrameGen", "Enabled", "false" },
        { "FrameGen", "FGInput", "nofg" },
        { "FrameGen", "FGOutput", "nofg" },
    };

    for (const auto &setting : settings)
    {
        if (!set_ini_value_utf8(ini_path, setting.section, setting.key, setting.value))
            return false;
    }
    write_log("optiscaler_config_managed_settings_ready ini=" + wide_to_utf8(ini_path.wstring()) +
        " fsr4_update=" + fsr4_update + " upscaler_index=" + upscaler_index +
        " force_int8=" + force_int8);
    return true;
}

bool ensure_missing_component_configurations(const BootstrapConfig &config)
{
    const std::filesystem::path default_directory = g_module_directory / L"payload" / L"default_config";
    bool success = true;
    if (!config.bridge_path.empty())
    {
        const std::filesystem::path bridge_ini = config.bridge_path.parent_path() / L"Dx11FsrBridge.ini";
        if (!file_exists(bridge_ini) && !copy_file_replace(default_directory / L"Dx11FsrBridge.ini", bridge_ini))
        {
            write_log("config_initialize_failed component=bridge");
            success = false;
        }
    }
    if (!config.optiscaler_path.empty())
    {
        const std::filesystem::path optiscaler_directory = config.optiscaler_path.parent_path();
        const std::filesystem::path optiscaler_ini = optiscaler_directory / L"OptiScaler.ini";
        if (!file_exists(optiscaler_ini))
        {
            if (!copy_file_replace(default_directory / L"OptiScaler.ini", optiscaler_ini) ||
                !apply_optiscaler_managed_settings(optiscaler_ini, optiscaler_directory))
            {
                write_log("config_initialize_failed component=optiscaler");
                success = false;
            }
        }
        const std::filesystem::path nvidia_directory =
            g_module_directory / L"payload" / L"NVIDIA" / L"DLSS";
        const std::filesystem::path dlss_destination = optiscaler_directory / L"nvngx_dlss.dll";
        // 先做文件级短路，命中缺失且包内含组件时才枚举 GPU（避免每次启动 DXGI 枚举）。
        if (!file_exists(dlss_destination) &&
            file_exists(nvidia_directory / L"nvngx_dlss.dll") &&
            has_nvidia_rtx_adapter())
        {
            copy_file_replace(nvidia_directory / L"nvngx_dlss.dll", dlss_destination);
            if (file_exists(nvidia_directory / L"nvngx_dlss.license.txt"))
                copy_file_replace(
                    nvidia_directory / L"nvngx_dlss.license.txt",
                    optiscaler_directory / L"nvngx_dlss.license.txt");
        }
    }
    return success;
}

bool reset_all_configurations(const BootstrapConfig &config)
{
    const std::filesystem::path default_directory = g_module_directory / L"payload" / L"default_config";
    const std::filesystem::path game_directory = module_path(nullptr).parent_path();
    bool success = true;

    const auto reset_file = [&success](
        const std::filesystem::path &source,
        const std::filesystem::path &destination,
        const char *label) {
        if (!copy_file_replace(source, destination))
        {
            write_log(std::string("config_reset_failed component=") + label +
                " source=" + wide_to_utf8(source.wstring()) +
                " destination=" + wide_to_utf8(destination.wstring()));
            success = false;
            return;
        }
        write_log(std::string("config_reset_file component=") + label +
            " destination=" + wide_to_utf8(destination.wstring()));
    };

    if (!config.bridge_path.empty())
    {
        reset_file(
            default_directory / L"Dx11FsrBridge.ini",
            config.bridge_path.parent_path() / L"Dx11FsrBridge.ini",
            "bridge");
    }

    if (!config.optiscaler_path.empty())
    {
        const std::filesystem::path optiscaler_directory = config.optiscaler_path.parent_path();
        const std::filesystem::path optiscaler_ini = optiscaler_directory / L"OptiScaler.ini";
        reset_file(default_directory / L"OptiScaler.ini", optiscaler_ini, "optiscaler");
        if (file_exists(optiscaler_ini) && !apply_optiscaler_managed_settings(
                optiscaler_ini, optiscaler_directory))
        {
            write_log("config_reset_failed component=optiscaler reason=dll_path_update");
            success = false;
        }

        const std::filesystem::path fakenvapi_default = optiscaler_directory / L"fakenvapi.default.ini";
        if (file_exists(fakenvapi_default))
            reset_file(fakenvapi_default, optiscaler_directory / L"fakenvapi.ini", "fakenvapi");

        const std::filesystem::path nvidia_directory =
            g_module_directory / L"payload" / L"NVIDIA" / L"DLSS";
        const std::filesystem::path dlss_dll = nvidia_directory / L"nvngx_dlss.dll";
        const std::filesystem::path dlss_license = nvidia_directory / L"nvngx_dlss.license.txt";
        if (!has_nvidia_rtx_adapter())
        {
            write_log("config_reset_nvidia_skipped reason=rtx_not_detected");
        }
        else
        {
            if (file_exists(dlss_dll))
                reset_file(dlss_dll, optiscaler_directory / L"nvngx_dlss.dll", "nvidia_dlss");
            else
                write_log("config_reset_nvidia_skipped reason=nvngx_dlss_missing");
            if (file_exists(dlss_license))
                reset_file(dlss_license, optiscaler_directory / L"nvngx_dlss.license.txt", "nvidia_dlss_license");
        }
    }

    reset_file(default_directory / L"ReShade.ini", game_directory / L"ReShade.ini", "reshade");
    reset_file(default_directory / L"ReShadePreset.ini", game_directory / L"ReShadePreset.ini", "reshade_preset");
    if (!config.reshade_path.empty() && !prepare_reshade_game_configuration(config))
        success = false;

    if (!set_ini_value_utf8(
            g_module_directory / L"config.ini", "ResetConfigurations", "Value", "0"))
    {
        write_log("config_reset_failed component=plugin reason=clear_request");
        success = false;
    }

    write_log(std::string("config_reset_complete success=") + (success ? "1" : "0"));
    return success;
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
    if (const auto *value = find_fufu(L"ResetConfigurations"))
        config.reset_configurations = parse_bool(*value, config.reset_configurations);
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
            L"OptiScaler\\OptiScaler\\OptiScaler.dll",
            L"..\\OptiScaler\\OptiScaler.dll",
            L"..\\OptiScaler\\OptiScaler\\OptiScaler.dll",
            L"payload\\OptiScaler\\OptiScaler.dll",
            L"payload\\OptiScaler\\OptiScaler\\OptiScaler.dll",
            L"..\\payload\\OptiScaler\\OptiScaler.dll",
            L"..\\payload\\OptiScaler\\OptiScaler\\OptiScaler.dll",
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
    write_log("plugin_loaded version=2.0.0 path=" + wide_to_utf8(self_path.wstring()));

    if (!is_target_process())
    {
        write_log("unsupported_process path=" + wide_to_utf8(module_path(nullptr).wstring()));
        return 1;
    }

    const BootstrapConfig config = load_config();
    if (config.reset_configurations)
        reset_all_configurations(config);
    else
        ensure_missing_component_configurations(config);
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

    // ReShade must install its graphics hooks before the Bridge/OptiScaler
    // chain.  In particular, loading it afterwards can make NVIDIA + FSR4
    // fail to negotiate the expected D3D interception path.
    if (config.enable_reshade)
    {
        if (GetModuleHandleW(L"ReShade64.dll") != nullptr)
        {
            write_log("plugin_stopped reason=reshade_already_loaded");
            return 14;
        }
        if (!prepare_reshade_game_configuration(config))
            return 15;
        HMODULE reshade = load_module("reshade", config.reshade_path);
        if (reshade == nullptr)
            return 16;
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

    write_log(std::string("plugin_success bridge=") + (config.enable_bridge ? "1" : "0") +
        " optiscaler=" + (config.enable_optiscaler ? "1" : "0") +
        " reshade=" + (config.enable_reshade ? "1" : "0") +
        " order=reshade,bridge,optiscaler");
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
