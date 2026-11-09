// NgxBackend.cpp —— D3D11 NGX（DLSS Super Resolution）后端实现。
//
// 【T-2 阶段状态】本文件实现「初始化链路」（SDS §3.4 / §3.9A.1 / §3.10.2 / §3.11.3 / §3.13）：
//   ① §3.3.4.1 struct NgxInstance —— **只有字段声明**，资源池由 T-4 建（本任务不创建任何 GPU 资源）
//   ② §3.3.4.2 全局状态块 —— 除 g_release_all / g_logged_config / g_ngx_log_count 外**都是普通变量**
//      （T-1 曾把配置项写成 std::atomic，T-2 按 SDS 改回普通 bool/int）
//   ③ §3.4.1 qpc_us()（含零频守卫）+ 幂等契约 + 5 秒失败退避窗口
//   ④ §3.4.6 十个函数指针 typedef（逐字照抄）+ §3.3.4.2 的十个全局函数指针
//   ⑤ §3.4.2 ensure_init() 的十一步（第 0 步配置快照 + 第 1..10 步）
//   ⑥ §3.10.2 配置快照五行（顺序固定）+ §3.13 的 config_warn 尾行
//   ⑦ §3.9A.1 log_ngx_core / log_ngx_error / flags_to_string / warn_to_string
//   ⑧ §3.11.3 ngx_log_cb（含 200 条硬限流）
//   ⑨ §3.13 的 8 个 setter 语义（只存值；非法值回退并置 g_config_warn 位）
//
// 【T-2 时仍然不做什么】dispatch() 当时只落地 §3.7 的第 0 步与第 1 步（+ ensure_init 调用），
//   在 ensure_init 之后**一律 return false** ⇒ 调用方走原生放行（fail-open，D9）。
//   ★ T-5 已补齐第 2/2b 步与第 4-11 步（见下），本段历史说明保留以便追溯。
//
// 【T-5 阶段状态】dispatch 十二步全部接线 + shutdown_all 完整实现：
//   ① shutdown_all()（§3.9A.1 只给了签名，函数体按 §3.7 第 2 步 / §3.9.7 共同纪律逐条落地）
//   ② dispatch 的第 2 / 2b / 4-11 步（第 0 / 1 / 3 步 T-2 已有；第 3 步的 dev->Release()
//      按 SDS 挪到第 4 步末尾 —— 第 4 步建池还要用 dev）
//   ③ 帧计数 g_ngx_frame_count + 三个日志闸门（§3.10.1 的 ngx_in / ngx_out / ngx_core eval）
//   ④ §4.2 第 18/19/20 行的诊断抽检（全部包在 DX11FSRBRIDGE_ENABLE_DIAGNOSTICS 内）
//
// 【T-3 阶段状态】新增五个匿名命名空间内的函数（定义顺序即依赖顺序）：
//   set_tex（§3.9.6）/ build_create_flags（§3.8.2）/ quality_from_ratio（§3.8.3）/
//   apply_render_preset（§3.8.3）/ create_feature（§3.8.1，含授权的偏离 1 / 2 / 3）。
//   本阶段仍不出图：dispatch() 里的 TODO(T-5) 一字未动。

// ---- ① 标准库 ----
#include <atomic>         // std::atomic（g_release_all / g_logged_config / g_ngx_log_count）
#include <cstdarg>        // va_list / va_start / va_end（log_ngx_* 的 varargs）
#include <cstdio>         // _vsnwprintf_s / std::snprintf
#include <cstdlib>        // std::wcstoull 兜底
#include <cstring>        // memcpy / strlen
#include <limits>         // std::numeric_limits<float>::quiet_NaN()（cb_uploaded_* 初值）
#include <string>         // std::wstring / std::string（flags_to_string / warn_to_string）
#include <unordered_map>  // 仅日志/诊断辅助；实例本身是全局单例（v8）
#include <utility>        // std::move
#include <vector>         // g_feature_dirs / g_feature_dir_ptrs

// ---- ② Windows / D3D11（必须先于 NGX 头）----
#include <d3d11.h>        // ID3D11Device / ID3D11DeviceContext / ID3D11Texture2D
#include <dxgi.h>         // IDXGIAdapter / IDXGIDevice（GetFeatureRequirements 反查）
#include <d3dcompiler.h>  // D3DCompile（两个自写 CS）
#include <shlobj.h>       // ★ T-2 增（§3.4.4）：SHGetKnownFolderPath / FOLDERID_LocalAppData
#include <wrl/client.h>   // Microsoft::WRL::ComPtr

// ---- ③ 本项目 ----
#include "BridgeLogger.h" // ★★ blog::Level / blog::write / blog::cat（S5：漏它 ⇒ 39 条 error）
#include "Ffx12Backend.h" // ffx12::FrameInput / ffx12::backend()
#include "Ffx12ShaderSource.h" // g_depth_extract_hlsl / g_motion_decode_d11_hlsl
#include "NgxBackend.h"

// ---- ④ NGX（必须最后；且只这两个）----
#include "third_party/ngx/nvsdk_ngx_defs.h"
#include "third_party/ngx/nvsdk_ngx_params.h"

// ---- ⑤ 系统库（T-2 增）----
// 2026-11-09（PR #12 审查）：原注释写「不改 CMakeLists.txt，它已冻结」，但本 PR
//   确实改了 CMakeLists.txt（加 NGX 开关、加 user32）。保留 #pragma comment 而非
//   改链接行的理由**不是**「CMakeLists 不能动」，而是：这三行只在
//   DX11FSRBRIDGE_ENABLE_NGX 打开时生效，不需要污染 CMake 的全局链接列表。
// 三行等价于在链接行加同名 .lib，实测可用，且不改变任何既有 target 配置：
//   advapi32 → RegGetValueW（§4.2 第 1 行）；ole32 + shell32 → SHGetKnownFolderPath（§3.4.4）
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

// ★ 本项目 Ffx12Backend.cpp:34 有 using namespace Microsoft::WRL; 的写法，
//   本 TU 不照抄，而是显式写一行（否则裸 ComPtr 处报 C7568：
//   "ComPtr": 后面跟 "::" 的标识符必须是类或命名空间名）：
using Microsoft::WRL::ComPtr;

namespace ngx {

namespace {

// ============================================================================
// §3.4.6 十个函数指针 typedef（**逐字照抄，不得改**）
// ★ 粘贴顺序：typedef 必须先于 §3.3.4.2 的十个全局函数指针变量，否则 C2065。
// ============================================================================
#define NGX_CALL __cdecl   // x64 下无影响；显式写与头文件语义一致

using PFN_NGX_Init_Ext =
    NVSDK_NGX_Result (NGX_CALL *)(unsigned long long, const wchar_t *,
                                  ID3D11Device *, NVSDK_NGX_Version,
                                  const NVSDK_NGX_FeatureCommonInfo *);   // ★ 第 5 参是 fc，不是 Parameter

using PFN_NGX_GetCapabilityParameters =
    NVSDK_NGX_Result (NGX_CALL *)(NVSDK_NGX_Parameter **);

using PFN_NGX_AllocateParameters =
    NVSDK_NGX_Result (NGX_CALL *)(NVSDK_NGX_Parameter **);

using PFN_NGX_DestroyParameters =
    NVSDK_NGX_Result (NGX_CALL *)(NVSDK_NGX_Parameter *);

using PFN_NGX_GetScratchBufferSize =
    NVSDK_NGX_Result (NGX_CALL *)(NVSDK_NGX_Feature, const NVSDK_NGX_Parameter *, size_t *);

using PFN_NGX_CreateFeature =
    NVSDK_NGX_Result (NGX_CALL *)(ID3D11DeviceContext *, NVSDK_NGX_Feature,
                                  NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);   // ★ 非 const 形态

using PFN_NGX_ReleaseFeature =
    NVSDK_NGX_Result (NGX_CALL *)(NVSDK_NGX_Handle *);

using PFN_NGX_EvaluateFeature =
    NVSDK_NGX_Result (NGX_CALL *)(ID3D11DeviceContext *, const NVSDK_NGX_Handle *,
                                  const NVSDK_NGX_Parameter *,
                                  PFN_NVSDK_NGX_ProgressCallback);   // 第 4 参恒传 nullptr

using PFN_NGX_Shutdown1 =
    NVSDK_NGX_Result (NGX_CALL *)(ID3D11Device *);

using PFN_NGX_GetFeatureRequirements =
    NVSDK_NGX_Result (NGX_CALL *)(IDXGIAdapter *,
                                  const NVSDK_NGX_FeatureDiscoveryInfo *,
                                  NVSDK_NGX_FeatureRequirement *);

// ============================================================================
// §3.4.1 的 qpc_us()：**必须在 NgxBackend.cpp 里自己定义一份**
// Ffx12Backend.cpp:136 的同名函数在匿名命名空间内（内部链接），本 TU 根本调不到
// （照抄即 C3861）。8 行与 Ffx12Backend.cpp:136-145 逐字同形（含零频守卫），
// 同一口径是为了让 §7.1-6 的 p95/p99 两侧可比。
// ============================================================================
static std::uint64_t qpc_us()
{
    LARGE_INTEGER f {}, c {};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return f.QuadPart
        ? static_cast<std::uint64_t>(c.QuadPart * 1000000.0 / static_cast<double>(f.QuadPart))
        : 0;   // 零频守卫，与 Ffx12Backend.cpp:142 的三元式一致
}

// ============================================================================
// §3.3.4.1 struct NgxInstance（本 TU 内，**不对外暴露**）
// T-2 只需要字段存在：资源池 / feature 句柄的实际创建在 T-4。
// ============================================================================
struct NgxInstance
{
    // ---- 身份与尺寸 ----
    std::uint64_t key            = 0;      // instance_key；与传入值不同即触发切换重建。**0 是合法值**（B-7）
    std::uint32_t rw = 0, rh = 0;          // 建池时的 render 尺寸
    std::uint32_t dw = 0, dh = 0;          // 建池时的 display 尺寸
    DXGI_FORMAT   color_fmt  = DXGI_FORMAT_UNKNOWN;   // = in.color 的实际格式（TYPELESS）
    DXGI_FORMAT   output_fmt = DXGI_FORMAT_UNKNOWN;   // = in.output_target 的实际格式（TYPELESS）

    // ---- 自建资源池（5 张纹理，全部 MiscFlags=0，见 §3.12.4）----
    // ★ T-2 只声明字段、**不创建任何资源**（资源池是 T-4 的交付物）。
    Microsoft::WRL::ComPtr<ID3D11Texture2D> color;     // render 尺寸，绑定 SRV（两条 CS 的 t0 来源）
    Microsoft::WRL::ComPtr<ID3D11Texture2D> depth;     // render 尺寸，绑定 UAV（深度 CS 的输出）
    Microsoft::WRL::ComPtr<ID3D11Texture2D> motion;    // render 尺寸，绑定 UAV（motion CS 的 u0）
    Microsoft::WRL::ComPtr<ID3D11Texture2D> reactive;  // render 尺寸 R8_UNORM，绑定 UAV（motion CS 的 u1）
    Microsoft::WRL::ComPtr<ID3D11Texture2D> output;    // **display** 尺寸，**必须含 D3D11_BIND_UNORDERED_ACCESS**（F-47b）
                                                       // 且**保持 TYPELESS**（= output_fmt），否则 CopyResource 会失败

    // ---- 池内视图（仅供我们自己的 CS 绑定；**不进 NGX 参数**，见 §3.9.6 的 set_tex 强制）----
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>  color_srv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> depth_uav;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> motion_uav;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> reactive_uav;
    // **没有 output_uav / depth_srv / motion_srv / reactive_srv**：v1 不用，不要加。

    // ---- 输入侧 SRV（§3.6.4）----
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depth_src_srv;   // 深度 CS 的 t0，格式三分支
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> motion_src_srv;  // motion CS 的 t0，固定 R10G10B10A2_UNORM
    ID3D11Texture2D *depth_src_last  = nullptr;   // ★ 裸指针，**不是 ComPtr**（只做身份比对，**不做 Release**）
    ID3D11Texture2D *motion_src_last = nullptr;   // ★ 同上。只做身份比对，**不做 Release**（纹理归游戏）

    // ---- b0 常数缓冲（16 B，两条 CS 共用）----
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb;
    float cb_uploaded_depth = std::numeric_limits<float>::quiet_NaN();  // 缓存判据（照 Ffx12Backend.cpp:1680-1683）
    float cb_uploaded_flip  = std::numeric_limits<float>::quiet_NaN();

    // ---- NGX feature ----
    NVSDK_NGX_Handle *feature = nullptr;   // CreateFeature 的句柄；**实例切换销毁前必须 ReleaseFeature**（v8：无驱逐）
    int  feature_preset       = 0;         // **仅日志与 Q18 对账用，不在重建判据内**（ini 无热重载 ⇒ 运行期不变）
    // ★ v7 改（S-81）：**同一对象生命周期内永不清除**。尺寸/格式变化只释放资源、不重试 CreateFeature。
    //   只有设备更换/丢失触发 shutdown_all() 时随实例表整体销毁 —— 那是"对象没了"，不是"清锁重试"。
    bool feature_create_failed = false;

    // ---- T-8 修复（M4-a）：本次 CreateFeature **实际**使用的 CreateFlags 可变输入快照 ----
    // 为什么必须记录而不是每帧重算后直接用：
    //   ① Create_Flags 只在 create_feature 时写进 g_params，EvaluateFeature **不会**重算它；
    //   ② 所以「本次 feature 是按哪一组输入建的」这个事实只能由 create_feature 当场记下。
    //
    // ★ 形态选择：**打包成一个 unsigned 位图**，而不是若干个独立 bool 对。
    //   理由：比较退化成一次整数不等，且新增一个可变输入只需占用一个 bit、
    //   并在 flags_input_bits() 里加一行 —— 不会出现「加了字段却忘了加比较」的漏项。
    //   本项目最怕的正是这种静默失效（改了一处、另一处没跟上，编译期零告警）。
    //   位定义见 flags_input_bits()：bit0=IsHDR / bit1=DepthInverted(T-9) / bit2=MVJittered。
    //
    // ★ feature_flags_valid 区分「从没建过 feature」与「建过、且当时的位图是 0」——
    //   没有它，首次 dispatch 会把 bits=0 误判成"值变了"从而多走一次无意义的重建。
    unsigned feature_flags_bits  = 0;
    bool     feature_flags_valid = false;

