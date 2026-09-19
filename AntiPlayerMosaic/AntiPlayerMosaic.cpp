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

// 日志级别（2026-09-19 审核报告：替换"按英文关键词过滤"）。
//
// **原实现的问题**：用 10 个英文关键词（failed / disabled / error …）决定一行是否落盘。
//   - 误**丢**：成功与生命周期行不含这些词 → 全部被静默丢弃。结果是
//     **插件正常工作时日志为空**，完全无法用它验证插件是否生效。
//     例：`AntiPlayerMosaic loaded`、`patched PlayerPerspective with main-thread hook`、
//     `main module ready after X ms`、`HideUID active: hidden N ui targets` 都不落盘。
//   - 更根本：靠自然语言措辞决定日志去留，**任何一次文案改动都可能静默改变行为**。
//
// 说明：本文件的重复性输出**已经有原子守卫**（`g_hide_uid_logged_*` 的
// `exchange(true)`、`g_hide_uid_next_tick` 的 CAS 退避），所以并不存在刷屏风险；
// 关键词过滤纯属多余且有害。
//
// **现在**：显式级别。Release 下 INFO 常开（每条都是一次性、可验证的生命周期事实），
// DEBUG 仅在非 release 构建输出。当前没有高频细节行需要 DEBUG，
// 故只保留级别参数本身，不留未使用的 log_debug 包装（避免死代码）。
//
// ⚠️ 2026-09-19：枚举与 `log_line` 的声明移到**匿名命名空间之前** ——
// `read_cached_signature` / `scan_unique_signature`（在命名空间内、`log_line` 定义之前）
// 需要在签名模式解析失败时记日志，原先它们看不到 `log_line`。
enum class LogLevel
{
    Info,
    Debug
};

// `log_line` 的**定义**必须在匿名命名空间**之外**（2026-09-19）。
//
// 原因：`read_cached_signature` / `scan_unique_signature` 需要调用它，而这两个函数
// 在匿名命名空间内、`log_line` 原定义之前。若只在命名空间外声明、在命名空间内定义，
// 那是**两个不同的函数**（内部链接 vs 外部链接）→ 链接期 `LNK2019`。
// 故把定义与它依赖的两个全局一起放在命名空间外（外部链接，声明与定义一致）。
std::filesystem::path g_log_path;
std::mutex g_log_mutex;

void log_line(const std::string &line, LogLevel level = LogLevel::Info)
{
#if defined(ANTIPLAYER_RELEASE_RUNTIME)
    // Release：只保留 INFO。生命周期行必须可见 —— 否则无法验证插件是否生效。
    if (level == LogLevel::Debug)
        return;
#else
    (void)level;
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

namespace
{
// "主模块已解析"门控（2026-09-19 审核报告）。
//
// 原名 `g_main_base`（`std::uint8_t *`），但**它存的值从未被读取** ——
// 唯一的用途是 `if (g_main_base != nullptr)` 这一处非空判断
//（扫描工作全部使用 `resolve_targets()` 内的局部 `base`）。
// 即：用一个指针当布尔用，语义误导。
//
// 门控**必须保留**（不是死代码）：`hide_uid_from_main_thread` 是插进
// `PlayerPerspective` 的 stub 回调，可能在签名解析完成前就被游戏主线程调用；
// 而 `hide_uid_once()` 在所需签名未解析时会 `g_hide_uid_enabled.store(false)`
// **永久禁用** HideUID。所以"未就绪时直接返回"是必要的保护。
//
// 跨线程：worker 线程写、游戏主线程读 → 用 `atomic_bool`（原为普通指针，
// 属数据竞争）。
std::atomic_bool g_main_module_resolved { false };
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
constexpr ULONGLONG k_hide_uid_retry_cap_ms = 64000; // 连续失败退避上限（约 1 分钟）
std::atomic<UINT32> g_hide_uid_retry_count { 0 };
std::array<void *, k_hide_uid_paths.size()> g_hide_uid_string_cache {};
std::array<void *, k_hide_uid_paths.size()> g_hide_uid_object_cache {};
std::atomic_int g_hide_uid_exception_streak { 0 };

struct ModuleFingerprint
{
    std::uint32_t timestamp = 0;
    std::uint32_t image_size = 0;
    std::uint32_t checksum = 0;
};

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

std::filesystem::path feature_cache_path(HMODULE module)
{
    wchar_t path[MAX_PATH] {};
    const auto length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    return std::filesystem::path(std::wstring(path, path + length)).parent_path() /
        L"AntiPlayerMosaic.features.cache";
}

// 2026-09-19（审核报告）：删除未使用的 `fingerprint` 参数。
// 它此前被 `(void)fingerprint;` 显式丢弃 —— 本函数**已经**做了等价于指纹校验的
// 边界与内容检查（RVA 必须落在已提交的可执行页内，且该处机器码仍匹配签名），
// 所以指纹参数是冗余的。删除而非"恢复校验"，避免重复同一件事。
std::uint8_t *read_cached_signature(HMODULE module,
    const pattern_scanner::Signature &signature, std::uint32_t expected_rva)
{
    const auto base = reinterpret_cast<std::uint8_t *>(module);
    auto *address = base + expected_rva;
    MEMORY_BASIC_INFORMATION memory {};
    if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory) || memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0)
        return nullptr;
    const auto pattern = pattern_scanner::parse_pattern(signature.text);
    // 模式解析不完整 → 显式失败（2026-09-19 审核报告：原为静默截断）。
    // 缓存路径也走这里：若签名被改坏，缓存命中也必须拒绝，否则会把
    // "用错误模式算出的 RVA" 当成有效缓存。
    if (!pattern.valid)
    {
        log_line(std::string(signature.name) + " pattern parse failed at offset=" +
            std::to_string(pattern.error_offset) + " (signature text malformed)");
        return nullptr;
    }
    const auto region_end = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
    if (reinterpret_cast<std::uintptr_t>(address) > region_end ||
        !pattern_scanner::matches_at(address, region_end - reinterpret_cast<std::uintptr_t>(address), 0, pattern))
        return nullptr;
    return address;
}

