#include <Windows.h>
#include <Psapi.h>

#include "PatternScanner.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace
{
std::filesystem::path g_log_path;
std::mutex g_log_mutex;
std::uint8_t *g_main_base = nullptr;
void *g_player_perspective_stub = nullptr;
std::uint8_t *g_hide_uid_find_string = nullptr;
std::uint8_t *g_hide_uid_find_object = nullptr;
std::uint8_t *g_hide_uid_object_active = nullptr;
constexpr std::array<const char *, 3> k_hide_uid_paths {
    "/BetaWatermarkCanvas(Clone)/Panel/TxtUID",
    "/Canvas/Pages/PlayerProfilePage/GrpProfile/Right/GrpPlayerCard/UID",
    "/Canvas/Pages/InLevelMapPage/GrpMap/GrpPlayer/UID",
};
std::atomic_bool g_hide_uid_enabled { true };
std::atomic_bool g_hide_uid_logged_success { false };
std::atomic_bool g_hide_uid_logged_waiting { false };
std::atomic_bool g_hide_uid_logged_failure { false };
std::atomic_bool g_hide_uid_logged_cache_reset { false };
std::atomic<ULONGLONG> g_hide_uid_next_tick { 0 };
constexpr ULONGLONG k_hide_uid_retry_interval_ms = 1200;
constexpr ULONGLONG k_hide_uid_steady_interval_ms = 8000;
std::array<void *, k_hide_uid_paths.size()> g_hide_uid_string_cache {};
std::array<void *, k_hide_uid_paths.size()> g_hide_uid_object_cache {};
int g_hide_uid_exception_streak = 0;

using find_string_fn = void *(__fastcall *)(const char *);
using find_object_fn = void *(__fastcall *)(void *);
using object_active_fn = void(__fastcall *)(void *, bool);

bool hide_uid_once();

void log_line(const std::string &line)
{
#if defined(ANTIPLAYER_RELEASE_RUNTIME)
    std::string lowered = line;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char value)
        { return static_cast<char>(std::tolower(value)); });
    static constexpr std::array<std::string_view, 10> error_terms {
        "failed",
        "failure",
        "disabled",
        "unresolved",
        "unavailable",
        "mismatch",
        "exception",
        "invalid",
        "allocation",
        "error",
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

void reset_release_log()
{
#if defined(ANTIPLAYER_RELEASE_RUNTIME)
    std::lock_guard lock(g_log_mutex);
    std::ofstream truncate(g_log_path, std::ios::trunc);
#endif
}

std::filesystem::path module_dir(HMODULE module)
{
    wchar_t buffer[MAX_PATH] {};
    DWORD length = GetModuleFileNameW(module, buffer, MAX_PATH);
    return std::filesystem::path(std::wstring(buffer, buffer + length)).parent_path();
}

int hide_uid_once_unsafe()
{
    const auto find_string = reinterpret_cast<find_string_fn>(g_hide_uid_find_string);
    const auto find_object = reinterpret_cast<find_object_fn>(g_hide_uid_find_object);
    const auto object_active = reinterpret_cast<object_active_fn>(g_hide_uid_object_active);
    int hidden_count = 0;

    __try
    {
        for (std::size_t i = 0; i < k_hide_uid_paths.size(); ++i)
        {
            void *string_object = g_hide_uid_string_cache[i];
            if (string_object == nullptr)
            {
                string_object = find_string(k_hide_uid_paths[i]);
                g_hide_uid_string_cache[i] = string_object;
            }
            if (string_object == nullptr)
                continue;

            void *object = g_hide_uid_object_cache[i];
            if (object == nullptr)
            {
                object = find_object(string_object);
                g_hide_uid_object_cache[i] = object;
            }
            if (object == nullptr)
                continue;

            object_active(object, false);
            ++hidden_count;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }

    return hidden_count;
}

bool hide_uid_once()
{
    if (!g_hide_uid_enabled.load())
        return false;

    if (g_hide_uid_find_string == nullptr || g_hide_uid_find_object == nullptr || g_hide_uid_object_active == nullptr)
    {
        if (!g_hide_uid_logged_failure.exchange(true))
            log_line("HideUID disabled: required signatures unresolved");
        g_hide_uid_enabled.store(false);
        return false;
    }

    const int hidden_count = hide_uid_once_unsafe();
    if (hidden_count < 0)
    {
        g_hide_uid_object_cache.fill(nullptr);
        ++g_hide_uid_exception_streak;
        if (!g_hide_uid_logged_cache_reset.exchange(true))
            log_line("HideUID cache reset after exception");

        if (g_hide_uid_exception_streak >= 3)
        {
            if (!g_hide_uid_logged_failure.exchange(true))
                log_line("HideUID disabled: repeated exceptions while using cached objects");
            g_hide_uid_enabled.store(false);
        }
        return false;
    }

    g_hide_uid_exception_streak = 0;
    g_hide_uid_logged_cache_reset.store(false);

    if (hidden_count > 0)
    {
        if (!g_hide_uid_logged_success.exchange(true))
            log_line("HideUID active: hidden " + std::to_string(hidden_count) + " ui targets");
        return true;
    }

    if (!g_hide_uid_logged_waiting.exchange(true))
        log_line("HideUID waiting: ui targets not ready yet");
    return false;
}

bool patch_bytes(std::uint8_t *address, const std::uint8_t *bytes, std::size_t size)
{
    DWORD old_protect = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old_protect))
        return false;

    std::memcpy(address, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), address, size);

    DWORD ignored = 0;
    VirtualProtect(address, size, old_protect, &ignored);
    return true;
}

