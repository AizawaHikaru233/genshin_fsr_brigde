#pragma once
// Fsr2FamilyTakeover.h — Phase 1：FSR2 5-PS 合成族识别与预处理 pass 跳过。
//
// 背景（探针实测 7.0，见 D:\Dump\work\probe-verdicts-20260822.md）：
//   游戏 FSR2 上采样 = 每帧 5 个连续合成 PS pass（固定顺序），其中前 4 个是
//   render-size 预处理（重建/膨胀/混合等），第 5 个是 display-size 累积/上采样
//   （现桥 Mode 2 的替换目标）。前 4 个 pass 的输出只被族内消费。
//
// 本模块（默认关闭）在"上一次累积 pass 被桥成功替换"的前提下跳过 4 个预处理 pass，
// 消除双跑残余。纯 C++ 状态机，不依赖 D3D11/Windows（时间由调用方注入，可单测）。

#include <cstddef>
#include <cstdint>

namespace fsr2_family_takeover
{
// ---- 7.0 观测的合成族哈希（适配表初值；随版本复核） ----
constexpr std::uint64_t k_pre_hash_1 = 0x3CDF78FAC0ABCF6Dull; // PRE-1: cb0=1696, t1=render-size R8_TYPELESS
constexpr std::uint64_t k_pre_hash_2 = 0xAC63A3AF611EC7C9ull; // PRE-2: cb0=480, t1=160x560（SMAA LUT 变体）
constexpr std::uint64_t k_pre_hash_3 = 0x6018B8E925D4124Bull; // PRE-3: cb0=480, t1=render-size R8G8B8A8_TYPELESS
constexpr std::uint64_t k_pre_hash_4 = 0x590E69FEB210010Eull; // PRE-4: cb0=480, t1=render-size R8G8B8A8_TYPELESS
constexpr std::uint64_t k_accumulate_hash = 0x78057A29AF6C2D99ull; // 累积/上采样（现 Mode 2 目标）
constexpr std::uint64_t k_smaa_hash = 0xF41E6080D4BEA352ull;      // SMAA 模式合成（排除项）

// 重置状态（进程初始化/上下文重建时调用）
void reset();

bool is_pre_pass(std::uint64_t hash);
bool is_accumulate(std::uint64_t hash);
bool is_smaa(std::uint64_t hash);

// 预处理 pass 是否应跳过：
//   1) hash ∈ PRE 集合
//   2) 上次累积 pass 被桥成功替换（notify_accumulate_result(true)）
//   3) 未超过 expire_ms（now_ms - last_accumulate_tick <= expire_ms）
bool should_skip_pre(std::uint64_t hash, std::uint64_t now_ms, std::uint64_t expire_ms);

// 累积 pass 处理结果回填（try_fsr2_translation_draw 的返回值语义 + 当前时刻）
void notify_accumulate_result(bool replaced_ok, std::uint64_t now_ms);

// 接管权**交棒**（老接管者"离开接管"）时调用：立刻解除"跳过预处理 pass"的许可。
//
// 为什么必须有这一步（实机定案，2026-09-28）：
//   本状态机是**全局单份**——它只记住"上次累积 pass 被桥替换过"，不看实例。
//   P1 单实例接管下，接管权换给另一个实例后：
//     · 老实例的累积 pass 改由**游戏原生**执行（passthrough）；
//     · 可它的 4 个预处理 pass 仍被本状态机的全局许可跳过 ✗
//   ⇒ 原生 FSR2 输入缺失 ⇒ 该路视图冻在最后一帧（与"另一个机位实时"叠成残影）。
//   交棒瞬间显式解除许可，保证"放行"真的把原生路径还回去（下一次累积 pass 若是
//   接管者自己的，会由 notify_accumulate_result(true) 重新武装——语义不变）。
void notify_takeover_leave();

// 交棒释放次数（诊断/验收：应随交棒事件增长，而不是恒 0）
std::uint64_t takeover_leave_count();

// ---------------------------------------------------------------------------
// P1 单实例接管：修复②的**唯一判定点**（纯函数 ⇒ 可离线单测）。
//
// 背景（实机回归，本轮）：修复②的第一版直接"多义 ⇒ 拒绝"，但接管者的 out_a/out_b 是
// **第一次成功 dispatch 时**才建立的 ⇒ 第一次 bootstrap 被拒 ⇒ 归属永远建立不起来 ⇒
// 双实例场景下超分**完全停止**（实机：ffx12_ambiguous_bootstrap=3072、out_a=out_b=0、
// ffx12_result 停在 14 条）。所以判据抽成纯函数，并把两条"不可饿死"不变式写死在里面：
//   · claimer_has_ownership == false       ⇒ 永不拒绝（放行它是建立归属的唯一途径）
//   · claimer_recently_dispatched == false ⇒ 永不拒绝（拒绝不刷新派发时间戳 ⇒ 自愈）
// 单测（Fsr2FamilyTakeoverTest）逐条钉住这两条 ⇒ 回归不可能再次悄悄发生。
struct TokenOnlyClaimFacts
{
    bool single_instance_takeover = false;  // Ffx12SingleInstance=1（P1 生效）
    bool second_instance_present = false;   // 当前确有多个**活跃**实例（非粘性，见下）
    // ⚠️ 实机回归（2026-09-28）：`second_instance_present` 早先被实现为**粘性**（"本进程出现过
    // 第二个实例"）。状态槽只增不减、场景/视图切换也不回收 ⇒ 一次为真终生为真 ⇒ 单视图场景下
    // 这个拒绝判据永久命中，接管者每个累积 draw 都被放行给原生 ⇒ 超分大幅降低、静止抖动/锯齿。
    // 现在拆成两个事实并**必须同时**为真才可能拒绝：
    //   · second_instance_present        — 本进程当前确有多个活跃实例（裁决出来的活性计数）
    //   · other_instance_currently_live   — 除认领者之外，确实还有实例在画目标累积 draw
    //     （"只调 Render、早就不产画面"的幽灵实例不算 ⇒ 不能否掉认领）
    bool other_instance_currently_live = false;
    bool claimer_is_current_taker = false;  // 认领者就是当前接管者
    bool claimed_by_token_only = false;     // match_path == 2（无输出归属校验）
    bool output_belongs_to_claimer = false; // 输出命中认领者的 out_a/out_b（path 1/3）
    // ⚠️ 实机定案（2026-09-29，左右 1:1 分屏）：只有"输出归属"这一种正面证据**不够**。
    // 原因（代码 + 日志逐条对齐，见下方 `claim_has_positive_evidence` 的注释）：
    //   接管者每一帧写的是自己**两个**输出缓冲之一，而 out_b 只在"成功派发过"之后才被记下；
    //   写第二个缓冲的那些 draw 走 path 2 ⇒ 无输出归属 ⇒ 被拒 ⇒ 那次能学会 out_b 的派发
    //   永远不发生（自锁）⇒ 那半屏在 FSR4 / 原生之间来回切（实机 86% 被拒 = 可见抖动）。
    // 本批新增第二种正面证据：**输入归属** —— 本 draw 采样用的 color 就是认领者上一次
    // 成功派发时用的那一张（同一视图逐帧稳定 ⇒ 用于区分"这两路视图"）。
    bool input_belongs_to_claimer = false;  // 本 draw 的 color 命中认领者上次派发用过的 color
    bool claim_has_generation = false;      // call_gen != 0（确有新 Render 代次）
    std::uint64_t same_size_token_candidates = 0; // 同尺寸未消费 token 的**活跃**实例数
    bool claimer_has_ownership = false;     // out_a/out_b 非 0（已有归属记忆）
    bool claimer_recently_dispatched = false; // 最近 500ms 内尝试过派发（健康）
};

// 一次 token-only 认领是否**已有正面证据**说明它属于认领者自己（抽出来是为了可离线单测）：
//   · 输出归属（path 1/3）：这个 draw 写的就是认领者记过的输出缓冲；
//   · 输入归属（本批新增）：这个 draw 采样用的 color 就是认领者上次派发用的那一张
//     （⇒ 是同一路视图的帧；另一路视图有它自己的 G-buffer）。
// 两者皆无 ⇒ 这次认领只是"token 代次最新"的推断，**不构成证据**（可能属于另一路视图）。
bool claim_has_positive_evidence(const TokenOnlyClaimFacts &facts);

// 【本批 2026-09-29 · 分屏 draw→实例 归属】一个 bootstrap 候选是否应**排除**（不认领它）。
//
// 背景（实机：左右 1:1 分屏，两路 render 尺寸相同）：bootstrap 只能按"token 代次最新"选实例
// ⇒ 实机上那一帧**另一路**的累积 draw 会被认到接管者头上（它的 `seen` 混进别人的 draw）。
// 现在多了一个不依赖代次顺序的证据：本 draw 采样的 color（每路视图各自的 G-buffer）。
//   · source_matches == true  ⇒ 是它的（正面证据；调用方走"首选"分支，这里恒 false）
//   · source_known && !matches && memory_fresh ⇒ **排除**（它记过的来源是新的、且确非本 draw）
//   · 记忆不新鲜（切视图/切场景后可能只是过期）或没有记忆（另一路从未派发过）⇒ **不排除**
//
// ⚠️ 第三条是**防死锁不变式**，必须由单测钉住：若对"过期记忆"也排除，接管者在换了 G-buffer
//    之后会永久认不出自己的 draw（既没有正面证据、又被自己排除）⇒ 视图永远走原生。
bool p1_exclude_candidate_by_source(bool source_known, bool source_matches, bool memory_fresh);

// true = 拒绝这次认领（调用方：放行游戏原生）
bool p1_refuse_token_only_claim(const TokenOnlyClaimFacts &facts);

// ---------------------------------------------------------------------------
// P1 活性判据（**不是**"历史上出现过"的粘性条件）。
//
// 语义：本进程**当前**是否真的还有第二个"仍在画目标累积 draw"的实例。
// 为什么必须有它（实机回归，2026-09-28）：
//   早先的判据是 `sdk234_inst_count > 1`（状态槽只增不减 ⇒ 一次为真，终生为真）。而状态槽
//   在**场景/视图切换**时并不回收：老实例的槽会一直留着（它的 :g 代次冻住不再推进，却仍在
//   被游戏调用 Render ⇒ 每帧都有一枚新鲜的 250ms token）。于是"多实例"永久为真：
//     · `fsr2_family_should_skip_draw` 永久关闭跳过许可；
//     · `release_untagged_accumulate` 永久放行认不出归属的累积 draw；
//     · 同尺寸 token 候选数恒为 2 ⇒ `p1_refuse_token_only_claim` 永久拒绝接管者自己的认领。
//   三者叠加 ⇒ 普通（单视图）场景下桥的超分被大面积放行给游戏原生 ⇒ 用户报告
//   "超分效果大幅降低、静止状态边缘抖动/锯齿"（= 时域累积没生效的典型签名）。
//
// 判据定义：某个实例在窗口内被本函数**匹配到过**（= 它确实还在画这一路的累积 draw）。
//   幽灵实例（只调 Render、不再画）永远不满足 ⇒ 不再能污染任何裁决 ✓
//   从未出现过第二个实例的单实例场景 ⇒ 恒为 false ⇒ 行为与引入本判据之前**逐字相同** ✓
//
// 入参（两个数组**同序对齐**，由调用方保证；count 为有效项数）：
//   instances[i]        — 第 i 个已知实例指针
//   last_seen_ms[i]     — 该实例最近一次"被本函数匹配到"的时刻（0 = 从未匹配 ⇒ 视为不活跃）
//   exclude_instance    — 当前正在认领的实例（它自己不算"另一个实例"）
//   now_ms / live_ms    — 当前时刻与活性窗口（与 P1 裁决用的 k_p1_live_ms 同值）
bool p1_other_instance_currently_live(const std::uint64_t *instances,
                                      const std::uint64_t *last_seen_ms,
                                      std::size_t count,
                                      std::uint64_t exclude_instance,
                                      std::uint64_t now_ms,
                                      std::uint64_t live_ms);

// ---------------------------------------------------------------------------
// ★★【发布构建裁剪（2026-10-01 · 收尾清理）】下面这一段（直到本文件末尾的三个 `#endif`
//   之前）全部是**只喂日志**的东西，发布构建整体排除（`DX11FSRBRIDGE_ENABLE_DIAGNOSTICS`
//   未定义）；**诊断构建照旧全部保留** ⇒ 将来复查靠诊断构建。
//     · 问题 A  ：累积 draw 去向分类（`ffx12_accum_dest` 那一行的口径本体）
//     · 问题 B  ：输入别名分类（`ffx12_input_alias` 的 `kind=concurrent|stale_record`）
//     · B125    ：共享几何输入分类 + 抖动新鲜度（`sh_geom=` / `in_jit=` / `jit_frozen=`）
//   这三组的共同点：**没有任何消费者改变行为**（唯一消费者 = 诊断日志行）。
// ⚠️ 与之相对，**B124 的归属修法是功能**（修分屏可见抖动）：
//   `claim_has_positive_evidence` / `p1_exclude_candidate_by_source` / `TokenOnlyClaimFacts::
//   input_belongs_to_claimer` 位于本门控**之外**，发布构建必须保留。
// ⚠️ 单测仍要能测这三组：`Fsr2FamilyTakeoverTest` 与主 target 用**同一个**诊断开关
//   （见 CMakeLists 的 `DX11FSRBRIDGE_BUILD_FAMILY_TEST` 块）⇒ 诊断构建里该宏已定义。
#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)
// ---------------------------------------------------------------------------
// 【本批新增 · 问题 A】被选中实例的累积 draw"去向"分类（纯函数 ⇒ 可离线单测）。
//
// 为什么需要它：单实例接管（Ffx12SingleInstance=1）下，
//   · **未选中**那一路的累积 draw 有计数器（`ffx12_single_instance_passthrough`）；
//   · 而**被选中**那一路"没进 dispatch"的那些 draw 此前**没有任何计数器**：
//     · 修复②的多义拒绝（`ffx12_ambiguous_bootstrap`）有计数但口径是"拒绝"；
//     · 走到函数末 fail-closed（既不派发、也不放行）的那一类**完全不可见**
//       （`ffx12_skip` 在"最近 50ms 派发过"时被刻意抑制 ⇒ 典型形态打不出来）。
//   ⇒ 实机出现过"被选中实例只派发 2 次 / 另一路 passthrough 6656 次"这种读不出来的局面。
//
// 分类口径 = 代码里的**决策顺序**（见 `classify_accumulate_destination` 的判定顺序），
// 与 `try_fsr2_translation_draw` 里各个 return 点一一对应；纯函数只为"可离线单测"。
enum class AccumulateDestination
{
    None = 0,
    Dispatch,                 // ① 进了 dispatch（推进 FSR 历史）
    DispatchRepair,           // ①' 输出修复拷贝（同代次配对 draw，不推进历史）
    DispatchReuse,            // ①'' 同代次复用拷贝（不推进历史）
    PassthroughNotTaker,      // ② 未被选中 ⇒ 放行游戏原生
    PassthroughRefuse,        // ② 修复②多义拒绝 ⇒ 放行游戏原生
    PassthroughNativeWindow,  // ② 交棒后的纯原生窗口 ⇒ 放行游戏原生
    PassthroughStrict,        // ② E2 严格匹配 ⇒ 放行游戏原生
    FailClosed,               // ③ 被 fail-closed 吞掉（既不派发、也不放行）
    DispatchFailed,           // ⑤ 其它：派发失败（fail-open 放行原生）
    TextureMissing,           // ⑤ 其它：纹理取不到（继续走 fail-closed）
    Untagged,                 // ④ 没被匹配到任何实例
};

// 类别名（日志字段用；返回的指针指向静态字面量，生命周期同程序）
const char *accumulate_destination_name(AccumulateDestination destination);

struct AccumulateDestinationFacts
{
    bool matched = false;             // 该 draw 被某个实例认领（match_inst != 0）
    bool taker_branch_passthrough = false; // 未被选中（选举结论）⇒ 分支直接放行
    bool refused_ambiguous = false;   // 修复②多义拒绝 ⇒ 消费 token 后放行
    bool native_window = false;       // 交棒后的纯原生窗口
    bool strict_skip = false;         // E2 严格匹配实验
    bool textures_missing = false;    // color/depth/motion/output 未取全
    bool repair_copy = false;         // 输出修复拷贝
    bool reuse_copy = false;          // 同代次复用拷贝
    bool dispatch_ok = false;         // 真正派发成功
    bool dispatch_failed = false;     // 派发失败（返回 false，交回原生）
    bool fail_closed = false;         // 走到函数末 return true（吞掉这一 draw）
};

// 判定顺序（**就是分类口径本身**）：未匹配 > 未选中放行 > 纯原生窗口 > 多义拒绝 >
// 严格匹配 > 纹理缺失 > 修复拷贝 > 复用拷贝 > 派发 > 派发失败 > fail-closed。
AccumulateDestination classify_accumulate_destination(const AccumulateDestinationFacts &facts);

// ---------------------------------------------------------------------------
// 【本批新增 · 问题 B】`ffx12_input_alias` 的判据本体（纯函数 ⇒ 可离线单测）。
//
// 原判据（实机代码）**只比较两个 `ID3D11Texture2D` 的裸指针**：
//   `其他实例上次派发记下的 (color,depth)` == `本 draw 现场绑定到的 (color,depth)`
// 它**没有**任何活性/新鲜度条件 ⇒ 只要那个槽没被复用，"另一个实例"可以是**早已停止
// 派发（甚至已被游戏销毁）**的老实例。因此：
//   · 同一路视图在**场景/视图切换后重建实例**（新指针，但输入纹理被复用）也会命中；
//   · ⇒ 它**不能**单独作为"两路在共用同一份输入纹理"的证据。
// 本批只把判据补全（不改任何行为）：把命中分成"另一个实例**仍在派发**"（真·并发共用）
// 与"只是一条**陈旧记录**"（时间序交替 / 指针复用）两类，让日志一眼可判。
enum class InputAliasKind
{
    None = 0,      // 不是别名（指针不同，或没有另一条记录）
    Concurrent,    // 同 (color,depth) 指针 + 另一实例**最近仍在派发** ⇒ 真·两路共用
    StaleRecord,   // 同 (color,depth) 指针 + 另一实例早已静默 ⇒ 陈旧记录/指针复用
};

struct InputAliasFacts
{
    std::uint64_t current_color = 0; // 本 draw 现场绑定到的 color 资源指针
    std::uint64_t current_depth = 0;
    std::uint64_t other_color = 0;   // 另一个实例**上次派发**记下的 color 资源指针
    std::uint64_t other_depth = 0;
    std::uint64_t other_record_tick = 0; // 那条记录的时刻（GetTickCount64）
    std::uint64_t now_ms = 0;
    std::uint64_t recent_ms = 100;   // "仍在派发"的时间窗（一帧以上）
    bool other_currently_live = false; // 另一实例在活性表里（= 最近被匹配到过）
};

// 判据本体：只有**同一个 color 指针 + 同一个 depth 指针**才算"别名"（与桥侧逐字一致）。
bool input_alias_same_pointers(const InputAliasFacts &facts);
// 别名分类：同名指针 + 另一实例"仍在派发"（时间或活性表）⇒ Concurrent，否则 StaleRecord。
InputAliasKind classify_input_alias(const InputAliasFacts &facts);

// ---------------------------------------------------------------------------
// 【本批 2026-09-29 · B125】"两路共用同一份 depth/motion"的**分类**（纯函数 ⇒ 可离线单测）。
//
// 背景（实机定案 · 左右 1:1 分屏 · `Ffx12SingleInstance=0` · 诊断 DLL 743,936 B）：
//   两路各自的累积 draw 现场读数（`ffx12_accum_dest` 的 `in=`）为
//     甲路 inst=0x293A1B00：color=0xCE9C5AE9B0 depth=0xCE9C5E86F0 motion=0xCE9C5B2D30 out=…
//     乙路 inst=0x1301EF40：color=0xCE9C5E57F0 depth=0xCE9C5E86F0 motion=0xCE9C5B2D30 out=…
//   ⇒ color / out 各自不同，**depth 与 motion 是同一份**（16 行 × 2 秒、32 秒内恒定）。
//   而桥侧对资源的读取是**每个 draw 现读**（`PSGetShaderResources`/`OMGetRenderTargets`
//   在钩子现场；缓存的只有"值指纹 → 槽位号"的布局，不含资源指针）⇒ 这**不可能**是
//   "取了别处缓存的输入"；`in_slots` 两路相同也只是**固定槽位签名**（0/2/3/1）的常量，
//   与"解析时机"无关。
//   ⇒ 结论：**这是游戏自己的绑定**（`identify_target_upscaler_resources` 识别的就是游戏
//     自己那条累积 draw：两路都把同一张 depth、同一张 motion 绑在同一批槽位上）。
//
// 为什么还要这个纯函数：把上面那条事实变成**每行可判**的日志字段，并明确区分三种情形：
//   · `DepthAndMotion` / `DepthOnly` / `MotionOnly` —— 另一路**在线**（活性表里确实还有它）
//     ⇒ 游戏把同一份几何输入绑给了两路（本批实机形态）；
//   · `None` —— 另一条记录**早已静默**（陈旧记录）或那其实是**同一路**（color 相同 =
//     视图/实例重建）⇒ 按 B123 已定的口径，**不算**"两路共用"。
// ⚠️ 本函数**不参与任何决策**：它只决定日志字段 `sh_geom=` / `sh_other=` 的取值。
enum class SharedGeometryKind
{
    None = 0,           // 没有"另一路在线、且与我们同几何输入"的实例
    DepthOnly = 1,      // 只共用 depth
    MotionOnly = 2,     // 只共用 motion
    DepthAndMotion = 3, // depth 与 motion 都是同一份
};

struct SharedGeometryFacts
{
    std::uint64_t self_color = 0;  // 本实例**上次成功派发**用的输入（0 = 从未派发过）
    std::uint64_t self_depth = 0;
    std::uint64_t self_motion = 0;
    std::uint64_t other_color = 0; // 候选"另一路"上次成功派发用的输入
    std::uint64_t other_depth = 0;
    std::uint64_t other_motion = 0;
    bool other_currently_live = false; // 候选"另一路"在活性表里（= 它确实还在画）
};

SharedGeometryKind classify_shared_geometry(const SharedGeometryFacts &facts);
const char *shared_geometry_kind_name(SharedGeometryKind kind);

// ---------------------------------------------------------------------------
// 【本批 2026-09-29 · B125】喂给 FSR4 的**抖动来源**是否"新"（纯函数 ⇒ 可离线单测）。
//
// 实机事实（本场 22:25:49~22:32:34，全部 `ffx12_dispatch` 采样，逐条可复算）：
//   · 进程最初那几帧：`jitter_px` 是**逐帧变化的 Halton**（0.5 / 0.25 / 0.75 / 0.125 …）✓
//   · 之后每个实例的 `jitter_px` **恒定**：分屏那一路 13/13 次采样同值、另一路 108/108 次
//     同值；两个历史会话里同样是 47/47 与 28/28 ⇒ ★【抖动输入全程是常数】✗
// 为什么这与"画面偏软 + 边缘闪"直接相关：FSR 的时域累积靠**逐帧变化的亚像素抖动**取得
//   采样多样性；抖动恒定 ⇒ 没有新的亚像素信息 ⇒ 重建只能停在"单帧放大 + 不收敛的历史"上
//   （本仓自己的记录就写着"某视图槽位冻结导致 jitter 冻结 → 无 AA"）。
// 判据：把"本帧抖动与上一帧**逐位相同**"记成冻结，并累计连续冻结帧数（供日志与后续判据用）。
// ⚠️ 本函数**不参与任何决策**：它只喂 `in_jit=` / `jit_frozen=` 两个日志字段。
struct JitterFreshness
{
    bool fresh = true;               // 本帧抖动相对上一帧是否变化（真 = 有抖动多样性）
    std::uint32_t frozen_frames = 0; // 连续"与上一帧相同"的帧数（fresh 时为 0）
    bool has_previous = false;       // 是否已有上一帧可比（首帧无可比对象 ⇒ 视为 fresh）
};

JitterFreshness classify_jitter_freshness(float current_x, float current_y,
                                          float previous_x, float previous_y,
                                          bool has_previous,
                                          std::uint32_t frozen_frames_so_far);
#endif // DX11FSRBRIDGE_ENABLE_DIAGNOSTICS（只喂日志的分类纯函数：去向 / 输入别名 / 共享几何 / 抖动新鲜度）

// 统计（限频日志用）
std::uint64_t skipped_count();
std::uint64_t accumulate_replaced_count();
} // namespace fsr2_family_takeover
