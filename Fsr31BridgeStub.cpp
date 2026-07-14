#include "Fsr31Bridge.h"

Fsr31Bridge::EnsureResult Fsr31Bridge::ensure_context(
    ID3D11Device *,
    std::uint32_t,
    std::uint32_t,
    std::uint32_t,
    std::uint32_t)
{
    return EnsureResult::failed;
}

bool Fsr31Bridge::prepare_inputs(
    ID3D11DeviceContext *,
    ID3D11ShaderResourceView *,
    ID3D11ShaderResourceView *,
    ID3D11ShaderResourceView *)
{
    return false;
}

std::string Fsr31Bridge::last_error() const
{
    return "FSR3.1 experimental backend is not included in this build";
}
