# Fetches the NVIDIA NGX SDK headers into third_party/ngx.
#
# The headers are NVIDIA-proprietary and are therefore not committed. They are
# plain declarations (types, enums, string constants) pulled from NVIDIA's
# public DLSS repository; no NVIDIA implementation is redistributed.
#
# Usage:
#   pwsh -File tools/Fetch-NgxHeaders.ps1
#   pwsh -File tools/Fetch-NgxHeaders.ps1 -Ref main
#   pwsh -File tools/Fetch-NgxHeaders.ps1 -Force

[CmdletBinding()]
param(
    [string]$Ref = 'main',
    [string]$Destination,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $Destination) {
    $Destination = Join-Path $repoRoot 'Dx11FsrBridge\third_party\ngx'
}

# Names as published in https://github.com/NVIDIA/DLSS/tree/main/include
$files = @(
    'nvsdk_ngx.h',
    'nvsdk_ngx_defs.h',
    'nvsdk_ngx_params.h',
    'nvsdk_ngx_helpers.h',
    'nvsdk_ngx_helpers_dlssd.h',
    'nvsdk_ngx_helpers_vk.h'
)

$baseUri = "https://raw.githubusercontent.com/NVIDIA/DLSS/$Ref/include"

if (-not (Test-Path $Destination)) {
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
}

Write-Host "Fetching NGX headers (ref: $Ref)"
Write-Host "  -> $Destination"

foreach ($name in $files) {
    $target = Join-Path $Destination $name
    if ((Test-Path $target) -and -not $Force) {
        Write-Host "  skip  $name (already present, use -Force to refetch)"
        continue
    }

    $uri = "$baseUri/$name"
    try {
        Invoke-WebRequest -Uri $uri -OutFile $target -UseBasicParsing
    }
    catch {
        throw "Failed to download $uri : $($_.Exception.Message)"
    }

    $size = (Get-Item $target).Length
    if ($size -lt 256) {
        Remove-Item $target -Force
        throw "$name came back as $size bytes, which looks like an error page rather than a header"
    }
    Write-Host "  ok    $name ($size bytes)"
}

# nvsdk_ngx_defs.h is the one the backend depends on most heavily; fail loudly if
# the version macro moved, because that value goes straight into Init_Ext.
$defs = Join-Path $Destination 'nvsdk_ngx_defs.h'
if (Test-Path $defs) {
    $hit = Select-String -Path $defs -Pattern 'NVSDK_NGX_VERSION_API_MACRO\s+(0x[0-9A-Fa-f]+)' |
        Select-Object -First 1
    if ($hit) {
        Write-Host "  NVSDK_NGX_VERSION_API_MACRO = $($hit.Matches[0].Groups[1].Value)"
    }
    else {
        Write-Warning 'NVSDK_NGX_VERSION_API_MACRO not found in nvsdk_ngx_defs.h - upstream layout may have changed'
    }
}

Write-Host 'Done.'
