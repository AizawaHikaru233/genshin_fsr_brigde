// Dx11FsrBridge/NgxParamGuard.h
//
// NGX 厂商参数袋（NVSDK_NGX_Parameter 虚表）调用的 SEH 守卫 —— header-only。
// 【编号约定】`SDS §x.x` = 本项目设计文档的章节号（§1.1 = 守卫为何独立成 header、§2.2 = inline 与 ODR、
//   §2.2a = 计数口径、§7 = 日志行格式）；`T-n` = 实施批次号。这些编号只为查证方便，
//   每个编号旁的结论都已就地展开，不依赖任何外部文档即可读懂。
// 【背景】本守卫解决的问题已于 2026-11-24 关闭，结论与实测证据见提交
//   b87c859，以及 docs/OPEN-ISSUES.md 的「已解决」节（该文件随仓库分发）。
//
// 为什么守卫必须是独立 header、而不是写在 NgxBackend.cpp 的匿名 namespace 里：
//   探针（probe/）必须 include 同一份守卫，才能证明生产代码真的接住 AV。若守卫只存在于
//   .cpp，探针只能复制一份逻辑去测 —— 测到的不是生产代码，本仓库把这种情形视为假验证
//   （SDS §1.1；§12 已把「守卫写在 NgxBackend.cpp 内」列为否决方案）。
//   代价：每个定义都必须 inline，否则 NgxBackend.cpp 与探针两个 TU 同时 include 会 LNK2005
//   （SDS §2.2）。
//
// 为什么每个包装都 __declspec(noinline) 且只收 POD：
//   MSVC 不允许「需要 C++ 栈展开」的函数使用 __try/__except（C2712）。因此包装器体内不得出现
//   std::string / std::function / 任何带析构的对象，出参只能是 POD 的 NgxGuardResult*（SDS S1）。
//   E1：原型头三构型（/O2 无 /EHsc、/O2 有 /EHsc、/Od）各 0 错误 0 警告。
//
// 为什么 __declspec 写在返回类型之前：
//   写成 f(...) __declspec(noinline); （即写在声明符之后）会报 C2144 语法错误 —— 已用
//   MSVC 实编确认。本文件的 9 个包装 + 3 个访问器全部采用「前缀」形式，改这一行前请先读 SDS §2.2a。
//
// 为什么包装器里不打日志、不拼串：
//   log_ngx_core / log_ngx_error 在 NgxBackend.cpp 的匿名 namespace 内（整段都在其中，无固定行号可引），独立 header
//   根本调不到；而且拼串会产生 std::string，直接违反上面的 C2712 约束（SDS S5）。
//   日志一律由调用点侧在包装器返回之后做（那时已离开 __try）。
//
// 2026-11-24 新建（T1）。证据等级：V1/V2/V3 见本轮报告；实机（E2）表现未验证。
// 与原型 probe/ngx_param_guard_proto.h 的差异：按 SDS §11.1 的 7 条全部应用（命名空间化、
// 去掉 extern "C"、补判空、复位函数改名并包进宏、NGX 头路径、新增第 9 个回调包装、
// 新增第三个计数器 g_vt_guard_skips）。原型只有 8 个包装，缺 guarded_optimal_cb。

#pragma once

#include <atomic>

#include <windows.h> // EXCEPTION_EXECUTE_HANDLER / GetExceptionCode

// 走真实厂商头，不本地复刻（SDS §11.1 第 5 条：路径相对 Dx11FsrBridge/）。
// PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback 由 nvsdk_ngx_defs.h:550 定义，params.h 已 include 它。
#include "third_party/ngx/nvsdk_ngx_params.h"

// 出参：严格 POD（4 个标量，无构造/析构），可以安全穿过 SEH 边界，也不触发 C2712。
struct NgxGuardResult
{
    unsigned int ex_code; // 0 = 没进过 __except；否则是 GetExceptionCode()
    bool tripped;         // 本次真的进了 __try 并被 __except 接住
    bool skipped;         // 粘滞位已置，本次根本没进 __try
    bool first_trip;      // 本次是首次置位粘滞位（exchange 返回的旧值为 false）

