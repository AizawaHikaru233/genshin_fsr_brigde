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
};

inline constexpr Signature k_signatures[] {
    // ⚠️ 2026-09-24：相对位移**不能当特征**。
    //
    // `call rel32`(E8) / `jmp rel32`(E9) / `jcc rel32`(0F 8x) / 短跳转(7x) 的操作数
    // 是**相对位移**——游戏在别处增删代码，位移就变，整个签名随即匹配不上，
    // 而 `scan_unique_signature` 要求**恰好 1 次**命中（`match_count != 1` 即失败），
    // 于是 5 条签名只要有一条失败，`cache_valid` 即被置 false ⇒ **HideUID 整体失效**。
    //
    // 本版对**实测可掩**的签名把位移掩成 `??`。实测（本机 YuanShen.exe
    // 445006744 B / SHA256 7F89938D…，逐签名整段穷举掩码组合）：
    //
    //   FindString      不掩=1 次，掩=1 次  ⇒ 唯一性充足，掩码只为稳健
    //   FindObject      不掩=1 次，掩=1 次  ⇒ 同上
    //   ObjectActive    不掩=0 次，掩=45 次 ⇒ **不唯一，掩码不可用**（见下）
    //   PlayerPerspective / PlayerDiveMosaic：**掩与不掩都是 0 次**
    //                                        ⇒ 指令序列本身变了，非位移问题
    //
    // 术语：E8/E9 掩其后 4 字节；0F 8x 掩其后 4 字节；短跳转只掩其后 1 字节。
    // **不要误掩** ModRM（例：`48 83 7C 24 28 00` 里的 `7C` 是 ModRM，不是 jcc）。
    {
        "FindString",
        "56 48 83 EC 20 48 89 CE E8 ?? ?? ?? ?? 48 89 F1 89 C2 48 83 C4 20 5E E9 ?? ?? ?? ?? CC CC CC CC "
        "55 56 57 53 48 83 EC 28 48 8D 6C 24 20 48 C7 45 00 FE FF FF FF 48 89 CE 85 D2 74 ??"
    },
    {
        "FindObject",
        "40 53 48 83 EC 50 48 89 4C 24 60 48 8D 54 24 20 48 8D 4C 24 60 E8 ?? ?? ?? ?? 48 8B 08 48 85 C9 "
        "75 ?? 48 8D 48 08 E8 ?? ?? ?? ?? 48 8B 4C 24 20 48 8B D8 48 85 C9 74 ?? 48 83 7C 24 28 00 76 ??"
    },
    {
        // ⚠️ 这条**不能掩位移**：掩掉后本机实测命中 **45 处**（该代码形状在本版里
        // 有多个同形兄弟函数，彼此**只在 rel32 上有别**）⇒ `scan_unique_signature`
        // 因 "!= 1" 失败。故保留具体位移值（**版本锚定**）。
        //
        // 三个 rel32 已按本版实际值更新（原值 A6 7E FF FF / 69 03 56 00 / D1 27 07 00
        // 对应旧版，本版为 96 70 7F 00 / 29 E8 00 00 / F1 38 87 00）——
        // 命中处 VA=0xC53D10，与该函数原有非位移字节逐字节吻合。
        //
        // 代价：下次游戏更新会再次失效。彻底解法需要**非字节级**锚点
        // （例如跟随 E8 解析被调函数再校验其序言），属独立改动。
        "ObjectActive",
        "48 89 5C 24 08 57 48 83 EC 20 0F B6 FA 48 8B D9 48 85 C9 74 22 E8 96 70 7F 00 48 85 C0 74 18 40 "
        "84 FF 48 8B C8 0F 95 C2 48 8B 5C 24 30 48 83 C4 20 5F E9 29 E8 00 00 48 8B CB E8 F1 38 87 00 CC"
    },
    {
        // ⚠️ 2026-09-24 实测：本版**掩与不掩都是 0 次命中** ⇒ 该函数的**指令序列
        // 本身已变**（非位移问题）。需要从新版二进制**重新推导签名**。
        // 证据（掩码后仍 0 命中的固定片段）：
        //   序言 `41 56 56 57 55 53 48 83 EC 20 41 89 D0 48 89 CE` 命中 0
        //   `41 0F 95 C6 45 08 C6`（setne r14b / mov byte [r8+8]）命中 0
        // 仅 `80 B9 C7 00 00 00 00 0F 84`（cmp byte [rcx+0xC7],0 / jz）还有 194 处命中，
        // 过于通用，不足以单独定位。
        "PlayerPerspective",
        "41 56 56 57 55 53 48 83 EC 20 41 89 D0 48 89 CE 80 3D ?? ?? ?? ?? 00 0F 85 27 01 00 00 48 8B BE "
        "?? ?? ?? ?? 48 85 FF 0F 84 FD 00 00 00 0F B6 86 ?? ?? ?? ?? 38 86 ?? ?? ?? ?? 41 0F 95 C6 45 08 C6 "
        "41 80 FE 01 75 41 88 86 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 80 B9 C7 00 00 00 00 0F 84 19 01 00 00"
    },
    {
        // ⚠️ 2026-09-24 实测：同 PlayerPerspective —— 掩与不掩都是 0 次命中，
        // 指令序列已变，需重新推导。证据（掩码后仍 0 命中的固定片段）：
        //   `89 C2 89 03 80 3D`（mov edx,eax / mov [rbx],eax / cmp byte[rip+x],0）命中 0
        //   `C7 03 FF FF FF FF 48 8D`（mov dword [rbx],-1 / lea）命中 0
        "PlayerDiveMosaic",
        "E8 ?? ?? ?? ?? 89 C2 89 03 80 3D ?? ?? ?? ?? 00 74 39 80 3D ?? ?? ?? ?? 00 0F 85 2E 03 00 00 85 D2 "
        "78 28 48 8B 86 ?? ?? ?? ?? 48 85 C0 0F 84 D7 03 00 00 48 8B 48 28 48 85 C9 0F 84 CF 03 00 00 E8 "
        "?? ?? ?? ?? C7 03 FF FF FF FF 48 8D 9E ?? ?? ?? ?? 8B 96 ?? ?? ?? ?? 85 D2 0F 89 0C 01 00 00"
    },
};

struct Pattern
{
    std::vector<PatternByte> bytes;
    std::size_t anchor_offset = 0;
    std::size_t anchor_size = 0;
    // 解析是否**完整**消费了输入（2026-09-19 审核报告）。
    //
    // 原先 `parse_pattern` 在两处直接 `break`（见下），**静默截断**：
    //   - 末尾只剩一个十六进制字符（不完整字节）
    //   - 遇到非十六进制字符
    // 截断后返回的是"部分模式"，症状是**扫描不到**（而非报错），难以归因。
    // 这个雷是真实存在的：`k_signatures` 的文本由**多个相邻字符串字面量拼接**而成
    //（如 "… 85 D2 " "78 28 …"），一旦有人在拼接处漏掉空格，
    // 就会出现 "D278" 之类的非法字节 → 静默截断。
    //
    // 注意：已核对**当前 5 个签名均完整解析**（60/64/64/97/96 字节），
    // 故此标志目前恒为 true —— 它的价值是**防止将来改签名时静默出错**。
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
