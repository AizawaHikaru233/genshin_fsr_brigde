#pragma once
// TjTraceProbe.h —— 【诊断探针】侧车文件写入单元（2026-11-23）。
//
// 【它为什么存在】
//   要判的问题：正式版日志里 `transparent_jitter_installed` 26/26 出现，
//   而同一模块的 `transparent_jitter_stats` 0/26 出现。候选成因有六种
//   （shutdown 两次提前返回 / sink 未注册 / 被 blog 等级闸门吞掉 / 入队后没落盘 /
//   CRT atexit 先于 DllMain(DETACH) 跑掉了 blog::shutdown()），**静态读代码区分不了**。
//
// 【为什么不用日志系统】
//   要判的正是日志系统本身（blog::enabled 首行 `if (!g_active) return false`，
//   且 shutdown 之后 writer 也已收尾）—— 用日志观测日志是循环论证：
//   没有输出既可能是"没执行"，也可能是"没通道"，结论必然两义。
//
// 【纪律（全部是硬约束，改本文件前先读这里）】
//   · 只用 kernel32：GetModuleFileNameW / CreateFileW / WriteFile / CloseHandle /
//     GetLocalTime / GetCurrentProcessId / DeleteFileW / GetLastError / OutputDebugStringW；
//   · **不构造 std::string / std::wstring / std::filesystem::path** —— 那要走 CRT 堆，
//     取堆锁。本单元**常态无锁**、不碰 CRT 流；
//     ⚠️ 诚实口径（2026-11-23 / 审核 R5）：**不是**绝对的"不取任何锁" —— 若进程曾调用
//     `setlocale`，UCRT 的 `snprintf` 会取 `__acrt_locale_lock`（阻塞锁）。本仓库零
//     `setlocale` 调用（可 `git grep -n setlocale` 复核），故常态下这条路径不取锁。
//   · 不 Sleep、不等待除"这一次 WriteFile 完成"以外的任何东西 —— 调用点全在
//     DllMain(DLL_PROCESS_DETACH) 与 blog::shutdown() 里，一旦阻塞就是进程退不掉；
//   · ⚠️ 诚实标注（2026-11-23 / 审核 MINOR-4）：**在 DllMain 内做文件 I/O 本身**就是 MSDN 明确
//     不建议的事 —— 一次 WriteFile 仍可能被 AV 实时扫描 / 文件系统过滤驱动 / 网络重定向路径
//     阻塞到任意长。本单元只能保证"不主动 Sleep、不取锁、不等待额外事件"，**不能**保证
//     那一次 I/O 的耗时上界。这是本探针为"拿到落盘级证据"付的已知代价，故它**默认关闭**
//     （见 CMakeLists 的 DX11FSRBRIDGE_TJ_TRACE_PROBE），只在排查时打开；
//   · 暂存用**栈上**定长缓冲（512 B，不分配、不进堆），格式化用 std::snprintf；
//     刻意不用 thread_local（会拉进 CRT 动态 TLS 初始化，退出路径上少一个依赖就少一个未知）；
//   · 失败即返回，并把 phase + GetLastError 追加进 `_tj-trace.err`（第二通道，写失败即静默）。
//     ODS 只在 DX11FSRBRIDGE_ENABLE_DIAGNOSTICS 构建里启用：生产 [Log] to_debugger=0，
//     ODS 看不见，只靠它会让"写失败"与"没走到那一步"不可区分。
//
// 【落点】与 Dx11FsrBridge.log 同目录（即本 DLL 所在目录）、文件名 `_tj-trace.txt`；
//   每次 DLL_PROCESS_ATTACH 先删一次旧文件（文件名固定，不删会把上个进程的行读成本次的）。
//   每行带单调序号 `seq=N`（跨线程 fetch_add relaxed，无锁）：判读时用于定位"中断在第几条之后"。
//   ⚠️ 已知局限：多条 write 之间不构成原子段，进程若在两条之间被 TerminateProcess，
//   只能靠 seq 看出断点，无法证明倒数第二条之后的动作是否发生。
//   行尾统一 \r\n（.txt 与 .err 同一口径）：混用会让按行读的工具把两条记录并成一条。
//
// 【零行为改动】本单元只写文件，不读不写任何既有状态、不改任何返回值。
//   调用方把调用写成**独立语句**插在原语句之间，不改写原语句。

namespace tjtrace
{

#if !defined(DX11FSRBRIDGE_TJ_TRACE_PROBE)
// 2026-11-23（审核 R7）：探针默认**关**（宏 DX11FSRBRIDGE_TJ_TRACE_PROBE 默认 OFF，见 CMakeLists）。
// 关掉时这里给一组**内联空实现**，而不是让调用点去写 #if：
//   · 调用点保持一行一条的形态，判读表与源码对得上（不必在两套形态间切换）；
//   · 空实现是 inline 且全在头里 ⇒ 发布构建里连函数调用都不会生成，更不会有 _tj-trace.txt
//     这个宽字符串进产物（已实测：OFF 产物里 UTF-16 查不到它）。
// 返回值统一 false（"没写成"），但**没有任何调用方检查它** —— 探针本就不改任何行为。
inline void set_module(void *) {}
inline void reset() {}
inline bool write(const char *, const char *, const char *, const char *, const char *,
                  const char *, const char *, ...)
{
    return false;
}
#else

// 注入本 DLL 的模块句柄（HMODULE，用 void * 是为了让本头**不依赖 windows.h**，
// 便于 BridgeLogger.cpp 这种"刻意不 include 桥的头"的 TU 直接包含）。
// 必须在 reset() / write() 之前调用；不调用则退回进程主模块目录（那是游戏 exe 所在目录，
// **不是**日志目录）—— 故 DllMain(ATTACH) 第一行就要调它。
// 为什么不用 GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, ...) 自己查：
// 那要取 loader lock，而本探针的调用点恰在 loader lock 内（DllMain）与进程退出期，
// 多一次锁就是多一条"进程退不掉"的路径。ATTACH 时句柄就在手边，存下来即可。
void set_module(void *module);

// 把本进程（本次会话）的 trace 文件清空。**只**在 DllMain(DLL_PROCESS_ATTACH) 里调一次：
// 与写路径同一纪律（纯 kernel32、不取锁）。在别处调用会抹掉已经积累的行。
void reset();

// 写一行：`<ISO本地时间> stage=<stage> seq=<N> <field=value...> pid=<pid>`（`seq` 见上面【落点】）。
//   stage        —— 必填（为空则不写：没有 stage 的行走不了判读表，写了只会误导）
//   field1..5    —— 可选的 `key=value` 字段，传 nullptr 结束；空串也视为结束。
//   format…      —— 可变参数的 printf 格式（K&R 风格；C++20 下不能省略参数）。
//                   字段名与顺序由调用方就地给出，这样每个探针点的行**自成文档**，
//                   判读表不必回头查调用点。
// 用法示例：
//   tjtrace::write("tj_shutdown_early2", nullptr, nullptr, nullptr, nullptr, nullptr,
//                  "target=%d stub=%d", target_present, stub_present);
// 成功返回 true；任何一步失败返回 false（不抛、不重试、不等待）。
bool write(const char *stage,
           const char *field1, const char *field2, const char *field3,
           const char *field4, const char *field5,
           const char *format, ...);

#endif // DX11FSRBRIDGE_TJ_TRACE_PROBE

} // namespace tjtrace
