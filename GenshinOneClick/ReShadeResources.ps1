Set-StrictMode -Version Latest

function Get-ReShadeResourceSpec {
    return [pscustomobject]@{
        ReShadeVersion = '6.7.3'
        ReShadeSetupUrl = 'https://reshade.me/downloads/ReShade_Setup_6.7.3_Addon.exe'
        ReShadeSetupSha256 = 'C78DB69BD127E98054BD496FB422655F4A1CC664E28F8D12CE9835B2647BC571'
        ReShadeDllSha256 = 'EC9245D05C11751F2AC0D2256E6921AD8FB36BE9172EF6D587856591EB729A25'
        StandardCommit = '6db142b4b1a05c764222e5b0bd9a644b7ccfe1dc'
        StandardUrl = 'https://github.com/crosire/reshade-shaders/archive/6db142b4b1a05c764222e5b0bd9a644b7ccfe1dc.zip'
        StandardSha256 = '12D082C8AB1DBCB5E221E1B6116A0343F3182EE517F09BB966B117ACC7635312'
        LiliumCommit = '5093d4f7441d8ca793d4a04496d1b78f640418e6'
        LiliumUrl = 'https://github.com/EndlesslyFlowering/ReShade_HDR_shaders/archive/5093d4f7441d8ca793d4a04496d1b78f640418e6.zip'
        LiliumSha256 = 'F8972F060A35BA4EFC4F48F8CE7283F8F3B92DD8BFE421C154161539109FE016'
        SweetFxCommit = '16d1a42247cb5baaf660120ee35c9a33bb94649c'
        SweetFxUrl = 'https://github.com/CeeJayDK/SweetFX/archive/16d1a42247cb5baaf660120ee35c9a33bb94649c.zip'
        SweetFxSha256 = '7901037254B06B85E564F5B8774F2F59BF2503143CE1562F9AC704A3F3D74EC6'
    }
}

