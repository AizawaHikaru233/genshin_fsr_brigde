// ============================================================================
// BridgeLogger 实现。设计说明见 BridgeLogger.h 顶部注释。
// ============================================================================

#include "BridgeLogger.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace blog
{
namespace
{

// ---- 等级/分类状态 --------------------------------------------------------

std::atomic<bool> g_active { false };
std::atomic<bool> g_alive { true };   // writer 线程存活标志（detach 时置 false，不 join）
std::atomic<std::uint8_t> g_level { static_cast<std::uint8_t>(Level::Info) };

std::mutex g_cat_mutex;
std::unordered_map<std::string, Level> g_cat_levels;   // 分类覆盖
std::atomic<bool> g_has_cat_overrides { false };       // 热路径快速判定（有覆盖才取锁）

// 写入端的固定容量环形队列：满了丢最旧，保证调用线程永不阻塞。
constexpr std::size_t k_max_queue = 16384;
std::array<std::string, k_max_queue> g_queue;
std::size_t g_head = 0;   // 读
std::size_t g_tail = 0;   // 写
std::size_t g_size = 0;
std::atomic<std::uint64_t> g_dropped { 0 };

std::mutex g_queue_mutex;
std::condition_variable g_queue_cv;

// ---- sink 状态 ------------------------------------------------------------

std::wstring g_directory;
std::wstring g_filename;
std::wstring g_path;
std::ofstream g_file;
std::atomic<std::uint64_t> g_max_file_bytes { 0 };
std::atomic<std::uint32_t> g_rotate_keep { 2 };
std::uint64_t g_bytes_written = 0;

std::atomic<bool> g_to_debugger { false };
std::atomic<std::uint32_t> g_debugger_max_per_sec { 200 };
std::atomic<std::uint32_t> g_debugger_in_window { 0 };
std::atomic<std::uint64_t> g_debugger_window_start { 0 };

std::thread g_writer;
std::mutex g_file_mutex;

// 兼容模式：旧 log_line 行按前缀归类（过渡开关）
std::atomic<bool> g_compat_prefix { false };

std::uint64_t steady_ms()
{
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// ---------------------------------------------------------------------------
// ⚠️ 路径编码陷阱（2026-09-18 实测，代价是一轮"完全没有日志"的排查）
//
// 注入目录常含中文（Starward 路线为 `...\原神解帧FSR插件包\payload\Bridge`）。
// 若把该目录按 CP_UTF8 转成窄串再交给 std::ofstream：
//     窄串字节 = UTF-8                （narrow() 的行为）
//     ofstream 按系统 ACP 解释该字节   （中文 Windows = 936/GBK）
//   → 路径变成乱码 → **open 失败且不抛异常、不报错**
//   → 表现为"日志器明明在跑，却一个文件都没有"。
//
// 实测对照（同一个含中文的目录）：
//     narrow(CP_UTF8) -> ofstream  open=0
//     wchar_t*        -> ofstream  open=1   ← 本文件采用
//
// 旧代码没踩这个坑，是因为它用 `std::ofstream out(std::filesystem::path)`：
// 在 Windows 上 filesystem::path 会走宽字符重载。改成窄 string 时才引入回归。
// 因此：**凡涉及文件路径，一律走宽字符 API（ofstream(wchar_t*)、MoveFileExW、
// GetFileAttributesExW）**，不要经过 narrow()。
// ---------------------------------------------------------------------------

void open_file_locked(bool truncate)
{
    if (g_path.empty())
        return;
    g_file.close();
    // 必须走宽字符路径重载：窄串会被 ofstream 按系统 ACP 解释，
    // 含中文的注入目录会因此打开失败（且不抛异常，只能靠下面的 log_open_failed 暴露）。
    g_file.open(g_path.c_str(), truncate ? (std::ios::out | std::ios::trunc)
                                         : (std::ios::out | std::ios::app));
    if (!g_file.is_open())
        return;
    // 定位当前大小（append 模式下 tellp 可能为 0，用文件系统实测）。
    WIN32_FILE_ATTRIBUTE_DATA attr {};
    g_bytes_written = 0;
    if (GetFileAttributesExW(g_path.c_str(), GetFileExInfoStandard, &attr))
        g_bytes_written = (static_cast<std::uint64_t>(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;
}

void rotate_locked()
{
    const std::uint64_t limit = g_max_file_bytes.load(std::memory_order_relaxed);
    if (limit == 0 || g_bytes_written < limit)
        return;
    g_file.close();
    const std::uint32_t keep = g_rotate_keep.load(std::memory_order_relaxed);
    if (keep > 0)
    {
        // 从最旧的一份开始往后挪：x.(keep-1) 丢弃，x.n -> x.(n+1)
        for (std::uint32_t i = keep; i >= 1; --i)
        {
            const std::wstring from = g_path + L"." + std::to_wstring(i);
            const std::wstring to = g_path + L"." + std::to_wstring(i + 1);
            if (i == keep)
                DeleteFileW(from.c_str());
            else
                MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        MoveFileExW(g_path.c_str(), (g_path + L".1").c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    open_file_locked(true);
}

void debugger_sink(const std::string &line)
{
    const std::uint64_t now = GetTickCount64();
    std::uint64_t window = g_debugger_window_start.load(std::memory_order_relaxed);
    if (now - window >= 1000)
    {
        g_debugger_window_start.store(now, std::memory_order_relaxed);
        g_debugger_in_window.store(0, std::memory_order_relaxed);
        window = now;
    }
    const std::uint32_t max_per_sec = g_debugger_max_per_sec.load(std::memory_order_relaxed);
    if (g_debugger_in_window.fetch_add(1, std::memory_order_relaxed) >= max_per_sec)
        return; // 限流：调试器 sink 不能拖慢游戏
    OutputDebugStringA(line.c_str());
    OutputDebugStringA("\n");
}

// 后台写线程：唯一碰文件的地方。detach 后自行退出，不阻塞卸载路径。
void writer_loop()
{
    for (;;)
    {
        std::string line;
        {
            std::unique_lock<std::mutex> lock(g_queue_mutex);
            g_queue_cv.wait_for(lock, std::chrono::milliseconds(200),
                                [] { return g_size != 0 || !g_alive.load(std::memory_order_relaxed); });
            if (g_size == 0)
            {
                if (!g_alive.load(std::memory_order_relaxed))
                    return;
                continue;
            }
            line = std::move(g_queue[g_head]);
            g_queue[g_head].clear();
            g_head = (g_head + 1) % k_max_queue;
            --g_size;
        }
        {
            std::lock_guard<std::mutex> lock(g_file_mutex);
            if (g_file.is_open())
            {
                g_file << line << "\n";
                g_bytes_written += line.size() + 1;
                rotate_locked();
            }
        }
        if (g_to_debugger.load(std::memory_order_relaxed))
            debugger_sink(line);
    }
}

// 兼容模式辅助：前缀 -> 分类。
// ⚠️ 仅在 [Log] compat_prefix=1 时用于**未迁移的旧 log_line 调用点**；
// 新代码一律经 LOG_*(分类, ...) 显式声明。保留它是为了让"还没迁移的二十来处调用点"
// 在过渡期仍能进到正确分类，而不是因为漏迁移就静默消失——那是旧方案的原病。
std::string_view category_from_prefix(std::string_view line)
{
    struct Entry { std::string_view prefix; std::string_view category; };
    static constexpr std::array<Entry, 25> table {{
        {"texture_trace", cat::probe}, {"texture_create", cat::probe},
        {"rtv_bind_target", cat::probe}, {"final_scene", cat::probe}, {"similarity", cat::probe},
        {"hdr_", cat::hdr}, {"tone_map", cat::hdr}, {"dxgi_hdr", cat::hdr},
        {"fsr2_", cat::upscale}, {"ffx12_", cat::upscale}, {"target_upscaler", cat::upscale},
        {"mode2_", cat::upscale}, {"draw_", cat::frame}, {"present", cat::frame},
        {"render_scale_menu", cat::menu}, {"hooked", cat::hook}, {"iat_scan", cat::hook},
        {"detour", cat::hook}, {"GetProcAddress", cat::hook}, {"LoadLibrary", cat::hook},
        {"dlssg_", cat::coexist}, {"optiscaler", cat::coexist}, {"fg_", cat::fg},
        {"build_profile", cat::core}, {"warning", cat::core},
    }};
    for (const Entry &entry : table)
        if (line.starts_with(entry.prefix) || line.find(entry.prefix) != std::string_view::npos)
            return entry.category;
    return cat::core;
}

// 兼容模式下的等级推断（**仅** log_line 兼容路径使用；
// 新代码一律走显式等级的 LOG_* 宏，不再有内容推断）
Level level_from_prefix(std::string_view line)
{
    static constexpr std::array<std::string_view, 6> error_terms {
        "failed", "failure", "error", "invalid", "unsupported", "missing"
    };
    for (const std::string_view term : error_terms)
        if (line.find(term) != std::string_view::npos)
            return Level::Error;
    if (line.starts_with("warning "))
        return Level::Warn;
    return Level::Info;
}

std::string format_line(Level level, std::string_view category, std::string_view message)
{
    SYSTEMTIME st {};
    GetLocalTime(&st);
    char prefix[80] {};
    std::snprintf(prefix, sizeof(prefix), "%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] [%.*s] ",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                  level_name(level), static_cast<int>(category.size()), category.data());
    std::string line;
    line.reserve(std::strlen(prefix) + message.size());
    line.append(prefix);
    line.append(message.data(), message.size());
    return line;
}

void push(std::string &&line)
{
    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        if (g_size == k_max_queue)
        {
            // 队列满：丢最旧一条（保证调用线程永不阻塞；丢的是最老的诊断，不是最新的）
            g_queue[g_head].clear();
            g_head = (g_head + 1) % k_max_queue;
            --g_size;
            g_dropped.fetch_add(1, std::memory_order_relaxed);
        }
        g_queue[g_tail] = std::move(line);
        g_tail = (g_tail + 1) % k_max_queue;
        ++g_size;
    }
    g_queue_cv.notify_one();
}

} // namespace

// ---------------------------------------------------------------------------

const char *level_name(Level level)
{
    switch (level)
    {
        case Level::Off:   return "OFF";
        case Level::Error: return "ERROR";
        case Level::Warn:  return "WARN";
        case Level::Info:  return "INFO";
        case Level::Debug: return "DEBUG";
        case Level::Trace: return "TRACE";
    }
    return "?";
}

bool parse_level(std::string_view text, Level &out)
{
    std::string lowered;
    lowered.reserve(text.size());
    for (const char c : text)
        lowered.push_back(static_cast<char>(::tolower(static_cast<unsigned char>(c))));
    struct Entry { std::string_view name; Level level; };
    static constexpr std::array<Entry, 12> table {{
        {"off", Level::Off}, {"0", Level::Off},
        {"error", Level::Error}, {"1", Level::Error},
        {"warn", Level::Warn}, {"warning", Level::Warn}, {"2", Level::Warn},
        {"info", Level::Info}, {"3", Level::Info},
        {"debug", Level::Debug}, {"4", Level::Debug},
        {"trace", Level::Trace},
    }};
    for (const Entry &entry : table)
    {
        if (lowered == entry.name)
        {
            out = entry.level;
            return true;
        }
    }
    if (lowered == "5")
    {
        out = Level::Trace;
        return true;
    }
    return false;
}

std::uint64_t now_ms()
{
    return GetTickCount64();
}

void set_level(Level level)
{
    g_level.store(static_cast<std::uint8_t>(level), std::memory_order_relaxed);
}

Level level()
{
    return static_cast<Level>(g_level.load(std::memory_order_relaxed));
}

void set_category_level(std::string_view category, Level level)
{
    std::lock_guard<std::mutex> lock(g_cat_mutex);
    g_cat_levels[std::string(category)] = level;
    g_has_cat_overrides.store(!g_cat_levels.empty(), std::memory_order_relaxed);
}

void clear_category_levels()
{
    std::lock_guard<std::mutex> lock(g_cat_mutex);
    g_cat_levels.clear();
    g_has_cat_overrides.store(false, std::memory_order_relaxed);
}

bool enabled(Level level, std::string_view category)
{
    if (!g_active.load(std::memory_order_relaxed))
        return false;
    if (level == Level::Off)
        return false;
    Level effective = static_cast<Level>(g_level.load(std::memory_order_relaxed));
    // 分类覆盖只在"确实存在覆盖项"时才取锁——常见情形（无覆盖）是一次原子读，
    // 这是热路径（每帧上千次判定）必须保证的。
    if (g_has_cat_overrides.load(std::memory_order_relaxed))
    {
        std::lock_guard<std::mutex> lock(g_cat_mutex);
        const auto it = g_cat_levels.find(std::string(category));
        if (it != g_cat_levels.end())
            effective = it->second;
    }
    if (effective == Level::Off)
        return false;
    return static_cast<std::uint8_t>(level) <= static_cast<std::uint8_t>(effective);
}

void write(Level level, std::string_view category, std::string_view message)
{
    if (!enabled(level, category))
        return;
    push(format_line(level, category, message));
}

bool active()
{
    return g_active.load(std::memory_order_relaxed);
}

void init(const Config &config)
{
    if (g_active.load(std::memory_order_relaxed))
        return;

    g_level.store(static_cast<std::uint8_t>(config.level), std::memory_order_relaxed);
    g_max_file_bytes.store(config.max_file_bytes, std::memory_order_relaxed);
    g_rotate_keep.store(config.rotate_keep, std::memory_order_relaxed);
    g_to_debugger.store(config.to_debugger, std::memory_order_relaxed);
    g_debugger_max_per_sec.store(config.debugger_max_per_sec, std::memory_order_relaxed);
    g_compat_prefix.store(config.compat_prefix_categories, std::memory_order_relaxed);
    g_dropped.store(0, std::memory_order_relaxed);

    if (config.to_file)
    {
        g_directory = config.directory_w;
        g_filename = config.filename_w.empty() ? std::wstring(L"Dx11FsrBridge.log") : config.filename_w;
        g_path = g_directory.empty() ? g_filename : (g_directory + L"\\" + g_filename);
        std::lock_guard<std::mutex> lock(g_file_mutex);
        open_file_locked(config.truncate_on_start);
        if (!g_file.is_open())
        {
            // 打开失败必须显式暴露：否则表现为"完全没有日志"，与"没跑日志器"无法区分。
            // （实测过的成因：路径按 UTF-8 转窄后交给 ofstream，被按 ACP=936 解释成乱码。）
            std::wstring message = L"log_open_failed path=" + g_path;
            OutputDebugStringW(message.c_str());
        }
    }

    g_alive.store(true, std::memory_order_relaxed);
    g_active.store(true, std::memory_order_relaxed);
    try
    {
        g_writer = std::thread(&writer_loop);
        g_writer.detach(); // detach：卸载路径不 join，避免 DllMain 死锁（线程自行退出）
    }
    catch (...)
    {
        // 线程创建失败：退化为"只入队不落盘"（不抛异常给注入宿主）
    }
}

void shutdown()
{
    if (!g_active.exchange(false, std::memory_order_relaxed))
        return;
    // 让 writer 把残余写完再自行退出；这里最多等 500ms，绝不无限阻塞。
    const std::uint64_t deadline = GetTickCount64() + 500;
    for (;;)
    {
        {
            std::lock_guard<std::mutex> lock(g_queue_mutex);
            if (g_size == 0)
                break;
        }
        if (GetTickCount64() >= deadline)
            break;
        Sleep(2);
    }
    g_alive.store(false, std::memory_order_relaxed);
    g_queue_cv.notify_all();
    {
        std::lock_guard<std::mutex> lock(g_file_mutex);
        if (g_file.is_open())
            g_file.flush();
    }
}

void log_effective_config()
{
    if (!active())
        return;
    std::string cats;
    {
        std::lock_guard<std::mutex> lock(g_cat_mutex);
        for (const auto &entry : g_cat_levels)
        {
            if (!cats.empty())
                cats += ",";
            cats += entry.first;
            cats += "=";
            cats += level_name(entry.second);
        }
    }
    bool file_open = false;
    {
        std::lock_guard<std::mutex> lock(g_file_mutex);
        file_open = g_file.is_open();
    }
    char buf[320] {};
    std::snprintf(buf, sizeof(buf),
                  "logger effective level=%s categories=%s file_open=%d file=%s",
                  level_name(level()), cats.empty() ? "(none)" : cats.c_str(),
                  file_open ? 1 : 0, file_open ? "ok" : "(OPEN FAILED)");
    write(Level::Info, cat::config, buf);
    // 另记一行文件全路径（宽字符原样转 UTF-8）：排查"日志到底写哪了"时必需。
    // 注意这里用本文件内的转换，**不经过桥的 narrow()**——那条路径曾因
    // UTF-8/ACP 不一致而把中文目录毁掉。
    {
        std::lock_guard<std::mutex> lock(g_file_mutex);
        if (!g_path.empty())
        {
            const int needed = WideCharToMultiByte(CP_UTF8, 0, g_path.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (needed > 1)
            {
                std::string utf8(static_cast<std::size_t>(needed - 1), '\0');
                WideCharToMultiByte(CP_UTF8, 0, g_path.c_str(), -1, utf8.data(), needed, nullptr, nullptr);
                write(Level::Info, cat::config, "logger file_path=" + utf8);
            }
        }
    }
}

} // namespace blog
