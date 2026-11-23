// TjTraceProbe.cpp —— 【诊断探针】侧车文件写入单元的实现。
// 设计理由（为什么独立 TU、为什么只用 kernel32、为什么不取锁）见 TjTraceProbe.h 顶部。
//
// 为什么单独成 TU、而不是塞进 Dx11FsrBridge.cpp：
//   调用方有三个互不依赖的 TU —— Dx11FsrBridge.cpp（DllMain / atexit）、
//   BridgeLogger.cpp（blog::shutdown）、TransparentJitterHook.cpp（shutdown 埋点）。
//   其中 BridgeLogger.cpp 刻意不 include 桥的头，而 TransparentJitterHook.cpp 为了能被
//   单测单独编译，也刻意不依赖 BridgeLogger ⇒ 把侧车写函数放进任一既有 TU，都会让另一个 TU
//   反向依赖它。独立 TU 让三方只依赖一个"零依赖的小接口"，构建风险最小。
//
// 行尾口径（2026-11-23 修复轮 2 / 缺陷 C）：`_tj-trace.txt` 与 `_tj-trace.err` **都用 \r\n**。
//   选它的理由只需两条、都可当场复核：① 本文件是 Windows 侧车日志，记事本/VS Code 直接可读；
//   ② 两者必须同口径 —— 混用会让按行读的工具在某些行上把两条记录并成一条。
//   （原先此处写"与 2026-11-22 那次 detach 探针一致"。那句已删，理由如实说清：
//   `detach_marker` 在**本仓库已跟踪的代码里**不存在（`git grep` 与 `git log -S` 均 0 命中），
//   只出现在某份**不随仓库分发**的本地设计稿的原型声明里 —— 那种稿子读者拿不到，
//   不能当作可复核的一致性依据。2026-11-23 / 审核 R5 残留 C 更正。）
//
// 2026-11-23（独立审核 MAJOR-8 / MINOR 后补齐的三件事，都是零锁零分配）：
//   ① _tj-trace.err 第二通道：本文件写失败时原先只走 OutputDebugStringW，而生产构建
//      [Log] to_debugger=0 ⇒ ODS 永远看不见 ⇒ "写失败"与"没走到那一步"在观测者眼里一样。
//      现在失败原因（phase + GetLastError()）追加进 _tj-trace.err。**写 .err 失败即静默**
//      （不递归、不重试），否则就是无限套娃。
//   ② 每行带单调序号 seq=N：进程在两条 write 之间被 TerminateProcess 时（多行写非原子，
//      本质无法消除），靠 seq 能看出"中断在第几行之后"。
//   ③ ODS 只在诊断构建里启用（DBWIN 握手理论上可等待，正式版不该多这一条路径）。

#include "TjTraceProbe.h"

