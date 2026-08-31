# Known Issue — DLSSG swapchain left uncleaned on exit (bridge-side)

**Status: RECORDED — NOT FIXED (deferred)**

## Problem

After exiting the game (Genshin Impact + GenshinFSRBridge + OptiScaler with
DLSSG frame generation), the game process sometimes **stays alive in the
background** (window closed, process not exiting). This is caused by the bridge,
not by OptiScaler.

## Root cause (bridge-side)

`Dx11FsrBridge`'s `DLL_PROCESS_DETACH` (Dx11FsrBridge.cpp L13235-13249) only
performs:

- `il2cpp_callsite::shutdown()`
- log stream flush/close
- OSD window close

It does **NOT**:

- restore the swapchain vtable hooks installed by `install_swapchain_hooks()`
  (Present / ResizeBuffers / SetFullscreenState / GetFullscreenState /
  ResizeTarget / SetColorSpace1 / SetHDRMetadata …)
- clean up the DLSSG DXGI workaround state (`dlssg_dxgi_workaround_active()`,
  the `g_original_*_by_instance` maps, back-buffer resource tracking)

Because the bridge keeps the DLSSG swapchain / its vtable patched and holds
references, OptiScaler/Streamline's own swapchain teardown is blocked, so the
process lingers in the background.

## Evidence

- Same engine family, same OptiScaler: **Honkai: Star Rail (DX11) exits
  cleanly** — no bridge involved, OptiScaler/Streamline clean up DLSSG swapchain
  themselves.
- **Genshin Impact with the bridge** — process lingers after exit.
- Confirmed by the bridge developer (this repo) as bridge-side responsibility.

## Fix direction (deferred)

1. On `DLL_PROCESS_DETACH`, restore every hooked swapchain vtable entry to its
   original pointer before OptiScaler/Streamline tears down the swapchain.
2. Release bridge-held references in the `g_original_*_by_instance` maps and
   back-buffer resource set.
3. Coordinate teardown order so bridge restoration completes before DLSSG
   swapchain release.

## Reproduction conditions

- AMD GPU + Windows HDR enabled
- GenshinFSRBridge 2.1.0 (all components installed)
- OptiScaler nightly 0815 / 0831 with DLSSG (OptiFG + DLSSG or Nukems/FSRFG)
- Exit the game → process may stay in background

---
Recorded 2026-08-31. Not fixed yet.
