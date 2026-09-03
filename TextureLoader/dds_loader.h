// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// dds_loader.h - Minimal DDS texture loader for the TextureLoader mod DLL.
//
// This is a from-scratch minimal loader that understands the DDS + DX10
// extended header layouts and the compressed/uncompressed DXGI formats used
// by Genshin Impact texture mods (BC1..BC7, R8G8B8A8, R8, etc). It is derived
// from the *format semantics* documented publicly by the DDS spec and
// DirectXTK; it does not link against DirectXTK.
//
// TextureLoader is GPL-3.0-or-later (see LICENSE.GPL.txt). The DDS/DX10
// header layout is a public format specification, not 3DMigoto code.

#include <d3d11.h>
#include <dxgiformat.h>
#include <stdint.h>
#include <wchar.h>

struct DdsLoadResult {
    ID3D11Texture2D *texture = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT width = 0;
    UINT height = 0;
    UINT array_size = 1;
    UINT mip_levels = 1;
    uint64_t gdds_ready_fence = 0; // GDDS 完成 fence（0=非 GDDS）
    bool skipped = false;          // 因 max_texture_side 等配置跳过（非错误）
};

// Loads a .dds file from disk and creates an immutable (DEFAULT usage)
// ID3D11Texture2D. Returns S_OK on success.
HRESULT LoadDdsTexture(ID3D11Device *device, const wchar_t *path, DdsLoadResult *out);

// True if a byte sequence at the start of a file is a DDS magic (0x20534444).
static inline bool IsDdsMagic(const void *ptr)
{
    const unsigned char *p = (const unsigned char *)ptr;
    return p[0] == 'D' && p[1] == 'D' && p[2] == 'S' && p[3] == ' ';
}
