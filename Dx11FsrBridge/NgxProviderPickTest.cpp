// NgxProviderPickTest.cpp —— `NgxProviderPick` 的零依赖单测。
//
// 为什么要有它：`ensure_init()` 的「实现来源选择」此前只能靠实机凑环境验证
// （无 OptiScaler / 只装桥 / 有注册表 / 无注册表 的四种组合），而这正是最难
// 二分的那类问题。把顺序抽成纯函数之后，这里可以把**每一种组合**钉住，
// 机器上不需要任何 GPU / 游戏 / NGX 头 / 注册表。
//
// ============================================================================
// 本测试覆盖哪一半：**注册表 + 外部两半都覆盖，但要按调用方真实的调用顺序测。**
// ============================================================================
// 生产侧（NgxBackend.cpp 的 ensure_init）**调 pick() 两次**：
//   ① 第一轮：外部探测还没跑 ⇒ external_found 恒为 false；
//   ② 第二轮：把 try_external() 的真实结局回填后，用同一份 probes 再问一次。
// 因此本文件的每个用例都按「同一探针事实、问两轮」组织：`pick_twice()` 就是那条
// 生产路径的模型，断言写在**第二轮的结局**上 —— 那才是调用方真正消费的 decision。
//
// ⚠️ 为什么强调这一点：曾经本文件里有若干断言直接构造"注册表失败 + 外部命中"的单轮输入，
//   而生产侧的 pick() 那时只在探测之前被调一次 ⇒ **那些断言测的是一个生产不可能出现的
//   状态**，测试通过与否和生产行为无关（这正是该缺陷长期没被发现的原因）。
//   现在任何断言都必须能对应到一条真实的两轮调用序列上。
//
// 断言风格与 NgxParamMapTest.cpp 一致（自计数 + 失败行），不引第三方框架。

#include "NgxProviderPick.h"

#include <cstdio>
#include <string>

using namespace ngx_provider_pick;