function Assert-ReShadeResourceHash {
    param([string]$Path, [string]$ExpectedSha256, [string]$Label)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing file: $Label ($Path)" }
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    if (-not [string]::Equals($actual, $ExpectedSha256, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label SHA-256 mismatch. Actual: $actual"
    }
}

function Invoke-ReShadeResourceDownload {
    param([string]$Url, [string]$Destination, [string]$UserAgent)
    try {
        Invoke-WebRequest -UseBasicParsing -Headers @{ 'User-Agent' = $UserAgent } -Uri $Url -OutFile $Destination
    }
    catch {
        throw "Failed to download an official ReShade resource: $Url$([Environment]::NewLine)$($_.Exception.Message)"
    }
}

function Expand-ReShadeSetupModule {
    param([string]$SetupPath, [string]$Destination)
    $bytes = [IO.File]::ReadAllBytes($SetupPath)
    $archiveOffset = -1
    for ($offset = 0; $offset -le $bytes.Length - 30; $offset += 512) {
        if ($bytes[$offset] -eq 0x50 -and $bytes[$offset + 1] -eq 0x4B -and
            $bytes[$offset + 2] -eq 0x03 -and $bytes[$offset + 3] -eq 0x04) {
            $archiveOffset = $offset
            break
        }
    }
    if ($archiveOffset -lt 0) { throw 'The embedded ZIP was not found in the official ReShade setup.' }

    Add-Type -AssemblyName System.IO.Compression
    $memory = [IO.MemoryStream]::new($bytes, $archiveOffset, $bytes.Length - $archiveOffset, $false)
    $archive = [IO.Compression.ZipArchive]::new($memory, [IO.Compression.ZipArchiveMode]::Read, $false)
    try {
        $entry = $archive.GetEntry('ReShade64.dll')
        if ($null -eq $entry) { throw 'ReShade64.dll was not found in the official ReShade setup.' }
        $source = $entry.Open()
        $target = [IO.File]::Create($Destination)
        try { $source.CopyTo($target) }
        finally {
            $target.Dispose()
            $source.Dispose()
        }
    }
    finally {
        $archive.Dispose()
        $memory.Dispose()
    }
}

function Get-ReShadeArchiveRoot {
    param([string]$ExpandedDirectory, [string]$Label)
    $roots = @(Get-ChildItem -LiteralPath $ExpandedDirectory -Directory -Force)
    if ($roots.Count -ne 1) { throw "$Label has an unexpected archive layout." }
    return $roots[0].FullName
}

function Copy-ReShadeDirectoryContents {
    param([string]$Source, [string]$Destination)
    if (-not (Test-Path -LiteralPath $Source -PathType Container)) { throw "Missing upstream directory: $Source" }
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    Get-ChildItem -LiteralPath $Source -Force | Copy-Item -Destination $Destination -Recurse -Force
}

function New-OfficialReShadePayload {
    param(
        [string]$DestinationDirectory,
        [string]$BundledSourceDirectory,
        [string]$TemporaryDirectory,
        [string]$UserAgent = 'GenshinFsrBridge-ReShadeInstaller'
    )
    $spec = Get-ReShadeResourceSpec
    if (Test-Path -LiteralPath $DestinationDirectory) {
        Remove-Item -LiteralPath $DestinationDirectory -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $DestinationDirectory | Out-Null
    $shaderRoot = Join-Path $DestinationDirectory 'reshade-shaders'
    $shaderDestination = Join-Path $shaderRoot 'Shaders'
    $textureDestination = Join-Path $shaderRoot 'Textures'
    $addonDestination = Join-Path $shaderRoot 'Addons'
    New-Item -ItemType Directory -Force -Path $shaderDestination, $textureDestination, $addonDestination | Out-Null

    $downloads = @(
        [pscustomobject]@{ Name = 'ReShade_Setup_6.7.3_Addon.exe'; Url = $spec.ReShadeSetupUrl; Hash = $spec.ReShadeSetupSha256; Label = 'ReShade 6.7.3 Add-on setup' },
        [pscustomobject]@{ Name = 'reshade-shaders.zip'; Url = $spec.StandardUrl; Hash = $spec.StandardSha256; Label = 'ReShade standard effects' },
        [pscustomobject]@{ Name = 'lilium-hdr.zip'; Url = $spec.LiliumUrl; Hash = $spec.LiliumSha256; Label = 'Lilium HDR shaders' },
        [pscustomobject]@{ Name = 'sweetfx.zip'; Url = $spec.SweetFxUrl; Hash = $spec.SweetFxSha256; Label = 'SweetFX shaders' }
    )
    foreach ($download in $downloads) {
        $path = Join-Path $TemporaryDirectory $download.Name
        Write-Host "Downloading $($download.Label)..." -ForegroundColor Cyan
        Invoke-ReShadeResourceDownload -Url $download.Url -Destination $path -UserAgent $UserAgent
        Assert-ReShadeResourceHash -Path $path -ExpectedSha256 $download.Hash -Label $download.Label
    }

    $reshadeDll = Join-Path $DestinationDirectory 'ReShade64.dll'
    Expand-ReShadeSetupModule -SetupPath (Join-Path $TemporaryDirectory 'ReShade_Setup_6.7.3_Addon.exe') -Destination $reshadeDll
    Assert-ReShadeResourceHash -Path $reshadeDll -ExpectedSha256 $spec.ReShadeDllSha256 -Label 'ReShade64.dll'

    $standardExpanded = Join-Path $TemporaryDirectory 'standard-expanded'
    $liliumExpanded = Join-Path $TemporaryDirectory 'lilium-expanded'
    $sweetFxExpanded = Join-Path $TemporaryDirectory 'sweetfx-expanded'
    Expand-Archive -LiteralPath (Join-Path $TemporaryDirectory 'reshade-shaders.zip') -DestinationPath $standardExpanded -Force
    Expand-Archive -LiteralPath (Join-Path $TemporaryDirectory 'lilium-hdr.zip') -DestinationPath $liliumExpanded -Force
    Expand-Archive -LiteralPath (Join-Path $TemporaryDirectory 'sweetfx.zip') -DestinationPath $sweetFxExpanded -Force
    $standardRoot = Get-ReShadeArchiveRoot -ExpandedDirectory $standardExpanded -Label 'ReShade standard effects'
    $liliumRoot = Get-ReShadeArchiveRoot -ExpandedDirectory $liliumExpanded -Label 'Lilium HDR shaders'
    $sweetFxRoot = Get-ReShadeArchiveRoot -ExpandedDirectory $sweetFxExpanded -Label 'SweetFX shaders'

    foreach ($name in @('ReShade.fxh', 'ReShadeUI.fxh')) {
        Copy-Item -LiteralPath (Join-Path $standardRoot "Shaders\$name") -Destination (Join-Path $shaderDestination $name) -Force
    }
    foreach ($name in @('FontAtlas.png', 'lut.png')) {
        Copy-Item -LiteralPath (Join-Path $standardRoot "Textures\$name") -Destination (Join-Path $textureDestination $name) -Force
    }
    Copy-ReShadeDirectoryContents -Source (Join-Path $liliumRoot 'Shaders') -Destination $shaderDestination
    Copy-ReShadeDirectoryContents -Source (Join-Path $liliumRoot 'Textures') -Destination $textureDestination
    Copy-Item -LiteralPath (Join-Path $sweetFxRoot 'Shaders\SweetFX\FakeHDR.fx') -Destination (Join-Path $shaderDestination 'FakeHDR.fx') -Force
    foreach ($name in @('AreaTex.png', 'Layer.png', 'SearchTex.png')) {
        Copy-Item -LiteralPath (Join-Path $sweetFxRoot "Textures\SweetFX\$name") -Destination (Join-Path $textureDestination $name) -Force
    }

    Copy-Item -LiteralPath (Join-Path $liliumRoot 'LICENSE') -Destination (Join-Path $shaderRoot 'LICENSE-ReShade_HDR_shaders-GPL-3.0.txt') -Force
    Copy-Item -LiteralPath (Join-Path $sweetFxRoot 'LICENSE') -Destination (Join-Path $shaderRoot 'LICENSE-SweetFX-MIT.txt') -Force

    if (-not [string]::IsNullOrWhiteSpace($BundledSourceDirectory) -and
        (Test-Path -LiteralPath $BundledSourceDirectory -PathType Container)) {
        foreach ($rootFile in @('LICENSE-ReShade-BSD-3-Clause.txt')) {
            $source = Join-Path $BundledSourceDirectory $rootFile
            if (Test-Path -LiteralPath $source -PathType Leaf) {
                Copy-Item -LiteralPath $source -Destination (Join-Path $DestinationDirectory $rootFile) -Force
            }
        }
        $bundledShaderRoot = Join-Path $BundledSourceDirectory 'reshade-shaders'
        $bundledAddons = Join-Path $bundledShaderRoot 'Addons'
        if (Test-Path -LiteralPath $bundledAddons -PathType Container) {
            Copy-ReShadeDirectoryContents -Source $bundledAddons -Destination $addonDestination
        }
        foreach ($pattern in @('NOTICE-RenoDX-genshin*', 'NOTICE-ReShade_HDR_shaders.txt')) {
            Get-ChildItem -LiteralPath $bundledShaderRoot -File -Filter $pattern -ErrorAction SilentlyContinue |
                Copy-Item -Destination $shaderRoot -Force
        }
    }

    $provenance = @(
        'Downloaded directly from the following upstream sources by the installer:',
        "ReShade $($spec.ReShadeVersion): $($spec.ReShadeSetupUrl)",
        "crosire/reshade-shaders commit $($spec.StandardCommit): $($spec.StandardUrl)",
        "EndlesslyFlowering/ReShade_HDR_shaders commit $($spec.LiliumCommit): $($spec.LiliumUrl)",
        "CeeJayDK/SweetFX commit $($spec.SweetFxCommit): $($spec.SweetFxUrl)"
    )
    [IO.File]::WriteAllLines(
        (Join-Path $shaderRoot 'NOTICE-Downloaded-Upstream-Sources.txt'),
        $provenance,
        [Text.UTF8Encoding]::new($false))

    foreach ($required in @(
        $reshadeDll,
        (Join-Path $shaderDestination 'ReShade.fxh'),
        (Join-Path $shaderDestination 'ReShadeUI.fxh'),
        (Join-Path $shaderDestination 'FakeHDR.fx'),
        (Join-Path $shaderDestination 'lilium__tone_mapping.fx'),
        (Join-Path $textureDestination 'lilium__font_atlas.png')
    )) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "The official ReShade resources are incomplete: $required" }
    }
    return $DestinationDirectory
}
