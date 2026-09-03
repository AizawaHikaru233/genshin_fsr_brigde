// SPDX-License-Identifier: GPL-3.0-or-later
// texture_hash.cpp
// 3DMigoto-compatible texture hash. Ported faithfully from bo3b/3Dmigoto
// (GPL-3.0) DirectX11/ResourceHash.cpp so runtime hashes match the
// [TextureOverride] hash= values authored by mods (e.g. Texture++).
//
// GIMI/XXMI set texture_hash=0, i.e. texture_hash_version == 0, which selects
// the legacy 3DMigoto v1.2.1-compatible data-hash path with the
// v1.2.11+ per-row (skip-padding) fallback for BC compressed textures.
//
// The GetSurfaceInfo/BitsPerPixel helpers below are the DirectXTK-derived
// copies that ship inside 3DMigoto (MIT). TextureLoader itself is
// GPL-3.0-or-later (see LICENSE.GPL.txt).

#include "texture_hash.h"
#include "crc32c/crc32c.h"

#include <algorithm>

using std::min;
using std::max;

// ---------------------------------------------------------------------------
// DirectXTK-derived helpers (copied from 3DMigoto's embedded copy)
// ---------------------------------------------------------------------------

static inline size_t BitsPerPixel(DXGI_FORMAT fmt) noexcept
{
    switch (fmt) {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT:
            return 128;
        case DXGI_FORMAT_R32G32B32_TYPELESS:
        case DXGI_FORMAT_R32G32B32_FLOAT:
        case DXGI_FORMAT_R32G32B32_UINT:
        case DXGI_FORMAT_R32G32B32_SINT:
            return 96;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_R32G32_TYPELESS:
        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R32G32_UINT:
        case DXGI_FORMAT_R32G32_SINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        case DXGI_FORMAT_Y416:
        case DXGI_FORMAT_Y210:
        case DXGI_FORMAT_Y216:
            return 64;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UINT:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SINT:
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16G16_SNORM:
        case DXGI_FORMAT_R16G16_SINT:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R32_SINT:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
        case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
        case DXGI_FORMAT_R8G8_B8G8_UNORM:
        case DXGI_FORMAT_G8R8_G8B8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_AYUV:
        case DXGI_FORMAT_Y410:
        case DXGI_FORMAT_YUY2:
            return 32;
        case DXGI_FORMAT_P010:
        case DXGI_FORMAT_P016:
            return 24;
        case DXGI_FORMAT_R8G8_TYPELESS:
        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R8G8_UINT:
        case DXGI_FORMAT_R8G8_SNORM:
        case DXGI_FORMAT_R8G8_SINT:
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R16_SNORM:
        case DXGI_FORMAT_R16_SINT:
        case DXGI_FORMAT_B5G6R5_UNORM:
        case DXGI_FORMAT_B5G5R5A1_UNORM:
        case DXGI_FORMAT_A8P8:
        case DXGI_FORMAT_B4G4R4A4_UNORM:
            return 16;
        case DXGI_FORMAT_NV12:
        case DXGI_FORMAT_420_OPAQUE:
        case DXGI_FORMAT_NV11:
            return 12;
        case DXGI_FORMAT_R8_TYPELESS:
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_R8_SNORM:
        case DXGI_FORMAT_R8_SINT:
        case DXGI_FORMAT_A8_UNORM:
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
        case DXGI_FORMAT_AI44:
        case DXGI_FORMAT_IA44:
        case DXGI_FORMAT_P8:
            return 8;
        case DXGI_FORMAT_R1_UNORM:
            return 1;
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
            return 4;
        default:
            return 0;
    }
}

static inline HRESULT GetSurfaceInfo(size_t width, size_t height, DXGI_FORMAT fmt,
                                     size_t *outNumBytes, size_t *outRowBytes,
                                     size_t *outNumRows) noexcept
{
    uint64_t numBytes = 0, rowBytes = 0, numRows = 0;
    bool bc = false, packed = false, planar = false;
    size_t bpe = 0;
    switch (fmt) {
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
            bc = true; bpe = 8; break;
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            bc = true; bpe = 16; break;
        case DXGI_FORMAT_R8G8_B8G8_UNORM:
        case DXGI_FORMAT_G8R8_G8B8_UNORM:
        case DXGI_FORMAT_YUY2:
            packed = true; bpe = 4; break;
        case DXGI_FORMAT_Y210:
        case DXGI_FORMAT_Y216:
            packed = true; bpe = 8; break;
        case DXGI_FORMAT_NV12:
        case DXGI_FORMAT_420_OPAQUE:
            if (height % 2 != 0) return E_INVALIDARG;
            planar = true; bpe = 2; break;
        case DXGI_FORMAT_P010:
        case DXGI_FORMAT_P016:
            if (height % 2 != 0) return E_INVALIDARG;
            planar = true; bpe = 4; break;
        default:
            break;
    }

    if (bc) {
        uint64_t numBlocksWide = (width > 0) ? std::max<uint64_t>(1, (uint64_t(width) + 3u) / 4u) : 0;
        uint64_t numBlocksHigh = (height > 0) ? std::max<uint64_t>(1, (uint64_t(height) + 3u) / 4u) : 0;
        rowBytes = numBlocksWide * bpe;
        numRows = numBlocksHigh;
        numBytes = rowBytes * numBlocksHigh;
    } else if (packed) {
        rowBytes = ((uint64_t(width) + 1u) >> 1) * bpe;
        numRows = uint64_t(height);
        numBytes = rowBytes * height;
    } else if (fmt == DXGI_FORMAT_NV11) {
        rowBytes = ((uint64_t(width) + 3u) >> 2) * 4u;
        numRows = uint64_t(height) * 2u;
        numBytes = rowBytes * numRows;
    } else if (planar) {
        rowBytes = ((uint64_t(width) + 1u) >> 1) * bpe;
        numBytes = (rowBytes * uint64_t(height)) + ((rowBytes * uint64_t(height) + 1u) >> 1);
        numRows = height + ((uint64_t(height) + 1u) >> 1);
    } else {
        size_t bpp = BitsPerPixel(fmt);
        if (!bpp) return E_INVALIDARG;
        rowBytes = (uint64_t(width) * bpp + 7u) / 8u;
        numRows = uint64_t(height);
        numBytes = rowBytes * height;
    }

    if (outNumBytes) *outNumBytes = static_cast<size_t>(numBytes);
    if (outRowBytes) *outRowBytes = static_cast<size_t>(rowBytes);
    if (outNumRows) *outNumRows = static_cast<size_t>(numRows);
    return S_OK;
}

