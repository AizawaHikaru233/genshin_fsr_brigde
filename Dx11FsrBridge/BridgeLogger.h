#pragma once
// ============================================================================
// BridgeLogger — Dx11FsrBridge 的统一日志接口
//
// 设计目标（替代旧的"按消息内容猜等级"白名单/黑名单方案）：
//   1) **显式等级**：等级由调用点声明（LOG_INFO/LOG_DEBUG/...），不再靠关键词推断。
//      旧方案的根因问题：`log_line_level()` 用 error_terms/info_terms/debug_terms 三张
//      词表猜等级，未命中即返回 3；结果是"探针明明在跑、日志却一行不落盘"，
//      与"探针没跑"在现象上完全一致（实测为此白排查两轮）。
//   2) **分类过滤**：等级之外再按子系统（category）过滤，可只把某个子系统调到 debug，
//      不必开全局 verbose。这是 OptiScaler 用多个具名 logger 达到的效果。
//   3) **可预测的开关**：等级/分类在 ini 里显式配置（[Log] 段），不做内容推断。
//   4) **多 sink**：文件（唯一事实来源）+ 可选调试器输出（有限流）。控制台 sink 不提供：
//      游戏内注入没有控制台，AllocConsole 会抢焦点。
//   5) **异步写盘**：调用线程只做入队，写文件在单一后台线程（注入 DLL 尤其不能阻塞渲染线程）。
//   6) **不会淹死日志的手段**：`LOG_ONCE` / `LOG_THROTTLE` 两个宏自带去重与限频，
//      取代散落各处、各自为政的 `static std::atomic_uint64_t xxx_count` 手写节流。
//   7) **轮转**：超过大小上限即轮转（保留 N 份），长场会话不再产出 7MB+ 单文件。
//   8) **零成本过滤**：等级/分类被过滤时，消息表达式**不求值**（见宏里的早退判断），
//      因此热路径上可以直接写 `LOG_TRACE("draw", "..." + expensive())` 而不担心开销。
//
// 兼容性：`log_line()` 保留为 std::string 入口（旧调用点与外部回调无需立刻改写），
// 但它**不再做内容推断**——统一按 (core, info) 记录。迁移期可用 ini `compat=` 打开
// 兼容分类，让旧行按前缀归入分类（过渡用，默认关闭）。
// ============================================================================

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

namespace blog
{

// 等级：数值越小越严重。与 spdlog 的语义对齐（trace < debug < info < warn < error < off）。
enum class Level : std::uint8_t
{
    Off = 0,
    Error = 1,
    Warn = 2,
    Info = 3,
    Debug = 4,
    Trace = 5,
};

const char *level_name(Level level);          // "ERROR"/"WARN"/"INFO"/"DEBUG"/"TRACE"
bool parse_level(std::string_view text, Level &out); // "info"/"2"/"warn" 均可

// 分类（子系统）。新增子系统时在此登记，便于 ini 文档与过滤一致。
namespace cat
{
inline constexpr std::string_view core = "core";           // 生命周期/加载/路由/GPU
inline constexpr std::string_view config = "config";       // ini 读取与生效结果
inline constexpr std::string_view hook = "hook";           // DXGI/D3D11/il2cpp 钩子安装
inline constexpr std::string_view upscale = "upscale";     // FSR/FFX 后端与 dispatch
inline constexpr std::string_view frame = "frame";         // 每帧状态（默认关）
inline constexpr std::string_view probe = "probe";         // 诊断探针（纹理/渲染目标追踪等）
inline constexpr std::string_view menu = "menu";           // 渲染精度菜单集成
inline constexpr std::string_view hdr = "hdr";             // HDR/色彩/色调映射
inline constexpr std::string_view coexist = "coexist";     // OptiScaler / ReShade / DLSSG 共存
inline constexpr std::string_view fg = "fg";               // 帧生成
} // namespace cat

// 一次性初始化。filename 为空时使用调用方目录下的默认名。
// 由 bridge 在读完 ini 后调用一次；未调用时日志器处于关闭状态（零开销）。
struct Config
{
    bool enabled = true;
    bool to_file = true;
    bool to_debugger = false;         // OutputDebugString（DebugView 可看；有限流）
    // ⚠️ 路径必须是**宽字符串**：注入目录常含中文（实测 Starward 路线为
    // `...\原神解帧FSR插件包\payload\Bridge`）。若在此传 UTF-8 窄串再交给
    // std::ofstream，流会按系统 ACP（中文 Windows 为 936/GBK）解释字节 →
    // 路径乱码 → **文件打不开且不报错**，表现为"完全没有日志"。
    // 旧实现之所以没事，是因为它用的是 std::filesystem::path 的宽字符重载。
    std::wstring directory_w;         // 缺省 = 调用方自行决定
    std::wstring filename_w = L"Dx11FsrBridge.log";
    Level level = Level::Info;        // 全局默认等级
    bool truncate_on_start = true;    // 每次启动清空（保留旧行为）
    std::uint64_t max_file_bytes = 16ull * 1024 * 1024; // 0 = 不轮转
    std::uint32_t rotate_keep = 2;
    std::uint32_t queue_capacity = 8192;
    std::uint32_t debugger_max_per_sec = 200; // 调试器 sink 限流，防拖慢游戏
    bool compat_prefix_categories = false;    // 旧 log_line 行按前缀归类（过渡开关）
};

void init(const Config &config);
void shutdown();
bool active();

// 运行时调整（ini 热重载用）。
void set_level(Level level);
Level level();
// 分类覆盖：level 为 Off 表示"该分类静默"。
void set_category_level(std::string_view category, Level level);
void clear_category_levels();

// 过滤判定（宏用；调用点可自行判断以避免构造昂贵消息）。
bool enabled(Level level, std::string_view category);

// 低频诊断：把当前生效的等级/分类配置回显到日志（启动时调用一次）。
void log_effective_config();

// 兼容模式：按行首前缀推断分类与等级（仅 [Log] compat_prefix=1 时的旧 log_line 路径）。
// 未开启 compat 时不要调用——新代码一律用 LOG_*(分类, ...) 显式声明。
bool compat_prefix_enabled();
void write_compat_line(std::string_view line);

// std::string 入口（旧调用点 / 回调适配）。level/category 显式给出，不做内容推断。
void write(Level level, std::string_view category, std::string_view message);
inline void write(Level level, std::string_view category, const std::string &message)
{
    write(level, category, std::string_view(message));
}
inline void write(Level level, std::string_view category, const char *message)
{
    write(level, category, std::string_view(message ? message : ""));
}

} // namespace blog

