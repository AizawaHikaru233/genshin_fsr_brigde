// EnumAdapters.cpp - enumerate DXGI adapters exactly as the bridge sees them
// (CreateDXGIFactory1 -> EnumAdapters1 -> GetDesc1) and classify each with the
// same name-based rules as Dx11FsrBridge.cpp. ASCII-only source.
#include <Windows.h>
#include <dxgi1_4.h>
#include <cstdio>
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
        if (contains_ci(desc, L"610M") || contains_ci(desc, L"660M") || contains_ci(desc, L"680M"))
            return GpuArch::Rdna2;
        if (contains_ci(desc, L"Graphics"))
        {
            if (device == 0x13C0u || device == 0x1506u || device == 0x163Fu ||
                device == 0x164Du || device == 0x164Eu || device == 0x1681u)
                return GpuArch::Rdna2;
            if (device == 0x15BFu || device == 0x15C8u || device == 0x164Fu ||
                device == 0x1900u || device == 0x1901u)
                return GpuArch::Rdna3Igpu;
            if (device == 0x150Eu || device == 0x1586u || device == 0x1114u || device == 0x1902u)
                return GpuArch::Rdna3Igpu;
            return GpuArch::Rdna2;
        }
        if (device == 0x73F0u)
            return GpuArch::Rdna3;
        if (device >= 0x7500u && device <= 0x75FFu)
            return GpuArch::Rdna4;
        if (device >= 0x7440u && device <= 0x74FFu)
            return GpuArch::Rdna3;
        if ((device >= 0x73A0u && device <= 0x73FFu) || (device >= 0x7420u && device <= 0x743Fu))
            return GpuArch::Rdna2;
        return GpuArch::Other;
    }
    else if (vendor == 0x10DEu) // NVIDIA
    {
        if (contains_ci(desc, L"GTX 16") || contains_ci(desc, L"RTX 2") || contains_ci(desc, L"RTX 3") ||
            contains_ci(desc, L"RTX 4") || contains_ci(desc, L"RTX 5"))
            return GpuArch::Nvidia16_50;
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
        if ((device >= 0x5600u && device <= 0x56FFu) ||
            (device >= 0xE200u && device <= 0xE2FFu))
            return GpuArch::IntelArc;
        return GpuArch::Other;
    }
    return GpuArch::Other;
}

int main()
{
    std::printf("EnumAdapters start\n");
    IDXGIFactory1 *factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    std::printf("CreateDXGIFactory1 hr=0x%08X\n", static_cast<unsigned>(hr));
    if (FAILED(hr) || factory == nullptr)
        return 1;

    for (UINT i = 0;; ++i)
    {
        IDXGIAdapter1 *adapter = nullptr;
        hr = factory->EnumAdapters1(i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND)
            break;
        if (FAILED(hr))
        {
            std::printf("adapter[%u] enum hr=0x%08X\n", i, static_cast<unsigned>(hr));
            break;
        }
        DXGI_ADAPTER_DESC1 desc {};
        if (SUCCEEDED(adapter->GetDesc1(&desc)))
        {
            const GpuArch arch = classify_gpu_arch(desc.VendorId, desc.DeviceId, desc.Description);
            const bool use_402c = arch == GpuArch::Rdna2 || arch == GpuArch::Nvidia16_50 ||
                                  arch == GpuArch::IntelArc;
            std::printf("adapter[%u] vendor=0x%04X device=0x%04X luid=%08X%08X flags=0x%08X\n",
                        i, desc.VendorId, desc.DeviceId,
                        static_cast<unsigned>(desc.AdapterLuid.HighPart),
                        static_cast<unsigned>(desc.AdapterLuid.LowPart),
                        static_cast<unsigned>(desc.Flags));
            std::wprintf(L"  description: %ls\n", desc.Description);
            std::printf("  classify -> arch=%s route=%s\n",
                        arch_name(arch), use_402c ? "402c provider" : "default SDK");
        }
        adapter->Release();
    }
    factory->Release();
    std::printf("EnumAdapters done\n");
    return 0;
}