    // ---- 运行期状态 ----
    // ★ v8 补（MAJOR-1）：key 默认 0 是合法值，无法用 key==0 表示"尚未建过"。
    bool          key_valid        = false;
    bool          first            = true; // 初值 true；尺寸变化置 true；成功 EvaluateFeature 后置 false（F-32）
    std::uint32_t eval_fail_streak = 0;    // 连续 EvaluateFeature 失败次数；达 3 置 g_release_all（D9 条件 3）
    std::uint64_t eval_fail_total  = 0;    // 累计失败次数（**只用于日志**，不参与任何判定）
    std::uint32_t pool_rebuilds    = 0;    // 本实例建池次数（**只用于日志**）
    std::uint32_t lru_evict        = 0;    // **v8 起恒为 0**（单实例无驱逐）；保留字段仅为日志格式兼容
    std::uint64_t last_used_tick   = 0;    // 最后使用时刻（GetTickCount64）；v8 起不用于 LRU，仅日志
};

// ============================================================================
// §3.3.4.2 全局状态块
// ★ 形态纪律（T-2）：**只有 g_release_all / g_logged_config / g_ngx_log_count 是 std::atomic**，
//   其余状态位与全部配置项都是普通 bool/int（SDS 指定）。g_release_all 是唯一被跨 TU 无锁读取的
//   变量（Dx11FsrBridge.cpp 的 draw 钩子直接读，§3.3.3），所以它必须是 atomic；其余只在
//   ffx12::dispatch() 持 g_mutex 的调用栈内访问。
// ============================================================================

// ---- 进程级状态位 ----
std::atomic<bool> g_release_all { false };   // D9 全局放手；0→1 后不可逆，只读于两道 gate
bool g_inited    = false;   // ensure_init 成功过（**失败路径不置位**，见 §3.4.2 与 B-9 的更正）
bool g_available = false;   // SuperSampling 可用

// ---- 设备与参数 ----
Microsoft::WRL::ComPtr<ID3D11Device> g_device;   // 只 AddRef 一次；与游戏设备比对用
NVSDK_NGX_Parameter *g_params = nullptr;          // 全局一份，**不调 Reset()**（§3.9.5）
std::atomic<bool>   g_logged_config { false };    // 配置快照只打一次（§3.10.2 第 0 步）
std::uint64_t       g_init_retry_after_us = 0;   // ★ 失败退避窗口（§3.4.1 / MAJ-13）；0 = 未失败过

// ---- 入口（10 个函数指针，§3.4.3 / §3.4.6）----
PFN_NGX_Init_Ext                 g_init_ext   = nullptr;
PFN_NGX_GetCapabilityParameters  g_get_caps   = nullptr;
PFN_NGX_AllocateParameters       g_alloc      = nullptr;
PFN_NGX_DestroyParameters        g_destroy    = nullptr;
PFN_NGX_GetScratchBufferSize     g_scratch    = nullptr;
PFN_NGX_CreateFeature            g_create     = nullptr;
PFN_NGX_ReleaseFeature           g_release    = nullptr;
PFN_NGX_EvaluateFeature          g_eval       = nullptr;
PFN_NGX_Shutdown1                g_shutdown   = nullptr;
PFN_NGX_GetFeatureRequirements   g_req        = nullptr;

// ---- 自写 CS（全局，形状与尺寸无关；只编一次，§3.5）----
// T-2 只声明；ensure_shaders 是 T-4 的交付物。
Microsoft::WRL::ComPtr<ID3D11ComputeShader> g_depth_cs;
Microsoft::WRL::ComPtr<ID3D11ComputeShader> g_motion_cs;
bool g_shaders_ready = false;
bool g_shader_compile_failed = false;     // 编译输入不变；进程生命周期锁存，不因设备重建清除
bool g_shader_object_failed = false;      // CreateComputeShader 设备对象失败；shutdown_all 后允许新设备重试

// ---- 实例表 ----
NgxInstance g_inst;   // v8（S-90）：全局唯一实例，不再是 unordered_map + LRU

// ★ v8 补（MAJOR-1）：上一次的 instance_key，**仅用于 ngx_inst switch from=… 日志**。
std::uint64_t g_prev_key = 0;

// ---- 配置（9 个 ini 键的落地变量，与 §3.13 的 8 个 setter + ffx12::set_backend 一一对应）----
// 注意：Ffx12Backend 那个键**不在这里** —— 它归 ffx12::set_backend()，见 §3.13.1。
unsigned long long        g_app_id        = 1337;
std::vector<std::wstring> g_feature_dirs;              // NgxFeatureDirs 的**原始输入**（空 = 自动探测）
std::vector<std::wstring> g_feature_dir_storage;       // ★ 解析后的字符串本体（PathListInfo 的持有者）
std::vector<const wchar_t *> g_feature_dir_ptrs;       // PathListInfo.Path 指向它的 data()
// ★ 类型说明：SDS §3.3.4.2 把 g_feature_dir_storage 写成 std::wstring，但 §3.4.5 的持有纪律表
//   （"字符串本体 = static std::vector<std::wstring>"）与计划 v18-r12 §D14.0-c2 都要求 vector，
//   且每个目录是一条独立字符串 ⇒ 取 vector（计划优先；§3.4.5 是同一份纪律的正文处）。
bool                      g_hdr           = true;      // 保留：旧 set_hdr 的语义（强制值）
// T-8：三态与探测状态
// ★ m4（T-8 审核）：探测状态是**跨线程**的——
//   写方 = hooked_set_color_space1（在游戏的 Present/Init 线程上跑），
//   读方 = ngx::dispatch（在 ffx12::g_mutex 内，但是另一个线程）。
//   裸 bool 的读写在 C++ 内存模型下是数据竞争（UB），且编译器可以把它提到循环外/合并两次读。
//   ⇒ 必须是 atomic，两端统一 memory_order_relaxed：
//     这不是同步手段（没有任何数据靠这个标志发布），只是消除 UB。
// ★★ MINOR-1（T-8 收尾）：**三态打包进单个 atomic<unsigned>**，一次 store / 一次 load。
//   原实现是「值 + known」两个独立 atomic，set 时先写值再置 known，并声称这样安全 ——
//   该论断**是错的**：两个独立的 relaxed store 之间**不构成任何顺序约束**，编译器与 CPU
//   都可以让 known=true 先对读方可见 ⇒ 读方看到「known=true + 值仍是初始 false」
//   ⇒ HDR 会话被短暂判成 SDR。合并成一个 atomic 后，读方一次 load 拿到的必然是
//   某一个**完整的**三态值，**不存在「两个变量之间被改写」的窗口**。
//   取值：0 = 从未探测到（未知）；1 = 已知且 SDR；2 = 已知且 HDR。
std::atomic<unsigned>     g_swapchain_hdr_state { 0 };   // SetColorSpace1 探测结果（三态）
// ★ m4：主动探测源（T-8 M4-b 路径 A）——swapchain 的 BufferDesc.Format。
//   它不依赖 SetColorSpace1 是否被调用过，所以对「游戏从不调 SetColorSpace1」是净收益。
//   同样跨线程（install_swapchain_hooks 写、dispatch 读）⇒ 同款 atomic。
std::atomic<unsigned>     g_swapchain_format { 0 };          // 0 = 未知（DXGI_FORMAT_UNKNOWN）

// T-10：Windows 显示配置 API 探测结果（0=未探测 1=SDR 2=HDR）。
// ★ 独立于 g_swapchain_hdr_state：那是游戏主动声明的意图，这是系统级真实状态，两者互为校验。
std::atomic<unsigned>     g_displayconfig_hdr_state { 0 };

// ★ T-11：①②③ 三级探测的**预计算结果**（0=未定 1=SDR 2=HDR）。
//   设计原则：探测源都是**事件驱动**的（hook / ensure_init），不是每帧变化的，
//   所以不该在每帧路径上重复读+重复判断。写入方在状态变化后立即调
//   refresh_hdr_probe_cache() 刷新本缓存，每帧只做一次 atomic load。
//   ★ 只有缓存为 0（三级都没探测到）时，才回落到 ④ 的「按输入纹理格式猜」。
std::atomic<unsigned>     g_hdr_probe_cache { 0 };

// ★ m4：g_hdr_mode **故意保持普通 int**：它只由 load_config() 在 blog::init() 之前写入，
//   之后运行期只读，cfg 无热重载（AGENTS §8.5）⇒ 不存在跨线程写，加 atomic 是纯开销。
//   g_hdr 同理（且它已退化为「本次决策的回写镜像」，见 resolve_is_hdr 的注释）。
int                       g_hdr_mode      = 0;         // 0=auto 1=force-on 2=force-off
int                       g_render_preset = 0;          // P-11：默认 0 = 不设 preset，跟随 NGX 自身规则
bool                      g_dt_enabled    = true;
bool                      g_use_reactive  = false;
int                       g_log_level     = 1;
bool                      g_passthrough   = false;

// ---- 诊断计数 ----
std::atomic<unsigned> g_ngx_log_count { 0 };   // §3.11.3 的 200 条硬限流

// ---- dispatch 帧计数（T-5，§3.10.1 三条逐帧日志共用的闸门输入）----
// ★ **不复用 g_ngx_log_count**：那是 NGX 日志回调的 200 条硬限流（§3.11.3），
//   语义（回调条数 vs 帧数）与生命周期（回调线程 vs dispatch 调用栈）都不同。
// ★ 类型：普通 std::uint64_t，**不是 atomic** —— 本文件的状态形态纪律（§3.3.4.2）是
//   「除 g_release_all / g_logged_config / g_ngx_log_count 外都是普通变量」；
//   g_ngx_log_count 之所以必须是 atomic，是因为 NGX 日志回调来自任意线程（§3.11.3），
//   而本计数器只在 ngx::dispatch 内自增/读取，dispatch 只在 ffx12::g_mutex 内被调用
//   （SDS §4.4 B-8「timed_mutex 保证串行」），没有任何跨线程暴露面。
//   ⇒ 加 atomic 等于为一个不存在的竞争付一次全内存序，且破坏本文件的形态纪律。
std::uint64_t g_ngx_frame_count = 0;

// 2026-11-09（PR #12 审查次要项 1 复核）：passthrough 逐帧行的**诊断档位**计数闸门输入。
// 与 g_ngx_frame_count 同一形态纪律（普通 std::uint64_t，只在 ngx::dispatch 内自增）。
// ★ 不复用 g_ngx_frame_count：那一份的语义是「第几帧」，被 ngx_in/ngx_out/eval 三条线
//   共用；passthrough 时那三条线**全部不可达**（第 0 步就 return），复用它等于让它
//   在 passthrough 会话里只为本行服务 —— 名字与实际用途不符，下一个人一定会读错。
std::uint64_t g_ngx_passthrough_count = 0;

// ---- NGX ini 非法值位图（此处不能打日志，所以先记位）----
// backend_invalid 由 Dx11FsrBridge.cpp 的 load_config 缓存到 pending_warnings，
// 因为 Native 回退时不会进入 ngx::ensure_init()。
enum NgxConfigWarn : unsigned {
    kWarnAppIdZero      = 1u << 0,   // NgxApplicationId=0 → 强制 1337
    kWarnPresetInvalid  = 1u << 1,   // NgxRenderPreset 越界 → 回退 0
};
unsigned g_config_warn = 0;

// ============================================================================
// §3.9A.1 内部辅助函数（全部在本匿名命名空间内）
// ============================================================================

// ---- 日志：多套形态并存是**有意**的，不要合并 ----
// log_ngx_core：带级别参数，用于生命周期事件与配置快照（§3.10.1 的 ngx_core 层）
// log_ngx_error：等价于 log_ngx_core(Level::Error, …)，仅用于"失败且要置 g_release_all"的路径
//
// ★ 2026-11-09（PR #12 审查 D-1 / D-2）：**上面两个函数都不做任何计数闸门**，
//   这才是「等价」二字的实际含义。上一版只给 log_ngx_error 加了计数闸门，
//   于是注释与实现互相矛盾（违反 AGENTS.md §二.2），且闸门本身是错的（见下方 D-1 注）。
//   频次控制的责任在**调用方**，按错误性质二选一：
//     · 致命错误（置 g_release_all / 设备丢失 / 一次性事件）⇒ log_ngx_error 或
//       log_ngx_core(Level::Error, …)，**不加闸门**。它们承载的是「NGX 已全局放手」的
//       唯一解释，吞掉任何一条都会让现场无法复盘。
//     · 高频重复的**非致命**错误（建池 / 建视图 / 采样失败）⇒ log_ngx_error_throttled
//       （按调用点独立限频），或 BridgeLogger.h:152/171 的 LOG_THROTTLE / LOG_THROTTLE_COUNT。
//   ★ 改这里任何一个函数的行为，都必须同步改本段注释（§二.2：注释不得与代码矛盾）。
void log_ngx_core(blog::Level lv, const wchar_t *fmt, ...);   // 无计数闸门（频次控制归调用方）
void log_ngx_error(const wchar_t *fmt, ...);                  // 恒以 Level::Error 输出，无计数闸门

// 2026-11-09（PR #12 审查 D-1/D-2 新增）：**按调用点独立限频**的 Error 日志。
//   ★ 存在理由（缺陷原状）：上一版把计数闸门加在 log_ngx_error **内部**，所有调用点共用
//     一个 static 计数器 ⇒ 前 32 条之后的**任何新种类**错误都被静默吞掉，直到计数恰好落在
//     1024 的倍数；而其中若干条是置 g_release_all 的致命失败。当时的注释还声称
//     「首次出现的错误必然落在前 32 条里」—— 那句话是错的：计数器不区分调用点。
//     结论：闸门不能按"函数"共享，必须按"调用点"独立。
//
// ★★ 2026-11-09（R-5 MAJOR 修复 + R-9 MINOR-1 收口）：槽位改为**编译期模板参数**，不再对
//   tag 字符串做运行期哈希。上一版的槽位是在**运行期**对 tag 字符串做 FNV-1a 哈希取模算出的，
//   而 static_assert 只比较手工列出的 kUsedSlots[] —— 两者之间没有任何强制关联。于是"新增
//   tag 撞槽会被编译器拦下"是**假的**：新调用点只要不把 tag 加进 kUsedSlots，编译器不会报错，
//   撞槽被静默接受；而撞槽 = 两个调用点共享同一个计数器，后出现的那个 tag 的**首现会被吞掉**。
//   当时发现不了的原因：哈希值无法从 tag 名一眼看出（撞槽只能靠事后核对发现），而那条断言
//   核对的是常量表、不是实际传入的 tag。
//   ⇒ R-5 先把槽位改成模板参数，但断言只做到"传进来的值在常量表里"，仍留一条残余路径：
//     **另起一个取同值的常量、不登记进表**照样编译通过（slot_is_registered(3) 会因为 3 已被
//     ColorSrvFail 占用而返回 true），两个调用点于是静默共用 count<3>，新调用点的首现被吞。
//     R-9 用 g++ 与 MSVC 双编译器复现了这条路径（新调用点首现打印次数 = 0）。
//   ⇒ R-9 MINOR-1 的收口：**enum 与常量表由同一份 LOG_SLOT_LIST 展开**（见下方命名空间）。
//     ★ 能保证的只有「**经 LOG_SLOT_LIST 新增**的枚举子与表项同时产生、逐项对应」——
//       那正是本文件规定的唯一写法，不是语言层面堵死的性质：
//       枚举定义处**仍可手工追加**枚举子（T-FIX-18 已用 g++ 16.2.0 实测：追加
//       `NewCallSite = 14` 后 slots_all_distinct() 照样通过、两个枚举子共用 count<14>）。
//       所以「不加显式转换就写不出第二处槽号」**不成立**，绕过点有两个：
//       ① 直接在 enum 花括号里追加枚举子（不受 static_assert 约束）；
//       ② 显式转换 static_cast<Slot>(n) / Slot(n) / (Slot)n / Slot{n}。
//       但显式转换仍可绕过：static_cast<Slot>(n) 只是其中一种写法，Slot(n) / (Slot)n / Slot{n}
//       同样可行（g++ 16.2.0 实测，见下方模板定义处的「残余」说明）。
//     每个枚举子是一个**独立的模板特化**，各自持有自己的 static 计数器（物理隔离）
//     ⇒ 运行期不存在哈希，也就不存在哈希冲突。
//   ★ 调用点写法：log_ngx_error_throttled<log_throttle_slot::Slot::PoolTexFail>(fmt, ...)，
//     **没有** tag 实参；模板参数是 enum，裸整数（如 <3>）不再能隐式转换进去。
//   ★ 前 8 条全打（启动/首帧可见，槽的首现必在其中），之后每 1024 条一条。
//   适用：高频重复的**非致命**错误（pool 创建失败、sample 失败等）。
//   ★ 致命错误（置 g_release_all 的）**必须**用 log_ngx_error，不要用本函数。

namespace log_throttle_slot
{
// ★★ 唯一的真值源：新增/删除/改名调用点只改这一处，enum 与常量表都由它展开。
//   格式：X(枚举子名, 槽号)。槽号是**手写常量**、只用来标识身份，不是任何哈希的结果。
#define LOG_SLOT_LIST(X) \
    X(PoolTexFail,       14) \
    X(ColorSrvFail,       3) \
    X(UavFail,           20) \
    X(CbFail,             5) \
    X(SampleStagingFail, 26) \
    X(SampleMapFail,     15)

// ① 调用点可以传的**全部**槽位（模板参数的类型就是它）。
#define LOG_SLOT_ENUM_ITEM(name, value) name = value,
enum class Slot : unsigned { LOG_SLOT_LIST(LOG_SLOT_ENUM_ITEM) };
#undef LOG_SLOT_ENUM_ITEM

// ② 常量表：由同一份列表展开 ⇒ 与 ① 逐项对应，"加了枚举子却忘了登记"不再是一个可表达的状态。
#define LOG_SLOT_TABLE_ITEM(name, value) Slot::name,
constexpr Slot kUsedSlots[] = { LOG_SLOT_LIST(LOG_SLOT_TABLE_ITEM) };
#undef LOG_SLOT_TABLE_ITEM
#undef LOG_SLOT_LIST

constexpr unsigned kSlots = 32;   // 槽号上限；LOG_SLOT_LIST 里的槽号必须 < kSlots
constexpr unsigned kUsedSlotCount = sizeof(kUsedSlots) / sizeof(kUsedSlots[0]);

// 表内两两互异。★ X-macro 只保证 enum 与表**逐项对应**，它管不住两个枚举子被显式赋成
//   **同一个槽号** —— 那等于两个调用点共用同一个计数器、后出现者的首现被吞，必须编译期拦下。
//   6 个元素 ⇒ 15 次比较，编译期零成本。
constexpr bool slots_all_distinct()
{
    for (unsigned i = 0; i < kUsedSlotCount; ++i)
        for (unsigned j = i + 1; j < kUsedSlotCount; ++j)
            if (kUsedSlots[i] == kUsedSlots[j])
                return false;
    return true;
}

static_assert(slots_all_distinct(),
              "log_ngx_error_throttled：LOG_SLOT_LIST 里两个条目取了同一个槽号 ⇒ 两个调用点会共用"
              "同一个计数器，后出现者的首现会被吞掉。请换一个未占用的槽号。");
} // namespace log_throttle_slot

template <log_throttle_slot::Slot S>
void log_ngx_error_throttled(const wchar_t *fmt, ...);

// ---- flags / 配置位 转字符串：**返回 std::string**（配合 %hs 打印，见 §3.10.1）----
std::string flags_to_string(unsigned int flags);
std::string warn_to_string(unsigned int bits);

// T-10：用 Windows 显示配置 API 探测显示器真实 HDR 状态。
// 返回 0=探测失败/未知，1=SDR，2=HDR（任一活动显示器开着 advanced color 即判 HDR）。
// ★ 只在 ensure_init 调用一次（QueryDisplayConfig 有枚举开销，不能每帧调）。
// ★ 失败不置 g_release_all：探测失败只是少一个信息源，不是致命错误。
static unsigned probe_displayconfig_hdr()
{
    UINT32 path_count = 0, mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS)
        return 0;
    if (path_count == 0)
        return 0;

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count > 0 ? mode_count : 1);

    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(),
                           &mode_count, modes.data(), nullptr) != ERROR_SUCCESS)
        return 0;

    bool any_hdr = false;
    bool any_ok  = false;
    for (UINT32 i = 0; i < path_count; ++i)
    {
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info {};
        info.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        info.header.size      = sizeof(info);
        info.header.adapterId = paths[i].targetInfo.adapterId;
        info.header.id        = paths[i].targetInfo.id;

        if (DisplayConfigGetDeviceInfo(&info.header) != ERROR_SUCCESS)
            continue;

        any_ok = true;
        if (info.advancedColorEnabled)
            any_hdr = true;
    }

    if (!any_ok) return 0;
    return any_hdr ? 2u : 1u;
}

// T-11：重算 ①②③ 三级的合并结果并写入 g_hdr_probe_cache。
// 由四个写入点调用（set_swapchain_hdr / set_swapchain_format /
// ensure_init 的 DisplayConfig 探测 / set_hdr_mode）。
// ★ 优先级与 resolve_is_hdr 一致：① > ② > ③；都为 0 时缓存置 0（交给 ④ 兜底）。
static void refresh_hdr_probe_cache()
{
    const unsigned sw = g_swapchain_hdr_state.load(std::memory_order_relaxed);
    if (sw != 0)
    {
        g_hdr_probe_cache.store(sw, std::memory_order_relaxed);
        return;
    }

    const unsigned dc = g_displayconfig_hdr_state.load(std::memory_order_relaxed);
    if (dc != 0)
    {
        g_hdr_probe_cache.store(dc, std::memory_order_relaxed);
        return;
    }

    const unsigned fmt_raw = g_swapchain_format.load(std::memory_order_relaxed);
    if (fmt_raw != 0)
    {
        const DXGI_FORMAT fmt = static_cast<DXGI_FORMAT>(fmt_raw);
        const bool hdr = (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                          fmt == DXGI_FORMAT_R16G16B16A16_TYPELESS);
        g_hdr_probe_cache.store(hdr ? 2u : 1u, std::memory_order_relaxed);
        return;
    }

    g_hdr_probe_cache.store(0u, std::memory_order_relaxed);   // 三级都未定 ⇒ ④ 兜底
}

// ---- T-8：IsHDR 决策（定义在 §3.8.2 开头的 build_create_flags 之前）----
// ★ 必须在这里前置声明：§3.10.2 的配置快照第 3 行（log_config_snapshot，定义在 §3.9A.1 之后、
//   §3.8 之前）就要用它来取"与 build_create_flags 同源的 HDR 决策"（M2 修复）。
//   漏这一行 ⇒ C3861（DIAG 构型实测：NgxBackend.cpp(730) error C3861: "resolve_is_hdr"）。
//   注意：函数体的定义位置**不变**（仍在 build_create_flags 之前），此处只补声明 ——
//   不要为了消这个错把函数体搬到前面，那会打乱 §3.8 一节「定义顺序写死」的约定。
//
// ★ MINOR-1（T-8 第 3 轮收尾）：新增可选出参 sw_state_out（默认 nullptr）。
//   用途只有一个：让调用方能拿到**决策所依据的那一次** g_swapchain_hdr_state load 的原始值，
//   使「决策」与「日志里的 sw_known/sw_hdr」同源，杜绝两次 load 之间被写方翻转造成的自相矛盾日志。
//   默认 nullptr ⇒ 既有调用点（log_config_snapshot / flags_input_bits）**不需要改**、行为不变。
//   默认实参只写在**本条声明**上，定义处不得重复写（否则是重定义默认实参，编译错误）。
static bool resolve_is_hdr(DXGI_FORMAT color_fmt, const char **src, unsigned *sw_state_out = nullptr);

// ---- §3.10.3 的两级叠加第 1 级：NgxLogLevel 档位换算 ----
// 两把尺子的刻度不同：NgxLogLevel 是 1=error / 2=info / 3=debug，而 blog::Level 是
// Error=1 / Warn=2 / Info=3 / Debug=4 / Trace=5。**必须先换算再比较** —— 直接拿 blog::Level 的
// 数值去比 g_log_level，会在 NgxLogLevel=2 时把 info 行（数值 3）误杀，而那正是 §7.4 规定
// "级别 2 加 eval 成功行 / ngx_in / ngx_out 逐帧"的前提。
static int ngx_level_of(blog::Level lv)
{
    switch (lv)
    {
    case blog::Level::Error:
    case blog::Level::Warn:
        return 1;   // error
    case blog::Level::Info:
        return 2;   // info
    case blog::Level::Debug:
    case blog::Level::Trace:
        return 3;   // debug
    default:
        return 0;   // Off：永不出
    }
}

// 宽 → 窄（UTF-8）后交给 blog::write。**不手写逐字符截断**：路径里可能有中文
// （部署现场就是 ...\原神解帧FSR插件包\payload\Bridge），截断会打出乱码路径。
// 只在主线程调用（§3.9A.1 要点 5）；回调线程走 ngx_log_cb 的 const char* 直通。
static void vlog_ngx(blog::Level lv, const wchar_t *fmt, va_list args)
{
    wchar_t buf[1024] {};
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);   // 截断而非溢出
    char narrow[2048] {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, narrow,
                                      static_cast<int>(sizeof(narrow)), nullptr, nullptr);
    if (n <= 0)
        return;   // 转换失败（缓冲不足等）：宁可不打，也不打半条
    blog::write(lv, blog::cat::upscale, std::string(narrow, static_cast<std::size_t>(n - 1)));
}

void log_ngx_core(blog::Level lv, const wchar_t *fmt, ...)
{
    if (g_log_level <= 0)
        return;                                    // NgxLogLevel=0 全关（§3.10.3 / 要点 1）
    if (ngx_level_of(lv) > g_log_level)
        return;                                    // 只有 lv 的档位 <= g_log_level 才输出
    va_list args;
    va_start(args, fmt);
    vlog_ngx(lv, fmt, args);
    va_end(args);                                  // ★ va_list 只消费一次（AGENTS.md §16.1-3）
}

void log_ngx_error(const wchar_t *fmt, ...)
{
    if (g_log_level <= 0)
        return;
    // ★★ 2026-11-09（PR #12 审查 D-1 / D-2）：**本函数不做任何计数闸门**。
    //   缺陷原状（已删除）：这里曾有一个 static std::atomic_uint64_t，**所有**调用点共用，
    //   前 32 条全打、之后每 1024 条一条。它有两个后果：
    //     ① 计数器不区分调用点、不区分错误种类 ⇒ 第 33 条之后**任何新种类**错误都被吞掉
    //        （例如第 40 条才第一次出现的 set_mismatch），直到计数恰好落在 1024 的倍数。
    //        旧注释「首次出现的错误必然落在前 32 条里」正是这个错误的来源，已证伪并删除。
    //     ② 本函数承载若干**置 g_release_all 的致命失败**（fail_init / set_mismatch /
    //        release_all / dev_removed），它们是「NGX 已全局放手」的唯一解释 —— 吞掉任何
    //        一条都会让现场无法复盘。
    //   ⇒ 频次控制移到**调用方**（AGENTS.md §五.6 的要求仍然成立，只是责任点不同）：
    //     非致命的重复性错误请改用 log_ngx_error_throttled（按槽独立限频，见该模板的注释）。
    //   ★ 本函数与 log_ngx_core 因此在闸门语义上**重新等价**（两者都只做等级闸门）。
    va_list args;
    va_start(args, fmt);
    vlog_ngx(blog::Level::Error, fmt, args);
    va_end(args);
}

// 2026-11-09（PR #12 审查 D-1/D-2）：按调用点独立限频的 Error 日志（实现说明见声明处）。
//
// ★★ 槽位（2026-11-09 R-5 MAJOR 修复 + R-9 MINOR-1 收口）：模板参数是 log_throttle_slot::Slot，
//   **不接受裸整数**。上一版是在运行期对 tag 字符串做 FNV-1a 哈希取模来算槽，带来三个后果：
//     ① static_assert 是**假的**：它只比较常量表，与调用点实际传入的字符串毫无关联
//        ⇒ 新增调用点忘了登记就不会被编译器发现。
//     ② 撞槽**吞首现**：计数器是按槽共享的，某个 tag 打满前 8 条后，撞进同一槽的新 tag
//        首现时 n 已 >8 且通常不是 1024 的倍数 ⇒ 首现被静默丢弃（旧注释声称"首现必在
//        前 8 条里"，只在"该槽从未被别的 tag 用过"时成立，而槽是共享的，前提不成立）。
//     ③ 阈值 32→8 把"首现被吞"所需的前置次数从 32 降到 8，放大 ②。
//   ⇒ R-5 先改成模板参数 Slot，但断言只检查"Slot 的值在常量表里"，于是留下残余路径：
//     另起一个**取同值**的常量、不登记进表，断言照样通过（R-9 用 g++ 与 MSVC 双编译器实测，
//     新调用点首现打印次数 = 0）。这是 R-5 那个 MAJOR 的残余路径。
//   ⇒ 现在把风险从"看不出的静默撞车"收敛为"必须显式越权"，依据如下（**不是**结构上不可能）：
//     · enum 与常量表由**同一份 LOG_SLOT_LIST** 展开 ⇒ **经该列表新增**的枚举子与表项逐项对应；
//     · 表内两两互异由 slots_all_distinct() 在编译期守 ⇒ **表内**两个条目不会取到同一槽号。
//       ★ 该断言只看 kUsedSlots[]，**看不到 enum 定义处手工追加的枚举子**（g++ 16.2.0 实测：
//         追加 `NewCallSite = 14` 后断言仍通过，count<14> 被两个枚举子共用 ⇒ 首现被吞）；
//         故「两个枚举子不可能取到同一槽号」**不成立**，改为"表内条目互异"。
//     · 模板参数的类型就是那个 enum ⇒ 传裸整数（如 <3>）不再能隐式转换进来。
//     ⇒ 每个枚举子是一个**独立的模板特化**，各自持有自己的 static 计数器（物理隔离），
//       运行期零哈希、零冲突，② 与 ③ 的存在前提被消除。
//   ★ 残余（诚实标注，不是"已完全堵死"）：以下三种写法仍会让两个调用点共享计数器 ——
//     ① 把**同一个枚举子**用在两个调用点；
//     ② 显式写 static_cast<log_throttle_slot::Slot>(n)（或 Slot(n) / (Slot)n / Slot{n}）指定槽号；
//     ③ 在 enum 定义处**手工追加**一个取同值的枚举子（绕过 LOG_SLOT_LIST ⇒ static_assert 看不到）。
//     三者都是 diff 里一眼可见的显式越权，而不是旧方案那种"看不出撞车"的静默接受。