namespace
{
int g_checks = 0;
int g_failures = 0;

void check(bool cond, const char *what, int line)
{
    ++g_checks;
    if (!cond)
    {
        ++g_failures;
        std::printf("FAIL [line %d] %s\n", line, what);
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)
#define CHECK_EQ(a, b) check((a) == (b), #a " == " #b, __LINE__)

// 生产路径的模型：先问一轮（此时外部探测尚未发生），再按第一轮是否需要外部探测回填结果，
// 然后问第二轮。返回**两轮**的结局，让用例能同时断言「第一轮看到了什么」与「最终结局」。
//
// registry  = try_nvidia_registry() 的结局（Mode::External 时根本不探，由 probe_reg 控制）
// ext_found = try_external() 的真实返回值。它与「没跑探测」在结局上是同一回事：
//             生产侧用「第一轮 decision != UseRegistry」决定跑不跑，而注册表命中的那些组合
//             本来就轮不到外部参与。
struct TwoRounds
{
    PickOutcome first;
    PickOutcome final_round;
    bool        external_probed = false;   // 第二轮之前是否真的跑了 try_external()
};

TwoRounds pick_twice(Mode mode, RegistryStage registry, bool probe_reg, bool ext_found)
{
    ProbeResults probes {};
    if (probe_reg)
        probes.registry = registry;

    TwoRounds r {};
    r.first = pick(mode, probes);

    // 与 ensure_init 的接线逐字同构：注册表命中就不再找外部。
    if (r.first.decision != Decision::UseRegistry)
    {
        r.external_probed       = true;
        probes.external_found   = ext_found;
    }

    r.final_round = pick(mode, probes);
    return r;
}
} // namespace

// ---- clamp_mode：未知整数一律 Auto ----
static void test_clamp_mode()
{
    CHECK_EQ(clamp_mode(0), Mode::Auto);
    CHECK_EQ(clamp_mode(1), Mode::Nvidia);
    CHECK_EQ(clamp_mode(2), Mode::External);

    // 越界与负数都回落 Auto（与读 ini 的行为一致）。
    CHECK_EQ(clamp_mode(3), Mode::Auto);
    CHECK_EQ(clamp_mode(-1), Mode::Auto);
    CHECK_EQ(clamp_mode(99), Mode::Auto);
}

// ---- Mode::Nvidia：注册表命中用它，未命中即硬失败，不换来源 ----
static void test_nvidia_mode()
{
    // 三个非 Ok 档位逐档过一遍，按**两轮**模型断言结局。
    // ⚠️ 第一轮就已经是 HardFailRegistry 的组合，生产侧当场 fail_init 早退、
    //    **根本走不到外部探测** —— 这里的 ext_found=true 只是把"即使外部可用也不换来源"
    //    这个反例喂进决策层，它本身进不了真实路径。
    for (int s = 1; s <= 3; ++s)   // 1..3 = 三个非 Ok 档位
    {
        const RegistryStage reg = static_cast<RegistryStage>(s);
        const TwoRounds hf = pick_twice(Mode::Nvidia, reg, true, /*ext_found=*/true);
        CHECK_EQ(hf.first.decision, Decision::HardFailRegistry);
        CHECK_EQ(hf.final_round.decision, Decision::HardFailRegistry);
        CHECK(hf.external_probed);   // 模型里会探，但生产侧在此之前已 fail_init 早退

        // 硬失败不记 fallback 明细（终局，没有后续拼接）。
        CHECK(!hf.first.record_registry_detail);
    }

    // 注册表命中 ⇒ 用它，且**不探外部**（生产侧同一条件）。
    const TwoRounds ok = pick_twice(Mode::Nvidia, RegistryStage::Ok, true, /*ext_found=*/true);
    CHECK_EQ(ok.first.decision, Decision::UseRegistry);
    CHECK(!ok.external_probed);
    CHECK_EQ(ok.final_round.decision, Decision::UseRegistry);
}

// ---- Mode::External：跳过注册表，只看外部实现 ----
static void test_external_mode()
{
    // 两轮模型里 probe_reg=false：生产侧 `g_provider_mode != kProviderExternal` 才探注册表，
    // 所以 registry 这一档在本模式下**根本没被观测过**，喂什么值都不该影响结局。
    for (int s = 0; s <= 3; ++s)
    {
        const RegistryStage reg = static_cast<RegistryStage>(s);

        // 外部命中 ⇒ 用外部（注册表那档是什么都无所谓）。
        const TwoRounds hit = pick_twice(Mode::External, reg, /*probe_reg=*/false, /*ext_found=*/true);
        CHECK(hit.external_probed);
        CHECK_EQ(hit.final_round.decision, Decision::UseExternal);

        // 外部没命中 ⇒ 软失败（**不是** UseRegistry：本模式根本不信注册表）。
        // 这条是 External 与 Nvidia 的分水岭，最容易写错。
        const TwoRounds miss = pick_twice(Mode::External, reg, /*probe_reg=*/false, /*ext_found=*/false);
        CHECK_EQ(miss.final_round.decision, Decision::SoftFailNoProvider);

        // External 模式下永远不会返回 HardFailRegistry（两轮都不）。
        CHECK(miss.first.decision != Decision::HardFailRegistry);
        CHECK(miss.final_round.decision != Decision::HardFailRegistry);
    }
}

// ---- Mode::Auto：注册表优先，失败后找外部，都不成才软失败 ----
//
// ★ 本段是这次缺陷的原地：旧断言直接构造「注册表失败 + 外部命中」的单轮输入，而生产侧
//   那时只在探测之前调一次 pick() ⇒ 断言通过，但那条分支在生产里不存在。
//   现在一律按两轮模型测，且**同时**断言第一轮看到了什么（决定跑不跑外部探测）。
static void test_auto_mode()
{
    // 注册表四档 × 外部命中与否，全覆盖。
    for (int s = 0; s <= 3; ++s)
    {
        const RegistryStage reg = static_cast<RegistryStage>(s);
        const bool reg_ok = (reg == RegistryStage::Ok);

        // ---- 外部命中 ----
        const TwoRounds ext = pick_twice(Mode::Auto, reg, /*probe_reg=*/true, /*ext_found=*/true);
        if (reg_ok)
        {
            // 注册表命中 ⇒ 用它，且**不探外部**（外部可用也不换）。
            CHECK_EQ(ext.first.decision, Decision::UseRegistry);
            CHECK(!ext.external_probed);
            CHECK_EQ(ext.final_round.decision, Decision::UseRegistry);
        }
        else
        {
            // 第一轮：注册表没命中、外部尚未探测 ⇒ 只能回答「继续找外部」。
            CHECK_EQ(ext.first.decision, Decision::SoftFailNoProvider);
            CHECK(ext.first.record_registry_detail);   // 要留线索，否则软失败行看不出试过什么
            CHECK(ext.external_probed);
            // 第二轮：外部命中 ⇒ 最终用外部。**这就是曾经不可达的那条分支。**
            CHECK_EQ(ext.final_round.decision, Decision::UseExternal);
        }

        // ---- 外部未命中 ----
        const TwoRounds none = pick_twice(Mode::Auto, reg, /*probe_reg=*/true, /*ext_found=*/false);
        if (reg_ok)
        {
            CHECK_EQ(none.final_round.decision, Decision::UseRegistry);
        }
        else
        {
            // 两轮都是软失败：谁都没命中 ⇒ 不置 g_release_all，等外部实现加载完再试。
            CHECK_EQ(none.first.decision, Decision::SoftFailNoProvider);
            CHECK_EQ(none.final_round.decision, Decision::SoftFailNoProvider);
            CHECK(none.first.record_registry_detail);
        }
    }

    // ★ 不再有"load 命中"与"scan 命中"两条断言：生产侧 try_external() 只交回一个 bool
    //   （它内部先 LoadLibraryW 再扫模块），决策层也就只吃一个字段。为两条路分别写断言
    //   等于重新制造"测试断言一个生产不可达的状态"——那正是本轮要修的问题。
    //   两条路的**优先级**由 try_external() 自己保证，不在本测试的覆盖范围内。
}

// ---- 与改动前的内联实现逐组合等价（这是 MAJOR 修复的验收判据）----
//
// 背景：选择顺序原先是**内联在 ensure_init 里**的 if 链，抽成纯函数后必须证明"换实现不换结局"。
// 光在注释里写"逐字节等价"没有说服力，所以这里把**旧的 if 链**照抄成一个小模型，
// 与"新接线（两轮 pick + 按 decision 分派）"在 mode × 注册表 4 档 × 外部命中与否
// 的全部 24 种组合上逐一对比。任何一格不等 ⇒ 本次接线改变了生产行为，测试失败。

enum class Disposition
{
    UseRegistry,   // 用注册表那条命中的实现
    UseExternal,   // 用外部实现
    HardFail,      // fail_init（置 g_release_all，不可逆）
    SoftFail,      // soft_fail_init（可重试 12 次，不置 g_release_all）
};

const char *disposition_name(Disposition d)
{
    switch (d)
    {
    case Disposition::UseRegistry: return "use_registry";
    case Disposition::UseExternal: return "use_external";
    case Disposition::HardFail:    return "hard_fail";
    case Disposition::SoftFail:    return "soft_fail";
    default:                       return "unknown";
    }
}

// 改动前：ensure_init 第 1..3 步的内联 if 链（逐条照抄，含"注册表命中就不探外部"）。
Disposition old_disposition(Mode mode, RegistryStage reg, bool ext_found)
{
    const bool probe_reg = (mode != Mode::External);
    const RegistryStage stage = probe_reg ? reg : RegistryStage::NoRegistry;

    if (probe_reg && stage == RegistryStage::Ok)
        return Disposition::UseRegistry;
    if (probe_reg && mode == Mode::Nvidia)
        return Disposition::HardFail;   // 显式 nvidia 未命中：不重试、不换来源
    return ext_found ? Disposition::UseExternal : Disposition::SoftFail;
}

// 改动后：两轮 pick + 按 decision 分派（与 NgxBackend.cpp 的接线同构）。
Disposition new_disposition(Mode mode, RegistryStage reg, bool ext_found)
{
    const TwoRounds r = pick_twice(mode, reg, /*probe_reg=*/(mode != Mode::External), ext_found);

    // 硬失败：第一轮即判死（HardFailRegistry 只可能出现在第一轮）
    if (is_failure(r.first.decision) && !is_soft_failure(r.first.decision))
        return Disposition::HardFail;
    // 失败但可重试
    if (is_failure(r.final_round.decision))
        return Disposition::SoftFail;
    return r.final_round.decision == Decision::UseRegistry ? Disposition::UseRegistry
                                                           : Disposition::UseExternal;
}

static void test_equivalence_with_pre_change_inline_logic()
{
    static const Mode modes[] = { Mode::Auto, Mode::Nvidia, Mode::External };

    for (const Mode mode : modes)
    {
        for (int s = 0; s <= 3; ++s)
        {
            const RegistryStage reg = static_cast<RegistryStage>(s);
            for (int e = 0; e <= 1; ++e)
            {
                const bool ext_found = (e != 0);
                const Disposition before = old_disposition(mode, reg, ext_found);
                const Disposition after  = new_disposition(mode, reg, ext_found);
                if (before != after)
                {
                    // 打印具体是哪一格不同 —— 失败信息必须能直接定位，不能只说"不等"。
                    // ⚠️ 同时打原始整数值：stage_reason(Ok) 返回 "unknown"（Ok 不是失败档），
                    //    只打名字会让人以为这一格压根没跑。
                    std::printf("  mismatch: mode=%s registry=%s(%d) ext_found=%d  before=%s after=%s\n",
                                mode_name(mode), stage_reason(reg), s, ext_found ? 1 : 0,
                                disposition_name(before), disposition_name(after));
                }
                CHECK_EQ(before, after);
            }
        }
    }
}

// ---- is_failure / is_soft_failure：与调用方的分支一致 ----
static void test_failure_classification()
{
    CHECK(!is_failure(Decision::UseRegistry));
    CHECK(!is_failure(Decision::UseExternal));
    CHECK(is_failure(Decision::HardFailRegistry));
    CHECK(is_failure(Decision::SoftFailNoProvider));

    // 软失败的语义：不置 g_release_all（外部 provider 可能还没加载完）。
    CHECK(!is_soft_failure(Decision::HardFailRegistry));
    CHECK(is_soft_failure(Decision::SoftFailNoProvider));
    CHECK(!is_soft_failure(Decision::UseExternal));
}

// ---- 名字稳定：日志里出现过的字符串不能悄悄改 ----
// ★ 这几个导出函数在**生产侧也有消费者**（ensure_init 的 "ngx_core provider pick" 行的
//   mode= / registry= / decision= 三个字段），不再是"只有单测在用"。改这里的返回值
//   等于改日志口径，必须同步改 NgxBackend.cpp 的检索判据。
static void test_names()
{
    CHECK(std::string(mode_name(Mode::Auto)) == "auto");
    CHECK(std::string(mode_name(Mode::Nvidia)) == "nvidia");
    CHECK(std::string(mode_name(Mode::External)) == "external");

    // 与 NgxBackend.cpp 的 provider_stage_reason() 逐条对应。
    CHECK(std::string(stage_reason(RegistryStage::NoRegistry)) == "no_registry");
    CHECK(std::string(stage_reason(RegistryStage::LoadFailed)) == "load_failed");
    CHECK(std::string(stage_reason(RegistryStage::MissingEntry)) == "missing_entry");

    CHECK(std::string(decision_name(Decision::UseRegistry)) == "use_registry");
    CHECK(std::string(decision_name(Decision::UseExternal)) == "use_external");
    CHECK(std::string(decision_name(Decision::HardFailRegistry)) == "hard_fail_registry");
    CHECK(std::string(decision_name(Decision::SoftFailNoProvider)) == "soft_fail_no_provider");
}

int main()
{
    test_clamp_mode();
    test_nvidia_mode();
    test_external_mode();
    test_auto_mode();
    test_equivalence_with_pre_change_inline_logic();
    test_failure_classification();
    test_names();

    std::printf("NgxProviderPickTest: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
