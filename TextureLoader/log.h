// SPDX-License-Identifier: GPL-3.0-or-later
// log.h — 轻量线程安全日志（写 DLL 同目录 TextureLoader.log）
#pragma once
#include <string>
#include <cstdint>

namespace tloader
{
// 初始化日志：路径为 DLL 同目录 / TextureLoader.log。可在 DllMain 调用。
void log_init(const std::wstring &dll_dir);
void log_shutdown();
void log_write(const wchar_t *fmt, ...);

// 可调配置（TextureLoader.ini，跨模块共享；TextureLoader.cpp 读取后赋值）
extern int g_log_level;            // 0=仅关键日志 1=常规（默认） 2=详细
extern int g_vram_threshold_pct;   // 显存压力阈值（%）：可用显存 < 总显存此百分比 → 淘汰
extern uint64_t g_vram_threshold_bytes; // 显存压力阈值（字节）；0=用百分比模式
extern int g_max_texture_side;     // 替换纹理最大边长（像素）；0=不限制
} // namespace tloader

#define TL_LOG(...) ::tloader::log_write(__VA_ARGS__)
#define TL_LOG_IF(level, ...) do { if (::tloader::g_log_level >= (level)) ::tloader::log_write(__VA_ARGS__); } while (0)
