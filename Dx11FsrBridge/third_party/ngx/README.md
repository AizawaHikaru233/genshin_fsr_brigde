# NVIDIA NGX SDK headers

Vendored copy of the two NVIDIA NGX (DLSS) headers that the optional D3D11 NGX
backend compiles against. `NOTICE.md` has the provenance, the license situation
and the integrity rules; this file only covers what is here and how the build
uses it.

## Files

| File | Role |
|---|---|
| `nvsdk_ngx_defs.h` | Types, `NVSDK_NGX_Parameter_*` key strings, result codes, feature flags |
| `nvsdk_ngx_params.h` | The `NVSDK_NGX_Parameter` pure-virtual interface |
| `NVIDIA-SDK-LICENSE.txt` | Upstream license text for the two headers |
| `NOTICE.md` | Provenance, pinned commit, SHA-256, license boundaries |

## Why only two headers

`Dx11FsrBridge/NgxBackend.cpp` includes exactly these two. The rest of the SDK
is deliberately absent:

- `nvsdk_ngx.h` declares the entry points with `NVSDK_NGX_API` (`extern "C"`,
  dllimport unless `NVSDK_NGX_EXPORTS` is defined), so including it would make
  MSVC look for `__imp_NVSDK_NGX_D3D11_*` at link time and fail. Every entry
  point is resolved through `GetProcAddress` instead.
- `nvsdk_ngx_helpers*.h` are included by nothing in this repository.

## How the build picks them up

`Dx11FsrBridge/CMakeLists.txt` adds `third_party/ngx` to the include path and
defines `DX11FSRBRIDGE_ENABLE_NGX=1` when `DX11FSRBRIDGE_ENABLE_NGX` is ON (the
default) and `nvsdk_ngx_defs.h` exists. There is no separate include-directory
variable to set.

Turning the backend off explicitly:

```powershell
cmake -S Dx11FsrBridge -B build -DDX11FSRBRIDGE_ENABLE_NGX=OFF
```

## Updating to a newer SDK

1. Find the target commit in <https://github.com/NVIDIA/DLSS> (full 40-character
   SHA — never a branch name).
2. Point `$Commit` in `tools/Fetch-NgxHeaders.ps1` at it.
3. Run `pwsh -File tools/Fetch-NgxHeaders.ps1 -Update` to print the new hashes,
   review them, then update the `$expected` table.
4. Run `pwsh -File tools/Fetch-NgxHeaders.ps1` to install and verify.
5. Refresh `NVIDIA-SDK-LICENSE.txt` if the upstream license text changed, and
   update the table in `NOTICE.md`.
