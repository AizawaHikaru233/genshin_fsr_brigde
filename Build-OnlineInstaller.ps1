[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release',
    [switch]$GithubLiteOnly,
    [string]$SevenZipPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSCommandPath))
$installerSource = Join-Path $root 'tools\FpsUnlockInstaller'
$packageAssets = Join-Path $root 'assets\FpsUnlockPackage'
$fufuSource = Join-Path $root 'FufuGraphicsPlugin'
$fufuBuild = Join-Path $root 'build-package-fufu-graphics'
$fufuDll = Join-Path $fufuBuild "$Configuration\FSR-Bridge-Plugin.dll"
$dist = Join-Path $root 'dist'
$optiRuntime = Join-Path $root 'SharedResources\OptiScaler\runtime'
$dlssRuntime = Join-Path $root 'SharedResources\NVIDIA\DLSS'
$unlockerRuntime = Join-Path $root 'SharedResources\FpsUnlocker\runtime'
$reshadeRuntime = Join-Path $root 'SharedResources\ReShade\runtime'
$bridgePackageConfig = Join-Path $root 'Dx11FsrBridge\Dx11FsrBridge.package.ini'
$bridgeBuild = Join-Path $root 'build-package-bridge'
$antiBuild = Join-Path $root 'build-package-antiplayermosaic'
$script:bridgeDll = $null
$script:antiDll = $null

function Get-BridgeVersion {
    $version = [string](Get-Item -LiteralPath $bridgeDll).VersionInfo.FileVersion
    if ($version -notmatch '^(\d+\.\d+\.\d+)') {
        throw "编译产物 Dx11FsrBridge.dll 缺少有效文件版本：$version"
    }
    return $matches[1]
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

function Build-PackageComponents {
    Invoke-Cmake @(
        '-S', (Join-Path $root 'Dx11FsrBridge'), '-B', $bridgeBuild, '-A', 'x64',
        "-DCMAKE_BUILD_TYPE=$Configuration",
        '-DDX11FSRBRIDGE_RELEASE_RUNTIME=ON',
        '-DDX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL=ON'
    )
    Invoke-Cmake @('--build', $bridgeBuild, '--config', $Configuration)
    Invoke-Cmake @('-S', (Join-Path $root 'AntiPlayerMosaic'), '-B', $antiBuild, '-A', 'x64', "-DCMAKE_BUILD_TYPE=$Configuration")
    Invoke-Cmake @('--build', $antiBuild, '--config', $Configuration)

    $script:bridgeDll = Get-ChildItem -LiteralPath $bridgeBuild -Recurse -File -Filter 'Dx11FsrBridge.dll' |
        Select-Object -First 1 -ExpandProperty FullName
    $script:antiDll = Get-ChildItem -LiteralPath $antiBuild -Recurse -File -Filter 'AntiPlayerMosaic.dll' |
        Select-Object -First 1 -ExpandProperty FullName
    if ([string]::IsNullOrWhiteSpace($script:bridgeDll) -or [string]::IsNullOrWhiteSpace($script:antiDll)) {
        throw 'CMake 未生成必要的 FPS Unlock 包 DLL。'
    }
}

function Assert-OptiConfigMatchesRuntime {
    $configPath = Join-Path $optiRuntime 'OptiScaler.ini'
    $runtimePath = Join-Path $optiRuntime 'OptiScaler.dll'
    if (-not (Test-Path -LiteralPath $runtimePath -PathType Leaf)) {
        Write-Host 'OptiScaler.dll 未随仓库提供（Lite 包不内置，安装时从上游下载），跳过 OptiScaler 运行时版本校验。'
        return
    }
    $config = Get-Content -LiteralPath $configPath -Raw -Encoding UTF8
    $marker = [regex]::Match($config, '(?m)^;\s*FSR Bridge OptiScalerRuntimeVersion\s*=\s*([^\r\n;]+)\s*$')
    if (-not $marker.Success) {
        throw 'OptiScaler.ini 缺少 FSR Bridge OptiScalerRuntimeVersion 标记。更新 OptiScaler 时必须同步配置模板。'
    }
    $expectedVersion = $marker.Groups[1].Value.Trim()
    $actualVersion = [string](Get-Item -LiteralPath $runtimePath).VersionInfo.FileVersion
    if (-not [string]::Equals($expectedVersion, $actualVersion, [StringComparison]::OrdinalIgnoreCase)) {
        throw "OptiScaler.ini 适配版本为 $expectedVersion，但当前 OptiScaler.dll 为 $actualVersion。请同步更新配置模板。"
    }
}

function New-PackageComponentManifest {
    param(
        [Parameter(Mandatory)][string]$PackageRoot,
        [switch]$Full
    )

    $componentPaths = [ordered]@{
        'Dx11FsrBridge.dll' = 'payload\Bridge\Dx11FsrBridge.dll'
        'AntiPlayerMosaic.dll' = 'payload\AntiPlayerMosaic\AntiPlayerMosaic.dll'
        'ReShade64.dll' = 'payload\ReShade\ReShade64.dll'
    }
    if ($Full) { $componentPaths['OptiScaler.dll'] = 'payload\OptiScaler\OptiScaler.dll' }

    $manifest = foreach ($name in $componentPaths.Keys) {
        $relativePath = $componentPaths[$name]
        $item = Get-Item -LiteralPath (Join-Path $PackageRoot $relativePath)
        $fileVersion = [string]$item.VersionInfo.FileVersion
        if ([string]::IsNullOrWhiteSpace($fileVersion)) { $fileVersion = 'unknown' }
        [pscustomobject]@{
            Name = $name
            Version = $fileVersion
            SHA256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash
            Bytes = $item.Length
        }
    }
    $manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $PackageRoot 'component-manifest.json') -Encoding UTF8
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

function Find-SevenZip {
    if (-not [string]::IsNullOrWhiteSpace($SevenZipPath)) {
        if (-not (Test-Path -LiteralPath $SevenZipPath -PathType Leaf)) {
            throw "指定的 7-Zip 路径不存在: $SevenZipPath"
        }
        return (Resolve-Path -LiteralPath $SevenZipPath).Path
    }

    foreach ($commandName in @('7z.exe', '7zz.exe', '7za.exe')) {
        $command = Get-Command $commandName -ErrorAction SilentlyContinue
        if ($null -ne $command) { return $command.Source }
    }

    foreach ($candidate in @(
        (Join-Path $root 'tools\7zip\7z.exe'),
        'C:\Program Files\7-Zip\7z.exe',
        'C:\Program Files (x86)\7-Zip\7z.exe',
        (Join-Path $env:LOCALAPPDATA 'Programs\7-Zip\7z.exe')
    )) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }

    throw '本地 FPS Unlock 包使用 7z 格式。请安装 7-Zip，或使用 -SevenZipPath 指定 7z.exe。'
}

