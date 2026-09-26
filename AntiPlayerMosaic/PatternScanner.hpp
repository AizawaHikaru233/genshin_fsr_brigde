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
    // 可选"首选 RVA"（0 = 未指定）。
    //
    // ⚠️ 2026-09-26（第六次修正）：**某些函数无法用纯字节模式唯一定位** ——
    // `GameObject::SetActive` 就是这种。本镜像里它的 64 字节模式有
    // **45 个逐字节完全相同的 IL2CPP 包装器**：所有固定字节一致，差别**只在**
    // `E8`/`E9` 位移字段里 —— 而位移是**由调用者自身地址派生**的
    //（45 个包装器都调用同一个 m_CachedPtr 解包助手，位移不同纯粹因为调用者地址不同）
    // ⇒ 位移**必然随版本漂移**，掩掉后 45 个全中、保留则等于把地址写死。
    // **结论：这个函数在字节层面就是不可唯一识别的。**
    //
    // 参照实现（`FufuLauncher/FufuLauncher.UnlockerIsland` 的 `Patterns.h`）
    // 对同一函数用的正是**按版本记录的偏移**（`SetActiveOffset`，如 `1452EE0`）
    // —— 它不是偷懒，而是这个问题在字节模式下的必然答案。
    //
    // 这里采用"**记录 RVA + 运行时校验**"的折中：
    //   - 仍随版本更新（改这一个数即可），但**绝不再盲信** ——
    //     使用时必须用 `text`（全掩码）在该 RVA 处**逐字节校验** ✓
    //   - 校验不过 ⇒ 显式记日志并回退到扫描，**绝不静默拿错函数** ✓
    std::uint32_t preferred_rva = 0;
};

