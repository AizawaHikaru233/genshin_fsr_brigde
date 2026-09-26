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

// 日志级别：Release 下 INFO 常开 —— 这些是一次性、可验证的生命周期事实，
// 丢任何一条都无法从日志判断插件是否生效。DEBUG 仅非 release 构建输出。
//
// 级别与 `log_line` 的声明必须在**匿名命名空间之前**：命名空间内还需要它们
//（`read_cached_signature` / `scan_unique_signature` 要记解析失败），
// 而在命名空间内定义会变成另一个函数（内部链接）⇒ LNK2019。
enum class LogLevel
{
    Info,
    Debug
};

// `log_line` 的**定义**必须在匿名命名空间**之外**（否则命名空间内的同名函数是
// 另一个函数 —— 内部链接，声明与定义不一致 ⇒ LNK2019）。它依赖的两个全局一并放这里。
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
// "主模块已解析"门控。
//
// 门控**必须保留**（不是死代码）：`hide_uid_from_main_thread` 是插进
// `PlayerPerspective` 的 stub 回调，可能在签名解析完成前就被游戏主线程调用；
// 而 `hide_uid_once()` 在所需签名未解析时会 `g_hide_uid_enabled.store(false)`
// **永久禁用** HideUID ⇒ "未就绪时直接返回"是必要的保护。
//
// 跨线程（worker 写、游戏主线程读）⇒ 必须用 `atomic_bool`。
std::atomic_bool g_main_module_resolved { false };
void *g_player_perspective_stub = nullptr;
std::uint8_t *g_hide_uid_find_string = nullptr;
std::uint8_t *g_hide_uid_find_object = nullptr;
std::uint8_t *g_hide_uid_object_active = nullptr;
// UID 目标表：每个逻辑目标给**多条候选路径**（带/不带 `(Clone)`、带/不带前导斜杠）。
//
// 三条约束都是实机踩出来的，勿简化：
//   * **不缓存任何指针** —— `BetaWatermarkCanvas(Clone)` 这类 Clone 会被销毁重建，
//     缓存下来的指针随即悬空；而对悬空指针调隐藏函数**既不抛异常也不生效**，
//     却会让人以为"藏住了" ⇒ 必须每轮现场 `find_string` + `find_object`。
//   * **隐藏后立刻反查验证** —— Unity 的 `GameObject.Find` 只返回激活对象，
//     所以"反查不到"即确实藏住了 ⇒ `hidden N` 只统计验证过的隐藏，不说谎。
//   * **只有确有隐藏才降频重试** —— 否则一次误报之后长时间不再纠正。
constexpr std::size_t k_uid_paths_per_target = 5;

struct UidTarget
{
    const char *label;                                  // 日志用的短名
    std::array<const char *, k_uid_paths_per_target> paths; // 候选；nullptr/空串=无效项
};

constexpr std::array<UidTarget, 3> k_hide_uid_targets {
    // 水印额外给**整块画布**候选：`BetaWatermarkCanvas` 是专供水印的画布，
    // 连画布一起隐藏不会误伤别的 UI。
    // **个人资料页 / 地图页绝不能这么做** —— 藏它们的祖先把整页都藏掉了。
    UidTarget { "watermark", { "/BetaWatermarkCanvas(Clone)/Panel/TxtUID",
                               "/BetaWatermarkCanvas(Clone)",
                               "/BetaWatermarkCanvas/Panel/TxtUID",
                               "/BetaWatermarkCanvas",
                               "BetaWatermarkCanvas(Clone)/Panel/TxtUID" } },
    UidTarget { "profile", { "/Canvas/Pages/PlayerProfilePage/GrpProfile/Right/GrpPlayerCard/UID",
                             "/Canvas/Pages/PlayerProfilePage/GrpProfile/Right/GrpPlayerCard/UID(Clone)",
                             "Canvas/Pages/PlayerProfilePage/GrpProfile/Right/GrpPlayerCard/UID",
                             nullptr, nullptr } },
    UidTarget { "map", { "/Canvas/Pages/InLevelMapPage/GrpMap/GrpPlayer/UID",
                         "/Canvas/Pages/InLevelMapPage/GrpMap/GrpPlayer/UID(Clone)",
                         "Canvas/Pages/InLevelMapPage/GrpMap/GrpPlayer/UID",
                         nullptr, nullptr } },
};