function New-SevenZipArchive {
    param([string]$SourceDirectory, [string]$ArchivePath)
    $sevenZip = Find-SevenZip
    if (Test-Path -LiteralPath $ArchivePath) { Remove-Item -LiteralPath $ArchivePath -Force }

    Push-Location -LiteralPath $SourceDirectory
    try {
        # 固实压缩配合 LZMA2 优先减小完整本地包的体积，便于提交蓝奏云 100 MB 限制。
        & $sevenZip a -t7z -mx=9 -m0=lzma2 -ms=on -mmt=on $ArchivePath '.\*' | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "7-Zip 打包失败: $ArchivePath" }
    }
    finally {
        Pop-Location
    }
}

function Assert-LanzouUploadSize {
    param([Parameter(Mandatory)][string]$ArchivePath)
    $limit = [long]100000000
    $size = (Get-Item -LiteralPath $ArchivePath).Length
    if ($size -gt $limit) {
        throw "本地 7z 包仍为 $size bytes，超过蓝奏云 100 MB 限制。请进一步精简组件后再发布。"
    }
}

function Prepare-FpsStage {
    param([string]$Stage, [switch]$Full, [Parameter(Mandatory)][string]$Version)
    Reset-Stage -Path $Stage
    Copy-Item -LiteralPath (Join-Path $installerSource 'Installer.ps1'), (Join-Path $installerSource 'README.md') -Destination $Stage -Force
    Copy-Item -LiteralPath (Join-Path $installerSource 'Configure-Launcher.bat') -Destination (Join-Path $Stage '一键配置.bat') -Force
    Copy-Item -LiteralPath (Join-Path $installerSource 'Configure-Launcher.en.bat') -Destination (Join-Path $Stage 'GenshinFSRBridgeTools.bat') -Force
    $stageScripts = Join-Path $Stage 'scripts'
    New-Item -ItemType Directory -Path $stageScripts -Force | Out-Null
    foreach ($scriptName in @('Configure.ps1', 'Localization.ps1', 'ReShadeResources.ps1', 'Apply-PackageUpdate.ps1')) {
        Copy-Item -LiteralPath (Join-Path $installerSource $scriptName) -Destination (Join-Path $stageScripts $scriptName) -Force
    }
    Copy-Item -LiteralPath (Join-Path $packageAssets 'Feedback.txt') -Destination $Stage -Force
    [IO.File]::WriteAllText((Join-Path $Stage 'Package-Version.txt'), "$Version`r`n", [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $Stage 'NonFrameGeneration.edition'), "`r`n", [Text.UTF8Encoding]::new($false))

    if ($Full) {
        Copy-Item -LiteralPath (Join-Path $unlockerRuntime 'unlockfps_nc.exe'), (Join-Path $unlockerRuntime 'UnlockerStub.dll') -Destination $Stage -Force
    }

    $payload = Join-Path $Stage 'payload'
    $stagePayloadBridge = Join-Path $payload 'Bridge'
    $stagePayloadAnti = Join-Path $payload 'AntiPlayerMosaic'
    $stagePayloadReShade = Join-Path $payload 'ReShade'
    $stageDefaults = Join-Path $payload 'default_config'
    New-Item -ItemType Directory -Path $payload, $stagePayloadBridge, $stagePayloadAnti, $stagePayloadReShade, $stageDefaults -Force | Out-Null
    Copy-Item -LiteralPath $bridgeDll -Destination (Join-Path $stagePayloadBridge 'Dx11FsrBridge.dll') -Force
    Copy-Item -LiteralPath $bridgePackageConfig -Destination (Join-Path $stagePayloadBridge 'Dx11FsrBridge.ini') -Force
    Copy-Item -LiteralPath $antiDll -Destination (Join-Path $stagePayloadAnti 'AntiPlayerMosaic.dll') -Force
    Copy-DirectoryContents -Source $reshadeRuntime -Destination $stagePayloadReShade
    Remove-NonBundledReShadeEffects -ReShadeDirectory $stagePayloadReShade
    Remove-Item -LiteralPath (Join-Path $stagePayloadReShade 'ReShade.ini'), (Join-Path $stagePayloadReShade 'ReShadePreset.ini') -Force -ErrorAction SilentlyContinue
    # The package defaults are generated from the exact component resources used in this build.
    Copy-Item -LiteralPath $bridgePackageConfig -Destination (Join-Path $stageDefaults 'Dx11FsrBridge.ini') -Force
    Copy-Item -LiteralPath (Join-Path $optiRuntime 'OptiScaler.ini'), (Join-Path $optiRuntime 'OptiScaler-UpscalingFiles.json') -Destination $stageDefaults -Force
    Copy-Item -LiteralPath (Join-Path $reshadeRuntime 'ReShade.ini'), (Join-Path $reshadeRuntime 'ReShadePreset.ini') -Destination $stageDefaults -Force

    if ($Full) {
        $stageOpti = Join-Path $payload 'OptiScaler'
        $stageNvidia = Join-Path $payload 'NVIDIA\DLSS'
        New-Item -ItemType Directory -Path $stageOpti, $stageNvidia -Force | Out-Null
        Copy-DirectoryContents -Source $optiRuntime -Destination $stageOpti
        Copy-Item -LiteralPath (Join-Path $dlssRuntime 'nvngx_dlss.dll') -Destination $stageNvidia -Force
        Copy-Item -LiteralPath (Join-Path $dlssRuntime 'nvngx_dlss.license.txt') -Destination $stageNvidia -Force
    }
    New-PackageComponentManifest -PackageRoot $Stage -Full:$Full
}

