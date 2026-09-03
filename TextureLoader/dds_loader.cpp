// SPDX-License-Identifier: GPL-3.0-or-later
// dds_loader.cpp — CPU 路径 DDS/DX10 纹理加载器（自研实现，不链接 DirectXTK）。
#include "dds_loader.h"
#include "log.h"

#include <stdio.h>
#include <malloc.h>
#include <string.h>

#pragma pack(push, 1)

#define DDS_MAGIC 0x20534444 // "DDS "

#define DDSD_CAPS       0x00000001
#define DDSD_HEIGHT     0x00000002
#define DDSD_WIDTH      0x00000004
#define DDSD_PITCH      0x00000008
#define DDSD_PIXELFORMAT 0x00001000
#define DDSD_MIPMAPCOUNT 0x00020000
#define DDSD_LINEARSIZE 0x00080000
#define DDSD_DEPTH      0x00800000

#define DDPF_ALPHAPIXELS 0x00000001
#define DDPF_FOURCC      0x00000004
#define DDPF_RGB         0x00000040
#define DDPF_LUMINANCE   0x00020000

#define DDSCAPS_TEXTURE 0x00001000
#define DDSCAPS2_CUBEMAP 0x00000200
#define DDSCAPS2_VOLUME  0x00200000

#define FOURCC_DX10 0x30315844 // 'DX10'

struct DdsPixelFormat {
    uint32_t size;
    uint32_t flags;
    uint32_t fourCC;
    uint32_t rgbBitCount;
    uint32_t rBitMask;
    uint32_t gBitMask;
    uint32_t bBitMask;
    uint32_t aBitMask;
};

struct DdsHeader {
    uint32_t size;
    uint32_t flags;
    uint32_t height;
    uint32_t width;
    uint32_t pitchOrLinearSize;
    uint32_t depth;
    uint32_t mipMapCount;
    uint32_t reserved1[11];
    DdsPixelFormat ddspf;
    uint32_t caps;
    uint32_t caps2;
    uint32_t caps3;
    uint32_t caps4;
    uint32_t reserved2;
};

struct DdsHeaderDx10 {
    uint32_t dxgiFormat;
    uint32_t resourceDimension;
    uint32_t miscFlag;
    uint32_t arraySize;
    uint32_t miscFlags2;
};

#pragma pack(pop)

// ---------------------------------------------------------------------------
// Format helpers
// ---------------------------------------------------------------------------

static bool DxgiFormatIsBc(DXGI_FORMAT f)
{
    return (f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM) ||
           (f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB);
}

static bool DxgiFormatIsPacked(DXGI_FORMAT f) // 3-byte or 4-byte packed
{
    return f == DXGI_FORMAT_R8G8_B8G8_UNORM || f == DXGI_FORMAT_G8R8_G8B8_UNORM ||
           f == DXGI_FORMAT_YUY2 || f == DXGI_FORMAT_NV12 || f == DXGI_FORMAT_420_OPAQUE ||
           f == DXGI_FORMAT_AI44 || f == DXGI_FORMAT_IA44 || f == DXGI_FORMAT_P8 || f == DXGI_FORMAT_A8P8;
}

static bool DxgiFormatIsPalettized(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_AI44 || f == DXGI_FORMAT_IA44 || f == DXGI_FORMAT_P8 || f == DXGI_FORMAT_A8P8;
}

// Returns bytes-per-pixel for formats that have a fixed bits-per-pixel, else 0.
static UINT DxgiFormatBpp(DXGI_FORMAT f)
{
    switch (f) {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT:
            return 16;
        case DXGI_FORMAT_R32G32B32_TYPELESS:
        case DXGI_FORMAT_R32G32B32_FLOAT:
        case DXGI_FORMAT_R32G32B32_UINT:
        case DXGI_FORMAT_R32G32B32_SINT:
            return 12;
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
            return 8;
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
            return 4;
        case DXGI_FORMAT_R8G8_B8G8_UNORM:
        case DXGI_FORMAT_G8R8_G8B8_UNORM:
            return 4;
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
            return 2;
        case DXGI_FORMAT_R8_TYPELESS:
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_R8_SNORM:
        case DXGI_FORMAT_R8_SINT:
        case DXGI_FORMAT_A8_UNORM:
            return 1;
        default:
            return 0;
    }
}

