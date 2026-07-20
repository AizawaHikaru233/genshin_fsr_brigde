[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release',
    [switch]$GithubLiteOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSCommandPath))
$source = Join-Path $root 'GenshinOneClick'
$fufuSource = Join-Path $root 'FufuGraphicsPlugin'
$fufuBuild = Join-Path $root 'build-package-fufu-graphics'
$fufuDll = Join-Path $fufuBuild "$Configuration\FSR-Bridge-Plugin.dll"
$dist = Join-Path $root 'dist'
$manifestPath = Join-Path $source 'component-manifest.json'
$optiRuntime = Join-Path $root 'SharedResources\OptiScaler\runtime'
$optiDefaults = Join-Path $root 'SharedResources\OptiScaler\default_config'
$reshadeDefaults = Join-Path $root 'SharedResources\ReShade\default_config'
$bridgeDefaults = Join-Path $root 'SharedResources\Bridge\default_config'

function Get-BridgeVersion {
    $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $bridge = @($manifest | Where-Object { $_.Name -eq 'Dx11FsrBridge.dll' } | Select-Object -First 1)
    if ($bridge.Count -ne 1 -or [string]$bridge[0].Version -notmatch '^\d+\.\d+\.\d+$') {
        throw '组件清单缺少有效的 Dx11FsrBridge.dll 版本。'
    }
    return [string]$bridge[0].Version
}

function Invoke-Cmake {
    param([string[]]$Arguments)
    $cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
    $cmakePath = if ($null -ne $cmake) { $cmake.Path } else { $null }
    if ($null -eq $cmakePath) {
        $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
            $visualStudioPath = & $vswhere -latest -products * -property installationPath
            $candidate = Join-Path $visualStudioPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { $cmakePath = $candidate }
        }
    }
    if ($null -eq $cmakePath) { throw '没有找到 CMake。' }
    & $cmakePath @Arguments | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "CMake 命令失败: $($Arguments -join ' ')" }
}