function Build-FpsPackage {
    param(
        [string]$PackageKind,
        [Parameter(Mandatory)][string]$Version,
        [ValidateSet('Zip', 'SevenZip')][string]$ArchiveFormat = 'Zip'
    )
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
        $extension = if ($ArchiveFormat -eq 'SevenZip') { '7z' } else { 'zip' }
        $name = if ($full) { "原神解帧FSR插件包Full_v$Version.$extension" } else { "原神解帧FSR插件包Lite_v$Version.$extension" }
        $archive = Join-Path $dist $name
        if ($ArchiveFormat -eq 'SevenZip') {
            New-SevenZipArchive -SourceDirectory $stage -ArchivePath $archive
            Assert-LanzouUploadSize -ArchivePath $archive
        }
        else {
            New-ZipArchive -SourceDirectory $stage -ArchivePath $archive
        }
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
    Copy-Item -LiteralPath (Join-Path $packageAssets 'Feedback.txt') -Destination $stage -Force
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
    Copy-Item -LiteralPath $bridgeDll -Destination (Join-Path $bridge 'Dx11FsrBridge.dll') -Force
    Copy-Item -LiteralPath $bridgePackageConfig -Destination (Join-Path $bridge 'Dx11FsrBridge.ini') -Force
    Copy-DirectoryContents -Source $optiRuntime -Destination $opti
    Copy-Item -LiteralPath (Join-Path $dlssRuntime 'nvngx_dlss.dll') -Destination $nvidia -Force
    Copy-Item -LiteralPath (Join-Path $dlssRuntime 'nvngx_dlss.license.txt') -Destination $nvidia -Force
    Copy-DirectoryContents -Source $reshadeRuntime -Destination $reshade
    Remove-NonBundledReShadeEffects -ReShadeDirectory $reshade
    Copy-Item -LiteralPath $bridgePackageConfig -Destination (Join-Path $defaults 'Dx11FsrBridge.ini') -Force
    Copy-Item -LiteralPath (Join-Path $optiRuntime 'OptiScaler.ini'), (Join-Path $optiRuntime 'OptiScaler-UpscalingFiles.json') -Destination $defaults -Force
    Copy-Item -LiteralPath (Join-Path $reshadeRuntime 'ReShade.ini'), (Join-Path $reshadeRuntime 'ReShadePreset.ini') -Destination $defaults -Force
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

Build-PackageComponents
Assert-OptiConfigMatchesRuntime
$version = Get-BridgeVersion
New-Item -ItemType Directory -Path $dist -Force | Out-Null
if (-not $GithubLiteOnly) {
    foreach ($pattern in @(
        '原神解帧FSR插件包Lite_*.zip',
        '原神解帧FSR插件包Full_*.zip',
        '原神解帧FSR插件包Lite_*.7z',
        '原神解帧FSR插件包Full_*.7z',
        '芙芙启动器插件包Lite_*.zip',
        '芙芙启动器插件包Full_*.zip',
        'FSR-Bridge-Plugin.v*.zip'
    )) {
        Get-ChildItem -LiteralPath $dist -File -Filter $pattern -ErrorAction SilentlyContinue | Remove-Item -Force
    }
}

$fpsLiteArchive = $null
$fpsFullArchive = $null
$fufuArchive = $null
if (-not $GithubLiteOnly) {
    $fpsLiteArchive = Build-FpsPackage -PackageKind Lite -Version $version -ArchiveFormat SevenZip
    $fpsFullArchive = Build-FpsPackage -PackageKind Full -Version $version -ArchiveFormat SevenZip
    $fufuArchive = Build-FufuMarketplacePackage -Version $version
}

$githubReleaseDist = Join-Path $dist 'github-release'
if (Test-Path -LiteralPath $githubReleaseDist) { Remove-Item -LiteralPath $githubReleaseDist -Recurse -Force }
New-Item -ItemType Directory -Path $githubReleaseDist -Force | Out-Null
$githubLiteArchive = Join-Path $githubReleaseDist "GenshinFSRBridge.Lite_v$version.zip"
$githubLiteSourceArchive = Build-FpsPackage -PackageKind Lite -Version $version -ArchiveFormat Zip
Copy-Item -LiteralPath $githubLiteSourceArchive -Destination $githubLiteArchive -Force
if (-not $GithubLiteOnly) { Remove-Item -LiteralPath $githubLiteSourceArchive -Force }

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
