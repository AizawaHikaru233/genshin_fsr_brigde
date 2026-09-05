// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// dds_loader.h — 统一纹理加载结果类型 + CPU DDS 加载器 + 加载器注册表。
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

// 统一加载结果：DDS（CPU）与 GDDS（DirectStorage GPU 解压）共用。
// 加载成功后 out->texture 非空且已 AddRef（调用方登记持有）。
struct TextureLoadResult {
    ID3D11Texture2D *texture = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT width = 0;
    UINT height = 0;
    UINT array_size = 1;
    UINT mip_levels = 1;
    uint64_t ready_fence = 0; // GDDS 完成 fence（渲染线程绑定前等待；DDS 恒 0）
    bool skipped = false;     // 因 max_texture_side 等配置主动跳过（非错误）
};

// 加载器函数签名。
typedef HRESULT (*TextureLoaderFn)(ID3D11Device *device, const wchar_t *path,
                                   TextureLoadResult *out);

// 加载器注册表项：extension 为小写扩展名（不含点，如 L"gdds"），
// nullptr 表示默认加载器（兜底，处理未显式注册的扩展名）。
struct TextureLoaderEntry {
    const wchar_t *extension; // 小写无点；nullptr = 默认
    TextureLoaderFn load;
};

// 按路径扩展名选择加载器：线性扫注册表（条目数极少，无需哈希）。
// 未匹配到注册项时返回默认项（extension==nullptr）；无默认项返回 nullptr。
const TextureLoaderEntry *SelectTextureLoader(const wchar_t *path,
                                              const TextureLoaderEntry *registry,
                                              size_t count);

// CPU DDS 加载器（实现见 dds_loader.cpp）。
HRESULT LoadDdsTexture(ID3D11Device *device, const wchar_t *path, TextureLoadResult *out);

// True if a byte sequence at the start of a file is a DDS magic (0x20534444).
static inline bool IsDdsMagic(const void *ptr)
{
    const unsigned char *p = (const unsigned char *)ptr;
    return p[0] == 'D' && p[1] == 'D' && p[2] == 'S' && p[3] == ' ';
}
