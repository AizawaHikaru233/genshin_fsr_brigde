// SPDX-License-Identifier: GPL-3.0-or-later
// log.cpp
#include "log.h"
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <string>

namespace tloader
{
namespace
{
std::mutex g_mutex;
FILE *g_file = nullptr;
bool g_init_done = false;
// ⚠️ 2026-09-23（审核报告）：崩溃路径要用的两条**预先算好**的绝对路径。
// 必须预先算好 —— 崩溃处理器内做 `std::wstring` 拼接会碰堆，
// 而堆可能正是被损坏的那块。
std::wstring g_main_path;  // <dir>\TextureLoader.log
std::wstring g_crash_path; // <dir>\TextureLoader.crash.log
} // namespace

int g_log_level = 1;
int g_vram_threshold_pct = 0;    // 0=自适应（按显存容量推导临界值）；>0=用户显式百分比
uint64_t g_vram_threshold_bytes = 0; // 0=百分比模式；>0=用户显式容量
int g_max_texture_side = 0;
int g_gdds_enabled = 1;          // 默认启用 GDDS（DirectStorage GPU 解压）
int g_async_load = 1;            // 默认异步（后台线程）；N 卡在 AttachToDevice 时自动切同步
int g_async_load_explicit = 0;   // 0=未显式设置（按 GPU 厂商自动调整）

// 真正打开日志文件。**只允许在非 DllMain 上下文调用**（见 log_init 的说明）。
// 调用方须持有 g_mutex。
static void open_locked()
{
    if (g_file || g_main_path.empty())
        return;
    // 每次运行覆盖上一次日志（"w" = 截断重写），避免日志无限增长
    _wfopen_s(&g_file, g_main_path.c_str(), L"w, ccs=UTF-8");
    // 打开失败（或成功）后都不再重试：避免每次写日志都做一次失败的 fopen。
}

// 记录日志目录，但**不**在这里打开文件。
//
// ⚠️ 2026-09-19（审核报告）：原实现在本函数内直接 `_wfopen_s`，而它由
// `DllMain(DLL_PROCESS_ATTACH)` 调用 —— 即**在 loader lock 持有期间做文件 I/O**。
// 那是 Windows 上著名的死锁模式：CRT 的 `fopen` 需要初始化 CRT 内部状态/堆，
// 若此时另一个线程正卡在 loader lock 上等待（或 CRT 内部锁与 loader lock 形成环），
// 就会**整个进程挂死**，且症状（启动即卡住、无日志）极难归因。
//
// 现在只记路径，真正的 `fopen` 推迟到**第一次 `log_write`** —— 那必然发生在
// `DllMain` 返回之后（首条日志来自 AttachToDevice / 设备钩子），loader lock 已释放。
// 好处：日志行为不变（仍写同一文件、仍是首次写入时创建），
// 但把不可控的 I/O 移出了 loader lock 窗口。
void log_init(const std::wstring &dll_dir)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_init_done)
        return;
    g_init_done = true;
    // 预先算好两条路径（崩溃处理器内不做字符串拼接/堆分配 —— 见 g_main_path 说明）
    g_main_path = dll_dir + L"\\TextureLoader.log";
    g_crash_path = dll_dir + L"\\TextureLoader.crash.log";
    // 若此前已有写入请求（理论上不会：DllMain 内不会有 log_write），此处不打开。
}

void log_shutdown()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file)
    {
        fclose(g_file);
        g_file = nullptr;
    }
    g_init_done = false;
}

// 崩溃路径写入（2026-09-23，审核报告）：**绝不阻塞在 g_mutex 上**。
// 详细理由见 log.h 的声明处。要点：
//   崩溃可能发生在持有 g_mutex 的代码里（fwprintf/fflush 内部违例、堆损坏），
//   此时取同一把非递归锁 = 自死锁 ⇒ 崩溃报告永远写不出来，进程还挂着。
//
// 本函数刻意只用**不会碰 CRT 堆、不会取锁**的 API：
//   _vsnwprintf_s（不分配）/ GetLocalTime / CreateFileW / WriteFile / CloseHandle。
// 不做 std::wstring 拼接 —— 路径已在 log_init 里预先算好。
void log_crash_write(const wchar_t *fmt, ...)
{
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    const wchar_t *level = L"";

    // 1) 首选：正常日志文件 —— 但只**尝试**取锁，拿不到就走退路
    {
        std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
        if (lock.owns_lock())
        {
            if (!g_file)
                open_locked();
            if (g_file)
            {
                fwprintf(g_file, L"%04d-%02d-%02d %02d:%02d:%02d.%03d  %s\n",
                         st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                         st.wMilliseconds, buf);
                fflush(g_file);
                return;
            }
        }
    }

    // 2) 退路：独立句柄 + 独立文件，完全不碰 g_mutex。
    //    正常写入此刻已停止（进程在崩溃），两条路径不会交错。
    if (g_crash_path.empty())
        return;
    HANDLE h = CreateFileW(g_crash_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;

    char line[2304];
    const int head = _snprintf_s(line, sizeof(line), _TRUNCATE,
                                 "%04d-%02d-%02d %02d:%02d:%02d.%03d  [crash-fallback] ",
                                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                                 st.wSecond, st.wMilliseconds);
    int used = (head > 0) ? head : 0;
    int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, line + used,
                                (int)sizeof(line) - used - 1, nullptr, nullptr);
    if (n > 0)
    {
        used += n - 1; // n 含结尾 NUL，不计入
        if (used < (int)sizeof(line) - 2)
        {
            line[used++] = '\r';
            line[used++] = '\n';
        }
        DWORD written = 0;
        WriteFile(h, line, (DWORD)used, &written, nullptr);
    }
    CloseHandle(h);
    (void)level;
}

void log_write(const wchar_t *fmt, ...)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    // 首次写入时才真正打开（把 fopen 移出 DllMain 的 loader lock 窗口）
    if (!g_file)
    {
        open_locked();
        if (!g_file)
            return;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);

    va_list ap;
    va_start(ap, fmt);
    wchar_t buf[2048];
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);

    fwprintf(g_file, L"%04d-%02d-%02d %02d:%02d:%02d.%03d  %s\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
             buf);
    fflush(g_file);
}
} // namespace tloader
