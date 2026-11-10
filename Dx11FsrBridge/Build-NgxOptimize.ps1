# Build-NgxOptimize.ps1 - build the current branch (upstream main + local fixes).
#
# Unlike Build-DlssBridge.ps1 (which targets the dlss-input branch and its own
# DlssInput.cpp), this one builds the *upstream* source layout, i.e. it compiles
# NgxBackend.cpp and needs DX11FSRBRIDGE_ENABLE_NGX=1.
#
# Source list mirrors CMakeLists.txt's add_library() plus the NGX-gated TU.
#
# Toolchain: portable MSVC + Windows SDK, no CMake, no admin rights.
#   <ToolchainRoot>\_vs\msvcroot    -> MSVC 14.44.35207
#   <ToolchainRoot>\_msvc\sdkfull   -> Windows SDK 10.0.22621
#   <ToolchainRoot>\_ffxsdk         -> FidelityFX SDK (ffx_api.h / ffx_upscale.h)

[CmdletBinding()]
param(
    # Root holding _vs / _msvc / _ffxsdk. Empty => parent of the repo checkout.
    [string] $ToolchainRoot = "",
    # Extra include dirs are derived from ToolchainRoot; override the repo here.
    [string] $RepoRoot = "",
    [switch] $Clean,
    # Build with diagnostics (Fsr2InputDump.cpp + DX11FSRBRIDGE_ENABLE_DIAGNOSTICS).
    [switch] $Diagnostics
)

$ErrorActionPreference = "Stop"

# $PSScriptRoot is only bound after the param block; resolve here.
$scriptSrcDir = $PSScriptRoot                       # <repo>\Dx11FsrBridge
$scriptRepo   = Split-Path -Parent $scriptSrcDir    # <repo>
if ([string]::IsNullOrWhiteSpace($RepoRoot))    { $RepoRoot = $scriptRepo }
if ([string]::IsNullOrWhiteSpace($ToolchainRoot)) {
    $ToolchainRoot = Split-Path -Parent $scriptRepo
}

$src     = Join-Path $RepoRoot "Dx11FsrBridge"
$vcRoot  = Join-Path $ToolchainRoot "_vs\msvcroot\VC\Tools\MSVC\14.44.35207"
$cl      = Join-Path $vcRoot "bin\Hostx64\x64\cl.exe"
$link    = Join-Path $vcRoot "bin\Hostx64\x64\link.exe"
$sdk     = Join-Path $ToolchainRoot "_msvc\sdkfull\c"
$sdkInc  = Join-Path $sdk "Include\10.0.22621.0"
$sdkLib  = Join-Path $sdk "Lib\10.0.22621.0"
$rc      = Join-Path $sdk "bin\10.0.22621.0\x64\rc.exe"
$ffxRoot = Join-Path $ToolchainRoot "_ffxsdk\FidelityFX-SDK-main\Kits\FidelityFX"
$ffxApi  = Join-Path $ffxRoot "api\include"
$ffxUp   = Join-Path $ffxRoot "upscalers\include"

# ---- preflight: report every missing piece, then abort (never degrade silently)
$missing = [System.Collections.Generic.List[string]]::new()
foreach ($p in @($cl, $link, $rc)) {
    if (-not (Test-Path $p)) { $missing.Add("toolchain: $p") }
}
foreach ($p in @(
    (Join-Path $sdkInc "um\windows.h"),
    (Join-Path $sdkLib "ucrt\x64\ucrt.lib"),
    (Join-Path $sdkLib "um\x64\kernel32.lib"),
    (Join-Path $ffxApi "ffx_api.h"),
    (Join-Path $ffxUp  "ffx_upscale.h"),
    (Join-Path $src    "third_party\ngx\nvsdk_ngx_defs.h"),
    (Join-Path $src    "third_party\ngx\nvsdk_ngx_params.h")
)) {
    if (-not (Test-Path $p)) { $missing.Add("dependency: $p") }
}
if ($missing.Count -gt 0) {
    Write-Host "MISSING BUILD DEPENDENCIES:" -ForegroundColor Red
    foreach ($m in $missing) { Write-Host ("  - " + $m) -ForegroundColor Red }
    Write-Host ""
    Write-Host "ToolchainRoot was: $ToolchainRoot"
    Write-Host "Pass -ToolchainRoot <dir> pointing at the folder holding _vs / _msvc / _ffxsdk."
    exit 1
}

