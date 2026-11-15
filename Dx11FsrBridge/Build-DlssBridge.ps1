# Build-DlssBridge.ps1 - build Dx11FsrBridge.dll with a portable MSVC toolchain.
#
# WHY THIS SCRIPT EXISTS
#   The bridge has 19 __try blocks that probe game memory and patch the IAT.
#   MinGW clang under the windows-gnu ABI compiles __try but emits NO SEH tables,
#   so access violations kill the game instead of falling back gracefully.
#   A DLL built with MinGW therefore looks fine and crashes on any game update.
#   This script uses real MSVC instead.
#
#   No Visual Studio is installed and there is no admin rights here, so it uses
#   the VS BuildTools --layout mode (download only, no install) to obtain cl.exe
#   plus the Windows SDK pulled from NuGet, assembled into one usable toolchain.
#
# USAGE
#   powershell -ExecutionPolicy Bypass -File .\Build-DlssBridge.ps1
#   powershell -ExecutionPolicy Bypass -File .\Build-DlssBridge.ps1 -Clean
#
# OUTPUT
#   build\Dx11FsrBridge.dll  (static CRT, no VC redist needed)

[CmdletBinding()]
param(
    # Root that holds the toolchain layout (a directory containing
    # _vs\msvcroot and _msvc\sdkfull, see -ToolchainRoot below).
    #
    # Left empty on purpose: `$PSScriptRoot` is NOT yet bound while parameter
    # default values are evaluated, so `Split-Path -Parent $PSScriptRoot` here
    # throws ParameterArgumentValidationErrorEmptyStringNotAllowed when the
    # script is invoked via `powershell -File`. Resolved after the param block.
    [string] $WorkspaceRoot = "",
    # Explicit toolchain root; overrides $WorkspaceRoot when non-empty.
    [string] $ToolchainRoot = "",
    [switch] $Clean,
    # Value written to DlssInput in the generated ini. 1=auto 2=force.
    # ⚠️ 没有 0=off 这一档：桥侧把 0 与任何未知值都按 auto 处理 —— 原先把 0 当
    #   「强制 FFX」会让桥自己跑 FFX 超分，与 OptiScaler 的 Dx11Upscaler=dlss 冲突。
    # 默认 1=auto：与 package.ini / Deploy-Bridge.ps1 / load_config 四处一致。
    [ValidateSet(1, 2)][int] $DlssInputMode = 1,
    # Emit DlssInputMvProbe=1 in the generated ini (MV readback diagnostic).
    [switch] $MvProbe,
    # Build WITH diagnostics (Fsr2InputDump etc.). Release builds exclude them.
    [switch] $Diagnostics,
    # Depth range normalisation factor written to the generated ini (default 32).
    [ValidateRange(1, 256)][int] $DepthScale = 32
)

$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------------------
# Resolve paths AFTER the param block ($PSScriptRoot is only bound from here on)
# ---------------------------------------------------------------------------
# $PSScriptRoot = <repo>\Dx11FsrBridge ; the repo root is its parent.
$scriptRepoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($ToolchainRoot)) {
    if (-not [string]::IsNullOrWhiteSpace($WorkspaceRoot)) {
        $ToolchainRoot = $WorkspaceRoot
    } else {
        $ToolchainRoot = Split-Path -Parent $scriptRepoRoot
    }
}
$WorkspaceRoot = $ToolchainRoot

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
# Source tree comes from the script location, NOT from $WorkspaceRoot: an
# outsider may clone the repo anywhere while the toolchain lives elsewhere.
$repo       = $scriptRepoRoot
$src        = Join-Path $repo "Dx11FsrBridge"
$toolRoot   = Join-Path $ToolchainRoot "_vs"
$msvcRoot   = Join-Path $toolRoot "msvcroot\VC\Tools\MSVC\14.44.35207"
$msvcBin    = Join-Path $msvcRoot "bin\Hostx64\x64"
$sdkRoot    = Join-Path $WorkspaceRoot "_msvc\sdkfull\c"
$sdkInclude = Join-Path $sdkRoot "Include\10.0.22621.0"
$sdkLib     = Join-Path $sdkRoot "Lib\10.0.22621.0"
$ffxKit     = Join-Path $WorkspaceRoot "_ffxsdk\FidelityFX-SDK-main\Kits\FidelityFX"
$outDir     = Join-Path $src "build-dlssbridge"   # 2026-11-16（审查 F5）：与 Build-Bridge.ps1 分开，避免 obj 互相污染
$objDir     = Join-Path $outDir "obj"
$tmpDir     = Join-Path $toolRoot "wtemp"

