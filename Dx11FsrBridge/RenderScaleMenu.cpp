#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>
#include <TlHelp32.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_set>

#include "detours.h"
#include "RenderScaleMenu.h"

namespace
{
constexpr LONG MH_OK = NO_ERROR;
constexpr void *MH_ALL_HOOKS = nullptr;
bool g_detour_transaction_active = false;

LONG MH_Initialize()
{
    LONG status = DetourTransactionBegin();
    if (status == NO_ERROR)
        status = DetourUpdateThread(GetCurrentThread());
    if (status != NO_ERROR)
    {
        DetourTransactionAbort();
        return status;
    }
    g_detour_transaction_active = true;
    return NO_ERROR;
}

LONG MH_CreateHook(void *target, void *detour, void **original)
{
    if (!g_detour_transaction_active || target == nullptr || detour == nullptr || original == nullptr)
        return ERROR_INVALID_PARAMETER;
    *original = target;
    const LONG status = DetourAttach(reinterpret_cast<PVOID *>(original), detour);
    if (status != NO_ERROR)
    {
        DetourTransactionAbort();
        g_detour_transaction_active = false;
    }
    return status;
}

LONG MH_EnableHook(void *)
{
    if (!g_detour_transaction_active)
        return ERROR_INVALID_FUNCTION;
    const LONG status = DetourTransactionCommit();
    g_detour_transaction_active = false;
    return status;
}

using build_cmd_buffers_fn = void(__fastcall *)(void *);
using render_scale_setter_fn = void(__fastcall *)(void *, float);
using render_scale_apply_fn = void(__fastcall *)(float, void *);
using graphics_float_setter_fn = std::uint8_t(__fastcall *)(void *, float);
using graphics_option_apply_fn = void(__fastcall *)(void *, void *);
using graphics_option_lookup_fn = void *(__fastcall *)(void *, void *, void *);
using graphics_index_resolver_fn = std::int32_t(__fastcall *)(void *, std::int32_t, std::int32_t, void *, std::int32_t *);
using encoded_float_decoder_fn = float(__fastcall *)(const std::int32_t *);
using encoded_float_encoder_fn = void(__fastcall *)(std::int32_t *, float);

HMODULE g_module = nullptr;
build_cmd_buffers_fn g_original_build_cmd_buffers = nullptr;
render_scale_setter_fn g_original_render_scale_setter = nullptr;
render_scale_apply_fn g_original_render_scale_apply = nullptr;
graphics_float_setter_fn g_original_graphics_float_setter = nullptr;
graphics_option_apply_fn g_original_graphics_option_apply = nullptr;
graphics_option_lookup_fn g_original_graphics_option_lookup = nullptr;
graphics_index_resolver_fn g_original_graphics_index_resolver = nullptr;
encoded_float_decoder_fn g_original_encoded_float_decoder = nullptr;
render_scale_menu_log_fn g_log_callback = nullptr;
std::atomic_bool g_started { false };
std::atomic_uint64_t g_call_count { 0 };
std::atomic_uint64_t g_setter_call_count { 0 };
std::atomic_uint64_t g_apply_call_count { 0 };
std::atomic_uint64_t g_graphics_setter_call_count { 0 };
std::atomic_uintptr_t g_last_render_scale_descriptor { 0 };
std::atomic_uintptr_t g_last_render_scale_lookup_result { 0 };
std::atomic_uintptr_t g_patched_candidate_array { 0 };
std::atomic_uintptr_t g_last_label_item_array { 0 };
std::atomic_int g_pending_label_scan_attempts { 0 };
std::atomic_uint64_t g_next_label_scan_tick { 0 };
std::atomic_uint64_t g_last_render_scale_index_tick { 0 };
std::atomic_int32_t g_current_render_scale_internal_index { -1 };
std::atomic_bool g_native_aa_selected { false };
std::atomic_uint64_t g_native_aa_build_spoof_count { 0 };
std::atomic_uint64_t g_native_aa_dimension_override_count { 0 };
std::array<std::atomic_uint32_t, 12> g_render_scale_candidate_encodings {};
std::atomic_int32_t g_render_scale_candidate_count { 0 };
std::mutex g_candidate_decode_log_mutex;
std::unordered_set<std::uint64_t> g_candidate_decode_log_keys;
std::atomic_bool g_read_fault_logged { false };
std::atomic_uintptr_t g_watch_address { 0 };
std::atomic_uint32_t g_watch_thread_id { 0 };
std::atomic_uintptr_t g_last_write_next_rva { 0 };
std::atomic_uint32_t g_last_write_value_bits { 0 };
std::mutex g_field_access_log_mutex;
std::unordered_set<std::uint64_t> g_field_access_log_keys;
PVOID g_vectored_exception_handler = nullptr;

constexpr DWORD kArmFieldWatchException = 0xE0425253;

struct Config
{
    bool enabled = true;
    std::uintptr_t build_cmd_buffers_rva = 0x6812110;
    std::uintptr_t render_scale_setter_rva = 0x680C650;
    std::uintptr_t render_scale_apply_rva = 0xADA0900;
    std::uintptr_t render_scale_offset = 0x88;
    std::uintptr_t source_scale_rva = 0x505E5FC;
    std::uintptr_t graphics_float_setter_rva = 0xC212450;
    std::uintptr_t render_scale_key_pointer_rva = 0x52B1180;
    std::uintptr_t graphics_option_apply_rva = 0xC212160;
    std::uintptr_t graphics_option_lookup_rva = 0x128B8590;
    std::uintptr_t graphics_index_resolver_rva = 0xC209120;
    std::uintptr_t encoded_float_decoder_rva = 0xA3B55D0;
    std::uintptr_t encoded_float_encoder_rva = 0xA3B56E0;
    bool field_write_watch_enabled = false;
    bool field_access_watch_enabled = false;
    bool source_scale_watch_enabled = false;
    bool candidate_decode_trace_enabled = false;
    std::int32_t candidate_decode_trace_max_index = 1;
    bool object_string_graph_trace_enabled = false;
    bool label_array_scanner_enabled = true;
    bool candidate_list_patch_enabled = true;
    bool apply_candidate_source_scale = true;
    bool force_native_aa_fsr = false;
    float native_aa_gate_scale = 0.999f;
    bool apply_unclamped_scale = false;
    bool override_enabled = false;
    float override_scale = 1.0f;
    std::uint64_t log_unchanged_every = 0;
};

Config g_config;
bool g_has_previous_scale = false;
float g_previous_scale = 0.0f;

std::filesystem::path module_directory()
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(g_module, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

void log_line(const std::string &message)
{
    if (g_log_callback == nullptr)
        return;
    static constexpr std::array<std::string_view, 11> important_terms {
        "failed", "failure", "mismatch", "invalid", "exception", "unavailable",
        "target_not_executable", "targets_validated", "hook_ready",
        "candidate_list_patch", "all_label_references_replaced"
    };
    const bool important = std::any_of(important_terms.begin(), important_terms.end(),
        [&](std::string_view term) { return message.find(term) != std::string::npos; });
    if (important)
        g_log_callback("render_scale_menu " + message);
}

std::uintptr_t read_hex_value(const std::filesystem::path &ini_path, const wchar_t *key, std::uintptr_t fallback)
{
    wchar_t fallback_text[32] {};
    swprintf_s(fallback_text, L"0x%llX", static_cast<unsigned long long>(fallback));
    wchar_t value[64] {};
    GetPrivateProfileStringW(L"RenderScaleMenu", key, fallback_text, value,
        static_cast<DWORD>(std::size(value)), ini_path.c_str());
    wchar_t *end = nullptr;
    const auto parsed = static_cast<std::uintptr_t>(wcstoull(value, &end, 0));
    return end != value ? parsed : fallback;
}

void load_config()
{
    const auto ini_path = module_directory() / L"Dx11FsrBridge.ini";
    g_config.enabled = GetPrivateProfileIntW(L"RenderScaleMenu", L"Enabled", 1, ini_path.c_str()) != 0;
    g_config.build_cmd_buffers_rva = read_hex_value(ini_path, L"BuildCmdBuffersRva", g_config.build_cmd_buffers_rva);
    g_config.render_scale_setter_rva = read_hex_value(ini_path, L"RenderScaleSetterRva", g_config.render_scale_setter_rva);
    g_config.render_scale_apply_rva = read_hex_value(ini_path, L"RenderScaleApplyRva", g_config.render_scale_apply_rva);
    g_config.render_scale_offset = read_hex_value(ini_path, L"RenderScaleOffset", g_config.render_scale_offset);
    g_config.source_scale_rva = read_hex_value(ini_path, L"SourceScaleRva", g_config.source_scale_rva);
    g_config.graphics_float_setter_rva =
        read_hex_value(ini_path, L"GraphicsFloatSetterRva", g_config.graphics_float_setter_rva);
    g_config.render_scale_key_pointer_rva =
        read_hex_value(ini_path, L"RenderScaleKeyPointerRva", g_config.render_scale_key_pointer_rva);
    g_config.graphics_option_apply_rva =
        read_hex_value(ini_path, L"GraphicsOptionApplyRva", g_config.graphics_option_apply_rva);
    g_config.graphics_option_lookup_rva =
        read_hex_value(ini_path, L"GraphicsOptionLookupRva", g_config.graphics_option_lookup_rva);
    g_config.graphics_index_resolver_rva =
        read_hex_value(ini_path, L"GraphicsIndexResolverRva", g_config.graphics_index_resolver_rva);
    g_config.encoded_float_decoder_rva =
        read_hex_value(ini_path, L"EncodedFloatDecoderRva", g_config.encoded_float_decoder_rva);
    g_config.encoded_float_encoder_rva =
        read_hex_value(ini_path, L"EncodedFloatEncoderRva", g_config.encoded_float_encoder_rva);
    g_config.field_write_watch_enabled =
        GetPrivateProfileIntW(L"RenderScaleMenu", L"FieldWriteWatchEnabled", 0, ini_path.c_str()) != 0;
    g_config.field_access_watch_enabled =
        GetPrivateProfileIntW(L"RenderScaleMenu", L"FieldAccessWatchEnabled", 0, ini_path.c_str()) != 0;
    g_config.source_scale_watch_enabled =
        GetPrivateProfileIntW(L"RenderScaleMenu", L"SourceScaleWriteWatchEnabled", 0, ini_path.c_str()) != 0;
    g_config.candidate_decode_trace_enabled =
        GetPrivateProfileIntW(L"RenderScaleMenu", L"CandidateDecodeTraceEnabled", 0, ini_path.c_str()) != 0;
    g_config.candidate_decode_trace_max_index =
        GetPrivateProfileIntW(L"RenderScaleMenu", L"CandidateDecodeTraceMaxIndex", 1, ini_path.c_str());
    g_config.object_string_graph_trace_enabled =
        GetPrivateProfileIntW(L"RenderScaleMenu", L"ObjectStringGraphTraceEnabled", 0, ini_path.c_str()) != 0;
    g_config.label_array_scanner_enabled =
        GetPrivateProfileIntW(L"RenderScaleMenu", L"LabelArrayScannerEnabled", 1, ini_path.c_str()) != 0;
    g_config.candidate_list_patch_enabled =
        GetPrivateProfileIntW(L"RenderScaleMenu", L"CandidateListPatchEnabled", 1, ini_path.c_str()) != 0;
    g_config.apply_candidate_source_scale =
        GetPrivateProfileIntW(L"RenderScaleMenu", L"ApplyCandidateSourceScale", 1, ini_path.c_str()) != 0;
    g_config.force_native_aa_fsr =
        GetPrivateProfileIntW(L"RenderScaleMenu", L"ForceNativeAaFsr", 0, ini_path.c_str()) != 0;
    g_config.apply_unclamped_scale =
        GetPrivateProfileIntW(L"RenderScaleMenu", L"ApplyUnclampedScale", 0, ini_path.c_str()) != 0;
    g_config.override_enabled = GetPrivateProfileIntW(L"RenderScaleMenu", L"OverrideEnabled", 0, ini_path.c_str()) != 0;
    g_config.log_unchanged_every = static_cast<std::uint64_t>(
        GetPrivateProfileIntW(L"RenderScaleMenu", L"LogUnchangedEvery", 0, ini_path.c_str()));
    wchar_t scale_text[64] {};
    GetPrivateProfileStringW(L"RenderScaleMenu", L"NativeAaGateScale", L"0.999", scale_text,
        static_cast<DWORD>(std::size(scale_text)), ini_path.c_str());
    g_config.native_aa_gate_scale = std::clamp(
        static_cast<float>(wcstod(scale_text, nullptr)), 0.95f, 0.9999f);
    GetPrivateProfileStringW(L"RenderScaleMenu", L"OverrideScale", L"1.0", scale_text,
        static_cast<DWORD>(std::size(scale_text)), ini_path.c_str());
    g_config.override_scale = std::clamp(static_cast<float>(wcstod(scale_text, nullptr)), 0.01f, 3.0f);
}

bool read_scale(void *instance, float &value)
{
    __try
    {
        value = *reinterpret_cast<float *>(reinterpret_cast<std::uintptr_t>(instance) + g_config.render_scale_offset);
        return std::isfinite(value);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool read_float_address(std::uintptr_t address, float &value)
{
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

bool write_scale(void *instance, float value)
{
    __try
    {
        *reinterpret_cast<float *>(reinterpret_cast<std::uintptr_t>(instance) + g_config.render_scale_offset) = value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool read_value_bits(std::uintptr_t address, std::uint32_t &value_bits)
{
    __try
    {
        value_bits = *reinterpret_cast<const std::uint32_t *>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool read_pointer(std::uintptr_t address, void *&value)
{
    __try
    {
        value = *reinterpret_cast<void **>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        value = nullptr;
        return false;
    }
}

bool il2cpp_string_equals(void *left, void *right)
{
    if (left == right)
        return left != nullptr;
    if (left == nullptr || right == nullptr)
        return false;

    __try
    {
        const auto left_address = reinterpret_cast<std::uintptr_t>(left);
        const auto right_address = reinterpret_cast<std::uintptr_t>(right);
        const auto left_length = *reinterpret_cast<const std::int32_t *>(left_address + 0x10);
        const auto right_length = *reinterpret_cast<const std::int32_t *>(right_address + 0x10);
        if (left_length < 0 || left_length > 1024 || left_length != right_length)
            return false;
        return std::memcmp(reinterpret_cast<const void *>(left_address + 0x14),
                   reinterpret_cast<const void *>(right_address + 0x14),
                   static_cast<std::size_t>(left_length) * sizeof(char16_t)) == 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

struct Il2CppStringSnapshot
{
    std::int32_t length = 0;
    wchar_t text[65] {};
};

bool snapshot_il2cpp_string(void *object, Il2CppStringSnapshot &snapshot)
{
    if (object == nullptr)
        return false;
    __try
    {
        const auto address = reinterpret_cast<std::uintptr_t>(object);
        const auto length = *reinterpret_cast<const std::int32_t *>(address + 0x10);
        if (length <= 0 || length > 64)
            return false;
        const auto *characters = reinterpret_cast<const wchar_t *>(address + 0x14);
        for (std::int32_t i = 0; i < length; ++i)
        {
            const wchar_t character = characters[i];
            if (character < 0x20 && character != L'\t')
                return false;
            snapshot.text[i] = character;
        }
        snapshot.text[length] = L'\0';
        snapshot.length = length;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

std::string utf8_from_snapshot(const Il2CppStringSnapshot &snapshot)
{
    const int required = WideCharToMultiByte(CP_UTF8, 0, snapshot.text, snapshot.length,
        nullptr, 0, nullptr, nullptr);
    if (required <= 0)
        return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, snapshot.text, snapshot.length,
        result.data(), required, nullptr, nullptr);
    return result;
}

void log_object_string_graph(const char *name, void *object)
{
    if (!g_config.object_string_graph_trace_enabled || object == nullptr)
        return;

    std::ostringstream message;
    message << "object_strings name=" << name << " object=0x" << std::hex
        << reinterpret_cast<std::uintptr_t>(object);
    Il2CppStringSnapshot direct {};
    if (snapshot_il2cpp_string(object, direct))
        message << " direct=\"" << utf8_from_snapshot(direct) << '\"';

    for (std::uintptr_t offset = 0x10; offset <= 0x70; offset += sizeof(void *))
    {
        void *field = nullptr;
        if (!read_pointer(reinterpret_cast<std::uintptr_t>(object) + offset, field) || field == nullptr)
            continue;
        Il2CppStringSnapshot field_string {};
        if (snapshot_il2cpp_string(field, field_string))
            message << " +0x" << std::hex << offset << "=\"" << utf8_from_snapshot(field_string) << '\"';
    }
    log_line(message.str());
}

bool il2cpp_string_equals_literal(void *object, const wchar_t *expected)
{
    Il2CppStringSnapshot snapshot {};
    if (!snapshot_il2cpp_string(object, snapshot))
        return false;
    const auto expected_length = static_cast<std::int32_t>(wcslen(expected));
    return snapshot.length == expected_length &&
        std::memcmp(snapshot.text, expected, static_cast<std::size_t>(expected_length) * sizeof(wchar_t)) == 0;
}

bool is_readable_region(const MEMORY_BASIC_INFORMATION &memory)
{
    if (memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return false;
    const DWORD protection = memory.Protect & 0xFF;
    return protection == PAGE_READONLY || protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
        protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

bool is_writable_region(const MEMORY_BASIC_INFORMATION &memory)
{
    if (!is_readable_region(memory))
        return false;
    const DWORD protection = memory.Protect & 0xFF;
    return protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
        protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

void scan_region_for_06_strings(std::uintptr_t base, std::size_t size,
    std::uintptr_t *matches, std::size_t capacity, std::size_t &count)
{
    __try
    {
        for (std::size_t offset = 0; offset + 6 <= size && count < capacity; offset += sizeof(wchar_t))
        {
            const auto *text = reinterpret_cast<const wchar_t *>(base + offset);
            if (text[0] != L'0' || text[1] != L'.' || text[2] != L'6')
                continue;
            const auto object = base + offset - 0x14;
            if (object >= base && il2cpp_string_equals_literal(reinterpret_cast<void *>(object), L"0.6"))
                matches[count++] = object;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

bool matches_original_label_sequence(std::uintptr_t data_address)
{
    constexpr const wchar_t *labels[] {
        L"0.6", L"0.8", L"0.9", L"1.0", L"1.1", L"1.2", L"1.3", L"1.4", L"1.5"
    };
    __try
    {
        for (std::size_t i = 0; i < std::size(labels); ++i)
        {
            void *string_object = *reinterpret_cast<void **>(data_address + i * sizeof(void *));
            if (!il2cpp_string_equals_literal(string_object, labels[i]))
                return false;
        }
        const auto array_address = data_address - 0x20;
        return *reinterpret_cast<const std::uintptr_t *>(array_address + 0x18) == std::size(labels);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void scan_region_for_label_arrays(std::uintptr_t base, std::size_t size,
    const std::uintptr_t *first_strings, std::size_t first_string_count,
    std::uintptr_t *arrays, std::size_t capacity, std::size_t &array_count)
{
    __try
    {
        const auto aligned_base = (base + sizeof(void *) - 1) & ~(sizeof(void *) - 1);
        const auto end = base + size;
        for (auto address = aligned_base; address + 9 * sizeof(void *) <= end && array_count < capacity;
             address += sizeof(void *))
        {
            const auto first = *reinterpret_cast<const std::uintptr_t *>(address);
            bool possible = false;
            for (std::size_t i = 0; i < first_string_count; ++i)
            {
                if (first_strings[i] == first)
                {
                    possible = true;
                    break;
                }
            }
            if (possible && address >= base + 0x20 && matches_original_label_sequence(address))
                arrays[array_count++] = address - 0x20;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

struct LabelObjectCandidate
{
    std::uintptr_t object = 0;
    std::uintptr_t string_offset = 0;
};

void *create_probe_string(void *template_string, const wchar_t *text);

bool plausible_managed_object(std::uintptr_t object)
{
    if (object < 0x70000000000ull || object >= 0x80000000000ull)
        return false;
    void *object_class = nullptr;
    if (!read_pointer(object, object_class))
        return false;
    const auto class_address = reinterpret_cast<std::uintptr_t>(object_class);
    return class_address >= 0x10000000000ull && class_address < 0x70000000000ull;
}

void scan_region_for_label_object_candidates(std::uintptr_t base, std::size_t size,
    const std::uintptr_t *first_strings, std::size_t first_string_count,
    LabelObjectCandidate *candidates, std::size_t capacity, std::size_t &candidate_count)
{
    __try
    {
        const auto aligned_base = (base + sizeof(void *) - 1) & ~(sizeof(void *) - 1);
        const auto end = base + size;
        for (auto address = aligned_base; address + sizeof(void *) <= end && candidate_count < capacity;
             address += sizeof(void *))
        {
            const auto value = *reinterpret_cast<const std::uintptr_t *>(address);
            bool is_first_string = false;
            for (std::size_t i = 0; i < first_string_count; ++i)
            {
                if (first_strings[i] == value)
                {
                    is_first_string = true;
                    break;
                }
            }
            if (!is_first_string)
                continue;

            for (std::uintptr_t offset = 0x10; offset <= 0x80 && candidate_count < capacity;
                 offset += sizeof(void *))
            {
                if (address < base + offset)
                    continue;
                const auto object = address - offset;
                if (!plausible_managed_object(object))
                    continue;
                bool duplicate = false;
                for (std::size_t i = 0; i < candidate_count; ++i)
                {
                    if (candidates[i].object == object && candidates[i].string_offset == offset)
                    {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate)
                    candidates[candidate_count++] = { object, offset };
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

bool matches_label_item_sequence(std::uintptr_t data_address, std::uintptr_t string_offset)
{
    constexpr const wchar_t *labels[] {
        L"0.6", L"0.8", L"0.9", L"1.0", L"1.1", L"1.2", L"1.3", L"1.4", L"1.5"
    };
    __try
    {
        for (std::size_t i = 0; i < std::size(labels); ++i)
        {
            const auto item = *reinterpret_cast<const std::uintptr_t *>(data_address + i * sizeof(void *));
            if (!plausible_managed_object(item))
                return false;
            void *string_object = *reinterpret_cast<void **>(item + string_offset);
            if (!il2cpp_string_equals_literal(string_object, labels[i]))
                return false;
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

struct LabelItemArrayMatch
{
    std::uintptr_t array = 0;
    std::uintptr_t string_offset = 0;
};

void scan_region_for_label_item_arrays(std::uintptr_t base, std::size_t size,
    const LabelObjectCandidate *candidates, std::size_t candidate_count,
    LabelItemArrayMatch *matches, std::size_t capacity, std::size_t &match_count)
{
    __try
    {
        const auto aligned_base = (base + sizeof(void *) - 1) & ~(sizeof(void *) - 1);
        const auto end = base + size;
        for (auto address = aligned_base; address + 9 * sizeof(void *) <= end && match_count < capacity;
             address += sizeof(void *))
        {
            const auto first_item = *reinterpret_cast<const std::uintptr_t *>(address);
            for (std::size_t i = 0; i < candidate_count; ++i)
            {
                if (candidates[i].object != first_item || address < base + 0x20)
                    continue;
                if (!matches_label_item_sequence(address, candidates[i].string_offset))
                    continue;
                const auto array = address - 0x20;
                const auto capacity_value = *reinterpret_cast<const std::uintptr_t *>(array + 0x18);
                if (capacity_value < 9 || capacity_value > 256)
                    continue;
                matches[match_count++] = { array, candidates[i].string_offset };
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

std::size_t shrink_lists_for_array(std::uintptr_t target_array,
    std::uintptr_t scan_begin = 0x70000000000ull,
    std::uintptr_t scan_end = 0x80000000000ull,
    bool apply = true)
{
    std::size_t shrunk = 0;
    for (std::uintptr_t address = scan_begin; address < scan_end;)
    {
        MEMORY_BASIC_INFORMATION memory {};
        if (VirtualQuery(reinterpret_cast<void *>(address), &memory, sizeof(memory)) != sizeof(memory))
            break;
        const auto region_base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        const auto next = region_base + memory.RegionSize;
        if (is_writable_region(memory))
        {
            const auto clipped_begin = std::max(region_base, scan_begin);
            const auto clipped_end = std::min(next, scan_end);
            __try
            {
                const auto aligned_base = (clipped_begin + sizeof(void *) - 1) & ~(sizeof(void *) - 1);
                const auto end = clipped_end;
                for (auto slot = aligned_base; slot + sizeof(void *) <= end; slot += sizeof(void *))
                {
                    if (*reinterpret_cast<const std::uintptr_t *>(slot) != target_array || slot < clipped_begin + 0x10)
                        continue;
                    const auto list = slot - 0x10;
                    if (!plausible_managed_object(list))
                        continue;
                    const auto count = *reinterpret_cast<const std::int32_t *>(list + 0x18);
                    if (count != 8 && count != 9)
                        continue;
                    if (apply && count == 9)
                        *reinterpret_cast<std::int32_t *>(list + 0x18) = 8;
                    ++shrunk;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        if (next <= address)
            break;
        address = next;
    }
    return shrunk;
}

std::size_t replace_pointer_references(std::uintptr_t old_pointer, std::uintptr_t new_pointer,
    std::uintptr_t scan_begin, std::uintptr_t scan_end)
{
    if (old_pointer == 0 || new_pointer == 0 || old_pointer == new_pointer)
        return 0;
    std::size_t replaced = 0;
    for (std::uintptr_t address = scan_begin; address < scan_end;)
    {
        MEMORY_BASIC_INFORMATION memory {};
        if (VirtualQuery(reinterpret_cast<void *>(address), &memory, sizeof(memory)) != sizeof(memory))
            break;
        const auto region_base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        const auto next = region_base + memory.RegionSize;
        if (is_writable_region(memory))
        {
            const auto clipped_begin = std::max(region_base, scan_begin);
            const auto clipped_end = std::min(next, scan_end);
            __try
            {
                const auto aligned_begin = (clipped_begin + sizeof(void *) - 1) & ~(sizeof(void *) - 1);
                for (auto slot = aligned_begin; slot + sizeof(void *) <= clipped_end; slot += sizeof(void *))
                {
                    if (*reinterpret_cast<const std::uintptr_t *>(slot) != old_pointer)
                        continue;
                    *reinterpret_cast<std::uintptr_t *>(slot) = new_pointer;
                    ++replaced;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        if (next <= address)
            break;
        address = next;
    }
    return replaced;
}

void log_label_reference_replacement(std::size_t total_count)
{
    std::ostringstream message;
    message << "all_label_references_replaced"
        << " labels=9"
        << " total_count=" << total_count;
    log_line(message.str());
}

bool patch_label_item_array(const LabelItemArrayMatch &match, std::size_t &lists_shrunk)
{
    constexpr const wchar_t *labels[] {
        // String-object order: 0.6,0.8,0.9,1.0,1.1,1.2,1.3,1.4,1.5.
        // The UI presents original 0.9 after 1.4, so use object order here.
        L"0.2", L"0.3", L"原生", L"0.4", L"0.5", L"0.6", L"0.7", L"0.8", L"0.9"
    };
    constexpr std::uintptr_t scan_begin = 0x70000000000ull;
    constexpr std::uintptr_t scan_end = 0x80000000000ull;
    if (shrink_lists_for_array(match.array, scan_begin, scan_end, false) == 0)
        return false;

    void *template_string = nullptr;
    std::uintptr_t original_strings[9] {};
    void *replacement[std::size(labels)] {};
    __try
    {
        const auto first_item = *reinterpret_cast<const std::uintptr_t *>(match.array + 0x20);
        template_string = *reinterpret_cast<void **>(first_item + match.string_offset);
        for (std::size_t i = 0; i < std::size(original_strings); ++i)
        {
            const auto item = *reinterpret_cast<const std::uintptr_t *>(
                match.array + 0x20 + i * sizeof(void *));
            original_strings[i] = *reinterpret_cast<const std::uintptr_t *>(item + match.string_offset);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    for (std::size_t i = 0; i < std::size(labels); ++i)
    {
        replacement[i] = create_probe_string(template_string, labels[i]);
        if (replacement[i] == nullptr)
            return false;
    }
    __try
    {
        for (std::size_t i = 0; i < std::size(labels); ++i)
        {
            const auto item = *reinterpret_cast<const std::uintptr_t *>(match.array + 0x20 + i * sizeof(void *));
            *reinterpret_cast<void **>(item + match.string_offset) = replacement[i];
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    g_last_label_item_array.store(match.array, std::memory_order_release);
    // Keep all nine entries; only verify that an active list references the array.
    lists_shrunk += shrink_lists_for_array(match.array, scan_begin, scan_end, false);
    std::size_t total_replaced = 0;
    for (std::size_t position = 0; position < std::size(labels); ++position)
    {
        total_replaced += replace_pointer_references(
            original_strings[position],
            reinterpret_cast<std::uintptr_t>(replacement[position]),
            scan_begin, scan_end);
    }
    if (total_replaced != 0)
        log_label_reference_replacement(total_replaced);
    return true;
}

void *create_probe_string(void *template_string, const wchar_t *text)
{
    const std::size_t length = wcslen(text);
    const std::size_t bytes = (0x14 + (length + 1) * sizeof(wchar_t) + 15) & ~std::size_t(15);
    auto *memory = static_cast<std::uint8_t *>(
        VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (memory == nullptr)
        return nullptr;
    void *string_class = nullptr;
    if (!read_pointer(reinterpret_cast<std::uintptr_t>(template_string), string_class) || string_class == nullptr)
    {
        VirtualFree(memory, 0, MEM_RELEASE);
        return nullptr;
    }
    *reinterpret_cast<void **>(&memory[0]) = string_class;
    *reinterpret_cast<void **>(&memory[8]) = nullptr;
    *reinterpret_cast<std::int32_t *>(&memory[0x10]) = static_cast<std::int32_t>(length);
    std::memcpy(&memory[0x14], text, (length + 1) * sizeof(wchar_t));
    return memory;
}

bool patch_label_array(std::uintptr_t array_address)
{
    constexpr const wchar_t *labels[] {
        L"0.2", L"0.3", L"原生", L"0.4", L"0.5", L"0.6", L"0.7", L"0.8", L"0.9"
    };
    void *template_string = nullptr;
    if (!read_pointer(array_address + 0x20, template_string) || template_string == nullptr)
        return false;

    void *replacement[std::size(labels)] {};
    for (std::size_t i = 0; i < std::size(labels); ++i)
    {
        replacement[i] = create_probe_string(template_string, labels[i]);
        if (replacement[i] == nullptr)
            return false;
    }

    __try
    {
        for (std::size_t i = 0; i < std::size(labels); ++i)
            *reinterpret_cast<void **>(array_address + 0x20 + i * sizeof(void *)) = replacement[i];
        *reinterpret_cast<std::uintptr_t *>(array_address + 0x18) = std::size(labels);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

std::size_t scan_and_patch_label_arrays(std::uintptr_t scan_begin = 0x70000000000ull,
    std::uintptr_t scan_end = 0x80000000000ull, bool always_log = true)
{
    std::uintptr_t first_strings[256] {};
    std::size_t first_string_count = 0;
    std::uintptr_t arrays[32] {};
    std::size_t array_count = 0;
    LabelObjectCandidate object_candidates[512] {};
    std::size_t object_candidate_count = 0;
    LabelItemArrayMatch item_arrays[32] {};
    std::size_t item_array_count = 0;

    for (std::uintptr_t address = scan_begin; address < scan_end;)
    {
        MEMORY_BASIC_INFORMATION memory {};
        if (VirtualQuery(reinterpret_cast<void *>(address), &memory, sizeof(memory)) != sizeof(memory))
            break;
        const auto region_base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        const auto next = region_base + memory.RegionSize;
        const auto clipped_begin = std::max(region_base, scan_begin);
        const auto clipped_end = std::min(next, scan_end);
        if (is_readable_region(memory))
            scan_region_for_06_strings(clipped_begin, clipped_end - clipped_begin,
                first_strings, std::size(first_strings), first_string_count);
        if (next <= address)
            break;
        address = next;
    }

    for (std::uintptr_t address = scan_begin; address < scan_end;)
    {
        MEMORY_BASIC_INFORMATION memory {};
        if (VirtualQuery(reinterpret_cast<void *>(address), &memory, sizeof(memory)) != sizeof(memory))
            break;
        const auto region_base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        const auto next = region_base + memory.RegionSize;
        const auto clipped_begin = std::max(region_base, scan_begin);
        const auto clipped_end = std::min(next, scan_end);
        if (is_writable_region(memory))
        {
            scan_region_for_label_arrays(clipped_begin, clipped_end - clipped_begin,
                first_strings, first_string_count, arrays, std::size(arrays), array_count);
            scan_region_for_label_object_candidates(clipped_begin, clipped_end - clipped_begin,
                first_strings, first_string_count, object_candidates, std::size(object_candidates),
                object_candidate_count);
        }
        if (next <= address)
            break;
        address = next;
    }

    for (std::uintptr_t address = scan_begin; address < scan_end;)
    {
        MEMORY_BASIC_INFORMATION memory {};
        if (VirtualQuery(reinterpret_cast<void *>(address), &memory, sizeof(memory)) != sizeof(memory))
            break;
        const auto region_base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        const auto next = region_base + memory.RegionSize;
        const auto clipped_begin = std::max(region_base, scan_begin);
        const auto clipped_end = std::min(next, scan_end);
        if (is_writable_region(memory))
            scan_region_for_label_item_arrays(clipped_begin, clipped_end - clipped_begin,
                object_candidates, object_candidate_count, item_arrays, std::size(item_arrays), item_array_count);
        if (next <= address)
            break;
        address = next;
    }

    std::size_t patched = 0;
    for (std::size_t i = 0; i < array_count; ++i)
        patched += patch_label_array(arrays[i]) ? 1 : 0;
    std::size_t item_arrays_patched = 0;
    std::size_t lists_shrunk = 0;
    for (std::size_t i = 0; i < item_array_count; ++i)
        item_arrays_patched += patch_label_item_array(item_arrays[i], lists_shrunk) ? 1 : 0;
    if (always_log || patched != 0 || item_arrays_patched != 0)
    {
        std::ostringstream message;
        message << "label_array_scan"
            << " mode=" << (always_log ? "full" : "automatic")
            << " first_strings=" << first_string_count
            << " arrays=" << array_count
            << " patched=" << patched
            << " object_candidates=" << object_candidate_count
            << " item_arrays=" << item_array_count
            << " item_arrays_patched=" << item_arrays_patched
            << " lists_shrunk=" << lists_shrunk
            << " last_item_array=0x" << std::hex
            << g_last_label_item_array.load(std::memory_order_acquire);
        log_line(message.str());
    }
    return patched + item_arrays_patched;
}

struct ContainerSnapshot
{
    bool valid = false;
    std::int32_t count = 0;
    void *nested_items = nullptr;
    std::uint32_t values[12] {};
    std::int32_t value_count = 0;
};

bool snapshot_container(void *object, ContainerSnapshot &snapshot)
{
    if (object == nullptr)
        return false;
    __try
    {
        const auto address = reinterpret_cast<std::uintptr_t>(object);
        snapshot.nested_items = *reinterpret_cast<void **>(address + 0x10);
        snapshot.count = *reinterpret_cast<const std::int32_t *>(address + 0x18);
        if (snapshot.count < 0 || snapshot.count > 256)
            return false;
        snapshot.value_count = std::min<std::int32_t>(snapshot.count, static_cast<std::int32_t>(std::size(snapshot.values)));
        for (std::int32_t i = 0; i < snapshot.value_count; ++i)
            snapshot.values[i] = *reinterpret_cast<const std::uint32_t *>(address + 0x20 + i * sizeof(std::uint32_t));
        snapshot.valid = true;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool rewrite_candidate_array(void *array, encoded_float_encoder_fn encoder)
{
    if (array == nullptr || encoder == nullptr)
        return false;

    // Keep all nine native entries. The visible UI order is
    // [0,1,2,3,4,5,6,7,8], so the unmodified native 1.0 mode is internal index 8.
    const float scales[] {
        0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f
    };
    __try
    {
        const auto address = reinterpret_cast<std::uintptr_t>(array);
        const auto capacity = *reinterpret_cast<const std::uintptr_t *>(address + 0x18);
        if (capacity < std::size(scales))
            return false;
        for (std::size_t i = 0; i < std::size(scales); ++i)
            encoder(reinterpret_cast<std::int32_t *>(address + 0x20 + i * sizeof(std::int32_t)), scales[i]);
        *reinterpret_cast<std::uintptr_t *>(address + 0x18) = std::size(scales);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void append_container_snapshot(std::ostringstream &message, const char *name, void *object)
{
    ContainerSnapshot direct {};
    message << ' ' << name << "=0x" << std::hex << reinterpret_cast<std::uintptr_t>(object);
    if (!snapshot_container(object, direct))
        return;

    message << "{count=" << std::dec << direct.count << ",nested=0x" << std::hex
        << reinterpret_cast<std::uintptr_t>(direct.nested_items) << ",u32=[";
    for (std::int32_t i = 0; i < direct.value_count; ++i)
    {
        if (i != 0)
            message << ',';
        float float_value = 0.0f;
        std::memcpy(&float_value, &direct.values[i], sizeof(float_value));
        message << "0x" << std::hex << direct.values[i] << '/' << std::dec << std::fixed << std::setprecision(3)
            << float_value;
    }
    message << "]}";

    ContainerSnapshot nested {};
    if (snapshot_container(direct.nested_items, nested))
    {
        message << "->array{count=" << std::dec << nested.count << ",u32=[";
        for (std::int32_t i = 0; i < nested.value_count; ++i)
        {
            if (i != 0)
                message << ',';
            float float_value = 0.0f;
            std::memcpy(&float_value, &nested.values[i], sizeof(float_value));
            message << "0x" << std::hex << nested.values[i] << '/' << std::dec << std::fixed
                << std::setprecision(3) << float_value;
        }
        message << "]}";
    }
}

LONG CALLBACK field_watch_exception_handler(EXCEPTION_POINTERS *exception)
{
    if (exception == nullptr || exception->ExceptionRecord == nullptr || exception->ContextRecord == nullptr)
        return EXCEPTION_CONTINUE_SEARCH;

    auto *record = exception->ExceptionRecord;
    auto *context = exception->ContextRecord;
    if (record->ExceptionCode == kArmFieldWatchException)
    {
        const auto address = g_watch_address.load(std::memory_order_acquire);
        if (address == 0)
            return EXCEPTION_CONTINUE_SEARCH;

        context->Dr0 = address;
        context->Dr6 = 0;
        context->Dr7 &= ~((1ull << 0) | (1ull << 1) | (0xFull << 16));
        const auto access_mode = g_config.field_access_watch_enabled ? 3ull : 1ull;
        context->Dr7 |= (1ull << 0) | (access_mode << 16) | (3ull << 18);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (record->ExceptionCode != EXCEPTION_SINGLE_STEP || (context->Dr6 & 3ull) == 0)
        return EXCEPTION_CONTINUE_SEARCH;

    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto next_instruction = static_cast<std::uintptr_t>(context->Rip);
    const auto next_rva = next_instruction >= base && next_instruction < base + 0x18FBE000ull
        ? next_instruction - base : 0;

    context->Dr6 &= ~1ull;
    const auto address = g_watch_address.load(std::memory_order_acquire);
    if (address == 0 || context->Dr0 != address)
        return EXCEPTION_CONTINUE_SEARCH;

    if (g_config.field_access_watch_enabled)
    {
        if (next_rva == 0)
            return EXCEPTION_CONTINUE_EXECUTION;
        const bool native_aa = g_native_aa_selected.load(std::memory_order_acquire);
        if (native_aa && next_rva == 0x680FCED)
        {
            // The trapped instruction loaded renderScale into XMM1 solely for
            // render-target dimension multiplication. Use exact 1.0 in the
            // register while leaving the stored field below 1.0 for FSR gates.
            context->Xmm1.Low = (context->Xmm1.Low & 0xFFFFFFFF00000000ull) | 0x3F800000ull;
            const auto count = g_native_aa_dimension_override_count.fetch_add(
                1, std::memory_order_relaxed) + 1;
            if (count == 1)
                log_line("native_aa_dimension_scale_override next_rva=0x680fced register_scale=1.0");
        }
        const auto key = (static_cast<std::uint64_t>(next_rva) << 1) | (native_aa ? 1ull : 0ull);
        bool inserted = false;
        {
            std::lock_guard lock(g_field_access_log_mutex);
            inserted = g_field_access_log_keys.insert(key).second;
        }
        if (inserted)
        {
            std::ostringstream message;
            message << "render_scale_field_access"
                << " mode=" << (native_aa ? "native_aa" : "scaled")
                << " field=0x" << std::hex << address
                << " thread=" << std::dec << GetCurrentThreadId()
                << " next_rva=0x" << std::hex << next_rva;
            log_line(message.str());
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    std::uint32_t value_bits = 0;
    if (!read_value_bits(address, value_bits))
        return EXCEPTION_CONTINUE_EXECUTION;

    const auto previous_rva = g_last_write_next_rva.exchange(next_rva, std::memory_order_relaxed);
    const auto previous_value = g_last_write_value_bits.exchange(value_bits, std::memory_order_relaxed);
    if (previous_rva != next_rva || previous_value != value_bits)
    {
        float value = 0.0f;
        static_assert(sizeof(value) == sizeof(value_bits));
        std::memcpy(&value, &value_bits, sizeof(value));
        std::ostringstream message;
        message << (g_config.source_scale_watch_enabled ? "source_scale_write" : "render_scale_field_write")
            << " value=" << std::fixed << std::setprecision(6) << value
            << " field=0x" << std::hex << address
            << " thread=" << std::dec << GetCurrentThreadId()
            << " next_rva=0x" << std::hex << next_rva;
        log_line(message.str());
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

void arm_field_write_watch(void *instance)
{
    if ((!g_config.field_write_watch_enabled && !g_config.field_access_watch_enabled) ||
        g_vectored_exception_handler == nullptr)
        return;

    const auto address = reinterpret_cast<std::uintptr_t>(instance) + g_config.render_scale_offset;
    const auto thread_id = GetCurrentThreadId();
    const auto previous_address = g_watch_address.load(std::memory_order_relaxed);
    const auto previous_thread = g_watch_thread_id.load(std::memory_order_relaxed);
    if (previous_address == address && previous_thread == thread_id)
        return;

    g_watch_address.store(address, std::memory_order_release);
    g_watch_thread_id.store(thread_id, std::memory_order_release);
    RaiseException(kArmFieldWatchException, 0, 0, nullptr);

    std::ostringstream message;
    message << "field_write_watch_armed"
        << " field=0x" << std::hex << address
        << " thread=" << std::dec << thread_id;
    log_line(message.str());
}

bool arm_field_write_watch_on_thread(DWORD thread_id, std::uintptr_t address)
{
    const HANDLE thread = OpenThread(
        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
        FALSE, thread_id);
    if (thread == nullptr)
        return false;

    bool success = false;
    if (SuspendThread(thread) != static_cast<DWORD>(-1))
    {
        CONTEXT context {};
        context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(thread, &context))
        {
            context.Dr0 = address;
            context.Dr6 = 0;
            context.Dr7 &= ~((1ull << 0) | (1ull << 1) | (0xFull << 16));
            const auto access_mode = g_config.field_access_watch_enabled ? 3ull : 1ull;
            context.Dr7 |= (1ull << 0) | (access_mode << 16) | (3ull << 18);
            success = SetThreadContext(thread, &context) != FALSE;
        }
        ResumeThread(thread);
    }
    CloseHandle(thread);
    return success;
}

void synchronize_field_write_watch_threads(std::uintptr_t address, std::unordered_set<DWORD> &armed_threads)
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;

    const DWORD process_id = GetCurrentProcessId();
    const DWORD current_thread_id = GetCurrentThreadId();
    std::uint32_t armed = 0;
    std::uint32_t failed = 0;
    THREADENTRY32 entry {};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry))
    {
        do
        {
            if (entry.th32OwnerProcessID != process_id || entry.th32ThreadID == current_thread_id ||
                armed_threads.contains(entry.th32ThreadID))
                continue;

            if (arm_field_write_watch_on_thread(entry.th32ThreadID, address))
            {
                armed_threads.insert(entry.th32ThreadID);
                ++armed;
            }
            else
            {
                ++failed;
            }
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);

    if (armed != 0 || failed != 0)
    {
        std::ostringstream message;
        message << "field_write_watch_thread_sync"
            << " field=0x" << std::hex << address
            << " armed=" << std::dec << armed
            << " failed=" << failed
            << " total=" << armed_threads.size();
        log_line(message.str());
    }
}

void __fastcall hooked_build_cmd_buffers(void *instance)
{
    arm_field_write_watch(instance);
    const std::uint64_t call = g_call_count.fetch_add(1, std::memory_order_relaxed) + 1;
    float before = 0.0f;
    if (read_scale(instance, before))
    {
        const bool changed = !g_has_previous_scale || std::fabs(before - g_previous_scale) > 0.0001f;
        const bool heartbeat = g_config.log_unchanged_every != 0 && call % g_config.log_unchanged_every == 0;
        if (changed || heartbeat)
        {
            const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
            std::ostringstream message;
            message << (changed ? "native_scale_change" : "native_scale_heartbeat")
                << " call=" << call
                << " value=" << std::fixed << std::setprecision(6) << before
                << " instance=0x" << std::hex << reinterpret_cast<std::uintptr_t>(instance)
                << " field=0x" << (reinterpret_cast<std::uintptr_t>(instance) + g_config.render_scale_offset)
                << " caller_rva=0x" << (caller >= base ? caller - base : 0);
            log_line(message.str());
        }
        g_previous_scale = before;
        g_has_previous_scale = true;

        if (g_config.override_enabled && !write_scale(instance, g_config.override_scale))
            log_line("override_write_failed");
    }
    else if (!g_read_fault_logged.exchange(true, std::memory_order_relaxed))
    {
        log_line("render_scale_read_failed");
    }

    bool native_aa_build_spoofed = false;
    float scale_to_restore = 0.0f;
    if (g_config.force_native_aa_fsr &&
        g_native_aa_selected.load(std::memory_order_acquire) &&
        read_scale(instance, scale_to_restore) && scale_to_restore >= 1.0f)
    {
        const float branch_scale = std::nextafter(1.0f, 0.0f);
        native_aa_build_spoofed = write_scale(instance, branch_scale);
        if (native_aa_build_spoofed)
        {
            const auto spoof_call = g_native_aa_build_spoof_count.fetch_add(1, std::memory_order_relaxed) + 1;
            if (spoof_call == 1)
            {
                std::ostringstream spoof_message;
                spoof_message << "native_aa_build_spoof"
                    << " call=" << spoof_call
                    << " stored_scale=" << std::fixed << std::setprecision(9) << scale_to_restore
                    << " branch_scale=" << branch_scale;
                log_line(spoof_message.str());
            }
        }
        else
        {
            log_line("native_aa_build_spoof_write_failed");
        }
    }

    if (g_original_build_cmd_buffers != nullptr)
        g_original_build_cmd_buffers(instance);

    if (native_aa_build_spoofed && !write_scale(instance, scale_to_restore))
        log_line("native_aa_build_restore_failed");
}

void __fastcall hooked_render_scale_setter(void *instance, float value)
{
    const std::uint64_t call = g_setter_call_count.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    std::ostringstream message;
    message << "render_scale_setter"
        << " call=" << call
        << " requested=" << std::fixed << std::setprecision(6) << value
        << " instance=0x" << std::hex << reinterpret_cast<std::uintptr_t>(instance)
        << " caller_rva=0x" << (caller >= base ? caller - base : 0);
    log_line(message.str());
    if (g_original_render_scale_setter != nullptr)
        g_original_render_scale_setter(instance, value);
}

void __fastcall hooked_render_scale_apply(float value, void *instance)
{
    if (g_original_render_scale_apply != nullptr)
        g_original_render_scale_apply(value, instance);

    float desired = 0.0f;
    const auto module_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!read_float_address(module_base + g_config.source_scale_rva, desired) ||
        desired < 0.199f || desired > 1.001f)
        return;
    float applied = 0.0f;
    if (read_scale(instance, applied) && std::fabs(applied - desired) > 0.0001f &&
        !write_scale(instance, desired))
        log_line("render_scale_menu_apply_failed");
}

std::uint8_t __fastcall hooked_graphics_float_setter(void *key, float value)
{
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    void *render_scale_key = nullptr;
    read_pointer(base + g_config.render_scale_key_pointer_rva, render_scale_key);

    float forwarded_value = value;
    if (il2cpp_string_equals(key, render_scale_key))
    {
        const bool native_aa = g_config.force_native_aa_fsr &&
            value >= g_config.native_aa_gate_scale - 0.0005f;
        g_native_aa_selected.store(native_aa, std::memory_order_release);
        if (native_aa)
            forwarded_value = g_config.native_aa_gate_scale;
        const auto call = g_graphics_setter_call_count.fetch_add(1, std::memory_order_relaxed) + 1;
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        std::ostringstream message;
        message << "render_scale_config_setter"
            << " call=" << call
            << " value=" << std::fixed << std::setprecision(6) << value
            << " forwarded=" << forwarded_value
            << " native_aa=" << (native_aa ? 1 : 0)
            << " key=0x" << std::hex << reinterpret_cast<std::uintptr_t>(key)
            << " caller_rva=0x" << (caller >= base ? caller - base : 0);
        log_line(message.str());
    }

    return g_original_graphics_float_setter != nullptr
        ? g_original_graphics_float_setter(key, forwarded_value) : 0;
}

void __fastcall hooked_graphics_option_apply(void *key, void *descriptor)
{
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    void *render_scale_key = nullptr;
    read_pointer(base + g_config.render_scale_key_pointer_rva, render_scale_key);
    const bool is_render_scale = il2cpp_string_equals(key, render_scale_key);

    if (is_render_scale)
    {
        const auto descriptor_address = reinterpret_cast<std::uintptr_t>(descriptor);
        const auto previous = g_last_render_scale_descriptor.exchange(descriptor_address, std::memory_order_relaxed);
        if (previous != descriptor_address)
        {
            void *fields[5] {};
            for (std::size_t i = 0; i < std::size(fields); ++i)
                read_pointer(descriptor_address + 0x10 + i * sizeof(void *), fields[i]);
            std::ostringstream message;
            message << "render_scale_descriptor"
                << " descriptor=0x" << std::hex << descriptor_address
                << " caller_rva=0x" << (reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - base);
            append_container_snapshot(message, "field10", fields[0]);
            append_container_snapshot(message, "field18", fields[1]);
            append_container_snapshot(message, "field20", fields[2]);
            append_container_snapshot(message, "field28", fields[3]);
            append_container_snapshot(message, "field30", fields[4]);
            log_line(message.str());
            log_object_string_graph("descriptor", descriptor);
            for (std::size_t i = 0; i < std::size(fields); ++i)
            {
                const std::string field_name = "descriptor_field" + std::to_string(0x10 + i * 8);
                log_object_string_graph(field_name.c_str(), fields[i]);
            }
        }
    }

    if (g_original_graphics_option_apply != nullptr)
        g_original_graphics_option_apply(key, descriptor);
}

void * __fastcall hooked_graphics_option_lookup(void *key, void *lookup_key, void *type_metadata)
{
    void *result = g_original_graphics_option_lookup != nullptr
        ? g_original_graphics_option_lookup(key, lookup_key, type_metadata)
        : nullptr;

    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    void *render_scale_key = nullptr;
    read_pointer(base + g_config.render_scale_key_pointer_rva, render_scale_key);
    if (!il2cpp_string_equals(key, render_scale_key) || result == nullptr)
        return result;

    const auto result_address = reinterpret_cast<std::uintptr_t>(result);
    void *candidate_array = nullptr;
    read_pointer(result_address + 0x18, candidate_array);
    if (g_config.candidate_list_patch_enabled && candidate_array != nullptr)
    {
        const auto encoder = reinterpret_cast<encoded_float_encoder_fn>(base + g_config.encoded_float_encoder_rva);
        const bool patched = rewrite_candidate_array(candidate_array, encoder);
        const auto candidate_address = reinterpret_cast<std::uintptr_t>(candidate_array);
        const auto previous_array = g_patched_candidate_array.exchange(candidate_address, std::memory_order_relaxed);
        if (previous_array != candidate_address)
        {
            std::ostringstream patch_message;
            patch_message << "candidate_list_patch"
                << " array=0x" << std::hex << candidate_address
                << " success=" << std::dec << (patched ? 1 : 0)
                << " internal_values=0.2,0.3,0.4,0.5,0.6,0.7,0.8,0.9,1.0"
                << " native_internal_index=8";
            log_line(patch_message.str());
        }
    }
    return result;
}

std::int32_t __fastcall hooked_graphics_index_resolver(
    void *key, std::int32_t requested, std::int32_t count, void *source, std::int32_t *status)
{
    const auto result = g_original_graphics_index_resolver != nullptr
        ? g_original_graphics_index_resolver(key, requested, count, source, status)
        : -1;

    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    void *render_scale_key = nullptr;
    read_pointer(base + g_config.render_scale_key_pointer_rva, render_scale_key);
    if (il2cpp_string_equals(key, render_scale_key))
    {
        if (g_config.label_array_scanner_enabled)
        {
            const auto now = GetTickCount64();
            const auto previous_tick = g_last_render_scale_index_tick.exchange(now, std::memory_order_acq_rel);
            // Treat the first resolver call after an idle gap as a menu-open edge.
            // Do not restart the expensive managed-heap scan window on every UI call.
            if (previous_tick == 0 || now < previous_tick || now - previous_tick >= 1000)
            {
                g_pending_label_scan_attempts.store(8, std::memory_order_release);
                g_next_label_scan_tick.store(now + 100, std::memory_order_release);
            }
        }
    }
    return result;
}

float __fastcall hooked_encoded_float_decoder(const std::int32_t *encoded_value)
{
    const float decoded = g_original_encoded_float_decoder != nullptr
        ? g_original_encoded_float_decoder(encoded_value)
        : 0.0f;

    std::uint32_t encoded = 0;
    if (encoded_value == nullptr || !read_value_bits(reinterpret_cast<std::uintptr_t>(encoded_value), encoded))
        return decoded;

    const auto count = g_render_scale_candidate_count.load(std::memory_order_acquire);
    std::int32_t candidate_index = -1;
    for (std::int32_t i = 0; i < count; ++i)
    {
        if (g_render_scale_candidate_encodings[i].load(std::memory_order_relaxed) == encoded)
        {
            candidate_index = i;
            break;
        }
    }
    if (candidate_index < 0)
        return decoded;
    if (candidate_index > g_config.candidate_decode_trace_max_index)
        return decoded;

    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (caller < base || caller - base >= 0x20000000)
        return decoded;
    const auto caller_rva = caller - base;
    const auto log_key = (static_cast<std::uint64_t>(caller_rva) << 8) |
        static_cast<std::uint32_t>(candidate_index);
    {
        std::lock_guard lock(g_candidate_decode_log_mutex);
        if (!g_candidate_decode_log_keys.insert(log_key).second)
            return decoded;
    }

    std::ostringstream message;
    message << "candidate_decode"
        << " index=" << candidate_index
        << " value=" << std::fixed << std::setprecision(6) << decoded
        << " encoded=0x" << std::hex << encoded
        << " caller_rva=0x" << caller_rva;
    void *frames[16] {};
    const USHORT frame_count = CaptureStackBackTrace(0, static_cast<DWORD>(std::size(frames)), frames, nullptr);
    message << " stack=[";
    bool first_frame = true;
    for (USHORT i = 0; i < frame_count; ++i)
    {
        const auto frame = reinterpret_cast<std::uintptr_t>(frames[i]);
        if (frame < base || frame - base >= 0x20000000)
            continue;
        if (!first_frame)
            message << ',';
        first_frame = false;
        message << "0x" << std::hex << (frame - base);
    }
    message << ']';
    log_line(message.str());
    return decoded;
}

DWORD WINAPI initialize_probe(void *)
{
    load_config();
    if (!g_config.enabled)
    {
        log_line("disabled_by_config");
        return 0;
    }

#if defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto apply_target = base + g_config.render_scale_apply_rva;
    const auto lookup_target = base + g_config.graphics_option_lookup_rva;
    const auto resolver_target = base + g_config.graphics_index_resolver_rva;
    const auto encoder_target = base + g_config.encoded_float_encoder_rva;
    const auto is_executable = [](std::uintptr_t address) {
        MEMORY_BASIC_INFORMATION memory {};
        return VirtualQuery(reinterpret_cast<void *>(address), &memory, sizeof(memory)) == sizeof(memory) &&
            memory.State == MEM_COMMIT &&
            (memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
    };
    if (!is_executable(apply_target) || !is_executable(lookup_target) ||
        !is_executable(resolver_target) || !is_executable(encoder_target))
    {
        log_line("target_not_executable refusing_runtime_hooks");
        return 0;
    }

    constexpr std::uint8_t expected_apply[] { 0x56, 0x57, 0x48, 0x83, 0xEC, 0x48, 0x0F, 0x29 };
    constexpr std::uint8_t expected_lookup[] { 0x55, 0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x81 };
    constexpr std::uint8_t expected_resolver[] { 0x55, 0x41, 0x57, 0x41, 0x56, 0x56, 0x57, 0x53 };
    constexpr std::uint8_t expected_encoder[] { 0x56, 0x53, 0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B };
    if (memcmp(reinterpret_cast<const void *>(apply_target), expected_apply, sizeof(expected_apply)) != 0 ||
        memcmp(reinterpret_cast<const void *>(lookup_target), expected_lookup, sizeof(expected_lookup)) != 0 ||
        memcmp(reinterpret_cast<const void *>(resolver_target), expected_resolver, sizeof(expected_resolver)) != 0 ||
        memcmp(reinterpret_cast<const void *>(encoder_target), expected_encoder, sizeof(expected_encoder)) != 0)
    {
        log_line("target_prefix_mismatch_refusing_runtime_hooks");
        return 0;
    }

    if (MH_Initialize() != MH_OK ||
        MH_CreateHook(reinterpret_cast<void *>(apply_target), reinterpret_cast<void *>(&hooked_render_scale_apply),
            reinterpret_cast<void **>(&g_original_render_scale_apply)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<void *>(lookup_target), reinterpret_cast<void *>(&hooked_graphics_option_lookup),
            reinterpret_cast<void **>(&g_original_graphics_option_lookup)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<void *>(resolver_target), reinterpret_cast<void *>(&hooked_graphics_index_resolver),
            reinterpret_cast<void **>(&g_original_graphics_index_resolver)) != MH_OK ||
        MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
    {
        log_line("runtime_hook_install_failed");
        return 0;
    }

    log_line("hook_ready runtime_hooks=3");
    while (g_config.label_array_scanner_enabled)
    {
        const auto now = GetTickCount64();
        const auto pending = g_pending_label_scan_attempts.load(std::memory_order_acquire);
        if (pending > 0 && now >= g_next_label_scan_tick.load(std::memory_order_acquire))
        {
            const auto patched = scan_and_patch_label_arrays(
                0x70000000000ull, 0x80000000000ull, pending == 1);
            if (patched != 0)
                g_pending_label_scan_attempts.store(0, std::memory_order_release);
            else
            {
                g_pending_label_scan_attempts.fetch_sub(1, std::memory_order_acq_rel);
                g_next_label_scan_tick.store(now + 750, std::memory_order_release);
            }
        }
        Sleep(100);
    }
    return 0;
#else

    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto build_target = base + g_config.build_cmd_buffers_rva;
    const auto setter_target = base + g_config.render_scale_setter_rva;
    const auto apply_target = base + g_config.render_scale_apply_rva;
    const auto graphics_setter_target = base + g_config.graphics_float_setter_rva;
    const auto graphics_option_apply_target = base + g_config.graphics_option_apply_rva;
    const auto graphics_option_lookup_target = base + g_config.graphics_option_lookup_rva;
    const auto graphics_index_resolver_target = base + g_config.graphics_index_resolver_rva;
    const auto encoded_float_decoder_target = base + g_config.encoded_float_decoder_rva;
    const auto encoded_float_encoder_target = base + g_config.encoded_float_encoder_rva;
    const auto is_executable = [](std::uintptr_t address) {
        MEMORY_BASIC_INFORMATION memory {};
        return VirtualQuery(reinterpret_cast<void *>(address), &memory, sizeof(memory)) == sizeof(memory) &&
            memory.State == MEM_COMMIT &&
            (memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
    };
    if (!is_executable(build_target) || !is_executable(setter_target) || !is_executable(apply_target) ||
        !is_executable(graphics_setter_target) || !is_executable(graphics_option_apply_target) ||
        !is_executable(graphics_option_lookup_target) || !is_executable(graphics_index_resolver_target) ||
        !is_executable(encoded_float_decoder_target) || !is_executable(encoded_float_encoder_target))
    {
        std::ostringstream message;
        message << "target_not_executable build_rva=0x" << std::hex << g_config.build_cmd_buffers_rva
            << " setter_rva=0x" << g_config.render_scale_setter_rva
            << " apply_rva=0x" << g_config.render_scale_apply_rva
            << " graphics_setter_rva=0x" << g_config.graphics_float_setter_rva
            << " graphics_option_apply_rva=0x" << g_config.graphics_option_apply_rva
            << " graphics_option_lookup_rva=0x" << g_config.graphics_option_lookup_rva
            << " graphics_index_resolver_rva=0x" << g_config.graphics_index_resolver_rva
            << " encoded_float_decoder_rva=0x" << g_config.encoded_float_decoder_rva
            << " encoded_float_encoder_rva=0x" << g_config.encoded_float_encoder_rva;
        log_line(message.str());
        return 0;
    }

    constexpr std::uint8_t expected_build_prefix[] { 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54 };
    constexpr std::uint8_t expected_setter_prefix[] { 0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0xCE };
    constexpr std::uint8_t expected_apply_prefix[] { 0x56, 0x57, 0x48, 0x83, 0xEC, 0x48, 0x0F, 0x29 };
    constexpr std::uint8_t expected_graphics_setter_prefix[] { 0x56, 0x57, 0x48, 0x83, 0xEC, 0x38, 0x0F, 0x29 };
    constexpr std::uint8_t expected_graphics_option_apply_prefix[] { 0x56, 0x57, 0x55, 0x53, 0x48, 0x83, 0xEC, 0x48 };
    constexpr std::uint8_t expected_graphics_option_lookup_prefix[] { 0x55, 0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x81 };
    constexpr std::uint8_t expected_graphics_index_resolver_prefix[] { 0x55, 0x41, 0x57, 0x41, 0x56, 0x56, 0x57, 0x53 };
    constexpr std::uint8_t expected_encoded_float_decoder_prefix[] { 0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x05 };
    constexpr std::uint8_t expected_encoded_float_encoder_prefix[] { 0x56, 0x53, 0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B };
    if (memcmp(reinterpret_cast<const void *>(build_target), expected_build_prefix, sizeof(expected_build_prefix)) != 0 ||
        memcmp(reinterpret_cast<const void *>(setter_target), expected_setter_prefix, sizeof(expected_setter_prefix)) != 0 ||
        memcmp(reinterpret_cast<const void *>(apply_target), expected_apply_prefix, sizeof(expected_apply_prefix)) != 0 ||
        memcmp(reinterpret_cast<const void *>(graphics_setter_target), expected_graphics_setter_prefix,
            sizeof(expected_graphics_setter_prefix)) != 0 ||
        memcmp(reinterpret_cast<const void *>(graphics_option_apply_target), expected_graphics_option_apply_prefix,
            sizeof(expected_graphics_option_apply_prefix)) != 0 ||
        memcmp(reinterpret_cast<const void *>(graphics_option_lookup_target), expected_graphics_option_lookup_prefix,
            sizeof(expected_graphics_option_lookup_prefix)) != 0 ||
        memcmp(reinterpret_cast<const void *>(graphics_index_resolver_target), expected_graphics_index_resolver_prefix,
            sizeof(expected_graphics_index_resolver_prefix)) != 0 ||
        memcmp(reinterpret_cast<const void *>(encoded_float_decoder_target), expected_encoded_float_decoder_prefix,
            sizeof(expected_encoded_float_decoder_prefix)) != 0 ||
        memcmp(reinterpret_cast<const void *>(encoded_float_encoder_target), expected_encoded_float_encoder_prefix,
            sizeof(expected_encoded_float_encoder_prefix)) != 0)
    {
        log_line("target_prefix_mismatch_refusing_hook");
        return 0;
    }

    std::ostringstream ready;
    ready << "targets_validated build_rva=0x" << std::hex << g_config.build_cmd_buffers_rva
        << " setter_rva=0x" << g_config.render_scale_setter_rva
        << " apply_rva=0x" << g_config.render_scale_apply_rva
        << " field_offset=0x" << g_config.render_scale_offset
        << " field_write_watch=" << std::dec << (g_config.field_write_watch_enabled ? 1 : 0)
        << " field_access_watch=" << (g_config.field_access_watch_enabled ? 1 : 0)
        << " source_scale_rva=0x" << std::hex << g_config.source_scale_rva
        << " source_scale_write_watch=" << std::dec << (g_config.source_scale_watch_enabled ? 1 : 0)
        << " graphics_setter_rva=0x" << std::hex << g_config.graphics_float_setter_rva
        << " graphics_option_apply_rva=0x" << g_config.graphics_option_apply_rva
        << " graphics_option_lookup_rva=0x" << g_config.graphics_option_lookup_rva
        << " graphics_index_resolver_rva=0x" << g_config.graphics_index_resolver_rva
        << " encoded_float_decoder_rva=0x" << g_config.encoded_float_decoder_rva
        << " encoded_float_encoder_rva=0x" << g_config.encoded_float_encoder_rva
        << " render_scale_key_pointer_rva=0x" << g_config.render_scale_key_pointer_rva
        << " candidate_decode_trace=" << std::dec << (g_config.candidate_decode_trace_enabled ? 1 : 0)
        << " candidate_decode_trace_max_index=" << g_config.candidate_decode_trace_max_index
        << " object_string_graph_trace=" << (g_config.object_string_graph_trace_enabled ? 1 : 0)
        << " label_array_scanner=" << (g_config.label_array_scanner_enabled ? 1 : 0)
        << " candidate_list_patch=" << (g_config.candidate_list_patch_enabled ? 1 : 0)
        << " apply_candidate_source_scale=" << (g_config.apply_candidate_source_scale ? 1 : 0)
        << " force_native_aa_fsr=" << (g_config.force_native_aa_fsr ? 1 : 0)
        << " native_aa_gate_scale=" << g_config.native_aa_gate_scale
        << " apply_unclamped_scale=" << (g_config.apply_unclamped_scale ? 1 : 0)
        << " override=" << std::dec << (g_config.override_enabled ? 1 : 0)
        << " override_scale=" << g_config.override_scale;
    log_line(ready.str());

    if (MH_Initialize() != MH_OK)
    {
        log_line("minhook_initialize_failed");
        return 0;
    }
    if (g_config.field_write_watch_enabled || g_config.field_access_watch_enabled ||
        g_config.source_scale_watch_enabled)
    {
        g_vectored_exception_handler = AddVectoredExceptionHandler(1, field_watch_exception_handler);
        if (g_vectored_exception_handler == nullptr)
        {
            log_line("add_vectored_exception_handler_failed");
            return 0;
        }
        if (g_config.source_scale_watch_enabled)
            g_watch_address.store(base + g_config.source_scale_rva, std::memory_order_release);
    }
    if (MH_CreateHook(reinterpret_cast<void *>(build_target), reinterpret_cast<void *>(&hooked_build_cmd_buffers),
            reinterpret_cast<void **>(&g_original_build_cmd_buffers)) != MH_OK)
    {
        log_line("create_hook_failed");
        return 0;
    }
    if (MH_CreateHook(reinterpret_cast<void *>(setter_target), reinterpret_cast<void *>(&hooked_render_scale_setter),
            reinterpret_cast<void **>(&g_original_render_scale_setter)) != MH_OK)
    {
        log_line("create_setter_hook_failed");
        return 0;
    }
    if (MH_CreateHook(reinterpret_cast<void *>(apply_target), reinterpret_cast<void *>(&hooked_render_scale_apply),
            reinterpret_cast<void **>(&g_original_render_scale_apply)) != MH_OK)
    {
        log_line("create_apply_hook_failed");
        return 0;
    }
    if (MH_CreateHook(reinterpret_cast<void *>(graphics_setter_target),
            reinterpret_cast<void *>(&hooked_graphics_float_setter),
            reinterpret_cast<void **>(&g_original_graphics_float_setter)) != MH_OK)
    {
        log_line("create_graphics_setter_hook_failed");
        return 0;
    }
    if (MH_CreateHook(reinterpret_cast<void *>(graphics_option_apply_target),
            reinterpret_cast<void *>(&hooked_graphics_option_apply),
            reinterpret_cast<void **>(&g_original_graphics_option_apply)) != MH_OK)
    {
        log_line("create_graphics_option_apply_hook_failed");
        return 0;
    }
    if (MH_CreateHook(reinterpret_cast<void *>(graphics_option_lookup_target),
            reinterpret_cast<void *>(&hooked_graphics_option_lookup),
            reinterpret_cast<void **>(&g_original_graphics_option_lookup)) != MH_OK)
    {
        log_line("create_graphics_option_lookup_hook_failed");
        return 0;
    }
    if (MH_CreateHook(reinterpret_cast<void *>(graphics_index_resolver_target),
            reinterpret_cast<void *>(&hooked_graphics_index_resolver),
            reinterpret_cast<void **>(&g_original_graphics_index_resolver)) != MH_OK)
    {
        log_line("create_graphics_index_resolver_hook_failed");
        return 0;
    }
    if (g_config.candidate_decode_trace_enabled &&
        MH_CreateHook(reinterpret_cast<void *>(encoded_float_decoder_target),
            reinterpret_cast<void *>(&hooked_encoded_float_decoder),
            reinterpret_cast<void **>(&g_original_encoded_float_decoder)) != MH_OK)
    {
        log_line("create_encoded_float_decoder_hook_failed");
        return 0;
    }
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
    {
        log_line("enable_hook_failed");
        return 0;
    }
    log_line("hook_ready observation_only=" + std::to_string(g_config.override_enabled ? 0 : 1));

    if (g_config.label_array_scanner_enabled)
    {
        log_line("label_array_scanner_ready menu_trigger=1 automatic_repatch=1");
        std::uintptr_t synchronized_field_address = 0;
        std::unordered_set<DWORD> field_armed_threads;
        ULONGLONG next_watch_sync = 0;
        for (;;)
        {
            const auto now = GetTickCount64();
            const auto pending_attempts = g_pending_label_scan_attempts.load(std::memory_order_acquire);
            if (pending_attempts > 0 && now >= g_next_label_scan_tick.load(std::memory_order_acquire))
            {
                const auto patched = scan_and_patch_label_arrays(
                    0x70000000000ull, 0x80000000000ull, pending_attempts == 1);
                if (patched != 0)
                {
                    g_pending_label_scan_attempts.store(0, std::memory_order_release);
                }
                else
                {
                    g_pending_label_scan_attempts.fetch_sub(1, std::memory_order_acq_rel);
                    g_next_label_scan_tick.store(now + 750, std::memory_order_release);
                }
            }
            if (now >= next_watch_sync)
            {
                const auto field_address = g_watch_address.load(std::memory_order_acquire);
                if (field_address != 0 &&
                    (g_config.field_write_watch_enabled || g_config.field_access_watch_enabled ||
                        g_config.source_scale_watch_enabled))
                {
                    if (field_address != synchronized_field_address)
                    {
                        synchronized_field_address = field_address;
                        field_armed_threads.clear();
                    }
                    synchronize_field_write_watch_threads(field_address, field_armed_threads);
                }

                next_watch_sync = now + 500;
            }
            Sleep(100);
        }
    }

    if (g_config.field_write_watch_enabled || g_config.field_access_watch_enabled ||
        g_config.source_scale_watch_enabled)
    {
        std::uintptr_t synchronized_address = 0;
        std::unordered_set<DWORD> armed_threads;
        for (;;)
        {
            const auto address = g_watch_address.load(std::memory_order_acquire);
            if (address != 0)
            {
                if (address != synchronized_address)
                {
                    synchronized_address = address;
                    armed_threads.clear();
                }
                synchronize_field_write_watch_threads(address, armed_threads);
            }
            Sleep(500);
        }
    }
    return 0;
#endif
}
}

void initialize_render_scale_menu(HMODULE bridge_module, render_scale_menu_log_fn log_callback)
{
    if (g_started.exchange(true, std::memory_order_acq_rel))
        return;
    g_module = bridge_module;
    g_log_callback = log_callback;
    if (HANDLE thread = CreateThread(nullptr, 0, initialize_probe, nullptr, 0, nullptr))
        CloseHandle(thread);
    else
        log_line("create_worker_thread_failed");
}
