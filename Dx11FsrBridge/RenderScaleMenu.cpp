#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "detours.h"
#include "RenderScaleMenu.h"

namespace
{
using build_cmd_buffers_fn = void(__fastcall *)(void *);
using update_inner_target_fn = void(__fastcall *)(void *);
using get_text_fn = void *(__fastcall *)(void *);

constexpr std::array<float, 9> k_render_scales {
    0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 0.999f
};

constexpr std::array<const wchar_t *, 9> k_native_labels {
    L"0.6", L"0.8", L"0.9", L"1.0", L"1.1", L"1.2", L"1.3", L"1.4", L"1.5"
};

constexpr std::array<const wchar_t *, 9> k_bridge_labels {
    L"0.2", L"0.3", L"0.4", L"0.5", L"0.6", L"0.7", L"0.8", L"0.9", L"0.999"
};

constexpr std::array<float, 9> k_native_scales {
    0.6f, 0.8f, 0.9f, 1.0f, 1.1f, 1.2f, 1.3f, 1.4f, 1.5f
};

// These signatures are unique among the executable sections of the current 7.0 game binary.
constexpr std::array<std::uint8_t, 24> k_build_cmd_buffers_signature {
    0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57,
    0x55, 0x53, 0x48, 0x81, 0xEC, 0xD8, 0x01, 0x00, 0x00, 0x48,
    0x89, 0xCE, 0x0F, 0x57
};

// PostProcessLayer::UpdateInnerTarget. This is the point that consumes both
// m_InnerResolutionScale (+0x88) and its DRS ratio to create the inner RT.
// It runs before BuildCmdBuffers, including the train-door camera path.
constexpr std::array<std::uint8_t, 48> k_update_inner_target_signature {
    0x56, 0x57, 0x55, 0x53, 0x48, 0x81, 0xEC, 0xC8, 0x00, 0x00, 0x00, 0x44,
    0x0F, 0x29, 0x84, 0x24, 0xB0, 0x00, 0x00, 0x00, 0x0F, 0x29, 0xBC, 0x24,
    0xA0, 0x00, 0x00, 0x00, 0x0F, 0x29, 0xB4, 0x24, 0x90, 0x00, 0x00, 0x00,
    0x48, 0x89, 0xCE, 0x80, 0xB9, 0x68, 0x01, 0x00, 0x00, 0x00, 0x74, 0x17
};

constexpr std::array<std::uint8_t, 32> k_get_text_signature {
    0x48, 0x8B, 0x81, 0xE0, 0x00, 0x00, 0x00, 0xC3,
    0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x56, 0x57, 0x48, 0x83, 0xEC, 0x28, 0x48, 0x89,
    0xCE, 0x48, 0x85, 0xD2, 0x0F, 0x84, 0xC2, 0x00
};

struct Il2CppStringSnapshot
{
    std::int32_t length = 0;
    wchar_t text[8] {};
};

HMODULE g_bridge_module = nullptr;
render_scale_menu_log_fn g_log_callback = nullptr;
build_cmd_buffers_fn g_original_build_cmd_buffers = nullptr;
update_inner_target_fn g_original_update_inner_target = nullptr;
get_text_fn g_original_get_text = nullptr;
std::array<void *, 9> g_label_replacements {};
std::mutex g_replacement_mutex;
std::unordered_map<std::uintptr_t, std::size_t> g_last_label_by_object;
std::mutex g_selection_mutex;
std::atomic_int32_t g_selected_index { -1 };
constexpr std::size_t k_render_scale_member_offset = 0x88;
std::atomic_int32_t g_last_written_index { -1 };
std::atomic_bool g_started { false };
std::atomic_uint64_t g_build_call_count { 0 };
std::atomic_uintptr_t g_last_build_instance { 0 };
std::atomic_uint32_t g_build_diagnostic_mismatches { 0 };
std::atomic_uint64_t g_last_menu_snapshot_tick { 0 };
std::array<std::atomic_bool, 9> g_label_option_layout_logged {};
std::array<std::uint8_t, 0x400> g_recent_object_snapshot {};
std::uintptr_t g_recent_object_snapshot_instance = 0;
std::uint64_t g_recent_object_snapshot_tick = 0;
std::mutex g_recent_object_snapshot_mutex;
std::mutex g_writable_cache_mutex;
std::unordered_map<std::uintptr_t, bool> g_writable_cache; // 实例字段可写性缓存（跨帧稳定；上限防实例重建残留）

bool is_executable_address(const void *address);
bool read_float_value(const void *address, float &value);

struct ScaleFieldInstruction
{
    std::uintptr_t instruction = 0;
    std::uintptr_t function = 0;
    std::uint32_t function_size = 0;
    bool writes_field = false;
};

struct ModuleFingerprint
{
    std::uint32_t timestamp = 0;
    std::uint32_t image_size = 0;
    std::uint32_t checksum = 0;
};

std::filesystem::path module_directory(HMODULE module)
{
    wchar_t path[MAX_PATH] {};
    const auto length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    return std::filesystem::path(std::wstring(path, path + length)).parent_path();
}

bool get_module_fingerprint(HMODULE module, ModuleFingerprint &fingerprint)
{
    if (module == nullptr)
        return false;
    const auto *base = reinterpret_cast<const std::uint8_t *>(module);
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return false;
    fingerprint.timestamp = nt->FileHeader.TimeDateStamp;
    fingerprint.image_size = nt->OptionalHeader.SizeOfImage;
    fingerprint.checksum = nt->OptionalHeader.CheckSum;
    return true;
}

bool read_feature_cache(HMODULE module, const ModuleFingerprint &fingerprint,
    const std::array<std::uint8_t, 24> &build_signature,
    const std::array<std::uint8_t, 32> &get_text_signature,
    void *&build_target, void *&get_text_target)
{
    std::ifstream input(module_directory(g_bridge_module) / L"Dx11FsrBridge.features.cache");
    if (!input)
        return false;

    std::string key;
    std::uint64_t timestamp = 0;
    std::uint64_t image_size = 0;
    std::uint64_t checksum = 0;
    std::uint64_t build_rva = 0;
    std::uint64_t get_text_rva = 0;
    while (input >> key)
    {
        if (key == "timestamp") input >> timestamp;
        else if (key == "image_size") input >> image_size;
        else if (key == "checksum") input >> checksum;
        else if (key == "build_rva") input >> build_rva;
        else if (key == "get_text_rva") input >> get_text_rva;
        else { std::string ignored; input >> ignored; }
    }
    if (timestamp != fingerprint.timestamp || image_size != fingerprint.image_size ||
        checksum != fingerprint.checksum || build_rva == 0 || get_text_rva == 0)
        return false;

    const auto base = reinterpret_cast<std::uintptr_t>(module);
    auto *cached_build = reinterpret_cast<std::uint8_t *>(base + build_rva);
    auto *cached_get_text = reinterpret_cast<std::uint8_t *>(base + get_text_rva);
    if (!is_executable_address(cached_build) || !is_executable_address(cached_get_text) ||
        std::memcmp(cached_build, build_signature.data(), build_signature.size()) != 0 ||
        std::memcmp(cached_get_text, get_text_signature.data(), get_text_signature.size()) != 0)
        return false;
    build_target = cached_build;
    get_text_target = cached_get_text;
    return true;
}

void write_feature_cache(HMODULE module, const ModuleFingerprint &fingerprint,
    const void *build_target, const void *get_text_target)
{
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    std::ofstream output(module_directory(g_bridge_module) / L"Dx11FsrBridge.features.cache", std::ios::trunc);
    if (!output)
        return;
    output << "version 1\n"
        << "timestamp " << fingerprint.timestamp << "\n"
        << "image_size " << fingerprint.image_size << "\n"
        << "checksum " << fingerprint.checksum << "\n"
        << "build_rva " << (reinterpret_cast<std::uintptr_t>(build_target) - base) << "\n"
        << "get_text_rva " << (reinterpret_cast<std::uintptr_t>(get_text_target) - base) << "\n";
}

void log_line(const std::string &message)
{
    if (g_log_callback != nullptr)
        g_log_callback("render_scale_menu " + message);
}

std::string hex_address(std::uintptr_t value)
{
    std::ostringstream output;
    output << std::hex << value;
    return output.str();
}

std::filesystem::path selection_cache_path()
{
    return module_directory(g_bridge_module) / L"Dx11FsrBridge.render-scale.cache";
}

void save_selection_cache(std::int32_t index)
{
    if (index < 0 || index >= static_cast<std::int32_t>(k_render_scales.size()))
        return;

    std::ofstream output(selection_cache_path(), std::ios::trunc);
    if (!output)
        return;
    output << "version 1\nindex " << index << "\n";
}

void load_selection_cache()
{
    std::ifstream input(selection_cache_path());
    if (!input)
        return;

    std::string key;
    std::int32_t index = -1;
    while (input >> key)
    {
        if (key == "index")
            input >> index;
        else
        {
            std::string ignored;
            input >> ignored;
        }
    }
    if (index < 0 || index >= static_cast<std::int32_t>(k_render_scales.size()))
        return;

    g_selected_index.store(index, std::memory_order_release);
    log_line("startup_selection_cache_hit index=" + std::to_string(index) +
        " scale=" + std::to_string(k_render_scales[static_cast<std::size_t>(index)]));
}

bool is_executable_address(const void *address)
{
    MEMORY_BASIC_INFORMATION memory {};
    return VirtualQuery(address, &memory, sizeof(memory)) == sizeof(memory) &&
        memory.State == MEM_COMMIT &&
        (memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

template <std::size_t Length>
void *find_unique_executable_signature(HMODULE module, const std::array<std::uint8_t, Length> &signature,
    std::uint32_t *match_count = nullptr)
{
    if (match_count != nullptr)
        *match_count = 0;
    if (module == nullptr)
        return nullptr;

    const auto *base = reinterpret_cast<const std::uint8_t *>(module);
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return nullptr;
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return nullptr;

    // Some game code is brought online after the bridge has been loaded. Scan
    // committed executable pages instead of assuming every PE section is fully
    // readable at that instant.
    const auto module_begin = reinterpret_cast<std::uintptr_t>(base);
    const auto module_end = module_begin + nt->OptionalHeader.SizeOfImage;
    void *result = nullptr;
    auto address = module_begin;
    while (address < module_end)
    {
        MEMORY_BASIC_INFORMATION memory {};
        if (VirtualQuery(reinterpret_cast<const void *>(address), &memory, sizeof(memory)) != sizeof(memory) ||
            memory.RegionSize == 0)
            break;

        const auto region_begin = std::max(address, reinterpret_cast<std::uintptr_t>(memory.BaseAddress));
        const auto region_end = std::min(module_end,
            reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
        address = region_end;

        const auto executable = (memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
            PAGE_EXECUTE_WRITECOPY)) != 0;
        if (memory.State != MEM_COMMIT || !executable || memory.Protect == PAGE_NOACCESS ||
            region_end <= region_begin || region_end - region_begin < Length)
            continue;

        const auto *begin = reinterpret_cast<const std::uint8_t *>(region_begin);
        const auto size = static_cast<std::size_t>(region_end - region_begin);
        for (std::size_t offset = 0; offset <= size - Length; ++offset)
        {
            if (std::memcmp(begin + offset, signature.data(), Length) != 0)
                continue;
            if (match_count != nullptr)
                ++*match_count;
            if (result != nullptr)
                return nullptr;
            result = const_cast<std::uint8_t *>(begin + offset);
        }
    }
    return result;
}

bool snapshot_il2cpp_string(void *object, Il2CppStringSnapshot &snapshot)
{
    if (object == nullptr)
        return false;
    __try
    {
        const auto address = reinterpret_cast<std::uintptr_t>(object);
        const auto length = *reinterpret_cast<const std::int32_t *>(address + 0x10);
        if (length <= 0 || length >= static_cast<std::int32_t>(std::size(snapshot.text)))
            return false;
        const auto *characters = reinterpret_cast<const wchar_t *>(address + 0x14);
        for (std::int32_t index = 0; index < length; ++index)
            snapshot.text[index] = characters[index];
        snapshot.text[length] = L'\0';
        snapshot.length = length;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

std::int32_t label_index(const Il2CppStringSnapshot &snapshot)
{
    for (std::size_t index = 0; index < k_native_labels.size(); ++index)
    {
        const auto length = static_cast<std::int32_t>(wcslen(k_native_labels[index]));
        if (snapshot.length == length &&
            std::memcmp(snapshot.text, k_native_labels[index], static_cast<std::size_t>(length) * sizeof(wchar_t)) == 0)
            return static_cast<std::int32_t>(index);
    }
    return -1;
}

void *make_il2cpp_string(void *template_string, const wchar_t *text)
{
    if (template_string == nullptr || text == nullptr)
        return nullptr;

    const auto length = wcslen(text);
    const auto bytes = (0x14 + (length + 1) * sizeof(wchar_t) + 15) & ~std::size_t(15);
    auto *memory = static_cast<std::uint8_t *>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (memory == nullptr)
        return nullptr;

    __try
    {
        *reinterpret_cast<void **>(memory) = *reinterpret_cast<void **>(template_string);
        *reinterpret_cast<void **>(memory + 8) = nullptr;
        *reinterpret_cast<std::int32_t *>(memory + 0x10) = static_cast<std::int32_t>(length);
        std::memcpy(memory + 0x14, text, (length + 1) * sizeof(wchar_t));
        return memory;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        VirtualFree(memory, 0, MEM_RELEASE);
        return nullptr;
    }
}

void record_selection(void *instance, std::int32_t index)
{
    const auto object = reinterpret_cast<std::uintptr_t>(instance);
    std::lock_guard lock(g_selection_mutex);
    const auto [entry, inserted] = g_last_label_by_object.emplace(object, static_cast<std::size_t>(index));
    if (inserted)
    {
        // get_text is also called while the menu enumerates every available
        // label.  A first observation only identifies an option object; it is
        // not evidence that the player selected that option.  Persisting it
        // here made the final enumerated label (often 0.7) become the startup
        // render scale on the next launch.
        return;
    }
    if (entry->second == static_cast<std::size_t>(index))
        return;

    entry->second = static_cast<std::size_t>(index);
    g_selected_index.store(index, std::memory_order_release);
    save_selection_cache(index);
    log_line("selection_changed index=" + std::to_string(index) +
        " scale=" + std::to_string(k_render_scales[static_cast<std::size_t>(index)]));
}

bool seed_selection_from_native_scale(void *instance)
{
    if (instance == nullptr)
        return false;

    const auto field = reinterpret_cast<std::uintptr_t>(instance) + k_render_scale_member_offset;
    float native_scale = 0.0f;
    if (!read_float_value(reinterpret_cast<const void *>(field), native_scale))
        return false;

    std::int32_t index = -1;
    for (std::size_t candidate = 0; candidate < k_native_scales.size(); ++candidate)
    {
        if (std::fabs(native_scale - k_native_scales[candidate]) <= 0.0001f)
        {
            index = static_cast<std::int32_t>(candidate);
            break;
        }
    }
    if (index < 0)
        return false;

    std::int32_t expected = -1;
    if (!g_selected_index.compare_exchange_strong(expected, index, std::memory_order_acq_rel))
        return true;

    save_selection_cache(index);
    log_line("startup_selection_seed native=" + std::to_string(native_scale) +
        " index=" + std::to_string(index) +
        " scale=" + std::to_string(k_render_scales[static_cast<std::size_t>(index)]));
    return true;
}

bool is_writable_address(const void *address, std::size_t length)
{
    const auto key = reinterpret_cast<std::uintptr_t>(address);
    {
        std::lock_guard lock(g_writable_cache_mutex);
        const auto it = g_writable_cache.find(key);
        if (it != g_writable_cache.end())
            return it->second;
    }
    MEMORY_BASIC_INFORMATION memory {};
    const bool result = VirtualQuery(address, &memory, sizeof(memory)) == sizeof(memory) &&
        memory.State == MEM_COMMIT &&
        memory.Protect != PAGE_NOACCESS && (memory.Protect & PAGE_GUARD) == 0 &&
        (memory.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0 &&
        reinterpret_cast<std::uintptr_t>(address) <=
            reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize &&
        length <= (reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize) -
            reinterpret_cast<std::uintptr_t>(address);
    std::lock_guard lock(g_writable_cache_mutex);
    if (g_writable_cache.size() >= 128)
        g_writable_cache.clear(); // 实例重建后重新评估
    g_writable_cache.emplace(key, result);
    return result;
}

bool read_float_value(const void *address, float &value)
{
    if (address == nullptr)
        return false;
    __try
    {
        value = *reinterpret_cast<const float *>(address);
        return std::isfinite(value);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool read_pointer_value(const void *address, std::uintptr_t &value)
{
    if (address == nullptr)
        return false;
    __try
    {
        value = *reinterpret_cast<const std::uintptr_t *>(address);
        return value != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool is_scale_candidate(float value)
{
    if (!std::isfinite(value))
        return false;
    for (const float scale : k_render_scales)
    {
        if (std::fabs(value - scale) <= 0.0001f)
            return true;
    }
    for (const float scale : k_native_scales)
    {
        if (std::fabs(value - scale) <= 0.0001f)
            return true;
    }
    return false;
}

std::string snapshot_scale_members(std::uintptr_t object)
{
    std::ostringstream output;
    bool first = true;
    std::size_t found = 0;
    for (std::size_t offset = 0x10; offset <= 0x200; offset += sizeof(float))
    {
        float value = 0.0f;
        if (!read_float_value(reinterpret_cast<const void *>(object + offset), value) || !is_scale_candidate(value))
            continue;
        const bool watched_offset = offset == 0x88 || offset == 0x8C ||
            offset == 0x168 || offset == 0x16C || offset == 0x170;
        if (!watched_offset && std::fabs(value - 1.0f) <= 0.0001f)
            continue;
        if (!first)
            output << ',';
        first = false;
        output << "+0x" << std::hex << offset << '=' << std::dec << value;
        if (++found == 24)
            break;
    }
    return first ? "none" : output.str();
}

void log_menu_scale_members()
{
    const auto now = GetTickCount64();
    const auto previous = g_last_menu_snapshot_tick.load(std::memory_order_relaxed);
    if (previous != 0 && now >= previous && now - previous < 1000)
        return;
    g_last_menu_snapshot_tick.store(now, std::memory_order_relaxed);

    const auto instance = g_last_build_instance.load(std::memory_order_acquire);
    if (instance == 0)
        return;
    log_line("menu_scale_members instance=0x" + hex_address(instance) +
        " values={" + snapshot_scale_members(instance) + "}");
}

bool copy_object_snapshot(std::uintptr_t object, std::array<std::uint8_t, 0x400> &snapshot)
{
    if (object == 0)
        return false;
    __try
    {
        std::memcpy(snapshot.data(), reinterpret_cast<const void *>(object), snapshot.size());
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void refresh_object_snapshot(std::uintptr_t object)
{
    const auto now = GetTickCount64();
    std::lock_guard lock(g_recent_object_snapshot_mutex);
    if (g_recent_object_snapshot_instance == object && now >= g_recent_object_snapshot_tick &&
        now - g_recent_object_snapshot_tick < 100)
        return;
    if (copy_object_snapshot(object, g_recent_object_snapshot))
    {
        g_recent_object_snapshot_instance = object;
        g_recent_object_snapshot_tick = now;
    }
}

void log_menu_object_delta()
{
    const auto object = g_last_build_instance.load(std::memory_order_acquire);
    if (object == 0)
        return;
    std::array<std::uint8_t, 0x400> current {};
    if (!copy_object_snapshot(object, current))
        return;

    std::lock_guard lock(g_recent_object_snapshot_mutex);
    if (g_recent_object_snapshot_instance != object)
        return;
    std::ostringstream changes;
    std::size_t count = 0;
    for (std::size_t offset = 0; offset < current.size(); offset += sizeof(std::uint32_t))
    {
        std::uint32_t before = 0;
        std::uint32_t after = 0;
        std::memcpy(&before, g_recent_object_snapshot.data() + offset, sizeof(before));
        std::memcpy(&after, current.data() + offset, sizeof(after));
        if (before == after)
            continue;
        if (count != 0)
            changes << ',';
        float before_float = 0.0f;
        float after_float = 0.0f;
        std::memcpy(&before_float, &before, sizeof(before_float));
        std::memcpy(&after_float, &after, sizeof(after_float));
        changes << "+0x" << std::hex << offset << std::dec
            << "=" << before_float << "->" << after_float;
        if (++count == 24)
            break;
    }
    log_line("menu_object_delta instance=0x" + hex_address(object) +
        " age_ms=" + std::to_string(GetTickCount64() - g_recent_object_snapshot_tick) +
        " changes={" + (count == 0 ? std::string("none") : changes.str()) + "}");
    g_recent_object_snapshot = current;
    g_recent_object_snapshot_tick = GetTickCount64();
}

void log_label_option_layout(void *instance, std::int32_t index)
{
    if (instance == nullptr || index < 0 || index >= static_cast<std::int32_t>(k_native_scales.size()) ||
        g_label_option_layout_logged[static_cast<std::size_t>(index)].exchange(true, std::memory_order_acq_rel))
        return;

    const auto object = reinterpret_cast<std::uintptr_t>(instance);
    std::ostringstream direct;
    std::ostringstream nested;
    std::size_t direct_count = 0;
    std::size_t nested_count = 0;
    for (std::size_t offset = 0x10; offset <= 0x200; offset += sizeof(float))
    {
        float value = 0.0f;
        if (!read_float_value(reinterpret_cast<const void *>(object + offset), value) || !is_scale_candidate(value))
            continue;
        if (direct_count != 0)
            direct << ',';
        direct << "+0x" << std::hex << offset << '=' << std::dec << value;
        if (++direct_count == 12)
            break;
    }
    for (std::size_t pointer_offset = 0x10; pointer_offset <= 0x200 && nested_count < 12;
         pointer_offset += sizeof(std::uintptr_t))
    {
        std::uintptr_t target = 0;
        if (!read_pointer_value(reinterpret_cast<const void *>(object + pointer_offset), target) ||
            target < 0x10000000000ull)
            continue;
        for (std::size_t value_offset = 0x10; value_offset <= 0x100; value_offset += sizeof(float))
        {
            float value = 0.0f;
            if (!read_float_value(reinterpret_cast<const void *>(target + value_offset), value) || !is_scale_candidate(value))
                continue;
            if (nested_count != 0)
                nested << ',';
            nested << "+0x" << std::hex << pointer_offset << "->0x" << value_offset
                << '=' << std::dec << value;
            if (++nested_count == 12)
                break;
        }
    }
    log_line("label_option_layout index=" + std::to_string(index) +
        " object=0x" + hex_address(object) +
        " direct={" + (direct_count == 0 ? std::string("none") : direct.str()) + "}" +
        " nested={" + (nested_count == 0 ? std::string("none") : nested.str()) + "}");
}

void * __fastcall hooked_get_text(void *instance)
{
    void *text = g_original_get_text != nullptr ? g_original_get_text(instance) : nullptr;
    Il2CppStringSnapshot snapshot {};
    if (!snapshot_il2cpp_string(text, snapshot))
        return text;

    const auto index = label_index(snapshot);
    if (index < 0)
        return text;

    record_selection(instance, index);
    std::lock_guard lock(g_replacement_mutex);
    auto &replacement = g_label_replacements[static_cast<std::size_t>(index)];
    if (replacement == nullptr)
        replacement = make_il2cpp_string(text, k_bridge_labels[static_cast<std::size_t>(index)]);
    return replacement != nullptr ? replacement : text;
}

bool apply_selected_render_scale(void *instance, const char *source)
{
    auto index = g_selected_index.load(std::memory_order_acquire);
    if (index < 0 && !seed_selection_from_native_scale(instance))
        return false;
    index = g_selected_index.load(std::memory_order_acquire);
    if (instance == nullptr || index < 0 || index >= static_cast<std::int32_t>(k_render_scales.size()))
        return false;

    const auto field = reinterpret_cast<std::uintptr_t>(instance) + k_render_scale_member_offset;
    if (!is_writable_address(reinterpret_cast<const void *>(field), sizeof(float)))
        return false;

    *reinterpret_cast<float *>(field) = k_render_scales[static_cast<std::size_t>(index)];
    const auto previous = g_last_written_index.exchange(index, std::memory_order_acq_rel);
    if (previous != index)
    {
        log_line(std::string("render_scale_written source=") + source +
            " index=" + std::to_string(index) +
            " scale=" + std::to_string(k_render_scales[static_cast<std::size_t>(index)]));
    }
    return true;
}

void __fastcall hooked_update_inner_target(void *instance)
{
    apply_selected_render_scale(instance, "update_inner_target");
    if (g_original_update_inner_target != nullptr)
        g_original_update_inner_target(instance);
}

void __fastcall hooked_build_cmd_buffers(void *instance)
{
    apply_selected_render_scale(instance, "build_cmd_buffers");
    if (g_original_build_cmd_buffers != nullptr)
        g_original_build_cmd_buffers(instance);
}

DWORD WINAPI initialize_menu(void *)
{
    load_selection_cache();
    const auto game_module = GetModuleHandleW(nullptr);
    void *build_target = nullptr;
    void *update_target = nullptr;
    void *get_text_target = nullptr;
    std::uint32_t build_matches = 0;
    std::uint32_t update_matches = 0;
    std::uint32_t get_text_matches = 0;
    ModuleFingerprint fingerprint {};
    const bool fingerprint_ready = get_module_fingerprint(game_module, fingerprint);
    if (fingerprint_ready && read_feature_cache(game_module, fingerprint,
        k_build_cmd_buffers_signature, k_get_text_signature, build_target, get_text_target))
    {
        log_line("feature_cache_hit");
    }
    else
    {
        for (int attempt = 0; attempt != 60; ++attempt)
        {
            build_target = find_unique_executable_signature(game_module, k_build_cmd_buffers_signature, &build_matches);
            get_text_target = find_unique_executable_signature(game_module, k_get_text_signature, &get_text_matches);
            if (build_target != nullptr && get_text_target != nullptr &&
                is_executable_address(build_target) && is_executable_address(get_text_target))
                break;

            if (attempt == 0 || (attempt + 1) % 10 == 0)
            {
                log_line("feature_query_retry attempt=" + std::to_string(attempt + 1) +
                    " build_matches=" + std::to_string(build_matches) +
                    " get_text_matches=" + std::to_string(get_text_matches));
            }
            Sleep(500);
        }

        if (build_target != nullptr && get_text_target != nullptr && fingerprint_ready)
            write_feature_cache(game_module, fingerprint, build_target, get_text_target);
    }

    if (build_target == nullptr || get_text_target == nullptr ||
        !is_executable_address(build_target) || !is_executable_address(get_text_target))
    {
        log_line("feature_query_failed build_matches=" + std::to_string(build_matches) +
            " get_text_matches=" + std::to_string(get_text_matches));
        return 0;
    }

    update_target = find_unique_executable_signature(game_module, k_update_inner_target_signature, &update_matches);
    if (update_target == nullptr || !is_executable_address(update_target))
    {
        log_line("update_inner_target_feature_query_failed matches=" + std::to_string(update_matches) +
            "; continuing with late build fallback");
        update_target = nullptr;
    }
    else
    {
        log_line("update_inner_target_hook_target rva=0x" + hex_address(
            reinterpret_cast<std::uintptr_t>(update_target) - reinterpret_cast<std::uintptr_t>(game_module)));
    }

    if (DetourTransactionBegin() != NO_ERROR || DetourUpdateThread(GetCurrentThread()) != NO_ERROR)
    {
        DetourTransactionAbort();
        log_line("hook_transaction_failed");
        return 0;
    }

    g_original_build_cmd_buffers = reinterpret_cast<build_cmd_buffers_fn>(build_target);
    g_original_update_inner_target = reinterpret_cast<update_inner_target_fn>(update_target);
    g_original_get_text = reinterpret_cast<get_text_fn>(get_text_target);
    if (DetourAttach(reinterpret_cast<PVOID *>(&g_original_build_cmd_buffers), &hooked_build_cmd_buffers) != NO_ERROR ||
        (g_original_update_inner_target != nullptr && DetourAttach(
            reinterpret_cast<PVOID *>(&g_original_update_inner_target), &hooked_update_inner_target) != NO_ERROR) ||
        DetourAttach(reinterpret_cast<PVOID *>(&g_original_get_text), &hooked_get_text) != NO_ERROR ||
        DetourTransactionCommit() != NO_ERROR)
    {
        DetourTransactionAbort();
        log_line("hook_install_failed");
        return 0;
    }

    log_line("hook_ready mode=feature_query update_inner_target=" + std::to_string(
        g_original_update_inner_target != nullptr ? 1 : 0) +
        " scales=0.2,0.3,0.4,0.5,0.6,0.7,0.8,0.9,0.999");
    return 0;
}
}

void initialize_render_scale_menu(HMODULE bridge_module, render_scale_menu_log_fn log_callback)
{
    if (g_started.exchange(true, std::memory_order_acq_rel))
        return;
    g_bridge_module = bridge_module;
    g_log_callback = log_callback;
    if (const HANDLE thread = CreateThread(nullptr, 0, initialize_menu, nullptr, 0, nullptr))
        CloseHandle(thread);
    else
        log_line("create_worker_thread_failed");
}
