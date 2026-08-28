// GpuRouteTest.cpp - simulate classify_gpu_arch with the full GPU id table
// (desktop "GPU id summary: AMD RDNA2-4 + NVIDIA RTX16-50" reference). Inputs
// are exactly what the bridge can obtain at runtime: VendorId + DeviceId + Description.
// ASCII-only source.
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>

enum class GpuArch
{
    Rdna4,
    Rdna3,
    Rdna3Igpu,
    Rdna2,
    Nvidia16_50,
    IntelArc,
    Other
};

static const char *arch_name(GpuArch a)
{
    switch (a)
    {
    case GpuArch::Rdna4: return "rdna4";
    case GpuArch::Rdna3: return "rdna3";
    case GpuArch::Rdna3Igpu: return "rdna3_igpu";
    case GpuArch::Rdna2: return "rdna2";
    case GpuArch::Nvidia16_50: return "nvidia16_50";
    case GpuArch::IntelArc: return "intel_arc";
    default: return "other";
    }
}

static bool contains_ci(const std::wstring &hay, const wchar_t *needle)
{
    const std::size_t n = std::wcslen(needle);
    if (n == 0 || hay.size() < n)
        return false;
    for (std::size_t i = 0; i + n <= hay.size(); ++i)
    {
        bool match = true;
        for (std::size_t j = 0; j < n; ++j)
        {
            if (std::towlower(hay[i + j]) != std::towlower(needle[j]))
            {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

static GpuArch classify_gpu_arch(std::uint32_t vendor, std::uint32_t device, const std::wstring &desc)
{
    // ---------- name-based fuzzy match (preferred) ----------
    if (vendor == 0x1002u) // AMD
    {
        if (contains_ci(desc, L"RX 9") || contains_ci(desc, L"RX9") || contains_ci(desc, L"PRO W9"))
            return GpuArch::Rdna4;
        if (contains_ci(desc, L"RX 7") || contains_ci(desc, L"RX7") || contains_ci(desc, L"PRO W7"))
            return GpuArch::Rdna3;
        if (contains_ci(desc, L"RX 6") || contains_ci(desc, L"RX6"))
            return GpuArch::Rdna2;
        // RDNA3/3.5 iGPU model names -> Rdna3Igpu (official FSR4 is dGPU-only)
        if (contains_ci(desc, L"740M") || contains_ci(desc, L"760M") || contains_ci(desc, L"780M") ||
            contains_ci(desc, L"8040S") || contains_ci(desc, L"8050S") || contains_ci(desc, L"8060S") ||
            contains_ci(desc, L"840M") || contains_ci(desc, L"860M") || contains_ci(desc, L"880M") ||
            contains_ci(desc, L"890M"))
            return GpuArch::Rdna3Igpu;
        // RDNA2 iGPU model names -> Rdna2 (610M/660M/680M)
        if (contains_ci(desc, L"610M") || contains_ci(desc, L"660M") || contains_ci(desc, L"680M"))
            return GpuArch::Rdna2;

        // ---------- iGPU: description usually just "AMD Radeon(TM) Graphics" -> DeviceId fallback ----------
        if (contains_ci(desc, L"Graphics"))
        {
            // RDNA2 iGPU (table 1.2): Van Gogh 163F, Rembrandt 164D/1681, Raphael 164E,
            // Mendocino 1506, Granite Ridge 13C0 (Ryzen 9000 desktop, Radeon 610M)
            if (device == 0x13C0u || device == 0x1506u || device == 0x163Fu ||
                device == 0x164Du || device == 0x164Eu || device == 0x1681u)
                return GpuArch::Rdna2;
            // RDNA3 iGPU (table 1.4): Phoenix 15BF/15C8/164F, Hawk Point 1900/1901
            if (device == 0x15BFu || device == 0x15C8u || device == 0x164Fu ||
                device == 0x1900u || device == 0x1901u)
                return GpuArch::Rdna3Igpu;
            // RDNA3.5 iGPU (table 1.5): Strix 150E, Strix Halo 1586, Krackan 1114/1902
            if (device == 0x150Eu || device == 0x1586u || device == 0x1114u || device == 0x1902u)
                return GpuArch::Rdna3Igpu;
            return GpuArch::Rdna2; // unknown iGPU -> Rdna2 bucket (conservative 402c)
        }

        // ---------- dGPU without model name -> DeviceId fallback ----------
        if (device == 0x73F0u)
            return GpuArch::Rdna3; // Navi 33 (RX 7600M XT) sits inside the RDNA2 id range
        if (device >= 0x7500u && device <= 0x75FFu)
            return GpuArch::Rdna4; // Navi 48/44 (RX 9000: 7550/7551/7590)
        if (device >= 0x7440u && device <= 0x74FFu)
            return GpuArch::Rdna3; // Navi 31/32/33 dGPU (7448-749F, 747E, 7480...)
        if ((device >= 0x73A0u && device <= 0x73FFu) || (device >= 0x7420u && device <= 0x743Fu))
            return GpuArch::Rdna2; // Navi 21/22/23/24 dGPU (73A1-73FF, 7420-743F)
        return GpuArch::Other;
    }
    else if (vendor == 0x10DEu) // NVIDIA: GTX16 + RTX20-50 (lua int8_rules)
    {
        if (contains_ci(desc, L"GTX 16") || contains_ci(desc, L"RTX 2") || contains_ci(desc, L"RTX 3") ||
            contains_ci(desc, L"RTX 4") || contains_ci(desc, L"RTX 5"))
            return GpuArch::Nvidia16_50;
        // DeviceId fallback (table 2): Turing 1E00-1FFF/2180-21FF, Ampere 2200-25FF,
        // Ada 2600-28FF, Blackwell 2B00-2FFF (incl. GB205 2Fxx)
        if ((device >= 0x1E00u && device <= 0x1FFFu) ||
            (device >= 0x2180u && device <= 0x21FFu) ||
            (device >= 0x2200u && device <= 0x25FFu) ||
            (device >= 0x2600u && device <= 0x28FFu) ||
            (device >= 0x2B00u && device <= 0x2FFFu))
            return GpuArch::Nvidia16_50;
        return GpuArch::Other;
    }
    else if (vendor == 0x8086u) // Intel
    {
        if (contains_ci(desc, L"Arc"))
            return GpuArch::IntelArc;
        if ((device >= 0x5600u && device <= 0x56FFu) || // Alchemist dGPU (A380/A750/A770)
            (device >= 0xE200u && device <= 0xE2FFu))   // Battlemage dGPU (B580)
            return GpuArch::IntelArc;
        return GpuArch::Other;
    }
    return GpuArch::Other;
}

struct Case
{
    std::uint32_t vendor;
    std::uint32_t device;
    const wchar_t *desc;
    GpuArch expected;
};

int main()
{
    const Case cases[] = {
        // ===== AMD RDNA4 dGPU (table 1.6) =====
        {0x1002, 0x7550, L"AMD Radeon RX 9070 XT", GpuArch::Rdna4},
        {0x1002, 0x7550, L"AMD Radeon RX 9070", GpuArch::Rdna4},
        {0x1002, 0x7550, L"AMD Radeon RX 9070 GRE", GpuArch::Rdna4},
        {0x1002, 0x7590, L"AMD Radeon RX 9060 XT", GpuArch::Rdna4},
        {0x1002, 0x7590, L"AMD Radeon RX 9050", GpuArch::Rdna4},
        {0x1002, 0x7551, L"Radeon AI PRO R9700", GpuArch::Rdna4}, // id fallback 0x7500-0x75FF
        // ===== AMD RDNA3 dGPU (table 1.3) =====
        {0x1002, 0x744C, L"AMD Radeon RX 7900 XTX", GpuArch::Rdna3},
        {0x1002, 0x744C, L"AMD Radeon RX 7900 XT", GpuArch::Rdna3},
        {0x1002, 0x744C, L"AMD Radeon RX 7900 GRE", GpuArch::Rdna3},
        {0x1002, 0x747E, L"AMD Radeon RX 7800 XT", GpuArch::Rdna3},
        {0x1002, 0x747E, L"AMD Radeon RX 7700 XT", GpuArch::Rdna3},
        {0x1002, 0x7480, L"AMD Radeon RX 7600 XT", GpuArch::Rdna3},
        {0x1002, 0x7480, L"AMD Radeon RX 7600", GpuArch::Rdna3},
        {0x1002, 0x73F0, L"AMD Radeon RX 7600M XT", GpuArch::Rdna3},
        {0x1002, 0x7448, L"AMD Radeon PRO W7900", GpuArch::Rdna3},
        {0x1002, 0x7461, L"AMD Radeon PRO V710", GpuArch::Rdna3}, // id fallback 0x7440-0x74FF
        // ===== AMD RDNA2 dGPU (table 1.1) =====
        {0x1002, 0x73A5, L"AMD Radeon RX 6950 XT", GpuArch::Rdna2},
        {0x1002, 0x73AF, L"AMD Radeon RX 6900 XT", GpuArch::Rdna2},
        {0x1002, 0x73BF, L"AMD Radeon RX 6800 XT", GpuArch::Rdna2},
        {0x1002, 0x73BF, L"AMD Radeon RX 6800", GpuArch::Rdna2},
        {0x1002, 0x73DF, L"AMD Radeon RX 6750 XT", GpuArch::Rdna2},
        {0x1002, 0x73DF, L"AMD Radeon RX 6700 XT", GpuArch::Rdna2},
        {0x1002, 0x73EF, L"AMD Radeon RX 6650 XT", GpuArch::Rdna2},
        {0x1002, 0x73FF, L"AMD Radeon RX 6600 XT", GpuArch::Rdna2},
        {0x1002, 0x73FF, L"AMD Radeon RX 6600", GpuArch::Rdna2},
        {0x1002, 0x743F, L"AMD Radeon RX 6500 XT", GpuArch::Rdna2},
        {0x1002, 0x743F, L"AMD Radeon RX 6400", GpuArch::Rdna2},
        {0x1002, 0x7422, L"AMD Radeon PRO W6400", GpuArch::Rdna2}, // id fallback 0x7420-0x743F
        // ===== AMD iGPU with model names =====
        {0x1002, 0x15BF, L"AMD Radeon(TM) 780M", GpuArch::Rdna3Igpu},
        {0x1002, 0x15BF, L"AMD Radeon(TM) 760M", GpuArch::Rdna3Igpu},
        {0x1002, 0x15C8, L"AMD Radeon(TM) 740M", GpuArch::Rdna3Igpu},
        {0x1002, 0x150E, L"AMD Radeon(TM) 890M", GpuArch::Rdna3Igpu},
        {0x1002, 0x150E, L"AMD Radeon(TM) 880M", GpuArch::Rdna3Igpu},
        {0x1002, 0x1114, L"AMD Radeon(TM) 860M", GpuArch::Rdna3Igpu},
        {0x1002, 0x1114, L"AMD Radeon(TM) 840M", GpuArch::Rdna3Igpu},
        {0x1002, 0x1586, L"AMD Radeon(TM) 8060S", GpuArch::Rdna3Igpu},
        {0x1002, 0x1681, L"AMD Radeon(TM) 680M", GpuArch::Rdna2},
        {0x1002, 0x164D, L"AMD Radeon(TM) 660M", GpuArch::Rdna2},
        {0x1002, 0x13C0, L"AMD Radeon(TM) 610M", GpuArch::Rdna2},
        // ===== AMD iGPU unnamed "AMD Radeon(TM) Graphics" -> DeviceId fallback =====
        {0x1002, 0x13C0, L"AMD Radeon(TM) Graphics", GpuArch::Rdna2}, // Granite Ridge 610M (this machine)
        {0x1002, 0x1506, L"AMD Radeon(TM) Graphics", GpuArch::Rdna2}, // Mendocino 610M
        {0x1002, 0x163F, L"AMD Radeon(TM) Graphics", GpuArch::Rdna2}, // Van Gogh (Steam Deck)
        {0x1002, 0x164D, L"AMD Radeon(TM) Graphics", GpuArch::Rdna2}, // Rembrandt 660M
        {0x1002, 0x164E, L"AMD Radeon(TM) Graphics", GpuArch::Rdna2}, // Raphael 610M
        {0x1002, 0x1681, L"AMD Radeon(TM) Graphics", GpuArch::Rdna2}, // Rembrandt 680M
        {0x1002, 0x15BF, L"AMD Radeon(TM) Graphics", GpuArch::Rdna3Igpu}, // Phoenix 780M
        {0x1002, 0x15C8, L"AMD Radeon(TM) Graphics", GpuArch::Rdna3Igpu}, // Phoenix2 740M
        {0x1002, 0x164F, L"AMD Radeon(TM) Graphics", GpuArch::Rdna3Igpu}, // Phoenix early
        {0x1002, 0x1900, L"AMD Radeon(TM) Graphics", GpuArch::Rdna3Igpu}, // Hawk Point
        {0x1002, 0x1901, L"AMD Radeon(TM) Graphics", GpuArch::Rdna3Igpu}, // Hawk Point2
        {0x1002, 0x150E, L"AMD Radeon(TM) Graphics", GpuArch::Rdna3Igpu}, // Strix Point 880M/890M
        {0x1002, 0x1586, L"AMD Radeon(TM) Graphics", GpuArch::Rdna3Igpu}, // Strix Halo 8050S/8060S
        {0x1002, 0x1114, L"AMD Radeon(TM) Graphics", GpuArch::Rdna3Igpu}, // Krackan 840M/860M
        {0x1002, 0x1902, L"AMD Radeon(TM) Graphics", GpuArch::Rdna3Igpu}, // Krackan2
        {0x1002, 0x9999, L"AMD Radeon(TM) Graphics", GpuArch::Rdna2}, // unknown iGPU -> conservative
        // ===== AMD RDNA1 / unknown dGPU =====
        {0x1002, 0x0000, L"AMD Radeon RX 5700 XT", GpuArch::Other},
        {0x1002, 0x731F, L"AMD Radeon RX 5700 XT", GpuArch::Other}, // RDNA1 id < 0x73A0
        {0x1002, 0x0000, L"AMD Radeon RX 580", GpuArch::Other},
        // ===== NVIDIA 16-50 series (table 2) =====
        {0x10DE, 0x2B85, L"NVIDIA GeForce RTX 5090", GpuArch::Nvidia16_50},
        {0x10DE, 0x2C02, L"NVIDIA GeForce RTX 5080", GpuArch::Nvidia16_50},
        {0x10DE, 0x2C05, L"NVIDIA GeForce RTX 5070 Ti", GpuArch::Nvidia16_50},
        {0x10DE, 0x2F04, L"NVIDIA GeForce RTX 5070", GpuArch::Nvidia16_50},
        {0x10DE, 0x2D05, L"NVIDIA GeForce RTX 5060", GpuArch::Nvidia16_50},
        {0x10DE, 0x2D83, L"NVIDIA GeForce RTX 5050", GpuArch::Nvidia16_50},
        {0x10DE, 0x2684, L"NVIDIA GeForce RTX 4090", GpuArch::Nvidia16_50},
        {0x10DE, 0x2709, L"NVIDIA GeForce RTX 4070", GpuArch::Nvidia16_50},
        {0x10DE, 0x2786, L"NVIDIA GeForce RTX 4070", GpuArch::Nvidia16_50},
        {0x10DE, 0x2808, L"NVIDIA GeForce RTX 4060", GpuArch::Nvidia16_50},
        {0x10DE, 0x2882, L"NVIDIA GeForce RTX 4060", GpuArch::Nvidia16_50},
        {0x10DE, 0x2204, L"NVIDIA GeForce RTX 3090", GpuArch::Nvidia16_50},
        {0x10DE, 0x2206, L"NVIDIA GeForce RTX 3080", GpuArch::Nvidia16_50},
        {0x10DE, 0x2484, L"NVIDIA GeForce RTX 3070", GpuArch::Nvidia16_50},
        {0x10DE, 0x2503, L"NVIDIA GeForce RTX 3060", GpuArch::Nvidia16_50},
        {0x10DE, 0x2582, L"NVIDIA GeForce RTX 3050", GpuArch::Nvidia16_50},
        {0x10DE, 0x1E04, L"NVIDIA GeForce RTX 2080 Ti", GpuArch::Nvidia16_50},
        {0x10DE, 0x1E82, L"NVIDIA GeForce RTX 2080", GpuArch::Nvidia16_50},
        {0x10DE, 0x1F02, L"NVIDIA GeForce RTX 2070", GpuArch::Nvidia16_50},
        {0x10DE, 0x1F06, L"NVIDIA GeForce RTX 2060 SUPER", GpuArch::Nvidia16_50},
        {0x10DE, 0x2184, L"NVIDIA GeForce GTX 1660", GpuArch::Nvidia16_50},
        {0x10DE, 0x21C4, L"NVIDIA GeForce GTX 1660 SUPER", GpuArch::Nvidia16_50},
        {0x10DE, 0x1F82, L"NVIDIA GeForce GTX 1650", GpuArch::Nvidia16_50},
        {0x10DE, 0x1F83, L"NVIDIA GeForce GTX 1630", GpuArch::Nvidia16_50},
        {0x10DE, 0x2520, L"NVIDIA GeForce RTX 3060 Laptop GPU", GpuArch::Nvidia16_50},
        {0x10DE, 0x1E90, L"NVIDIA GeForce RTX 2080 Mobile", GpuArch::Nvidia16_50},
        {0x10DE, 0x2717, L"NVIDIA GeForce RTX 4090 Laptop GPU", GpuArch::Nvidia16_50},
        {0x10DE, 0x2C18, L"NVIDIA GeForce RTX 5090 Laptop GPU", GpuArch::Nvidia16_50},
        {0x10DE, 0x2230, L"NVIDIA RTX A6000", GpuArch::Nvidia16_50}, // id fallback
        {0x10DE, 0x2684, L"NVIDIA GeForce", GpuArch::Nvidia16_50},   // id fallback, no model name
        {0x10DE, 0x1E82, L"NVIDIA GeForce", GpuArch::Nvidia16_50},   // id fallback
        // ===== NVIDIA older series -> not in group =====
        {0x10DE, 0x1B81, L"NVIDIA GeForce GTX 1080 Ti", GpuArch::Other},
        {0x10DE, 0x1C82, L"NVIDIA GeForce GTX 1050 Ti", GpuArch::Other},
        {0x10DE, 0x1380, L"NVIDIA GeForce GTX 750 Ti", GpuArch::Other},
        // ===== Intel Arc (dGPU + iGPU) =====
        {0x8086, 0x56A0, L"Intel(R) Arc(TM) A770 Graphics", GpuArch::IntelArc},
        {0x8086, 0x56A1, L"Intel(R) Arc(TM) A380 Graphics", GpuArch::IntelArc},
        {0x8086, 0xE20B, L"Intel(R) Arc(TM) B580 Graphics", GpuArch::IntelArc},
        {0x8086, 0x7D55, L"Intel(R) Arc(TM) Graphics", GpuArch::IntelArc},
        {0x8086, 0x56A0, L"Intel Graphics", GpuArch::IntelArc}, // id fallback Alchemist
        {0x8086, 0xE20B, L"Intel Graphics", GpuArch::IntelArc}, // id fallback Battlemage
        // ===== Intel non-Arc =====
        {0x8086, 0x3E91, L"Intel(R) UHD Graphics 630", GpuArch::Other},
        {0x8086, 0x9A49, L"Intel(R) Iris(R) Xe Graphics", GpuArch::Other},
        // ===== unknown vendor =====
        {0x1414, 0x008C, L"Microsoft Basic Render Driver", GpuArch::Other},
    };

    int pass = 0, fail = 0;
    for (const Case &c : cases)
    {
        const GpuArch got = classify_gpu_arch(c.vendor, c.device, c.desc);
        const bool ok = got == c.expected;
        std::printf("%s  vendor=0x%04X dev=0x%04X  %-42ls -> %-11s (expect %s)\n",
                    ok ? "PASS" : "FAIL",
                    c.vendor, c.device, c.desc, arch_name(got), arch_name(c.expected));
        if (ok) ++pass; else ++fail;
    }
    std::printf("\n%d passed, %d failed\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
