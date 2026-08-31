# OptiScaler Frame Generation Regression Report

**Nightly 0815 works — Nightly 0831 frame generation does not.**

## Summary

Two OptiScaler nightly builds were tested in the same environment (same game,
same configuration, same system). Frame generation (FG) via **DLSSG (Nukems
replacement, Streamline)** works correctly with the **0815** nightly but fails
with the **0831** nightly. The upscaler path (FFX/FSR4) still runs on 0831; only
the frame-generation layer breaks.

**Root symptom:** on 0831, every `Dx11wDx12SC::Present` that reaches the FG
presenter raises a Streamline-side exception (`writeMiniDump`), the FG present
fails with `0x80004004` (`E_ABORT`), and `NVSDK_NGX_D3D12_EvaluateFeature`
(DLSSG evaluate) runs only a handful of times before stopping permanently.

## Environment

| Item | Value |
|---|---|
| Host | Windows 11 (10.0.28000) |
| Game | Genshin Impact (Unity 2017.4.30.0) — D3D11 renderer |
| Bridge | Dx11FsrBridge (D3D11 → D3D12 shared-texture upscaler bridge) |
| OptiScaler (working) | nightly **0815** (`FGWork-0815.log`) |
| OptiScaler (broken) | nightly **0831** (`FGnotWork-0831.log`) |
| FrameGen config | `Enabled=true`, `FGInput=Upscaler`, `FGOutput=DLSSG`, `FGNvngxReplacement=Nukems` |
| Runtime files | `nvngx_dlss.dll`, `nvngx_dlssg.dll`, `sl.dlss_g.dll` (Streamline), `amd_fidelityfx_framegeneration_dx12.dll`, `libxess_fg.dll` — all present and loaded in both runs |

Both logs were captured with identical configuration and identical payload
layout (`D:\FSR\dist\原神解帧FSR插件包_v2.1.0\payload\OptiScaler`).

## Key Metrics

| Metric | 0815 (working) | 0831 (broken) |
|---|---|---|
| `NVSDK_NGX_D3D12_EvaluateFeature` (DLSSG) calls | **4302** | **11** |
| `fg Present failed: 80004004` (`E_ABORT`) | **0** | **16** |
| Streamline `Exception detected - writeMiniDump` | **0** | **16** |
| `UpdateFfxApiProvider Upscaling 0x80070057` | 9 (benign) | 9 (same) |
| `Frame count jumped too much` warnings | 6 (benign) | 6 (same) |
| `Dx11wDx12SC::Present` "real FG presenter detected" | yes | yes |

The two benign metrics (`UpdateFfxApiProvider` and `Frame count jumped`) appear
identically in both builds, so they are **not** the cause of the regression.

## Failure Chain (0831)

Reproduced at `01:23:30` (≈14.3 s after start), repeating every frame:

```
01:23:30.502273 [D] Dx11wDx12SC::_CopyDx11BackBufferToShared ... to shadow copy
01:23:30.502427 [T] Dx11wDx12SC::Present real FG presenter detected; skipping wrapper overlay path
01:23:30.502559 [D] FGHooks::hkFGPresent SyncInterval: 1, Flags: 0
01:23:30.502575 [T] FGHooks::FGPresent FG feature exists but is inactive/paused; pass-through present only
01:23:30.502675 [D] LocalPresent 5870
01:23:30.503818 [E] Streamline exception.cpp:75 writeMiniDump - Exception detected - thread 12092
01:23:30.752155 [E] Dx11wDx12SC::Present fg Present failed: 80004004
```

After the first few failures, the DLSSG feature stops receiving evaluate calls
entirely; the FFX upscaler (`ffxDispatch_Dx12`, `FFXFeatureDx12::EvaluateInternal
Dispatch!!`) keeps running, so the game displays upscaled frames but **no
generated frames**.

On 0815 the same present path runs cleanly for the whole session
(`LocalPresent Original present result: 0`, `DLSSG_Dx12::Deactivate` only at
shutdown) with 4302 DLSSG evaluates.

## Excluded Variables

- **Configuration**: identical `FrameGen` settings in both runs.
- **Runtime files**: identical payload folder; all FG DLLs load successfully in
  both logs (`FfxApiProxy::InitFfxDx12_FG LoadResult: true`,
  `XeFGProxy::HookXeFG LoadResult: true`, `Nvngx_Nukems::LoadLibraries Nukem's
  initialized`, `sl.dlss_g` plugin loaded + hooked).
- **Streamline version**: same `Streamline v2.11.1.da40c` in both logs.
- **GPU/driver path warnings** (`unable to find driver path`, `NGX Updater not
  available`): identical in both logs, unrelated to FG execution.
- **Upscaler provider errors** (`UpdateFfxApiProvider 0x80070057`): identical
  counts, benign in 0815.
- **Frame pacing warnings** (`Frame count jumped too much`): identical counts.

## Analysis

The regression is in the **frame-generation present path**, most plausibly the
`Dx11wDx12SC::Present` / Streamline `dlfgPresent` interaction introduced or
changed between the 0815 and 0831 nightlies. The `E_ABORT (0x80004004)` from the
FG present, preceded by a Streamline-side exception that triggers a minidump on
every frame, is characteristic of a present-path state/ownership change (e.g.
swapchain/queue handling, back-buffer shadow copy indexing, or the
"real FG presenter detected" branch) rather than of input configuration.

## Recommendation for OptiScaler Developers

1. Compare `Dx11wDx12SC::Present` / `Dx11wDx12SC::_CopyDx11SharedToDx12FGBackBuffer`
   and the FG-present branch (`real FG presenter detected; skipping wrapper
   overlay path`) between the 0815 and 0831 nightlies.
2. Inspect the minidumps produced by `exception.cpp:75 writeMiniDump` (thread
   12092) for the exact exception code/address in the `dlfgPresent` call stack.
3. If a quick fix is needed meanwhile: the 0815 nightly has no FG regression
   with the same configuration.

## Attached Files

- `optiscaler-fg-logs-0815-0831.zip` — the two raw logs:
  - `FGWork-0815.log` (191,970 lines — FG working)
  - `FGnotWork-0831.log` (92,809 lines — FG broken)

---

Report generated for upstream investigation. Configuration, game and payload
are identical between the two captured sessions.
