// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// mod_ini.h - Parser for 3DMigoto-format mod .ini files
// ([TextureOverrideX] hash=...  +  [ResourceX] filename=...).
// Format-compatible with the Texture++ pack ini files and d3dx.ini syntax.

#include <string>
#include <vector>
#include <unordered_map>

// One texture override mapping a runtime texture hash to a replacement dds.
struct TextureOverrideEntry {
    uint32_t hash = 0;
    std::wstring ini_file;     // full path of the ini that defined it
    std::wstring ini_dir;      // directory of the ini (base for relative paths)
    std::wstring dds_path;     // resolved absolute path to the replacement dds
    std::wstring ini_section;  // original [TextureOverride...] name
};

// Scans a directory tree for *.ini, parses [TextureOverride]/[Resource]
// sections and builds a hash -> dds map. Returns number of entries found.
size_t LoadModInis(const std::wstring &root_dir,
                   std::unordered_map<uint32_t, TextureOverrideEntry> &out_map);

// Look up a replacement by texture hash.
bool FindOverride(const std::unordered_map<uint32_t, TextureOverrideEntry> &map,
                  uint32_t hash, const TextureOverrideEntry **out);
