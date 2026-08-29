# Genshin FSR Bridge

A graphics plugin for the Genshin Impact Windows DX11 client. It independently hooks the game's native FSR2 calls and forwards them to the AMD FFX12 official SDK for upscaling: FSR4/FSR3/FSR2 are provided directly on supported GPUs without any external plugin such as OptiScaler. OptiScaler can additionally be connected to extend the upscaler types (DLSS, XeSS, FSR4 INT8, etc.).

This repository also includes the `AntiPlayerMosaic/` subproject. It is an independently built plugin for fixing Genshin Impact's mosaic effects and hiding the UID. See the README in that directory for details.

For the Chinese documentation, see [README.md](README.md).

## Support Scope and Risk Notice

- Intended for the Chinese and Global Windows DX11 clients of Genshin Impact.
- Updated for Genshin Impact `7.0`. With the current feature-query hooks, ordinary future game updates are expected not to require another compatibility update.
- This project is not affiliated with, endorsed by, or authorized by `HoYoverse`, `miHoYo`, `Genshin Impact`, or `原神`. All related names and trademarks belong to their respective owners.
- Third-party DLLs, injectors, mods, and graphics plugins may violate game rules and could result in account restrictions or bans. Users must evaluate the risks themselves and accept full responsibility.

## Demo

![FSR4 active](assets/FSR4激活.jpg)

![Upscaling preset selection](assets/超分档位切换.jpg)

## Frame-Generation Branch

The frame-generation feature in the `frame-generation` branch is built against `OptiScaler 0.10.0-pre1`. OptiScaler versions earlier than `0.10.0-pre1` do not support this frame-generation feature.

## Release Packages

Packages are assembled directly by the build script: installer scripts live in `tools/FpsUnlockInstaller/`, package feedback and component metadata live in `assets/FpsUnlockPackage/`, and runtime resources and default configuration live in `SharedResources/`. GitHub release packages do not bundle the NVIDIA DLSS component or ReShade binaries; the installer downloads them from their official upstream sources at install time (DLSS from NVIDIA Streamline, ReShade from reshade.me). Local distributions must complete those components on their own.

The two first-party DLLs are build outputs and are not committed to the repository. To generate the GitHub release package, run the following command on Windows:

```powershell
powershell -ExecutionPolicy Bypass -File .\Build-OnlineInstaller.ps1 -Configuration Release -GithubOnly
```

The output is written to `dist\原神解帧FSR插件包_v*.7z`, while the GitHub release directory contains `dist\github-release\GenshinFSRBridge_v*.zip`. GitHub Actions builds and publishes only this GitHub release package; it does not generate a FuFu package.

## FuFu Launcher Plugin Source and Local Build