// 2026-11-23（审核 R7）：本 TU 只在 DX11FSRBRIDGE_TJ_TRACE_PROBE=ON 时进源列表；
// 这里再加一道编译期守卫，防止有人手工把它塞进别的 target（那时会与头里的内联空实现
// 重定义，链接期报 duplicate symbol —— 与其让人对着链接错误猜，不如让本 TU 直接空掉）。
#if defined(DX11FSRBRIDGE_TJ_TRACE_PROBE)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace tjtrace
{
namespace
{

constexpr wchar_t k_trace_leaf[] = L"\\_tj-trace.txt";
constexpr wchar_t k_error_leaf[] = L"\\_tj-trace.err";
// 一行最多多少个字（探针行都是 stage + seq + 3~6 个 key=value，256 足够；
// 超了就整行不写，见 write() 里的"写不下就整行不写"）。
constexpr std::size_t k_line_capacity = 512;

// 本 DLL 的模块句柄（DllMain(ATTACH) 第一行由 set_module() 写入，此后只读）。
// 写成 void* 是为了让 TjTraceProbe.h 不依赖 windows.h（见头文件说明）。
void *g_module = nullptr;

// 行序号：跨线程单调（fetch_add relaxed，无锁、无分配）。只用于"中断在第几行之后"的判读，
// 不参与任何逻辑判定，故 relaxed 足够。
std::atomic<unsigned> g_seq { 0 };

// 2026-11-23（诊断探针 / MINOR）：ODS 只在诊断构建里启用。
// 理由：ODS 在有 debug monitor 时会与 DBWIN_BUFFER_READY 事件握手，理论上可等待；
// 而本单元的调用点全在 DllMain(DETACH) 与 blog::shutdown() 里。正式构建走 .err 文件通道，
// 观测能力不变（.err 有 phase + GetLastError，比一行固定字面量信息更多）。
void debug_note(const wchar_t *text)
{
#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)
    OutputDebugStringW(text);
#else
    (void)text;
#endif
}

// 2026-11-23（诊断探针）：本函数在 loader lock 内被调用，**不许分配、不许取锁**。
// 因此路径拼装用定长 wchar_t 缓冲 + memcpy，不构造 std::wstring、
// 也不新建 std::filesystem::path 临时对象 —— 那两者都会走 CRT 堆（取堆锁）。
// leaf 的长度由 leaf_length 给出（刻意不用 wcslen：能不下沉到 CRT 就不下沉）。
bool build_path(const wchar_t *leaf, std::size_t leaf_length, wchar_t *out, std::size_t capacity)
{
    if (leaf == nullptr || out == nullptr || capacity < 64)
        return false;
    out[0] = L'\0';

    // GetModuleFileNameW 只取文件系统路径，不取任何锁。
    // 用**本 DLL** 的句柄，不是 nullptr：nullptr 取的是进程主模块（YuanShen.exe）的目录，
    // 而 trace 必须落在 Dx11FsrBridge.log 旁边。g_module 未注入时退回主模块目录 ——
    // 那种情形只可能是"DllMain 没进来就跑到了写点"，本身就该被判读成异常。
    wchar_t module_path[MAX_PATH] {};
    const DWORD length = GetModuleFileNameW(static_cast<HMODULE>(g_module), module_path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return false;

    // 去掉文件名，保留到最后一个分隔符（含）。
    std::size_t dir_length = length;
    while (dir_length > 0 && module_path[dir_length - 1] != L'\\' && module_path[dir_length - 1] != L'/')
        --dir_length;
    if (dir_length == 0)
        return false;

    if (dir_length + leaf_length + 1 > capacity)
        return false;
    std::memcpy(out, module_path, dir_length * sizeof(wchar_t));
    std::memcpy(out + dir_length, leaf, (leaf_length + 1) * sizeof(wchar_t));
    return true;
}

bool trace_path(wchar_t *out, std::size_t capacity)
{
    constexpr std::size_t k_leaf_length = std::size(k_trace_leaf) - 1;   // 不含结尾 '\0'
    return build_path(k_trace_leaf, k_leaf_length, out, capacity);
}

// 2026-11-23（诊断探针 / MAJOR-8 C·D）：第二通道。**写失败即静默** —— 本函数任何一步失败都
// 直接返回，绝不再调用自己、也不再试 ODS（否则"写不进去"会变成"无限重试"）。
// phase 只接受静态字面量（不拼字符串 ⇒ 不进堆、不取堆锁）。
void error_note(const char *phase, DWORD last_error)
{
    if (phase == nullptr)
        return;

    constexpr std::size_t k_leaf_length = std::size(k_error_leaf) - 1;
    wchar_t path[MAX_PATH] {};
    if (!build_path(k_error_leaf, k_leaf_length, path, std::size(path)))
        return;

    char line[192] {};
    SYSTEMTIME st {};
    GetLocalTime(&st);
    const int written = std::snprintf(line, sizeof(line),
        "%04u-%02u-%02uT%02u:%02u:%02u.%03u phase=%s err=%lu pid=%lu\r\n",
        static_cast<unsigned>(st.wYear), static_cast<unsigned>(st.wMonth),
        static_cast<unsigned>(st.wDay), static_cast<unsigned>(st.wHour),
        static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond),
        static_cast<unsigned>(st.wMilliseconds), phase,
        static_cast<unsigned long>(last_error), static_cast<unsigned long>(GetCurrentProcessId()));
    if (written <= 0 || written >= static_cast<int>(sizeof(line)))
        return;

    // 三个共享位全开（与 trace 同口径）：.err 也可能正被文本查看器打开。
    const HANDLE file = CreateFileW(path, FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;   // ← 到这儿就到底了：不递归、不重试、不出声。

    DWORD bytes_written = 0;
    WriteFile(file, line, static_cast<DWORD>(written), &bytes_written, nullptr);
    CloseHandle(file);   // 返回码刻意不看：这一通道只负责"尽力留证"，不留证也不影响主流程
}

} // namespace

void set_module(void *module)
{
    g_module = module;
}

namespace
{

// 2026-11-23（诊断探针 / 审核 MAJOR-9）：删一个会话文件，失败留证但不重试、不递归。
// phase 由调用方给（.txt 与 .err 用不同的 phase，好在 .err 里区分是哪个文件删不掉）。
void delete_session_file(const wchar_t *path, const char *phase)
{
    if (DeleteFileW(path) == FALSE)
    {
        const DWORD saved_error = GetLastError();
        // ERROR_FILE_NOT_FOUND = 本会话第一次跑、旧文件本来就不存在 —— 正常情形，不留证。
        if (saved_error != ERROR_FILE_NOT_FOUND)
        {
            debug_note(L"tj_trace: delete_failed");
            error_note(phase, saved_error);
        }
    }
}

} // namespace

void reset()
{
    wchar_t path[MAX_PATH] {};
    if (!trace_path(path, std::size(path)))
    {
        // 连落点都算不出来 ⇒ 文件通道不存在。必须留证，否则会被读成"探针从没工作过"。
        // 2026-11-23（修复轮 2 / 缺陷 B）：**先**取 GetLastError 再出声 —— debug_note 在诊断
        // 构建里会调 OutputDebugStringW，那会覆盖 last error，导致 .err 记下假原因。
        // 诚实标注：若失败原因是纯长度检查（dir_length==0 / 缓冲不够），GetModuleFileNameW
        // 并未失败，这里的 err 可能是上一步残留的旧值 —— 故 .err 里 err= 只能当线索，不能当铁证。
        const DWORD saved_error = GetLastError();
        debug_note(L"tj_trace: path_unavailable");
        error_note("path_unavailable", saved_error);
        return;
    }
    // 2026-11-23（诊断探针 / 审核 MAJOR-9）：`.err` 也必须一起清。
    // 为什么：`.err` 用 OPEN_ALWAYS + FILE_APPEND_DATA 写，**永不截断** ⇒ 上个会话的失败记录
    // 会留到本次，而判读表第一行是「有 phase=open_failed/write_short ⇒ 通道坏」⇒
    // 一条历史失败会把本次的"通道正常"读成"通道坏"（假阳性）。
    // 与 .txt 同口径：删不掉就算了，静默（.err 里留证），绝不递归。
    //
    // ⚠️ **顺序刻意是先 .err、后 .txt**（自审时发现的问题，2026-11-23）：
    //   两条删除都会在失败时 error_note() 往 .err 追加一条记录。若先删 .txt：
    //   「.txt 删不掉」的证据先写进 .err，紧接着删 .err 就把它抹掉了 ⇒ 证据自毁。
    //   先删 .err 则后续 .txt 失败的记录落在**新建的** .err 里，能留下。
    wchar_t error_path[MAX_PATH] {};
    if (build_path(k_error_leaf, std::size(k_error_leaf) - 1, error_path, std::size(error_path)))
        delete_session_file(error_path, "delete_err_failed");

    // 删不掉（被占用 / 只读）就放弃：最多是留一份上个进程的旧内容，
    // 判读时靠每行的 pid / seq / 时间戳区分，绝不影响加载。
    // 2026-11-23（修复轮 2 / 缺陷 B）：DeleteFileW 的错误码必须在**它之后立刻**取，
    // 且必须在 debug_note **之前**取（ODS 会覆盖 last error）。
    delete_session_file(path, "delete_failed");
}

bool write(const char *stage,
           const char *field1, const char *field2, const char *field3,
           const char *field4, const char *field5,
           const char *format, ...)
{
    if (stage == nullptr || stage[0] == '\0')
        return false;

    wchar_t path[MAX_PATH] {};
    if (!trace_path(path, std::size(path)))
    {
        // 2026-11-23（修复轮 2 / 缺陷 B）：同 reset() —— 先取错误码，再出声。
        const DWORD saved_error = GetLastError();
        debug_note(L"tj_trace: path_unavailable");
        error_note("path_unavailable", saved_error);
        return false;
    }

    // 2026-11-23（诊断探针）：整行先在定长缓冲里拼好，再**一次** WriteFile —— 保证一行
    // 原子落盘；分两次写会在中间被别的进程插进去（trace 可能正被文本查看器开着）。
    // 缓冲放**栈上**（512 B，可忽略）：thread_local 会拉进 CRT 的动态 TLS 初始化，
    // 而本函数在 DllMain(DETACH) 与进程退出期被调用 —— 那条路径上少一个 CRT 依赖就少一个未知。
    char line[k_line_capacity] {};
    int written = 0;

    // seq 紧跟在 stage 后面：判读时"最后一条 seq 与其后应出现的行"就能看出中断在哪一步。
    // 注意它只能区分"断在哪两条之间"，**不能**救"两条 write 之间被 TerminateProcess"
    // —— 多行写本来就不是原子的，这条局限写进判读表。
    const unsigned seq = g_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    SYSTEMTIME st {};
    GetLocalTime(&st);
    // pid 是本行的自证字段：文件名固定且 reset() 的删除可能失败（返回值已检查并留证），
    // 上一个进程留下的行会原样保留 —— 只有 pid 能把两代会话分开。
    written = std::snprintf(line, sizeof(line), "%04u-%02u-%02uT%02u:%02u:%02u.%03u stage=%s seq=%u",
                            static_cast<unsigned>(st.wYear), static_cast<unsigned>(st.wMonth),
                            static_cast<unsigned>(st.wDay), static_cast<unsigned>(st.wHour),
                            static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond),
                            static_cast<unsigned>(st.wMilliseconds), stage, seq);
    if (written <= 0 || written >= static_cast<int>(sizeof(line)))
    {
        debug_note(L"tj_trace: overlong_stage");
        error_note("overlong_stage", 0);
        return false;
    }
    int cursor = written;

    // 追加调用方给的 key=value 字段（传 nullptr 即结束）。每个字段前面加一个空格，
    // 使行形如 stage=NAME seq=N k=v k=v，便于按空格切分。
    const char *const fields[5] = {field1, field2, field3, field4, field5};
    for (const char *field : fields)
    {
        if (field == nullptr || field[0] == '\0')
            break;
        const int appended = std::snprintf(line + cursor, sizeof(line) - static_cast<std::size_t>(cursor),
                                           " %s", field);
        if (appended <= 0 || cursor + appended >= static_cast<int>(sizeof(line)))
        {
            debug_note(L"tj_trace: overlong_line");
            error_note("overlong_line", 0);
            return false;
        }
        cursor += appended;
    }

    // format 是各探针点自己的"字段名 + 顺序"（见头文件用法示例）。为空表示不带数值字段。
    if (format != nullptr && format[0] != '\0')
    {
        va_list args;
        va_start(args, format);
        const int appended = std::vsnprintf(line + cursor, sizeof(line) - static_cast<std::size_t>(cursor),
                                            format, args);
        va_end(args);
        if (appended <= 0 || cursor + appended >= static_cast<int>(sizeof(line)))
        {
            // 写不下就整行不写：宁可少一行，也不要落一行被截断、字段读不全的记录 ——
            // 半截行的字段缺失会被读成"这个值是 0"。
            debug_note(L"tj_trace: overlong_line");
            error_note("overlong_line", 0);
            return false;
        }
        cursor += appended;
    }

    const int tail = std::snprintf(line + cursor, sizeof(line) - static_cast<std::size_t>(cursor),
                                   " pid=%lu\r\n", static_cast<unsigned long>(GetCurrentProcessId()));
    if (tail <= 0 || cursor + tail >= static_cast<int>(sizeof(line)))
    {
        debug_note(L"tj_trace: overlong_line");
        error_note("overlong_line", 0);
        return false;
    }
    cursor += tail;

    // 三个共享位全开：trace 可能正被文本查看器打开，共享冲突会让观测**静默失败** ——
    // 那正是本探针要防的假阴性（"没写进去"会被读成"没执行"）。
    const HANDLE file = CreateFileW(path, FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        // 2026-11-23（修复轮 2 / 缺陷 B）：CreateFileW 的错误码先存下来，再出声。
        const DWORD saved_error = GetLastError();
        debug_note(L"tj_trace: open_failed");
        error_note("open_failed", saved_error);
        return false;
    }

    DWORD bytes_written = 0;
    // 校验实际写入字节数：WriteFile 成功也可能只写一部分（盘满/配额），
    // 落一行残行却当成功，残行字段缺失会被读成 0。
    const BOOL ok = WriteFile(file, line, static_cast<DWORD>(cursor), &bytes_written, nullptr);
    // 2026-11-23（修复轮 2 / 缺陷 A）：错误码必须在 CloseHandle **之前**取。
    // CloseHandle 会改 last error ⇒ 原先 .err 里记的是 CloseHandle 的结果（通常是 0），
    // 而 .err 通道存在的全部意义就是留住真实失败原因，那等于在最需要时给出错值。
    // 成功路径写 0：此时 err= 不参与判读（bytes_written 短写才是信号）。
    const DWORD write_error = (ok == FALSE) ? GetLastError() : 0;
    CloseHandle(file);
    if (ok == FALSE || bytes_written != static_cast<DWORD>(cursor))
    {
        debug_note(L"tj_trace: write_short");
        error_note("write_short", write_error);
        return false;
    }
    return true;
}

} // namespace tjtrace

#endif // DX11FSRBRIDGE_TJ_TRACE_PROBE