// ---------------------------------------------------------------------------
// 3DMigoto hash internals
// ---------------------------------------------------------------------------

uint32_t crc32c_hw(uint32_t crc, const void *data, size_t length)
{
    return crc32c_append(crc, static_cast<const uint8_t *>(data), length);
}

static UINT CompressedFormatBlockSize(DXGI_FORMAT format)
{
    switch (format) {
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
            return 8;
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return 16;
    }
    return 0;
}

static size_t Texture2DLength(const D3D11_TEXTURE2D_DESC *pDesc,
                              const D3D11_SUBRESOURCE_DATA *pInitialData,
                              UINT level)
{
    UINT block_size, padded_width, padded_height;
    UINT mip_width = max(pDesc->Width >> level, 1u);
    UINT mip_height = max(pDesc->Height >> level, 1u);

    block_size = CompressedFormatBlockSize(pDesc->Format);
    if (!block_size) {
        // Uncompressed texture - use SysMemPitch (includes any padding)
        return pInitialData->SysMemPitch * mip_height;
    }
    padded_width = (mip_width + 3) & ~0x3;
    padded_height = (mip_height + 3) & ~0x3;
    return padded_width * padded_height / 16 * block_size;
}

static uint32_t hash_tex2d_data(uint32_t hash, const void *data, size_t length,
                                const D3D11_TEXTURE2D_DESC *pDesc, bool zero_padding,
                                bool skip_padding, UINT mapped_row_pitch)
{
    size_t row_pitch, slice_pitch, row_count;

    if (!zero_padding && !skip_padding)
        return crc32c_hw(hash, data, length);

    GetSurfaceInfo(pDesc->Width, pDesc->Height, pDesc->Format,
                   &slice_pitch, &row_pitch, &row_count);

    const uint8_t *sptr = static_cast<const uint8_t *>(data);
    size_t msize = min(row_pitch, (size_t)mapped_row_pitch);
    signed padding = (signed)mapped_row_pitch - (signed)row_pitch;
    uint8_t *zeroes = nullptr;
    if (zero_padding && padding > 0) {
        zeroes = new uint8_t[padding];
        memset(zeroes, 0, padding);
    }

    signed remaining = (signed)length;
    for (size_t h = 0; h < row_count && remaining > 0; h++) {
        hash = crc32c_hw(hash, sptr, min(msize, (size_t)remaining));
        sptr += mapped_row_pitch;
        remaining -= (signed)msize;

        if (zeroes && remaining > 0) {
            hash = crc32c_hw(hash, zeroes, min((size_t)padding, (size_t)remaining));
            remaining -= padding;
        }
    }

    delete[] zeroes;
    return hash;
}

uint32_t CalcTexture2DDataHash(const D3D11_TEXTURE2D_DESC *pDesc,
                               const D3D11_SUBRESOURCE_DATA *pInitialData)
{
    uint32_t hash = 0;
    size_t length_v12, length;

    if (!pDesc || !pInitialData || !pInitialData->pSysMem)
        return 0;

    // texture_hash_version == 0 (GIMI default): 3DMigoto v1.2.1-compatible.
    length_v12 = pDesc->Width * pDesc->Height * pDesc->ArraySize;
    length = Texture2DLength(pDesc, &pInitialData[0], 0);

    if (length_v12 <= length) {
        // v1.2.1-compatible: simple full-buffer CRC over the (broken) length
        return hash_tex2d_data(hash, pInitialData[0].pSysMem, length_v12,
                               pDesc, false, false, pInitialData[0].SysMemPitch);
    }

    // v1.2.11+ path: hash first subresource only, skipping row padding
    length = Texture2DLength(pDesc, &pInitialData[0], 0);
    return hash_tex2d_data(hash, pInitialData[0].pSysMem, length,
                           pDesc, false, true, pInitialData[0].SysMemPitch);
}

uint32_t CalcTexture2DDescHash(uint32_t data_hash, const D3D11_TEXTURE2D_DESC *pDesc)
{
    // 3DMigoto adjusts full-screen-resolution textures to constant magic
    // values so hashes are resolution-independent; GIMI does not enable a
    // resolution override, so no adjustment is applied here.
    return crc32c_hw(data_hash, pDesc, sizeof(D3D11_TEXTURE2D_DESC));
}