// 目标处理结果（POD；`__try` 内不得出现带析构的对象 ⇒ MSVC C2712）。
// 不含"异常"项：异常时 `__except` 直接返回 -1，且无法确定是哪个目标抛的。
enum UidStatus : int
{
    k_uid_absent = 0,       // 候选路径全都没找到对象（UI 未出现，或路径已改）
    k_uid_no_string = 1,    // 连字符串对象都没建出来（find_string 失败 —— 与"找不到对象"是不同故障）
    k_uid_hidden = 2,       // 隐藏后同一路径**反查不到** ⇒ 确实藏住了
    k_uid_ineffective = 3,  // 隐藏后**仍能反查到** ⇒ 对象仍是激活的，隐藏没落地
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
// 每个目标只报一次"状态已确定"（位掩码；避免 std::array<atomic_bool> 的初始化麻烦）
std::atomic<unsigned> g_hide_uid_reported_mask { 0 };
std::atomic_int g_hide_uid_exception_streak { 0 };
// 上一轮每个目标的状态（仅用于诊断输出；由主线程独占写）
std::array<int, k_hide_uid_targets.size()> g_hide_uid_last_status {};

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

// 本函数**已经**做了等价于指纹校验的边界与内容检查（RVA 必须落在已提交的可执行页内，
// 且该处机器码仍匹配签名），故无需再传指纹参数 —— 那只会重复同一件事。
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
    // 模式解析不完整 → 显式失败（而非静默截断）。缓存路径也走这里：若签名被改坏，
    // 缓存命中也必须拒绝，否则会把"用错误模式算出的 RVA"当成有效缓存。
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


// ⚠️ **不要再尝试 patch `object_active`** —— 曾装过一个"活动标志过滤 stub"接管其内部
// 写入器，实机证明**有害**：stub 连本模块自己的 `object_active(..., false)` 调用也一并吃掉，
// 一旦 stub 有偏差就让 `SetActive` 对所有人失效。参照实现（FufuLauncher.UnlockerIsland 的
// `HideUI.cpp`）**不 hook、不 patch 任何函数**，只是定期重贴一次 `SetActive(false)` —— 足够。

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

int hide_uid_once_unsafe(int *status_out)
{
    const auto find_string = reinterpret_cast<find_string_fn>(g_hide_uid_find_string);
    const auto find_object = reinterpret_cast<find_object_fn>(g_hide_uid_find_object);
    const auto object_active = reinterpret_cast<object_active_fn>(g_hide_uid_object_active);
    int hidden_count = 0;

    __try
    {
        for (std::size_t i = 0; i < k_hide_uid_targets.size(); ++i)
        {
            const UidTarget &target = k_hide_uid_targets[i];

            bool built_string = false; // 是否至少建成过一次字符串对象（区分两类失败）
            bool any_native = false;   // 至少解析到一个对象 ✓
            bool any_stuck = false;    // 至少一个候选的写入**落地**了 ✓

            // 逐个候选现场解析（**不缓存**任何指针 ⇒ 无悬空指针），且**不提前 break**：
            // 每个能解析到的候选都藏一遍 —— 否则"藏 TxtUID 不生效"时，整块画布那条
            // 候选永远没机会试。（profile/map 刻意没有祖先候选，见目标表。）
            for (std::size_t p = 0; p < target.paths.size(); ++p)
            {
                const char *candidate = target.paths[p];
                if (candidate == nullptr || *candidate == '\0')
                    continue;

                void *string_object = find_string(candidate);
                if (string_object == nullptr)
                    continue;
                built_string = true;

                void *object = find_object(string_object);
                if (object == nullptr)
                    continue;

                // 解包 `Object::m_CachedPtr`（`[obj + 0x10]`，与 `object_active` 内部
                // 做的事一致），仅用于判定"确实拿到了真实对象"。
                const auto native = *reinterpret_cast<std::uintptr_t *>(
                    reinterpret_cast<std::uint8_t *>(object) + 0x10);
                if (native == 0)
                    continue;
                any_native = true;

                object_active(object, false);

                // 判据：隐藏后**再 Find 一次**。`GameObject.Find` 只返回激活对象，
                // 所以同一路径找不到即确实藏住了（参照实现亦以此为依据）。
                if (find_object(string_object) == nullptr)
                    any_stuck = true;
            }

            // 状态：**任一候选写落地即算"藏住"** ✓（计数仍是"目标数" ✓，与日志语义一致）
            if (any_stuck)
            {
                status_out[i] = k_uid_hidden;
                ++hidden_count;
            }
            else if (any_native)
                status_out[i] = k_uid_ineffective;
            else if (built_string)
                status_out[i] = k_uid_absent;
            else
                status_out[i] = k_uid_no_string; // 与"找不到对象"是不同故障 ✓
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // 具体哪个目标抛的无法在此判断（__try 内不能有带析构的对象），
        // 调用方据 status_out 中仍为 k_uid_absent 的项自行判断。
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

    std::array<int, k_hide_uid_targets.size()> status {};
    const int hidden_count = hide_uid_once_unsafe(status.data());
    if (hidden_count < 0)
    {
        const int streak = g_hide_uid_exception_streak.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!g_hide_uid_logged_cache_reset.exchange(true))
            log_line("HideUID exception while hiding ui targets (streak=" + std::to_string(streak) + ")");

        if (streak >= 3)
        {
            if (!g_hide_uid_logged_failure.exchange(true))
                log_line("HideUID disabled: repeated exceptions while hiding ui targets");
            g_hide_uid_enabled.store(false);
        }
        return false;
    }

    g_hide_uid_exception_streak.store(0, std::memory_order_relaxed);
    g_hide_uid_logged_cache_reset.store(false);

    // ---- 逐目标诊断（2026-09-26 新增）----
    //
    // 用户在实机上**只会跑一次、只给一份日志** ⇒ 这一份必须能定位问题 ✗
    // 每个目标在**状态首次确定**、以及**状态变化**时各记一行 ✓（不刷屏 ✓）
    bool any_ineffective = false;
    for (std::size_t i = 0; i < status.size(); ++i)
    {
        if (status[i] == k_uid_ineffective)
            any_ineffective = true;
        if (status[i] == g_hide_uid_last_status[i])
            continue;
        g_hide_uid_last_status[i] = status[i];
        // `absent` 是常态（个人资料页/地图页没打开时本来就找不到）⇒ 不记，避免噪音
        if (status[i] == k_uid_absent)
            continue;
        // 三种可达状态各自如实措辞（`absent` 已在上面 continue 掉）
        const char *word = (status[i] == k_uid_hidden)
            ? "hidden (re-find returns null)"
            : (status[i] == k_uid_ineffective)
                ? "NOT hidden (still findable after SetActive false)"
                : "find_string failed (cannot build path string)";
        log_line(std::string("HideUID target ") + k_hide_uid_targets[i].label + ": " + word);
    }

    if (hidden_count > 0)
    {
        if (!g_hide_uid_logged_success.exchange(true))
            log_line("HideUID active: " + std::to_string(hidden_count) +
                " ui targets hidden (verified by re-find)");
        // ⚠️ 只有"确实藏住了、且没有任何目标调用无效"时才降到低频；
        // 否则保持高频重试，避免像旧版那样**假报成功后长期不再纠正** ✗
        return !any_ineffective;
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

// 长度可指定的 rel32 跳转。用于替换**短函数的第一条指令**：被替换的指令可能长于
// 5 字节（`E9 rel32` 只覆盖前 5 字节），残留的尾部字节会让反汇编错位 ⇒ 这里按
// `size` 写 `E9 rel32`、其余用 `0x90`(nop) 填满。`size` 必须 ≥ 5。
bool patch_rel32_jump_n(std::uint8_t *address, std::uintptr_t absolute_target, std::size_t size)
{
    if (size < 5)
        return false;
    const auto next = reinterpret_cast<std::uintptr_t>(address + 5);
    const auto diff = static_cast<std::intptr_t>(absolute_target) - static_cast<std::intptr_t>(next);
    if (diff < INT32_MIN || diff > INT32_MAX)
        return false;

    std::array<std::uint8_t, 32> patch {};
    if (size > patch.size())
        return false;
    patch.fill(0x90);
    patch[0] = 0xE9;
    const auto displacement = static_cast<std::int32_t>(diff);
    std::memcpy(patch.data() + 1, &displacement, sizeof(displacement));
    return patch_bytes(address, patch.data(), size);
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
    // 模式解析不完整 → 显式失败（而非静默截断）。静默截断会产出"部分模式"，
    // 只表现为"签名未找到"，无法区分是游戏版本变了还是签名文本被改坏。
    if (!pattern.valid)
    {
        log_line(std::string(signature.name) + " pattern parse failed at offset=" +
            std::to_string(pattern.error_offset) + " (signature text malformed)");
        return nullptr;
    }
    const auto *sections = IMAGE_FIRST_SECTION(nt);
    std::array<std::uint8_t *, 2> matches {};
    std::size_t match_count = 0;
    // `total_matches` 与 `match_count` 是两个量：前者是**真实**匹配数，后者受
    // `matches` 容量（2）封顶。只报后者会把"50 处匹配"写成 "matches=2"，诊断时失真。
    // 容量 2 是有意的 —— 只需区分 0 / 1 / ≥2（1 才可用），提前停止可省扫描成本。
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

// 包装器模式里那个 `E9 rel32`（尾调用）的位置。`E9` 在模式里只出现一次，故无歧义；
// 找不到就返回 npos，由调用方显式失败。
std::size_t tail_jump_offset(const pattern_scanner::Pattern &pattern)
{
    for (std::size_t i = 0; i < pattern.bytes.size(); ++i)
    {
        if (!pattern.bytes[i].wildcard && pattern.bytes[i].value == 0xE9)
            return i;
    }
    return static_cast<std::size_t>(-1);
}

// `ObjectActive` 专用解析：它的包装器模式有 **45 个逐字节同形的孪生**
//（差别只在随调用者地址派生出的 `E8`/`E9` 位移上）⇒ 包装器层面无法唯一识别。
//
// 做法（全程内容特征，**不写死任何地址**）：
//   ① 用 `signature.discriminator` 唯一命中原生实现，即真正的 `GameObject::SetActive`；
//   ② 扫出全部包装器候选；
//   ③ 取**尾调用它**的那一个（包装器末尾是 `E9 rel32`）。
//
// 任何一步命中数不是 1 就返回 nullptr 并记日志 —— **失败即放弃，绝不猜**：
// 猜错会去调用另一个函数的孪生，症状是"调了、不报错、什么也不发生"，极难归因。
std::uint8_t *resolve_object_active(
    std::uint8_t *base,
    std::size_t image_size,
    const pattern_scanner::Signature &signature)
{
    const std::string name(signature.name);
    const auto wrapper_pattern = pattern_scanner::parse_pattern(signature.text);
    const auto impl_pattern = pattern_scanner::parse_pattern(signature.discriminator);
    if (!wrapper_pattern.valid || !impl_pattern.valid)
    {
        log_line(name + " pattern parse failed (signature text malformed)");
        return nullptr;
    }

    const std::size_t jump_offset = tail_jump_offset(wrapper_pattern);
    if (jump_offset == static_cast<std::size_t>(-1))
    {
        log_line(name + " wrapper pattern has no tail E9; cannot disambiguate");
        return nullptr;
    }

    if (image_size < sizeof(IMAGE_DOS_HEADER))
        return nullptr;
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > image_size)
    {
        log_line(name + " image is not a mapped PE image (bad DOS header)");
        return nullptr;
    }
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    {
        log_line(name + " image is not a mapped PE image (bad NT header)");
        return nullptr;
    }

    std::vector<std::uint8_t *> impl_hits;
    std::vector<std::uint8_t *> wrappers;
    const auto *sections = IMAGE_FIRST_SECTION(nt);
    // 必须遍历**全部可执行节**：本 EXE 有三个（.text / il2cpp / .upx0），漏掉 il2cpp
    // 会得出"0 命中、游戏改了代码"的反向结论。
    for (unsigned section_index = 0; section_index < nt->FileHeader.NumberOfSections; ++section_index)
    {
        const auto &section = sections[section_index];
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 || section.VirtualAddress >= image_size)
            continue;

        std::size_t section_size = section.Misc.VirtualSize != 0 ? section.Misc.VirtualSize : section.SizeOfRawData;
        section_size = (std::min)(section_size, image_size - section.VirtualAddress);
        auto *section_base = base + section.VirtualAddress;

        for (const auto offset : pattern_scanner::find_matches(section_base, section_size, impl_pattern))
            impl_hits.push_back(section_base + offset);
        for (const auto offset : pattern_scanner::find_matches(section_base, section_size, wrapper_pattern))
            wrappers.push_back(section_base + offset);
    }

    if (impl_hits.size() != 1)
    {
        log_line(name + " impl scan failed: matches=" + std::to_string(impl_hits.size()));
        return nullptr;
    }
    std::uint8_t *const impl = impl_hits.front();

    std::uint8_t *picked = nullptr;
    std::size_t picked_count = 0;
    for (auto *wrapper : wrappers)
    {
        if (wrapper + jump_offset + 5 > base + image_size)
            continue;
        std::int32_t relative = 0;
        std::memcpy(&relative, wrapper + jump_offset + 1, sizeof(relative));
        if (wrapper + jump_offset + 5 + relative == impl)
        {
            picked = wrapper;
            ++picked_count;
        }
    }
    if (picked_count != 1)
    {
        log_line(name + " disambiguation failed: wrappers=" + std::to_string(wrappers.size()) +
            " tail-matching-impl=" + std::to_string(picked_count));
        return nullptr;
    }

    char message[192] {};
    std::snprintf(message, sizeof(message),
        "%.*s resolved: rva=0x%08llX (impl=0x%08llX, %llu twins, by tail call)",
        static_cast<int>(signature.name.size()), signature.name.data(),
        static_cast<unsigned long long>(picked - base),
        static_cast<unsigned long long>(impl - base),
        static_cast<unsigned long long>(wrappers.size()));
    log_line(message);
    return picked;
}

// 有界等待主模块代码段可读（替代固定 `Sleep(3000)`）。
//
// 判定依据：主模块映像的**首个页面**可读且已提交。就绪即继续（不白等），未就绪则
// 轮询到上限（不过早扫描导致静默失效）。返回实际等待毫秒数，供日志诊断"启动慢"。
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
    // 日志路径在**本线程**计算：DllMain 只做 pin + CreateThread，模块句柄经参数传入，
    // 以免在持有 loader lock 期间做 `GetModuleFileNameW` / `std::filesystem` 构造。
    const auto self_module = static_cast<HMODULE>(parameter);
    if (self_module != nullptr)
        g_log_path = module_dir(self_module) / "AntiPlayerMosaic.log";

    reset_release_log();
    log_line("AntiPlayerMosaic loaded (dynamic scan)");

    // 有界轮询代替固定等待：游戏启动慢时过早扫描会导致签名找不到（功能静默失效），
    // 启动快时固定等待又是白等。就绪即继续，最长等 `k_init_wait_timeout_ms`。
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

    // ① **带判据的函数优先解析**：`ObjectActive` 的包装器模式有 45 个逐字节同形的孪生，
    //    只能靠 `discriminator`（原生实现的特征模式）加尾调用关系**动态**选定。
    //
    // ⚠️ **必须排在缓存之前**：旧缓存里可能存着**错误孪生**的 RVA，而它同样能通过
    //    包装器模式校验 ⇒ 缓存那道校验挡不住它，不能让它抢先。
    for (std::size_t index = 0; index < targets.size(); ++index)
    {
        const auto &signature = pattern_scanner::k_signatures[index];
        if (signature.discriminator.empty())
            continue;
        targets[index] = resolve_object_active(base, size, signature);
    }

    bool cache_valid = get_module_fingerprint(main_module, fingerprint) &&
        read_feature_cache(main_module, fingerprint, cached_rvas);
    if (cache_valid)
    {
        for (std::size_t index = 0; index < targets.size(); ++index)
        {
            if (targets[index] != nullptr)
                continue; // 已由首选 RVA 解出
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
        {
            if (targets[index] != nullptr)
                continue; // 已由首选 RVA 解出
            targets[index] = scan_unique_signature(base, size, pattern_scanner::k_signatures[index]);
        }
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

    // 让 `object_active` 保持原样：**不要 patch 它** —— 原因见文件上方那段说明。

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
        // **不在 DllMain 内做任何需要 loader lock 的工作**：日志路径的计算与后续
        // 全部初始化都放到工作线程里做。在 DllMain 内 CreateThread 本身仍是已知
        // 反模式，但线程内不碰加载器锁，也就不会与 DllMain 争锁而死锁。
        HANDLE thread = CreateThread(nullptr, 0, worker_thread, hModule, 0, nullptr);
        if (thread)
            CloseHandle(thread);
    }
    return TRUE;
}
