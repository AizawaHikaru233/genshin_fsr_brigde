# Genshin FSR Bridge

An FSR2 ABI bridge DLL for the Genshin Impact Windows DX11 client. It exposes standard FSR2 exports inside the game process and forwards the game's upscaling calls to a compatible external implementation, such as OptiScaler.

This repository also includes the `AntiPlayerMosaic/` subproject. It is an independently built plugin for fixing Genshin Impact's mosaic effects and hiding the UID. See the README in that directory for details.

For the Chinese documentation, see [README.md](README.md).

## Support Scope and Risk Notice

- Intended for the Chinese and Global Windows DX11 clients of Genshin Impact.
- Tested only with version `6.7 (Luna VIII)`; compatibility with other game versions or client environments is not guaranteed.
- This project is not affiliated with, endorsed by, or authorized by `HoYoverse`, `miHoYo`, `Genshin Impact`, or `原神`. All related names and trademarks belong to their respective owners.
- Third-party DLLs, injectors, mods, and graphics plugins may violate game rules and could result in account restrictions or bans. Users must evaluate the risks themselves and accept full responsibility.

## Demo

![FSR4 active](assets/FSR4激活.jpg)

![Upscaling preset selection](assets/超分档位切换.jpg)

## Frame-Generation Branch

The frame-generation feature in the `frame-generation` branch is built against `OptiScaler 0.10.0-pre1`. OptiScaler versions earlier than `0.10.0-pre1` do not support this frame-generation feature.

## Lite Release Package

`GenshinOneClick/` contains the complete Lite installer scripts, default configuration, the official ReShade Add-on DLL, and the RenoDX Add-on covered by explicit redistribution permission. The Lite packages no longer bundle any other ReShade shader packages; the installer can download them directly from the official ReShade, Lilium HDR shaders, and SweetFX upstream sources. FPS Unlocker and OptiScaler are likewise downloaded from official sources or selected manually. The existing contents and packaging method of local Full packages remain unchanged.

The two first-party DLLs are build outputs and are not committed to the repository. To generate the same FPS Unlock Lite ZIP as GitHub Actions, run the following command on Windows:

```powershell
powershell -ExecutionPolicy Bypass -File .\Build-OnlineInstaller.ps1 -Configuration Release -GithubLiteOnly
```

The output is written to `dist\原神解帧FSR插件包Lite_v*.zip`, while the GitHub release directory contains only `dist\github-release\GenshinFSRBridge.Lite_v*.zip`. GitHub Actions builds and publishes only this FPS Unlock Lite package; it does not generate a FuFu package.

## FuFu Launcher Plugin Source and Local Build

`FufuGraphicsPlugin/` contains the FuFu Launcher plugin source, configuration templates, and the marketplace/local-test Lua installer scripts. The repository does not commit a prebuilt FuFu Launcher plugin binary. A locally built FuFu Launcher plugin package requires the user to complete the runtime components manually; it does not include scripts or resources that automatically download components. To build the local FPS Unlock Full package and FuFu Launcher plugin package, run:

```powershell
powershell -ExecutionPolicy Bypass -File .\Build-OnlineInstaller.ps1 -Configuration Release
```

The FuFu Launcher plugin package is written only to local `dist\FSR-Bridge-Plugin.v*.zip`; it is not included in GitHub Actions or GitHub Releases. To build the plugin DLL separately:

```powershell
cmake -S .\FufuGraphicsPlugin -B .\build-fufu-plugin -G "Visual Studio 17 2022" -A x64
cmake --build .\build-fufu-plugin --config Release
```

## Features

- Intercepts DX11 device and context activity to obtain the timing of Genshin Impact's FSR2 calls.
- Exposes standard FSR2 exports so external upscaling tools can detect the FSR2 interface.
- Prepares color, depth, motion-vector, jitter, and history resources for the external processor.
- Extends the in-game render-scale menu to `0.2–0.9 + Native`; the `Native` preset uses the game's original `1.0` render scale.
- Writes runtime logs to `Dx11FsrBridge.log` beside the DLL by default for load and hook diagnostics.

## Repository Layout

- Repository root: FSR Bridge source, configuration, and build files.
- `AntiPlayerMosaic/`: anti-aliasing blur removal, UID hiding, and underwater mosaic fix plugin.
- `third_party/`: Bridge build dependencies and their original notices.

## Usage

