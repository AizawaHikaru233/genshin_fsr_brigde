// Fsr2InputDump 的纯函数单测（无 D3D 设备、无游戏、无 Logger）
//
// 为什么这些必须测：本模块是"一次实机运行就要定性"的诊断工具。
// 若 PNG 写坏、或 motion 解码公式与后端 CS 不一致，用户跑一次拿到的图会是错的，
// 而错误会伪装成"翅膀区域没有异常"——正是我们要避免的误判。
//
// 覆盖：
//   1. crc32_bytes        —— 对齐标准测试向量
//   2. half_to_float      —— 对齐 IEEE 754 半精度
//   3. decode_motion_rgba10 —— 对齐 Ffx12Backend.cpp 的 CS 公式（含中性点 0.498039）
//   4. encode_png_rgba    —— 结构 + 每个 chunk 的 CRC + zlib stored 解压回读逐字节比对
//   5. make_raw_header    —— 离线脚本依赖的定长头
#include "Fsr2InputDump.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{

int g_pass = 0;
int g_fail = 0;

void check(const char *name, bool ok)
{
    if (ok)
    {
        ++g_pass;
        std::printf("[PASS] %s\n", name);
    }
    else
    {
        ++g_fail;
        std::printf("[FAIL] %s\n", name);
    }
}

