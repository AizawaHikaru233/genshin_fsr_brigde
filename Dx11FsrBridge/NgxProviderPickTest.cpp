// NgxProviderPickTest.cpp —— `NgxProviderPick` 的零依赖单测。
//
// 为什么要有它：`ensure_init()` 的「实现来源选择」此前只能靠实机凑环境验证
// （无 OptiScaler / 只装桥 / 有注册表 / 无注册表 的四种组合），而这正是最难
// 二分的那类问题。把顺序抽成纯函数之后，这里可以用 20 条断言把**每一种组合**
// 钉住，机器上不需要任何 GPU / 游戏 / NGX 头 / 注册表。
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
    ProbeResults hit {};
    hit.registry = RegistryStage::Ok;
    PickOutcome o = pick(Mode::Nvidia, hit);
    CHECK_EQ(o.decision, Decision::UseRegistry);

    // 关键：注册表失败时**即使外部实现可用**也要硬失败 ——
    // 这是「显式 nvidia」的语义，原实现逐字如此。
    ProbeResults miss {};
    miss.registry          = RegistryStage::NoRegistry;
    miss.external_load_ok  = true;
    miss.external_scan_ok  = true;
    o = pick(Mode::Nvidia, miss);
    CHECK_EQ(o.decision, Decision::HardFailRegistry);

    // 硬失败不记 fallback 明细（终局，没有后续拼接）。
    CHECK(!o.record_registry_detail);
    CHECK(!o.record_external_detail);
}

// ---- Mode::External：跳过注册表，只看外部实现 ----
static void test_external_mode()
{
    // 即使注册表命中也不理它。
    ProbeResults both {};
    both.registry         = RegistryStage::Ok;
    both.external_load_ok = true;
    PickOutcome o = pick(Mode::External, both);
    CHECK_EQ(o.decision, Decision::UseExternal);

    // 注册表命中但外部没命中 ⇒ 软失败（不是 UseRegistry！）。
    // 这条是 External 与 Nvidia 的分水岭，最容易写错。
    ProbeResults reg_only {};
    reg_only.registry = RegistryStage::Ok;
    o = pick(Mode::External, reg_only);
    CHECK_EQ(o.decision, Decision::SoftFailNoProvider);

    // External 模式下永远不会返回 HardFailRegistry。
    for (int s = 0; s <= 3; ++s)
    {
        ProbeResults p {};
        p.registry = static_cast<RegistryStage>(s);
        CHECK(pick(Mode::External, p).decision != Decision::HardFailRegistry);
    }
}

// ---- Mode::Auto：注册表优先，失败后找外部，都不成才软失败 ----
static void test_auto_mode()
{
    // 注册表命中 ⇒ 用它（外部也可用时也不换）。
    ProbeResults reg {};
    reg.registry          = RegistryStage::Ok;
    reg.external_load_ok  = true;
    CHECK_EQ(pick(Mode::Auto, reg).decision, Decision::UseRegistry);

    // 注册表失败 + 外部 load 命中 ⇒ 用外部，并记下注册表失败原因。
    ProbeResults ext_load {};
    ext_load.registry         = RegistryStage::LoadFailed;
    ext_load.external_load_ok = true;
    PickOutcome o = pick(Mode::Auto, ext_load);
    CHECK_EQ(o.decision, Decision::UseExternal);
    CHECK(o.record_registry_detail);

    // 注册表失败 + 只有 scan 命中 ⇒ 同样用外部。
    ProbeResults ext_scan {};
    ext_scan.registry         = RegistryStage::MissingEntry;
    ext_scan.external_scan_ok = true;
    o = pick(Mode::Auto, ext_scan);
    CHECK_EQ(o.decision, Decision::UseExternal);

    // 全都不成 ⇒ 软失败，且两条明细都要留（否则日志里看不出试过什么）。
    ProbeResults none {};
    none.registry = RegistryStage::NoRegistry;
    o = pick(Mode::Auto, none);
    CHECK_EQ(o.decision, Decision::SoftFailNoProvider);
    CHECK(o.record_registry_detail);
    CHECK(o.record_external_detail);
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
    test_failure_classification();
    test_names();

    std::printf("NgxProviderPickTest: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