$outDir = Join-Path $src "build"
$objDir = Join-Path $outDir "obj"
if ($Clean -and (Test-Path $outDir)) { Remove-Item $outDir -Recurse -Force }
New-Item -ItemType Directory -Path $objDir -Force | Out-Null
Write-Host "  cleaned $outDir"

# ---- Version.h from Version.cmake
$verFile = Join-Path $RepoRoot "Version.cmake"
$verText = Get-Content $verFile -Raw
if ($verText -notmatch 'set\(FSR_SUITE_VERSION\s+([0-9]+)\.([0-9]+)\.([0-9]+)\.([0-9]+)\)') {
    throw "cannot parse FSR_SUITE_VERSION from $verFile"
}
$a = $Matches[1]; $b = $Matches[2]; $c = $Matches[3]; $d = $Matches[4]
$versionH = Join-Path $outDir "Version.h"
$vl = @(
    '#pragma once',
    ('#define DX11FSRBRIDGE_VERSION_MAJOR ' + $a),
    ('#define DX11FSRBRIDGE_VERSION_MINOR ' + $b),
    ('#define DX11FSRBRIDGE_VERSION_PATCH ' + $c),
    ('#define DX11FSRBRIDGE_VERSION_TWEAK ' + $d),
    '#define DX11FSRBRIDGE_STR2(x) #x',
    '#define DX11FSRBRIDGE_STR(x) DX11FSRBRIDGE_STR2(x)',
    ('#define DX11FSRBRIDGE_VERSION_DOTTED DX11FSRBRIDGE_STR(DX11FSRBRIDGE_VERSION_MAJOR) "." ' +
     'DX11FSRBRIDGE_STR(DX11FSRBRIDGE_VERSION_MINOR) "." ' +
     'DX11FSRBRIDGE_STR(DX11FSRBRIDGE_VERSION_PATCH) "." ' +
     'DX11FSRBRIDGE_STR(DX11FSRBRIDGE_VERSION_TWEAK)'),
    ('#define DX11FSRBRIDGE_VERSION_COMMA DX11FSRBRIDGE_VERSION_MAJOR,' +
     'DX11FSRBRIDGE_VERSION_MINOR,DX11FSRBRIDGE_VERSION_PATCH,DX11FSRBRIDGE_VERSION_TWEAK')
)
[System.IO.File]::WriteAllLines($versionH, $vl, (New-Object System.Text.ASCIIEncoding))