function Reset-Stage {
    param([string]$Path)
    $distRoot = [IO.Path]::GetFullPath($dist).TrimEnd('\') + '\'
    $fullPath = [IO.Path]::GetFullPath($Path)
    if (-not $fullPath.StartsWith($distRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "构建目录不在 dist 下: $fullPath"
    }
    if (Test-Path -LiteralPath $fullPath) { Remove-Item -LiteralPath $fullPath -Recurse -Force }
    New-Item -ItemType Directory -Path $fullPath -Force | Out-Null
}

function Copy-DirectoryContents {
    param([string]$Source, [string]$Destination)
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    Get-ChildItem -LiteralPath $Source -Force | Copy-Item -Destination $Destination -Recurse -Force
}

function Remove-NonBundledReShadeEffects {
    param([string]$ReShadeDirectory)
    $shaderRoot = Join-Path $ReShadeDirectory 'reshade-shaders'
    foreach ($name in @('Shaders', 'Textures')) {
        $path = Join-Path $shaderRoot $name
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
        New-Item -ItemType Directory -Path $path -Force | Out-Null
    }
    foreach ($pattern in @('LICENSE-ReShade_HDR_shaders-*', 'NOTICE-ReShade_HDR_shaders.txt', 'LICENSE-SweetFX-*', 'NOTICE-Downloaded-Upstream-Sources.txt')) {
        Get-ChildItem -LiteralPath $shaderRoot -File -Filter $pattern -ErrorAction SilentlyContinue | Remove-Item -Force
    }
}

function Assert-RequiredFiles {
    param([string]$Path, [string[]]$RelativePaths)
    foreach ($relativePath in $RelativePaths) {
        if (-not (Test-Path -LiteralPath (Join-Path $Path $relativePath) -PathType Leaf)) {
            throw "商城包缺少文件: $relativePath"
        }
    }
}

function Assert-CleanPackage {
    param([string]$Path)
    $forbiddenNames = @(
        'amd_fidelityfx_framegeneration_dx12.dll', 'amd_fidelityfx_vk.dll',
        'dlssg_to_fsr3_amd_is_better.dll', 'fakenvapi.dll', 'fakenvapi.ini', 'libxess_fg.dll',
        'nvngx_dlssg.dll', 'nvngx_dlssd.dll'
    )
    foreach ($file in Get-ChildItem -LiteralPath $Path -Recurse -File -Force) {
        if ($file.Name -in $forbiddenNames) { throw "商城包包含禁止文件: $($file.FullName)" }
        if ($file.Extension -in @('.log', '.dmp', '.bak')) { throw "商城包包含运行残留: $($file.FullName)" }
    }
}

function New-ZipArchive {
    param([string]$SourceDirectory, [string]$ArchivePath)
    if (Test-Path -LiteralPath $ArchivePath) { Remove-Item -LiteralPath $ArchivePath -Force }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory(
        $SourceDirectory,
        $ArchivePath,
        [IO.Compression.CompressionLevel]::Optimal,
        $false)
}

function Prepare-FpsStage {
    param([string]$Stage, [switch]$Full, [Parameter(Mandatory)][string]$Version)
    Reset-Stage -Path $Stage
    $rootExcludes = @(
        'fps_config.json', '.installer-state.json', '.last-install-error.log', 'backups',
        'payload', 'README.md', 'Configure-Launcher.bat', 'Configure-Launcher.en.bat',
        'Configure.ps1', 'Localization.ps1', 'ReShadeResources.ps1', 'Apply-PackageUpdate.ps1',
        'unlockfps_nc.exe', 'UnlockerStub.dll'
    )
    Get-ChildItem -LiteralPath $source -Force |
        Where-Object { $_.Name -notin $rootExcludes } |
        Copy-Item -Destination $Stage -Recurse -Force

    Copy-Item -LiteralPath (Join-Path $source 'Configure-Launcher.bat') -Destination (Join-Path $Stage '一键配置.bat') -Force
    Copy-Item -LiteralPath (Join-Path $source 'Configure-Launcher.en.bat') -Destination (Join-Path $Stage 'GenshinFSRBridgeTools.bat') -Force
    $stageScripts = Join-Path $Stage 'scripts'
    New-Item -ItemType Directory -Path $stageScripts -Force | Out-Null
    foreach ($scriptName in @('Configure.ps1', 'Localization.ps1', 'ReShadeResources.ps1', 'Apply-PackageUpdate.ps1')) {
        Copy-Item -LiteralPath (Join-Path $source $scriptName) -Destination (Join-Path $stageScripts $scriptName) -Force
    }
    Copy-Item -LiteralPath (Join-Path $source 'Feedback.txt') -Destination $Stage -Force
    [IO.File]::WriteAllText((Join-Path $Stage 'Package-Version.txt'), "$Version`r`n", [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $Stage 'NonFrameGeneration.edition'), "`r`n", [Text.UTF8Encoding]::new($false))

    if ($Full) {
        Copy-Item -LiteralPath (Join-Path $source 'unlockfps_nc.exe') -Destination $Stage -Force
        Copy-Item -LiteralPath (Join-Path $source 'UnlockerStub.dll') -Destination $Stage -Force
    }

    $payload = Join-Path $Stage 'payload'
    $stagePayloadBridge = Join-Path $payload 'Bridge'
    $stagePayloadAnti = Join-Path $payload 'AntiPlayerMosaic'
    $stagePayloadReShade = Join-Path $payload 'ReShade'
    $stageDefaults = Join-Path $payload 'default_config'
    New-Item -ItemType Directory -Path $payload, $stagePayloadBridge, $stagePayloadAnti, $stagePayloadReShade, $stageDefaults -Force | Out-Null
    Copy-DirectoryContents -Source (Join-Path $source 'payload\Bridge') -Destination $stagePayloadBridge
    Copy-DirectoryContents -Source (Join-Path $source 'payload\AntiPlayerMosaic') -Destination $stagePayloadAnti
    Copy-DirectoryContents -Source (Join-Path $source 'payload\ReShade') -Destination $stagePayloadReShade
    Remove-NonBundledReShadeEffects -ReShadeDirectory $stagePayloadReShade
    Remove-Item -LiteralPath (Join-Path $stagePayloadReShade 'ReShade.ini'), (Join-Path $stagePayloadReShade 'ReShadePreset.ini') -Force -ErrorAction SilentlyContinue
    Copy-DirectoryContents -Source $optiDefaults -Destination $stageDefaults
    Copy-DirectoryContents -Source $reshadeDefaults -Destination $stageDefaults
    Copy-DirectoryContents -Source $bridgeDefaults -Destination $stageDefaults

    if ($Full) {
        $stageOpti = Join-Path $payload 'OptiScaler'
        $stageNvidia = Join-Path $payload 'NVIDIA\DLSS'
        New-Item -ItemType Directory -Path $stageOpti, $stageNvidia -Force | Out-Null
        Copy-DirectoryContents -Source $optiRuntime -Destination $stageOpti
        Copy-Item -LiteralPath (Join-Path $source 'payload\NVIDIA\DLSS\nvngx_dlss.dll') -Destination $stageNvidia -Force
        Copy-Item -LiteralPath (Join-Path $source 'payload\NVIDIA\DLSS\nvngx_dlss.license.txt') -Destination $stageNvidia -Force
    }
}

function Build-FpsPackage {
    param([string]$PackageKind, [Parameter(Mandatory)][string]$Version)
    $full = $PackageKind -eq 'Full'
    $stageName = if ($full) { '.fps-full-stage' } else { '.fps-lite-stage' }
    $stage = Join-Path $dist $stageName
    Prepare-FpsStage -Stage $stage -Full:$full -Version $Version
    try {
        $required = @(
            '一键配置.bat', 'GenshinFSRBridgeTools.bat', 'scripts\Configure.ps1', 'scripts\ReShadeResources.ps1',
            'scripts\Apply-PackageUpdate.ps1', 'scripts\Localization.ps1', 'component-manifest.json',
            'Feedback.txt', 'Package-Version.txt', 'NonFrameGeneration.edition',
            'payload\Bridge\Dx11FsrBridge.dll', 'payload\Bridge\Dx11FsrBridge.ini',
            'payload\AntiPlayerMosaic\AntiPlayerMosaic.dll', 'payload\ReShade\ReShade64.dll',
            'payload\ReShade\reshade-shaders\Addons\renodx-genshin.addon64',
            'payload\ReShade\reshade-shaders\NOTICE-RenoDX-genshin.txt',
            'payload\ReShade\reshade-shaders\NOTICE-RenoDX-genshin-permission.png',
            'payload\default_config\Dx11FsrBridge.ini', 'payload\default_config\OptiScaler.ini',
            'payload\default_config\OptiScaler-UpscalingFiles.json', 'payload\default_config\ReShade.ini',
            'payload\default_config\ReShadePreset.ini'
        )
        if ($full) {
            $required += @(
                'unlockfps_nc.exe', 'UnlockerStub.dll', 'payload\OptiScaler\OptiScaler.dll',
                'payload\OptiScaler\amd_fidelityfx_dx12.dll', 'payload\OptiScaler\amd_fidelityfx_upscaler_dx12.dll',
                'payload\OptiScaler\libxell.dll', 'payload\OptiScaler\libxess.dll',
                'payload\OptiScaler\libxess_dx11.dll', 'payload\OptiScaler\D3D12_Optiscaler\D3D12Core.dll',
                'payload\NVIDIA\DLSS\nvngx_dlss.dll', 'payload\NVIDIA\DLSS\nvngx_dlss.license.txt'
            )
        }
        Assert-RequiredFiles -Path $stage -RelativePaths $required
        Assert-CleanPackage -Path $stage
        $name = if ($full) { "原神解帧FSR插件包Full_v$Version.zip" } else { "原神解帧FSR插件包Lite_v$Version.zip" }
        $archive = Join-Path $dist $name
        New-ZipArchive -SourceDirectory $stage -ArchivePath $archive
        return $archive
    }
    finally {
        if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
    }
}

function Build-FufuMarketplacePackage {
param([Parameter(Mandatory)][string]$Version)
$version = $Version
New-Item -ItemType Directory -Path $dist -Force | Out-Null
Get-ChildItem -LiteralPath $dist -File -Filter 'FSR-Bridge-Plugin.v*.zip' -ErrorAction SilentlyContinue | Remove-Item -Force

Invoke-Cmake @('-S', $fufuSource, '-B', $fufuBuild, '-A', 'x64')
Invoke-Cmake @('--build', $fufuBuild, '--config', $Configuration)
if (-not (Test-Path -LiteralPath $fufuDll -PathType Leaf)) { throw "插件编译输出不存在: $fufuDll" }
if ((Get-Item -LiteralPath $fufuDll).VersionInfo.FileVersion -ne "$version.0") {
    throw "FSR-Bridge-Plugin.dll 版本未同步：实际 $((Get-Item $fufuDll).VersionInfo.FileVersion)，期望 $version.0"
}

$stage = Join-Path $dist '.fufu-marketplace-stage'
Reset-Stage -Path $stage
try {
    Copy-Item -LiteralPath $fufuDll -Destination (Join-Path $stage 'FSR-Bridge-Plugin.dll') -Force
    Copy-Item -LiteralPath (Join-Path $source 'Feedback.txt') -Destination $stage -Force
    [IO.File]::WriteAllText((Join-Path $stage 'Package-Version.txt'), "$version`r`n", [Text.UTF8Encoding]::new($false))

    $config = Get-Content -LiteralPath (Join-Path $fufuSource 'config.ini') -Raw -Encoding UTF8
    $config = ([regex]'(?m)^Name\s*=.*$').Replace($config, 'Name = 原神FSR2桥接插件', 1)
    $config = ([regex]'(?m)^Developer\s*=.*$').Replace($config, 'Developer = シリアCelia', 1)
    $config = ([regex]'(?m)^Version\s*=.*$').Replace($config, "Version = $version", 1)
    [IO.File]::WriteAllText((Join-Path $stage 'config.ini'), $config, [Text.UTF8Encoding]::new($false))

    $payload = Join-Path $stage 'payload'
    $bridge = Join-Path $payload 'Bridge'
    $opti = Join-Path $payload 'OptiScaler'
    $nvidia = Join-Path $payload 'NVIDIA\DLSS'
    $reshade = Join-Path $payload 'ReShade'
    $defaults = Join-Path $payload 'default_config'

    New-Item -ItemType Directory -Path $bridge, $opti, $nvidia, $reshade, $defaults -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $source 'payload\Bridge\Dx11FsrBridge.dll') -Destination $bridge -Force
    Copy-Item -LiteralPath (Join-Path $source 'payload\Bridge\Dx11FsrBridge.ini') -Destination $bridge -Force
    Copy-DirectoryContents -Source $optiRuntime -Destination $opti
    Copy-Item -LiteralPath (Join-Path $source 'payload\NVIDIA\DLSS\nvngx_dlss.dll') -Destination $nvidia -Force
    Copy-Item -LiteralPath (Join-Path $source 'payload\NVIDIA\DLSS\nvngx_dlss.license.txt') -Destination $nvidia -Force
    Copy-DirectoryContents -Source (Join-Path $source 'payload\ReShade') -Destination $reshade
    Remove-NonBundledReShadeEffects -ReShadeDirectory $reshade
    Copy-DirectoryContents -Source $optiDefaults -Destination $defaults
    Copy-DirectoryContents -Source $reshadeDefaults -Destination $defaults
    Copy-DirectoryContents -Source $bridgeDefaults -Destination $defaults
    Remove-Item -LiteralPath (Join-Path $reshade 'ReShade.ini'), (Join-Path $reshade 'ReShadePreset.ini') -Force -ErrorAction SilentlyContinue

    Assert-RequiredFiles -Path $stage -RelativePaths @(
        'FSR-Bridge-Plugin.dll', 'config.ini', 'Feedback.txt', 'Package-Version.txt',
        'payload\Bridge\Dx11FsrBridge.dll', 'payload\Bridge\Dx11FsrBridge.ini',
        'payload\OptiScaler\OptiScaler.dll', 'payload\OptiScaler\amd_fidelityfx_dx12.dll',
        'payload\OptiScaler\amd_fidelityfx_upscaler_dx12.dll', 'payload\OptiScaler\libxell.dll',
        'payload\OptiScaler\libxess.dll', 'payload\OptiScaler\libxess_dx11.dll',
        'payload\OptiScaler\D3D12_Optiscaler\D3D12Core.dll',
        'payload\NVIDIA\DLSS\nvngx_dlss.dll', 'payload\NVIDIA\DLSS\nvngx_dlss.license.txt',
        'payload\ReShade\ReShade64.dll', 'payload\ReShade\reshade-shaders\Addons\renodx-genshin.addon64',
        'payload\ReShade\reshade-shaders\NOTICE-RenoDX-genshin.txt',
        'payload\ReShade\reshade-shaders\NOTICE-RenoDX-genshin-permission.png',
        'payload\default_config\Dx11FsrBridge.ini', 'payload\default_config\OptiScaler.ini',
        'payload\default_config\OptiScaler-UpscalingFiles.json', 'payload\default_config\ReShade.ini',
        'payload\default_config\ReShadePreset.ini'
    )
    Assert-CleanPackage -Path $stage

    $archive = Join-Path $dist "FSR-Bridge-Plugin.v$version.zip"
    New-ZipArchive -SourceDirectory $stage -ArchivePath $archive
    $item = Get-Item -LiteralPath $archive
    Write-Host ''
    Write-Host 'FufuLauncher 官方商城完整包构建完成。' -ForegroundColor Green
    Write-Host "$($item.Name)  $($item.Length) bytes  SHA256=$((Get-FileHash $archive -Algorithm SHA256).Hash)"
}
finally {
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
}

return (Join-Path $dist "FSR-Bridge-Plugin.v$version.zip")
}

$version = Get-BridgeVersion
New-Item -ItemType Directory -Path $dist -Force | Out-Null
foreach ($pattern in @(
    '原神解帧FSR插件包Lite_*.zip',
    '原神解帧FSR插件包Full_*.zip',
    '芙芙启动器插件包Lite_*.zip',
    '芙芙启动器插件包Full_*.zip',
    'FSR-Bridge-Plugin.v*.zip'
)) {
    Get-ChildItem -LiteralPath $dist -File -Filter $pattern -ErrorAction SilentlyContinue | Remove-Item -Force
}

$fpsLiteArchive = Build-FpsPackage -PackageKind Lite -Version $version
$fpsFullArchive = $null
$fufuArchive = $null
if (-not $GithubLiteOnly) {
    $fpsFullArchive = Build-FpsPackage -PackageKind Full -Version $version
    $fufuArchive = Build-FufuMarketplacePackage -Version $version
}

$githubReleaseDist = Join-Path $dist 'github-release'
if (Test-Path -LiteralPath $githubReleaseDist) { Remove-Item -LiteralPath $githubReleaseDist -Recurse -Force }
New-Item -ItemType Directory -Path $githubReleaseDist -Force | Out-Null
$githubLiteArchive = Join-Path $githubReleaseDist "GenshinFSRBridge.Lite_v$version.zip"
Copy-Item -LiteralPath $fpsLiteArchive -Destination $githubLiteArchive -Force

Write-Host ''
if ($GithubLiteOnly) {
    Write-Host 'GitHub FPS Unlock Lite 包构建完成。' -ForegroundColor Green
}
else {
    Write-Host 'FPS Unlock Lite、Full 与 FufuLauncher 完整包构建完成。' -ForegroundColor Green
}
foreach ($archive in @($fpsLiteArchive, $fpsFullArchive, $fufuArchive, $githubLiteArchive) | Where-Object { $null -ne $_ }) {
    $item = Get-Item -LiteralPath $archive
    Write-Host "$($item.FullName)  $($item.Length) bytes  SHA256=$((Get-FileHash $archive -Algorithm SHA256).Hash)"
}