bool read_feature_cache(HMODULE module, const ModuleFingerprint &fingerprint,
    std::array<std::uint32_t, 5> &rvas)
{
    std::ifstream input(feature_cache_path(module));
    if (!input)
        return false;
    std::string key;
    std::uint64_t timestamp = 0, image_size = 0, checksum = 0;
    std::array<bool, 5> seen {};
    while (input >> key)
    {
        if (key == "timestamp") input >> timestamp;
        else if (key == "image_size") input >> image_size;
        else if (key == "checksum") input >> checksum;
        else if (key.rfind("rva", 0) == 0)
        {
            const auto index = static_cast<std::size_t>(std::stoul(key.substr(3)));
            if (index < rvas.size()) { input >> rvas[index]; seen[index] = true; }
        }
        else { std::string ignored; input >> ignored; }
    }
    return timestamp == fingerprint.timestamp && image_size == fingerprint.image_size &&
        checksum == fingerprint.checksum && std::all_of(seen.begin(), seen.end(), [](bool value) { return value; });
}

void write_feature_cache(HMODULE module, const ModuleFingerprint &fingerprint,
    const std::array<std::uint8_t *, 5> &targets)
{
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    std::ofstream output(feature_cache_path(module), std::ios::trunc);
    if (!output)
        return;
    output << "version 1\n" << "timestamp " << fingerprint.timestamp << "\n"
        << "image_size " << fingerprint.image_size << "\n" << "checksum " << fingerprint.checksum << "\n";
    for (std::size_t i = 0; i < targets.size(); ++i)
        output << "rva" << i << " " << (reinterpret_cast<std::uintptr_t>(targets[i]) - base) << "\n";
}

using find_string_fn = void *(__fastcall *)(const char *);
using find_object_fn = void *(__fastcall *)(void *);
using object_active_fn = void(__fastcall *)(void *, bool);