    // 为什么 tripped 与 skipped 必须是两个字段（SDS S4）：
    //   二者是不同事实 ——「又崩了一次」与「这次被粘滞位挡下」。（AGENTS.md §五.1：
    //   「暂时没在画」与「已经没了」不能合并成一条判据。）合成一个 bool 后，日志就再也分不清
    //   是厂商实现还在崩，还是它早已被判死。
    //
    // 为什么要有 first_trip（审核 MAJOR-6）：
    //   SDS §7 的「降级状态行」要求在置位那一刻打一条 ngx_vt_guard state 行，且只打一条。
    //   若置位用 store(true)，调用方拿不到旧值 ⇒ 结构性无法判断「本次是否首次」。
    //   故统一用 exchange(true) 并把「旧值取反」回填到这里。
};

namespace ngxguard
{
// 三个进程级状态（SDS §3）。C++17 inline 变量，header-only 不违反 ODR。
// 为什么是进程级：g_params 本身就是进程级单例（NgxBackend.cpp）。
// 为什么不随 shutdown_all() 清理：粘滞位描述的是「厂商参数袋实现已不可信」，不是设备状态；
//   清掉只会在下一帧重踩同一个 AV（SDS §12 已否决「shutdown_all() 时清」）。
// 常量初始化 ⇒ 都在 .data、初值 0，没有 ??__E 动态初始化器（E1 原型已验证），
//   因此不存在静态初始化顺序问题。
// 为什么放在 namespace ngxguard：桥 DLL 里已经有大量 g_ 名字，裸的 g_vt_guard_tripped
//   有撞名风险（原型验证时发现的真实风险）；namespace 不改名、只加作用域。
inline std::atomic<bool> g_vt_guard_tripped{false};         // 粘滞：置位后短路
inline std::atomic<unsigned long long> g_vt_guard_trips{0};  // 累计 __except 命中次数
inline std::atomic<unsigned long long> g_vt_guard_skips{0};  // 累计短路次数

// 包装器总数：单一事实源（AGENTS.md §五.2）。SDS §7 的 ngx_vt_guard armed 行必须引用它，
// 不许在日志里写字面量 9 —— 否则将来加/减包装时，日志会静默对不上代码。
inline constexpr unsigned kGuardWrapperCount = 9; // 8 个虚表方法类型 + 1 个厂商回调

// 只读访问器（SDS §2.3）。全部 inline：多个 TU 包含同一 header 时不会 LNK2005。
// relaxed：这些值只用于日志与早退判断，不参与任何「用计数同步两件事」的协议。
inline bool ngx_guard_tripped()
{
    return g_vt_guard_tripped.load(std::memory_order_relaxed);
}

inline unsigned long long ngx_guard_trip_count()
{
    return g_vt_guard_trips.load(std::memory_order_relaxed);
}

inline unsigned long long ngx_guard_skip_count()
{
    return g_vt_guard_skips.load(std::memory_order_relaxed);
}

#if defined(DX11FSRBRIDGE_GUARD_PROBE)
// 仅供探针，生产构建里根本不编译（SDS §2.3）：生产二进制里不能存在一个「能把粘滞位清掉」
// 的入口 —— 粘滞位一旦置位必须永久粘住（SDS §4：单向、不可逆）。
// 探针需要它，是因为「9 个包装各自的崩溃路径」必须逐个从干净状态开始；不 reset 的话第二个
// 起全是 skipped，V2 会假通过（SDS §11.2 步骤 5）。
inline void ngx_guard_reset_for_probe()
{
    g_vt_guard_trips.store(0, std::memory_order_relaxed);
    g_vt_guard_skips.store(0, std::memory_order_relaxed);
    g_vt_guard_tripped.store(false, std::memory_order_relaxed);
}
#endif

// ---------------------------------------------------------------------------------------
// 9 个包装。前 8 个对应 params.h:55-71 的 8 类虚表方法（Set/Get 各 4 个具名重载），
// 第 9 个是厂商回调，见本节末尾。
//
// 禁止按 vtable 槽号实现或断言（SDS S10 / R8）：E1 实测 MSVC x64 上槽序与声明顺序相反
//   （Set(void*)→0 … Set(unsigned long long)→7）。本文件一律用具名成员函数，由编译器自己选槽。
//
// 返回码约定：Get 侧失败统一返回 NVSDK_NGX_Result_Fail，不新造结果码 —— 不给上游 NGX 结果码
//   语义添新值。**失败原因由调用点读 `gr->tripped` 区分**（真实异常）；
//   `gr->skipped` 表示「粘滞位已置、本次根本没进 __try」，生产代码**不读它**做分支 ——
//   它只是诊断字段（日志宏按 tripped/first_trip 决定打不打行），不要把它当调用点的判据。
//   Set 侧在 params.h 里全部返回 void ⇒ 唯一的失败通道就是 gr，故 gr 必须回写。
//
// 计数口径（SDS §2.2a，三条互相约束）：
//   · 正常（未命中、未短路）路径零原子 RMW：入口只做一次 relaxed load，x64 上是普通 mov，
//     没有 LOCK 前缀（E1 原型 C1−C0 = +0.014 ns，噪声内）。
//   · g_vt_guard_trips 只在 __except 命中时 fetch_add。
//   · g_vt_guard_skips 只在粘滞已置而短路时 fetch_add —— 不是零原子：短路要付 1 次 relaxed RMW。
//     这不算热路径违规（AGENTS.md §二.5 防的是「每帧 16 次无条件原子写」），因为短路只发生在
//     已经是故障态之后（SDS S3 明写：短路省掉的是 SEH 分派与厂商代码执行，不是原子操作）。
//
// 边界条件（SDS §6）：
//   · param == nullptr / cb == nullptr ⇒ 返回失败、ex_code=0、不置粘滞。
//     空指针是「能力缺失」，不是「厂商崩溃」；若让它进 __try，ex_code 会是 0xC0000005，
//     日志就会把它报成厂商 DLL AV —— 那是错误归因（与既有 guarded_get_caps 的纪律一致：
//     看 NgxBackend.cpp 里 `guarded_get_caps` 的 `caps_crash != 0` 判空分支）。
//     这一点与原型不同：原型没有判空分支（SDS §11.1 第 3 条）。
//   · 粘滞已置 ⇒ skips.fetch_add(1)、skipped=true、不进 __try，返回失败。
//   · Get 返回非 Success ⇒ 原样返回该 rc、ex_code=0、不置粘滞（SDS S9）：
//     那是「参数不存在」，不是「厂商崩了」；混为一谈会让一次正常的 Fail 锁死整个后端。
//
// 判空与粘滞的先后（按 SDS §4 的数据流：先粘滞、再判空）：两者都返回失败且都不置粘滞，
//   故顺序不改变结果；唯一的差别是「粘滞后仍拿空指针来调」这一次会不会被计入 skips。
//
// 已知语义变化（SDS S8 / R7，必须如实记录）：本守卫会吞掉厂商抛出的 C++ 异常
//   （ex_code = 0xE06D7363）。生产 Release 构建没有 /EHsc（Dx11FsrBridge/CMakeLists.txt:250
//   只给 Debug 加），改动前这种异常没有任何人接住 ⇒ 直接 terminate；改动后变成一次可归因的失败。
//   方向上可接受，但确实是行为变化。
//   与它相对：ex_code = 0xC0000005 是厂商实现的访问违例（vtable 被打坏、句柄失效后仍被调用），
//   两者要分开看 —— 前者是厂商主动抛，后者是厂商踩空。
//
// 未验证（诚实记录）：
//   · 打进桥 DLL 之后、真实 NGX 句柄损坏时的实际表现：未验证（需实机 E2）。
//   · 厂商是否依赖「异常能传到调用方」或「守卫式重试」：未验证（仅代码阅读，无实证）。
// ---------------------------------------------------------------------------------------

// ---- Get 侧（返回 NGX 结果码）----

inline __declspec(noinline) NVSDK_NGX_Result get_u32(NVSDK_NGX_Parameter *param, const char *name, unsigned int *out,
                                                     NgxGuardResult *gr)
{
    if (gr != nullptr)
    {
        gr->ex_code = 0u;
        gr->tripped = false;
        gr->skipped = false;
        gr->first_trip = false;
    }
    if (g_vt_guard_tripped.load(std::memory_order_relaxed))
    {
        g_vt_guard_skips.fetch_add(1, std::memory_order_relaxed);
        if (gr != nullptr)
            gr->skipped = true;
        return NVSDK_NGX_Result_Fail;
    }
    if (param == nullptr)
        return NVSDK_NGX_Result_Fail;
    __try
    {
        // 具名重载 ⇒ 由编译器选槽（params.h:67 Get(const char*, unsigned int*)）。
        return param->Get(name, out);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_vt_guard_trips.fetch_add(1, std::memory_order_relaxed);
        const bool was_tripped = g_vt_guard_tripped.exchange(true, std::memory_order_relaxed);
        if (gr != nullptr)
        {
            gr->ex_code = static_cast<unsigned int>(GetExceptionCode());
            gr->tripped = true;
            gr->first_trip = !was_tripped;
        }
        return NVSDK_NGX_Result_Fail;
    }
}

inline __declspec(noinline) NVSDK_NGX_Result get_i32(NVSDK_NGX_Parameter *param, const char *name, int *out,
                                                     NgxGuardResult *gr)
{
    if (gr != nullptr)
    {
        gr->ex_code = 0u;
        gr->tripped = false;
        gr->skipped = false;
        gr->first_trip = false;
    }
    if (g_vt_guard_tripped.load(std::memory_order_relaxed))
    {
        g_vt_guard_skips.fetch_add(1, std::memory_order_relaxed);
        if (gr != nullptr)
            gr->skipped = true;
        return NVSDK_NGX_Result_Fail;
    }
    if (param == nullptr)
        return NVSDK_NGX_Result_Fail;
    __try
    {
        // params.h:68 Get(const char*, int*)。不要因为「看起来一样」改走 u32 重载：
        // NgxBackend.cpp 里 `create_feature` 对 PerfQualityValue 的**写入**，与
        //   `diag_check_create_params` 的**回读校验**必须同源（同类型 ⇒ 同重载），
        //   否则回读会比出假 mismatch（SDS §2.2b）。两处都用 get_i32/set_i32（int）。
        return param->Get(name, out);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_vt_guard_trips.fetch_add(1, std::memory_order_relaxed);
        const bool was_tripped = g_vt_guard_tripped.exchange(true, std::memory_order_relaxed);
        if (gr != nullptr)
        {
            gr->ex_code = static_cast<unsigned int>(GetExceptionCode());
            gr->tripped = true;
            gr->first_trip = !was_tripped;
        }
        return NVSDK_NGX_Result_Fail;
    }
}

inline __declspec(noinline) NVSDK_NGX_Result get_f32(NVSDK_NGX_Parameter *param, const char *name, float *out,
                                                     NgxGuardResult *gr)
{
    if (gr != nullptr)
    {
        gr->ex_code = 0u;
        gr->tripped = false;
        gr->skipped = false;
        gr->first_trip = false;
    }
    if (g_vt_guard_tripped.load(std::memory_order_relaxed))
    {
        g_vt_guard_skips.fetch_add(1, std::memory_order_relaxed);
        if (gr != nullptr)
            gr->skipped = true;
        return NVSDK_NGX_Result_Fail;
    }
    if (param == nullptr)
        return NVSDK_NGX_Result_Fail;
    __try
    {
        // params.h:65 Get(const char*, float*)
        return param->Get(name, out);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_vt_guard_trips.fetch_add(1, std::memory_order_relaxed);
        const bool was_tripped = g_vt_guard_tripped.exchange(true, std::memory_order_relaxed);
        if (gr != nullptr)
        {
            gr->ex_code = static_cast<unsigned int>(GetExceptionCode());
            gr->tripped = true;
            gr->first_trip = !was_tripped;
        }
        return NVSDK_NGX_Result_Fail;
    }
}

inline __declspec(noinline) NVSDK_NGX_Result get_ptr(NVSDK_NGX_Parameter *param, const char *name, void **out,
                                                     NgxGuardResult *gr)
{
    if (gr != nullptr)
    {
        gr->ex_code = 0u;
        gr->tripped = false;
        gr->skipped = false;
        gr->first_trip = false;
    }
    if (g_vt_guard_tripped.load(std::memory_order_relaxed))
    {
        g_vt_guard_skips.fetch_add(1, std::memory_order_relaxed);
        if (gr != nullptr)
            gr->skipped = true;
        return NVSDK_NGX_Result_Fail;
    }
    if (param == nullptr)
        return NVSDK_NGX_Result_Fail;
    __try
    {
        // params.h:71 Get(const char*, void**)。NgxBackend.cpp 的 `query_dlss_optimal_range`
        //   用它在取 DLSS 最优设置回调指针，随后交给下面的 guarded_optimal_cb 调用。
        return param->Get(name, out);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_vt_guard_trips.fetch_add(1, std::memory_order_relaxed);
        const bool was_tripped = g_vt_guard_tripped.exchange(true, std::memory_order_relaxed);
        if (gr != nullptr)
        {
            gr->ex_code = static_cast<unsigned int>(GetExceptionCode());
            gr->tripped = true;
            gr->first_trip = !was_tripped;
        }
        return NVSDK_NGX_Result_Fail;
    }
}

// ---- Set 侧（返回 void）----

inline __declspec(noinline) void set_u32(NVSDK_NGX_Parameter *param, const char *name, unsigned int value,
                                         NgxGuardResult *gr)
{
    if (gr != nullptr)
    {
        gr->ex_code = 0u;
        gr->tripped = false;
        gr->skipped = false;
        gr->first_trip = false;
    }
    if (g_vt_guard_tripped.load(std::memory_order_relaxed))
    {
        g_vt_guard_skips.fetch_add(1, std::memory_order_relaxed);
        if (gr != nullptr)
            gr->skipped = true;
        return;
    }
    if (param == nullptr)
        return;
    __try
    {
        // params.h:58 Set(const char*, unsigned int)
        param->Set(name, value);
        return;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_vt_guard_trips.fetch_add(1, std::memory_order_relaxed);
        const bool was_tripped = g_vt_guard_tripped.exchange(true, std::memory_order_relaxed);
        if (gr != nullptr)
        {
            gr->ex_code = static_cast<unsigned int>(GetExceptionCode());
            gr->tripped = true;
            gr->first_trip = !was_tripped;
        }
    }
}

inline __declspec(noinline) void set_i32(NVSDK_NGX_Parameter *param, const char *name, int value, NgxGuardResult *gr)
{
    if (gr != nullptr)
    {
        gr->ex_code = 0u;
        gr->tripped = false;
        gr->skipped = false;
        gr->first_trip = false;
    }
    if (g_vt_guard_tripped.load(std::memory_order_relaxed))
    {
        g_vt_guard_skips.fetch_add(1, std::memory_order_relaxed);
        if (gr != nullptr)
            gr->skipped = true;
        return;
    }
    if (param == nullptr)
        return;
    __try
    {
        // params.h:59 Set(const char*, int)。别把 int 实参走成 unsigned int 重载：
        // SDS §2.2b 的裁决 —— int 会隐式转 unsigned，静默调用 :58 而非 :59，且编译零告警。
        param->Set(name, value);
        return;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_vt_guard_trips.fetch_add(1, std::memory_order_relaxed);
        const bool was_tripped = g_vt_guard_tripped.exchange(true, std::memory_order_relaxed);
        if (gr != nullptr)
        {
            gr->ex_code = static_cast<unsigned int>(GetExceptionCode());
            gr->tripped = true;
            gr->first_trip = !was_tripped;
        }
    }
}

inline __declspec(noinline) void set_f32(NVSDK_NGX_Parameter *param, const char *name, float value, NgxGuardResult *gr)
{
    if (gr != nullptr)
    {
        gr->ex_code = 0u;
        gr->tripped = false;
        gr->skipped = false;
        gr->first_trip = false;
    }
    if (g_vt_guard_tripped.load(std::memory_order_relaxed))
    {
        g_vt_guard_skips.fetch_add(1, std::memory_order_relaxed);
        if (gr != nullptr)
            gr->skipped = true;
        return;
    }
    if (param == nullptr)
        return;
    __try
    {
        // params.h:56 Set(const char*, float)
        param->Set(name, value);
        return;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_vt_guard_trips.fetch_add(1, std::memory_order_relaxed);
        const bool was_tripped = g_vt_guard_tripped.exchange(true, std::memory_order_relaxed);
        if (gr != nullptr)
        {
            gr->ex_code = static_cast<unsigned int>(GetExceptionCode());
            gr->tripped = true;
            gr->first_trip = !was_tripped;
        }
    }
}

// set_tex 收 ID3D11Resource*（params.h:60），不是 ID3D11Texture2D*：
//   NgxBackend.cpp 里那个同名 inline 外壳只做 static_cast 转发（SDS §2.4 定死的做法），
//   真正的虚表调用只在这里，否则「旧形态残留 = 0」那条自检判据不成立。
inline __declspec(noinline) void set_tex(NVSDK_NGX_Parameter *param, const char *name, ID3D11Resource *value,
                                         NgxGuardResult *gr)
{
    if (gr != nullptr)
    {
        gr->ex_code = 0u;
        gr->tripped = false;
        gr->skipped = false;
        gr->first_trip = false;
    }
    if (g_vt_guard_tripped.load(std::memory_order_relaxed))
    {
        g_vt_guard_skips.fetch_add(1, std::memory_order_relaxed);
        if (gr != nullptr)
            gr->skipped = true;
        return;
    }
    if (param == nullptr)
        return;
    __try
    {
        param->Set(name, value);
        return;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_vt_guard_trips.fetch_add(1, std::memory_order_relaxed);
        const bool was_tripped = g_vt_guard_tripped.exchange(true, std::memory_order_relaxed);
        if (gr != nullptr)
        {
            gr->ex_code = static_cast<unsigned int>(GetExceptionCode());
            gr->tripped = true;
            gr->first_trip = !was_tripped;
        }
    }
}

// ---- 第 9 个：厂商回调 ----
//
// 为什么需要它（SDS §2.2a）：NgxBackend.cpp 的 `query_dlss_optimal_range` 里那个 cb(p) 是厂商函数指针，
//   与虚表调用同类 —— 它自己也会消耗 p 并回调 p->Set/Get。v1 的 SDS 漏掉了它，等于静默缩小范围。
// 嵌套语义（2026-11-24 订正）：回调内部那句 `p->Set/Get` **不是**我们的包装，
//   而是厂商自己的裸虚调（`ngxguard::*` 是自由函数，从未装进 NVSDK_NGX_Parameter 虚表）
//   ⇒ **不存在「内层先命中」**：回调内的 AV 传回本包装的 `__except`，`trips` **只 +1 一次**。
//   故「同一次事故」不会重复计数；`total` 的语义是「累计命中次数」，不同帧各命中一次就各计一次
//   —— 每次都是一次真实的 `__except`（SDS S7 / §2.2a）。
// PFN_ 是函数指针（POD），可以安全穿过 SEH 边界。收 void* 再在这里转也行，但让调用点先转好
//   类型更干脆（调用点已有 const auto cb = reinterpret_cast<...>(callback);）。
inline __declspec(noinline) NVSDK_NGX_Result guarded_optimal_cb(PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback cb,
                                                               NVSDK_NGX_Parameter *param, NgxGuardResult *gr)
{
    if (gr != nullptr)
    {
        gr->ex_code = 0u;
        gr->tripped = false;
        gr->skipped = false;
        gr->first_trip = false;
    }
    if (g_vt_guard_tripped.load(std::memory_order_relaxed))
    {
        g_vt_guard_skips.fetch_add(1, std::memory_order_relaxed);
        if (gr != nullptr)
            gr->skipped = true;
        return NVSDK_NGX_Result_Fail;
    }
    // cb 与 param 都要判：空 cb 是「厂商没给回调」，空 param 是「参数袋不可用」，
    //   两者都是确定的能力缺失，而不是厂商崩溃（SDS §2.2a 的空判条 + §6 首行）。
    if (cb == nullptr || param == nullptr)
        return NVSDK_NGX_Result_Fail;
    __try
    {
        return cb(param);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_vt_guard_trips.fetch_add(1, std::memory_order_relaxed);
        const bool was_tripped = g_vt_guard_tripped.exchange(true, std::memory_order_relaxed);
        if (gr != nullptr)
        {
            gr->ex_code = static_cast<unsigned int>(GetExceptionCode());
            gr->tripped = true;
            gr->first_trip = !was_tripped;
        }
        return NVSDK_NGX_Result_Fail;
    }
}

} // namespace ngxguard