template <std::size_t N>
bool patch_bytes(std::uint8_t *address, const std::array<std::uint8_t, N> &bytes)
{
    return patch_bytes(address, bytes.data(), bytes.size());
}

bool patch_rel32_jump(std::uint8_t *address, std::uintptr_t absolute_target)
{
    const auto next = reinterpret_cast<std::uintptr_t>(address + 5);
    const auto diff = static_cast<std::intptr_t>(absolute_target) - static_cast<std::intptr_t>(next);
    if (diff < INT32_MIN || diff > INT32_MAX)
        return false;

    std::array<std::uint8_t, 5> patch { 0xE9, 0, 0, 0, 0 };
    const auto displacement = static_cast<std::int32_t>(diff);
    std::memcpy(patch.data() + 1, &displacement, sizeof(displacement));
    return patch_bytes(address, patch);
}

void hide_uid_from_main_thread()
{
    if (g_main_base != nullptr)
    {
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG next_allowed = g_hide_uid_next_tick.load(std::memory_order_relaxed);
        if (now < next_allowed)
            return;

        const bool hidden = hide_uid_once();
        const ULONGLONG interval = hidden ? k_hide_uid_steady_interval_ms : k_hide_uid_retry_interval_ms;
        g_hide_uid_next_tick.store(now + interval, std::memory_order_relaxed);
    }
}

void *allocate_near_address(void *target, std::size_t size)
{
    SYSTEM_INFO info {};
    GetSystemInfo(&info);

    const std::uintptr_t granularity = static_cast<std::uintptr_t>(info.dwAllocationGranularity);
    const std::uintptr_t target_address = reinterpret_cast<std::uintptr_t>(target);
    const std::uintptr_t max_distance = 0x70000000ull;

    for (std::uintptr_t distance = granularity; distance < max_distance; distance += granularity)
    {
        for (int direction : { 1, -1 })
        {
            std::uintptr_t hint_address = 0;
            if (direction > 0)
            {
                hint_address = target_address + distance;
            }
            else
            {
                if (target_address <= distance)
                    continue;
                hint_address = target_address - distance;
            }

            hint_address -= hint_address % granularity;
            void *allocated = VirtualAlloc(reinterpret_cast<void *>(hint_address), size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (allocated != nullptr)
                return allocated;
        }
    }

    return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
}

void *build_player_perspective_stub(std::uint8_t *player_perspective)
{
    if (g_player_perspective_stub != nullptr)
        return g_player_perspective_stub;

    auto *stub = static_cast<std::uint8_t *>(allocate_near_address(player_perspective, 0x1000));
    if (stub == nullptr)
        return nullptr;

    std::array<std::uint8_t, 21> code {
        0x48, 0x83, 0xEC, 0x28,
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
        0xFF, 0xD0,
        0x48, 0x83, 0xC4, 0x28,
        0xC3
    };

    const auto callback = reinterpret_cast<std::uintptr_t>(&hide_uid_from_main_thread);
    std::memcpy(code.data() + 6, &callback, sizeof(callback));
    std::memcpy(stub, code.data(), code.size());
    FlushInstructionCache(GetCurrentProcess(), stub, code.size());

    g_player_perspective_stub = stub;
    return stub;
}

bool patch_player_perspective(std::uint8_t *player_perspective)
{
    void *stub = build_player_perspective_stub(player_perspective);
    if (stub == nullptr)
    {
        log_line("PlayerPerspective stub allocation failed");
        return false;
    }

    if (!patch_rel32_jump(player_perspective, reinterpret_cast<std::uintptr_t>(stub)))
    {
        log_line("PlayerPerspective hook patch failed");
        return false;
    }

    log_line("patched PlayerPerspective with main-thread hook");
    return true;
}

bool patch_player_dive_mosaic(std::uint8_t *call_site)
{
    if (call_site[0] != 0xE8)
    {
        log_line("PlayerDiveMosaic exact call signature mismatch");
        return false;
    }

    constexpr std::array<std::uint8_t, 5> patch { 0xB8, 0x00, 0x00, 0x00, 0x00 };
    if (!patch_bytes(call_site, patch))
    {
        log_line("PlayerDiveMosaic patch failed");
        return false;
    }

    log_line("patched PlayerDiveMosaic");
    return true;
}

std::uint8_t *scan_unique_signature(
    std::uint8_t *base,
    std::size_t image_size,
    const pattern_scanner::Signature &signature)
{
    if (image_size < sizeof(IMAGE_DOS_HEADER))
        return nullptr;

    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > image_size)
        return nullptr;

    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return nullptr;

    const auto pattern = pattern_scanner::parse_pattern(signature.text);
    const auto *sections = IMAGE_FIRST_SECTION(nt);
    std::array<std::uint8_t *, 2> matches {};
    std::size_t match_count = 0;

    for (unsigned section_index = 0; section_index < nt->FileHeader.NumberOfSections && match_count < matches.size(); ++section_index)
    {
        const auto &section = sections[section_index];
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 || section.VirtualAddress >= image_size)
            continue;

        std::size_t section_size = section.Misc.VirtualSize != 0 ? section.Misc.VirtualSize : section.SizeOfRawData;
        section_size = (std::min)(section_size, image_size - section.VirtualAddress);
        const auto section_matches = pattern_scanner::find_matches(
            base + section.VirtualAddress,
            section_size,
            pattern,
            matches.size() - match_count);
        for (const auto offset : section_matches)
            matches[match_count++] = base + section.VirtualAddress + offset;
    }

    if (match_count != 1)
    {
        log_line(std::string(signature.name) + " scan failed: matches=" + std::to_string(match_count));
        return nullptr;
    }

    char message[128] {};
    const auto rva = static_cast<std::uintptr_t>(matches[0] - base);
    std::snprintf(message, sizeof(message), "%.*s resolved: rva=0x%08llX",
        static_cast<int>(signature.name.size()), signature.name.data(), static_cast<unsigned long long>(rva));
    log_line(message);
    return matches[0];
}

