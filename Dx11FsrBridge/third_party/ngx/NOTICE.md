# NOTICE — NVIDIA NGX SDK headers

This directory contains two header files from the NVIDIA NGX (DLSS) SDK, stored
verbatim. They are **not** covered by this project's license.

| File | Bytes | SHA-256 |
|---|---|---|
| `nvsdk_ngx_defs.h` | 44313 | `ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e` |
| `nvsdk_ngx_params.h` | 7744 | `943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc` |
| `NVIDIA-SDK-LICENSE.txt` | 27176 | `21b5daec892b12bea692e66bc8fe45cf5902ccaf3a7b831e78050d8859881c37` |

## Source

- Upstream repository: <https://github.com/NVIDIA/DLSS>
- Pinned commit: `a291cc7d2cc642a51566f3dfd5376f635cd1b284` ("DLSS 310.7.0 SDK")
- Upstream paths: `include/nvsdk_ngx_defs.h`, `include/nvsdk_ngx_params.h`, `LICENSE.txt` (repository root)
- NGX API macro in this snapshot: `NVSDK_NGX_VERSION_API_MACRO = 0x0000015` (1.5.0)

## License

The headers carry `SPDX-License-Identifier: LicenseRef-NvidiaProprietary` and are
distributed by NVIDIA under the *NVIDIA RTX SDKs License*. The full text shipped
with this snapshot is in `NVIDIA-SDK-LICENSE.txt`, and the canonical copy lives
at <https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt>.

Copyright and all rights in these files remain with NVIDIA Corporation. This
project claims no ownership over them and adds no terms of its own.

Two points from that license matter for anyone reading or reusing this tree:

- It is **not** compatible with this project's GPL-3.0-or-later. The GPL covers
  the rest of the repository; it does not, and cannot, extend to these headers.
- The license restricts use to systems with NVIDIA GPUs, and constrains
  redistribution of SDK material and of binaries built from it. Compiling and
  shipping a binary that contains code derived from these headers is subject to
  that license — see sections 1.c, 2.c and 4.e of `NVIDIA-SDK-LICENSE.txt`.

## Why they are in the repository

They are a **build dependency only**, for the optional D3D11 NGX backend
(`Dx11FsrBridge/NgxBackend.cpp`, enabled with `DX11FSRBRIDGE_ENABLE_NGX`).
Keeping them here means a fresh checkout builds the same thing the release
package ships, with no network fetch and no silent downgrade to a backend-less
build.

They are declarations only: type definitions, enum values, the
`NVSDK_NGX_Parameter_*` key strings, and the `NVSDK_NGX_Parameter` pure-virtual
interface. No NVIDIA implementation is present. The runtime implementation
(`_nvngx.dll`) comes with the display driver and is reached exclusively through
`GetProcAddress`; nothing from the SDK is linked.

## Integrity

`tools/Fetch-NgxHeaders.ps1` is the single source of truth for the pinned
commit and the two hashes above.

```powershell
# Windows PowerShell 5.1 上验证过（脚本按最弱解释器编写，pwsh 同样可跑）
powershell -NoProfile -File tools\Fetch-NgxHeaders.ps1 -Verify   # check the files on disk, no network
powershell -NoProfile -File tools\Fetch-NgxHeaders.ps1           # re-download from the pinned commit
powershell -NoProfile -File tools\Fetch-NgxHeaders.ps1 -Update   # print hashes for a newer commit
```

These files must stay **byte-identical** to upstream, so `.gitattributes` marks
them `-text`: no end-of-line or encoding conversion is applied on checkout.
Without that, `core.autocrlf` would rewrite the line endings on a fresh clone
and `-Verify` would report a hash mismatch on files nobody touched.

Editing these headers by hand is not supported: `-Verify` fails on any byte
change, and CI runs it before building the NGX configuration.
