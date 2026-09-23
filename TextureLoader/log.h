// SPDX-License-Identifier: GPL-3.0-or-later
// log.h — 轻量线程安全日志（写 DLL 同目录 TextureLoader.log）
#pragma once
#include <string>
#include <cstdint>

namespace tloader
{
// 记录日志目录（DLL 同目录 / TextureLoader.log）。**可在 DllMain 安全调用** ——
// 它只保存路径，不做文件 I/O（2026-09-19 审核报告：原实现在此直接 fopen，
// 而本函数由 DllMain 调用 → 在 loader lock 持有期间做文件 I/O，属已知死锁模式）。
// 真正的 fopen 推迟到第一次 log_write，那时 DllMain 已返回。
void log_init(const std::wstring &dll_dir);
void log_shutdown();
void log_write(const wchar_t *fmt, ...);

// 崩溃路径专用写入（2026-09-23，审核报告）。
//
// 与 `log_write` 的区别：**绝不阻塞在 `g_mutex` 上**。
// 崩溃可能就发生在持有该锁的代码里（如 `fwprintf`/`fflush` 内部违例、
// 堆损坏）—— 此时崩溃处理器再取同一把**非递归**锁就是**自死锁**，
// 结果是崩溃报告永远写不出来、进程还挂在那里。
// 而这份报告正是定位问题唯一的依据（IAT 写违例就是靠它定案的）。
//
// 策略：`try_lock` 成功 → 照常写 TextureLoader.log（保持既有排查工作流）；
// 失败 → 退回独立句柄 + 独立文件 `TextureLoader.crash.log`，完全不碰锁。
void log_crash_write(const wchar_t *fmt, ...);

// 可调配置（TextureLoader.ini，跨模块共享；TextureLoader.cpp 读取后赋值）
extern int g_log_level;            // 0=仅关键日志 1=常规（默认） 2=详细
extern int g_vram_threshold_pct;   // 显存压力阈值（%）：可用显存 < 总显存此百分比 → 淘汰
extern uint64_t g_vram_threshold_bytes; // 显存压力阈值（字节）；0=用百分比模式
extern int g_max_texture_side;     // 替换纹理最大边长（像素）；0=不限制
extern int g_gdds_enabled;         // 1=启用 GDDS（DirectStorage GPU 解压）；0=禁用（.gdds 跳过）
extern int g_async_load;           // 1=异步加载（后台线程建纹理）；0=同步（渲染线程建纹理，N 卡驱动规避）
extern int g_async_load_explicit;  // 1=用户 ini 显式设置 async_load（不再按 GPU 厂商自动调整）
} // namespace tloader

#define TL_LOG(...) ::tloader::log_write(__VA_ARGS__)
#define TL_LOG_IF(level, ...) do { if (::tloader::g_log_level >= (level)) ::tloader::log_write(__VA_ARGS__); } while (0)
// 崩溃处理器内**必须**用这个，不能用 TL_LOG —— 见 log_crash_write 的说明。
#define TLC_LOG(...) ::tloader::log_crash_write(__VA_ARGS__)