// ★★ 槽位是模板参数，每个枚举子是**独立的模板特化** ⇒ 下面那个 static 计数器是
//   **每个特化各自一份**（C++ 标准：函数模板的静态局部变量属于该特化，不是全模板共享）。
//   这就是"物理隔离"的全部依据：枚举子不同 ⇒ 特化不同 ⇒ 计数器不同 ⇒ 不存在互相挤占，
//   也就不存在"撞槽吞首现"。反过来说，同一调用点每次必然传同一个枚举子，
//   所以它自己的计数是连续的、语义明确的。
template <log_throttle_slot::Slot S>
void log_ngx_error_throttled(const wchar_t *fmt, ...)
{
    // ① 越界：槽号 >= kSlots 说明 LOG_SLOT_LIST 里写了超范围的槽号（扩表时忘了改上限）。
    constexpr unsigned Slot = static_cast<unsigned>(S);
    static_assert(Slot < log_throttle_slot::kSlots,
                  "log_ngx_error_throttled：槽号越界（必须 < log_throttle_slot::kSlots）。"
                  "请检查 LOG_SLOT_LIST 里的槽号。");
    // ② 不再需要"是否登记"断言：S 的类型就是 log_throttle_slot::Slot，而**经 LOG_SLOT_LIST**
    //    写出来的枚举子与常量表由**同一份列表**展开 ⇒ "传进来的值不在表里"这个状态不会出现。
    //    ★ 但"枚举子一定来自 LOG_SLOT_LIST"**不成立**：enum 定义处可手工追加（见上方 ③）。
    //    ★ 但显式转换（static_cast<Slot>(n) / Slot(n) / (Slot)n / Slot{n}）可构造出任意数值
    //      （含根本不是枚举子的 7），该状态仍可表达。这是**已知残余**，见上方「残余」段；
    //      不靠断言拦，靠 code review 拦。

    if (g_log_level <= 0)
        return;

    // 本特化独有的计数器（每个枚举子一份，物理隔离）。
    static std::atomic_uint64_t count { 0 };
    const std::uint64_t n = count.fetch_add(1, std::memory_order_relaxed) + 1;

    // 前 8 条全打（启动/首帧可见 ⇒ 本槽的**首现必然打出**），之后每 1024 条一条。
    //   ★ 「首现必打」的前提（T-FIX-18 补齐，此前只写在下面第三颗 ★ 里）：
    //     ① 本槽的计数从 0 起（count 是该特化的静态局部变量，n 由 1 开始）；
    //     ② 该槽只被一个调用点使用 —— 撞槽（见上方"残余"）会让共享槽的第二个调用点
    //        首现时 n 已 >8 且通常不是 1024 的倍数 ⇒ 被吞；
    //     ③ 日志链路的其它闸门放行：vlog_ngx 本身只做 g_log_level 档位闸门，
    //        但 blog::write 还有一层 enabled()，可用 [Log] 分类覆盖把 upscale 关掉
    //        （BridgeLogger.cpp:394-403）。那种配置下"首现必打"也不成立。
    // ★ 上界是「本槽重复错误打多少条」的预算，不是「哪些错误能打」的白名单 —— 这正是
    //   旧实现弄错的地方：它把预算做成了全局的，于是新种类错误被当成"第 33 条重复"吞掉。
    // ★ 计数器是**槽级**的（每个枚举子一份），所以 n 的语义严格说是「本槽第几次」，不是
    //   「本调用点第几次」；两者只在"一个槽只被一个调用点使用"时重合，而那是本文件的常态。
    //   由此"首现必打"成立。
    if (n > 8 && (n % 1024) != 0)
        return;

    va_list args;
    va_start(args, fmt);
    vlog_ngx(blog::Level::Error, fmt, args);
    va_end(args);   // ★ va_list 只消费一次（AGENTS.md §16.1-3）
}

// §3.9A.1：输入是 9 个 bit 的 OR 结果；输出形如 IsHDR|MVLowRes|DepthInverted（无位时为 None）。
// 位名与顺序写死（与 §3.8.2 的 bit 表同序），分隔符只有 '|'（§3.10.1 的"本表是唯一定义"）。
// ★ 不列 DoSharpening(1<<5)：SR_DEPRECATED_SHARPENING 在 MSVC 下展开成 [[deprecated]]
//   （nvsdk_ngx_defs.h:60 / :296），引用该枚举值即 C4996；且本设计恒不设该位。
std::string flags_to_string(unsigned int flags)
{
    struct BitName { unsigned int bit; const char *name; };
    const BitName names[] = {
        { NVSDK_NGX_DLSS_Feature_Flags_IsHDR,          "IsHDR" },
        { NVSDK_NGX_DLSS_Feature_Flags_MVLowRes,       "MVLowRes" },
        { NVSDK_NGX_DLSS_Feature_Flags_MVJittered,     "MVJittered" },
        { NVSDK_NGX_DLSS_Feature_Flags_DepthInverted,  "DepthInverted" },
        { NVSDK_NGX_DLSS_Feature_Flags_AutoExposure,   "AutoExposure" },
        { NVSDK_NGX_DLSS_Feature_Flags_AlphaUpscaling, "AlphaUpscaling" },
    };
    if (flags == 0)
        return std::string("None");
    std::string out;
    unsigned int known = 0;
    for (const BitName &b : names)
    {
        known |= b.bit;
        if ((flags & b.bit) == 0)
            continue;
        if (!out.empty())
            out += "|";
        out += b.name;
    }
    const unsigned int rest = flags & ~known;
    if (rest != 0)
    {
        char tail[32] {};
        std::snprintf(tail, sizeof(tail), "unknown(0x%X)", rest);
        if (!out.empty())
            out += "|";
        out += tail;
    }
    return out;
}

// §3.13 末：配置位 → 位名。**只有两位**（appid_zero / preset_invalid）。
// backend_invalid 不在此位图（它走 pending_warnings，因为 Native 回退后 ensure_init 不可达）。
// bits == 0 返回空串（调用方只在 g_config_warn != 0 时调，见 §3.13.2 第 2 条）。
std::string warn_to_string(unsigned int bits)
{
    struct BitName { unsigned int bit; const char *name; };
    const BitName names[] = {
        { kWarnAppIdZero,     "appid_zero" },
        { kWarnPresetInvalid, "preset_invalid" },
    };
    if (bits == 0)
        return std::string {};
    std::string out;
    unsigned int known = 0;
    for (const BitName &b : names)
    {
        known |= b.bit;
        if ((bits & b.bit) == 0)
            continue;
        if (!out.empty())
            out += "|";
        out += b.name;
    }
    const unsigned int rest = bits & ~known;
    if (rest != 0)
    {
        char tail[32] {};
        std::snprintf(tail, sizeof(tail), "unknown(0x%X)", rest);
        if (!out.empty())
            out += "|";
        out += tail;
    }
    return out;
}

// ============================================================================
// §3.10.1 的三个日志闸门（T-5）
// ★ 三者的阈值**各不相同**，且都是 SDS 明文写死的，不要合并成一个函数 ——
//   合并会让"某条线的闸门调了、另一条忘了调"这种事故在下一次改动时重现。
// ★ 三种形态的来源：
//   ngx_in / ngx_out  → §3.10.1 表「闸门」列：count <= 8 || count % 1024 == 0
//   ngx_core eval 成功 → §3.10.1 表：每 1024 帧
//   ngx_core eval 失败 → §3.10.1 表：每次（前 8 次全打，之后每 256 次一次）
// ============================================================================

// 前 8 帧全打 + 之后每 1024 帧一次（ngx_in / ngx_out 共用，§3.10.1）。
static bool log_gate_win8_every1024(std::uint64_t count)
{
    return count <= 8u || (count % 1024u) == 0u;
}

// 每 1024 帧一次（ngx_core eval 成功行，§3.10.1）。
static bool log_gate_every1024(std::uint64_t count)
{
    return (count % 1024u) == 0u;
}

// 前 8 次全打，之后每 256 次一次（ngx_core eval 失败行，§3.10.1）。
// ★ 入参用 eval_fail_total（累计失败次数），**不是**帧计数 —— 失败行的密度应当与
//   失败频率相关，而不是与帧数相关；否则一个每 1000 帧失败一次的瞬态问题会永远淹没在
//   每 1024 帧的采样里。SDS 原文「失败行每次（前 8 次全打，之后每 256 次一次）」的
//   "次"指的是 eval 失败次数，这也与 release_all 行的 eval_fail_streak 同源。
static bool log_gate_fail8_every256(std::uint64_t fail_total)
{
    return fail_total <= 8u || (fail_total % 256u) == 0u;
}

// ============================================================================
// §3.11.3 NGX 日志回调（**逐字照抄**，含 200 条硬限流）
// BridgeLogger.h 已提供 write(Level, string_view, string_view)：内部是异步队列
// ⇒ 天然线程安全，满足 defs.h:368-374 "可从任意线程调用"；且不做 printf 族格式化
// （那正是早期 0xC0000409 崩溃的形态，见 §3.11.5）。
// ============================================================================
static void NVSDK_CONV ngx_log_cb(const char *message,
                                  NVSDK_NGX_Logging_Level level,       // 形参名必须保留，函数体要用它
                                  NVSDK_NGX_Feature /*source*/)
{
    // 1) 硬限流（防止 NGX 刷爆日志）：前 200 条全打，之后全丢
    const unsigned n = g_ngx_log_count.fetch_add(1, std::memory_order_relaxed);
    if (n >= 200) return;

    // 2) 级别的映射：NGX 的三档 → blog 的 Level
    //    OFF 不会进回调；ON → Info；VERBOSE → Debug
    const blog::Level lv = (level == NVSDK_NGX_LOGGING_LEVEL_VERBOSE)
                               ? blog::Level::Debug
                               : blog::Level::Info;
    blog::write(lv, blog::cat::upscale, message ? message : "");
}

// ============================================================================
// 路径工具（§3.4.4 / §3.4.5）
// ============================================================================

// 本 DLL 所在目录（**不含**尾部分隔符）。
// ★ 必须 GetModuleHandleExW(FROM_ADDRESS)：GetModuleFileNameW(nullptr, …) 取到的是**宿主 exe**
//   （Ffx12Backend.cpp:691-692 注释的同一条教训；计划 §R-32 也点名过）。
static std::wstring self_module_dir()
{
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&self_module_dir), &self) == FALSE ||
        self == nullptr)
        return std::wstring {};

    wchar_t path[MAX_PATH] {};
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return std::wstring {};
    const std::wstring full(path, path + n);
    const std::size_t slash = full.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring {} : full.substr(0, slash);
}

// 规范化：展开 ".."、去掉尾部分隔符（PathListInfo 里存整洁路径，日志也直接可读）。
static std::wstring normalize_dir(const std::wstring &dir)
{
    if (dir.empty())
        return std::wstring {};
    std::vector<wchar_t> buf(MAX_PATH * 2, L'\0');
    const DWORD n = GetFullPathNameW(dir.c_str(), static_cast<DWORD>(buf.size()), buf.data(), nullptr);
    if (n == 0 || n >= buf.size())
        return dir;
    std::wstring full(buf.data(), buf.data() + n);
    while (full.size() > 3 && (full.back() == L'\\' || full.back() == L'/'))
        full.pop_back();
    return full;
}

