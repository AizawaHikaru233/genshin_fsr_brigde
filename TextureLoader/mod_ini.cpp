// SPDX-License-Identifier: GPL-3.0-or-later
// mod_ini.cpp - Parse 3DMigoto-format mod ini files into hash->dds map.
// Format: [TextureOverrideX] hash=xxxxxxxx  +  [ResourceX] filename=file.dds
// Compatible with the Texture++ pack ini files.

#include "mod_ini.h"

#include <windows.h>
#include <shlwapi.h>
#include <cstdio>
#include <cctype>
#include <algorithm>

#pragma comment(lib, "shlwapi.lib")

namespace {

// Trim whitespace in place.
std::wstring Trim(const std::wstring &s)
{
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) a++;
    while (b > a && iswspace(s[b - 1])) b--;
    return s.substr(a, b - a);
}

// Parse a uint32 hash value (hex) from a string like "af33ae00".
bool ParseHash(const std::wstring &s, uint32_t *out)
{
    if (s.empty())
        return false;
    wchar_t *end = nullptr;
    unsigned long long v = wcstoull(s.c_str(), &end, 16);
    if (end == s.c_str())
        return false;
    *out = (uint32_t)v;
    return true;
}

// Normalize to forward slashes, lowercase.
std::wstring Normalize(const std::wstring &s)
{
    std::wstring r = s;
    for (auto &c : r) {
        if (c == L'\\') c = L'/';
        else c = (wchar_t)towlower(c);
    }
    return r;
}

// Parse a single ini file; entries are added into the map keyed by hash.
void ParseOneFile(const std::wstring &path,
                  std::unordered_map<uint32_t, TextureOverrideEntry> &map)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 64 * 1024 * 1024) {
        CloseHandle(h);
        return;
    }
    std::string data((size_t)sz.QuadPart, '\0');
    DWORD read = 0;
    ReadFile(h, &data[0], (DWORD)data.size(), &read, nullptr);
    CloseHandle(h);
    data.resize(read);

    // Convert to wide. ini files are usually UTF-8 or ASCII.
    int wlen = MultiByteToWideChar(CP_UTF8, 0, data.c_str(), (int)data.size(), nullptr, 0);
    std::wstring wdata;
    if (wlen > 0) {
        wdata.resize(wlen);
        MultiByteToWideChar(CP_UTF8, 0, data.c_str(), (int)data.size(), &wdata[0], wlen);
    } else {
        wdata.assign(data.begin(), data.end());
    }

    wchar_t dir[MAX_PATH];
    wcscpy_s(dir, path.c_str());
    PathRemoveFileSpecW(dir);

    // First pass: collect [ResourceX] filename= sections.
    std::unordered_map<std::wstring, std::wstring> resources; // lowercased section -> filename (raw)
    {
        std::wstring section;
        size_t pos = 0;
        while (pos < wdata.size()) {
            size_t eol = wdata.find(L'\n', pos);
            if (eol == std::wstring::npos) eol = wdata.size();
            std::wstring line = Trim(wdata.substr(pos, eol - pos));
            pos = eol + 1;
            if (line.empty() || line[0] == L';' || line[0] == L'#')
                continue;
            if (line[0] == L'[') {
                size_t end = line.find(L']');
                if (end != std::wstring::npos)
                    section = Trim(line.substr(1, end - 1));
                continue;
            }
            size_t eq = line.find(L'=');
            if (eq == std::wstring::npos)
                continue;
            std::wstring key = Trim(line.substr(0, eq));
            std::wstring val = Trim(line.substr(eq + 1));
            if (section.rfind(L"Resource", 0) == 0 && key == L"filename" && !val.empty()) {
                resources[Normalize(section)] = val;
            }
        }
    }

    // Second pass: [TextureOverrideX] hash= + this=ResourceX
    {
        std::wstring section;
        uint32_t hash = 0;
        bool have_hash = false;
        std::wstring this_res;
        size_t pos = 0;
        while (pos < wdata.size()) {
            size_t eol = wdata.find(L'\n', pos);
            if (eol == std::wstring::npos) eol = wdata.size();
            std::wstring line = Trim(wdata.substr(pos, eol - pos));
            pos = eol + 1;
            if (line.empty() || line[0] == L';' || line[0] == L'#')
                continue;
            if (line[0] == L'[') {
                // Flush previous override section
                if (have_hash && !this_res.empty()) {
                    auto it = resources.find(Normalize(this_res));
                    if (it != resources.end() && !it->second.empty()) {
                        TextureOverrideEntry e;
                        e.hash = hash;
                        e.ini_file = path;
                        e.ini_dir = dir;
                        e.ini_section = section;
                        std::wstring dds = Normalize(it->second);
                        // Resolve relative to ini dir
                        if (dds.find(L'/') != std::wstring::npos || dds.find(L'\\') != std::wstring::npos) {
                            e.dds_path = std::wstring(dir) + L"\\" + it->second;
                        } else {
                            e.dds_path = std::wstring(dir) + L"\\" + it->second;
                        }
                        // Simple dedup: later files win
                        map[hash] = e;
                    }
                }
                size_t end = line.find(L']');
                if (end != std::wstring::npos)
                    section = Trim(line.substr(1, end - 1));
                hash = 0;
                have_hash = false;
                this_res.clear();
                continue;
            }
            size_t eq = line.find(L'=');
            if (eq == std::wstring::npos)
                continue;
            std::wstring key = Trim(line.substr(0, eq));
            std::wstring val = Trim(line.substr(eq + 1));
            if (key == L"hash") {
                have_hash = ParseHash(val, &hash);
            } else if (key == L"this") {
                this_res = val;
            }
        }
        // Flush last
        if (have_hash && !this_res.empty()) {
            auto it = resources.find(Normalize(this_res));
            if (it != resources.end() && !it->second.empty()) {
                TextureOverrideEntry e;
                e.hash = hash;
                e.ini_file = path;
                e.ini_dir = dir;
                e.ini_section = section;
                e.dds_path = std::wstring(dir) + L"\\" + it->second;
                map[hash] = e;
            }
        }
    }
}

// Recursively collect ini files under a directory.
void CollectInis(const std::wstring &dir, std::vector<std::wstring> &out)
{
    std::wstring pattern = dir + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0 &&
                wcscmp(fd.cFileName, L"DISABLED") != 0) {
                std::wstring sub = dir + L"\\" + fd.cFileName;
                if (sub.rfind(L"DISABLED") == std::wstring::npos)
                    CollectInis(sub, out);
            }
        } else {
            std::wstring name = fd.cFileName;
            size_t dot = name.rfind(L'.');
            std::wstring ext = (dot == std::wstring::npos) ? L"" : name.substr(dot);
            if (_wcsicmp(ext.c_str(), L".ini") == 0)
                out.push_back(dir + L"\\" + name);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

} // namespace

size_t LoadModInis(const std::wstring &root_dir,
                   std::unordered_map<uint32_t, TextureOverrideEntry> &out_map)
{
    std::vector<std::wstring> files;
    CollectInis(root_dir, files);
    for (const auto &f : files)
        ParseOneFile(f, out_map);
    return out_map.size();
}

bool FindOverride(const std::unordered_map<uint32_t, TextureOverrideEntry> &map,
                  uint32_t hash, const TextureOverrideEntry **out)
{
    auto it = map.find(hash);
    if (it == map.end())
        return false;
    if (out)
        *out = &it->second;
    return true;
}