std::uint32_t be32(const std::uint8_t *p)
{
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

// 只解 zlib 的 stored 块（本模块只产出这一种）
bool inflate_stored(const std::vector<std::uint8_t> &z, std::vector<std::uint8_t> &out)
{
    out.clear();
    if (z.size() < 6)
        return false;
    if (z[0] != 0x78)
        return false;
    std::size_t pos = 2;
    while (pos < z.size())
    {
        if (pos + 5 > z.size())
            return false;
        const std::uint8_t hdr = z[pos++];
        const int bfinal = hdr & 1;
        const int btype = (hdr >> 1) & 3;
        if (btype != 0)
            return false;
        const std::uint16_t len = static_cast<std::uint16_t>(z[pos] | (z[pos + 1] << 8));
        const std::uint16_t nlen = static_cast<std::uint16_t>(z[pos + 2] | (z[pos + 3] << 8));
        pos += 4;
        if (static_cast<std::uint16_t>(~len) != nlen)
            return false;
        if (pos + len > z.size())
            return false;
        out.insert(out.end(), z.begin() + static_cast<std::ptrdiff_t>(pos),
                   z.begin() + static_cast<std::ptrdiff_t>(pos + len));
        pos += len;
        if (bfinal)
            break;
    }
    return true;
}

std::uint32_t adler32_of(const std::vector<std::uint8_t> &d)
{
    std::uint32_t a = 1, b = 0;
    for (std::uint8_t x : d)
    {
        a = (a + x) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

bool nearly(float a, float b, float eps)
{
    return std::fabs(a - b) <= eps;
}

} // namespace

int main()
{
    // ---------- 1. CRC32 标准向量 ----------
    {
        const char *s = "123456789";
        const std::uint32_t crc = fsr2dump::crc32_bytes(reinterpret_cast<const std::uint8_t *>(s), 9);
        check("crc32(\"123456789\") == 0xCBF43926", crc == 0xCBF43926u);
    }

    // ---------- 2. half → float ----------
    {
        check("half 0x0000 == 0.0", nearly(fsr2dump::half_to_float(0x0000), 0.0f, 1e-6f));
        check("half 0x3C00 == 1.0", nearly(fsr2dump::half_to_float(0x3C00), 1.0f, 1e-6f));
        check("half 0x4000 == 2.0", nearly(fsr2dump::half_to_float(0x4000), 2.0f, 1e-6f));
        check("half 0xBC00 == -1.0", nearly(fsr2dump::half_to_float(0xBC00), -1.0f, 1e-6f));
        check("half 0x3555 ~= 0.3333", nearly(fsr2dump::half_to_float(0x3555), 0.333251953f, 1e-5f));
    }

    // ---------- 3. motion 平方解码（必须与后端 CS 一致） ----------
    {
        float mx = 0, my = 0;
        fsr2dump::decode_motion_rgba10(0.498039f, 0.498039f, &mx, &my);
        check("motion 中性点 0.498039 => 位移 0", nearly(mx, 0.0f, 1e-6f) && nearly(my, 0.0f, 1e-6f));

        fsr2dump::decode_motion_rgba10(0.5f, 0.5f, &mx, &my);
        check("motion 0.5 => 极小负位移", mx < 0.0f && std::fabs(mx) < 1e-4f);

        fsr2dump::decode_motion_rgba10(1.0f, 0.0f, &mx, &my);
        check("motion 1.0 => -4*d^2 (d=0.501961)", nearly(mx, -1.007843f, 1e-4f));
        check("motion 0.0 => +4*d^2 (d=-0.498039)", nearly(my, 0.992172f, 1e-4f));

        fsr2dump::decode_motion_rgba10(0.498039f, 1.0f, &mx, &my);
        check("motion x/y 独立解算", nearly(mx, 0.0f, 1e-6f) && my < 0.0f);
    }

    // ---------- 4. PNG：结构 + CRC + 解压回读 ----------
    {
        const std::uint32_t w = 3, h = 2;
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4);
        for (std::size_t i = 0; i < rgba.size(); ++i)
            rgba[i] = static_cast<std::uint8_t>(i * 7 + 3);

        const std::vector<std::uint8_t> png = fsr2dump::encode_png_rgba(rgba.data(), w, h);
        check("PNG 非空", !png.empty());

        const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        check("PNG 签名正确", png.size() > 8 && std::memcmp(png.data(), sig, 8) == 0);

        // 遍历 chunk，逐个校验 CRC
        std::size_t pos = 8;
        bool saw_ihdr = false, saw_idat = false, saw_iend = false;
        bool ihdr_ok = false, crc_all_ok = true;
        std::vector<std::uint8_t> idat;
        while (pos + 12 <= png.size())
        {
            const std::uint32_t len = be32(&png[pos]);
            if (pos + 12 + len > png.size())
                break;
            const char *type = reinterpret_cast<const char *>(&png[pos + 4]);
            const std::uint8_t *payload = &png[pos + 8];
            const std::uint32_t stored_crc = be32(&png[pos + 8 + len]);
            const std::uint32_t calc_crc = fsr2dump::crc32_bytes(&png[pos + 4], 4 + len);
            if (stored_crc != calc_crc)
                crc_all_ok = false;
            if (std::memcmp(type, "IHDR", 4) == 0)
            {
                saw_ihdr = true;
                ihdr_ok = (len == 13) && be32(payload) == w && be32(payload + 4) == h &&
                          payload[8] == 8 && payload[9] == 6 && payload[10] == 0 && payload[11] == 0 &&
                          payload[12] == 0;
            }
            else if (std::memcmp(type, "IDAT", 4) == 0)
            {
                saw_idat = true;
                idat.insert(idat.end(), payload, payload + len);
            }
            else if (std::memcmp(type, "IEND", 4) == 0)
            {
                saw_iend = true;
            }
            pos += 12 + len;
        }
        check("含 IHDR/IDAT/IEND", saw_ihdr && saw_idat && saw_iend);
        check("IHDR 字段正确 (w,h,8,6,0,0,0)", ihdr_ok);
        check("所有 chunk 的 CRC 正确", crc_all_ok);
        check("chunk 总长与文件长度一致", pos == png.size());

        // zlib 头 + 解压回读（每行前置 filter 0）
        check("zlib 头为 0x78 0x01", idat.size() > 2 && idat[0] == 0x78 && idat[1] == 0x01);
        std::vector<std::uint8_t> raw;
        check("zlib stored 解压成功", inflate_stored(idat, raw));

        std::vector<std::uint8_t> expect;
        for (std::uint32_t y = 0; y < h; ++y)
        {
            expect.push_back(0); // filter type 0
            const std::uint8_t *row = rgba.data() + static_cast<std::size_t>(y) * w * 4;
            expect.insert(expect.end(), row, row + static_cast<std::size_t>(w) * 4);
        }
        check("解压后像素与输入逐字节一致", raw == expect);

        // Adler-32（zlib 尾部 4 字节，大端）
        if (idat.size() >= 4)
        {
            const std::uint32_t stored = be32(&idat[idat.size() - 4]);
            check("Adler-32 正确", stored == adler32_of(raw));
        }
        else
        {
            check("Adler-32 正确", false);
        }

        // 边界：空输入与零尺寸
        check("w=0 返回空", fsr2dump::encode_png_rgba(rgba.data(), 0, h).empty());
        check("nullptr 返回空", fsr2dump::encode_png_rgba(nullptr, w, h).empty());
    }

    // ---------- 5. raw 头 ----------
    {
        const std::vector<std::uint8_t> hdr = fsr2dump::make_raw_header(1920, 1080, 24, 7680, 12345);
        check("raw 头为 64 字节", hdr.size() == 64);
        check("magic = FDMP", hdr[0] == 'F' && hdr[1] == 'D' && hdr[2] == 'M' && hdr[3] == 'P');
        auto le32 = [&](std::size_t o)
        {
            return static_cast<std::uint32_t>(hdr[o]) | (static_cast<std::uint32_t>(hdr[o + 1]) << 8) |
                   (static_cast<std::uint32_t>(hdr[o + 2]) << 16) |
                   (static_cast<std::uint32_t>(hdr[o + 3]) << 24);
        };
        check("version = 1", le32(4) == 1);
        check("width = 1920", le32(8) == 1920);
        check("height = 1080", le32(12) == 1080);
        check("format = 24", le32(16) == 24);
        check("row_pitch = 7680", le32(20) == 7680);
        std::uint64_t frame = 0;
        for (int i = 0; i < 8; ++i)
            frame |= static_cast<std::uint64_t>(hdr[24 + i]) << (8 * i);
        check("frame_index = 12345", frame == 12345);
        check("尾部补零", hdr[63] == 0);
    }

    // ---------- 6. 触发状态机（延时自动 / 热键 / 间隔） ----------
    {
        // (a) 未配置触发 ⇒ 立刻开抓（保持旧行为），且抓满 frames 帧即停
        {
            fsr2dump::TriggerConfig cfg;
            cfg.frames = 2;
            cfg.require_trigger = false;
            fsr2dump::TriggerRuntime rt;
            const std::uint64_t t = 1000;
            fsr2dump::TriggerOut a = fsr2dump::trigger_update(cfg, rt, t, false);
            check("触发:未配置 ⇒ 立刻开抓", a.started && a.capture_now && !a.finished);
            check("触发:source=immediate", std::string(a.source) == "immediate");
            fsr2dump::TriggerOut b = fsr2dump::trigger_update(cfg, rt, t + 1, false);
            check("触发:第2帧抓到且本轮结束", b.capture_now && b.finished);
            fsr2dump::TriggerOut c = fsr2dump::trigger_update(cfg, rt, t + 2, false);
            check("触发:未配置时不会无限抓（抓满即停）", !c.capture_now && !c.started);
        }

        // (b) 热键触发：可重复；会话进行中忽略新的按下
        {
            fsr2dump::TriggerConfig cfg;
            cfg.frames = 3;
            cfg.hotkey = 122;
            cfg.require_trigger = true;
            fsr2dump::TriggerRuntime rt;
            const std::uint64_t t = 5000;
            check("触发:配了热键但没按 ⇒ 不抓",
                  !fsr2dump::trigger_update(cfg, rt, t, false).capture_now);
            fsr2dump::TriggerOut s1 = fsr2dump::trigger_update(cfg, rt, t + 10, true);
            check("触发:热键按下 ⇒ 开抓且当帧即抓",
                  s1.started && s1.capture_now && std::string(s1.source) == "hotkey");
            fsr2dump::TriggerOut s2 = fsr2dump::trigger_update(cfg, rt, t + 11, true);
            check("触发:会话中再按热键被忽略", !s2.started && s2.capture_now);
            fsr2dump::TriggerOut s3 = fsr2dump::trigger_update(cfg, rt, t + 12, false);
            check("触发:第3帧抓完 ⇒ finished", s3.finished && s3.capture_now);
            check("触发:轮次计数=1", rt.session_count == 1);
            fsr2dump::TriggerOut s4 = fsr2dump::trigger_update(cfg, rt, t + 20, true);
            check("触发:抓完后热键可再次触发", s4.started && s4.capture_now);
            check("触发:轮次计数=2", rt.session_count == 2);
        }

        // (c) 延时自动：到点触发一次，之后不再自动触发
        {
            fsr2dump::TriggerConfig cfg;
            cfg.frames = 1;
            cfg.autostart_sec = 30;
            cfg.require_trigger = true;
            fsr2dump::TriggerRuntime rt;
            const std::uint64_t t = 100000;
            check("触发:延时未到 ⇒ 不抓", !fsr2dump::trigger_update(cfg, rt, t, false).capture_now);
            check("触发:延时 29.9s ⇒ 不抓",
                  !fsr2dump::trigger_update(cfg, rt, t + 29900, false).capture_now);
            fsr2dump::TriggerOut a = fsr2dump::trigger_update(cfg, rt, t + 30000, false);
            check("触发:延时 30s ⇒ 自动开抓",
                  a.started && a.capture_now && std::string(a.source) == "timer");
            check("触发:自动只发生一次（标志置位）", rt.autostart_fired);
            fsr2dump::TriggerOut b = fsr2dump::trigger_update(cfg, rt, t + 90000, false);
            check("触发:再等 60s 也不会自动重复", !b.started && !b.capture_now);
        }

        // (d) 热键与定时都配 ⇒ 谁先到算谁；热键不消耗自动触发额度
        {
            fsr2dump::TriggerConfig cfg;
            cfg.frames = 1;
            cfg.hotkey = 122;
            cfg.autostart_sec = 30;
            cfg.require_trigger = true;
            fsr2dump::TriggerRuntime rt;
            const std::uint64_t t = 7000;
            fsr2dump::TriggerOut a = fsr2dump::trigger_update(cfg, rt, t + 100, true);
            check("触发:热键先到 ⇒ source=hotkey", a.started && std::string(a.source) == "hotkey");
            check("触发:热键触发不消耗自动触发", !rt.autostart_fired);
        }

        // (e) 帧间间隔：未到间隔不抓，到了才抓
        {
            fsr2dump::TriggerConfig cfg;
            cfg.frames = 3;
            cfg.interval_ms = 100;
            cfg.require_trigger = false;
            fsr2dump::TriggerRuntime rt;
            const std::uint64_t t = 2000;
            fsr2dump::TriggerOut a = fsr2dump::trigger_update(cfg, rt, t, false);
            check("间隔:首帧立刻抓", a.capture_now && !a.finished);
            fsr2dump::TriggerOut b = fsr2dump::trigger_update(cfg, rt, t + 50, false);
            check("间隔:未到 100ms ⇒ 不抓", !b.capture_now);
            fsr2dump::TriggerOut c = fsr2dump::trigger_update(cfg, rt, t + 100, false);
            check("间隔:到 100ms ⇒ 抓", c.capture_now && !c.finished);
            fsr2dump::TriggerOut d = fsr2dump::trigger_update(cfg, rt, t + 200, false);
            check("间隔:第3帧抓完 ⇒ finished", d.capture_now && d.finished);
        }

        // (f) 退化输入 frames=0 不应永不停机（按 1 帧处理）
        {
            fsr2dump::TriggerConfig cfg;
            cfg.frames = 0;
            cfg.require_trigger = false;
            fsr2dump::TriggerRuntime rt;
            fsr2dump::TriggerOut a = fsr2dump::trigger_update(cfg, rt, 42, false);
            check("退化:frames=0 按 1 帧处理并结束", a.capture_now && a.finished);
        }
    }

    std::printf("\n合计: %d 通过, %d 失败\n", g_pass, g_fail);
    if (g_fail == 0)
    {
        std::printf("ALL PASS\n");
        return 0;
    }
    return 1;
}
