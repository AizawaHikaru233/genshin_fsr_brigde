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
    {
        "FindString",
        "56 48 83 EC 20 48 89 CE E8 ?? ?? ?? ?? 48 89 F1 89 C2 48 83 C4 20 5E E9 ?? ?? ?? ?? CC CC CC CC "
        "55 56 57 53 48 83 EC 28 48 8D 6C 24 20 48 C7 45 00 FE FF FF FF 48 89 CE 85 D2 74 4E"
    },
    {
        "FindObject",
        "40 53 48 83 EC 50 48 89 4C 24 60 48 8D 54 24 20 48 8D 4C 24 60 E8 ?? ?? ?? ?? 48 8B 08 48 85 C9 "
        "75 04 48 8D 48 08 E8 ?? ?? ?? ?? 48 8B 4C 24 20 48 8B D8 48 85 C9 74 11 48 83 7C 24 28 00 76 09"
    },
    {
        "ObjectActive",
        "48 89 5C 24 08 57 48 83 EC 20 0F B6 FA 48 8B D9 48 85 C9 74 22 E8 06 81 FF FF 48 85 C0 74 18 40 "
        "84 FF 48 8B C8 0F 95 C2 48 8B 5C 24 30 48 83 C4 20 5F E9 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? CC"
    },
    {
        "PlayerPerspective",
        "41 56 56 57 55 53 48 83 EC 20 41 89 D0 48 89 CE 80 3D ?? ?? ?? ?? 00 0F 85 27 01 00 00 48 8B BE "
        "?? ?? ?? ?? 48 85 FF 0F 84 FD 00 00 00 0F B6 86 ?? ?? ?? ?? 38 86 ?? ?? ?? ?? 41 0F 95 C6 45 08 C6 "
        "41 80 FE 01 75 41 88 86 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 80 B9 C7 00 00 00 00 0F 84 19 01 00 00"
    },
    {
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

        if (i + 1 >= text.size())
            break;
        const int hi = hex_value(text[i]);
        const int lo = hex_value(text[i + 1]);
        if (hi < 0 || lo < 0)
            break;
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