if ($Clean -and (Test-Path $outDir)) {
    Remove-Item $outDir -Recurse -Force
    Write-Host "cleaned $outDir"
}
New-Item -ItemType Directory -Force -Path $outDir, $objDir, $tmpDir | Out-Null

# ---------------------------------------------------------------------------
# Preflight - report exactly what is missing, never degrade silently
# ---------------------------------------------------------------------------
$checks = [ordered]@{
    "cl.exe"            = Join-Path $msvcBin "cl.exe"
    "clui.dll"          = Join-Path $msvcBin "1033\clui.dll"
    "MSVC vcruntime.h"  = Join-Path $msvcRoot "include\vcruntime.h"
    "MSVC libcmt.lib"   = Join-Path $msvcRoot "lib\x64\libcmt.lib"
    "SDK windows.h"     = Join-Path $sdkInclude "um\windows.h"
    "SDK ucrt.lib"      = Join-Path $sdkLib "ucrt\x64\ucrt.lib"
    "SDK kernel32.lib"  = Join-Path $sdkLib "um\x64\kernel32.lib"
    "SDK rc.exe"        = Join-Path $sdkRoot "bin\10.0.22621.0\x64\rc.exe"
    "FFX ffx_api.h"     = Join-Path $ffxKit "api\include\ffx_api.h"
    "FFX ffx_upscale.h" = Join-Path $ffxKit "upscalers\include\ffx_upscale.h"
}
$missing = @()
foreach ($k in $checks.Keys) {
    if (-not (Test-Path -LiteralPath $checks[$k])) {
        $missing += ("{0}  ({1})" -f $k, $checks[$k])
    }
}
if ($missing.Count -gt 0) {
    Write-Host "MISSING BUILD DEPENDENCIES:" -ForegroundColor Red
    $missing | ForEach-Object { Write-Host ("  - " + $_) -ForegroundColor Red }
    Write-Host ""
    Write-Host "See DLSS_INPUT.md section 10 for the one-time toolchain setup." -ForegroundColor Yellow
    exit 1
}

# ---------------------------------------------------------------------------
# Emit a matching ini next to the DLL.
#
# The installer regenerates Dx11FsrBridge.ini from Dx11FsrBridge.package.ini on
# every install, which silently wipes any hand-added key (that is exactly how
# the first DlssInput=2 attempt got lost). Shipping a known-good ini with the
# build makes the deployed config unambiguous.
#
# -DlssInputMode overrides the default in the generated file:
#   0 = off, 1 = auto, 2 = force  (default 2 for test builds)
# ---------------------------------------------------------------------------

$env:INCLUDE = (@(
    (Join-Path $msvcRoot "include")
    (Join-Path $sdkInclude "ucrt")
    (Join-Path $sdkInclude "shared")
    (Join-Path $sdkInclude "um")
    (Join-Path $sdkInclude "winrt")
    (Join-Path $ffxKit "api\include")
    (Join-Path $ffxKit "upscalers\include")
    $src
    (Join-Path $src "third_party")
    (Join-Path $src "third_party\detours")
) -join ";")

$env:LIB = (@(
    (Join-Path $msvcRoot "lib\x64")
    (Join-Path $sdkLib "ucrt\x64")
    (Join-Path $sdkLib "um\x64")
) -join ";")

$env:TMP  = $tmpDir
$env:TEMP = $tmpDir
$env:PATH = $msvcBin + ";" + $env:PATH

# ---------------------------------------------------------------------------
# Version header (the .rc file does #include "Version.h")
# ---------------------------------------------------------------------------
$versionCmake = Join-Path $repo "Version.cmake"
$versionLine = Select-String -LiteralPath $versionCmake -Pattern 'set\s*\(\s*FSR_SUITE_VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)' |
    Select-Object -First 1
$version = "0.0.0"
if ($versionLine) { $version = $versionLine.Matches[0].Groups[1].Value }
$parts = @($version.Split('.')) + @('0', '0', '0')

