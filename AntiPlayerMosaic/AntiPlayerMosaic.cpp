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
// UID 目标表（2026-09-26 重构：从"三条写死路径 + 永久缓存"改为"多候选 + 每轮现场解析 + 反查验证"）。
//
// **原实现的三个缺陷（实机已复现）**：
//   1. 三条路径写死 ⇒ 游戏改 UI 结构就静默失效（`continue`，不落任何日志）✗
//   2. `find_object` 的结果被**永久缓存** ⇒ `BetaWatermarkCanvas(Clone)` 这类
//      **Clone 会被销毁重建**，缓存指针随即**悬空** ✗；而 `object_active(悬空, false)`
//      **既不抛异常、也不生效** ⇒ 仍 `++hidden_count` ✗
//      ⇒ 日志打出 `hidden 1` 而实际一个都没藏住 ✓（这就是实机看到的假报）
//   3. 只要有一个目标"成功"就把重试间隔拉到 8 秒 ⇒ 假报之后再难纠正 ✗
//
// **现在**：
//   - 每个逻辑目标给**多条候选路径**（带/不带 `(Clone)`、带/不带前导斜杠）✓
//   - **不缓存任何指针**，每轮现场 `find_string` + `find_object` ✓
//     （3 目标 × 3 候选，每 8 秒一次 —— 代价可忽略，却彻底消除悬空指针这一类问题）
//   - **隐藏后立刻用 `find_object` 反查验证** ✓✓✓
//     Unity 的 `GameObject.Find` **只返回激活对象** ⇒ "反查不到"即**确实隐藏了** ✓
//     ⇒ `hidden N` 从此只统计**验证过**的隐藏，不再说谎 ✓
struct UidTarget
{
    const char *label;                  // 日志用的短名
    std::array<const char *, 3> paths;  // 候选路径；空串/nullptr 表示无效项
};

constexpr std::array<UidTarget, 3> k_hide_uid_targets {
    UidTarget { "watermark", { "/BetaWatermarkCanvas(Clone)/Panel/TxtUID",
                               "/BetaWatermarkCanvas/Panel/TxtUID",
                               "BetaWatermarkCanvas(Clone)/Panel/TxtUID" } },
    UidTarget { "profile", { "/Canvas/Pages/PlayerProfilePage/GrpProfile/Right/GrpPlayerCard/UID",
                             "/Canvas/Pages/PlayerProfilePage/GrpProfile/Right/GrpPlayerCard/UID(Clone)",
                             "Canvas/Pages/PlayerProfilePage/GrpProfile/Right/GrpPlayerCard/UID" } },
    UidTarget { "map", { "/Canvas/Pages/InLevelMapPage/GrpMap/GrpPlayer/UID",
                         "/Canvas/Pages/InLevelMapPage/GrpMap/GrpPlayer/UID(Clone)",
                         "Canvas/Pages/InLevelMapPage/GrpMap/GrpPlayer/UID" } },
};