DWORD WINAPI worker_thread(void *)
{
    reset_release_log();
    log_line("AntiPlayerMosaic loaded (dynamic scan)");
    Sleep(3000);

    HMODULE main_module = GetModuleHandleW(nullptr);
    MODULEINFO module_info {};
    if (!main_module || !K32GetModuleInformation(GetCurrentProcess(), main_module, &module_info, sizeof(module_info)))
    {
        log_line("main module info failed");
        return 0;
    }

    auto *base = static_cast<std::uint8_t *>(module_info.lpBaseOfDll);
    const std::size_t size = static_cast<std::size_t>(module_info.SizeOfImage);
    g_main_base = base;

    wchar_t executable_path[MAX_PATH] {};
    GetModuleFileNameW(main_module, executable_path, MAX_PATH);
    log_line("game executable: " + std::filesystem::path(executable_path).filename().string());

    g_hide_uid_find_string = scan_unique_signature(base, size, pattern_scanner::k_signatures[0]);
    g_hide_uid_find_object = scan_unique_signature(base, size, pattern_scanner::k_signatures[1]);
    g_hide_uid_object_active = scan_unique_signature(base, size, pattern_scanner::k_signatures[2]);
    auto *player_perspective = scan_unique_signature(base, size, pattern_scanner::k_signatures[3]);
    auto *player_dive_mosaic = scan_unique_signature(base, size, pattern_scanner::k_signatures[4]);

    if (g_hide_uid_find_string == nullptr || g_hide_uid_find_object == nullptr || g_hide_uid_object_active == nullptr)
        g_hide_uid_enabled.store(false);

    const bool perspective_patched = player_perspective != nullptr && patch_player_perspective(player_perspective);
    if (!perspective_patched)
    {
        g_hide_uid_enabled.store(false);
        log_line("PlayerPerspective disabled: unique signature unavailable or patch failed");
    }
    else if (g_hide_uid_enabled.load())
    {
        log_line("HideUID main-thread hook active");
    }

    if (player_dive_mosaic == nullptr)
        log_line("PlayerDiveMosaic disabled: unique signature unavailable");
    else
        patch_player_dive_mosaic(player_dive_mosaic);
    return 0;
}
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        g_log_path = module_dir(hModule) / "AntiPlayerMosaic.log";
        HANDLE thread = CreateThread(nullptr, 0, worker_thread, nullptr, 0, nullptr);
        if (thread)
            CloseHandle(thread);
    }
    return TRUE;
}
