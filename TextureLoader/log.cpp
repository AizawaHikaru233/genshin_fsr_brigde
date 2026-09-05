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
int g_vram_threshold_pct = 0;    // 0=自适应（按显存容量推导临界值）；>0=用户显式百分比
uint64_t g_vram_threshold_bytes = 0; // 0=百分比模式；>0=用户显式容量
int g_max_texture_side = 0;
int g_gdds_enabled = 1;          // 默认启用 GDDS（DirectStorage GPU 解压）
int g_async_load = 1;            // 默认异步（后台线程）；N 卡在 AttachToDevice 时自动切同步
int g_async_load_explicit = 0;   // 0=未显式设置（按 GPU 厂商自动调整）

void log_init(const std::wstring &dll_dir)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file)
        return;
    // 每次运行覆盖上一次日志（"w" = 截断重写），避免日志无限增长
    std::wstring path = dll_dir + L"\\TextureLoader.log";
    _wfopen_s(&g_file, path.c_str(), L"w, ccs=UTF-8");
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
