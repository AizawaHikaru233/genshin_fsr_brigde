// 验证 parse_pattern 的 valid 标志（2026-09-19）
#include "PatternScanner.hpp"
#include <cstdio>
#include <string>

int main()
{
    int failures = 0;
    for (const auto &sig : pattern_scanner::k_signatures)
    {
        const auto p = pattern_scanner::parse_pattern(sig.text);
        std::printf("%-18s bytes=%3zu valid=%d", std::string(sig.name).c_str(), p.bytes.size(), p.valid ? 1 : 0);
        if (!p.valid)
        {
            std::printf("  error_offset=%zu", p.error_offset);
            ++failures;
        }
        std::printf("\n");
    }
    // 故意构造两类坏签名，均应被检出（这是本次新增 valid 标志的目的）
    //
    // ① 拼接处漏空格 → 非法十六进制。
    //    注意非法字符必须落在**偶数对齐**位置：若落在奇数位，会被当成上一字节的低位
    //    "吃掉"（hex_value 返回 -1 → 走到 `hi<0||lo<0` 分支，仍能被检出，
    //    但那样测的就不是"对齐后仍是垃圾"的情形了）。
    {
        const std::string bad = "48 83 C4 20 5E E9" "XY 00 00";
        const auto pb = pattern_scanner::parse_pattern(bad);
        std::printf("bad-invalid-hex    bytes=%3zu valid=%d", pb.bytes.size(), pb.valid ? 1 : 0);
        if (pb.valid) { std::printf("  <-- NOT DETECTED (BUG)\n"); ++failures; }
        else { std::printf("  error_offset=%zu (correctly rejected)\n", pb.error_offset); }
    }
    // ② 末尾只剩一个十六进制字符（不完整字节）
    {
        const std::string bad = "48 83 C4 20 5";
        const auto pb = pattern_scanner::parse_pattern(bad);
        std::printf("bad-incomplete     bytes=%3zu valid=%d", pb.bytes.size(), pb.valid ? 1 : 0);
        if (pb.valid) { std::printf("  <-- NOT DETECTED (BUG)\n"); ++failures; }
        else { std::printf("  error_offset=%zu (correctly rejected)\n", pb.error_offset); }
    }
    // ③ 合法输入不得被误判（回归保护）
    {
        const std::string good = "48 83 C4 20 5E E9 ?? ?? ?? ??";
        const auto pg = pattern_scanner::parse_pattern(good);
        std::printf("good-signature     bytes=%3zu valid=%d", pg.bytes.size(), pg.valid ? 1 : 0);
        if (!pg.valid) { std::printf("  <-- FALSE POSITIVE (BUG)\n"); ++failures; }
        else { std::printf(" (correctly accepted)\n"); }
    }

    std::printf(failures == 0 ? "ALL PASS\n" : "FAILURES: %d\n", failures);
    return failures == 0 ? 0 : 1;
}
