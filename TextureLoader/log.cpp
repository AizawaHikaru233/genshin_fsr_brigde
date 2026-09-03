// SPDX-License-Identifier: GPL-3.0-or-later
// log.cpp
#include "log.h"
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <mutex>

namespace tloader
{
namespace
{
std::mutex g_mutex;
FILE *g_file = nullptr;
} // namespace

int g_log_level = 1;
int g_vram_threshold_pct = 15;   // 默认：可用显存 < 总显存 15% 视为压力
uint64_t g_vram_threshold_bytes = 0; // 0=百分比模式
int g_max_texture_side = 0;

void log_init(const std::wstring &dll_dir)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file)
        return;
    std::wstring path = dll_dir + L"\\TextureLoader.log";
    _wfopen_s(&g_file, path.c_str(), L"a, ccs=UTF-8");
}

void log_shutdown()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file)
    {
        fclose(g_file);
        g_file = nullptr;
    }
}

void log_write(const wchar_t *fmt, ...)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file)
        return;

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