# ---- sources: mirrors CMakeLists add_library() + NGX TU
$cpp = @(
    "Dx11FsrBridge.cpp"
    "BridgeLogger.cpp"
    "RenderScaleMenu.cpp"
    "Fsr2FamilyTakeover.cpp"
    "Il2CppCallSiteHook.cpp"
    "TransparentJitterHook.cpp"
    "Ffx12Backend.cpp"
    "NgxBackend.cpp"
)
$defs = @(
    "WIN32_LEAN_AND_MEAN", "NOMINMAX", "UNICODE", "_UNICODE", "_USRDLL",
    "DX11FSRBRIDGE_RELEASE_RUNTIME=1",
    "DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL=1",
    "DX11FSRBRIDGE_ENABLE_NGX=1"
)
if ($Diagnostics) {
    $cpp += "Fsr2InputDump.cpp"
    $defs += "DX11FSRBRIDGE_ENABLE_DIAGNOSTICS=1"
    Write-Host "DIAGNOSTICS BUILD: Fsr2InputDump included" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "=== compiling ===" -ForegroundColor Cyan
$objs = [System.Collections.Generic.List[string]]::new()
$failed = [System.Collections.Generic.List[string]]::new()
foreach ($s in $cpp) {
    $srcPath = Join-Path $src $s
    if (-not (Test-Path $srcPath)) { Write-Host ("  MISSING {0}" -f $s) -ForegroundColor Red; $failed.Add($s); continue }
    $obj = Join-Path $objDir (($s -replace '\.cpp$', '') + ".obj")
    $args = @(
        "/nologo", "/c", "/std:c++20", "/O2", "/MT", "/W3", "/utf-8",
        "/Brepro", "/permissive-", "/Zc:__cplusplus", "/Zc:preprocessor",
        ("/I" + $src),
        ("/I" + $outDir),
        ("/I" + (Join-Path $src "third_party")),
        ("/I" + (Join-Path $src "third_party\detours")),
        ("/I" + (Join-Path $src "third_party\ngx")),
        ("/I" + $ffxApi),
        ("/I" + $ffxUp),
        ("/I" + (Join-Path $vcRoot "include")),
        ("/I" + (Join-Path $sdkInc "ucrt")),
        ("/I" + (Join-Path $sdkInc "shared")),
        ("/I" + (Join-Path $sdkInc "um")),
        ("/I" + (Join-Path $sdkInc "winrt")),
        "/D_CRT_SECURE_NO_WARNINGS",
        "/FIintrin.h"
    )
    foreach ($d in $defs) { $args += ("/D" + $d) }
    $args += @(("/Fo" + $obj), $srcPath)
    & $cl @args 2>&1 | ForEach-Object { if ($_ -match 'error') { Write-Host ("    " + $_) -ForegroundColor Red } }
    if ($LASTEXITCODE -ne 0) { Write-Host ("  FAIL {0}" -f $s) -ForegroundColor Red; $failed.Add($s) }
    else { Write-Host ("  ok   {0}" -f $s) -ForegroundColor Green; $objs.Add($obj) }
}
if ($failed.Count -gt 0) { throw ("compile failed: " + ($failed -join ", ")) }

# ---- resource
$rcSrc = Join-Path $src "Dx11FsrBridge.rc"
$resObj = Join-Path $objDir "Dx11FsrBridge.res"
$rcArgs = @("/nologo", ("/I" + $src), ("/I" + $outDir),
            ("/I" + (Join-Path $sdkInc "um")), ("/I" + (Join-Path $sdkInc "shared")),
            ("/fo" + $resObj), $rcSrc)
& $rc @rcArgs 2>&1 | ForEach-Object { if ($_ -match 'error') { Write-Host ("    " + $_) -ForegroundColor Red } }
if ($LASTEXITCODE -ne 0) { throw "rc failed" }
Write-Host "  ok   Dx11FsrBridge.rc" -ForegroundColor Green
$objs.Add($resObj)

# ---- link
Write-Host ""
Write-Host "=== linking ===" -ForegroundColor Cyan
$dll = Join-Path $outDir "Dx11FsrBridge.dll"
$la = @(
    "/nologo", "/DLL", "/DEBUG", "/Brepro", "/INCREMENTAL:NO", "/SUBSYSTEM:WINDOWS",
    ("/OUT:" + $dll),
    ("/PDB:" + (Join-Path $outDir "Dx11FsrBridge.pdb")),
    ("/MAP:" + (Join-Path $outDir "Dx11FsrBridge.map")),
    ("/LIBPATH:" + (Join-Path $vcRoot "lib\x64")),
    ("/LIBPATH:" + (Join-Path $sdkLib "ucrt\x64")),
    ("/LIBPATH:" + (Join-Path $sdkLib "um\x64")),
    "d3d11.lib", "d3d12.lib", "d3dcompiler.lib", "dxgi.lib",
    "user32.lib", "kernel32.lib", "advapi32.lib", "version.lib", "gdi32.lib",
    (Join-Path $src "third_party\detours\detours.lib"),
    "libcmt.lib", "libvcruntime.lib", "libucrt.lib", "oldnames.lib"
)
foreach ($o in $objs) { $la += $o }
& $link @la 2>&1 | ForEach-Object { Write-Host ("  " + $_) }
if ($LASTEXITCODE -ne 0) { throw "link failed" }

Write-Host ""
Write-Host "=== BUILD OK ===" -ForegroundColor Green
Write-Host ("  size   : {0:N0} bytes" -f (Get-Item $dll).Length)
Write-Host ("  sha256 : {0}" -f (Get-FileHash $dll -Algorithm SHA256).Hash)

# ---- ini
$iniSrc = Join-Path $src "Dx11FsrBridge.package.ini"
$iniOut = Join-Path $outDir "Dx11FsrBridge.ini"
if (Test-Path $iniSrc) {
    $bytes = [IO.File]::ReadAllBytes($iniSrc)
    $hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
    $text = if ($hasBom) { [Text.Encoding]::UTF8.GetString($bytes, 3, $bytes.Length - 3) }
            else { [Text.Encoding]::UTF8.GetString($bytes) }
    $enc = New-Object System.Text.UTF8Encoding($hasBom)
    [System.IO.File]::WriteAllBytes($iniOut, $enc.GetBytes($text))
    Write-Host ("  ini    : {0}  (bom={1})" -f $iniOut, $hasBom)
}

Write-Host ""
Write-Host "SEH self-check (Exception Directory / .pdata must be present):"
$dumpbin = Join-Path $vcRoot "bin\Hostx64\x64\dumpbin.exe"
if (Test-Path $dumpbin) {
    & $dumpbin /nologo /headers $dll 2>&1 | Select-String "Exception Directory" | ForEach-Object { "  " + $_.Line.Trim() }
}