// §3.4.5 的判据：**目录内含 nvngx_dlss.dll**，不是"目录存在"（第 1 轮审核 MIN-5）。
// 本机反例：payload\Bridge 存在但不含该 DLL，按"存在"判据会被错误加入 ⇒ available=0。
static bool dir_has_dlss(const std::wstring &dir)
{
    if (dir.empty())
        return false;
    const std::wstring probe = dir + L"\\nvngx_dlss.dll";
    const DWORD attr = GetFileAttributesW(probe.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// 逐个追加候选目录（含判据 + 去重）。
static void append_feature_dir(const std::wstring &dir)
{
    if (!dir_has_dlss(dir))
        return;
    const std::wstring norm = normalize_dir(dir);
    for (const std::wstring &existing : g_feature_dir_storage)
    {
        if (existing == norm)
            return;
    }
    g_feature_dir_storage.push_back(norm);
}

// 解析 PathListInfo 的目录表（§3.4.5）。
// ★ 必须在打配置快照之前调用：快照第 4 行 featuredirs=%zu first=%s 要反映真实条目数。
// ★ NgxFeatureDirs 非空时原样采用（用户指定优先）；空时按序探测三个候选目录。
static void resolve_feature_dirs()
{
    g_feature_dir_storage.clear();
    g_feature_dir_ptrs.clear();

    if (!g_feature_dirs.empty())
    {
        for (const std::wstring &d : g_feature_dirs)
        {
            if (d.empty())
                continue;
            g_feature_dir_storage.push_back(normalize_dir(d));
        }
    }
    else
    {
        const std::wstring base = self_module_dir();
        if (!base.empty())
        {
            append_feature_dir(base);                                      // 1) 本 DLL 目录本身（本机不含 ⇒ 跳过）
            append_feature_dir(base + L"\\..\\NVIDIA\\DLSS");               // 2) payload 布局（本机含 ✅）
            append_feature_dir(base + L"\\..\\OptiScaler\\Optiscaler");     // 3) OptiScaler 副本（本机含 ✅）
        }
    }

    // ★ 指针数组必须在 storage **定稿之后**建：push_back 会让先前取到的 c_str() 全部失效
    //   （§3.3.4.2 的持有纪律）。Length == 0 时调用方必须传 nullptr（§3.4.5 表最后一行）。
    g_feature_dir_ptrs.reserve(g_feature_dir_storage.size());
    for (const std::wstring &d : g_feature_dir_storage)
        g_feature_dir_ptrs.push_back(d.c_str());
}

// 逐级建目录（先父后子，§3.4.4）。已存在算成功。
static bool ensure_dir_tree(const std::wstring &dir)
{
    const std::wstring path = normalize_dir(dir);
    if (path.empty())
        return false;
    std::wstring cur;
    cur.reserve(path.size());
    for (std::size_t i = 0; i < path.size(); ++i)
    {
        cur.push_back(path[i]);
        const bool at_sep = (path[i] == L'\\' || path[i] == L'/');
        if (!at_sep && i + 1 != path.size())
            continue;
        if (cur.size() <= 3)   // "C:\" 这类根：必然存在
            continue;
        if (CreateDirectoryW(cur.c_str(), nullptr) == FALSE)
        {
            const DWORD err = GetLastError();
            if (err != ERROR_ALREADY_EXISTS)
                return false;
        }
    }
    return true;
}

// %LOCALAPPDATA%（**不用 GetEnvironmentVariableW**：某些启动器会清环境，§3.4.4）。
static std::wstring known_folder_localappdata()
{
    PWSTR raw = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw != nullptr)
        out.assign(raw);
    if (raw != nullptr)
        CoTaskMemFree(raw);
    return out;
}

// §3.4.4 的 InApplicationDataPath：
//   首选 <本 DLL 目录>\ngx\；失败 → %LOCALAPPDATA%\GenshinFsrBridge\ngx\；再失败 ⇒ 调用方按 no_appdata 失败。
// ★ 返回值的持有者是**函数内 static**（进程生命周期）：NGX 可能在 Init_Ext 之后继续使用该指针，
//   函数返回后就不能是悬垂的栈对象。
static const std::wstring &resolve_appdata_path()
{
    static std::wstring cached;
    if (!cached.empty())
        return cached;

    const std::wstring base = self_module_dir();
    if (!base.empty())
    {
        std::wstring primary = base + L"\\ngx";
        if (ensure_dir_tree(primary))
        {
            cached = normalize_dir(primary);
            return cached;
        }
    }

    const std::wstring local = known_folder_localappdata();
    if (!local.empty())
    {
        std::wstring fallback = local + L"\\GenshinFsrBridge\\ngx";
        if (ensure_dir_tree(fallback))
        {
            cached = normalize_dir(fallback);
            return cached;
        }
    }

    return cached;   // 空 ⇒ 调用方 fail-open（reason=no_appdata）
}

// ============================================================================
// §3.10.2 配置快照（五行，顺序固定）+ §3.13 的 config_warn 尾行
// ★ 全部用 blog::Level::Error：§3.10.3 的硬约束要求这些判据能在**默认 NgxLogLevel=1** 下取值
//   （与 §3.10.1 把 init/create_feature/release_all/flags 定为 error(1) 是同一条理由）。
// ============================================================================
static void log_config_snapshot()
{
    // 第 1 行：三条前置的字面值（用户裁决：按 SDS 字面串打印）。
    // 到达本函数即证明三条前置成立（唯一调用链：Dx11FsrBridge.cpp 的
    // try_fsr2_translation_draw() 内 if (g_config.ffx12 && il2cpp_callsite::active()) 块
    // → ffx12::dispatch() 内的 ngx::dispatch 调用点 → 本函数）。
    // 行号会随其它改动漂移，故只写函数名，不写死行号。
    // fsr2_mode 的字面值取自 SDS §3.10.2 的固定串。
    log_ngx_error(L"ngx_core preconditions ffx12=1 fsr2_mode=2 callsite_active=1");

    // 第 2 行：**按 SDS 原样，不加 backend=**（用户裁决；后端状态行由桥在 blog::init() 后独立打）。
    log_ngx_error(L"ngx_core appid=%llu preset=%d loglevel=%d passthrough=%d",
                  g_app_id, g_render_preset, g_log_level, g_passthrough ? 1 : 0);

    // 第 3 行：HDR 取值链。
    // ★ ffx12_hdr 取 ffx12::hdr_input()（SDS §3.9A.2 的两个 getter 之一）。
    //   **不能**在本 TU 里 extern 声明或直接读 g_hdr_input：后者位于 Ffx12Backend.cpp 的匿名
    //   命名空间内（:371），本 TU 根本读不到（照抄即 C2065）。
    //
    // ★★ M2 修复（T-8 审核）：本行的 HDR 字段**必须与 build_create_flags 同源**，
    //   即直接调用 resolve_is_hdr()，而不是打印 g_hdr。
    //   缺陷原状：桥侧 set_hdr() 已零调用点（全仓只有定义处，无任何调用）
    //   ⇒ g_hdr 恒为初始值 true；本行却把它当“HDR 取值”打印
    //   ⇒ 实测出现 hdr_flag=1 / src=ngx_hdr(1) / ffx12_hdr=0 这种自相矛盾的错误证据
    //   （ffx12_hdr=0 明明是 SDR，hdr_flag 却是 1）。这是“制造证据”，比不打印更糟。
    //
    // ★ 本函数由 ensure_init() 第 0 步调用，此时 g_inst.key_valid 仍为 false、
    //   g_inst.color_fmt 仍是初值 DXGI_FORMAT_UNKNOWN(0) —— 这是**正常且应如实体现**的：
    //   此时没有输入纹理，resolve_is_hdr 走 format 回退并把 src 置为 “unknown”
    //   （而不是伪装成已知值）。真正的决策取证行是 build_create_flags 里的
    //   ngx_hdr_decision（M1 修好后在默认 NgxLogLevel=1 下可见）。
    //
    // ★ 字段名不得删改：§7.1-8 的 grep 判据依赖 hdr_flag= / src=ngx_hdr( / ffx12_hdr=。
    //   src_ngx_hdr(%d) 里的数字现在就是同一个 is_hdr（旧字段名的语义被修正为“实际决策”）。
    {
        // color_fmt：key_valid 为假时它必然是 UNKNOWN，显式取出来而不是硬写常量，
        // 这样万一将来快照点后移，也不会静默打印过期值。
        const DXGI_FORMAT snap_fmt = g_inst.key_valid ? g_inst.color_fmt
                                                      : DXGI_FORMAT_UNKNOWN;
        const char *snap_src = "?";
        const bool snap_hdr = resolve_is_hdr(snap_fmt, &snap_src);
        log_ngx_error(L"ngx_core hdr_flag=%d src=ngx_hdr(%d) ffx12_hdr=%d src_kind=%hs color_fmt=%d",
                      snap_hdr ? 1 : 0, snap_hdr ? 1 : 0, ffx12::hdr_input() ? 1 : 0,
                      snap_src, static_cast<int>(snap_fmt));
    }

    // 第 4 行：PathListInfo 的条目数与首项（%s 在 _vsnwprintf_s 里是**宽**串）。
    log_ngx_error(L"ngx_core featuredirs=%zu first=%s",
                  g_feature_dir_storage.size(),
                  g_feature_dir_storage.empty() ? L"(none)" : g_feature_dir_storage.front().c_str());

    // 第 5 行：FrameTimeDelta 与 reactive。
    log_ngx_error(L"ngx_core dt_flag=%d reactive=%d",
                  g_dt_enabled ? 1 : 0, g_use_reactive ? 1 : 0);

    // §3.13 末：非法值位图（只在非零时打；warn_to_string 返回窄串 ⇒ %hs）。
    if (g_config_warn != 0)
        log_ngx_error(L"ngx_core config_warn bits=%hs", warn_to_string(g_config_warn).c_str());
}

// ============================================================================
// §3.4.2 每条失败路径的统一收尾
//   ① 打 init ok=0 available=0 rc=0x%X reason=<why>（§3.10.1 的 ngx_core 形态）
//   ② 置 g_release_all —— §4.2 矩阵里 ensure_init 的 8 条失败（第 1-8 行）全是**确定性**的
//      （注册表/装载/入口/目录/目录创建/Init_Ext/取参数/可用性），重试不会好，让原生接手（§4.3）
//   ③ 置 5 秒退避窗口（§3.4.1）——**不设 g_inited**
// ============================================================================
static bool fail_init(const wchar_t *reason, unsigned rc)
{
    log_ngx_error(L"ngx_core init ok=0 available=0 rc=0x%X reason=%s", rc, reason);
    g_release_all.store(true, std::memory_order_relaxed);
    g_init_retry_after_us = qpc_us() + 5000000ull;   // 5000 ms，写死，不做 ini 键（§3.4.1）
    return false;
}

// ============================================================================
// §3.4 ensure_init()：十一步，**顺序写死，不得调整**（§3.4.2）
// 唯一调用点：ngx::dispatch() 内部、GetDevice 之后（§3.4.1）。**没有公开 init()**。
// 幂等；只在首次成功时 AddRef 存 g_device；失败路径不设 g_inited。
// ============================================================================
static bool ensure_init(ID3D11Device *dev)
{
    // ---- 幂等契约（§3.4.1 第一行）----
    if (g_inited)
        return g_available;

    // ---- 失败退避窗口（§3.4.1 / MAJ-13）：窗口内直接放弃，不重读注册表、不重打 error 行 ----
    if (g_init_retry_after_us != 0)
    {
        const std::uint64_t now = qpc_us();
        if (now < g_init_retry_after_us)
            return false;
        g_init_retry_after_us = 0;   // 窗口过期：允许重试一次
    }

    // ---- 首初始化计时（2026-11-09，PR #12 审查次要项 8）----
    // 为什么在这里：本函数在 draw 钩子内首次被调到（dispatch 第 3 步），
    //   第 2 步的 LoadLibraryW 与第 9 步的 Sleep(500) 都发生在**渲染线程**上
    //   ⇒ 首次进游戏会出现一次可见卡顿。本计时就是为了把这个卡顿变成**数字**，
    //   而不是靠"应该会卡一下"的推断（AGENTS.md §一.6 / §二.5）。
    // ★ 起点取在退避窗口之后：窗口内的早退不做任何工作，计它会把"没初始化"算成"初始化很慢"。
    // ★ 本计时**只在首次初始化路径上**（g_inited 短路 + 5 秒退避窗口之外），
    //   不是每帧探针 ⇒ 不触犯 §二.5 的热路径红线。
    const std::uint64_t init_t0 = qpc_us();

    // ---- 第 0 步：配置快照（只打一次，由 g_logged_config 保证）----
    if (!g_logged_config.exchange(true, std::memory_order_relaxed))
    {
        // ★ 目录表必须先解析：第 4 行要打真实的条目数，而 §3.4.2 要求快照在 LoadLibrary **之前**。
        resolve_feature_dirs();
        // T-10：探测显示器真实 HDR 状态（只探一次；QueryDisplayConfig 有枚举开销）
        g_displayconfig_hdr_state.store(probe_displayconfig_hdr(), std::memory_order_relaxed);
        refresh_hdr_probe_cache();   // ★ T-11：② 级探测落地后立刻刷新预计算缓存
        log_config_snapshot();
    }

    // ---- 第 1 步：注册表 HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore → FullPath ----
    // ⚠️ 无系统目录兜底：v0 的 %WINDIR%\System32\_nvngx.dll 是错的，真运行时在 DriverStore。
    wchar_t full_path[1024] {};
    {
        DWORD cb = sizeof(full_path);
        const LSTATUS reg = RegGetValueW(HKEY_LOCAL_MACHINE,
                                         L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore",
                                         L"FullPath", RRF_RT_REG_SZ, nullptr,
                                         full_path, &cb);
        if (reg != ERROR_SUCCESS || full_path[0] == L'\0')
            return fail_init(L"no_registry", static_cast<unsigned>(reg));
    }

    // ---- 第 2 步：LoadLibraryW(FullPath + L"\\_nvngx.dll") ----
    // ★ 句柄不存：v1 不调 Shutdown1、也从不 FreeLibrary（模块与进程同寿，§6-13）。
    const std::wstring nvngx_path = std::wstring(full_path) + L"\\_nvngx.dll";
    HMODULE module = LoadLibraryW(nvngx_path.c_str());
    if (module == nullptr)
        return fail_init(L"load_failed", static_cast<unsigned>(GetLastError()));

    // ---- 第 3 步：GetProcAddress × 10（清单见 §3.4.3）----
    // ★ 名字一律用 §3.4.3 表里核实过的导出名；**不从 nvsdk_ngx.h 抄**（头文件与真实 DLL 有 4 处差异）。
    g_init_ext = reinterpret_cast<PFN_NGX_Init_Ext>(
        GetProcAddress(module, "NVSDK_NGX_D3D11_Init_Ext"));
    g_get_caps = reinterpret_cast<PFN_NGX_GetCapabilityParameters>(
        GetProcAddress(module, "NVSDK_NGX_D3D11_GetCapabilityParameters"));
    g_create = reinterpret_cast<PFN_NGX_CreateFeature>(
        GetProcAddress(module, "NVSDK_NGX_D3D11_CreateFeature"));
    g_release = reinterpret_cast<PFN_NGX_ReleaseFeature>(
        GetProcAddress(module, "NVSDK_NGX_D3D11_ReleaseFeature"));
    g_eval = reinterpret_cast<PFN_NGX_EvaluateFeature>(
        GetProcAddress(module, "NVSDK_NGX_D3D11_EvaluateFeature"));
    g_shutdown = reinterpret_cast<PFN_NGX_Shutdown1>(
        GetProcAddress(module, "NVSDK_NGX_D3D11_Shutdown1"));
    g_alloc = reinterpret_cast<PFN_NGX_AllocateParameters>(
        GetProcAddress(module, "NVSDK_NGX_D3D11_AllocateParameters"));
    g_destroy = reinterpret_cast<PFN_NGX_DestroyParameters>(
        GetProcAddress(module, "NVSDK_NGX_D3D11_DestroyParameters"));
    g_scratch = reinterpret_cast<PFN_NGX_GetScratchBufferSize>(
        GetProcAddress(module, "NVSDK_NGX_D3D11_GetScratchBufferSize"));
    g_req = reinterpret_cast<PFN_NGX_GetFeatureRequirements>(
        GetProcAddress(module, "NVSDK_NGX_D3D11_GetFeatureRequirements"));
    {
        // §4.2 第 3 行：**打印所有 null 的名字**，不是只报第一个 —— 少一个入口和少五个
        // 在某台机器上是完全不同的诊断结论。
        std::string missing;
        auto note = [&missing](bool present, const char *name)
        {
            if (present)
                return;
            if (!missing.empty())
                missing += ",";
            missing += name;
        };
        note(g_init_ext != nullptr, "NVSDK_NGX_D3D11_Init_Ext");
        note(g_get_caps != nullptr, "NVSDK_NGX_D3D11_GetCapabilityParameters");
        note(g_create   != nullptr, "NVSDK_NGX_D3D11_CreateFeature");
        note(g_release  != nullptr, "NVSDK_NGX_D3D11_ReleaseFeature");
        note(g_eval     != nullptr, "NVSDK_NGX_D3D11_EvaluateFeature");
        note(g_shutdown != nullptr, "NVSDK_NGX_D3D11_Shutdown1");
        note(g_alloc    != nullptr, "NVSDK_NGX_D3D11_AllocateParameters");
        note(g_destroy  != nullptr, "NVSDK_NGX_D3D11_DestroyParameters");
        note(g_scratch  != nullptr, "NVSDK_NGX_D3D11_GetScratchBufferSize");
        note(g_req      != nullptr, "NVSDK_NGX_D3D11_GetFeatureRequirements");
        if (!missing.empty())
        {
            log_ngx_error(L"ngx_core missing_entry names=%hs", missing.c_str());
            return fail_init(L"missing_entry", 0);
        }
    }

    // ---- 第 4 步：组装 NVSDK_NGX_FeatureCommonInfo（§3.4.5）----
    NVSDK_NGX_FeatureCommonInfo fc {};
    fc.PathListInfo.Path   = g_feature_dir_ptrs.empty() ? nullptr : g_feature_dir_ptrs.data();
    fc.PathListInfo.Length = static_cast<unsigned int>(g_feature_dir_ptrs.size());
    fc.InternalData        = nullptr;
    fc.LoggingInfo.LoggingCallback        = (g_log_level > 0) ? &ngx_log_cb : nullptr;
    fc.LoggingInfo.MinimumLoggingLevel    = (g_log_level > 0) ? NVSDK_NGX_LOGGING_LEVEL_ON
                                                              : NVSDK_NGX_LOGGING_LEVEL_OFF;
    fc.LoggingInfo.DisableOtherLoggingSinks = false;   // 保持 false：让 NGX 自己的 nvngx.log 也继续写

    // ---- 第 5 步：InApplicationDataPath（§3.4.4）+ CreateDirectoryW ----
    const std::wstring &appdata = resolve_appdata_path();
    if (appdata.empty())
        return fail_init(L"no_appdata", static_cast<unsigned>(GetLastError()));
    log_ngx_error(L"ngx_core appdata=%s", appdata.c_str());

    // ---- 第 6 步：Init_Ext(app_id, appdata, dev, NVSDK_NGX_Version_API, &fc) ← 第 5 参是 fc ----
    const NVSDK_NGX_Result init_rc =
        g_init_ext(g_app_id, appdata.c_str(), dev, NVSDK_NGX_Version_API, &fc);
    if (init_rc != NVSDK_NGX_Result_Success)
        return fail_init(L"init", static_cast<unsigned>(init_rc));

    // ---- 第 7 步：GetCapabilityParameters（全局一份，**不调 Reset()**）----
    NVSDK_NGX_Parameter *params = nullptr;
    const NVSDK_NGX_Result caps_rc = g_get_caps(&params);
    if (caps_rc != NVSDK_NGX_Result_Success || params == nullptr)
        return fail_init(L"no_params", static_cast<unsigned>(caps_rc));
    g_params = params;

    // ---- 第 8 步：SuperSampling.Available ----
    int avail = 0;
    const NVSDK_NGX_Result avail_rc =
        g_params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
    if (avail_rc != NVSDK_NGX_Result_Success || avail == 0)
        return fail_init(L"unavailable", static_cast<unsigned>(avail_rc));

    // ---- 第 9 步：Sleep(500)（E1：Sleep(500) 后 CreateFeature 才有效；只在首次初始化付一次）----
    Sleep(500);

    // ---- 第 10 步：成功 ----
    g_inited    = true;
    g_available = true;
    g_device    = dev;   // ComPtr::operator= 即 AddRef；只在首次成功时走到这里（g_inited 短路）
    log_ngx_error(L"ngx_core init ok=1 available=1 rc=0x%X",
                  static_cast<unsigned>(NVSDK_NGX_Result_Success));
    // 2026-11-09（PR #12 审查次要项 8）：首次初始化的**实测**耗时（含 LoadLibraryW
    // 与第 9 步的 Sleep(500)）。这一行是**固定一次**的（不是条件触发）——
    // 判据是 first_init_us 的**值**，不是这行出不出现；名字里的 slow 指
    // 「它量的是那次卡顿」，不指「只有卡顿才打」。
    // ★ 为什么走 log_ngx_error（Error 级）而不是 Info：NgxLogLevel 默认 = 1（error），
    //   用 Info 的话默认配置下**这条永远看不到** —— 而它的全部价值就是被看到。
    //   与相邻的 init ok=1 行同级别，两行一起出现便于对账。
    // ★ 为什么方案选 C（接受现状 + 计时）而不是 A/B：见变更日志的评估。
    // ★ 失败路径不走到这里（fail_init 早退），但那些路径**也到不了**第 9 步的 Sleep(500)，
    //   所以漏掉的只是"初始化失败时的耗时"，不影响本行要量的那个卡顿。
    log_ngx_error(L"ngx_core init_slow first_init_us=%.1f",
                  static_cast<double>(qpc_us() - init_t0));
    return true;
}

// ============================================================================
// §3.8.1 / §3.8.2 / §3.8.3 / §3.9.6 的五个辅助函数（T-3）
// ★ 定义顺序写死：set_tex -> build_create_flags -> quality_from_ratio ->
//   apply_render_preset -> create_feature。后者调用前四个，顺序颠倒必 C3861
//   （SDS §3.8 开头的 v7 修复 D3 警告）。
// ============================================================================

// 3.9.6 唯一的资源参数写法：传 SRV/UAV 会在这里编译失败（无隐式转换），
// 而不是悄悄落到 void* 重载上（AGENTS.md 15 的 Ffx12CameraNear 教训）。
inline void set_tex(NVSDK_NGX_Parameter *p, const char *name, ID3D11Texture2D *t)
{
    p->Set(name, static_cast<ID3D11Resource *>(t));
}

// T-8：决定 IsHDR 标志。返回 true 表示置 IsHDR。
// 优先级：强制模式 > swapchain color space（SetColorSpace1 hook）> swapchain buffer 格式
//         > 输入纹理格式（最后兜底）。
// src 输出决策来源，供日志用（"forced-on"/"forced-off"/"swapchain"/"swapchain_format"/"format"/"unknown"）。
//
// ★★ M2 修复（T-8 审核）：本函数现在是 **IsHDR 的唯一决策点**。
//   两行日志（§3.10.2 快照第 3 行、§3.10.1 第 5 层的 create_flags 行）都改为**调用本函数**取值，
//   不再各自读 g_hdr。g_hdr 退化为"上一次决策的回写镜像"（见函数末尾），只为旧字段名/旧配置兼容而留。
//   理由：桥侧 set_hdr() 已零调用点 ⇒ g_hdr 恒 = true（初始值），把它当"HDR 取值"打印就是在
//   制造错误证据（实测：SDR 会话打出 hdr_flag=1 src=ngx_hdr(1) ffx12_hdr=0 而 create_flags 含 IsHDR）。
//
// ★★ M3 修复（T-8 审核，主会话裁决）：**回退层保守化**。
//   原实现把 R10G10B10A2_* 与 R16G16B16A16_TYPELESS 一并判成 HDR，而本机**存在 10-bit SDR 会话**
//   ⇒ 恰好复现 T-8 要消除的形态（10-bit SDR 下误报 HDR，Create_Flags 带 IsHDR 喂给 DLSS）。
//   裁决：swapchain 探测是首选来源，回退层只在探测失败时生效，因此必须"宁可判 SDR 也不误报 HDR"。
//   ⇒ 只有浮点 HDR（R16G16B16A16_FLOAT）才在回退层返回 true；
//     R10G10B10A2_TYPELESS / R10G10B10A2_UNORM / R16G16B16A16_TYPELESS / 其余一律 false（SDR）。
//   代价（已知、可接受）：真正的 10-bit HDR 会话若探测也失效，会少置一个 IsHDR 标志；
//   它属于"画质可能不最优"，而反过来是"喂错标志"，后者正是本轮要修的正确性问题。
// ★★ MINOR-1：探测状态在**函数入口 load 一次**，并把原始三态经 sw_state_out 交给调用方。
//   · 为什么放在强制模式判断**之前**：日志里的 sw_known / sw_hdr 是"探测到了什么"的取证字段，
//     与"IsHDR 取什么值"不是一回事。强制模式只改变**决策**，不该让取证字段变成假的 sw_known=0
//     ——否则 NgxHdr=1/0 的会话里就再也看不到 swapchain 探测结果，恰好是本轮要的证据。
//   · 为什么**只 load 一次**：两处各自 load 会重新引入撕裂（见下），这正是本条要消除的。
// ★★ T-11：①②③ 三级改为**读预计算缓存** g_hdr_probe_cache（事件驱动，写入方见
//   refresh_hdr_probe_cache 的注释）。本函数不再在每帧路径上重复「三次 load + 三层分支」。
//   · 热路径（src == nullptr，即 flags_input_bits 的每帧调用）：**只有 1 次 atomic load**。
//   · 冷路径（src != nullptr，仅日志/建 feature 时）：额外读两次 atomic 只为标注**来源**，
//     这不影响决策、也不在每帧路径上 —— 可观测性保留在冷路径，热路径零额外成本。
static bool resolve_is_hdr(DXGI_FORMAT color_fmt, const char **src, unsigned *sw_state_out)
{
    // ★ MINOR-1：探测状态是**一个** atomic<unsigned> 里的三态（0=未知 1=SDR 2=HDR），
    //   所以这里**一次 load** 就拿到完整状态：单次 load 天然无撕裂，
    //   **不存在「两个变量之间被改写」的窗口**（旧的双 atomic 写法正是错在这里）。
    //   ★ 本行是**全函数唯一**的 load（g_swapchain_hdr_state），决策与日志共用它。
    const unsigned sw_state = g_swapchain_hdr_state.load(std::memory_order_relaxed);
    if (sw_state_out) *sw_state_out = sw_state;   // ★ MINOR-1：**决策与日志共用的唯一一次 load**

    if (g_hdr_mode == 1)
    {
        if (src)
            *src = "forced-on";
        g_hdr = true;
        return true;
    }
    if (g_hdr_mode == 2)
    {
        if (src)
            *src = "forced-off";
        g_hdr = false;
        return false;
    }

    // ---- auto：①②③ 的预计算结果（T-11：事件驱动，不在每帧重算）----
    // ① SetColorSpace1 hook（最准）> ② Windows 显示配置 API（T-10）> ③ swapchain buffer 格式（M4-b）。
    // 三者的合并与优先级由 refresh_hdr_probe_cache() 在**写入点**完成；此处只读一个 atomic。
    // ★ 为什么 ③ 的判据是「只有 16F 浮点后缓冲才倾向 HDR」：DXGI **没有** GetColorSpace
    //   （已 grep Windows SDK 10.0.26100.0/shared/dxgi1_4.h：IDXGISwapChain3 只有
    //   CheckColorSpaceSupport(:121) / SetColorSpace1(:127)），BufferDesc.Format 只是**能力级**信号；
    //   且本机存在 10-bit SDR 会话 ⇒ 10-bit 一律按 SDR（M3 裁决口径，见上文）。
    const unsigned cached = g_hdr_probe_cache.load(std::memory_order_relaxed);
    if (cached != 0)
    {
        const bool hdr = (cached == 2);
        if (src)
        {
            // 取证：说明是哪一级贡献的（仍然要能区分，便于实机排查）。
            // ★ 这两次 load **只在冷路径**（src != nullptr）发生；每帧的 flags_input_bits
            //   传的是 nullptr ⇒ 热路径上只有上面那一次 cached 的 load。
            const unsigned sw = g_swapchain_hdr_state.load(std::memory_order_relaxed);
            if (sw != 0)                     *src = "swapchain";
            else if (g_displayconfig_hdr_state.load(std::memory_order_relaxed) != 0)
                                             *src = "displayconfig";
            else                             *src = "swapchain_format";
        }
        g_hdr = hdr;
        return hdr;
    }

    // ---- auto：④ 输入纹理格式（最后兜底；见上文的 M3 裁决）----
    if (src) *src = (color_fmt == DXGI_FORMAT_UNKNOWN) ? "unknown" : "format";
    const bool fmt_is_hdr = (color_fmt == DXGI_FORMAT_R16G16B16A16_FLOAT);
    g_hdr = fmt_is_hdr;   // M2：回写镜像，让旧字段（及旧日志判据）与实际决策一致
    return fmt_is_hdr;
}

// T-8 M4-a：把 CreateFlags 的**全部可变输入**压成一个位图，供 create_feature 记录、
// dispatch 第 5 步比对。新增一个可变输入时：在这里加一个 bit，并把记录/比较点都不用动
// （比较点只比整数）。**这是本文件唯一允许定义该位图的地方**，不要在别处另立一套。
//   bit0 = IsHDR          （resolve_is_hdr 的决策；T-8）
//   bit1 = DepthInverted  （ffx12::depth_inverted()；T-9 接线 —— 同样是 CreateFlags 的输入，
//                          若不纳入判据，换 Ffx12DepthInverted 后不会重建 feature，
//                          等于 T-9 只在首次 CreateFeature 生效，与 M4 同族）
//   bit2 = MVJittered     （ffx12::motion_vectors_jittered()；同上）
enum : unsigned {
    kFlagsInputIsHdr        = 1u << 0,
    kFlagsInputDepthInvert  = 1u << 1,
    kFlagsInputMvJittered   = 1u << 2,
};

// M4-a 的热路径取值器：**只算位图，不打日志、不碰 NGX 参数袋**。
// ★ 为什么不能直接调用 build_create_flags() 来比较：那个函数会打 ngx_hdr_decision（Error 级），
//   在 dispatch 第 5 步这种每帧路径上调它等于每帧一条日志 ⇒ 日志洪水 + 每帧格式化开销。
//   本函数是纯计算（只有 resolve_is_hdr 会回写 g_hdr 镜像，那是刻意的），可以安全每帧调用。
// ★ 位定义与 build_create_flags 里的 ev.bits **必须一致** —— 两者都只从下面这三个来源取，
//   新增一个可变输入时**两处都要加**（这是本文件第二个也是最后一个需要同步的点）。
static unsigned flags_input_bits(DXGI_FORMAT color_fmt)
{
    unsigned bits = 0;
    // ★ 不取探测状态快照：本函数只算位图，不打日志（见上方注释）。第 3 参用默认值。
    // ★★ T-11：这是**每帧**路径（dispatch 第 5 步）。resolve_is_hdr 现在只做
    //   「1 次 g_hdr_probe_cache 的 atomic load」—— ①②③ 三级的读+判断已搬到写入点
    //   （refresh_hdr_probe_cache），不再每帧重复；src == nullptr ⇒ 不做来源取证 load。
    if (resolve_is_hdr(color_fmt, nullptr))          bits |= kFlagsInputIsHdr;
    if (ffx12::depth_inverted())                     bits |= kFlagsInputDepthInvert;
    if (ffx12::motion_vectors_jittered())            bits |= kFlagsInputMvJittered;
    return bits;
}

// flags_input_bits / build_create_flags 共用的一次求值结果。
// ★ 为什么要"一次求值、多处取用"，而不是让每个调用点各自调 resolve_is_hdr：
//   resolve_is_hdr 读的是跨线程的 atomic 探测状态（g_swapchain_hdr_state / g_swapchain_format）。
//   同一个函数里调两次，两次之间写方可能更新 ⇒ 会出现"日志说 A、实际写进 NGX 的是 B"。
//   本次审核的 M2 正是"日志与决策不同源"这一类缺陷，不能再用同样的形态修它。
struct CreateFlagsEval
{
    unsigned int flags = 0;          // 真正写进 NGX 的 CreateFlags
    unsigned     bits  = 0;          // 同一组可变输入的位图（M4-a 的重建判据）
    bool         is_hdr = false;     // flags 里 IsHDR 位的来源，供日志取用
    const char  *hdr_src = "?";      // resolve_is_hdr 给出的决策来源
    unsigned     sw_state = 0;       // ★ MINOR-1：决策所依据的那一次 swapchain 三态
                                     //   load（0=未知 1=SDR 2=HDR），供日志派生 sw_known/sw_hdr。
                                     //   与 is_hdr 同源于 resolve_is_hdr 入口的那一次 load
                                     //   ⇒ **同一次求值内**日志的两个字段不会互相矛盾。
                                     //   ★ 不是"永远"：强制模式（NgxHdr=1/0）下 is_hdr 由
                                     //     g_hdr_mode 决定、与 sw_state 无关，此时
                                     //     is_hdr=1 + sw_hdr=0 是**正常**形态（见 resolve_is_hdr
                                     //     的 MINOR-1 注释），不是撕裂。
};

// 3.8.2：九 bit 只设四个。绝不设 DoSharpening（SR_DEPRECATED_SHARPENING 在 MSVC 下
// 展开为 [[deprecated]]，引用即 C4996）；AutoExposure 也不设（桥从不提供曝光纹理）。
// ★ 这是 **CreateFlags 的唯一构造点**：flags 与 bits 在同一个函数体内、对同一组输入求值一次。
//   （flags_input_bits() 是它的「只算位图」精简版，只给每帧的重建判据用；
//     两者必须对同一组输入给出同一个 bits —— 见 flags_input_bits 的注释。）
static CreateFlagsEval build_create_flags(DXGI_FORMAT color_fmt)
{
    CreateFlagsEval ev;
    // ★ MINOR-1：把探测状态一并取出 ⇒ 决策（is_hdr）与下方日志（sw_known/sw_hdr）来自同一次 load。
    ev.is_hdr  = resolve_is_hdr(color_fmt, &ev.hdr_src, &ev.sw_state);
    ev.flags   = 0;
    if (ev.is_hdr)                              ev.flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
                                                ev.flags |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;      // 1<<1，恒设
    if (ffx12::motion_vectors_jittered())       ev.flags |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;   // 1<<2，默认不设
    // T-9：按 Ffx12DepthInverted 配置（1<<3）
    if (ffx12::depth_inverted())
        ev.flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    // ---- 同一组输入的位图（M4-a）----
    if (ev.is_hdr)                              ev.bits |= kFlagsInputIsHdr;
    if (ffx12::depth_inverted())                ev.bits |= kFlagsInputDepthInvert;
    if (ffx12::motion_vectors_jittered())       ev.bits |= kFlagsInputMvJittered;
    // 1<<4 Reserved_0 / 1<<5 DoSharpening / 1<<6 AutoExposure / 1<<7 AlphaUpscaling / 1<<8 Reserved_8 一律不设
    // T-8：把决策来源打进日志（便于实机排查）。
    // ★★ M1 修复：**必须是 blog::Level::Error**。NgxLogLevel 默认 1 = error，而
    //   ngx_level_of(Info) = 2 ⇒ Info 档位在默认配置下**永不打印**，本行等于不存在，
    //   本轮改动也就无法验收。依据是本文件 §3.10.2 上方 655-659 行的明文约定
    //   「取证行必须用 Error，才能在默认 NgxLogLevel=1 下取值」。
    // ★ MINOR-1：sw_known / sw_hdr **必须由同一次 load 派生**（两个字段名保留 = grep 判据不变）。
    //   分两次 load 会重新引入撕裂：中间写方可能改写，日志就会打出「known=0 但 hdr=1」
    //   或「known=1 hdr=0」这类物理上不存在的组合，那正是本条要消除的假证据。
    //   ★ 已改为复用 resolve_is_hdr 通过 &ev.sw_state 交回的那次 load —— 本函数**不再自己 load**。
    //   ★ ev.sw_state 是 resolve_is_hdr **入口处**那唯一一次 load 的原始值，因此它描述的是
    //     「swapchain 探测到了什么」，**与是否处于强制模式无关**：NgxHdr=1/0 时 sw_known/sw_hdr
    //     仍然如实反映探测结果（改动前正是如此，本项改动不改变这两个字段的语义）。
    log_ngx_core(blog::Level::Error,
                 L"ngx_hdr_decision is_hdr=%d src=%hs mode=%d sw_known=%d sw_hdr=%d "
                 L"sw_fmt=%u color_fmt=%d depth_inv=%d flags=0x%X",
                 ev.is_hdr ? 1 : 0, ev.hdr_src, g_hdr_mode,
                 (ev.sw_state != 0) ? 1 : 0,
                 (ev.sw_state == 2) ? 1 : 0,
                 g_swapchain_format.load(std::memory_order_relaxed),
                 static_cast<int>(color_fmt),
                 (ev.bits & kFlagsInputDepthInvert) ? 1 : 0, ev.flags);
    return ev;
}

// 3.8.3 六档映射。一律返回枚举名，不写整数字面量（防 Ffx12CameraNear 式静默失效）。
// 本机 3840/1920 = 2.0 => 落 MaxPerf。
static NVSDK_NGX_PerfQuality_Value quality_from_ratio(std::uint32_t render_w, std::uint32_t display_w)
{
    if (render_w == 0) return NVSDK_NGX_PerfQuality_Value_MaxPerf;   // 防御除零

    const float ratio = static_cast<float>(display_w) / static_cast<float>(render_w);
    if (ratio > 2.0f && ratio <= 3.0f) return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    if (ratio > 1.7f && ratio <= 2.0f) return NVSDK_NGX_PerfQuality_Value_MaxPerf;      // <- 本机 2.0x
    if (ratio > 1.5f && ratio <= 1.7f) return NVSDK_NGX_PerfQuality_Value_Balanced;
    if (ratio > 1.3f && ratio <= 1.5f) return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    if (ratio > 1.0f && ratio <= 1.3f) return NVSDK_NGX_PerfQuality_Value_UltraQuality;
    return NVSDK_NGX_PerfQuality_Value_DLAA;
}

// 3.8.3：v1 六档全设同一值（等价于 OptiScaler 的 RenderPresetForAll）。
// v <= 0 直接 return，六个键一个都不设 —— 语义是「交给 NGX」，不是「默认预设」。
static void apply_render_preset(int v)
{
    if (v <= 0) return;
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,             v);
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,          v);
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,         v);
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,      v);
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, v);
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality,     v);
}