$verLines = @(
    '/* Generated by Build-DlssBridge.ps1 from Version.cmake (single source of truth). */'
    '/* Do not edit: regenerate. ASCII only - rc.exe rejects CJK comments here. */'
    '#pragma once'
    ''
    ('#define DX11FSRBRIDGE_VERSION_MAJOR ' + $parts[0])
    ('#define DX11FSRBRIDGE_VERSION_MINOR ' + $parts[1])
    ('#define DX11FSRBRIDGE_VERSION_PATCH ' + $parts[2])
    '#define DX11FSRBRIDGE_VERSION_TWEAK 0'
    ''
    '#define DX11FSRBRIDGE_VERSION_COMMA DX11FSRBRIDGE_VERSION_MAJOR,DX11FSRBRIDGE_VERSION_MINOR,DX11FSRBRIDGE_VERSION_PATCH,DX11FSRBRIDGE_VERSION_TWEAK'
    ''
    '#define DX11FSRBRIDGE_STR2(x) #x'
    '#define DX11FSRBRIDGE_STR(x) DX11FSRBRIDGE_STR2(x)'
    '#define DX11FSRBRIDGE_VERSION_DOTTED DX11FSRBRIDGE_STR(DX11FSRBRIDGE_VERSION_MAJOR) "." DX11FSRBRIDGE_STR(DX11FSRBRIDGE_VERSION_MINOR) "." DX11FSRBRIDGE_STR(DX11FSRBRIDGE_VERSION_PATCH) "." DX11FSRBRIDGE_STR(DX11FSRBRIDGE_VERSION_TWEAK)'
)
Set-Content -LiteralPath (Join-Path $outDir "Version.h") -Value $verLines -Encoding ASCII
Write-Host ("version = " + $version)

# ---------------------------------------------------------------------------
# Sources - same set as the CMake main target. Diagnostic .cpp files are
# excluded, matching the release configuration.
# ---------------------------------------------------------------------------
$cppSources = @(
    "Dx11FsrBridge.cpp"
    "BridgeLogger.cpp"
    "RenderScaleMenu.cpp"
    "Fsr2FamilyTakeover.cpp"
    "Il2CppCallSiteHook.cpp"
    "TransparentJitterHook.cpp"
    "Ffx12Backend.cpp"
    "DlssInput.cpp"
    "NgxParamMap.cpp"   # 纯计算层：FFX 路径与直连层共用（map_jitter / map_mv_scale）
)

$defines = @(
    "WIN32_LEAN_AND_MEAN", "NOMINMAX", "UNICODE", "_UNICODE", "_USRDLL",
    "DX11FSRBRIDGE_RELEASE_RUNTIME=1",
    "DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL=1"
)