bool hide_uid_once();

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
        g_hide_uid_string_cache.fill(nullptr);
        g_hide_uid_object_cache.fill(nullptr);
        const int streak = g_hide_uid_exception_streak.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!g_hide_uid_logged_cache_reset.exchange(true))
            log_line("HideUID cache reset after exception");

        if (streak >= 3)
        {
            if (!g_hide_uid_logged_failure.exchange(true))
                log_line("HideUID disabled: repeated exceptions while using cached objects");
            g_hide_uid_enabled.store(false);
        }
        return false;
    }

    g_hide_uid_exception_streak.store(0, std::memory_order_relaxed);
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
    if (g_main_module_resolved.load(std::memory_order_acquire))
    {
        const ULONGLONG now = GetTickCount64();
        ULONGLONG next_allowed = g_hide_uid_next_tick.load(std::memory_order_relaxed);
        if (now < next_allowed)
            return;

        // Claim the time slot with a CAS so concurrent callers bail out here,
        // keeping hide_uid_once and the cache arrays single-threaded.
        if (!g_hide_uid_next_tick.compare_exchange_strong(
                next_allowed, now + k_hide_uid_retry_interval_ms, std::memory_order_relaxed))
            return;

        const bool hidden = hide_uid_once();
        if (hidden)
        {
            g_hide_uid_retry_count.store(0, std::memory_order_relaxed);
            g_hide_uid_next_tick.store(now + k_hide_uid_steady_interval_ms, std::memory_order_relaxed);
        }
        else
        {
            // 连续失败指数退避（封顶），成功后复位
            const UINT32 retries = g_hide_uid_retry_count.fetch_add(1, std::memory_order_relaxed) + 1;
            const ULONGLONG backoff = std::min<ULONGLONG>(
                k_hide_uid_retry_interval_ms * (1ull << std::min<UINT32>(retries, 6u)),
                k_hide_uid_retry_cap_ms);
            g_hide_uid_next_tick.store(now + backoff, std::memory_order_relaxed);
        }
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

    // sub rsp,0x28 / mov rax,callback / call rax / add rsp,0x28 / xor eax,eax / ret
    // The trailing xor eax,eax gives the replaced PlayerPerspective a defined
    // return value of 0, matching the mov eax,0 used by patch_player_dive_mosaic.
    std::array<std::uint8_t, 23> code {
        0x48, 0x83, 0xEC, 0x28,
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
        0xFF, 0xD0,
        0x48, 0x83, 0xC4, 0x28,
        0x31, 0xC0,
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
    // 模式解析不完整 → 显式失败（2026-09-19 审核报告：原为静默截断）。
    // 这条是主扫描路径：静默截断会产出"部分模式"→ 扫描不到 → 只表现为
    // "签名未找到"，无法区分是"游戏版本变了"还是"签名文本被改坏"。
    if (!pattern.valid)
    {
        log_line(std::string(signature.name) + " pattern parse failed at offset=" +
            std::to_string(pattern.error_offset) + " (signature text malformed)");
        return nullptr;
    }
    const auto *sections = IMAGE_FIRST_SECTION(nt);
    std::array<std::uint8_t *, 2> matches {};
    std::size_t match_count = 0;
    // 2026-09-19（审核报告）：独立统计**真实**匹配数。
    // 原先只报 match_count，而它在 matches 满 2 后就不再增长 ——
    // 实际有 50 处匹配时日志也写 "matches=2"，诊断时**信息失真**
    // （看不出是"2 处"还是"到处都是"）。
    // 保留 matches 容量 2 是**有意的**：只需区分 0 / 1 / ≥2 三种情况
    //（1 才可用），提前停止扫描可省下其余匹配的成本。
    std::size_t total_matches = 0;

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
        total_matches += section_matches.size();
        for (const auto offset : section_matches)
            matches[match_count++] = base + section.VirtualAddress + offset;
    }

    if (match_count != 1)
    {
        // 报真实总数；>=2 时明确标注"至少"（因为扫描已提前停止，可能还有更多）
        const std::string reported = match_count >= matches.size()
            ? (">=" + std::to_string(total_matches))
            : std::to_string(total_matches);
        log_line(std::string(signature.name) + " scan failed: matches=" + reported);
        return nullptr;
    }

    char message[128] {};
    const auto rva = static_cast<std::uintptr_t>(matches[0] - base);
    std::snprintf(message, sizeof(message), "%.*s resolved: rva=0x%08llX",
        static_cast<int>(signature.name.size()), signature.name.data(), static_cast<unsigned long long>(rva));
    log_line(message);
    return matches[0];
}

// 有界等待主模块代码段可读（替代固定 Sleep(3000)，2026-09-19 审核报告）。
//
// 判定依据：主模块映像的**首个页面**可读且已提交。这不足以证明游戏已完成所有
// 运行时初始化，但比"固定睡 3 秒"更贴近真实条件：
//   - 就绪早 → 立刻继续（不再白等）
//   - 就绪晚 → 继续轮询到上限（不再过早扫描导致静默失效）
// 返回实际等待的毫秒数，供日志记录（便于诊断"启动慢"导致的失败）。
constexpr ULONGLONG k_init_wait_timeout_ms = 30000; // 上限 30s（与旧行为同数量级，但可提前退出）
constexpr DWORD k_init_poll_interval_ms = 25;       // 轮询间隔：足够细，且不烧 CPU