`FufuGraphicsPlugin/` contains the FuFu Launcher plugin source, configuration templates, and the marketplace/local-test Lua installer scripts. The repository does not commit a prebuilt FuFu Launcher plugin binary. Before a local build, run `tools/Update-UpstreamComponents.ps1` to refresh the upstream component baseline (OptiScaler is pinned to v0.9.4; DLSS, ReShade, and FPS Unlocker fetch their latest official releases with SHA-256 verification), then run:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\Update-UpstreamComponents.ps1 -WorkspaceRoot .
powershell -ExecutionPolicy Bypass -File .\Build-OnlineInstaller.ps1 -Configuration Release
```

`-FetchUpstream` can be passed to the build script to refresh upstream components before packaging. The FuFu Launcher plugin package is written only to local `dist\FSR-Bridge-Plugin.v*.zip`; it is not included in GitHub Actions or GitHub Releases.

## Features

- Intercepts DX11 device and context activity to obtain the timing of Genshin Impact's FSR2 calls.
- Prepares color, depth, motion-vector, jitter, and history resources for the FFX12 SDK and forwards the upscaling dispatch.
- Auto-matches the FSR series by GPU capability (FSR4/FSR3/FSR2); upscaling works on supported GPUs without external plugins.
- Extends the in-game render-scale menu to `0.2–0.9 + 0.999`; `0.999` replaces the original highest menu slot.
- Writes runtime logs to `Dx11FsrBridge.log` beside the DLL by default for load and hook diagnostics.

## Repository Layout

- Repository root: FSR Bridge source, configuration, and build files.
- `AntiPlayerMosaic/`: anti-aliasing blur removal, UID hiding, and underwater mosaic fix plugin.
- `third_party/`: Bridge build dependencies and their original notices.

## Usage

Download an archive from [Releases](https://github.com/AizawaHikaru233/genshin_fsr_brigde/releases), extract it, and run `一键配置.bat`, then follow the prompts to install. For the English interface, run `GenshinFSRBridgeTools.bat`. You can also switch between Chinese and English at any time from the installer main menu; the selection is saved automatically. GitHub release packages include FPS Unlocker and OptiScaler; the installer downloads the [NVIDIA DLSS upscaling component (`nvngx_dlss.dll`)](https://github.com/NVIDIA-RTX/Streamline/releases) and ReShade from their official upstream sources at install time. Local distributions must complete the required components on their own.
In-game `FSR2` anti-aliasing must be enabled, and the render scale must be below `1`.

`Dx11FsrBridge.dll` independently hooks Genshin Impact's FSR2 calls and forwards them to the AMD FFX12 SDK, providing FSR4/FSR3/FSR2 upscaling directly on supported GPUs without any external plugin. [OptiScaler](https://github.com/optiscaler/OptiScaler) can optionally be connected to extend the upscaler types (DLSS, XeSS, FSR4 INT8, etc.). The package configures component loading in the default order, so manual ordering is normally not required; `AntiPlayerMosaic.dll` is an optional anti-mosaic/UID-hiding plugin.

When OptiScaler and ReShade are used, their runtime configurations are located in their respective component directories. OptiScaler DLL and log paths, as well as ReShade shader, texture, preset, and screenshot paths, use relative paths. This prevents third-party configuration-saving logic from incorrectly transcoding installation paths that contain Chinese characters. Only the game-directory `[INSTALL] BasePath`, which locates the external ReShade directory, must use a dynamically generated absolute path when installed across directories or drives.

## Build

Visual Studio with the Desktop development with C++ workload, the Windows SDK, and CMake 3.20 or newer (Ninja generator) are required. The release configuration requires the repository's `Dx11FsrBridge\third_party` directory, which contains the FFX12 ffx-api headers and the Microsoft Detours build dependency.

First refresh the upstream component baseline (only needed when component versions change; the script pins OptiScaler to v0.9.4 and fetches the latest official DLSS, ReShade, and FPS Unlocker releases):

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\Update-UpstreamComponents.ps1 -WorkspaceRoot .
```

Then build all release packages:

```powershell
powershell -ExecutionPolicy Bypass -File .\Build-OnlineInstaller.ps1 -Configuration Release
```

`-FetchUpstream` combines both steps. The three first-party DLLs (Bridge / AntiPlayerMosaic / FufuGraphicsPlugin) are compiled automatically by the build script with the Ninja generator; no manual cmake invocation is needed.

## Logs and Issue Feedback

Bridge and the anti-mosaic component retain error logs by default (when OptiScaler/ReShade are connected, they also retain their own logs). Each new run overwrites the previous run's logs.
If the game fails to start, FSR cannot be activated, switching upscalers causes a crash, or another issue occurs, do not launch the game again after reproducing it. Provide the following files and information:

1. `payload/Bridge/Dx11FsrBridge.log` (required)
2. `payload/OptiScaler/OptiScaler.log` and `payload/OptiScaler/OptiScaler.ini` (when using OptiScaler)
3. `payload/ReShade/ReShade.log` (for ReShade-related issues)
4. `payload/AntiPlayerMosaic/AntiPlayerMosaic.log` (for anti-mosaic, UID, or underwater mosaic issues)
5. `FSR-Bridge-Plugin.log` from the FuFu plugin directory (when using the FuFu Launcher plugin)
6. GPU model, game version, the stage at which the issue occurs, and the selected upscaling mode

When further diagnostics are needed, temporarily change `LogLevel` under `Log` in `OptiScaler.ini` (when OptiScaler is used) to `1 (Debug)` or `0 (Trace)`. Restore the release setting after diagnostics to avoid additional overhead.
Do not submit game account details, login information, or screenshots containing personal information to a public Issue.

## Third-Party Components

- FFX12 ffx-api headers and Microsoft Detours are build dependencies only; their original licenses and notices are retained.
- OptiScaler is an independent project: <https://github.com/optiscaler/OptiScaler>.
- GitHub release packages do not bundle the NVIDIA DLSS component or ReShade binaries; the installer downloads them from their official upstream sources at install time.
- Local distributions must complete the required components on their own and comply with each component's licensing terms (for example, shipping the GPL full text with source links, respecting ReShade's official "do not share the binaries" policy, and DLSS limited to NVIDIA GPUs).
- This project does not include NVIDIA DLSS or AMD FSR SDK runtime binaries.

## License

This project is licensed under [GPL-3.0-or-later](Dx11FsrBridge/LICENSE). You may use, modify, and redistribute the code; when distributing a modified version, you must provide the corresponding complete source code and license it under GPL-3.0-or-later.