// 3.8.1 七项参数 + create flags + 六个 preset 键。
// 签名保持 SDS 原样；资源一律从 inst 读（T-4 建池、T-5 保证调用时已就位）。
static bool create_feature(NgxInstance &inst, const ffx12::FrameInput &in,
                           ID3D11DeviceContext *ctx)
{
    // 签名由 SDS 3.8.1 写死，但本函数不读 in（资源全在 inst 里）=> 显式标注避免 C4100。
    static_cast<void>(in);

    // 释放旧 handle。返回值【不处理】：handle 已失效时 NGX 返回失败，不影响后续 CreateFeature。
    // 不调 g_params->Reset()（3.9.5）—— 那会清掉 CreateFeature 设的 7 项。
    if (g_release && inst.feature) (void)g_release(inst.feature);
    inst.feature = nullptr;

    // 偏离 1（防御，不改变正常路径）：g_params / g_create 为 null 只可能是 ensure_init
    //   从未成功（T-5 的接线前提不成立）=> 直接失败，不空指针崩溃。
    if (g_params == nullptr || g_create == nullptr)
    {
        log_ngx_error(L"ngx_core create_feature ok=0 preset=%d reason=no_params", g_render_preset);
        inst.feature_create_failed = true;
        g_release_all.store(true, std::memory_order_relaxed);
        return false;
    }

    // 偏离 2（T-3 强制要求，E1 实测）：NGX 的 CreateFeature 完全不校验 resource 参数 ——
    //   四个纹理全传 nullptr 仍返回 0x1 Success 并给出有效 handle。若 T-5 接线出错，
    //   本函数会打出一条 ok=1 的假成功日志，而后续 EvaluateFeature 的失败码无法区分
    //   「参数错」还是「资源空」（同源证据：SDS :231 的 F-47b）。
    //   => 四资源各做一次显式空判；缺哪个就把名字打出来，且与 ok=1 行可区分。
    {
        std::string missing;
        auto note = [&missing](bool present, const char *name)
        {
            if (present)
                return;
            if (!missing.empty())
                missing += ",";
            missing += name;
        };
        note(inst.color.Get()  != nullptr, "color");
        note(inst.depth.Get()  != nullptr, "depth");
        note(inst.motion.Get() != nullptr, "motion");
        note(inst.output.Get() != nullptr, "output");
        if (!missing.empty())
        {
            // 与 ok=1 行的区别：无 rc= 字段、带 reason=null_resource 与具体缺哪个资源名。
            log_ngx_error(L"ngx_core create_feature ok=0 preset=%d reason=null_resource missing=%hs",
                          g_render_preset, missing.c_str());
            inst.feature_create_failed = true;   // S-81：同一对象生命周期内永不清锁
            g_release_all.store(true, std::memory_order_relaxed);
            return false;
        }
    }

    // 1. Width / Height
    g_params->Set(NVSDK_NGX_Parameter_Width,  inst.rw);
    g_params->Set(NVSDK_NGX_Parameter_Height, inst.rh);
    // 2. OutWidth / OutHeight
    g_params->Set(NVSDK_NGX_Parameter_OutWidth,  inst.dw);
    g_params->Set(NVSDK_NGX_Parameter_OutHeight, inst.dh);
    // 3. Render.Subrect.Dimensions（= 无动态分辨率时的全屏子矩形）
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,  inst.rw);
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, inst.rh);
    // 4. PerfQualityValue
    g_params->Set(NVSDK_NGX_Parameter_PerfQualityValue,
                  quality_from_ratio(inst.rw, inst.dw));   // 两参：先 render 后 display
    // 5. Create.Flags（3.8.2）
    // ★ T-8：同一份求值结果既用于 Set()、也用于日志与快照（见 CreateFlagsEval 的注释）。
    const CreateFlagsEval flag_eval = build_create_flags(inst.color_fmt);
    const unsigned int flags = flag_eval.flags;
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, flags);
    // 6. 四个纹理（走 set_tex，杜绝传视图）
    set_tex(g_params, NVSDK_NGX_Parameter_Color,         inst.color.Get());
    set_tex(g_params, NVSDK_NGX_Parameter_Depth,         inst.depth.Get());
    set_tex(g_params, NVSDK_NGX_Parameter_MotionVectors, inst.motion.Get());
    set_tex(g_params, NVSDK_NGX_Parameter_Output,        inst.output.Get());
    // 7. 六个 preset 键（v1 全设同一值）
    apply_render_preset(g_render_preset);

    const NVSDK_NGX_Result rr =
        g_create(ctx, NVSDK_NGX_Feature_SuperSampling, g_params, &inst.feature);

    log_ngx_core(blog::Level::Error, L"ngx_core create_feature ok=%d preset=%d rc=0x%X",
                 rr == NVSDK_NGX_Result_Success ? 1 : 0, g_render_preset, rr);
    if (rr != NVSDK_NGX_Result_Success)
    {
        inst.feature = nullptr;
        inst.feature_create_failed = true;
        // 偏离 3：SDS 3.8.1 的代码块漏了下面这一行，但 4.2 第 10 行与 3.9.7 明文要求
        //   置 g_release_all（确定性失败，重试不会好，让原生接手）。
        g_release_all.store(true, std::memory_order_relaxed);
        return false;                          // 不重试（3.9.7 / S-81）
    }
    // 偏离 3b：3.12.1 注 5 要求成功时写 feature_preset，而 3.8.1 的代码块漏了。
    //   该字段只用于日志与 Q18 对账，不参与重建判据。
    inst.feature_preset = g_render_preset;

    // ---- T-8 M4-a：记录本次 CreateFeature 实际使用的 CreateFlags 可变输入快照 ----
    // ★ 写入点唯一：只有这里（create_feature 成功之后）。dispatch 第 5 步只**比较**，不改写。
    // ★ 必须放在 rr == Success 之后：失败时 feature 没建起来，记快照毫无意义，
    //   而且会在 S-81 的锁存路径上留下一个"看着像已建"的假状态。
    inst.feature_flags_bits  = flag_eval.bits;
    inst.feature_flags_valid = true;

    // flags 取证行（3.10.1 第 5 层）。注意：本行与 3.10.2 的配置快照第 3 行
    //   （ngx_core hdr_flag=1 src=ngx_hdr(1) ffx12_hdr=0）是两条不同的行、两套格式串，
    //   必须各自能 grep -F 到，不得合并。
    //
    // ★★ M2 修复（T-8 审核）：hdr_flag= 与 src_ngx_hdr= **必须与 flags 里的 IsHDR 位同源**。
    //   缺陷原状：本行打的是 g_hdr，而 g_hdr 恒 true（set_hdr 已零调用点）
    //   ⇒ SDR 会话打出 hdr_flag=1 而 create_flags 含 IsHDR，等于用日志给错误决策背书。
    //   现在：hdr_flag 直接取 flags 的 IsHDR 位（= build_create_flags 实际写进 NGX 的那一位），
    //   src_ngx_hdr 取同一次 resolve_is_hdr 的决策（与 M1 的 ngx_hdr_decision 行严格一致）。
    //   ⇒ 二者同源于**本函数里 build_create_flags(inst.color_fmt) 的那一次调用**：
    //     hdr_flag 是 flag_eval.flags 的 IsHDR 位，src_ngx_hdr 是同一个 flag_eval.is_hdr，
    //     而 ev.flags 的 IsHDR 位在 build_create_flags 内就是由 ev.is_hdr 置的。
    //   ★ 这保证的是「**本函数的单次求值内**二者一致」，不是「永远不会矛盾」：
    //     若将来把这两处拆成两次 build_create_flags 调用、或在 build_create_flags 里改用
    //     别的输入置 IsHDR 位而不同步 is_hdr，二者就会分叉。改动上述任一处时须一并复核本行。
    // ★ 字段名一个都不删：§7.1-8 的 grep 判据依赖 hdr_flag= / src_ngx_hdr= / ffx12_hdr=。
    log_ngx_core(blog::Level::Error,
        L"ngx_core create_flags=0x%X bits=%hs autoexposure=0 "
        L"src=ngx_no_exposure_texture hdr_flag=%d src_ngx_hdr=%d ffx12_hdr=%d src_kind=%hs",
        flags, flags_to_string(flags).c_str(),
        (flags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) ? 1 : 0,
        flag_eval.is_hdr ? 1 : 0, ffx12::hdr_input() ? 1 : 0, flag_eval.hdr_src);
    return true;
}

// §3.10.1 实例层日志。格式串必须与 §3.10.1 逐字相同，否则 §7.1-8 的 grep 判据失效。
static void log_ngx_inst_switch(std::uint64_t from, std::uint64_t to, std::uint64_t rebuild_us)
{
    log_ngx_core(blog::Level::Error,
                 L"ngx_inst switch from=0x%llX to=0x%llX rebuild_us=%d",
                 static_cast<unsigned long long>(from),
                 static_cast<unsigned long long>(to),
                 static_cast<int>(rebuild_us));
}

// 五张纹理的 desc 差异全部用这张表表达，不要每个字段手抄一遍
struct PoolDesc { DXGI_FORMAT fmt; UINT bind; const char *name; };

static bool make_pool_texture(ID3D11Device *dev, const PoolDesc &d,
                              UINT w, UINT h, ComPtr<ID3D11Texture2D> &out, int inst_id)
{
    D3D11_TEXTURE2D_DESC td {};
    td.Width            = w;
    td.Height           = h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = d.fmt;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = d.bind;
    td.CPUAccessFlags   = 0;
    td.MiscFlags        = 0;          // ★ §3.12.4：不要 SHARED / NT 共享
    const HRESULT hr = dev->CreateTexture2D(&td, nullptr, out.GetAddressOf());
    if (FAILED(hr))
    {
        // 非致命重复性错误（每帧重试建池）⇒ 按槽独立限频，见 log_ngx_error_throttled 的注释。
        // ★ 为什么不是致命：create_pool 失败只让本帧放弃，不置 g_release_all（S-81 的锁存由
        //   调用方按尺寸/格式变化决定），因此高频重复是可能的、也必须限频。
        log_ngx_error_throttled<log_throttle_slot::Slot::PoolTexFail>(
            L"ngx_inst pool tex fail inst=%d name=%hs w=%u h=%u fmt=%d hr=0x%08X",
            inst_id, d.name, w, h, static_cast<int>(d.fmt),
            static_cast<unsigned>(hr));
        return false;
    }
    return true;
}

// ============================================================================
// ★★ T-4 第 2 轮修复（MAJOR-1）：create_pool / destroy_instance 共用的「清本池资源」段。
//
// 为什么要抽出来：create_pool 现在要满足「先清后建」契约（可从任意状态调用），而
// destroy_instance 里本来就有同一段清理。两处共用一份，避免下一次改动只改一边。
//
// ★★ 本函数**只清资源**，绝不碰 feature_create_failed / key_valid / feature 句柄：
//   create_pool 在已建池实例上重入时会调用它，而 S-81 规定「尺寸/格式变化**不清锁、不重试**」。
//   若这里顺手把 feature_create_failed 置 false，0xBAD0000B 就会每帧重试 ——
//   这是本次修复最容易写错的地方。锁存与 key_valid 归 destroy_instance() 管，
//   feature 是裸 handle 也归 destroy_instance() 手工 Release。
//
// ★ 定义位置必须早于 create_pool 与 destroy_instance（否则 C3861）。
// ============================================================================
static void release_pool_resources(NgxInstance &inst)
{
    // ---- 5 张纹理（§3.12.3 规则 2 要求 Reset 的全部 5 张）----
    inst.color.Reset();  inst.depth.Reset();  inst.motion.Reset();
    inst.reactive.Reset(); inst.output.Reset();

    // ---- 池内视图 + cb：1 SRV + 3 UAV + 1 cb ----
    // ★ 只 Reset 真实存在的字段。**没有** output_uav / depth_srv / motion_srv / reactive_srv
    //   （§3.3.4.1 明文「v1 不用，不要加」；写过它们会 C2039）
    inst.color_srv.Reset();
    inst.depth_uav.Reset(); inst.motion_uav.Reset();
    inst.reactive_uav.Reset(); inst.cb.Reset();

    // ---- 输入侧 SRV 与身份缓存（depth_src_last / motion_src_last 是裸指针，只置空不 Release）----
    inst.depth_src_srv.Reset(); inst.motion_src_srv.Reset();
    inst.depth_src_last = nullptr;  inst.motion_src_last = nullptr;

    // ★★ 必须同步复位 cb 的缓存判据（SDS §3.12.3 规则 2 明文要求「cb + 3 UAV + 3 SRV」——
    //   SDS 早期把 SRV 数误记为 4，实际只有 color_srv + depth_src_srv + motion_src_srv 这 3 个，
    //   而 cb 的缓存判据是 cb 状态的一部分）。否则陷阱 1 / 1b 会在两条路径上复活：
    //     ① 实例重建路径（make/destroy 之间）；
    //     ② 已建池实例上的 create_pool 重入路径（T-5 §3.7 第 5 步）。
    //   create_pool 每次新建 cb（内容 = D3D11 零值），但 cb_uploaded_* 若是旧值，
    //   upload_cb 会判定「值没变」直接 return ⇒ 新 cb 保持全 0 ⇒ 深度 CS 写出全 0
    //   （表现：静止还行、一动就糊）。
    //   （E1 实测：实例切换后 cb.b0.x = 0.0，期望 1.0；重入路径同理）
    //   纪律：cb 缓存的生命周期 == cb 对象的生命周期。
    inst.cb_uploaded_depth = std::numeric_limits<float>::quiet_NaN();
    inst.cb_uploaded_flip  = std::numeric_limits<float>::quiet_NaN();
}

// TYPELESS 资源建 SRV 时必须显式给 typed 格式（D3D11 规定）：
// TYPELESS 在 CreateTexture2D 合法，但在 CreateShaderResourceView 的 desc 里**非法**，
// 会返回 E_INVALIDARG(0x80070057)。这是编译期零告警、只在运行期炸的形态。
// ★ 本机实测（2026-11-08）：SDR 下游戏给 R8G8B8A8_TYPELESS(27)，
//   旧代码只映射 R10G10B10A2_TYPELESS ⇒ 每帧 color_srv fail ⇒ NGX 完全不出图；
//   开 HDR 时游戏给 R8G8B8A8_UNORM(28)（已是 typed）⇒ 侥幸绕过，缺陷被掩盖。
static DXGI_FORMAT srv_typed_format(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    // ---- T-8 m1：补齐 3 个既有 typed 格式 ----
    // 这三个**本身已是 typed**，形式上 default 也能原样返回 —— 那为什么还要显式列出来？
    //   因为这三种是本机/目标平台真会出现的 SDR 半浮点与广色域格式，而 default 分支的语义是
    //   "我不认识它、默认它合法"。T-7 的 BLOCKER 正是"default 把未知格式原样放行"造成的，
    //   所以凡是**已知会出现**的 typed 格式都写明在这里：既是文档，也让将来若改 default 策略
    //   （例如对未知格式 fail-closed）时不会误伤这三个。
    // ★ 注意：这三个不是 TYPELESS 的孪生，映射是恒等映射（typed -> 同名 typed）。
    case DXGI_FORMAT_R16G16B16A16_UNORM:    return DXGI_FORMAT_R16G16B16A16_UNORM;      // 11
    case DXGI_FORMAT_R16G16B16A16_SNORM:    return DXGI_FORMAT_R16G16B16A16_SNORM;      // 13
    // R10G10B10_XR_BIAS_A2_UNORM(89)：DXGI 里**没有**它的 TYPELESS 孪生（TYPELESS 只有
    //   R10G10B10A2_TYPELESS(23)），所以它是"本身已是 typed、无需映射"的那一类，
    //   这里同样是恒等映射 —— 列出来只为覆盖"广色域 10-bit 后缓冲"这一已知形态。
    case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM: return DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM;  // 89
    default:                                 return f;   // 其余已是 typed，原样返回
    }
}

