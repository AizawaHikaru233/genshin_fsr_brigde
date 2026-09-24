// Fufu bootstrap 的 ReShade 路径写入策略测试（2026-09-23）
//
// 做法：`#include` 被测 .cpp —— 其实现全在匿名命名空间内，外部无法链接，
// 但同一 TU 内可见。exe 所在目录即"游戏目录"（module_path(nullptr) 取自身路径）。
#include "FufuGraphicsPlugin.cpp"

#include <cstdio>

static int g_ok = 0;
static int g_bad = 0;

static void chk(bool cond, const char *msg)
{
    if (cond) { std::printf("  ok:   %s\n", msg); ++g_ok; }
    else      { std::printf("  FAIL: %s\n", msg); ++g_bad; }
}

int main()
{
    std::printf("FufuReshadeConfigTest\n");

    const std::filesystem::path exe_dir = module_path(nullptr).parent_path();
    const std::filesystem::path game_ini = exe_dir / L"ReShade.ini";
    const std::filesystem::path game_preset = exe_dir / L"ReShadePreset.ini";

    // 造出 prepare_reshade_game_configuration 需要的最小布局：
    // ReShade.ini（用出厂模板，两项路径为空）+ ReShadePreset.ini（已存在，避免走复制分支）
    std::error_code ec;
    std::filesystem::remove(game_ini, ec);
    std::filesystem::remove(game_preset, ec);

    std::string template_bytes;
    if (!read_utf8_file(exe_dir / L"ReShade.template.ini", template_bytes))
    {
        std::printf("  FATAL: 缺少 ReShade.template.ini\n");
        return 2;
    }
    write_utf8_file(game_ini, template_bytes);
    write_utf8_file(game_preset, "[GENERAL]\r\n");

    BootstrapConfig cfg {};
    cfg.reshade_path = exe_dir / L"payload" / L"ReShade" / L"ReShade64.dll";
    std::filesystem::create_directories(exe_dir / L"payload" / L"ReShade" / L"reshade-shaders", ec);

    // ---- 第一轮：两项为空 ⇒ 应被写入 ----
    chk(prepare_reshade_game_configuration(cfg), "第一轮：prepare 返回 true");
    chk(!ini_value_is_empty_utf8(game_ini, "GENERAL", "PresetPath"), "第一轮：PresetPath 已写入（非空）");
    chk(!ini_value_is_empty_utf8(game_ini, "SCREENSHOT", "SavePath"), "第一轮：SavePath 已写入（非空）");
    chk(!ini_value_is_empty_utf8(game_ini, "ADDON", "AddonPath"), "第一轮：AddonPath 已写入");
    chk(!ini_value_is_empty_utf8(game_ini, "GENERAL", "EffectSearchPaths"), "第一轮：EffectSearchPaths 已写入");
    chk(!ini_value_is_empty_utf8(game_ini, "GENERAL", "TextureSearchPaths"), "第一轮：TextureSearchPaths 已写入");

    // ---- 第二轮：用户改过这两项 ⇒ 必须保留 ----
    set_ini_value_utf8(game_ini, "GENERAL", "PresetPath", "D:\\MY\\Custom.ini");
    set_ini_value_utf8(game_ini, "SCREENSHOT", "SavePath", "D:\\MY\\Shots");
    // 同时把 payload 路径写成旧值，验证它们【仍会被覆写】
    set_ini_value_utf8(game_ini, "GENERAL", "EffectSearchPaths", "D:\\OLD\\Shaders");

    chk(prepare_reshade_game_configuration(cfg), "第二轮：prepare 返回 true");

    std::string bytes;
    read_utf8_file(game_ini, bytes);
    const bool kept_preset = bytes.find("D:\\MY\\Custom.ini") != std::string::npos;
    const bool kept_save = bytes.find("D:\\MY\\Shots") != std::string::npos;
    const bool overwritten_effects = bytes.find("D:\\OLD\\Shaders") == std::string::npos;
    chk(kept_preset, "第二轮：PresetPath 保留用户值（未被覆写）");
    chk(kept_save, "第二轮：SavePath 保留用户值（未被覆写）");
    chk(overwritten_effects, "第二轮：payload 路径仍被覆写（旧值已消失）");

    // 清理
    std::filesystem::remove_all(exe_dir / L"payload", ec);
    std::filesystem::remove(game_ini, ec);
    std::filesystem::remove(game_preset, ec);
    std::filesystem::remove_all(exe_dir / L"Screenshots", ec);

    std::printf("  通过 %d / 失败 %d\n", g_ok, g_bad);
    return g_bad == 0 ? 0 : 1;
}
