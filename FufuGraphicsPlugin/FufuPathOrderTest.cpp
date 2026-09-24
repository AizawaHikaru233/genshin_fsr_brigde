// Fufu bootstrap 组件路径解析顺序测试（2026-09-23）
//
// 目的：验证 `..\..\FSRGraphicsPayload\...`（旧 Lite 包布局）**排在候选列表末位** ——
// 若它排在首位，用户机器上残留的**过期** `FSRGraphicsPayload\` 会遮蔽当前正确路径，
// 导致加载到旧 DLL。
//
// 做法：`#include` 被测 .cpp（实现全在匿名命名空间内），手动设置 `g_module_directory`，
// 在沙箱里同时造出"过期旧布局"与"当前正确布局"，调 `load_config()` 断言选中的是后者。
#include "FufuGraphicsPlugin.cpp"

#include <cstdio>

static int g_ok = 0;
static int g_bad = 0;

static void chk(bool cond, const char *msg)
{
    if (cond) { std::printf("  ok:   %s\n", msg); ++g_ok; }
    else      { std::printf("  FAIL: %s\n", msg); ++g_bad; }
}

static void touch(const std::filesystem::path &p)
{
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    write_utf8_file(p, "x");
}

int main()
{
    std::printf("FufuPathOrderTest\n");

    // 沙箱：%TEMP%\fufutest\  ——  插件目录取 a\b\plugin，
    // 于是 `..\..\FSRGraphicsPayload` 正好落在沙箱内（不会写到沙箱外）。
    std::error_code ec;
    const std::filesystem::path sandbox = std::filesystem::temp_directory_path() / L"fufutest";
    std::filesystem::remove_all(sandbox, ec);

    const std::filesystem::path plugin_dir = sandbox / L"a" / L"b" / L"plugin";
    std::filesystem::create_directories(plugin_dir, ec);

    // 过期旧布局。注意 `..\..` 从 `plugin` 出发落到 `a\`（plugin→b→a），
    // 对应真实部署里 `<启动器>\FSRGraphicsPayload\`。
    const std::filesystem::path legacy_root = sandbox / L"a" / L"FSRGraphicsPayload";
    touch(legacy_root / L"Bridge" / L"Dx11FsrBridge.dll");
    touch(legacy_root / L"ReShade" / L"ReShade64.dll");
    touch(legacy_root / L"TextureLoader" / L"TextureLoader.dll");
    touch(legacy_root / L"OptiScaler" / L"OptiScaler.dll");

    // 当前正确布局
    touch(plugin_dir / L"payload" / L"Bridge" / L"Dx11FsrBridge.dll");
    touch(plugin_dir / L"payload" / L"ReShade" / L"ReShade64.dll");
    touch(plugin_dir / L"payload" / L"TextureLoader" / L"TextureLoader.dll");
    touch(plugin_dir / L"payload" / L"OptiScaler" / L"OptiScaler.dll");

    g_module_directory = plugin_dir;

    // 无 paths 文件 ⇒ 走 first_existing 候选列表
    const BootstrapConfig cfg = load_config();

    const auto ends_with = [](const std::filesystem::path &p, const wchar_t *tail) {
        const std::wstring s = p.wstring();
        const std::wstring t = tail;
        return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
    };

    chk(!cfg.bridge_path.empty(), "bridge: 解析到路径");
    chk(ends_with(cfg.bridge_path, L"plugin\\payload\\Bridge\\Dx11FsrBridge.dll"),
        "bridge: 选中当前布局（未被过期 FSRGraphicsPayload 遮蔽）");
    chk(ends_with(cfg.reshade_path, L"plugin\\payload\\ReShade\\ReShade64.dll"),
        "reshade: 选中当前布局");
    chk(ends_with(cfg.texture_loader_path, L"plugin\\payload\\TextureLoader\\TextureLoader.dll"),
        "texture_loader: 选中当前布局");
    chk(ends_with(cfg.optiscaler_path, L"plugin\\payload\\OptiScaler\\OptiScaler.dll"),
        "optiscaler: 选中当前布局");

    // 反向：删掉当前布局后，应能回退到旧布局（旧版用户仍可用）
    std::filesystem::remove_all(plugin_dir / L"payload", ec);
    const BootstrapConfig legacy = load_config();
    chk(ends_with(legacy.bridge_path, L"FSRGraphicsPayload\\Bridge\\Dx11FsrBridge.dll"),
        "回退：当前布局缺失时仍能找到旧 Lite 包布局");
    chk(ends_with(legacy.reshade_path, L"FSRGraphicsPayload\\ReShade\\ReShade64.dll"),
        "回退：ReShade 同样回退到旧布局");

    std::filesystem::remove_all(sandbox, ec);
    std::printf("  通过 %d / 失败 %d\n", g_ok, g_bad);
    return g_bad == 0 ? 0 : 1;
}
