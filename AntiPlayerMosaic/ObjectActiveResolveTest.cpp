// 验证 `SetActive` 的**动态解析**（国服/国际服两份客户端）。
//
// 做法：把真实客户端按 PE 节表映射进内存（节放到各自的 VirtualAddress），
// 再直接调用**产品代码里的** `resolve_object_active` —— 测的是真实现，不是复刻。
//
// 覆盖：
//   ① 两份客户端都能唯一解出【原生实现】与【尾调用它的包装器】，且 RVA 符合预期；
//   ② 判据模式被换成通用序言（多处命中）时必须**失败**（fail-closed，不许猜）；
//   ③ 判据模式文本损坏时必须失败。
//
// 二进制不在本机时该用例记 SKIP（不误报失败），但**在场却解错就必须 FAIL**。
#include "AntiPlayerMosaic.cpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
struct Case
{
    const char *tag;
    const char *path;
    std::uint32_t expected_wrapper_rva;
    std::uint32_t expected_impl_rva;
};

// 期望值来自离线实测（也用于交叉验证参照实现记录的 SetActiveOffset=0x1452EE0）。
const Case k_cases[] {
    { "CN", "D:\\miHoYo Games\\Genshin Impact Game\\YuanShen.exe", 0x01452EE0, 0x019C7580 },
    { "GL", "D:\\miHoYo Games\\hk4e_global\\GenshinImpact.exe", 0x01451EE0, 0x019C6580 },
};

// 按 PE 节表把文件摆放成内存映像：`resolve_object_active` 依赖"节在 VirtualAddress 处"。
bool map_image(const std::string &path, std::vector<std::uint8_t> &image, std::size_t &image_size)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    const std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (raw.size() < sizeof(IMAGE_DOS_HEADER))
        return false;

    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(raw.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > raw.size())
        return false;
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(raw.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return false;

    image_size = nt->OptionalHeader.SizeOfImage;
    image.assign(image_size, 0);

    // PE 头必须落在映像偏移 0 处 —— `resolve_object_active` 正是从 base 解析 DOS/NT 与节表。
    const std::size_t headers_size = (std::min)(static_cast<std::size_t>(nt->OptionalHeader.SizeOfHeaders), raw.size());
    std::memcpy(image.data(), raw.data(), (std::min)(headers_size, image_size));

    const auto *sections = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        const auto &section = sections[i];
        if (section.SizeOfRawData == 0 || section.PointerToRawData == 0)
            continue;
        if (static_cast<std::size_t>(section.PointerToRawData) + section.SizeOfRawData > raw.size())
            return false;
        const std::size_t to_copy = (std::min)(static_cast<std::size_t>(section.SizeOfRawData),
            image_size - section.VirtualAddress);
        std::memcpy(image.data() + section.VirtualAddress, raw.data() + section.PointerToRawData, to_copy);
    }
    return true;
}
} // namespace

int main()
{
    int failures = 0;
    int executed = 0;
    const auto &signature = pattern_scanner::k_signatures[2]; // ObjectActive

    // 把产品代码的 `log_line` 落到测试目录，失败时打印出来定位（默认 g_log_path 为空
    // 时日志会被静默丢弃）。
    wchar_t self_path[MAX_PATH] {};
    GetModuleFileNameW(nullptr, self_path, MAX_PATH);
    g_log_path = std::filesystem::path(self_path).parent_path() / "ObjectActiveResolveTest.log";
    {
        std::error_code ignored;
        std::filesystem::remove(g_log_path, ignored);
    }

    for (const auto &test_case : k_cases)
    {
        std::vector<std::uint8_t> image;
        std::size_t image_size = 0;
        std::ifstream probe(test_case.path, std::ios::binary);
        if (!probe)
        {
            std::printf("[%s] SKIP  客户端不在本机: %s\n", test_case.tag, test_case.path);
            continue;
        }
        probe.close();

        std::printf("[%s] ", test_case.tag);
        if (!map_image(test_case.path, image, image_size))
        {
            std::printf("FAIL  无法映射映像\n");
            ++failures;
            continue;
        }
        ++executed;

        auto *wrapper = resolve_object_active(image.data(), image_size, signature);
        if (wrapper == nullptr)
        {
            std::printf("FAIL  解析失败（应为 rva=0x%08X）\n", test_case.expected_wrapper_rva);
            ++failures;
            continue;
        }
        const auto wrapper_rva = static_cast<std::uint32_t>(wrapper - image.data());
        if (wrapper_rva != test_case.expected_wrapper_rva)
        {
            std::printf("FAIL  包装器 rva=0x%08X，应为 0x%08X\n", wrapper_rva, test_case.expected_wrapper_rva);
            ++failures;
            continue;
        }

        // 原生实现也必须落在预期位置（顺带证明判据命中的是同一个函数）
        const auto impl_pattern = pattern_scanner::parse_pattern(signature.discriminator);
        const auto impl_hits = pattern_scanner::find_matches(image.data(), image_size, impl_pattern);
        bool impl_ok = impl_hits.size() == 1;
        if (impl_ok)
        {
            const auto impl_rva = static_cast<std::uint32_t>(impl_hits.front());
            impl_ok = impl_rva == test_case.expected_impl_rva;
            if (!impl_ok)
                std::printf("  原生实现 rva=0x%08X，应为 0x%08X\n", impl_rva, test_case.expected_impl_rva);
        }
        else
        {
            std::printf("  原生实现命中 %zu 次，应恰好 1 次\n", impl_hits.size());
        }
        if (!impl_ok)
        {
            ++failures;
            continue;
        }

        std::printf("PASS  wrapper=0x%08X  impl=0x%08X  (%zu 个同形孪生)\n",
            wrapper_rva, test_case.expected_impl_rva, pattern_scanner::find_matches(
                image.data(), image_size, pattern_scanner::parse_pattern(signature.text)).size());

        // ② 判据换成通用序言 ⇒ 多处命中 ⇒ 必须失败（fail-closed）
        {
            auto tampered = signature;
            tampered.discriminator = "48 89 5C 24 08 57 48 83 EC 20";
            const std::size_t hits = pattern_scanner::find_matches(
                image.data(), image_size, pattern_scanner::parse_pattern(tampered.discriminator)).size();
            auto *bad = resolve_object_active(image.data(), image_size, tampered);
            if (bad != nullptr)
            {
                std::printf("      FAIL  判据多处命中(%zu)时仍返回了结果 <-- 会静默拿错函数\n", hits);
                ++failures;
            }
            else
            {
                std::printf("      ok   判据多处命中(%zu) ⇒ 正确失败\n", hits);
            }
        }
        // ③ 判据文本损坏 ⇒ 必须失败
        {
            auto tampered = signature;
            tampered.discriminator = "40 53 48 83 EC 5";
            if (resolve_object_active(image.data(), image_size, tampered) != nullptr)
            {
                std::printf("      FAIL  判据文本损坏时仍返回了结果\n");
                ++failures;
            }
            else
            {
                std::printf("      ok   判据文本损坏 ⇒ 正确失败\n");
            }
        }
    }

    if (executed == 0)
    {
        std::printf("SKIP  两份客户端都不在本机，未能实测动态解析\n");
        return 0;
    }
    if (failures != 0)
    {
        std::printf("--- 产品代码日志（%s）---\n", g_log_path.string().c_str());
        std::ifstream log(g_log_path);
        std::string line;
        while (std::getline(log, line))
            std::printf("    %s\n", line.c_str());
        std::printf("FAILURES: %d\n", failures);
        return 1;
    }
    std::printf("ALL PASS\n");
    return 0;
}