# Diagnostics build: Fsr2InputDump.cpp is only compiled here (CMakeLists does the
# same: the file is excluded from release builds). -Diagnostics also defines
# DX11FSRBRIDGE_ENABLE_DIAGNOSTICS so the #if-guarded dump call sites exist.
if ($Diagnostics) {
    $cppSources += "Fsr2InputDump.cpp"
    $defines += "DX11FSRBRIDGE_ENABLE_DIAGNOSTICS=1"
    Write-Host "DIAGNOSTICS BUILD: Fsr2InputDump included (not for release)" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "=== compiling ===" -ForegroundColor Cyan
$objs = New-Object System.Collections.ArrayList
$failed = New-Object System.Collections.ArrayList

foreach ($s in $cppSources) {
    $leaf = [System.IO.Path]::GetFileNameWithoutExtension($s)
    $obj = Join-Path $objDir ($leaf + ".obj")

    # /Brepro: reproducible build. Without it the linker stamps the current time
    # into the PE header, so every rebuild yields a different SHA-256 even when
    # nothing changed - which makes "is this the same binary?" unanswerable.
    $a = @("/nologo", "/c", "/std:c++20", "/EHsc", "/O2", "/MT", "/W3", "/utf-8", "/Brepro")
    $a += ("/I" + $outDir)
    $a += ("/Fo" + $obj)
    foreach ($d in $defines) { $a += ("/D" + $d) }
    $a += (Join-Path $src $s)

    $logOut = & (Join-Path $msvcBin "cl.exe") @a 2>&1
    if ($LASTEXITCODE -ne 0) {
        [void]$failed.Add($s)
        Write-Host ("  FAIL " + $s) -ForegroundColor Red
        $logOut | Select-Object -First 25 | ForEach-Object { Write-Host ("    " + $_) }
    } else {
        [void]$objs.Add($obj)
        Write-Host ("  ok   " + $s) -ForegroundColor Green
    }
}

# Resource (version info). rc.exe ships with the Windows SDK, not with MSVC.
$rcExe  = Join-Path $sdkRoot "bin\10.0.22621.0\x64\rc.exe"
$rcObj = Join-Path $objDir "Dx11FsrBridge.res"
$rcArgs = @("/nologo", ("/I" + $outDir), ("/I" + $src), "/D_UNICODE", "/DUNICODE",
            ("/fo" + $rcObj), (Join-Path $src "Dx11FsrBridge.rc"))
$rcOut = & $rcExe @rcArgs 2>&1
if ($LASTEXITCODE -ne 0) {
    [void]$failed.Add("Dx11FsrBridge.rc")
    Write-Host "  FAIL Dx11FsrBridge.rc" -ForegroundColor Red
    $rcOut | Select-Object -First 15 | ForEach-Object { Write-Host ("    " + $_) }
} else {
    [void]$objs.Add($rcObj)
    Write-Host "  ok   Dx11FsrBridge.rc" -ForegroundColor Green
}

if ($failed.Count -gt 0) {
    Write-Host ""
    Write-Host ("COMPILE FAILED: " + ($failed -join ", ")) -ForegroundColor Red
    exit 2
}

# ---------------------------------------------------------------------------
# Link
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "=== linking ===" -ForegroundColor Cyan
$dll = Join-Path $outDir "Dx11FsrBridge.dll"

$la = @("/nologo", "/DLL", "/INCREMENTAL:NO", "/MACHINE:X64", "/Brepro")
$la += ("/OUT:" + $dll)
$la += ("/PDB:" + (Join-Path $outDir "Dx11FsrBridge.pdb"))
$la += ("/MAP:" + (Join-Path $outDir "Dx11FsrBridge.map"))
# Static CRT. The link order matters: libcmt first, then libvcruntime, then
# libucrt, then oldnames. libucrt.lib itself references __crt_debugger_hook,
# which is defined in the *desktop* libvcruntime.lib (2,087,406 bytes) - not in
# the enclave/onecore variants (2,063,722 bytes). See DLSS_INPUT.md section 10;
# the build script's preflight cannot detect that substitution, so the size is
# documented here as the tell.
foreach ($o in $objs) { $la += $o }
$la += @(
    "d3d11.lib", "d3d12.lib", "d3dcompiler.lib", "dxgi.lib",
    "user32.lib", "kernel32.lib", "advapi32.lib", "version.lib",
    # gdi32: OSD window (diagnostics build) uses CreateFontW/CreateSolidBrush/etc.
    "gdi32.lib",
    (Join-Path $src "third_party\detours\detours.lib"),
    "libcmt.lib", "libvcruntime.lib", "libucrt.lib", "oldnames.lib"
)

$linkOut = & (Join-Path $msvcBin "link.exe") @la 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "LINK FAILED:" -ForegroundColor Red
    $linkOut | Select-Object -First 30 | ForEach-Object { Write-Host ("  " + $_) }
    exit 3
}

if (-not (Test-Path -LiteralPath $dll)) {
    Write-Host "Link reported success but the artifact is missing." -ForegroundColor Red
    exit 4
}

