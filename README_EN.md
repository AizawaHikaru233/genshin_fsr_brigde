# Genshin FSR Bridge

<p align="center">
  <a href="https://github.com/AizawaHikaru233/genshin_fsr_brigde/releases"><img alt="Release" src="https://img.shields.io/github/v/release/AizawaHikaru233/genshin_fsr_brigde?sort=semver&amp;label=release&amp;color=0078D6"></a>
  <a href="Dx11FsrBridge/LICENSE"><img alt="License: GPL-3.0-or-later" src="https://img.shields.io/badge/license-GPL--3.0--or--later-blue"></a>
  <img alt="Platform: Windows DX11" src="https://img.shields.io/badge/platform-Windows%20DX11-6f42c1?logo=windows&amp;logoColor=white">
  <img alt="FSR4 / FSR3 / FSR2" src="https://img.shields.io/badge/FSR-4%20%7C%203%20%7C%202-f97316">
  <img alt="DLSS (NGX backend)" src="https://img.shields.io/badge/DLSS-NGX%20backend-76B900?logo=nvidia&amp;logoColor=white">
  <img alt="AMD FFX12 SDK" src="https://img.shields.io/badge/AMD-FFX12%20SDK-ED1C24?logo=amd&amp;logoColor=white">
  <img alt="Genshin 7.1+" src="https://img.shields.io/badge/Genshin-7.1%2B-0ea5e9">
</p>

<p align="center">
  <a href="README.md">简体中文</a> · <b>English</b>
</p>

<div align="center">
  <img src="assets/readme/hero-en.svg" alt="Genshin native FSR2 calls hooked by Dx11FsrBridge and split into two backends: AMD FFX12 SDK (FSR4/FSR3/FSR2) and NVIDIA NGX (DLSS)" width="100%">
</div>

A graphics plugin for the Genshin Impact Windows DX11 client. It independently hooks the game's native FSR2 calls and forwards them to the AMD FFX12 official SDK for upscaling: FSR4/FSR3/FSR2 are provided directly on supported GPUs without any external plugin such as OptiScaler. On NVIDIA GPUs it can also switch to the built-in NGX backend and output DLSS directly (also without OptiScaler); OptiScaler can additionally be connected to extend the upscaler types (XeSS, FSR4 INT8, etc.).

This repository also includes the `AntiPlayerMosaic/` and `TextureLoader/` subprojects: the former is an independently built plugin for fixing Genshin Impact's mosaic effects and hiding the UID; the latter is a 3DMigoto-compatible texture replacement / Mod loader (DDS replacement plus GDDS DirectStorage GPU decompression). See the README and NOTICE in each directory for details.

For the Chinese documentation, see [README.md](README.md).

## Contents

