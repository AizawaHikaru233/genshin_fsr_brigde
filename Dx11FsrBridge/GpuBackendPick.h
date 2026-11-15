#pragma once

// 后端自动选择（Ffx12Backend=auto）的**纯判定**函数。
//
// 为什么单独成头文件（而不是塞进 Dx11FsrBridge.cpp）：
//   1) 判据要能被单元测试直接调用（GpuBackendPickTest.cpp）——写进匿名命名空间的 .cpp 里就测不到；
//   2) 必须零依赖（不碰 D3D / 日志 / 配置）：测试 target 才能不拖任何库；
//   3) 判据是**预判**，不是可用性判定：ngx 真正能否跑仍由 ngx::ensure_init() 判（注册表/驱动/
//      导出面/特征目录）。预判为 true 而运行时失败时，dispatch 逐帧 fail-open 交回游戏原生路径，
//      所以这里判错只会退化成一条更慢的路，不会崩。

#include <cstdint>
#include <cwctype>
#include <string>

namespace gpu_pick
{

// 大小写不敏感的宽字符子串匹配（语义与 Dx11FsrBridge.cpp 的 contains_ci 相同，但独立命名空间，
// 避免与那个 static 函数互相干扰）。
inline bool contains_ci(const std::wstring &hay, const wchar_t *needle)
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

// 这块适配器是否**值得尝试** DLSS/NGX（= 有 Tensor 核心的 RTX 产品）。
//
// 判据顺序（先排除、再放行、最后按 id 兜底）：
//   ① vendor 必须是 NVIDIA（0x10DE）。判 id 不判名字——OEM / 本地化系统的适配器描述可能被改写。
//   ② 名字含 GTX / MX ⇒ false：这两条产品线没有 Tensor 核心（GTX 16 系 Turing 也没有），
//      DLSS 在其上不可能工作。放在 RTX 之前判，避免 OEM 拼接名里同时出现两个词时被误放行。
//   ③ 名字含 RTX ⇒ true（RTX 20/30/40/50 系与 RTX A 系列工作站卡都有 Tensor 核心）。
//   ④ 名字里认不出关键字（本地化 / 驱动只报 "NVIDIA Graphics Device"）时按 device id 兜底，
//      只认 Ampere / Ada / Blackwell 三段。**Turing 段（0x1E00-0x1FFF、0x2180-0x21FF）不兜底**：
//      它与 GTX 16 系同段，无法区分有无 Tensor 核心 ⇒ 保守判 native（少一条 ngx 路径，好过误判）。
inline bool supports_dlss(std::uint32_t vendor_id, std::uint32_t device_id, const std::wstring &desc)
{
    if (vendor_id != 0x10DEu)
        return false;
    if (contains_ci(desc, L"GTX") || contains_ci(desc, L"MX"))
        return false;
    if (contains_ci(desc, L"RTX"))
        return true;
    if (device_id >= 0x2200u && device_id <= 0x25FFu) // Ampere（GA10x，含移动版）
        return true;
    if (device_id >= 0x2600u && device_id <= 0x28FFu) // Ada（AD10x）
        return true;
    if (device_id >= 0x2B00u && device_id <= 0x2FFFu) // Blackwell（GB20x）
        return true;
    return false;
}

} // namespace gpu_pick