inline constexpr Signature k_signatures[] {
    // ⚠️ 相对位移**不能当特征**。
    //
    // `call rel32`(E8) / `jmp rel32`(E9) / `jcc rel32`(0F 8x) / 短跳转(7x) 的操作数
    // 是**相对位移**——游戏在别处增删代码，位移就变，整个签名随即匹配不上，
    // 而 `scan_unique_signature` 要求**恰好 1 次**命中（`match_count != 1` 即失败），
    // 于是 5 条签名只要有一条失败，`cache_valid` 即被置 false ⇒ **HideUID 整体失效**。
    //
    // 因此本版把位移**尽量**掩成 `??`；掩到"恰好唯一"所需的最少保留为止（见每条注释）。
    //
    // ─────────────────────────────────────────────────────────────────────
    // ⚠️ 2026-09-24 重要更正（此前一轮的结论是**错的**）
    //
    // 上一轮曾断言 `PlayerPerspective` / `PlayerDiveMosaic` "指令序列本身已变、
    // 掩与不掩都 0 命中、需重新推导" —— **该结论错误**，成因是**扫描范围漏了节**：
    // 它只扫了 `.text`，而本 EXE 有**三个**可执行节：
    //
    //     .text    VA=0x00001000  VSize=0x021A9A96
    //     il2cpp   VA=0x06C05000  VSize=0x13255BC6   ← 巨大的可执行节
    //     .upx0    VA=0x19E5B000  VSize=0x00E921B8
    //
    // 这两条签名命中的 RVA（0x0CA7CB30 / 0x0B9DF1BF）**都落在 `il2cpp` 节内**，
    // 只扫 `.text` 自然 0 命中，却容易被误判成"游戏改了代码"。
    //
    // **本 DLL 自身的扫描是对的**：`scan_unique_signature` 遍历所有
    // `IMAGE_SCN_MEM_EXECUTE` 节 —— 出错的是当时那份**离线分析脚本**。
    // ⇒ **教训：离线实测必须复刻 DLL 的节遍历口径，否则会得出反向结论。**
    // ─────────────────────────────────────────────────────────────────────
    //
    // 实测口径（本机 YuanShen.exe 445006744 B / SHA256 7F89938D…，
    // 遍历全部可执行节、朴素全量比对、按操作码精确判定位移字段）：
    //
    //   FindString         32 字节  位移全掩                  命中 1 @0x004B2F50
    //   FindObject         63 字节  位移全掩（含栈偏移立即数） 命中 1 @0x01453720
    //   ObjectActive       64 字节  掩 14 / 保留 4            命中 1 @0x00C53D10
    //   PlayerPerspective  97 字节  掩 13 / 保留 0            命中 1 @0x0CA7CB30
    //   PlayerDiveMosaic   96 字节  掩 18 / 保留 4            命中 1 @0x0B9DF1BF
    //
    // **5/5 恰好唯一** ✓ —— 命中 RVA 与部署版 DLL 日志（2026-09-26 实机）逐条一致。
    //
    // 代价（实测，勿凭直觉判断）：掩掉位移会让"最长非通配连续段"变短，而
    // `find_matches` 用该段首字节做 `memchr` ⇒ **探测量上升**。C++ 实测
    // （`/O2`，遍历三个可执行节共 371 MB）逐条耗时：
    //
    //   FindString 23.3ms / FindObject 35.3ms / ObjectActive 263.5ms /
    //   PlayerPerspective 51.9ms / PlayerDiveMosaic 263.9ms   ⇒ **合计 637.9ms**
    //
    // 这是**启动时一次**的后台线程扫描（成功后会写 `.features.cache`），
    // 换来版本更新时的韧性 —— 划算。
    // ⚠️ 别用 Python 脚本的耗时来判断：`bytes.find` 比 C 的 `memchr` 慢约两个数量级,
    //    同一组签名在 Python 里是 ~62 秒，会得出"不可接受"的错误结论。
    //
    // 术语：E8/E9 掩其后 4 字节；0F 8x 掩其后 4 字节；短跳转只掩其后 1 字节。
    // **不要误掩** ModRM（例：`48 83 7C 24 28 00` 里的 `7C` 是 ModRM，不是 jcc；
    // `7C` 既是 jcc 操作码又常作 ModRM ⇒ 前一条指令的边界必须先判定对）。
    // 2026-09-26（第五次修正）：`FindString` 与 `FindObject` 改用**参照实现**
    // （`FufuLauncher/FufuLauncher.UnlockerIsland` 的 `Patterns/Patterns.h`，
    //   用户提供、实机确认可用 ✓）的版本。
    //
    // `FindString`：旧版在 `CC CC CC CC`（函数末尾对齐填充）之后**继续深入下一个函数**
    //   的 9 字节通用序言（`55 56 57 53 48 83 EC 28 …`）✗ —— 通用序言一旦被游戏改动
    //   就会整个失配 ✗；参照版**在填充处收尾** ✓。
    //
    // `FindObject`：参照版把**所有栈偏移立即数都掩成通配** ✓
    //   （`48 83 EC ??` / `48 89 4C 24 ??` / `75 ??` / `74 ??` / `48 83 7C 24 ?? 00`），
    //   而旧版把这些值**硬编码**（`50`/`60`/`20`/`60`/`04`/`11`/`28`/`09`）✗
    //   ⇒ 游戏一改栈布局就失效 ✗。
    //
    // 实测（本机 YuanShen.exe，遍历全部可执行节）：两版**各自都恰好命中 1 次**，
    // 且 RVA 相同（@0x004B2F50 / @0x01453720 ✓）⇒ 换版等效但更耐版本变动 ✓
    //
    // ⚠️ 注释**必须写在 `"名字",` 之前** ✗ —— `local-only/apm-verify-final.py`
    //    用 `"(\w+)"\s*,\s*("...")` 解析，注释插在中间会让该条**被静默跳过** ✗
    //    （本项目刚因此得到过一次"5/5"的**假通过** ✗）。
    //    注意 `FindObject` 末尾 `76`（`JBE rel8` 的操作码）**不含其 rel8 字节** ✓。
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
        // 位移掩 14 字节、**只保留 1 个 rel32**（`E8 96 70 7F 00`，即函数内第三处 call）。
        // 保留它是**唯一性所需**：全掩后本版命中 45 处（该代码形状有多个同形兄弟函数，
        // 彼此只在 rel32 上不同）。保留 1 个字段即可收敛到 1 处 ——
        // 比"保留全部 3 个 rel32"（12 字节硬编码）更耐版本变动。
        // 代价：这一个值仍属**版本锚定**，下次游戏更新可能失效；届时重测并只更新这 4 字节。
        "ObjectActive",
        // ⚠️ 2026-09-26（第六次修正）：位移**全部掩掉**（此前保留 `E8 96 70 7F 00`）。
        //
        // 保留那个位移是**本 bug 的真正原因** ✗：它把模式钉在了**错误的孪生**上 ——
        //   · 真 SetActive  @0x01452EE0（参考实现 `SetActiveOffset=1452EE0` 同一地址 ✓）
        //   · 我们命中的    @0x00C53D10  ← 另一个包装器，原生实现只写一个字节
        // 两者**所有固定字节完全相同**，只有 `E8`/`E9` 位移不同 ⇒ 保留位移 = 选错。
        // 掩掉后 45 个同形包装器全中 ⇒ 靠 `preferred_rva` + 本模式校验来选定 ✓
        "48 89 5C 24 ?? 57 48 83 EC 20 0F B6 FA 48 8B D9 48 85 C9 74 ?? E8 ?? ?? ?? ?? 48 85 C0 74 ?? 40 "
        "84 FF 48 8B C8 0F 95 C2 48 8B 5C 24 ?? 48 83 C4 20 5F E9 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? CC",
        0x01452EE0
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