// ★★ T-4 第 2 轮修复（MAJOR-1）：「create_pool 可从任意状态调用」是本函数的契约。
//   旧实现在已建池实例上重入时用 GetAddressOf() 直接覆盖指针（不 Release）⇒
//   每次重入泄漏 5 张纹理 + 4 个视图 + 1 个 cb；且 cb_uploaded_* 不复位 ⇒
//   新 cb 全 0 且 upload_cb 被短路 ⇒ 陷阱 1b 复活。
//   T-5 §3.7 第 5 步（尺寸/格式变化）直接再调一次本函数，不需要自己清理。
static bool create_pool(ID3D11Device *dev, NgxInstance &inst,
                        std::uint32_t rw, std::uint32_t rh,
                        std::uint32_t dw, std::uint32_t dh,
                        std::uint64_t key)
{
    // ---- 「先清后建」：必须在读 inst.color_fmt / inst.output_fmt **之前**完成 ----
    // ★ 这里**只清资源**，绝不调用 destroy_instance()：那会把 feature_create_failed
    //   置 false，而 S-81 规定「尺寸/格式变化不清锁、不重试」⇒ 0xBAD0000B 每帧重试。
    release_pool_resources(inst);

    // ---- 失败路径统一回滚 ----
    // 契约的另一半：**返回 false 时 inst 上不留半成品**。
    // 以前这条由 create_instance() 的 destroy_instance() 兜住，但那条兜底路径顺带清锁
    // （feature_create_failed = false / key_valid = false），不是 create_pool 该管的事；
    // 直接调用 create_pool 的路径（§3.7 第 5 步重入）则完全没人收尾。
    // 这里只调 release_pool_resources：**不碰 feature / feature_create_failed / key_valid**。
    const auto fail = [&inst]() { release_pool_resources(inst); return false; };

    // 颜色/输出沿用游戏侧格式（§3.12.1 规则②：output 保持 TYPELESS）
    const DXGI_FORMAT color_fmt  = inst.color_fmt;    // = in.color 的 desc.Format
    const DXGI_FORMAT output_fmt = inst.output_fmt;   // = in.output_target 的 desc.Format

    const PoolDesc texs[5] = {
        { color_fmt,                 D3D11_BIND_SHADER_RESOURCE,                               "color"    },
        { DXGI_FORMAT_R32_FLOAT,     D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, "depth"    },
        { DXGI_FORMAT_R16G16_FLOAT,  D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, "motion"   },
        { DXGI_FORMAT_R8_UNORM,      D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, "reactive" },
        { output_fmt,                D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, "output"   },
    };
    // ★★ 必须用 std::addressof，**不能用 &inst.X** ——
    //   ComPtr 重载了 operator&（返回 ComPtrRef，其 operator T*() 的副作用是把 ptr_ 置空），
    //   写 { &inst.color, ... } 会把刚建好的 5 张纹理**全部置空**（E1 实测：AFTER a=0x0 b=0x0），
    //   随后 CreateUnorderedAccessView(nullptr) 返回 E_INVALIDARG 0x80070057 ⇒ create_pool 恒 false
    //   ⇒ NGX 永远不出图，而日志只有一行 uav fail，极易误判为显卡不支持。
    //   ★ 编译期零 warning，只能靠运行期抓。
    ComPtr<ID3D11Texture2D> *dst[5] = {
        std::addressof(inst.color),   std::addressof(inst.depth),
        std::addressof(inst.motion),  std::addressof(inst.reactive),
        std::addressof(inst.output)
    };

    int id = static_cast<int>(key & 0xFFFF);   // 仅用于失败日志定位是哪次建池
    for (int k = 0; k < 5; ++k)
    {
        const bool render_size = (k == 0 || k == 1 || k == 2 || k == 3);
        const UINT w = render_size ? rw : dw;
        const UINT h = render_size ? rh : dh;
        if (!make_pool_texture(dev, texs[k], w, h, *dst[k], id)) return fail();
    }

    // 池内视图：color_srv（SRV）+ depth_uav / motion_uav / reactive_uav（UAV）
    // ★ output 不建 UAV（§3.12.1 规则③：NGX 自己按 typed 孪生建）
    D3D11_SHADER_RESOURCE_VIEW_DESC sd {};
    sd.Format        = srv_typed_format(color_fmt);
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    HRESULT hr = dev->CreateShaderResourceView(inst.color.Get(), &sd, inst.color_srv.GetAddressOf());
    // 非致命重复性错误（每次建池都可能重犯）⇒ 按槽独立限频。
    if (FAILED(hr))
    {
        log_ngx_error_throttled<log_throttle_slot::Slot::ColorSrvFail>(
            L"ngx_inst color_srv fail hr=0x%08X", static_cast<unsigned>(hr));
        return fail();
    }

    // 三个 UAV：TYPELESS 资源必须显式给 typed 格式（D3D11 规定）
    // ★★ 同上：std::addressof，不要 &inst.X（否则三个 UAV 源纹理被置空 ⇒ E_INVALIDARG）
    ComPtr<ID3D11Texture2D> *uav_tex[3] = {
        std::addressof(inst.depth), std::addressof(inst.motion), std::addressof(inst.reactive)
    };
    ComPtr<ID3D11UnorderedAccessView> *uav_out[3] = {
        std::addressof(inst.depth_uav), std::addressof(inst.motion_uav), std::addressof(inst.reactive_uav)
    };
    const DXGI_FORMAT uav_fmt[3] = { DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R8_UNORM };
    for (int k = 0; k < 3; ++k)
    {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud {};
        ud.Format        = uav_fmt[k];
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        ud.Texture2D.MipSlice = 0;
        hr = dev->CreateUnorderedAccessView(uav_tex[k]->Get(), &ud, uav_out[k]->GetAddressOf());
        if (FAILED(hr))
        {
            // 非致命重复性错误 ⇒ 按槽独立限频。
            log_ngx_error_throttled<log_throttle_slot::Slot::UavFail>(
                L"ngx_inst uav fail k=%d hr=0x%08X", k, static_cast<unsigned>(hr));
            return fail();
        }
    }

    // 16 B 常量缓冲（§3.6.5：b0 由 upload_cb 填）
    D3D11_BUFFER_DESC bd {};
    bd.ByteWidth      = 16;
    bd.Usage          = D3D11_USAGE_DEFAULT;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = 0;
    hr = dev->CreateBuffer(&bd, nullptr, inst.cb.GetAddressOf());
    // 非致命重复性错误 ⇒ 按槽独立限频。
    if (FAILED(hr))
    {
        log_ngx_error_throttled<log_throttle_slot::Slot::CbFail>(
            L"ngx_inst cb fail hr=0x%08X", static_cast<unsigned>(hr));
        return fail();
    }

    return true;
}

// destroy_instance：幂等，未创建时调用安全。
// ★ T-4 第 2 轮修复（MAJOR-1）：资源清理段抽到 release_pool_resources()，与 create_pool 共用。
//   **对外行为完全不变**：仍 g_release(feature)、仍 feature_create_failed = false、
//   仍 key_valid = false。create_pool 不能承担这三件事（见 release_pool_resources 上方注释）。
static void destroy_instance()
{
    // ★ feature 是裸 handle，ComPtr 管不到，必须手工释放
    if (g_inst.feature)
    {
        if (g_release) g_release(g_inst.feature);
        g_inst.feature = nullptr;
    }
    g_inst.feature_create_failed = false;   // 对象销毁 ⇒ 锁存随对象消失（不是「清锁重试」）
    // T-8 M4-a：feature 句柄没了 ⇒ 那份「本次 CreateFlags 输入」快照也就不再描述任何活对象。
    // ★ 必须一起清：否则 key 切换后（destroy + create_instance）旧快照会残留，
    //   若此时位图恰好不同，第 5 步会判定 flags_changed 再白重建一次刚建好的资源池。
    g_inst.feature_flags_bits  = 0;
    g_inst.feature_flags_valid = false;
    // 5 纹理 + cb + 3 UAV + 3 SRV 全是 ComPtr，置空即自动 Release
    // （★ 含 cb_uploaded_* 复位与 depth_src_last / motion_src_last 归零）
    release_pool_resources(g_inst);
    g_inst.key_valid = false;
}

// create_instance：写 key/尺寸/格式 → 建池。失败时自己回滚。
static bool create_instance(std::uint64_t key, ID3D11Device *dev,
                            std::uint32_t rw, std::uint32_t rh,
                            std::uint32_t dw, std::uint32_t dh,
                            DXGI_FORMAT color_fmt, DXGI_FORMAT output_fmt)
{
    // 先把尺寸/格式写回 inst —— create_pool() 直接读 inst.color_fmt/output_fmt
    g_inst.key        = key;
    g_inst.rw = rw;  g_inst.rh = rh;
    g_inst.dw = dw;  g_inst.dh = dh;
    g_inst.color_fmt  = color_fmt;
    g_inst.output_fmt = output_fmt;
    g_inst.first      = true;
    g_inst.eval_fail_streak = 0;

    if (!create_pool(dev, g_inst, rw, rh, dw, dh, key))
    {
        destroy_instance();     // ★ 自己回滚：半成品资源不能留
        return false;
    }
    g_inst.key_valid = true;
    return true;
}

// ensure_instance：对外唯一入口。key 未变则直接返回；变了则 destroy + create。
// **失败返回 false，调用方 return false；不置 g_release_all**（建池失败可能是瞬时的）。
static bool ensure_instance(std::uint64_t key, ID3D11Device *dev,
                            std::uint32_t rw, std::uint32_t rh,
                            std::uint32_t dw, std::uint32_t dh,
                            DXGI_FORMAT color_fmt, DXGI_FORMAT output_fmt)
{
    // 同 key 且已建好：直接复用。
    // ★ key == 0 是合法值（§4.4 B-7），**不能**用 key != 0 判断「已建」。
    if (g_inst.key_valid && g_inst.key == key)
    {
        g_inst.last_used_tick = GetTickCount64();
        return true;
    }

    // ---- 切换：先释放，再重建 ----
    const std::uint64_t t0 = qpc_us();
    destroy_instance();                  // ReleaseFeature + ComPtr 自动放纹理/UAV/SRV/cb
    if (!create_instance(key, dev, rw, rh, dw, dh, color_fmt, output_fmt))
    {
        return false;                    // 建池失败：本帧放弃（create_instance 已回滚）
    }
    const std::uint64_t dt = qpc_us() - t0;
    log_ngx_inst_switch(g_prev_key, key, dt);
    g_prev_key = key;
    return true;
}

static bool ensure_shaders()
{
    if (g_shaders_ready) return true;
    if (g_shader_compile_failed || g_shader_object_failed) return false;

    // 深度提取：ffx12::g_depth_extract_hlsl（从 Ffx12ShaderSource.h 来）
    {
        ID3DBlob *code = nullptr, *err = nullptr;
        const HRESULT hr = D3DCompile(ffx12::g_depth_extract_hlsl,
                                      sizeof(ffx12::g_depth_extract_hlsl) - 1,
                                      "ngx_depth_extract", nullptr, nullptr,
                                      "main", "cs_5_0", 0, 0, &code, &err);
        // ★ 判 hr 且判 code 非空：失败时 code == nullptr，
        //   紧接着的 code->GetBufferPointer() 是【空指针解引用 ⇒ 崩溃】。
        if (FAILED(hr) || !code)
        {
            log_ngx_error(L"ngx_core compile fail stage=depth hr=0x%08X", static_cast<unsigned>(hr));
            if (err)
            {
                OutputDebugStringA(static_cast<const char *>(err->GetBufferPointer()));
                err->Release();
            }
            if (code) code->Release();
            g_shader_compile_failed = true;
            return false;
        }
        if (err)
        {
            err->Release();
            err = nullptr;
        }

        const HRESULT hr2 = g_device->CreateComputeShader(
            code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_depth_cs);
        code->Release();
        code = nullptr;
        if (FAILED(hr2))
        {
            log_ngx_error(L"ngx_core create_cs fail stage=depth hr=0x%08X", static_cast<unsigned>(hr2));
            g_shader_object_failed = true;
            return false;
        }
    }

    // motion 解码：**只编默认变体**（FFX12_MOTION_RAW / FFX12_MOTION_DEADZONE 都不定义）
    {
        ID3DBlob *code = nullptr, *err = nullptr;
        const D3D_SHADER_MACRO macros[] = { { nullptr, nullptr } };   // 空宏表
        const HRESULT hr = D3DCompile(ffx12::g_motion_decode_d11_hlsl,
                                      sizeof(ffx12::g_motion_decode_d11_hlsl) - 1,
                                      "ngx_motion_decode", macros, nullptr,
                                      "main", "cs_5_0", 0, 0, &code, &err);
        if (FAILED(hr) || !code)
        {
            log_ngx_error(L"ngx_core compile fail stage=motion hr=0x%08X", static_cast<unsigned>(hr));
            if (err)
            {
                OutputDebugStringA(static_cast<const char *>(err->GetBufferPointer()));
                err->Release();
            }
            if (code) code->Release();
            g_shader_compile_failed = true;
            return false;
        }
        if (err)
        {
            err->Release();
            err = nullptr;
        }

        const HRESULT hr2 = g_device->CreateComputeShader(
            code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_motion_cs);
        code->Release();
        code = nullptr;
        if (FAILED(hr2))
        {
            log_ngx_error(L"ngx_core create_cs fail stage=motion hr=0x%08X", static_cast<unsigned>(hr2));
            g_shader_object_failed = true;
            return false;
        }
    }

    g_shaders_ready = true;
    return true;
}

// 每帧两段 CS 之前调用一次；值与上次相同则跳过上传。
// ★ 缓存初值是 quiet_NaN()（不是 0.0f）⇒ 保证首帧必上传一次（§3.6.5）。
static void upload_cb(ID3D11DeviceContext *ctx, NgxInstance &inst,
                      float depth_scale, float flip)
{
    if (inst.cb_uploaded_depth == depth_scale && inst.cb_uploaded_flip == flip)
        return;
    const float vals[4] = { depth_scale, flip, 0.0f, 0.0f };   // 16 B，正好 b0
    ctx->UpdateSubresource(inst.cb.Get(), 0, nullptr, vals, 0, 0);
    inst.cb_uploaded_depth = depth_scale;
    inst.cb_uploaded_flip  = flip;
}

// 深度 CS 的 t0：格式三分支 + 身份缓存。
// ★ 必须走 GetDesc(&td) 出参形式 —— ID3D11Texture2D 没有无参 GetDesc() 重载，
//   写 GetDesc().Format 会 C2660。
static bool ensure_depth_src_srv(const ffx12::FrameInput &in, NgxInstance &inst)
{
    // ★★ T-4 第 2 轮修复（MAJOR-2）：入口判空。旧代码直接 in.depth->GetDesc(&dtd)，
    //   **E0 推断**（空指针解引用；未做崩溃复现归档故不标 E1，见 SDS §0 的 S-109）：注入型 DLL 里是游戏进程崩溃而非 fail-open。
    //   契约与函数尾部一致：false ⇒ 本帧放弃（fail-open，调用方走原生放行）。
    //   ★ 必须在 GetDesc 之前 —— 挪到后面等于没修。
    if (in.depth == nullptr)
        return false;

    DXGI_FORMAT srv_fmt = DXGI_FORMAT_R32_FLOAT;
    D3D11_TEXTURE2D_DESC dtd {};
    in.depth->GetDesc(&dtd);
    switch (dtd.Format)
    {
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
            srv_fmt = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            break;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
            // 官方约定：D32S8/R32G8X24 深度需用 R32_FLOAT_X8X24 视图读深度分量
            // （普通 R32_FLOAT 视图读不到）
            srv_fmt = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            break;
        default:
            srv_fmt = DXGI_FORMAT_R32_FLOAT;
            break;
    }

    if (!inst.depth_src_srv || inst.depth_src_last != in.depth)
    {
        inst.depth_src_srv.Reset();
        D3D11_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = srv_fmt;
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        if (FAILED(g_device->CreateShaderResourceView(in.depth, &srv, inst.depth_src_srv.GetAddressOf())))
            inst.depth_src_srv.Reset();
        inst.depth_src_last = inst.depth_src_srv ? in.depth : nullptr;
    }
    return inst.depth_src_srv != nullptr;
}

// motion CS 的 t0：格式固定 R10G10B10A2_UNORM（没有三分支）。
// ★ 不能照 in.motion 的实际格式：游戏 motion 是 R10G10B10A2_TYPELESS(23)，
//   TYPELESS 不能直接建 SRV（E_INVALIDARG），且解码公式假定 rg 是 UNORM [0,1]。
static bool ensure_motion_src_srv(const ffx12::FrameInput &in, NgxInstance &inst)
{
    // ★★ T-4 第 2 轮修复（MAJOR-2）：与 ensure_depth_src_srv 对齐的显式判空。
    //   本函数当前不会崩（不调 GetDesc），但不判空时 CreateShaderResourceView(nullptr)
    //   只是碰巧失败并被下面的 FAILED 分支吃掉 —— 依赖"碰巧"是缺陷，显式判空。
    //   同族函数行为必须对称（审核 MAJOR-2 的原始意见）。
    if (in.motion == nullptr)
        return false;

    if (!inst.motion_src_srv || inst.motion_src_last != in.motion)
    {
        inst.motion_src_srv.Reset();
        D3D11_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format        = DXGI_FORMAT_R10G10B10A2_UNORM;   // 固定
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        if (FAILED(g_device->CreateShaderResourceView(in.motion, &srv, inst.motion_src_srv.GetAddressOf())))
            inst.motion_src_srv.Reset();
        inst.motion_src_last = inst.motion_src_srv ? in.motion : nullptr;
    }
    return inst.motion_src_srv != nullptr;
}

// ============================================================================
// 2026-11-09（PR #12 审查次要项 9）：为什么两个 CS **用完置空**而不是「还原原值」
// ============================================================================
// 审查意见：用完置空（null）而非还原游戏原本绑定的 SRV，多组件共存（桥 + OptiScaler +
//   ReShade）时可能互相干扰。
// 结论：**保留置空**，理由与量化依据如下（不是"看起来更省事"）。
//
// ① 还原需要多少个 Get*（本文件实测点数，非估计）：
//    本文件两个 CS 各绑定 4 类槽位 —— CS shader / SRV0 / CB0 / UAV0..N。
//    ⇒ 还原必须成对调用 4 个 Get*：CSGetShader、CSGetShaderResources(0,1)、
//       CSGetConstantBuffers(0,1)、CSGetUnorderedAccessViews(0,N)。
//    两个 CS 每帧各跑一次 ⇒ **每帧 8 次 Get* + 8 次 Release**（每个 Get 返回的接口都
//    AddRef 一次，必须逐个 Release，漏一个就是永久泄漏，见 AGENTS.md §三 #12）。
//    而当前实现的解绑侧是 **8 次 Set***，一次 Get 都没有。
//    ⇒ 还原方案的代价是「+8 Get* +8 Release / 帧」，不是任务书设想的 2-3 个 Get*。
//
// ② 为什么不能改读桥自己的影子状态（g_state.cs_srvs / cs_uavs / cs_shader / cs_cbs）：
//    那四个影子数组只由 hooked_cs_set_* 维护，而 **CS 系列钩子在 Release 构建里根本不装**
//    （Dx11FsrBridge.cpp 的 patches 表：k_idx_cs_set_* 整组在
//    #if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME) 内）。生产构建读到的会是**从不更新**的
//    旧值 ⇒ 还原成任意别的资源，比置空更危险。故唯一可行路径就是上面那 8 个 Get*。
//
// ③ 为什么 8 个 Get* 在本项目算"代价明显"而不是"可以接受"：
//    · CSGetUnorderedAccessViews / CSGetShaderResources 在 D3D11 立即上下文上是
//      **回读型**调用：运行时为了让返回值准确，必须把延迟提交的状态落定。它们位于
//      ngx::dispatch 的第 8/9 步 —— 该函数由 ffx12::dispatch 在**每个 FSR2 派发点**上调用
//      （实机约每帧一次，与 NgxBackend 的 g_ngx_frame_count 同频），属于
//      AGENTS.md §二.5 点名的"每帧/每 draw 路径"。
//    · 附带 8 次 AddRef/Release 的原子引用计数操作，同样是每帧固定开销。
//    ★ 诚实标注：本条的 ③ 是 **E0（读 API 语义与代码结构推断）**，
//      **没有**做过「关/开还原」的帧数 A/B 实测（AGENTS.md §二.5 明确要求先验证探针
//      本身无扰动再谈收益，本轮不具备该条件）。因此本决策的依据是 ①（可数的事实：
//      8 Get* + 8 Release vs 0）与 ②（Release 构建没有影子状态可用），不是性能实测。
//
// ④ 置空方向为什么是**安全侧**：绑定 null 是「什么都不绑」，游戏在下一次自己的 CS 派发前
//    会重新绑定它需要的全部槽位（D3D11 立即上下文没有状态继承）。而"还原成猜的资源"
//    一旦猜错，就是**把别人的纹理绑进游戏的 CS**，形态是静默画面错误而非缺图。
//
// ④-b **还原在 API 层面就是不完整的**（这条是决定性的，已核 Windows SDK）：
//    `CSGetUnorderedAccessViews` 的签名是
//      void CSGetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs,
//                                     ID3D11UnorderedAccessView **ppUnorderedAccessViews);
//    ——**没有 pUAVInitialCounts 输出参数**（d3d11.h:8523-8529，10.0.26100.0）。
//    而绑定侧 `CSSetUnorderedAccessViews(..., const UINT *pUAVInitialCounts)` 是有的。
//    ⇒ 一旦走「保存/还原」，游戏原本设的 UAV 初始计数会被**静默丢弃**（还原时只能传 nullptr
//    或 -1 语义），而这正是 append/consume 型 UAV 的语义来源。
//    也就是说：**"还原原值"做不到真的还原原值**，只能还原一半 —— 比置空更难解释的形态。
//
// ⑤ 已知风险（如实记录，未验证）：多组件共存时，若 OptiScaler / ReShade 依赖
//    "桥退出后 CS 槽位仍是它们绑的那一份"，本实现会把它们看到的槽位清成 null。
//    当前无证据表明存在这种依赖（三组件共存场景下未做过对照实测）；
//    若现场出现"开桥后 ReShade 的某个 CS 后处理失效"，本条是**首要排查点**。
// ============================================================================

// §3.6.1 深度提取 CS（1 SRV / 1 CB / 1 UAV）
static void dispatch_depth_cs(ID3D11DeviceContext *ctx, NgxInstance &inst)
{
    // ---- 绑定 ----
    ctx->CSSetShader(g_depth_cs.Get(), nullptr, 0);      // ★ CS 为全局对象（§3.5），不在 inst 上
    ID3D11ShaderResourceView *srvs[1] = { inst.depth_src_srv.Get() };
    ctx->CSSetShaderResources(0, 1, srvs);
    ID3D11Buffer *cbs[1] = { inst.cb.Get() };
    ctx->CSSetConstantBuffers(0, 1, cbs);
    ID3D11UnorderedAccessView *uavs[1] = { inst.depth_uav.Get() };
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

    // ---- 派发 ----
    ctx->Dispatch((inst.rw + 7u) / 8u, (inst.rh + 7u) / 8u, 1u);

    // ---- 解绑（成对）----
    ID3D11UnorderedAccessView *null_uavs[1] = { nullptr };
    ctx->CSSetUnorderedAccessViews(0, 1, null_uavs, nullptr);
    ID3D11ShaderResourceView *null_srvs[1] = { nullptr };
    ctx->CSSetShaderResources(0, 1, null_srvs);
    ID3D11Buffer *null_cbs[1] = { nullptr };
    ctx->CSSetConstantBuffers(0, 1, null_cbs);
    ctx->CSSetShader(nullptr, nullptr, 0);
}

// §3.6.2 motion 解码 CS（1 SRV / 1 CB / 2 UAV）
static void dispatch_motion_cs(ID3D11DeviceContext *ctx, NgxInstance &inst)
{
    ctx->CSSetShader(g_motion_cs.Get(), nullptr, 0);
    ID3D11ShaderResourceView *msrvs[1] = { inst.motion_src_srv.Get() };
    ctx->CSSetShaderResources(0, 1, msrvs);
    ID3D11Buffer *mcbs[1] = { inst.cb.Get() };
    ctx->CSSetConstantBuffers(0, 1, mcbs);
    // ★ 槽序不可换：u0 = 解码 motion（R16G16），u1 = reactive（R8，motion B 通道）
    ID3D11UnorderedAccessView *muavs[2] = { inst.motion_uav.Get(), inst.reactive_uav.Get() };
    ctx->CSSetUnorderedAccessViews(0, 2, muavs, nullptr);

    ctx->Dispatch((inst.rw + 7u) / 8u, (inst.rh + 7u) / 8u, 1u);

    ID3D11UnorderedAccessView *null_uavs2[2] = { nullptr, nullptr };
    ctx->CSSetUnorderedAccessViews(0, 2, null_uavs2, nullptr);
    // ★ 必须重新声明自己的解绑数组：上一段的 null_srvs / null_cbs 已出作用域
    ID3D11ShaderResourceView *mn_srvs[1] = { nullptr };
    ctx->CSSetShaderResources(0, 1, mn_srvs);
    ID3D11Buffer *mn_cbs[1] = { nullptr };
    ctx->CSSetConstantBuffers(0, 1, mn_cbs);
    ctx->CSSetShader(nullptr, nullptr, 0);
}