ULONGLONG wait_for_main_module_ready()
{
    const ULONGLONG start = GetTickCount64();
    const ULONGLONG deadline = start + k_init_wait_timeout_ms;

    for (;;)
    {
        HMODULE main_module = GetModuleHandleW(nullptr);
        if (main_module != nullptr)
        {
            MEMORY_BASIC_INFORMATION info {};
            if (VirtualQuery(main_module, &info, sizeof(info)) == sizeof(info) &&
                info.State == MEM_COMMIT &&
                (info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                                 PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0)
            {
                const ULONGLONG waited = GetTickCount64() - start;
                log_line("main module ready after " + std::to_string(waited) + " ms");
                return waited;
            }
        }
        if (GetTickCount64() >= deadline)
        {
            const ULONGLONG waited = GetTickCount64() - start;
            log_line("main module not confirmed ready after " + std::to_string(waited) +
                     " ms; scanning anyway");
            return waited;
        }
        Sleep(k_init_poll_interval_ms);
    }
}

DWORD WINAPI worker_thread(void *parameter)
{
    // 2026-09-19（审核报告）：日志路径改在**本线程**计算。
    // 原先在 DllMain 里调用 module_dir()（内部 GetModuleFileNameW +
    // std::filesystem 构造）—— 那是在 **loader lock 持有期间**执行，
    // 是官方明确不建议的反模式（可能死锁/在锁内分配）。
    // 现在 DllMain 只做 pin + CreateThread，模块句柄经参数传入本线程。
    const auto self_module = static_cast<HMODULE>(parameter);
    if (self_module != nullptr)
        g_log_path = module_dir(self_module) / "AntiPlayerMosaic.log";

    reset_release_log();
    log_line("AntiPlayerMosaic loaded (dynamic scan)");

    // 2026-09-19（审核报告）：原为固定 `Sleep(3000)` 等待游戏初始化 ——
    // 游戏启动慢于 3 秒时会**过早扫描**（签名找不到 → 功能静默失效），
    // 启动快时又白白浪费 3 秒。改为**有界轮询**：轮询主模块代码段可读性，
    // 就绪即继续，最长仍等 k_init_wait_timeout_ms。
    wait_for_main_module_ready();

    HMODULE main_module = GetModuleHandleW(nullptr);
    MODULEINFO module_info {};
    if (!main_module || !K32GetModuleInformation(GetCurrentProcess(), main_module, &module_info, sizeof(module_info)))
    {
        log_line("main module info failed");
        return 0;
    }

    auto *base = static_cast<std::uint8_t *>(module_info.lpBaseOfDll);
    const std::size_t size = static_cast<std::size_t>(module_info.SizeOfImage);
    // 置位门控（release）：此后 stub 回调允许执行 hide_uid_once。
    // 注意置位点与旧实现一致 —— 都在**扫描签名之前**，语义不变。
    g_main_module_resolved.store(true, std::memory_order_release);

    wchar_t executable_path[MAX_PATH] {};
    GetModuleFileNameW(main_module, executable_path, MAX_PATH);
    log_line("game executable: " + std::filesystem::path(executable_path).filename().string());

    std::array<std::uint8_t *, 5> targets {};
    ModuleFingerprint fingerprint {};
    std::array<std::uint32_t, 5> cached_rvas {};
    bool cache_valid = get_module_fingerprint(main_module, fingerprint) &&
        read_feature_cache(main_module, fingerprint, cached_rvas);
    if (cache_valid)
    {
        for (std::size_t index = 0; index < targets.size(); ++index)
        {
            targets[index] = read_cached_signature(main_module,
                pattern_scanner::k_signatures[index], cached_rvas[index]);
            if (targets[index] == nullptr)
            {
                cache_valid = false;
                break;
            }
        }
    }
    if (cache_valid)
    {
        log_line("feature cache hit");
    }
    else
    {
        for (std::size_t index = 0; index < targets.size(); ++index)
            targets[index] = scan_unique_signature(base, size, pattern_scanner::k_signatures[index]);
        if (std::all_of(targets.begin(), targets.end(), [](const auto *target) { return target != nullptr; }) &&
            get_module_fingerprint(main_module, fingerprint))
            write_feature_cache(main_module, fingerprint, targets);
    }

    g_hide_uid_find_string = targets[0];
    g_hide_uid_find_object = targets[1];
    g_hide_uid_object_active = targets[2];
    auto *player_perspective = targets[3];
    auto *player_dive_mosaic = targets[4];

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
        // Pin this DLL so it can never be unloaded: PlayerPerspective is
        // permanently patched to jump into a stub inside this module, so a
        // FreeLibrary would make the next call land in unmapped memory.
        HMODULE pinned = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(hModule),
            &pinned);
        // 2026-09-19（审核报告）：**不在 DllMain 内做任何需要 loader lock 的工作**。
        // 原先此处还调用 module_dir(hModule)（GetModuleFileNameW + std::filesystem）
        // 设置日志路径 —— 属官方不建议的 loader lock 内操作。
        // 现在只做 pin + 启动工作线程，模块句柄作为参数传给线程，
        // 路径计算与后续全部初始化都在**loader lock 之外**完成。
        //
        // 说明：在 DllMain 内 CreateThread 本身仍是已知反模式；彻底消除需要
        // 宿主显式调用导出函数触发初始化（本项目为注入式，无此入口）。
        // 这里通过"线程内不做任何依赖 loader lock 的事"把风险降到实际可接受：
        // 线程不会与 DllMain 争用加载器锁，也就不会死锁。
        HANDLE thread = CreateThread(nullptr, 0, worker_thread, hModule, 0, nullptr);
        if (thread)
            CloseHandle(thread);
    }
    return TRUE;
}
