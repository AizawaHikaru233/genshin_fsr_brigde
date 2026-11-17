// NgxProviderPick.h —— NGX「实现来源」选择的**纯决策层**（零 D3D / 零 NGX / 零全局状态）。
//
// ============================================================================
// 这一层解决什么
// ============================================================================
// `NgxBackend.cpp` 的 `ensure_init()` 第 1..3 步要决定「这次用谁来当 NGX 实现」，
// 现场是三段串起来的探测：
//   ① `try_nvidia_registry()`  —— 读 HKLM\...\NGXCore\FullPath，加载并绑定导出
//   ② `try_external()`         —— 先 LoadLibraryW("_nvngx.dll")，
//                                 再 `scan_loaded_modules()` 扫已加载模块
// 再叠上用户态开关 `NgxProvider`（auto / nvidia / external）与「命中来自哪一路」，
// 最终走几条不同的分支（硬失败 / 软失败 / 继续用）。
//
// 重构前，这套选择**物理上长在** D3D 代码里：两个探测函数直接改 `ProviderHit`、
// 直接 `return fail_init(...)`、中间还写 `fallback_detail` 字符串。后果是
// **决策顺序无法单测** —— 而「为什么这次没用上 OptiScaler」正是最难实机二分的
// 那类问题（要凑出「无 OptiScaler / 只装桥 / 有注册表 / 无注册表」的组合）。
//
// 本文件把那套选择**逐字搬**成纯函数：输入是三条探测的**结果**（不含任何调用），
// 输出是一个枚举 + 一个"是否需要把失败细节写进日志"的布尔。
// 调用方只负责「跑探测 → 把结果喂进来 → 按返回值走原来的路径」，
// **选择顺序一个分支都不改**（改顺序 = 改行为，不属本次重构范围）。
//
// 于是 `NgxProviderPickTest.cpp` 可以在**无 GPU / 无游戏 / 无 NGX 头 / 无注册表**
// 的机器上把每一种组合钉住。
//
// ============================================================================
// 与 `DlssInput.cpp` 的 `route_output()` 的区别（别把两者混为一谈）
// ============================================================================
// `route_output()` 判的是「超分走 DLSS 直连还是走 FFX 原生」——那是**渲染路径**选择。
// 本文件判的是「NGX 由哪个模块提供」——那是**实现来源**发现。
// 两者输入不同、消费者不同、失败后果也不同，只是"都是按顺序挑一个"的结构相似。
//
// ============================================================================
// 零依赖的硬约束
// ============================================================================
// 本文件不 include d3d11.h / dxgi.h / nvsdk_ngx_*.h，不引用 ComPtr，不读任何全局。
// 只有 <cstdint> 与 <cstddef>。加依赖就会失去"能在任何机器上跑单测"这个唯一优势。
//
// 命名空间用小写 snake_case：与仓库其它模块（ffx12 / ngx / ngx_param_map /
// fsr2_family_takeover / transparent_jitter …）一致。

#pragma once

#include <cstddef>
#include <cstdint>

namespace ngx_provider_pick
{

// 用户态开关 `NgxProvider` 的三个取值（与 NgxBackend.cpp 的 kProvider* 同值）。
enum class Mode : std::int32_t
{
    Auto     = 0,   // 默认：先真 NGX（注册表），失败再找外部实现
    Nvidia   = 1,   // 只走真 NGX，失败即确定性失败（历史行为）
    External = 2,   // 只走外部实现（OptiScaler 之类）
};

// `try_nvidia_registry()` 的四个结局（与 ProviderStage 同值）。
enum class RegistryStage : std::int32_t
{
    Ok           = 0,
    NoRegistry   = 1,
    LoadFailed   = 2,
    MissingEntry = 3,
};

// 两条探测路线的结果 —— **只描述事实，不携带任何 DLL 句柄**。
//
// ★ 2026-11-17：外部那一侧原本拆成 external_load_ok / external_scan_ok 两个字段，但生产侧
//   **无法分别回填** —— try_external() 内部先 LoadLibraryW("_nvngx.dll") 再扫已加载模块，
//   对调用方只返回一个 bool。拆成两个字段的后果是 external_scan_ok 在生产里永远是 false，
//   "只扫到已加载模块"这条组合只有单测能构造 ⇒ 测试断言的状态生产不可达。
//   现合并为一个 external_found，与 try_external() 的返回值一一对应。
//   （两条路的**优先级**仍然留在 try_external() 内部，那是它唯一该在的地方。）
struct ProbeResults
{
    RegistryStage registry = RegistryStage::NoRegistry;   // 注册表那条路
    bool external_found    = false;   // try_external() 的返回值：load 或 scan 任一命中
};

// 最终决策。
enum class Decision : std::int32_t
{
    UseRegistry,          // 注册表那条路命中 ⇒ 用它
    UseExternal,          // 外部实现命中（load 或 scan）⇒ 用它
    HardFailRegistry,     // Mode::Nvidia 且注册表失败 ⇒ 确定性失败，不换来源
    SoftFailNoProvider,   // auto/external 且外部也没命中 ⇒ 软失败（可重试）
};

// 决策的附带信息，供调用方拼日志。**不参与判断**，只为"日志要能区分几种失败"。
struct PickOutcome
{
    Decision decision = Decision::SoftFailNoProvider;