// 目标处理结果（POD；`__try` 内不得出现带析构的对象 ⇒ MSVC C2712）
//
// 注意：**没有 `exception` 这一项** —— 异常时 `__except` 直接返回 -1，不会写任何状态
// （写了也无法确定是哪个目标抛的），所以列进来只会变成不可达分支 ✗。
enum UidStatus : int
{
    k_uid_absent = 0,       // 候选路径全都没找到对象（UI 未出现，或路径已改）
    k_uid_no_string = 1,    // 连字符串对象都没建出来（find_string 失败 ⇒ 另一类问题）✗
    k_uid_hidden = 2,       // **回读**那个被写的字节为 0 ⇒ 写已落地 ✓（字段级判据，非视觉 ✓）
    k_uid_ineffective = 3,  // **回读**仍为 1 ⇒ 写没留住（多半是游戏又把它设回去了）✗
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

// ---------------------------------------------------------------------------
// UID 隐藏的执行机制（2026-09-26 第三次修正 —— 基于反汇编的事实，不再猜）
//
// ## 实机证据（决定性，来自用户日志）
//
//     HideUID target watermark: NOT hidden (SetActive ineffective)
//     HideUID target profile:   NOT hidden (SetActive ineffective)
//
// 而**当时个人资料页根本没打开** ✗ —— Unity 的 `GameObject.Find` 按文档只返回
// **激活**对象，却能在页面未打开时找到该 UID ⇒ **`find_object` 并不按激活状态过滤**
// ⇒ 旧代码"反查不到 = 已隐藏"这条判据**不成立** ✗
// ⇒ 日志里的 `hidden` / `ineffective` **都不可信**（这也解释了旧版为什么假报 `hidden 1`）。
//
// ## 对真实二进制的反汇编结论（本机 YuanShen.exe，sha256 7F89938D…）
//
//     FindString   0x004B2F50 = il2cpp_string_new(const char*)   （strlen + 尾调用）
//     ObjectActive 0x00C53D10 = 解包 [obj+0x10]（IL2CPP Object::m_CachedPtr）
//                               → 尾调用 0x00C62570：
//                                     mov byte ptr [rcx + 0x148], dl
//                                     ret
//
// **⇒ 它只有两条指令，只是【裸写一个字节字段】**，**不是** Unity 的
// `GameObject::SetActive`（后者必须传播子物体、收发 OnEnable/OnDisable）✗
//
// **⇒ 于是只有两种可能**：
//   ① `+0x148` 不是控制可见性的那个字段 ✗
//   ② **游戏每帧把它重新激活** ✓ —— 这与"写调用没报错、对象却始终可见"
//      以及"`Find` 每次都还能找到"**完全吻合** ✓
//
// ## 因此本次改动：**在 IL2CPP 层 patch 那个写入器**（用户的明确要求）
//
// 不再"调一次 SetActive 就完事"，而是**把写入器本身接管**：
//   - 写入器的地址**从 `object_active` 的尾调用里解出来** ✓
//     ⇒ **不需要新签名、不写死 RVA** ⇒ 版本更新后能自动跟随 ✓
//   - 它写的字段偏移**也从它的指令里解出来** ✓（同样不硬编码 ✓）
//   - stub 只做一件事：若 `this` 是我们的 UID 对象 **且** 请求值是 `true`
//     ⇒ **强制改成 `false`**，再执行原指令 ✓
//     ⇒ **游戏再想把它激活也激活不了** ✓
//
// **为什么安全**：原写入器只有 `mov [rcx+disp32], dl` + `ret` 两条指令，
// **不含 RIP 相对寻址** ⇒ 可以原样搬进 stub，无需重定位 ✓；
// 若实测发现形态不符（指令更长 / 含 RIP 相对）⇒ **放弃并如实记日志**，不硬来 ✓
//
// ⚠️ 本文件不做任何"凭猜测写字段"的事：只碰 `object_active` **自己会写**的那个偏移 ✓。
// ---------------------------------------------------------------------------

// UID 目标的【解包后】指针（写 stub 比较用；主线程写、被 patch 的代码读）
std::array<std::uintptr_t, k_hide_uid_targets.size()> g_uid_filter_targets {};
// `object_active` 实际写入的字节偏移（从指令里解出 —— 不是硬编码 ✓）
std::uint32_t g_hide_uid_active_offset = 0;
// 原写入器地址（stub 装好前的备份，供诊断）
std::uint8_t *g_hide_uid_active_writer = nullptr;
// 过滤 stub 与命中统计（统计用于**判定这条 patch 是否真的被游戏用到** ✓）
void *g_hide_uid_filter_stub = nullptr;
std::atomic<std::uint64_t> g_hide_uid_filter_calls { 0 };
std::atomic<std::uint64_t> g_hide_uid_filter_forced { 0 };

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
            status_out[i] = k_uid_absent;
            const UidTarget &target = k_hide_uid_targets[i];
            bool built_string = false; // 是否至少建成过一次字符串对象（区分两类失败）
            std::uintptr_t native_found = 0;