// ---------------------------------------------------------------------------
// 宏：等级被过滤时消息表达式不求值（热路径零成本）
// ---------------------------------------------------------------------------
#define BLOG_ENABLED(lvl, category) \
    (::blog::enabled((lvl), (category)))

#define BLOG_WRITE(lvl, category, ...)                                        \
    do                                                                        \
    {                                                                         \
        if (BLOG_ENABLED((lvl), (category)))                                  \
            ::blog::write((lvl), (category), (__VA_ARGS__));                  \
    } while (0)

#define LOG_TRACE(category, ...) BLOG_WRITE(::blog::Level::Trace, (category), __VA_ARGS__)
#define LOG_DEBUG(category, ...) BLOG_WRITE(::blog::Level::Debug, (category), __VA_ARGS__)
#define LOG_INFO(category, ...)  BLOG_WRITE(::blog::Level::Info,  (category), __VA_ARGS__)
#define LOG_WARN(category, ...)  BLOG_WRITE(::blog::Level::Warn,  (category), __VA_ARGS__)
#define LOG_ERROR(category, ...) BLOG_WRITE(::blog::Level::Error, (category), __VA_ARGS__)

// 仅记录一次（按调用点标识去重）。用于"首次失败/首次成功"这类一次性诊断。
#define LOG_ONCE(lvl, category, tag, ...)                                     \
    do                                                                        \
    {                                                                         \
        static std::atomic_bool blog_once_##tag { false };                    \
        if (BLOG_ENABLED((lvl), (category)) &&                               \
            !blog_once_##tag.exchange(true, std::memory_order_relaxed))       \
            ::blog::write((lvl), (category), (__VA_ARGS__));                  \
    } while (0)

// 限频：同一调用点每 interval_ms 最多一条。取代手写的计数器节流。
#define LOG_THROTTLE(lvl, category, tag, interval_ms, ...)                    \
    do                                                                        \
    {                                                                         \
        static std::atomic<std::uint64_t> blog_next_##tag { 0 };              \
        if (BLOG_ENABLED((lvl), (category)))                                 \
        {                                                                     \
            const std::uint64_t blog_now_##tag = ::blog::now_ms();            \
            std::uint64_t blog_expected_##tag =                               \
                blog_next_##tag.load(std::memory_order_relaxed);              \
            if (blog_now_##tag >= blog_expected_##tag &&                      \
                blog_next_##tag.compare_exchange_strong(                      \
                    blog_expected_##tag, blog_now_##tag + (interval_ms),      \
                    std::memory_order_relaxed))                               \
                ::blog::write((lvl), (category), (__VA_ARGS__));              \
        }                                                                     \
    } while (0)

// 带计数器的限频：每 interval_ms 最多一条，且每 2^n 条额外放行一条（对数采样）。
// 适合"错误持续发生但需要看到量级"的场景。
#define LOG_THROTTLE_COUNT(lvl, category, tag, interval_ms, ...)              \
    do                                                                        \
    {                                                                         \
        static std::atomic<std::uint64_t> blog_count_##tag { 0 };             \
        static std::atomic<std::uint64_t> blog_next_##tag { 0 };              \
        if (BLOG_ENABLED((lvl), (category)))                                 \
        {                                                                     \
            const std::uint64_t blog_n_##tag =                                \
                blog_count_##tag.fetch_add(1, std::memory_order_relaxed) + 1; \
            const std::uint64_t blog_now_##tag = ::blog::now_ms();            \
            const bool blog_pow2_##tag = (blog_n_##tag & (blog_n_##tag - 1)) == 0; \
            std::uint64_t blog_expected_##tag =                               \
                blog_next_##tag.load(std::memory_order_relaxed);              \
            if ((blog_pow2_##tag || blog_now_##tag >= blog_expected_##tag) && \
                blog_next_##tag.compare_exchange_strong(                      \
                    blog_expected_##tag, blog_now_##tag + (interval_ms),      \
                    std::memory_order_relaxed))                               \
                ::blog::write((lvl), (category), (__VA_ARGS__));              \
        }                                                                     \
    } while (0)

namespace blog
{
// 宏用的毫秒时钟（与 spdlog 的 steady 时钟同义；此处暴露以便 LOG_THROTTLE 使用）。
std::uint64_t now_ms();
} // namespace blog
