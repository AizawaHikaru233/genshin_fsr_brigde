# NVIDIA NGX SDK headers

These headers are **not vendored in this repository**. They are fetched on demand:

```powershell
pwsh -File tools/Fetch-NgxHeaders.ps1
```

That drops them next to this README. The build then picks them up automatically
(see `Dx11FsrBridge/CMakeLists.txt`, `DX11FSRBRIDGE_NGX_INCLUDE_DIR`).

## Why they are not committed

`nvsdk_ngx*.h` carry `SPDX-License-Identifier: LicenseRef-NvidiaProprietary` and
state that reproduction, disclosure, or distribution without an express NVIDIA
license agreement is prohibited. This repository is GPL-3.0-or-later, so the two
cannot be mixed in one tree.

The headers are only *declarations* — types, enum values, and string constants.
None of NVIDIA's implementation is distributed. The runtime
(`C:\Windows\System32\DriverStore\...\_nvngx.dll`) comes with the display driver
and is loaded at runtime via `GetProcAddress`, never linked.

## Provenance

Source: `https://raw.githubusercontent.com/NVIDIA/DLSS/main/include/<name>`

| File | Bytes |
|---|---|
| `nvsdk_ngx.h` | 40289 |
| `nvsdk_ngx_defs.h` | 44313 |
| `nvsdk_ngx_params.h` | 7744 |
| `nvsdk_ngx_helpers.h` | 5317 |
| `nvsdk_ngx_helpers_dlssd.h` | 732 |
| `nvsdk_ngx_helpers_vk.h` | 19720 |

Only `nvsdk_ngx_defs.h` and `nvsdk_ngx_params.h` are needed by the backend.
The rest come along because the fetch is a plain copy of the upstream directory.

## Which files the backend actually includes

- `nvsdk_ngx_defs.h` — types, `NVSDK_NGX_Parameter_*` string keys, result codes
- `nvsdk_ngx_params.h` — the `NVSDK_NGX_Parameter` pure-virtual interface

`nvsdk_ngx.h` is deliberately **not** included: it declares the entry points with
`NVSDK_NGX_API` (`extern "C"`, dllimport when `NVSDK_NGX_EXPORTS` is absent), so
including it would make MSVC look for `__imp_NVSDK_NGX_D3D11_*` at link time.
All entry points are reached through `GetProcAddress` instead.
