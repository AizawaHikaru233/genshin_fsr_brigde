// GpuBackendPickTest.cpp - unit test for GpuBackendPick.h (no GPU, no game, no D3D).
//
// Covers the Ffx12Backend=auto decision (RTX -> ngx, everything else -> native):
//   (1) NVIDIA RTX product names (desktop / laptop / workstation) => true;
//   (2) NVIDIA non-Tensor lines (GTX / MX) => false, even when the id range overlaps Turing;
//   (3) non-NVIDIA vendors => false regardless of the description;
//   (4) name-only fallback by device id: Ampere / Ada / Blackwell => true, Turing => false;
//   (5) case-insensitive matching and empty description behave as documented.
//
// ASCII-only (consistent with the other tests in this repo).
#include "GpuBackendPick.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace
{

int failures = 0;
int checks = 0;
#define CHECK(cond, msg)                                    \
    do                                                      \
    {                                                       \
        ++checks;                                           \
        if (!(cond))                                        \
        {                                                   \
            std::printf("FAIL: %s\n", msg);                 \
            ++failures;                                     \
        }                                                   \
        else                                                \
        {                                                   \
            std::printf("ok:   %s\n", msg);                 \
        }                                                   \
    } while (0)

constexpr std::uint32_t kNvidia = 0x10DEu;
constexpr std::uint32_t kAmd = 0x1002u;
constexpr std::uint32_t kIntel = 0x8086u;

void run_nvidia_rtx_by_name()
{
    CHECK(gpu_pick::supports_dlss(kNvidia, 0x2705u, L"NVIDIA GeForce RTX 4070"),
          "RTX 4070 (Ada, RTX in name) -> true");
    CHECK(gpu_pick::supports_dlss(kNvidia, 0x2204u, L"NVIDIA GeForce RTX 3090"),
          "RTX 3090 (Ampere, RTX in name) -> true");
    CHECK(gpu_pick::supports_dlss(kNvidia, 0x2504u, L"NVIDIA GeForce RTX 4060 Laptop GPU"),
          "RTX 4060 Laptop (RTX in name) -> true");
    CHECK(gpu_pick::supports_dlss(kNvidia, 0x24B0u, L"NVIDIA RTX A4000"),
          "RTX A4000 workstation (RTX in name) -> true");
    CHECK(gpu_pick::supports_dlss(kNvidia, 0x2684u, L"NVIDIA GeForce RTX 4090"),
          "RTX 4090 (RTX in name) -> true");
}

void run_nvidia_non_tensor()
{
    CHECK(!gpu_pick::supports_dlss(kNvidia, 0x1F82u, L"NVIDIA GeForce GTX 1650"),
          "GTX 1650 (Turing without Tensor) -> false");
    CHECK(!gpu_pick::supports_dlss(kNvidia, 0x1F82u, L"NVIDIA GeForce GTX 1650"),
          "GTX id 0x1F82 is also rejected without a name keyword");
    CHECK(!gpu_pick::supports_dlss(kNvidia, 0x1C94u, L"NVIDIA GeForce MX450"),
          "MX450 (MX line, no Tensor) -> false");
    CHECK(!gpu_pick::supports_dlss(kNvidia, 0x1F82u, L"NVIDIA Graphics Device"),
          "Turing id with a vendor-neutral name -> false (Turing is not a fallback)");
}

void run_other_vendors()
{
    CHECK(!gpu_pick::supports_dlss(kAmd, 0x7550u, L"AMD Radeon RX 9070 XT"),
          "RX 9070 XT (AMD) -> false");
    CHECK(!gpu_pick::supports_dlss(kAmd, 0x73FFu, L"AMD Radeon RX 7900 XTX"),
          "RX 7900 XTX (AMD) -> false");
    CHECK(!gpu_pick::supports_dlss(kIntel, 0x56A0u, L"Intel(R) Arc(TM) A770 Graphics"),
          "Arc A770 (Intel) -> false");
    CHECK(!gpu_pick::supports_dlss(0x1414u, 0x008Cu, L"Microsoft Basic Render Driver"),
          "WARP (Microsoft) -> false");
    // A non-NVIDIA card whose description happens to contain RTX must still be refused.
    CHECK(!gpu_pick::supports_dlss(kAmd, 0x7550u, L"AMD Fake RTX Card"),
          "vendor check runs before the RTX keyword -> false");
}

void run_id_fallback()
{
    CHECK(gpu_pick::supports_dlss(kNvidia, 0x2504u, L"NVIDIA Graphics Device"),
          "Ampere id 0x2504 with no keyword -> true (id fallback)");
    CHECK(gpu_pick::supports_dlss(kNvidia, 0x2705u, L""),
          "Ada id 0x2705 with empty description -> true (id fallback)");
    CHECK(gpu_pick::supports_dlss(kNvidia, 0x2B85u, L"NVIDIA Graphics Device"),
          "Blackwell id 0x2B85 -> true (id fallback)");
    CHECK(!gpu_pick::supports_dlss(kNvidia, 0x1E04u, L"NVIDIA Graphics Device"),
          "Turing id 0x1E04 with no keyword -> false");
    CHECK(!gpu_pick::supports_dlss(kNvidia, 0x1B80u, L"NVIDIA Graphics Device"),
          "Pascal id 0x1B80 with no keyword -> false");
}

void run_case_and_edge_cases()
{
    CHECK(gpu_pick::supports_dlss(kNvidia, 0x2705u, L"nvidia geforce rtx 4070"),
          "lowercase description matches case-insensitively -> true");
    CHECK(!gpu_pick::supports_dlss(kNvidia, 0x2705u, L"nvidia geforce gtx 1070"),
          "lowercase GTX matches case-insensitively -> false");
    CHECK(gpu_pick::supports_dlss(kNvidia, 0x2705u, L""),
          "empty description on an Ada id -> true (id fallback, not a crash)");
    CHECK(!gpu_pick::supports_dlss(0u, 0u, L""),
          "vendor 0 (detection failed) -> false");
}

} // namespace

int main()
{
    std::printf("GpuBackendPickTest\n");
    run_nvidia_rtx_by_name();
    run_nvidia_non_tensor();
    run_other_vendors();
    run_id_fallback();
    run_case_and_edge_cases();
    if (failures == 0)
        std::printf("ALL PASS (%d checks)\n", checks);
    else
        std::printf("%d FAILURE(S) out of %d checks\n", failures, checks);
    return failures == 0 ? 0 : 1;
}
