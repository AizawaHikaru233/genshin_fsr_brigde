# Genshin FSR Bridge

An FSR2 ABI bridge DLL for the Genshin Impact Windows DX11 client. It exposes standard FSR2 exports inside the game process and forwards the game's upscaling calls to a compatible external implementation, such as OptiScaler.

This is not an official project of HoYoverse, AMD, or OptiScaler. It does not include OptiScaler, FSR SDK runtimes, GPU driver components, or any other upscaling DLLs.

This repository also includes the `AntiPlayerMosaic/` subproject, an independently built plugin for player mosaic fixes and UID hiding. Refer to its README for details.

[\u7b80\u4f53\u4e2d\u6587](README.md)

## Support Scope and Risk Notice

- Intended for the Chinese and Global Windows DX11 clients of Genshin Impact.
- Tested only with version 6.7 (“Luna VIII”). Compatibility with other game versions or client environments is not guaranteed.
- This project is not affiliated with, endorsed by, or authorized by HoYoverse, miHoYo, Genshin Impact, or 《原神》. All related names and trademarks belong to their respective owners.
- Third-party DLLs, injectors, mods, and graphics plugins may violate game rules and can result in account restrictions or bans. You are solely responsible for evaluating and accepting these risks.

## Frame-Generation Branch

The frame-generation feature in the `frame-generation` branch is built against `OptiScaler 0.10.0-pre1`. OptiScaler versions earlier than `0.10.0-pre1` do not support this frame-generation feature.

## Lite Release Package

`GenshinOneClick/` contains the complete Lite installer scripts, default configuration, the official ReShade Add-on DLL, and HDR shaders. FPS Unlocker and OptiScaler are not distributed with the Lite package or this repository; the installer downloads them from their official sources or asks you to select them manually.

The two first-party DLLs are build outputs and are not committed. To generate the same Lite ZIP as GitHub Actions on Windows, run:

```powershell
powershell -ExecutionPolicy Bypass -File .\Build-Package.ps1
```

The output is written to `dist\原神解帧FSR插件包Lite_v*.zip`. GitHub Actions is manually dispatched from the Actions page and always uploads a ZIP as the `GenshinOneClick-Lite` artifact. When the release option is enabled and a version tag is supplied, it also creates or updates the corresponding GitHub Release.

## Features

- Intercepts DX11 device and context activity to identify Genshin Impact FSR2 call timing.
- Exposes standard FSR2 exports so external upscalers can detect the FSR2 interface.
- Prepares color, depth, motion-vector, jitter, and history resources for the external processor.
- Writes runtime logs to `Dx11FsrBridge.log` beside the DLL by default for load and hook diagnostics.
- Disables resource exports, per-frame tracing, and debug probes in the release configuration to avoid unnecessary overhead.

## Repository Layout

- Repository root: FSR Bridge source, configuration, and build files.
- `AntiPlayerMosaic/`: player mosaic, UID hiding, and underwater mosaic fix plugin.
- `third_party/`: Bridge build dependencies and their original notices.

## Usage

Download a package from [Releases](https://github.com/AizawaHikaru233/genshin_fsr_brigde/releases), extract it, then run `GenshinFSRBridgeTools.bat` for English or `一键配置.bat` for Chinese. You can also switch languages at any time from the installer main menu; the selected language is saved automatically. Depending on your environment, the script obtains [Genshin FPS Unlock](https://github.com/34736384/genshin-fps-unlock/releases), the [NVIDIA DLSS upscaling component (`nvngx_dlss.dll`)](https://github.com/NVIDIA-RTX/Streamline/releases), and [OptiScaler](https://github.com/optiscaler/OptiScaler/releases) to complete the runtime environment.

`Dx11FsrBridge.dll` does not implement FSR, DLSS, XeSS, or any other upscaling algorithm. It only exposes a standard FSR2 interface to external tools and redirects the game's DX11 upscaling calls to that interface.

Other DLL injection tools may be used, but they must support stable ordered loading.

Recommended loading order:

1. `Dx11FsrBridge.dll`
2. `OptiScaler.dll`
3. `AntiPlayerMosaic.dll` (optional)
4. `ReShade64.dll` (optional)

When only using the upscaling bridge, the first two items must stay in this order: load Bridge first, then let [OptiScaler](https://github.com/optiscaler/OptiScaler) or a comparable tool scan and take over the standard FSR2 exports at startup. Bridge does not load, modify, or bundle OptiScaler. Backend selection, FSR3/FSR4 models, and other OptiScaler settings remain the responsibility of the user's own tool installation.

## Build

Visual Studio 2022 with the Desktop C++ workload, the Windows SDK, and CMake 3.20 or newer are required.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DDX11FSRBRIDGE_RELEASE_RUNTIME=ON `
  -DDX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL=ON
cmake --build build --config Release
```

The DLL and `Dx11FsrBridge.release.ini` are written to `build\Release`. The release configuration requires the repository `third_party` directory, which contains FSR2-compatible ABI headers and the Microsoft Detours build dependency.

`DX11FSRBRIDGE_ENABLE_FSR31_EXPERIMENTAL`, `DX11FSRBRIDGE_ENABLE_OPTISCALER_NGX_EXPERIMENTAL`, and related CMake options are experimental only and are not part of the supported release path.

## Configuration and Feedback

The DLL reads `Dx11FsrBridge.ini` from its own directory. The release package enables basic logging by default. After reproducing a problem, retain and submit:

- `Dx11FsrBridge.log`
- `Dx11FsrBridge.ini`
- OptiScaler logs and configuration, if used
- GPU model, game version, failure stage, and selected upscaling mode

Do not submit game account information, login details, or screenshots containing personal information to public issues.

## Third-Party Components

- FSR2 ABI headers and Microsoft Detours are build dependencies only; their original licenses and notices are retained.
- OptiScaler is an independent project: <https://github.com/optiscaler/OptiScaler>.
- Lite resources include the official ReShade Add-on DLL (BSD-3-Clause), the RenoDX Add-on redistributed with permission from [剪刀妹丽丽](https://www.bilibili.com/video/av116861345793770/), and Lilium HDR shaders (GPL-3.0). Their licenses and permission records are kept with the resources.
- This project does not include NVIDIA DLSS, the AMD FSR SDK, or OptiScaler runtime binaries.

## License

This project is licensed under [GPL-3.0-or-later](LICENSE). You may use, modify, and redistribute the code; when distributing a modified version, you must provide the corresponding complete source code and license it under GPL-3.0-or-later.