            // 逐个候选现场解析：**不缓存**任何指针 ⇒ 无悬空指针问题 ✓
            for (const char *candidate : target.paths)
            {
                if (candidate == nullptr || *candidate == '\0')
                    continue;

                void *string_object = find_string(candidate);
                if (string_object == nullptr)
                    continue;
                built_string = true;

                void *object = find_object(string_object);
                if (object == nullptr)
                    continue;

                // IL2CPP 解包：`[obj + 0x10]` 就是 Object::m_CachedPtr ——
                // 与 `object_active` **内部做的事完全一致**（见文件上方反汇编说明）✓
                // ⇒ 拿到"被 patch 的写入器实际会写的那个对象" ✓
                const auto native = *reinterpret_cast<std::uintptr_t *>(
                    reinterpret_cast<std::uint8_t *>(object) + 0x10);

                object_active(object, false);

                // ⚠️ 判据换成【回读我们实际写的那个字节】✓
                //
                // 旧判据是"再 Find 一次看还在不在"，但**实机已证伪**：
                // 个人资料页根本没打开时它照样 "找到" 了该 UID ⇒ `find_object`
                // 并不按激活状态过滤 ⇒ 那个判据既会假报成功、也会假报失败 ✗
                if (native != 0 && g_hide_uid_active_offset != 0)
                {
                    const auto value = *reinterpret_cast<volatile std::uint8_t *>(
                        native + g_hide_uid_active_offset);
                    status_out[i] = value == 0 ? k_uid_hidden : k_uid_ineffective;
                    if (value == 0)
                        ++hidden_count;
                    native_found = native;
                }
                else
                {
                    status_out[i] = k_uid_ineffective;
                }
                break; // 该目标已处理，试下一个目标
            }

            // 登记给过滤 stub：游戏之后任何"重新激活"都会被强制成 false ✓
            // 找不到时清 0 ⇒ **不会有悬空指针被 stub 命中** ✓
            g_uid_filter_targets[i] = native_found;