// Compute the number of rows / row-size in bytes for a BC or uncompressed format.
static void ComputePitch(DXGI_FORMAT fmt, UINT width, UINT height,
                         UINT *rowBytes, UINT *numRows)
{
    if (DxgiFormatIsBc(fmt)) {
        UINT blockW = (fmt >= DXGI_FORMAT_BC1_TYPELESS && fmt <= DXGI_FORMAT_BC1_UNORM_SRGB) ? 8 : 4;
        UINT blockH = blockW;
        UINT blockBytes = 8;
        switch (fmt) {
            case DXGI_FORMAT_BC1_TYPELESS:
            case DXGI_FORMAT_BC1_UNORM:
            case DXGI_FORMAT_BC1_UNORM_SRGB:
                blockBytes = 8;
                break;
            case DXGI_FORMAT_BC2_TYPELESS:
            case DXGI_FORMAT_BC2_UNORM:
            case DXGI_FORMAT_BC2_UNORM_SRGB:
            case DXGI_FORMAT_BC3_TYPELESS:
            case DXGI_FORMAT_BC3_UNORM:
            case DXGI_FORMAT_BC3_UNORM_SRGB:
            case DXGI_FORMAT_BC4_TYPELESS:
            case DXGI_FORMAT_BC4_UNORM:
            case DXGI_FORMAT_BC4_SNORM:
            case DXGI_FORMAT_BC5_TYPELESS:
            case DXGI_FORMAT_BC5_UNORM:
            case DXGI_FORMAT_BC5_SNORM:
                blockBytes = 16;
                break;
            case DXGI_FORMAT_BC6H_TYPELESS:
            case DXGI_FORMAT_BC6H_UF16:
            case DXGI_FORMAT_BC6H_SF16:
            case DXGI_FORMAT_BC7_TYPELESS:
            case DXGI_FORMAT_BC7_UNORM:
            case DXGI_FORMAT_BC7_UNORM_SRGB:
                blockBytes = 16;
                break;
            default:
                break;
        }
        UINT wBlocks = (width + blockW - 1) / blockW;
        UINT hBlocks = (height + blockH - 1) / blockH;
        *rowBytes = wBlocks * blockBytes;
        *numRows = hBlocks;
        return;
    }
    UINT bpp = DxgiFormatBpp(fmt);
    if (bpp == 0 || DxgiFormatIsPacked(fmt) || DxgiFormatIsPalettized(fmt)) {
        // Unsupported for loading (rare in texture mods); fall back to linear
        *rowBytes = width * 4;
        *numRows = height;
        return;
    }
    *rowBytes = width * bpp;
    *numRows = height;
}

// ---------------------------------------------------------------------------
// Loader
// ---------------------------------------------------------------------------