| Overview | Usage | Development & Support |
| :--- | :--- | :--- |
| [Support Scope and Risk Notice](#support-scope-and-risk-notice)<br>[Demo](#demo)<br>[Features](#features)<br>[GPU Support and NVIDIA Caveats](#gpu-support-and-nvidia-caveats) | [Release Packages](#release-packages)<br>[FuFu Launcher Plugin Source and Local Build](#fufu-launcher-plugin-source-and-local-build)<br>[Usage](#usage)<br>[Logs and Issue Feedback](#logs-and-issue-feedback) | [Repository Layout](#repository-layout)<br>[Build](#build)<br>[Third-Party Components](#third-party-components)<br>[License](#license) |

## Support Scope and Risk Notice

- Intended for the Chinese and Global Windows DX11 clients of Genshin Impact.
- Adapted for and verified against Genshin Impact `7.0` and `7.1`. The bridge locates the game's
  internal FSR2 calls and il2cpp methods through **dynamic signature recognition** instead of
  hard-coded RVA / prologue bytes, so it is **not limited to the versions tested so far**: later
  game updates keep working as long as nothing major changes (the FSR2 call path and the related
  internal structures stay stable), with no update from this project required.
- If an update does change those internal structures, the bridge abandons the hook and falls back
  when signatures no longer match (it will not crash), but **upscaling may stop working** until
  this project catches up. See [GPU Support and NVIDIA Caveats](#gpu-support-and-nvidia-caveats).
- Feature and hardware prerequisites: which FSR tier (FSR4/FSR3/FSR2) is actually available
  depends on the **GPU model and driver version** (for example FSR4 requires RDNA4 and a
  matching driver). Every "on supported GPUs" statement below refers to the range listed in
  that section — please confirm your GPU is covered before proceeding.
- This project is not affiliated with, endorsed by, or authorized by `HoYoverse`, `miHoYo`, `Genshin Impact`, or `原神`. All related names and trademarks belong to their respective owners.

> [!WARNING]
> Third-party DLLs, injectors, mods, and graphics plugins may violate game rules and could result in account restrictions or bans. Users must evaluate the risks themselves and accept full responsibility.

## Demo

<table>
  <tr>
    <td width="50%" align="center" valign="top">
      <a href="assets/FSR4激活.jpg"><img src="assets/FSR4激活.jpg" alt="FSR4 active" width="100%"></a>
      <br>
      <sub><b>FSR4 active</b> · overlay reports <code>FSR 4.1.0</code> (AMD Radeon RX 9070 XT)</sub>
    </td>
    <td width="50%" align="center" valign="top">
      <a href="assets/超分档位切换.jpg"><img src="assets/超分档位切换.jpg" alt="Upscaling preset selection" width="100%"></a>
      <br>
      <sub><b>Render scale extended</b> · the quality menu offers <code>0.2–0.999</code></sub>
    </td>
  </tr>
</table>

<table>
  <tr>
    <td width="50%" align="center" valign="top">
      <a href="assets/readme/installer-home.jpg"><img src="assets/readme/installer-home.jpg" alt="Installer · Home" width="100%"></a>
      <br>
      <sub><b>Installer · Home</b> · game directory, frame-rate cap, super-resolution input (Auto / FFX / NVNGX), and installed content in one screen</sub>
    </td>
    <td width="50%" align="center" valign="top">
      <a href="assets/readme/installer-plugins.jpg"><img src="assets/readme/installer-plugins.jpg" alt="Installer · Plugins" width="100%"></a>
      <br>
      <sub><b>Installer · Plugins</b> · component version switch, Mods directory, frame-rate tool options</sub>
    </td>
  </tr>
</table>

<sub>The first two images are in-game captures (3840×2160), the last two are the installer UI (2564×1604); click to open the originals.</sub>

## Release Packages

Packages are assembled directly by the build script, and the default installer is the graphical installer in `Installer/` (details in `Installer/README.md`). The old PowerShell installer scripts (`tools/FpsUnlockInstaller/`) are no longer part of the release package; the source is kept for now, and `-ScriptInstaller` still produces a legacy-flow package. Package feedback and component metadata live in `assets/FpsUnlockPackage/`, and runtime resources and default configuration live in `SharedResources/`. The package-root `README.md` is this file, and the demo images in `assets/readme/` are shipped with it. GitHub release packages do not bundle the NVIDIA DLSS runtime or ReShade binaries: ReShade is fetched from reshade.me only when it is ticked; on an RTX card the installer fetches `nvngx_dlss.dll` from NVIDIA Streamline when **Dx11FsrBridge** is ticked (super-resolution input Auto or NVNGX) **or** OptiScaler is ticked — the former lands in `payload\NVIDIA\DLSS\` (where the bridge's NGX backend reads it), and with OptiScaler ticked the same file is also copied into the OptiScaler component directory (GTX 16 only gets FSR4 INT8, DLSS is not deployed). Local distributions must complete those components on their own.

To generate the GitHub release package, run the following command on Windows:

```powershell
powershell -ExecutionPolicy Bypass -File .\Build-OnlineInstaller.ps1 -Configuration Release -GithubOnly
```

The output is written to `dist\原神解帧FSR插件包_v*.7z`, while the GitHub release directory contains `dist\github-release\GenshinFSRBridge_v*.7z` (the GitHub package is 7z as well). GitHub Actions builds and publishes only this GitHub release package; it does not generate a FuFu package.

## FuFu Launcher Plugin Source and Local Build

`FufuGraphicsPlugin/` contains the FuFu Launcher plugin source, configuration templates, and the marketplace/local-test Lua installer scripts. The repository does not commit a prebuilt FuFu Launcher plugin binary. Before a local build, run `tools/Update-UpstreamComponents.ps1` to refresh the upstream component baseline (OptiScaler is pinned to v0.9.4; DLSS, ReShade, and FPS Unlocker fetch their latest official releases with SHA-256 verification), then run:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\Update-UpstreamComponents.ps1 -WorkspaceRoot .
powershell -ExecutionPolicy Bypass -File .\Build-OnlineInstaller.ps1 -Configuration Release
```

`-FetchUpstream` can be passed to the build script to refresh upstream components before packaging. The FuFu Launcher plugin package is written only to local `dist\FSR-Bridge-Plugin.v*.zip`; it is not included in GitHub Actions or GitHub Releases.

## Features

The bridge intercepts the game's native FSR2 calls once and then attaches two backends behind them — the AMD FFX12 SDK and NVIDIA NGX (DLSS):

```mermaid
flowchart LR
    G["Genshin DX11 client<br/>native FSR2 calls"] --> B["Dx11FsrBridge.dll<br/>DX11 hook · FFX12 resource bridge"]
    B -->|"Ffx12Backend=native, or auto on a non-RTX GPU"| S["AMD FFX12 SDK<br/>FSR4 / FSR3 / FSR2"]
    B -->|"Ffx12Backend=ngx, or auto on an RTX GPU"| N["NVIDIA NGX<br/>DLSS"]
    S --> P["Game frame<br/>upscaled output"]
    N --> P
```

| Module | Capability | Notes |
| :---: | --- | --- |
| 🎯 | **Native FSR forwarding** | Intercepts DX11 device and context activity to obtain the timing of Genshin Impact's FSR2 calls. |
| 🧩 | **FFX12 resource bridge** | Prepares color, depth, motion-vector, jitter, and history resources for the FFX12 SDK and forwards the upscaling dispatch. |
| ⚡ | **Automatic tier matching** | Auto-matches the FSR series by GPU capability (FSR4/FSR3/FSR2); upscaling works on supported GPUs without external plugins. |
| 🟩 | **NGX backend (DLSS)** | On RTX 20 series and newer the same FSR2 call can be routed to NVIDIA NGX and output DLSS, also without OptiScaler; see "[DLSS on NVIDIA without OptiScaler](#dlss-on-nvidia-without-optiscaler-ngx-backend)" for the GPU test, prerequisites, and how to obtain the runtime. |
| 🎚 | **Extended render scale** | Extends the in-game render-scale menu to `0.2–0.999`. |
| 📝 | **Runtime logs** | Writes runtime logs to `Dx11FsrBridge.log` beside the DLL by default for load and hook diagnostics. |
| 🖼 | **TextureLoader** (texture/Mod loader) | 3DMigoto-compatible `[TextureOverride]` Mod loading (DDS replacement + GDDS DirectStorage GPU decompression), loading from the package `Mods` directory by default. **Currently it supports pure texture mods only — model mods are not supported** (see "[TextureLoader Scope](#textureloader-scope)" below). **On NVIDIA GPUs this component is not recommended** (replaced texture mods will very likely fail to load; it is not completely unusable, and it can still be enabled manually) — see "[GPU Support and NVIDIA Caveats](#gpu-support-and-nvidia-caveats)" below. |

## GPU Support and NVIDIA Caveats

| Component | AMD | NVIDIA | Intel |
| --- | --- | --- | --- |
| FSR Bridge (FSR4/FSR3/FSR2) | ✅ | ✅ | ✅ |
| FSR Bridge built-in DLSS (NGX backend) | ❌ N/A | ✅ RTX 20 series and newer (GTX 16 / MX unsupported) | ❌ N/A |
| OptiScaler (DLSS/XeSS/FSR4 INT8) | ✅ | ✅ | ✅ |
| Anti-Mosaic / Hide UID, ReShade + RenoDX | ✅ | ✅ | ✅ |
| **OptiScaler + ReShade enabled together** | ✅ | ⚠️ **May be unstable** | ✅ |
| **TextureLoader (texture/Mod loader)** | ✅ Supported | ⚠️ **Not recommended (texture mods may fail to load; can be enabled manually)** | ⚠️ Untested |

<sub>✅ Supported · ⚠️ Conditional or not recommended</sub>

### DLSS on NVIDIA without OptiScaler (NGX backend)

The bridge ships a **NVIDIA NGX backend**: on an NVIDIA card it turns the game's native FSR2 calls into **DLSS** directly, **without OptiScaler**. Which backend runs is decided by `Ffx12Backend` in `payload\Bridge\Dx11FsrBridge.ini` (section `[Dx11FsrBridge]`) — the installer's **Super resolution input** row is its graphical entry point:

| Installer option | Value written | Meaning |
| --- | --- | --- |
| **Auto** (default) | `auto` | Picked by GPU model: RTX goes NGX (DLSS), everything else goes FFX (FSR4/FSR3/FSR2) |
| **FFX** | `native` | Force the bundled FFX12 SDK: FSR4/FSR3/FSR2 only |
| **NVNGX** | `ngx` | Force NVIDIA NGX: DLSS only; a non-NVIDIA / non-RTX card fails and falls back to the game's own FSR2 |

> [!NOTE]
> This is the **super-resolution input interface** (which backend the game's upscaling call is wired to), **not** an upscaling-quality or upscaling-type choice.

`auto` is decided in `Dx11FsrBridge/GpuBackendPick.h`: the vendor must be NVIDIA (`0x10DE`); an adapter description containing `GTX` or `MX` is rejected (neither product line has Tensor cores, GTX 16 included); `RTX` is accepted; when an OEM rewrote the description beyond recognition it falls back to the device id and only accepts Ampere / Ada / Blackwell — the Turing range overlaps GTX 16 and cannot be told apart, so it conservatively says no. A wrong guess is harmless: when it cannot actually run, every frame fails open back to the game's native FSR2.

**Three prerequisites for DLSS (all required)**:

1. **An NVIDIA RTX card** with a properly installed driver. `_nvngx.dll` comes from the driver and the bridge locates it through the registry key `HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore` (the source can also be pinned with `NgxProvider`: `nvidia` / `external` / `auto`).
2. **The DLSS runtime `nvngx_dlss.dll` must be present** (neither the repository nor the release package bundles it). Put it in any of the following; the bridge probes them in this order (the test is **the directory really contains `nvngx_dlss.dll`**, not that the directory exists):
   1. `payload\Bridge\` (next to `Dx11FsrBridge.dll`)
   2. `payload\NVIDIA\DLSS\` — **where the installer puts it**
   3. `payload\OptiScaler\Optiscaler\` (the copy OptiScaler ships)

   You can also hard-code directories in `NgxFeatureDirs` in `payload\Bridge\Dx11FsrBridge.ini` (semicolon-separated; setting it disables auto-probing).
3. A **writable** NGX working directory: `payload\Bridge\ngx\` by default, falling back to `%LOCALAPPDATA%\GenshinFsrBridge\ngx\` when it is not writable; the actual path is logged as `ngx_core appdata=…`.

**How to obtain `nvngx_dlss.dll`**: tick **Dx11FsrBridge** in the installer; when the card is an RTX and the super-resolution input is Auto / NVNGX, the installer fetches one from NVIDIA Streamline into `payload\NVIDIA\DLSS\` (with OptiScaler also ticked, the same file is copied into the OptiScaler component directory as well). You can equally download the DLSS runtime yourself (`nvngx_dlss.dll`, e.g. from an NVIDIA Streamline package) and **import** it into any of those directories — a manual import behaves exactly like the automatic download.

**Acceptance criteria** (`payload\Bridge\Dx11FsrBridge.log`, visible at the default log level):

```text
ngx_core backend=ngx auto=ngx vendor=0x10DE gpu="NVIDIA GeForce RTX 4070 SUPER"
ngx_core provider resolved=nvidia_registry module=...\_nvngx.dll
ngx_core init ok=1 available=1 rc=0x1
ngx_core create_feature ok=1 preset=0 rc=0x1
```

The NVIDIA DLSS debug overlay (something like `DLSS V310.9.16 (default) DX11 chain d3d`) should show up in game. If the overlay disappears and the log stops at `create_feature ok=0 rc=…`, NGX did not come up for that session.

**Parameter semantics**: the keys in the common section (`TransparentJitter`, `Ffx12DepthInverted`, `Ffx12JitterMode`, `Ffx12JitterDelay`, `Fsr2MotionVectorsJittered`) apply to both backends; the FFX-only keys (`Ffx12NonLinear`, `Ffx12Hdr`, `Ffx12FovScale`, `Ffx12CameraNear`, …) apply to **FFX only** — NGX reads its own `Ngx*` keys (`NgxHdr`, `NgxRenderPreset`, `NgxCrossKeyReuse`, `NgxLogLevel`, …).

> [!WARNING]
> **Known issue (2.5.0 and earlier)**: with the in-game render scale above `0.7` (the menu's `0.8`–`0.999` slots), NGX fails to create the feature on some machines (`ngx_core create_feature ok=0 rc=0xBAD00010`, i.e. `UnsupportedParameter`) and **does not retry within that game session** — the DLSS debug overlay disappears and upscaling drops back to the game's own FSR2; restart the game. Cause: the bridge mapped the display/render ratio of those slots (≈1.0–1.3) to DLSS's `UltraQuality` level, which only accepts a fixed ratio. Fixed on the main branch (that level is now clamped to `MaxQuality` before the feature is created; render scale `0.9` was verified to go from failing to working); the fix ships with the next release. Until then, staying at `0.7` or below avoids it (`0.7` verified working; lower slots were not tested individually).

### Enabling OptiScaler and ReShade together may be unstable on NVIDIA

> [!WARNING]
> **Using OptiScaler alone, or ReShade alone, is fine; enabling both at the same time may misbehave or be unstable on NVIDIA.**

That depends on how the two interact with NVIDIA drivers and on their load order, not on this project's code — this project only loads them in the default order.

If you run into trouble:

- **Isolate first**: temporarily enable only one of the two and check whether the problem disappears;
- Report it to the [OptiScaler upstream repository](https://github.com/optiscaler/OptiScaler) or this project's [Issues](https://github.com/AizawaHikaru233/genshin_fsr_brigde/issues), attaching `OptiScaler.log` and `ReShade.log` from the game directory;
- If it is still unstable, **keep only one of the two**: FSR Bridge's own FSR4/FSR3/FSR2 and DLSS on NVIDIA depend on neither — enable OptiScaler for XeSS / FSR4 INT8, enable ReShade for HDR.

### TextureLoader is not recommended on NVIDIA (texture Mods may fail to load)

**TextureLoader is not recommended on NVIDIA cards, but it is not blocked: you can still enable it manually.**

**On NVIDIA cards there is a very high chance that replaced texture mods fail to load — it is not completely unusable, but it is not recommended; sometimes it works fine, and no specific condition has been identified.**

<details>
<summary><b>Why can replaced texture Mods fail to load on NVIDIA? (investigation log)</b></summary>

The author does not own an NVIDIA card and could only rely on repeated testing by community members, so the root cause was never located. The following were ruled out: the Mod texture files themselves (all 3139 DDS files verified as valid BC3), format and SRV view handling (every logged field matches the AMD machine exactly, `hr=0`), hash matching and ini overrides, threading (identical thread topology on both machines), cross-machine file differences (byte-identical FNV fingerprints), alpha-channel content, and reuse of the initial-data buffer by subsequent loads (that defect was fixed, yet the NVIDIA output is still wrong). The only factor that cannot be reproduced locally is **the NVIDIA driver's texture creation / upload timing**.

</details>

Every distribution channel therefore behaves as follows:

- **TextureLoader's configuration and installation entries are always visible and can be enabled manually**: the **FuFu Launcher plugin package** and the **GitHub release package** no longer hide or force-disable this component per GPU vendor. On NVIDIA cards it only prints a "not recommended" note when you select/switch it on.
- **On NVIDIA cards no automatic path ever enables it**: on NVIDIA, "install all modules at once" excludes TextureLoader (you must select module 5 by hand to get it); restoring factory settings, restoring/applying configuration templates, and the defaults used when the configuration is missing or corrupt never turn it on either — it stays **off** unless you enabled it yourself, and these paths will not switch off an enablement you made. It is loaded only when you explicitly enable it (manually selecting module 5 during installation, switching it on in the plugin configuration screen, or passing `-EnableTextureLoader` on the command line).
- **NVIDIA users who want this fixed properly** are welcome to fork the repository, fix it, and submit a Pull Request.
- **You can also sponsor an NVIDIA GPU for the author**, who will then attempt to locate and fix the issue.

> [!NOTE]
> AMD users are unaffected: interactive installation still asks whether to enable it (default No), and it can be toggled at any time from the plugin configuration screen. On AMD, "Install all modules" includes it automatically (on NVIDIA cards you must select module 5 by hand).

### TextureLoader Scope

**TextureLoader currently supports only "pure texture" mods — model mods are not supported.**

| Type | Supported | Notes |
| --- | --- | --- |
| **Pure texture replacement** (DDS / GDDS) | ✅ Yes | Matched via 3DMigoto-compatible `hash=`, replaces texture pixel content |
| Model / mesh replacement (mesh, `.buf`, `.ib`) | ❌ Not yet | Requires hooking vertex/index buffers and draw calls; this component does not |
| Skeleton / animation modification | ❌ Not yet | Same as above |
| Material / shader replacement | ❌ Not yet | Requires hooking shader compilation or material constants |
| `[CommandList*]` / `[Present]` draw-stage commands | ❌ Not yet | Draw-command replay is not implemented |

The hashing algorithm is a verbatim port of 3DMigoto, so **pre-generated `hash=` values from mod authors can be reused as-is**; however, the **execution capability is far smaller than 3DMigoto's** — the latter is a complete draw-interception and command-replay framework, whereas this component implements only the single path of "replace pixels when a texture is created."

> [!NOTE]
> If a mod contains both textures and models, **the texture part takes effect and the model part does not** — it will look like "the texture changed but the shape did not". That is not a defect; it is the current scope.
> See [`TextureLoader/README.md`](TextureLoader/README.md#支持范围) for the full description.

## Repository Layout

| Directory | Description |
| --- | --- |
| Repository root | FSR Bridge source, configuration, and build files. |
| `AntiPlayerMosaic/` | Anti-aliasing blur removal, UID hiding, and underwater mosaic fix plugin. |
| `FufuGraphicsPlugin/` | FuFu Launcher bootstrap, configuration files, and install scripts. |
| `TextureLoader/` | 3DMigoto-compatible texture replacement / Mod loader (DDS + GDDS, **pure texture mods only**). |
| `Installer/` | Graphical installer (Tauri 2 + Rust + WebView2): manages the injection chain, component versions, component logs, and self-update of the plugin package. |
| `SharedResources/` | Runtime resources and component archives shipped with the package. The Genshin-specific RenoDX HDR shader add-on and its redistribution authorization record are archived under `SharedResources/ReShade/runtime/reshade-shaders/`. |

## Usage

> [!WARNING]
> This package is **Windows-only**, and there is no Linux compatibility: the graphical installer depends on
> the WebView2 runtime, so it cannot run under Wine / Proton (it closes immediately when launched).
> Linux users should follow **Linux install / Windows manual install** below.

### Windows: graphical installer

1. Download the **latest** package from [Releases](https://github.com/AizawaHikaru233/genshin_fsr_brigde/releases)
   and extract it to a directory **without Chinese characters or illegal characters**
   (`GenshinFSRBridgeInstaller.exe` must sit next to `payload\`).
2. Double-click `GenshinFSRBridgeInstaller.exe`. No administrator rights are needed, and nothing is written to system directories.
3. Click **Change…** to pick the Genshin Impact install directory; the installer also scans the registry and common paths by itself.
4. Tick the **components to enable** and click **Save** — this writes the absolute path of every selected component into the
   injection chain (`DllList`) of `payload\FpsUnlocker\fps_config.json`, so no DLL is ever placed in the game directory.

   - **Replacing the upscaler only** → at least **Dx11FsrBridge** (required, ticked by default)
   - **DLSS needed (NVIDIA)** → **Dx11FsrBridge** alone is enough: keep the super-resolution input on **Auto**
     (or pick **NVNGX**) and the installer fetches `nvngx_dlss.dll` into `payload\NVIDIA\DLSS\`; see
     "[DLSS on NVIDIA without OptiScaler](#dlss-on-nvidia-without-optiscaler-ngx-backend)"
   - **XeSS needed (or a non-NVIDIA card that wants the DLSS interface)** → also tick **OptiScaler**
   - **HDR enhancement needed** → tick **ReShade + RenoDX** (on NVIDIA cards the installer warns about running it together with OptiScaler)

5. Click **Launch game**. The installer starts the frame-rate tool in place under `payload\FpsUnlocker\`, and that tool injects
   and launches the game. Afterwards just use the desktop shortcut (optional during install) or your own launcher.

> [!IMPORTANT]
> **In-game settings (required)**: enable `FSR2` anti-aliasing and set the render scale **below `1`**.

GitHub release packages include FPS Unlocker and OptiScaler; when a component is missing, the Plugins page offers a
**Download** button that fetches it from its official upstream on the spot (ReShade from reshade.me, the rest from this
repository Releases), instead of re-downloading the whole package. Local distributions must complete the required
components on their own.

The old PowerShell installer scripts (`一键配置.bat` / `GenshinFSRBridgeTools.bat`) are no longer shipped; to build the
legacy flow yourself, pass `-ScriptInstaller` to `Build-OnlineInstaller.ps1`.

### Linux install / Windows manual install

1. Download the **latest** package from [Releases](https://github.com/AizawaHikaru233/genshin_fsr_brigde/releases)
   and extract it to a directory **without Chinese characters or illegal characters**.
2. Find a launcher that can **inject DLLs into Genshin Impact under Linux + Wine/Proton**.
   (On Windows, using another launcher with DLL-injection support follows the same procedure.)
3. Install the components:

   - **Replacing the upscaler only** → install at least **Bridge**
   - **DLSS needed (NVIDIA)** → **Bridge** is still all you install, but you **must** supply `nvngx_dlss.dll` yourself
     and put it in `payload\Bridge\`, `payload\NVIDIA\DLSS\`, or `payload\OptiScaler\Optiscaler\` (the release package does not bundle it)
   - **XeSS needed (or a non-NVIDIA card that wants the DLSS interface)** → also install **OptiScaler**

**Injection order**: **Bridge must load before OptiScaler**. Other plugins have no strict requirement.

When injecting OptiScaler manually, either approach is recommended:

- Point **`OptiDllPath`** in the **`[Libraries]`** section of `OptiScaler.ini` at the **directory holding the upscaling
  components** — that is, the directory containing `amd_fidelityfx_upscaler_dx12.dll`. It is **not necessarily** the
  directory containing `OptiScaler.dll`:

  | OptiScaler version | Layout | What to set `OptiDllPath` to |
  |---|---|---|
  | 0.9.x | Flat: components sit next to `OptiScaler.dll` | the directory containing `OptiScaler.dll` |
  | 0.10 nightly | Nested: components live in an `OptiScaler\` subdirectory | that `OptiScaler\` subdirectory |
- Copy the **entire file set** into the game's main-executable directory the way OptiScaler officially recommends, then specify the DLL to inject.

> [!IMPORTANT]
> Note that `OptiScaler.dll` and `OptiScaler.ini` sit at the **root level in both layouts** (OptiScaler reads its
> ini from its own directory), so do **not** simply use the ini's directory as `OptiDllPath`. Getting it wrong makes
> OptiScaler inject successfully yet find no upscaling backend at all ("takes over but cannot enable"), and it may
> also hang in DETACH on exit, leaving the process behind.

The main executable differs by release:

| Release | Main executable |
|---|---|
| China (CN) | `YuanShen.exe` |
| Global | `GenshinImpact.exe` |

### Components and paths

`Dx11FsrBridge.dll` independently hooks Genshin Impact's FSR2 calls and forwards them to the AMD FFX12 SDK, providing
FSR4/FSR3/FSR2 upscaling directly on supported GPUs **without any external plugin**; on an RTX card it can also switch
to the built-in NGX backend and output **DLSS** directly (also without OptiScaler, but `nvngx_dlss.dll` is required —
see above).
[OptiScaler](https://github.com/optiscaler/OptiScaler) can optionally be connected to extend the upscaler types
(XeSS, FSR4 INT8, etc.; on a non-NVIDIA card it can also provide the DLSS interface). The package configures component loading in the default order, so manual ordering is
normally not required; `AntiPlayerMosaic.dll` is an optional anti-mosaic / UID-hiding plugin.

When OptiScaler and ReShade are used, their runtime configurations are located in their respective component
directories. OptiScaler DLL and log paths, as well as ReShade shader, texture, preset, and screenshot paths, use
**relative paths**. This prevents third-party configuration-saving logic from incorrectly transcoding installation
paths that contain Chinese characters. Only the game-directory `[INSTALL] BasePath`, which locates the external
ReShade directory, must use a dynamically generated absolute path when installed across directories or drives.

## Build

Visual Studio with the Desktop development with C++ workload, the Windows SDK, and CMake 3.20 or newer (Ninja generator) are required. The release configuration requires the repository's `Dx11FsrBridge\third_party` directory, which contains the FFX12 ffx-api headers, the Microsoft Detours build dependency, and the NVIDIA NGX headers.

A Rust toolchain (`cargo`) is also required to build the graphical installer: run `Installer\app\build-app.ps1` once to produce `Installer\app\GenshinFSRBridgeInstaller.exe`; the packaging script takes that file directly and fails with a hint when it is missing.

First refresh the upstream component baseline (only needed when component versions change; the script pins OptiScaler to v0.9.4 and fetches the latest official DLSS, ReShade, and FPS Unlocker releases):

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\Update-UpstreamComponents.ps1 -WorkspaceRoot .
```

Local builds must supply the required components themselves.

Then build all release packages:

```powershell
powershell -ExecutionPolicy Bypass -File .\Build-OnlineInstaller.ps1 -Configuration Release
```

`-FetchUpstream` combines both steps. The four first-party DLLs (Bridge / AntiPlayerMosaic / TextureLoader / FufuGraphicsPlugin) are compiled automatically by the build script with the Ninja generator; no manual cmake invocation is needed.

## Logs and Issue Feedback

Bridge and the anti-mosaic component retain error logs by default (when OptiScaler/ReShade are connected, they also retain their own logs). Each new run overwrites the previous run's logs.
If the game fails to start, FSR cannot be activated, switching upscalers causes a crash, or another issue occurs, do not launch the game again after reproducing it. Provide the following files and information:

| # | File / Information | When it applies |
| :---: | --- | --- |
| 1 | `payload/Bridge/Dx11FsrBridge.log` | **Required** |
| 2 | `payload/Bridge/ngx/nvngx.log` and the `nvngx_dlss_*.log` next to it | When using the built-in DLSS (NGX backend); the real directory is the one in the `ngx_core appdata=` log line |
| 3 | `payload/OptiScaler/OptiScaler.log` and `payload/OptiScaler/OptiScaler.ini` | When using OptiScaler |
| 4 | `payload/ReShade/ReShade.log` | For ReShade-related issues |
| 5 | `payload/AntiPlayerMosaic/AntiPlayerMosaic.log` | For anti-mosaic, UID, or underwater mosaic issues |
| 6 | `payload/TextureLoader/TextureLoader.log` | For texture replacement / Mod loading issues |
| 7 | `FSR-Bridge-Plugin.log` from the FuFu plugin directory | When using the FuFu Launcher plugin |
| 8 | GPU model, game version, the stage at which the issue occurs, and the selected upscaling mode | — |

When further diagnostics are needed, temporarily change `LogLevel` under `Log` in `OptiScaler.ini` (when OptiScaler is used) to `1 (Debug)` or `0 (Trace)`. Restore the release setting after diagnostics to avoid additional overhead.

For the built-in DLSS, raise `NgxLogLevel` in `[Dx11FsrBridge]` of `payload\Bridge\Dx11FsrBridge.ini` from the default `1` to `2` (info) or `3` (debug); the default `1` only records the error-level evidence lines (`ngx_core …` and NGX's own return codes).

> [!CAUTION]
> Do not submit game account details, login information, or screenshots containing personal information to a public Issue.

## Third-Party Components

- FFX12 ffx-api headers and Microsoft Detours are build dependencies only; their original licenses and notices are retained.
- The NVIDIA NGX (DLSS) headers are likewise a build dependency only (for the optional DLSS backend); their source, pinned version, and SHA-256 are documented in `Dx11FsrBridge/third_party/ngx/NOTICE.md`. They are licensed under the NVIDIA RTX SDKs License and are **not covered by GPL-3.0-or-later**; the license text ships with that directory and inside the package at `payload/Bridge/NVIDIA-NGX-SDK-LICENSE.txt`.
- OptiScaler is an independent project: <https://github.com/optiscaler/OptiScaler>.
- Local distributions must complete the required components on their own and comply with each component's licensing terms.
- This project does not include NVIDIA DLSS or AMD FSR SDK runtime binaries (`_nvngx.dll` comes from the GPU driver; `nvngx_dlss.dll` is fetched on the user's machine by the installer, or can be downloaded and placed in one of the directories listed above).

## License

This project is licensed under [GPL-3.0-or-later](Dx11FsrBridge/LICENSE). You may use, modify, and redistribute the code; when distributing a modified version, you must provide the corresponding complete source code and license it under GPL-3.0-or-later.

<div align="center">
  <sub>Genshin FSR Bridge · GPL-3.0-or-later · <a href="https://github.com/AizawaHikaru233/genshin_fsr_brigde/releases">Releases</a> · <a href="https://github.com/AizawaHikaru233/genshin_fsr_brigde/issues">Issues</a></sub>
</div>