            // 字符串都没建成 ⇒ 与"路径找不到对象"是**不同**的故障，如实区分 ✓
            if (status_out[i] == k_uid_absent && !built_string)
                status_out[i] = k_uid_no_string;
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
        const char *word = (status[i] == k_uid_hidden) ? "active flag reads 0 (write landed)"
            : (status[i] == k_uid_ineffective) ? "active flag still reads 1 (write did not stick)"
            : "find_string failed (cannot build path string)";
        log_line(std::string("HideUID target ") + k_hide_uid_targets[i].label + ": " + word);
    }

    if (hidden_count > 0)
    {
        if (!g_hide_uid_logged_success.exchange(true))
            log_line("HideUID active: " + std::to_string(hidden_count) +
                " ui targets with active flag cleared (forced-reactivation=" +
                std::to_string(g_hide_uid_filter_forced.load(std::memory_order_relaxed)) + ")");
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

// 长度可指定的 rel32 跳转（2026-09-26）。
//
// 用于替换**短函数的第一条指令**：`mov [rcx+disp32], dl` 是 **6 字节**，
// 而 5 字节的 `E9 rel32` 只会覆盖它的前 5 字节、把第 6 个字节留在原地 ✗
// ⇒ 被替换处会残留一个字节，反汇编错位。
// 这里按 `size` 写 `E9 rel32`，其余用 `0x90`(nop) 填满 ✓
// （`size` 必须 ≥ 5；= 5 时等价于 patch_rel32_jump）。
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

// ---------------------------------------------------------------------------
// UID 活动状态过滤（IL2CPP 层 patch）—— 实现见文件上方那段说明
// ---------------------------------------------------------------------------

// 安全读可执行内存（SEH 保护：地址可能落在未提交页）
bool safe_read_code(const std::uint8_t *address, std::uint8_t *out, std::size_t size)
{
    __try
    {
        for (std::size_t i = 0; i < size; ++i)
            out[i] = address[i];
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return true;
}

// 在缓冲里找 1 字节写入 `88 <modrm=10xxx101> <disp32>`（即 `mov [reg+disp32], r8`）。
// 这类指令**没有 RIP 相对寻址**，因此可以原样搬进 stub，无需重定位 ✓
bool find_byte_write(const std::uint8_t *code, std::size_t size, std::uint32_t *offset_out,
    std::size_t *at_out)
{
    for (std::size_t i = 0; i + 6 <= size; ++i)
    {
        if (code[i] != 0x88)
            continue;
        const std::uint8_t modrm = code[i + 1];
        if ((modrm & 0xC0) != 0x80 || (modrm & 0x07) != 0x05)
            continue; // 要求 [reg + disp32] 形态
        std::uint32_t displacement = 0;
        std::memcpy(&displacement, code + i + 2, sizeof(displacement));
        if (displacement == 0 || displacement > 0x2000)
            continue; // 合理性上界：IL2CPP 对象字段不会到 8 KB
        if (offset_out != nullptr)
            *offset_out = displacement;
        if (at_out != nullptr)
            *at_out = i;
        return true;
    }
    return false;
}

// 解出"这个 setter 最终把 this 写到哪个字节偏移"，并把写入者地址一并给出。
//   形态 A：函数体内直接写
//   形态 B：尾调用（E9 rel32）到一个只做写入的小函数（本机 ObjectActive 即此形态 ✓）
bool resolve_written_field(std::uint8_t *function, std::uint32_t *offset_out, std::uint8_t **writer_out)
{
    std::uint8_t head[64] {};
    if (!safe_read_code(function, head, sizeof(head)))
        return false;

    if (find_byte_write(head, sizeof(head), offset_out, nullptr))
    {
        if (writer_out != nullptr)
            *writer_out = function;
        return true;
    }

    // 尾调用：只在函数头 48 字节内找，避免误认函数体深处的其它 E9
    for (std::size_t i = 0; i + 5 <= 48; ++i)
    {
        if (head[i] != 0xE9)
            continue;
        std::int32_t relative = 0;
        std::memcpy(&relative, head + i + 1, sizeof(relative));
        auto *target = function + i + 5 + relative;
        std::uint8_t tail[32] {};
        if (!safe_read_code(target, tail, sizeof(tail)))
            continue;
        if (find_byte_write(tail, sizeof(tail), offset_out, nullptr))
        {
            if (writer_out != nullptr)
                *writer_out = target;
            return true;
        }
    }
    return false;
}

// 构造过滤 stub：
//     mov  rax, imm64(&g_uid_filter_targets)
//     [每个槽位]  mov r9, [rax+8i] / cmp rcx, r9 / je FORCE
//     jmp  WRITE
//   FORCE:
//     xor  edx, edx                  ; 请求值是 true ⇒ 强制成 false
//     mov  rax, imm64(&forcedCounter)
//     lock inc qword ptr [rax]       ; 统计"游戏试图重新激活"的次数（诊断关键 ✓）
//   WRITE:
//     <原写入指令，6 字节原样复制>     ; 不含 RIP 相对 ⇒ 可安全搬运
//     ret
//
// 语义：只有 `this` 命中我们的 UID 对象、且请求为 true 时才改写；
// 其余情况**完全等同原函数** ✓（不会影响游戏里任何其它对象）
void *build_active_filter_stub(std::uint8_t *writer, std::size_t write_at)
{
    if (g_hide_uid_filter_stub != nullptr)
        return g_hide_uid_filter_stub;

    std::uint8_t original[8] {};
    if (!safe_read_code(writer + write_at, original, sizeof(original)))
        return nullptr;
    if (original[6] != 0xC3)
    {
        // 形态不符（写入指令后面不是 ret）⇒ 不硬来 ✓
        log_line("HideUID active-filter: writer shape unexpected (no ret after write); skipped");
        return nullptr;
    }

    auto *stub = static_cast<std::uint8_t *>(allocate_near_address(writer, 0x1000));
    if (stub == nullptr)
        return nullptr;

    std::size_t p = 0;
    const auto targets = reinterpret_cast<std::uintptr_t>(&g_uid_filter_targets[0]);
    stub[p++] = 0x48; // mov rax, imm64
    stub[p++] = 0xB8;
    std::memcpy(stub + p, &targets, sizeof(targets));
    p += sizeof(targets);

    std::array<std::size_t, k_hide_uid_targets.size()> jump_positions {};
    for (std::size_t i = 0; i < k_hide_uid_targets.size(); ++i)
    {
        stub[p++] = 0x4C; stub[p++] = 0x8B; stub[p++] = 0x48;
        stub[p++] = static_cast<std::uint8_t>(i * 8);      // mov r9, [rax + 8i]
        stub[p++] = 0x4C; stub[p++] = 0x39; stub[p++] = 0xC9; // cmp rcx, r9
        stub[p++] = 0x74;                                   // je FORCE
        jump_positions[i] = p++;
    }
    const std::size_t jmp_write_at = p;
    stub[p++] = 0xEB;                                       // jmp WRITE
    const std::size_t jmp_write_disp = p++;

    const std::size_t force_at = p;
    stub[p++] = 0x31; stub[p++] = 0xD2;                     // xor edx, edx
    const auto counter = reinterpret_cast<std::uintptr_t>(&g_hide_uid_filter_forced);
    stub[p++] = 0x48; stub[p++] = 0xB8;                     // mov rax, imm64
    std::memcpy(stub + p, &counter, sizeof(counter));
    p += sizeof(counter);
    stub[p++] = 0xF0; stub[p++] = 0x48; stub[p++] = 0xFF; stub[p++] = 0x00; // lock inc [rax]

    const std::size_t write_in_stub = p;
    std::memcpy(stub + p, original, 6);                     // 原写入指令
    p += 6;
    stub[p++] = 0xC3;                                       // ret

    if (p > 0x1000)
        return nullptr;
    for (const std::size_t position : jump_positions)
        stub[position] = static_cast<std::uint8_t>(force_at - (position + 1));
    stub[jmp_write_disp] = static_cast<std::uint8_t>(write_in_stub - (jmp_write_at + 2));

    FlushInstructionCache(GetCurrentProcess(), stub, p);
    g_hide_uid_filter_stub = stub;
    return stub;
}

// 安装过滤：解出写入者 → 造 stub → 用 6 字节跳转替换原写入指令
bool install_active_filter_hook(std::uint8_t *object_active)
{
    std::uint32_t offset = 0;
    std::uint8_t *writer = nullptr;
    if (!resolve_written_field(object_active, &offset, &writer) || writer == nullptr)
    {
        log_line("HideUID active-filter: cannot resolve written field; skipped");
        return false;
    }
    g_hide_uid_active_offset = offset;
    g_hide_uid_active_writer = writer;

    std::uint8_t body[64] {};
    if (!safe_read_code(writer, body, sizeof(body)))
        return false;
    std::size_t write_at = 0;
    std::uint32_t check = 0;
    if (!find_byte_write(body, sizeof(body), &check, &write_at))
        return false;

    void *stub = build_active_filter_stub(writer, write_at);
    if (stub == nullptr)
        return false;

    if (!patch_rel32_jump_n(writer + write_at, reinterpret_cast<std::uintptr_t>(stub), 6))
    {
        log_line("HideUID active-filter: patch failed");
        return false;
    }

    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    char message[192] {};
    std::snprintf(message, sizeof(message),
        "HideUID active-filter installed: writer=+0x%llX field=+0x%X (forces active=false for uid targets)",
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(writer) - base),
        static_cast<unsigned>(offset));
    log_line(message);
    return true;
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

    // UID 活动标志过滤（IL2CPP 层 patch）—— 见文件上方那段说明。
    //
    // 若游戏每帧把 UID 对象重新激活，那么"每 1.2 秒写一次 false"永远赢不了 ✓
    // ⇒ 必须把 `object_active` **真正写入的那个函数**接管，才能压住它 ✓
    // 装不上也不影响其余功能（仍会每轮直接写一次字段 ✓），如实记日志即可 ✓
    if (g_hide_uid_object_active != nullptr)
    {
        if (!install_active_filter_hook(g_hide_uid_object_active))
            log_line("HideUID active-filter unavailable; falling back to direct field write");
    }

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