# ---------------------------------------------------------------------------
# Ship a matching ini beside the DLL
# ---------------------------------------------------------------------------
# Rationale: the installer regenerates Dx11FsrBridge.ini from the packaged
# template on every install, so any hand-added key is silently lost. Emitting a
# known-good ini from the same template at build time keeps "the binary I built"
# and "the config it expects" together, and makes the deploy step a plain copy.
#
# Encoding matters. The template is UTF-8 **without BOM**, and the official
# packaging flow (Build-OnlineInstaller.ps1:471/542) only ever does a byte-for-byte
# Copy-Item of it - it never re-encodes. Writing with UTF8Encoding($true) prepends
# EF BB BF (renders as a stray glyph / garbled first line); writing with the
# default ANSI code page mangles every CJK character. So: read the bytes, patch
# only the DlssInput line, write the bytes back unchanged otherwise.
$iniSrc = Join-Path $src "Dx11FsrBridge.package.ini"
$iniOut = Join-Path $outDir "Dx11FsrBridge.ini"
if (Test-Path -LiteralPath $iniSrc) {
    $raw = [System.IO.File]::ReadAllBytes($iniSrc)
    $hasBom = ($raw.Length -ge 3 -and $raw[0] -eq 0xEF -and $raw[1] -eq 0xBB -and $raw[2] -eq 0xBF)
    $text = (New-Object System.Text.UTF8Encoding($false)).GetString($raw)
    if ($hasBom) { $text = $text.TrimStart([char]0xFEFF) }
    $emitted = New-Object System.Collections.ArrayList
    $sawKey = $false
    $probeValue = if ($MvProbe) { 1 } else { 0 }
    foreach ($l in ($text -split "`r?`n")) {
        if ($l -match '^\s*DlssInput\s*=') {
            [void]$emitted.Add("DlssInput=$DlssInputMode")
            $sawKey = $true
        } elseif ($l -match '^\s*DlssInputMvProbe\s*=') {
            [void]$emitted.Add("DlssInputMvProbe=$probeValue")
        } elseif ($l -match '^\s*DlssInputDepthScale\s*=') {
            [void]$emitted.Add("DlssInputDepthScale=$DepthScale")
        } elseif ($l -match '^\s*DlssInputJitterProbe\s*=') {
            [void]$emitted.Add("DlssInputJitterProbe=$probeValue")
        } elseif ($Diagnostics -and $l -match '^\s*Fsr2InputDump\s*=') {
            [void]$emitted.Add("Fsr2InputDump=1")
        } elseif ($Diagnostics -and $l -match '^\s*Fsr2InputDumpFrames\s*=') {
            [void]$emitted.Add("Fsr2InputDumpFrames=6")
        } elseif ($Diagnostics -and $l -match '^\s*Fsr2InputDumpHotkey\s*=') {
            # F10 = VK_F10 = 0x79 = 121. Hotkey-only so the dump never fires
            # during the login screen (AutoStartSec must stay 0 for that).
            [void]$emitted.Add("Fsr2InputDumpHotkey=121")
        } elseif ($Diagnostics -and $l -match '^\s*Fsr2InputDumpAutoStartSec\s*=') {
            [void]$emitted.Add("Fsr2InputDumpAutoStartSec=0")
        } elseif ($Diagnostics -and $l -match '^\s*Fsr2InputDumpIntervalMs\s*=') {
            [void]$emitted.Add("Fsr2InputDumpIntervalMs=80")
        } else {
            [void]$emitted.Add($l)
        }
    }
    if (-not $sawKey) {
        Write-Host "WARNING: template has no DlssInput key; appending one." -ForegroundColor Yellow
        [void]$emitted.Add("DlssInput=$DlssInputMode")
    }
    # Preserve the template's BOM state exactly (it ships without one).
    $enc = New-Object System.Text.UTF8Encoding($hasBom)
    [System.IO.File]::WriteAllBytes($iniOut, $enc.GetBytes(($emitted -join "`r`n")))
    Write-Host ("  ini    : {0}  (DlssInput={1}, bom={2})" -f $iniOut, $DlssInputMode, $hasBom)
} else {
    Write-Host ("WARNING: ini template missing at {0}" -f $iniSrc) -ForegroundColor Yellow
}

# ---------------------------------------------------------------------------
# Verify
# ---------------------------------------------------------------------------
$info = Get-Item -LiteralPath $dll
$sha  = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash
Write-Host ""
Write-Host "=== BUILD OK ===" -ForegroundColor Green
Write-Host ("  " + $dll)
Write-Host ("  size   : {0:N0} bytes" -f $info.Length)
Write-Host ("  sha256 : " + $sha)
Write-Host ""
Write-Host "Architecture and dependencies:" -ForegroundColor Cyan
$dumpbin = Join-Path $msvcBin "dumpbin.exe"
& $dumpbin /headers $dll 2>&1 | Select-String "machine \(" | Select-Object -First 1
& $dumpbin /dependents $dll 2>&1 | Select-String "\.dll" | Select-Object -First 25
Write-Host ""
Write-Host "SEH self-check (Exception Directory / .pdata must be present):" -ForegroundColor Cyan
& $dumpbin /headers $dll 2>&1 | Select-String "Exception Directory|\.pdata" | Select-Object -First 3