HRESULT LoadDdsTexture(ID3D11Device *device, const wchar_t *path, DdsLoadResult *out)
{
    if (!device || !path || !out)
        return E_INVALIDARG;

    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f)
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fileSize < 4 + (long)sizeof(DdsHeader)) {
        fclose(f);
        return E_FAIL;
    }

    uint8_t *buf = (uint8_t *)_malloca((size_t)fileSize);
    if (!buf) {
        fclose(f);
        return E_OUTOFMEMORY;
    }
    if (fread(buf, 1, (size_t)fileSize, f) != (size_t)fileSize) {
        fclose(f);
        _freea(buf);
        return E_FAIL;
    }
    fclose(f);

    const uint32_t magic = *(const uint32_t *)buf;
    if (magic != DDS_MAGIC) {
        _freea(buf);
        return E_FAIL;
    }

    const DdsHeader *hdr = (const DdsHeader *)(buf + 4);
    if (hdr->size != sizeof(DdsHeader)) {
        _freea(buf);
        return E_FAIL;
    }

    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT width = hdr->width;
    UINT height = hdr->height;
    UINT mips = (hdr->flags & DDSD_MIPMAPCOUNT) ? hdr->mipMapCount : 1;
    if (mips == 0)
        mips = 1;
    UINT arraySize = 1;
    UINT depth = (hdr->flags & DDSD_DEPTH) ? hdr->depth : 1;

    // 可调上限：max_texture_side>0 时跳过超大纹理（保留原版，避免显存压力）
    if (::tloader::g_max_texture_side > 0 &&
        ((int)width > ::tloader::g_max_texture_side ||
         (int)height > ::tloader::g_max_texture_side)) {
        out->skipped = true;
        _freea(buf);
        return E_FAIL; // 非错误：调用方据 skipped 标记按"跳过"处理（不记 [fail]）
    }

    const uint8_t *pixelData = nullptr;
    const uint8_t *dataEnd = buf + fileSize;

    if (hdr->ddspf.size != sizeof(DdsPixelFormat)) {
        _freea(buf);
        return E_FAIL;
    }

    if ((hdr->ddspf.flags & DDPF_FOURCC) && hdr->ddspf.fourCC == FOURCC_DX10) {
        if (fileSize < 4 + (long)sizeof(DdsHeader) + (long)sizeof(DdsHeaderDx10)) {
            _freea(buf);
            return E_FAIL;
        }
        const DdsHeaderDx10 *dx10 = (const DdsHeaderDx10 *)(buf + 4 + sizeof(DdsHeader));
        format = (DXGI_FORMAT)dx10->dxgiFormat;
        arraySize = dx10->arraySize;
        if (arraySize == 0)
            arraySize = 1;
        pixelData = (const uint8_t *)(dx10 + 1);
    } else if (hdr->ddspf.flags & DDPF_FOURCC) {
        // Legacy FOURCC -> DXGI mapping for common BC formats
        switch (hdr->ddspf.fourCC) {
            case 0x31545844: format = DXGI_FORMAT_BC1_UNORM; break; // DXT1
            case 0x32545844: format = DXGI_FORMAT_BC2_UNORM; break; // DXT2
            case 0x33545844: format = DXGI_FORMAT_BC3_UNORM; break; // DXT3
            case 0x35545844: format = DXGI_FORMAT_BC5_UNORM; break; // DXT5
            case 0x55344342: format = DXGI_FORMAT_BC4_UNORM; break; // BC4U
            case 0x55354342: format = DXGI_FORMAT_BC5_UNORM; break; // BC5U
            case 0x44335232: format = DXGI_FORMAT_BC7_UNORM; break; // DX10 'D32R'
            default:
                _freea(buf);
                return E_FAIL;
        }
        pixelData = (const uint8_t *)(hdr + 1);
    } else if ((hdr->ddspf.flags & DDPF_RGB) && hdr->ddspf.rgbBitCount == 32) {
        // Try to identify common 32-bit layouts by masks
        if (hdr->ddspf.rBitMask == 0x00ff0000 && hdr->ddspf.gBitMask == 0x0000ff00 &&
            hdr->ddspf.bBitMask == 0x000000ff && hdr->ddspf.aBitMask == 0xff000000)
            format = DXGI_FORMAT_B8G8R8A8_UNORM;
        else if (hdr->ddspf.rBitMask == 0x000000ff && hdr->ddspf.gBitMask == 0x0000ff00 &&
                 hdr->ddspf.bBitMask == 0x00ff0000 && hdr->ddspf.aBitMask == 0xff000000)
            format = DXGI_FORMAT_R8G8B8A8_UNORM;
        else {
            _freea(buf);
            return E_FAIL;
        }
        pixelData = (const uint8_t *)(hdr + 1);
    } else {
        _freea(buf);
        return E_FAIL;
    }

    if (format == DXGI_FORMAT_UNKNOWN || DxgiFormatIsPalettized(format)) {
        _freea(buf);
        return E_FAIL;
    }

    if (pixelData > dataEnd) {
        _freea(buf);
        return E_FAIL;
    }

    // Build subresource data array
    UINT subresourceCount = mips * arraySize * (depth > 1 ? depth : 1);
    D3D11_SUBRESOURCE_DATA *initData = (D3D11_SUBRESOURCE_DATA *)_malloca(
        sizeof(D3D11_SUBRESOURCE_DATA) * subresourceCount);
    if (!initData) {
        _freea(buf);
        return E_OUTOFMEMORY;
    }

    const uint8_t *cursor = pixelData;
    const uint8_t *limit = dataEnd;
    UINT idx = 0;
    for (UINT a = 0; a < arraySize; ++a) {
        for (UINT m = 0; m < mips; ++m) {
            UINT w = width >> m;
            UINT h = height >> m;
            if (w == 0) w = 1;
            if (h == 0) h = 1;
            UINT rowBytes, numRows;
            ComputePitch(format, w, h, &rowBytes, &numRows);
            size_t sliceBytes = (size_t)rowBytes * numRows;
            if (cursor + sliceBytes > limit) {
                _freea(initData);
                _freea(buf);
                return E_FAIL;
            }
            initData[idx].pSysMem = cursor;
            initData[idx].SysMemPitch = rowBytes;
            initData[idx].SysMemSlicePitch = (UINT)sliceBytes;
            cursor += sliceBytes;
            idx++;
        }
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width;
    td.Height = height;
    td.MipLevels = mips;
    td.ArraySize = arraySize;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = 0;
    td.MiscFlags = 0;

    HRESULT hr = device->CreateTexture2D(&td, initData, &out->texture);
    if (SUCCEEDED(hr)) {
        out->format = format;
        out->width = width;
        out->height = height;
        out->array_size = arraySize;
        out->mip_levels = mips;
    }

    _freea(initData);
    _freea(buf);
    return hr;
}
