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
    // 带判据的函数（`discriminator` 非空）：
    //   ① 判据模式必须能完整解析；
    //   ② 包装器模式里必须**有且仅有**一个 `E9` —— `resolve_object_active` 的
    //      `tail_jump_offset` 靠这一不变量定位尾调用，多一个就会取错位置。
    for (const auto &sig : pattern_scanner::k_signatures)
    {
        if (sig.discriminator.empty())
            continue;

        const auto pd = pattern_scanner::parse_pattern(sig.discriminator);
        std::printf("discriminator %-14s bytes=%3zu valid=%d",
            std::string(sig.name).c_str(), pd.bytes.size(), pd.valid ? 1 : 0);
        if (!pd.valid) { std::printf("  error_offset=%zu  <-- NOT VALID (BUG)\n", pd.error_offset); ++failures; }
        else { std::printf("\n"); }

        const auto pw = pattern_scanner::parse_pattern(sig.text);
        std::size_t tail_jumps = 0;
        for (std::size_t i = 0; i < pw.bytes.size(); ++i)
        {
            if (!pw.bytes[i].wildcard && pw.bytes[i].value == 0xE9)
                ++tail_jumps;
        }
        std::printf("tail-E9 count %-14s count=%zu", std::string(sig.name).c_str(), tail_jumps);
        if (tail_jumps != 1) { std::printf("  <-- EXPECTED 1 (BUG)\n"); ++failures; }
        else { std::printf(" (ok)\n"); }
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
