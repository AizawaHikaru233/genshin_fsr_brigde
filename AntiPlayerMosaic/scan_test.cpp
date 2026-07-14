#include <Windows.h>

#include "PatternScanner.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2)
    {
        std::puts("usage: scan_test.exe <game.exe> [game.exe ...]");
        return 2;
    }

    bool all_unique = true;
    for (int argument = 1; argument < argc; ++argument)
    {
        std::ifstream input(argv[argument], std::ios::binary);
        if (!input)
        {
            std::printf("open failed: %ls\n", argv[argument]);
            all_unique = false;
            continue;
        }
        std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(input)), {});
        if (data.size() < sizeof(IMAGE_DOS_HEADER))
        {
            std::printf("invalid PE: %ls\n", argv[argument]);
            all_unique = false;
            continue;
        }

        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(data.data());
        const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(data.data() + dos->e_lfanew);
        const auto *sections = IMAGE_FIRST_SECTION(nt);
        std::printf("\n%ls\n", argv[argument]);

        for (const auto &signature : pattern_scanner::k_signatures)
        {
            const auto pattern = pattern_scanner::parse_pattern(signature.text);
            std::vector<std::uint32_t> hits;
            for (unsigned section_index = 0; section_index < nt->FileHeader.NumberOfSections; ++section_index)
            {
                const auto &section = sections[section_index];
                if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 || section.SizeOfRawData < pattern.bytes.size())
                    continue;
                const auto *section_data = data.data() + section.PointerToRawData;
                for (const auto offset : pattern_scanner::find_matches(section_data, section.SizeOfRawData, pattern))
                    hits.push_back(section.VirtualAddress + static_cast<std::uint32_t>(offset));
            }

            std::printf("%-20.*s bytes=%3zu hits=%zu", static_cast<int>(signature.name.size()), signature.name.data(), pattern.bytes.size(), hits.size());
            for (const auto rva : hits)
                std::printf(" rva=0x%08X", rva);
            std::puts("");
            all_unique &= hits.size() == 1;
        }
    }

    return all_unique ? 0 : 1;
}
