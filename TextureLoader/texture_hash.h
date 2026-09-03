// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// texture_hash.h - 3DMigoto-compatible texture hash calculation.
//
// Ported faithfully from bo3b/3Dmigoto (GPL-3.0) DirectX11/ResourceHash.cpp
// so that hashes computed at runtime exactly match the `hash = xxxxxxxx`
// values authored in [TextureOverride] sections (e.g. the Texture++ pack).
// GIMI/XXMI configure texture_hash=0, so we reproduce the 3DMigoto v1.2.1
// compatible path (texture_hash_version == 0), including the BC-compressed
// skip-padding branch.
//
// TextureLoader is GPL-3.0-or-later. See LICENSE.GPL.txt and the NOTICE
// for attribution.

#include <d3d11.h>
#include <dxgiformat.h>
#include <stdint.h>
#include <stddef.h>

// Compute the 3DMigoto texture data hash (first subresource only).
// pInitialData must be the array of subresource data passed to CreateTexture2D.
uint32_t CalcTexture2DDataHash(const D3D11_TEXTURE2D_DESC *pDesc,
                               const D3D11_SUBRESOURCE_DATA *pInitialData);

// Final resource hash as matched against [TextureOverride] hash= values:
// crc32c(data_hash, desc, sizeof(D3D11_TEXTURE2D_DESC)).
uint32_t CalcTexture2DDescHash(uint32_t data_hash, const D3D11_TEXTURE2D_DESC *pDesc);

// CRC-32C (Castagnoli) - hardware accelerated when available.
uint32_t crc32c_hw(uint32_t crc, const void *data, size_t length);