Download an archive from [Releases](https://github.com/AizawaHikaru233/genshin_fsr_brigde/releases), extract it, and run `一键配置.bat`, then follow the prompts to install. For the English interface, run `GenshinFSRBridgeTools.bat`. You can also switch between Chinese and English at any time from the installer main menu; the selection is saved automatically. Depending on the environment, the script obtains [Genshin FPS Unlock](https://github.com/34736384/genshin-fps-unlock/releases), the [NVIDIA DLSS upscaling component (`nvngx_dlss.dll`)](https://github.com/NVIDIA-RTX/Streamline/releases), and [OptiScaler](https://github.com/optiscaler/OptiScaler/releases) to complete the runtime environment.
In-game `FSR2` anti-aliasing must be enabled, and the render scale must be below `1`.

`Dx11FsrBridge.dll` does not implement FSR, DLSS, XeSS, or any other upscaling algorithm. It only exposes a standard FSR2 interface to external tools and forwards the game's DX11 upscaling calls to that interface.

Other DLL injection tools may also be used, but they must support stable ordered loading.

Recommended loading order:

1. `Dx11FsrBridge.dll`
2. `OptiScaler.dll`
3. `AntiPlayerMosaic.dll` (optional)
4. `ReShade64.dll` (optional)

When using only the upscaling bridge, the first two items must remain in this order: load Bridge first, then let [OptiScaler](https://github.com/optiscaler/OptiScaler), or a comparable tool, scan and take over the standard FSR2 exports at startup. Bridge does not directly load, modify, or bundle OptiScaler. Backend selection, FSR3/FSR4 models, and other OptiScaler settings are managed by the user's own tool installation.

OptiScaler and ReShade runtime configurations are located in their respective component directories. OptiScaler DLL and log paths, as well as ReShade shader, texture, preset, and screenshot paths, use relative paths. This prevents third-party configuration-saving logic from incorrectly transcoding installation paths that contain Chinese characters. Only the game-directory `[INSTALL] BasePath`, which locates the external ReShade directory, must use a dynamically generated absolute path when installed across directories or drives.

## Build

Visual Studio 2022 with the Desktop development with C++ workload, the Windows SDK, and CMake 3.20 or newer are required.

```powershell
cmake -S .\Dx11FsrBridge -B .\build-package-bridge -G "Visual Studio 17 2022" -A x64 `
  -DDX11FSRBRIDGE_RELEASE_RUNTIME=ON `
  -DDX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL=ON
cmake --build .\build-package-bridge --config Release
```

The DLL and `Dx11FsrBridge.release.ini` are written to `build-package-bridge\Release`. The release configuration requires the repository's `Dx11FsrBridge\third_party` directory, which contains FSR2-compatible ABI headers and the Microsoft Detours build dependency.

`DX11FSRBRIDGE_ENABLE_FSR31_EXPERIMENTAL`, `DX11FSRBRIDGE_ENABLE_OPTISCALER_NGX_EXPERIMENTAL`, and related CMake options are experimental only and are not part of the supported release path.

## Logs and Issue Feedback

Bridge, OptiScaler, and the anti-mosaic component retain error logs by default. Each new run overwrites the previous run's logs.
If the game fails to start, FSR cannot be activated, switching upscalers causes a crash, or another issue occurs, do not launch the game again after reproducing it. Provide the following files and information:

1. `payload/Bridge/Dx11FsrBridge.log` (required)
2. `payload/OptiScaler/OptiScaler.log` (required)
3. `payload/OptiScaler/OptiScaler.ini`
4. `payload/ReShade/ReShade.log` (for ReShade-related issues)
5. `payload/AntiPlayerMosaic/AntiPlayerMosaic.log` (for anti-mosaic, UID, or underwater mosaic issues)
6. `FSR-Bridge-Plugin.log` from the FuFu plugin directory (when using the FuFu Launcher plugin)
7. GPU model, game version, the stage at which the issue occurs, and the selected upscaling mode

When further diagnostics are needed, temporarily change `LogLevel` under `Log` in `OptiScaler.ini` to `1 (Debug)` or `0 (Trace)`. Restore the release setting after diagnostics to avoid additional overhead.
Do not submit game account details, login information, or screenshots containing personal information to a public Issue.

## Third-Party Components

- FSR2 ABI headers and Microsoft Detours are build dependencies only; their original licenses and notices are retained.
- OptiScaler is an independent project: <https://github.com/optiscaler/OptiScaler>.
- Lite resources bundle only the official ReShade Add-on DLL (BSD-3-Clause) and the RenoDX Add-on explicitly authorized for unmodified redistribution by [Bilibili UID 3461582765951639](https://space.bilibili.com/3461582765951639). Lilium HDR shaders (GPL-3.0), SweetFX (MIT), and the required standard ReShade effects are downloaded directly from their official upstream sources during installation. The RenoDX author's display name in the permission evidence is “卡文迪许爱吃香蕉” and was previously referenced as “剪刀妹丽丽”; the UID is the stable identity reference across name changes. `RenoDX-Genshin/` is the sole archived source for RenoDX and its permission evidence and is automatically synchronized to the ReShade payload during packaging.
- This project does not include NVIDIA DLSS, the AMD FSR SDK, or OptiScaler runtime binaries.

## License

This project is licensed under [GPL-3.0-or-later](Dx11FsrBridge/LICENSE). You may use, modify, and redistribute the code; when distributing a modified version, you must provide the corresponding complete source code and license it under GPL-3.0-or-later.