#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)
// ============================================================================
// §4.2 第 18/19/20 行的诊断抽检（**只在诊断构建里编译**；发布构建一个符号都不进产物）
// ★ 为什么整块包在 #if 里：第 18/20 行要 Map 一张 DEFAULT 纹理读回 —— Release 下
//   一次 Map 会 stall 整条 GPU 队列（SDS §4.2 第 20 行原文「Release 下 Map 会 stall」）。
//   第 19 行的 5 次 Get 回读每实例只做一次、代价可忽略，但为「发布构建不新增诊断符号」
//   这条纪律（§6）也一并包住。
// ============================================================================

// 采样一个像素，判定它是否"四字节全零"。
// 路径：1×1 CopySubresourceRegion → Map。★ **不做整张 staging 拷贝**：
//   SDS §4.2 第 18/20 行的原文就是"Map … 1×1 … 三点"，整张力度的拷贝在 4K 输出上
//   是 33 MB/次的带宽，诊断构建也不该付这个代价。
// 返回 false = **采样本身失败**（已打 error 行）。调用方必须把"读不回来"与"全黑"
//   区分开，否则一次 Map 失败就会被误判成 NGX 输出全黑并触发 g_release_all。
static bool sample_1x1_zero(ID3D11DeviceContext *ctx, ID3D11Texture2D *src,
                            UINT x, UINT y, bool &is_zero)
{
    is_zero = false;
    if (ctx == nullptr || src == nullptr)
        return false;

    D3D11_TEXTURE2D_DESC sd {};
    src->GetDesc(&sd);
    if (sd.Width == 0 || sd.Height == 0)
        return false;

    ComPtr<ID3D11Texture2D> staging;
    D3D11_TEXTURE2D_DESC st {};
    st.Width            = 1;
    st.Height           = 1;
    st.MipLevels        = 1;
    st.ArraySize        = 1;
    st.Format           = sd.Format;          // 必须同格式；TYPELESS→TYPELESS 合法
    st.SampleDesc.Count = 1;
    st.Usage            = D3D11_USAGE_STAGING;
    st.BindFlags        = 0;
    st.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
    st.MiscFlags        = 0;
    if (FAILED(g_device->CreateTexture2D(&st, nullptr, staging.GetAddressOf())) || !staging)
    {
        // 非致命重复性错误（诊断路径每 600 帧抽检一次，同一失败会反复出现）⇒ 按槽独立限频。
        // ★ 调用方必须能区分「采样失败」与「全黑」，所以这行**不能**改成不打（见本函数头注释）。
        log_ngx_error_throttled<log_throttle_slot::Slot::SampleStagingFail>(
            L"ngx_out sample staging fail fmt=%d", static_cast<int>(sd.Format));
        return false;
    }

    if (x >= sd.Width)  x = sd.Width - 1;     // 越界保护（右下角坐标由调用方按尺寸算）
    if (y >= sd.Height) y = sd.Height - 1;
    const D3D11_BOX box { x, y, 0, x + 1u, y + 1u, 1 };
    ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, src, 0, &box);

    D3D11_MAPPED_SUBRESOURCE ms {};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &ms)) || ms.pData == nullptr)
    {
        // 非致命重复性错误 ⇒ 按槽独立限频。
        log_ngx_error_throttled<log_throttle_slot::Slot::SampleMapFail>(
            L"ngx_out sample map fail x=%u y=%u", x, y);
        return false;
    }
    const std::uint8_t *px = static_cast<const std::uint8_t *>(ms.pData);
    // ★ 上界用 RowPitch（不是 4）：R8 之类单通道格式只有 1 B/像素，写死 4 会越界读。
    const std::size_t n = (ms.RowPitch < 4u) ? static_cast<std::size_t>(ms.RowPitch) : 4u;
    bool zero = true;
    for (std::size_t i = 0; i < n; ++i)
    {
        if (px[i] != 0)
        {
            zero = false;
            break;
        }
    }
    is_zero = zero;
    ctx->Unmap(staging.Get(), 0);
    return true;
}

// 第 19 行：Set() 返回 void（nvsdk_ngx_params.h:55-62，物理上无法检测），
// 所以只能 Get 回读抽检五个可回读项。任一不等（含 Get 本身失败）⇒ 说明 Set 被静默丢弃，
// 正是 §15 的 Ffx12CameraNear 教训在 NGX 参数袋上的形态 ⇒ 打 set_mismatch 并置 g_release_all。
static void diag_check_create_params(const NgxInstance &inst)
{
    auto mismatch = [](const char *name)
    {
        log_ngx_error(L"ngx_core set_mismatch name=%hs", name);
        g_release_all.store(true, std::memory_order_relaxed);
    };

    unsigned int w = 0, h = 0, ow = 0, oh = 0;
    int pq = -1;
    if (g_params->Get(NVSDK_NGX_Parameter_Width, &w) != NVSDK_NGX_Result_Success ||
        w != inst.rw)            { mismatch("Width");  return; }
    if (g_params->Get(NVSDK_NGX_Parameter_Height, &h) != NVSDK_NGX_Result_Success ||
        h != inst.rh)            { mismatch("Height"); return; }
    if (g_params->Get(NVSDK_NGX_Parameter_OutWidth, &ow) != NVSDK_NGX_Result_Success ||
        ow != inst.dw)           { mismatch("OutWidth");  return; }
    if (g_params->Get(NVSDK_NGX_Parameter_OutHeight, &oh) != NVSDK_NGX_Result_Success ||
        oh != inst.dh)           { mismatch("OutHeight"); return; }
    // PerfQualityValue 是 CreateFeature 时按 (rw, dw) 档位写进去的（§3.8.3 六档）。
    if (g_params->Get(NVSDK_NGX_Parameter_PerfQualityValue, &pq) != NVSDK_NGX_Result_Success ||
        pq != static_cast<int>(quality_from_ratio(inst.rw, inst.dw)))
    {
        mismatch("PerfQualityValue");
        return;
    }
}

// 第 20 行：首帧 Evaluate 全黑检测。**每个进程只做一次**（g_diag_first_eval_done）。
static bool g_diag_first_eval_done = false;
#endif // DX11FSRBRIDGE_ENABLE_DIAGNOSTICS

// ============================================================================
// shutdown_all()：整体关停（SDS §3.9A.1 只给了签名，函数体按 §3.7 第 2 步 /
// §3.9.7「共同纪律」逐条落地）
//
// ★★ **必须定义在 dispatch() 之前**：dispatch 第 2 步与第 11 步（设备丢失）都调它，
//    定义在后即 C3861（本项目 C3861 已烧过两次，见 SDS §3.9A.1 的长注）。
//
// 「该清什么 / 该留什么」是本函数唯一的规格来源，逐条对齐 §3.7 第 2 步与 §3.9.7：
//   清：feature / 全部实例资源 / key_valid / feature_create_failed / 两个 shader 对象 /
//       g_shaders_ready / g_shader_object_failed / g_device / g_params / g_inited / g_available
//   留：g_shader_compile_failed（进程级锁存，§3.5.4）
//       g_release_all（0→1 不可逆的全局放手信号，§4.3）
//       g_logged_config（配置快照只打一次，§3.10.2）
//       十个函数指针（指向已加载 DLL 的导出，DLL 与进程同寿，§6-13）
//
// ★ 幂等：未初始化时调用安全；连调多次安全（全部动作要么判空、要么是幂等的 Reset）。
// ★ 不调 NVSDK_NGX_D3D11_Shutdown（§6-13 明确不做）；不调 DestroyParameters（§3.9.5：
//   g_params 由 NGX 内部分配，v1 不释放，由进程回收）。
// ============================================================================
void shutdown_all()
{
    // ---- 1. 释放 feature（裸 handle，ComPtr 管不到，必须手工 Release）----
    // ★ 先判 g_release 非空：ensure_init 未跑过时它还是 nullptr。
    if (g_release && g_inst.feature)
        g_release(g_inst.feature);
    g_inst.feature = nullptr;

    // ---- 2. 清全部实例资源 + 两个随对象消失的锁存 ----
    // ★ 资源清理只有一个实现点（release_pool_resources），不要在这里再手写一遍 Reset。
    release_pool_resources(g_inst);
    g_inst.key_valid = false;
    // 对象销毁 ⇒ 锁存随对象消失（这不是"清锁重试"，§3.9.7 / S-81 只禁止尺寸变化时清它）
    g_inst.feature_create_failed = false;
    // T-8 M4-a：同上 —— 第 1 步已经把 feature 释放掉了，那份 CreateFlags 输入快照不再描述任何
    // 活对象。★ 必须与 destroy_instance() 同步清（两处都是"对象没了"的语义）；
    // 漏了它，换设备后第一次 dispatch 会拿旧快照比对，可能白重建一次刚建好的池。
    g_inst.feature_flags_bits  = 0;
    g_inst.feature_flags_valid = false;

    // ---- 3. 清 shader 对象（设备级）----
    // ★★ g_shader_compile_failed **必须保留**：D3DCompile 的输入（HLSL 字符串 + 编译器 +
    //    宏表）是进程级常量，同一份输入重编必然同样失败（§3.5.4 / D9 触发条件 4）。
    //    清了它 = 每帧重跑一次毫秒级的 D3DCompile ⇒ 卡顿，且无任何收益。
    //    而 g_shader_object_failed 是**设备级**的（CreateComputeShader 失败），
    //    新设备上值得重试一次 ⇒ 清。
    g_depth_cs.Reset();
    g_motion_cs.Reset();
    g_shaders_ready = false;
    g_shader_object_failed = false;

    // ---- 4. 清设备与参数 ----
    // ★ 不调 DestroyParameters：g_params 是 NGX 内部分配的，v1 不释放（§3.9.5）。
    g_device.Reset();
    g_params = nullptr;
    // ★★ 陷阱 3（T-5 任务书）：**漏清 g_inited ⇒ 下次 dispatch 以为已初始化、直接拿空表 ⇒ 崩溃**。
    //    与 g_device / g_params / 实例表必须一起清，一个都不能少。
    g_inited    = false;
    g_available = false;

    // ---- 以下**故意不清**（每一条都有明文依据，改任何一条都是设计变更）----
    //   g_release_all          —— 0→1 不可逆的全局放手信号（§4.3「转换：只能 0→1，不可逆」）
    //   g_logged_config        —— 配置快照只打一次（§3.10.2）
    //   g_init_retry_after_us  —— 失败退避窗口（§3.4.1）；shutdown_all 不是"初始化失败"，
    //                             清它会让 init 失败后的 5 秒退避在换设备时被绕过。
    //   十个函数指针           —— 指向已加载 DLL 的导出，DLL 从不卸载（§6-13）
}

} // namespace

// ============================================================================
// 配置 setter（SDS §3.13：全部幂等，**只存值、不打日志**）
// 理由：这些函数由 load_config() 调用，此时 blog::init() 尚未执行，任何 LOG_* 都会被丢弃
// （Dx11FsrBridge.cpp:140-143 注释自证）⇒ 非法值只记位，等 ensure_init 输出。
// ============================================================================
void set_application_id(unsigned long long id)
{
    if (id == 0)
    {
        // appId=0 ⇒ available=0、CreateFeature 必失败（E1，AGENTS.md §15）⇒ 强制 1337 并记位
        g_app_id = 1337;
        g_config_warn |= kWarnAppIdZero;
        return;
    }
    g_app_id = id;
}

void set_feature_dirs(const std::vector<std::wstring> &d)
{
    g_feature_dirs = d;   // 解析/探测在 ensure_init 的第 0 步（resolve_feature_dirs）
}

void set_hdr(bool on)
{
    g_hdr = on;
}

void set_hdr_mode(int mode)
{
    // T-8：0=auto 1=force-on 2=force-off；越界静默回 auto（不占用 config_warn 位：
    // kWarnPresetInvalid 语义是「NgxRenderPreset 越界」，与本键无关）。
    if (mode == 0 || mode == 1 || mode == 2)
        g_hdr_mode = mode;
    else
        g_hdr_mode = 0;
    // ★ T-11：本函数改的是强制模式、**不改变缓存内容**；这里刷新是为了让
    //   「NgxHdr=auto 切换进来」时缓存一定与当前三级探测一致（幂等，无副作用）。
    refresh_hdr_probe_cache();
}

void set_swapchain_hdr(bool hdr)
{
    // m4：跨线程写（hooked_set_color_space1 在游戏线程）⇒ 统一 relaxed store。
    // ★★ MINOR-1（T-8 收尾）：**一次 store 写入完整三态**（1 = 已知且 SDR，2 = 已知且 HDR）。
    //   旧写法是「先 store 值、再 store known=true」并声称先值后 known 就安全 ——
    //   **该论断是错的**：两个独立的 relaxed store 之间**不构成任何顺序约束**，
    //   编译器/CPU 完全可以让 known 先对读方可见，读方于是看到「已知 + 旧值」
    //   ⇒ HDR 会话被短暂判成 SDR。打包成一个 atomic 后，读方一次 load 只能看到
    //   某一个完整的旧三态或新三态，**不存在中间态**。
    g_swapchain_hdr_state.store(hdr ? 2u : 1u, std::memory_order_relaxed);
    refresh_hdr_probe_cache();   // ★ T-11：探测变了，立刻刷新预计算缓存
}

// T-8 M4-b（路径 A）：swapchain 后缓冲格式。由 install_swapchain_hooks() 在第一次
// 装 hook 时主动调用 —— 不依赖 SetColorSpace1 是否被调用过。
// ★ 为什么这是"能力级"而非"状态级"信号：DXGI 没有 GetColorSpace，读不回当前 color space；
//   BufferDesc.Format 只说明"后缓冲是什么格式"。但它对"游戏从不调 SetColorSpace1"这类
//   情况是净收益，且优先于输入纹理格式（见 resolve_is_hdr 的三级判据）。
// ★ DXGI_FORMAT_UNKNOWN(0) = 未知，resolve_is_hdr 会跳过它。
void set_swapchain_format(DXGI_FORMAT f)
{
    g_swapchain_format.store(static_cast<unsigned>(f), std::memory_order_relaxed);
    refresh_hdr_probe_cache();   // ★ T-11：探测变了，立刻刷新预计算缓存
}

// T-FIX-2：读回后缓冲格式，供 Dx11FsrBridge 的 is_hdr10_color_space 判断
// 「G10 色空间 + FP16 后缓冲」这一组合。单独 G10 不足以判 HDR
// （ACM + SDR 显示器也用 G10），详见 Dx11FsrBridge.cpp 该函数的注释。
DXGI_FORMAT swapchain_format()
{
    return static_cast<DXGI_FORMAT>(g_swapchain_format.load(std::memory_order_relaxed));
}

void set_render_preset(int v)
{
    // §3.13：不在 (0 或 5..15) → 回退 0 并记位。0 的语义是"六个 preset 参数一个都不设"，
    // 不是"默认预设"（§3.8.3 / T-3 ③）。
    // ★ P-11：非法值的回退目标从 12 改为 0 —— 与默认值一致，都是"跟随 NGX"。
    if (v == 0 || (v >= 5 && v <= 15))
    {
        g_render_preset = v;
        return;
    }
    g_render_preset = 0;
    g_config_warn |= kWarnPresetInvalid;
}

void set_frame_time_delta(bool on)
{
    g_dt_enabled = on;
}

void set_use_reactive_mask(bool on)
{
    g_use_reactive = on;
}

void set_log_level(int lv)
{
    if (lv < 0 || lv > 3)
        lv = 1;   // §3.13：越界回退 1
    g_log_level = lv;
}

void set_passthrough(bool on)
{
    g_passthrough = on;
}

// ============================================================================
// 查询
// ============================================================================
// T-1 审核 MINOR-2：SDS §3.2 的语义是"已成功初始化 **且** 当前可用 **且** 没有全局放手"。
bool active()
{
    return g_inited && g_available && !g_release_all.load(std::memory_order_relaxed);
}

bool available()
{
    return g_available;
}

bool passthrough()
{
    return g_passthrough;
}

