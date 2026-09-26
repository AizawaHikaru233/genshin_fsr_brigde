#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace pattern_scanner
{
struct PatternByte
{
    bool wildcard;
    std::uint8_t value;
};

struct Signature
{
    std::string_view name;
    std::string_view text;
    // 可选"判据模式"（空 = 无）。用于**包装器模式本身无法唯一命中**的函数。
    //
    // `GameObject::SetActive` 就是这种：本镜像里它有 **45 个逐字节完全相同的
    // IL2CPP 包装器**，差别只在 `E8`/`E9` 位移字段 —— 而位移由**调用者自身地址**
    // 派生（45 个都调用同一个 `m_CachedPtr` 解包助手）⇒ 掩掉则 45 个全中、
    // 保留则等于把地址写死。**包装器层面不可唯一识别。**
    //
    // 但这些包装器的**尾调用目标**（真正的原生实现）形态各异 ⇒ 这里放原生实现的
    // 特征模式：先唯一命中原生实现，再取**尾调用它的那个**包装器。全程只用内容
    // 特征、不写死任何地址 —— 这也是它能同时在国服/国际服两份客户端上成立的原因。
    std::string_view discriminator {};
};

inline constexpr Signature k_signatures[] {
    // ⚠️ 相对位移**不能当特征**。
    //
    // `call rel32`(E8) / `jmp rel32`(E9) / `jcc rel32`(0F 8x) / 短跳转(7x) 的操作数
    // 是**相对位移** —— 游戏在别处增删代码，位移就变，整条签名随即失配；而
    // `scan_unique_signature` 要求**恰好 1 次**命中，5 条里只要有一条失败，
    // `cache_valid` 即为 false ⇒ **HideUID 整体失效**。故位移**尽量**掩成 `??`，
    // 掩到"恰好唯一"所需的最少保留为止（见各条注释）。
    //
    // ⚠️ **扫描必须遍历全部可执行节**：本 EXE 有**三个**
    //（`.text` / `il2cpp`（321 MB，最易被漏）/ `.upx0`），而
    // `PlayerPerspective` / `PlayerDiveMosaic` 就落在 `il2cpp` 里 ——
    // 只扫 `.text` 会得出"0 命中、游戏改了代码"的**反向结论**。
    //
    // 代价：掩掉位移会缩短"最长非通配连续段"，而 `find_matches` 以该段首字节做
    // `memchr` ⇒ 探测量上升。实测（`/O2`，三节共 371 MB）合计约 640 ms，是
    // **启动时一次**的后台扫描（结果写入 `.features.cache`）—— 换版本韧性，划算。
    //
    // 术语：E8/E9 掩其后 4 字节；0F 8x 掩其后 4 字节；短跳转只掩其后 1 字节。
    // **不要误掩 ModRM**（例：`48 83 7C 24 28 00` 里的 `7C` 是 ModRM，不是 jcc ——
    // 它既是 jcc 操作码又常作 ModRM ⇒ 必须先判定对前一条指令的边界）。
    //
    // 下面两条取自参照实现（`FufuLauncher.UnlockerIsland` 的 `Patterns.h`，
    // 实机确认可用）—— 共同点是**只覆盖稳定的指令形状**，不把立即数写死：
    //   * `FindString` 在 `CC CC CC CC`（函数末尾对齐填充）处收尾 —— 若继续深入
    //     下一个函数的通用序言，序言一改就整条失配。
    //   * `FindObject` 把所有栈偏移立即数掩成通配（旧版把 `50`/`60`/`20`… 硬编码
    //     ⇒ 游戏一改栈布局即失效）；末尾 `76`（`JBE rel8` 的操作码）**不含其 rel8 字节**。
    //
    // ⚠️ 注释**必须写在 `"名字",` 之前**：`local-only/apm-verify-final.py` 按
    //    `"(\w+)"\s*,\s*("...")` 解析，注释插在两者之间会让该条**被静默跳过**
    //    ⇒ 得到假的"5/5 全通过"。
    {
        "FindString",
        "56 48 83 EC 20 48 89 CE E8 ?? ?? ?? ?? 48 89 F1 89 C2 48 83 C4 20 5E E9 ?? ?? ?? ?? CC CC CC CC"
    },
    {
        "FindObject",
        "40 53 48 83 EC ?? 48 89 4C 24 ?? 48 8D 54 24 ?? 48 8D 4C 24 ?? E8 ?? ?? ?? ?? 48 8B 08 48 85 C9 "
        "75 ?? 48 8D 48 ?? E8 ?? ?? ?? ?? 48 8B 4C 24 ?? 48 8B D8 48 85 C9 74 ?? 48 83 7C 24 ?? 00 76"
    },
    {
        // 位移**全部掩掉**：本镜像里这个函数有 45 个逐字节同形的 IL2CPP 包装器，
        // 彼此只在 `E8`/`E9` 位移上不同 ⇒ 掩掉则全中，保留任一字段都等于把地址写死，
        // 而且会**钉在错误的孪生上**。故由下面的 `discriminator` 选定。
        "ObjectActive",
        "48 89 5C 24 ?? 57 48 83 EC 20 0F B6 FA 48 8B D9 48 85 C9 74 ?? E8 ?? ?? ?? ?? 48 85 C0 74 ?? 40 "
        "84 FF 48 8B C8 0F 95 C2 48 8B 5C 24 ?? 48 83 C4 20 5F E9 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? CC",
        // 原生实现 `GameObject::SetActive` 的特征模式。
        // 相对位移（call / jmp / jcc / lea rip）全掩；字段偏移 `[rbx+0x200]` 与断言号
        // `0x2AE` 也掩掉（都是版本易变值）。掩到这个程度**在两份客户端上仍恰好命中
        // 1 次**（国服 @0x19C7580 / 国际服 @0x19C6580），且两份客户端的指令序列逐条
        // 相同 ⇒ 这一条模式对国服与国际服通用。
        "40 53 48 83 EC 50 48 8B D9 84 D2 74 ?? E8 ?? ?? ?? ?? 84 C0 0F 85 ?? ?? ?? ?? 8B 83 ?? ?? ?? ?? "
        "48 8B CB C1 E8 04 A8 01 74 ?? E8 ?? ?? ?? ?? 33 C9 C6 44 24 40 00 48 89 4C 24 38 4C 8D 05 ?? ?? ?? ?? "
        "89 4C 24 30 41 B9 ?? ?? ?? ?? 89 44 24 28 48 8D 0D ?? ?? ?? ?? 33 D2 C7 44 24 20 01 00 00 00 "
        "E8 ?? ?? ?? ?? 48 83 C4 50 5B C3"
    },
    {
        // 13 处位移**全部掩掉**、保留 0 —— 全掩后本版仍恰好 1 次命中（@0x0CA7CB30，节 il2cpp）。
        // 命中处逐字节吻合：序言 `41 56 56 57 55 53 48 83 EC 20 41 89 D0 48 89 CE` 之后是
        // `80 3D ?? ?? ?? ?? 00`（cmp byte [rip+x],0）与 `0F 85 ?? ?? ?? ??`（jnz），
        // 以及 `41 0F 95 C6 45 08 C6`（setne r14b / mov byte [r8+8]）。
        "PlayerPerspective",
        "41 56 56 57 55 53 48 83 EC 20 41 89 D0 48 89 CE 80 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 48 8B BE "
        "?? ?? ?? ?? 48 85 FF 0F 84 ?? ?? ?? ?? 0F B6 86 ?? ?? ?? ?? 38 86 ?? ?? ?? ?? 41 0F 95 C6 45 08 "
        "C6 41 80 FE 01 75 ?? 88 86 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 80 B9 C7 00 00 00 00 0F 84 ?? ?? ?? ??"
    },
    {
        // 位移掩 18 字节、**只保留 1 个 rel32**（`0F 85 2E 03 00 00`，jnz +0x32E）。
        // 保留它是**唯一性所需**：全掩后本版命中 2 处（该函数在本版里有**两份同构副本**，
        // 另一处在 0x0F643C16）；保留这一个字段即收敛到 1 处，且正好指向旧签名命中的
        // 那一处（@0x0B9DF1BF，节 il2cpp）。
        // 命中处首字节是 `E8`（call rel32）—— `patch_player_dive_mosaic` 要求
        // `call_site[0] == 0xE8`，这是该点位的必要性质，改动时勿破坏。
        // 代价：这一个值仍属**版本锚定**，下次游戏更新可能失效；届时重测并只更新这 4 字节。
        "PlayerDiveMosaic",
        "E8 ?? ?? ?? ?? 89 C2 89 03 80 3D ?? ?? ?? ?? 00 74 ?? 80 3D ?? ?? ?? ?? 00 0F 85 2E 03 00 00 85 "
        "D2 78 ?? 48 8B 86 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? 48 8B 48 28 48 85 C9 0F 84 ?? ?? ?? ?? "
        "E8 ?? ?? ?? ?? C7 03 FF FF FF FF 48 8D 9E ?? ?? ?? ?? 8B 96 ?? ?? ?? ?? 85 D2 0F 89 ?? ?? ?? ??"
    },
};

struct Pattern
{
    std::vector<PatternByte> bytes;
    std::size_t anchor_offset = 0;
    std::size_t anchor_size = 0;
    // 解析是否**完整**消费了输入。两处截断（末尾只剩一个十六进制字符、遇到非十六进制
    // 字符）原先直接 `break` ⇒ 返回"部分模式"，症状是**扫描不到**而非报错，难以归因。
    // 这个雷是真实存在的：签名文本由**多个相邻字面量拼接**而成，拼接处漏一个空格
    // 就会出现 `D278` 这类非法字节。故标记为无效而不是静默截断。
    bool valid = true;
    // 出错位置（valid==false 时有效），用于日志定位
    std::size_t error_offset = 0;
};

inline Pattern parse_pattern(std::string_view text)
{
    Pattern pattern;
    for (std::size_t i = 0; i < text.size();)
    {
        while (i < text.size() && text[i] == ' ')
            ++i;
        if (i >= text.size())
            break;

        if (text[i] == '?')
        {
            pattern.bytes.push_back({ true, 0 });
            i += (i + 1 < text.size() && text[i + 1] == '?') ? 2 : 1;
            continue;
        }

        const auto hex_value = [](char ch) -> int {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
            return -1;
        };

        // 不完整字节（末尾只剩一个十六进制字符）→ 标记无效而不是静默截断
        if (i + 1 >= text.size())
        {
            pattern.valid = false;
            pattern.error_offset = i;
            break;
        }
        const int hi = hex_value(text[i]);
        const int lo = hex_value(text[i + 1]);
        // 非十六进制字符（最常见成因：多行字面量拼接处漏了空格）→ 同样标记无效
        if (hi < 0 || lo < 0)
        {
            pattern.valid = false;
            pattern.error_offset = i;
            break;
        }
        pattern.bytes.push_back({ false, static_cast<std::uint8_t>((hi << 4) | lo) });
        i += 2;
    }

    for (std::size_t start = 0; start < pattern.bytes.size();)
    {
        if (pattern.bytes[start].wildcard)
        {
            ++start;
            continue;
        }
        std::size_t end = start + 1;
        while (end < pattern.bytes.size() && !pattern.bytes[end].wildcard)
            ++end;
        if (end - start > pattern.anchor_size)
        {
            pattern.anchor_offset = start;
            pattern.anchor_size = end - start;
        }
        start = end;
    }
    return pattern;
}

inline bool matches_at(const std::uint8_t *data, std::size_t size, std::size_t position, const Pattern &pattern)
{
    if (position + pattern.bytes.size() > size)
        return false;
    for (std::size_t i = 0; i < pattern.bytes.size(); ++i)
    {
        if (!pattern.bytes[i].wildcard && data[position + i] != pattern.bytes[i].value)
            return false;
    }
    return true;
}

inline std::vector<std::size_t> find_matches(
    const std::uint8_t *data,
    std::size_t size,
    const Pattern &pattern,
    std::size_t maximum_matches = static_cast<std::size_t>(-1))
{
    std::vector<std::size_t> matches;
    if (pattern.bytes.empty() || pattern.anchor_size == 0 || size < pattern.bytes.size())
        return matches;

    const auto anchor_value = pattern.bytes[pattern.anchor_offset].value;
    const auto *cursor = data + pattern.anchor_offset;
    const auto *last_anchor = data + size - pattern.bytes.size() + pattern.anchor_offset;
    while (cursor <= last_anchor && matches.size() < maximum_matches)
    {
        const auto remaining = static_cast<std::size_t>(last_anchor - cursor + 1);
        const auto *anchor = static_cast<const std::uint8_t *>(std::memchr(cursor, anchor_value, remaining));
        if (anchor == nullptr)
            break;

        bool anchor_matches = true;
        for (std::size_t i = 0; i < pattern.anchor_size; ++i)
        {
            if (anchor[i] != pattern.bytes[pattern.anchor_offset + i].value)
            {
                anchor_matches = false;
                break;
            }
        }

        const std::size_t position = static_cast<std::size_t>(anchor - data) - pattern.anchor_offset;
        if (anchor_matches && matches_at(data, size, position, pattern))
            matches.push_back(position);
        cursor = anchor + 1;
    }
    return matches;
}
}