    // 注册表失败时，是否要把它的 reason(+rc) 记进 fallback 明细。
    // true = 这次失败对最终结局有解释价值（auto 模式下会继续找外部，需要留下线索）。
    bool record_registry_detail = false;

    // ★ 2026-11-17：原有两个"外部明细"字段（record_external_detail / external_detail_empty）
    //   已删除。它们在任何输入下都由本文件写死、且调用方**一处都不读** —— 那正是"纯函数导出了
    //   一个生产不可达的状态"这一类问题的另一半：留着它们，单测就得为没人看的字段写断言，
    //   而断言通过并不能说明生产会做任何事。外部失败的 detail 由调用方从 try_external() 直接
    //   取（它才是该字符串的出处），不需要决策层转述。
};

// 纯函数：给定用户开关与三条探测结果，返回该走哪条路。
//
// 【顺序与原实现逐条对应】（NgxBackend.cpp ensure_init 第 1..3 步）：
//   1. Mode::Nvidia 跳过注册表？**不跳过** —— 原实现是 `if (mode != External)`
//      才试注册表，所以 Nvidia 也试。本函数保持同一条件。
//   2. 注册表命中 ⇒ UseRegistry（优先于一切外部实现）。
//   3. Mode::Nvidia 且注册表未命中 ⇒ HardFailRegistry（不换来源、不重试）。
//   4. 否则去找外部实现：load 或 scan 任一命中 ⇒ UseExternal。
//   5. 外部也没命中 ⇒ SoftFailNoProvider。
//
// ⚠️ Mode::External 时**不试注册表**（条件 1），所以 HardFailRegistry 不可能出现
//    —— 这条由 `assert`-风格的返回保证：External + 任意 registry 值都不会返回 HardFail。
//
// ★ 调用方（NgxBackend.cpp 的 ensure_init）**调它两次**，这不是冗余：
//   第一次跑在外部探测之前（外部两个输入恒 false），用来决定「注不注册表命中 / 硬失败 /
//   还要不要去找外部」；找到就填 ProbeResults::external_found，再用**同一份 probes**
//   问第二次，第二次的 decision 才是最终结局。
//   ⇒ 本函数必须保持**无副作用 + 只看入参**：两次调用之间唯一的差别就是 probes 被回填了。
//     若将来把任何探测挪进本函数，"外部半边永远不可达"那个缺陷就会以新的形式回来
//     （第一次调用拿到的是尚未发生的探测结果）。
PickOutcome pick(Mode mode, const ProbeResults &probes);

// 把 Mode 的合法范围夹住：未知整数一律回落 Auto（与 Dx11FsrBridge 的读键行为一致）。
// 单独暴露是为了让"读 ini → 夹值 → 决策"三步都能分别单测。
Mode clamp_mode(std::int32_t raw);

// 给日志用：Mode / RegistryStage / Decision 的稳定名字。
// 返回的是**静态字符串**，调用方不应释放。
const char *mode_name(Mode mode);
const char *stage_reason(RegistryStage stage);
const char *decision_name(Decision decision);

// 该决策是否意味着「初始化已经确定失败」（调用方据此决定走 fail_init 还是继续）。
//   HardFailRegistry / SoftFailNoProvider ⇒ true
//   UseRegistry / UseExternal             ⇒ false
bool is_failure(Decision decision);

// 该决策是否属于「软失败」（历史行为：不置 g_release_all，因为外部实现可能还没加载完）。
bool is_soft_failure(Decision decision);

} // namespace ngx_provider_pick