// ============================================================================
// 每帧（唯一入口）
// ============================================================================
// §3.7 的十二步（第 0 步 + 11 步）全部在此落地。**顺序写死，不得调整**（§3.7.1 三条硬约束）。
//
// ★★ 三条最容易写错、且都**不报编译错**的地方（每条都是本项目真实烧过时间的坑）：
//   ① 第 3 步成功路径**不能** dev->Release() —— 第 4 步建池还要用 dev（§7.3「先放后用」）。
//      释放点只有一个：第 4 步末尾、ensure_instance 之后。
//   ② 第 7-9 步（写资源**内容**）必须早于第 10 步（把资源**绑定**进参数袋）——
//      顺序颠倒的形态是「静止正常、一动就糊」，零错误码（§3.7.1 约束 2 / 陷阱 1）。
//   ③ 所有 return 路径都必须成对 Release dev（第 2b 步的 early return 尤其容易漏）。
bool dispatch(const ffx12::FrameInput &in, ID3D11DeviceContext *ctx,
              std::uint64_t instance_key)
{
    // ---- 第 0 步（§3.7）：passthrough 时不建池、不编 CS、不 CopyResource，成本必须是 0 ----
    // 位置硬约束：必须在 ensure_init 之前、且不持任何锁；这一行由 dispatch 入口自己打
    // （ensure_init 不在 passthrough 路径上）。
    if (g_passthrough)
    {
        // 2026-11-09（PR #12 审查次要项 1 复核）：**按档位分流**，兼顾两条互相拉扯的要求。
        //
        // 原报告意见：「passthrough 每帧写一条 Level::Error；log_ngx_core 只有级别闸门，
        //   没有计数闸门，诊断模式下会刷屏」。
        // 当时的判断是不加闸门，理由是 SDS 判据 14 要求这一行与 ngx_in **同帧对应**。
        // 复核后改为两者共存 —— 判据 14 是**验收时**的需求，不是常态：
        //   · g_log_level <= 2（error / info，即**验收与常规档位**）：**不限频**。
        //     判据 14 的取证（grep -c 计数、与 ngx_in 同帧对应）在这一档成立。
        //   · g_log_level == 3（debug，**诊断档位**）：前 8 条全打 + 之后每 256 条一条。
        //     ★ 为什么保留"前 8 条全打"而不是纯 %256：纯 %256 会让**全新一次运行的首条
        //       永不出现**（count=1 时 1%256≠0），而那正是判据 14 的 grep 对象
        //       ⇒ 在 debug 档跑验收会得到假阴性。前 8 条全打 + 计数从 1 起 ⇒
        //       「本次运行的首条」（n=1）一定在 <=8 内，不被本闸门吞掉；
        //       （注意这只管本闸门；blog::write 的 enabled() 仍可能按分类覆盖丢行。）
        //       阈值口径与本文件既有的 log_gate_fail8_every256（<=8 或 %256==0）一致。
        //   · g_log_level <= 0（全关）：log_ngx_core 自己第一行就 return，本处不必再判。
        // 成本：一次自增 + 一次比较，均在**已经要打日志**的分支内，无格式化、无字符串拼接。
        const std::uint64_t pt_count = ++g_ngx_passthrough_count;
        if (g_log_level < 3 || pt_count <= 8u || (pt_count % 256u) == 0u)
            log_ngx_core(blog::Level::Error, L"ngx_core passthrough=1 backend=ngx");
        return false;
    }

    // ---- 第 1 步：取设备（GetDevice 返回 void，失败时仍是 nullptr）----
    if (ctx == nullptr)
        return false;   // 防御：调用方保证非空；不改变正常路径行为

    // ★ 输入判空与尺寸判零前移到此处（而非第 4 步）：第 4 步一进来就 in.color->GetDesc()，
    //   空指针解引用在注入型 DLL 里是**游戏进程崩溃**，不是 fail-open。
    //   T-6 的分流点还会再判一遍（§3.3.2 复刻 native 的两道判空），这里判是自保，不是重复。
    //   B-1：四个尺寸任一为 0 ⇒ CopyResource / CreateTexture2D 都不接受。
    if (in.color == nullptr || in.output_target == nullptr)
        return false;
    if (in.render_w == 0 || in.render_h == 0 || in.display_w == 0 || in.display_h == 0)
        return false;

    ID3D11Device *dev = nullptr;
    ctx->GetDevice(&dev);
    if (dev == nullptr)
        return false;

    // 帧计数（§3.10.1 三条逐帧日志的闸门输入）。放在第 1 步之后：passthrough / 判空失败的
    // 调用不构成"一帧"，不能挤占前 8 帧的全量日志额度。
    const std::uint64_t frame_count = ++g_ngx_frame_count;

    // ---- 第 2 步：设备变化 ⇒ 整体关停（多设备 / 设备重建，§3.9.7 / D14.0-b2）----
    // ★ 必须在 ensure_init **之前**：设备换了，旧 NGX 绑定的是旧设备，不清就是 UB。
    //   g_device / g_params / 实例表 / g_inited 必须一起清（shutdown_all 的契约）。
    if (dev != g_device.Get())
        shutdown_all();

    // ---- 第 2b 步：全局放手 ⇒ 本帧什么都不做（S-84 / 计划 D14.4 第 1b 步）----
    // ★★ 必须成对 Release：此时抢在第 3 步之前返回，漏了就**每帧泄漏一个 device 引用计数**。
    // ★ 位置：第 2 步之后（设备更换仍需先清理）、ensure_init 与建实例之前（成本为 0）。
    if (should_release_all())
    {
        dev->Release();
        return false;
    }

    // ---- 第 3 步：ensure_init（幂等；首次成功才 AddRef）----
    // ★ 只在**失败**路径 Release。成功路径的 Release 在第 4 步末尾（见 ①）。
    if (!ensure_init(dev))
    {
        dev->Release();
        return false;
    }

    // ---- 第 4 步：单实例取/建（形参显式传入，§3.12.2 骨架）----
    // 先读出实际格式：create_pool 要用，且必须先于 ensure_instance（它是形参）。
    D3D11_TEXTURE2D_DESC cdesc {}, odesc {};
    in.color->GetDesc(&cdesc);
    in.output_target->GetDesc(&odesc);
    const bool inst_ok = ensure_instance(instance_key, dev, in.render_w, in.render_h,
                                         in.display_w, in.display_h,
                                         cdesc.Format, odesc.Format);
    // ★★ 成对释放：dev 已用完，且必须在所有使用点之后、所有 return 之前。
    //    成功路径**只有这一处** Release —— 第 3 步不再释放（见 ①）。
    dev->Release();
    if (!inst_ok)
        return false;
    NgxInstance &inst = g_inst;   // 全局唯一实例：无 map、无 LRU、无驱逐（v8 / S-90）

    // ---- 第 5 步：尺寸/格式六元组变化 **或 CreateFlags 输入变化** ⇒ 重建资源池 + feature（§3.12.3）----
    // ★★ T-8 M4-a（T-8 审核 MAJOR）：判据里**必须**含 CreateFlags 的可变输入（见 flags_input_bits）。
    //   缺陷原状：Create_Flags 只在 create_feature 时写进 g_params，EvaluateFeature 不重算；
    //   而重建判据只有尺寸/格式六元组 ⇒ 若 SetColorSpace1 钩子在首个 CreateFeature 之后才被调用，
    //   IsHDR 就**永久**是错的（直到尺寸/格式变化或换设备），且日志看不出来。
    //
    // ★ 为什么"值变化才重建"是安全的（不会每帧重建）：
    //   feature_flags_bits 只在 create_feature 成功后写入，判据是"当前位图 != 已记录的位图"。
    //   位图不变 ⇒ 条件恒假 ⇒ 零开销一次整数比较；位图变了 ⇒ 只重建**一次**，
    //   重建后记录被更新成当前值，下一帧条件又变假。
    //
    // ★ 与 S-81 的取舍（明确写出，因为这是本步最容易踩雷的地方）：
    //   S-81 的契约是"尺寸/格式变化**不清 feature_create_failed、不重试**"。本条**不破坏**它：
    //     ① 本 if 块里**没有**任何对 feature_create_failed 的写；
    //     ② CreateFlags 输入变化属于**新的输入变化**（HDR 状态本来就会在运行期改变，
    //        例如玩家切 HDR / 游戏调 SetColorSpace1），与"同一份输入重试"不是同一件事；
    //     ③ 且 flags_changed 自身就带 !feature_create_failed 门（见下方三前提），
    //        锁存为真时 HDR 变化**不会**触发重建；尺寸/格式变化照旧走本 if 的其余项。
    //   ⇒ HDR/DepthInverted 变化会且只会触发**一次**资源池 + feature 重建，不会形成重试风暴。
    //
    // ★ 一条已知的边界（记录，不在本轮修）：本 if 块同时承担"重建资源池"。
    //   若 HDR 在**同一尺寸**下反复翻转，会连带重建 5 张纹理（约几十 MB 的分配）。
    //   实际上 HDR 由注册表在启动时决定、运行期极少切换（见 hooked_set_color_space1 的注释），
    //   因此按"简单优先"处理；若将来出现运行期频繁切换，应把池重建与 feature 重建拆成两个判据。
    // ★★ 三条硬纪律（照 SDS §3.7 第 5 步 v17 修订版）：
    //   ① **禁止**自己写 ComPtr::Reset()：create_pool 自持「先清后建」契约，从任意已建池
    //      状态重入都安全。在这里再清一遍会让资源清理出现两个维护点，下次加字段必漏改一处。
    //   ② 建池失败要 inst.key_valid = false 回滚 —— 六元组已被本步写回，下一帧判据相等
    //      ⇒ 本步不会再进，池保持空 ⇒ 必须让下一帧经 ensure_instance 重建。
    //   ③ **不写** inst.feature_create_failed = false（S-81：尺寸/格式变化不清锁、不重试）。
    // ★ flags_changed：三个前提缺一不可。
    //   ① !inst.feature_create_failed —— S-81 的锁存优先：已经判定"这份输入的 feature 建不起来"
    //      就不再重建（否则 0xBAD0000B 会每帧重建一次池 + 每帧阻塞，正是 S-81 要防的）。
    //   ② inst.feature_flags_valid —— 还没建过 feature 时没有可比对的基准；
    //      若在这里恒为 true，则首帧就进重建分支，白建一次池。
    //   ③ 位图不等 —— 值没变 ⇒ 一次整数比较就短路，零开销、不重建。
    //   ★ 用 flags_input_bits()（纯计算、不打日志）而不是 build_create_flags()：
    //     后者会打一条 Error 级日志，放在这里就是每帧一条（日志洪水 + 每帧格式化开销）。
    const bool flags_changed = !inst.feature_create_failed &&
                               inst.feature_flags_valid &&
                               (flags_input_bits(cdesc.Format) != inst.feature_flags_bits);
    if (in.render_w != inst.rw || in.render_h != inst.rh ||
        in.display_w != inst.dw || in.display_h != inst.dh ||
        cdesc.Format != inst.color_fmt || odesc.Format != inst.output_fmt ||
        flags_changed)
    {
        if (g_release && inst.feature)
            g_release(inst.feature);
        inst.feature = nullptr;

        // ★ 建池之前必须先把新尺寸与新格式写回 inst：create_pool 直接读 inst.color_fmt /
        //   inst.output_fmt，漏写会拿 DXGI_FORMAT_UNKNOWN 去 CreateTexture2D ⇒
        //   E_INVALIDARG ⇒ 每次建池都失败、NGX 永远不出图，而日志只有一行 pool tex fail。
        inst.rw = in.render_w;  inst.rh = in.render_h;
        inst.dw = in.display_w; inst.dh = in.display_h;
        inst.color_fmt  = cdesc.Format;    // 游戏实际格式（本机实测 R10G10B10A2_TYPELESS = 23）
        inst.output_fmt = odesc.Format;

        if (!create_pool(g_device.Get(), inst, inst.rw, inst.rh, inst.dw, inst.dh, instance_key))
        {
            inst.key_valid = false;        // 见 ②：让下一帧经 ensure_instance 重建
            return false;                  // 建池失败：本帧放弃（create_pool 已自行清干净）
        }
        ++inst.pool_rebuilds;              // §3.12.2：本字段的唯一写入点
        inst.first = true;                 // 尺寸变化 ⇒ 历史失效 ⇒ 下一帧 Reset
    }

    // ---- 第 5 步之后：feature 的建与不建 ----
    // 永久不重试（§3.9.7 / S-81）：CreateFeature 失败的根因（PathListInfo 配错）不会自愈，
    // 相同输入重试只会每帧阻塞 + 每帧重复日志。锁存只在对象销毁时随对象消失。
    if (inst.feature_create_failed)
        return false;
    if (!inst.feature)
    {
        if (!create_feature(inst, in, ctx))
            return false;                  // 内部已置位 feature_create_failed 与 g_release_all
#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)
        // §4.2 第 19 行：Set() 返回 void，只能 Get 回读抽检。不等 ⇒ set_mismatch + 置位。
        diag_check_create_params(inst);
#endif
    }

    // ---- 第 6 步：两个自写 CS（§3.5.4）----
    // ★ 置位是**调用方**的责任：ensure_shaders 自己只返回 false（它还要区分进程级/设备级锁存）。
    // ★ 只在 g_release_all **首次 0→1** 时打 release_all 行（exchange 的返回值即旧值）。
    // ★ 本行与第 11 步 eval 路径的 release_all 行**字段完全一致**（plan D10 的
    //   `release_all reason=%s inst=0x%llX fail_streak=%d`），因此
    //   `grep "release_all reason="` 得到的每一行形状统一、可直接横向比较。
    //   fail_streak 在本路径上通常为 0 —— 那正是它应有的含义：「本次放手不是 eval 失败引起的」。
    if (!ensure_shaders())
    {
        const bool already_released = g_release_all.exchange(true, std::memory_order_relaxed);
        if (!already_released)
            log_ngx_error(L"ngx_core release_all reason=%s inst=0x%llX fail_streak=%d",
                          L"shader_failure",
                          static_cast<unsigned long long>(instance_key),
                          static_cast<int>(inst.eval_fail_streak));
        return false;
    }

    // ---- 第 7 步：color 用游戏原格式 TYPELESS 原样拷（§4.2 第 17 行）----
    // ★ CopyResource 返回 void，**无法检测失败**；正确性靠建池时的同 desc 构造性约束。
    // ★ 必须在第 8/9 步之前：两个 CS 的 t0 是**游戏的** in.depth / in.motion，
    //   但它们的 t1/颜色通道读的是 inst.color —— 见 §3.7.1 约束 1 的澄清。
    ctx->CopyResource(inst.color.Get(), in.color);

    // ---- 第 8 步：深度提取 CS（§3.6.1）----
    if (!ensure_depth_src_srv(in, inst))
        return false;
    upload_cb(ctx, inst, 1.0f, 1.0f);      // ★ depth_scale / flip 恒 1.0（§3.6.5），两段 CS 共用一个 cb
    dispatch_depth_cs(ctx, inst);

    // ---- 第 9 步：motion 解码 CS（§3.6.2）----
    if (!ensure_motion_src_srv(in, inst))
        return false;
    dispatch_motion_cs(ctx, inst);

    // ---- 第 10 步：写 §3.9.1 的十四行逐帧参数（协议 A：**不重写创建期七项**，§3.9.1a）----
    // ★★ 顺序硬约束（§3.7.1 约束 2）：本步绑的是 inst.* 的**资源**，第 7-9 步写入的是这些
    //    资源的**内容**。颠倒不报错，但 NGX 读到上一帧/未初始化内容 ⇒「静止正常、一动就糊」。
    // ★★ 第 1-4 项一律走 set_tex（§3.9.6）：Set() 没有 SRV/UAV 重载，传视图会隐式转
    //    void* ⇒ 编译通过、无警告、运行时静默错误。
    // ★ 第 14 项 ExposureTexture **明确不设**（桥从不提供曝光纹理，F-21）。
    if (g_params == nullptr)
        return false;   // 防御：ensure_init 成功即非空，但不拿空指针去解引用（同陷阱 2 的纪律）

    const int reset_val = (in.reset || inst.first) ? 1 : 0;

    // ---- ngx_in 行（§3.10.1 输入侧；info；闸门 count <= 8 || count % 1024 == 0）----
    // ★ fmt 的三个值取自**实际输入纹理的 GetDesc()**，不得凭空构造。
    // ★ 本行是**逐帧**行，绝不与一次性快照的 ngx_core 混用（§3.10.2 末尾的硬约束）。
    if (log_gate_win8_every1024(frame_count))
    {
        D3D11_TEXTURE2D_DESC ddesc {}, mdesc {};
        in.depth->GetDesc(&ddesc);
        in.motion->GetDesc(&mdesc);
        log_ngx_core(blog::Level::Info,
                     L"ngx_in jitter=%.6f,%.6f mvscale=%.1f,%.1f reset=%d dt=%.2f fmt=%d/%d/%d",
                     static_cast<double>(in.jitter_x), static_cast<double>(in.jitter_y),
                     static_cast<double>(in.motion_scale_x), static_cast<double>(in.motion_scale_y),
                     reset_val,
                     static_cast<double>(in.frame_time_delta_ms),
                     static_cast<int>(cdesc.Format), static_cast<int>(ddesc.Format),
                     static_cast<int>(mdesc.Format));
    }

    // 1-4：四个纹理（资源本体，**不是视图**）
    set_tex(g_params, NVSDK_NGX_Parameter_Color,         inst.color.Get());
    set_tex(g_params, NVSDK_NGX_Parameter_Depth,         inst.depth.Get());
    set_tex(g_params, NVSDK_NGX_Parameter_MotionVectors, inst.motion.Get());
    set_tex(g_params, NVSDK_NGX_Parameter_Output,        inst.output.Get());
    // 5：jitter —— **像素单位，直接透传**（不乘 render_w、不加 0.5，D4）
    g_params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, in.jitter_x);
    g_params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, in.jitter_y);
    // 6：motion 缩放 —— 原样透传（符号已由 E3 确证，D5）
    g_params->Set(NVSDK_NGX_Parameter_MV_Scale_X, in.motion_scale_x);
    g_params->Set(NVSDK_NGX_Parameter_MV_Scale_Y, in.motion_scale_y);
    // 7：Reset —— in.reset 或本实例首帧（F-32）。int 重载。
    g_params->Set(NVSDK_NGX_Parameter_Reset, reset_val);
    // 8：Width / Height —— **每帧也要设**（不只 CreateFeature），取当前实例的重建尺寸
    g_params->Set(NVSDK_NGX_Parameter_Width,  inst.rw);
    g_params->Set(NVSDK_NGX_Parameter_Height, inst.rh);
    // 9：Render.Subrect 维度 —— 无动态分辨率时等于全屏子矩形
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,  inst.rw);
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, inst.rh);
    // 10：Exposure_Scale **恒 1.0f**；与第 11 项是两个不同参数，都要设
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
    // 11：Pre_Exposure = in.pre_exposure（当前恒 1.0）。**不设 Flags_AutoExposure**（D8）
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, in.pre_exposure);
    // 12：FrameTimeDeltaInMsec —— **仅当 NgxSetFrameTimeDelta=1**。桥侧已 clamp [1,100]，
    //     此处**不重复 clamp**（B-4：过滤会掩盖上游 bug）。
    if (g_dt_enabled)
        g_params->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, in.frame_time_delta_ms);
    // 13：reactive mask —— **仅当 NgxUseReactiveMask=1**，且必须先过尺寸守卫（§3.9.2）。
    //     守卫恒真（inst.reactive 就是按 inst.rw x inst.rh 建的），保留它是为了与 FFX 侧口径一致。
    if (g_use_reactive && inst.rw >= in.render_w && inst.rh >= in.render_h)
        set_tex(g_params, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, inst.reactive.Get());

    // ---- 第 11 步：EvaluateFeature（含 eval_us 计时，§3.10.1）----
    // ★★ 陷阱 2：g_eval 是**函数指针**，nullptr 时直接调用 = 空指针崩溃
    //    （注入型 DLL 里是整个游戏进程崩，不是 fail-open）。
    if (g_eval == nullptr)
        return false;

    const std::uint64_t eval_t0 = qpc_us();
    const NVSDK_NGX_Result rr = g_eval(ctx, inst.feature, g_params, nullptr);
    const std::uint64_t eval_us = qpc_us() - eval_t0;

    if (rr != NVSDK_NGX_Result_Success)
    {
        // ★★ 设备丢失的检测**不能**拿 rr 去比 DXGI 码：NVSDK_NGX_Result 的值域里没有 DXGI 码
        //    （nvsdk_ngx_defs.h:96+ 全是 0xBAD000xx），那样写会编译通过、判定恒不成立 ⇒
        //    设备丢失被当成普通 eval 失败，连 3 次后置 g_release_all 而不是重建。
        //    唯一可靠出处是 ID3D11Device::GetDeviceRemovedReason()。
        //    ★ 用 g_device.Get() 而不是局部 dev：dev 已在第 4 步 Release（不得先放后用）。
        const HRESULT drr = g_device ? g_device->GetDeviceRemovedReason() : E_FAIL;
        const bool device_removed = FAILED(drr);

        ++inst.eval_fail_total;
        if (!device_removed)
            ++inst.eval_fail_streak;   // §4.2 第 12 行 / §3.9.7：设备丢失**不累加** streak

        if (log_gate_fail8_every256(inst.eval_fail_total))
        {
            log_ngx_core(blog::Level::Error,
                         L"ngx_core eval ok=%d rc=0x%X eval_fail_total=%llu "
                         L"eval_fail_streak=%u eval_us=%.1f",
                         0, static_cast<unsigned>(rr),
                         static_cast<unsigned long long>(inst.eval_fail_total),
                         static_cast<unsigned>(inst.eval_fail_streak),
                         static_cast<double>(eval_us));
        }

        if (device_removed)
        {
            // 设备没了：整体关停（保留进程级编译失败锁存），下一帧换新设备重建。
            // ★★ **绝不置 g_release_all**（§4.2 第 12 行「是否置位」列 = 否）。
            //    "设备没了"不是"NGX 用不了"：换新设备后 NGX 完全可以继续工作，此时放手
            //    等于永久退回原生，而设备重建本来就能自愈（先例 Ffx12Backend.cpp:2886 的
            //    recover_device_removed_locked()）。
            // ★★ **前缀不能写 release_all**：验收判据是「release_all 行出现后，日志中不得
            //    再出现 ngx_core eval 成功行 / ngx_out 行」（计划 §9.1-12）。在一条**没有**
            //    置位的路径上打 release_all，会让该判据误判成"已经放手却还在出图"。
            //    故本行用 ngx_core 的独立事件名 dev_removed，不进 release_all 的计数。
            // ★ 已知偏离（R-5 审核 MINOR-A / R-9 MINOR-3）：本行**不置 g_release_all**、且
            //   **可重复**（设备再次丢失时会再打），按"非致命 + 可重复 ⇒ 应改限频"的判据本该用
            //   log_ngx_error_throttled。此处**有意保留** log_ngx_error 不闸门，理由如下 ——
            //   ★ 旧注释写的"其频次已由外层限频"**不成立**，已删除：本分支没有任何计数或时间
            //     闸门；上面那条 eval 行的 log_gate_fail8_every256 只闸 eval 行，**不含本行**。
            //     实际约束只有一条：shutdown_all() 之后的下一次 ensure_init 若 Init_Ext 失败，
            //     fail_init() 会置 g_release_all，之后本路径不再进入；若重建成功，设备再丢就再打。
            //   ★ 旧注释写的"shutdown_all 会重建设备"也不准：shutdown_all() 只清
            //     g_device / g_params / g_inited（"清设备与参数"段），**不重建**；重建发生在
            //     下一次 dispatch 的 ensure_init（第 3 步）。
            //   ⇒ 保留不闸门的真实理由：设备反复丢失的**次数**本身就是要看的证据，闸门会把这个
            //     次数抹成"每 1024 次一条"。
            log_ngx_error(L"ngx_core dev_removed shutdown drr=0x%08X inst=0x%llX",
                          static_cast<unsigned>(drr),
                          static_cast<unsigned long long>(instance_key));
            shutdown_all();
            return false;
        }

        // 连续 3 次才放手（§4.2 第 11 行）：过滤偶发单帧抖动。
        if (inst.eval_fail_streak == 3)
        {
            const bool already_released = g_release_all.exchange(true, std::memory_order_relaxed);
            if (!already_released)
                log_ngx_error(L"ngx_core release_all reason=%s inst=0x%llX fail_streak=%d",
                              L"eval",
                              static_cast<unsigned long long>(instance_key),
                              static_cast<int>(inst.eval_fail_streak));
        }
        return false;
    }

    // ---- 成功：清零 streak、清 first、回写游戏 ----
    inst.eval_fail_streak = 0;   // 成功后只清 streak，**不清 total**（§3.12.2 计数器表）
    inst.first = false;

    const std::uint64_t copy_t0 = qpc_us();
    ctx->CopyResource(in.output_target, inst.output.Get());
    const std::uint64_t copy_us = qpc_us() - copy_t0;

    // ngx_out 行（§3.10.1 输出侧；逐帧 = **debug**；失败才用 error）
    if (log_gate_win8_every1024(frame_count))
    {
        log_ngx_core(blog::Level::Debug, L"ngx_out copy out=%dx%d fmt=%d copy_us=%.1f",
                     static_cast<int>(inst.dw), static_cast<int>(inst.dh),
                     static_cast<int>(odesc.Format), static_cast<double>(copy_us));
    }

    // ngx_core eval 成功行（**info**；每 1024 帧一次）
    if (log_gate_every1024(frame_count))
    {
        log_ngx_core(blog::Level::Info,
                     L"ngx_core eval ok=%d rc=0x%X eval_fail_total=%llu "
                     L"eval_fail_streak=%u eval_us=%.1f",
                     1, static_cast<unsigned>(rr),
                     static_cast<unsigned long long>(inst.eval_fail_total),
                     static_cast<unsigned>(inst.eval_fail_streak),
                     static_cast<double>(eval_us));
    }

#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)
    // ---- §4.2 第 18 行：每 600 帧抽检输出纹理的两个角（左上 + 右下）----
    // ★ 两角皆全零 ⇒ 疑似 CopyResource 无声失败。**不置 g_release_all**（可能是场景本身全黑，
    //   §4.2 第 18 行的「是否置位」列明确为「否」）。
    if ((frame_count % 600u) == 0u)
    {
        bool tl_zero = false, br_zero = false;
        const bool tl_ok = sample_1x1_zero(ctx, inst.output.Get(), 0, 0, tl_zero);
        const bool br_ok = sample_1x1_zero(ctx, inst.output.Get(), inst.dw - 1u, inst.dh - 1u, br_zero);
        if (tl_ok && br_ok && tl_zero && br_zero)
        {
            // ★ 已知偏离（R-5 审核 MINOR-A）：本行同样**不置 g_release_all**、且**可重复**
            //   （场景本身持续全黑时每 600 帧都会命中），按判据本该改 log_ngx_error_throttled。
            //   此处**有意保留** log_ngx_error 不闸门：外层已有 600 帧（约 10 秒）的天然限频，
            //   且这条是"CopyResource 可能无声失败"的唯一线索，排障时需要看到它是否
            //   与画面异常同步出现 —— 再叠一层计数闸门只会让因果对齐变难。
            log_ngx_error(L"ngx_out copy fail suspect inst=0x%llX out=%ux%u fmt=%d frame=%llu",
                          static_cast<unsigned long long>(instance_key),
                          inst.dw, inst.dh, static_cast<int>(odesc.Format),
                          static_cast<unsigned long long>(frame_count));
        }
    }

    // ---- §4.2 第 20 行：首帧全黑检测（三点采样，输出 vs 输入）----
    // ★ 判据：**输出三点全零 且 输入不是三点全零** ⇒ NGX 静默降级（Success 但没出图）。
    //   只做一次（首帧），因为它要 6 次 Map，代价远超一次 Evaluate。
    if (!g_diag_first_eval_done)
    {
        g_diag_first_eval_done = true;   // 无论采样成败都只做一次，避免采样失败后每帧重试
        const UINT cx = inst.dw / 2u, cy = inst.dh / 2u;
        bool o1 = false, o2 = false, o3 = false;
        bool i1 = false, i2 = false, i3 = false;
        const bool out_ok = sample_1x1_zero(ctx, inst.output.Get(), 0, 0, o1)
                         && sample_1x1_zero(ctx, inst.output.Get(), cx, cy, o2)
                         && sample_1x1_zero(ctx, inst.output.Get(), inst.dw - 1u, inst.dh - 1u, o3);
        const bool in_ok  = sample_1x1_zero(ctx, in.color, 0, 0, i1)
                         && sample_1x1_zero(ctx, in.color, in.render_w / 2u, in.render_h / 2u, i2)
                         && sample_1x1_zero(ctx, in.color, in.render_w - 1u, in.render_h - 1u, i3);
        if (out_ok && in_ok && o1 && o2 && o3 && !(i1 && i2 && i3))
        {
            log_ngx_error(L"ngx_out all_black inst=0x%llX out=%ux%u in=%ux%u",
                          static_cast<unsigned long long>(instance_key),
                          inst.dw, inst.dh, in.render_w, in.render_h);
            g_release_all.store(true, std::memory_order_relaxed);
        }
    }
#endif // DX11FSRBRIDGE_ENABLE_DIAGNOSTICS

    return true;
}

// ============================================================================
// fail-open 信号（D9 两道拦截共用）
// ============================================================================
// 只读 g_release_all；不碰容器、不取任何锁（硬约束，见 SDS §3.3.3）。
bool should_release_all()
{
    return g_release_all.load(std::memory_order_relaxed);
}

} // namespace ngx
