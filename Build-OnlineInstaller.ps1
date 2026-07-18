[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release'
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
$githubReleaseDist = Join-Path $dist 'github-release'
$sharedOptiDefaultConfig = Join-Path $root 'SharedResources\OptiScaler\default_config'
$sharedReShadeDefaultConfig = Join-Path $root 'SharedResources\ReShade\default_config'
$manifestPath = Join-Path $source 'component-manifest.json'

function Reset-Directory {
    param([string]$Path)
    $distRoot = [IO.Path]::GetFullPath($dist).TrimEnd('\') + '\'
    $fullPath = [IO.Path]::GetFullPath($Path)
    if (-not $fullPath.StartsWith($distRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "构建目录不在 dist 下: $fullPath"
    }
    if (Test-Path -LiteralPath $fullPath) { Remove-Item -LiteralPath $fullPath -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $fullPath | Out-Null
}

function Get-BridgeVersion {
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw "组件清单不存在: $manifestPath" }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $bridge = @($manifest | Where-Object { $_.Name -eq 'Dx11FsrBridge.dll' } | Select-Object -First 1)
    if ($bridge.Count -ne 1 -or [string]$bridge[0].Version -notmatch '^\d+(\.\d+)+$') {
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
            if (-not [string]::IsNullOrWhiteSpace($visualStudioPath)) {
                $candidate = Join-Path $visualStudioPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
                if (Test-Path -LiteralPath $candidate -PathType Leaf) { $cmakePath = $candidate }
            }
        }
    }
    if ($null -eq $cmakePath) { throw '没有找到 CMake。请安装 CMake 或包含 CMake 组件的 Visual Studio。' }
    & $cmakePath @Arguments | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "CMake 命令失败: cmake $($Arguments -join ' ')" }
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
    if (-not (Test-Path -LiteralPath $ArchivePath -PathType Leaf)) { throw "ZIP 创建失败: $ArchivePath" }
}

function Get-ZipEntries {
    param([string]$ArchivePath)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try { return @($archive.Entries | ForEach-Object { $_.FullName.Replace('/', '\') }) }
    finally { $archive.Dispose() }
}

function Assert-NoForbiddenFiles {
    param([string]$Path, [string[]]$ForbiddenNames)
    foreach ($file in Get-ChildItem -LiteralPath $Path -Recurse -File -Force) {
        if ($file.Name -in $ForbiddenNames) { throw "在线包包含禁止内置的组件: $($file.FullName)" }
        if ($file.Name -like '*.log' -or $file.Name -like '*.dmp' -or $file.Name -like '*.bak') {
            throw "在线包包含运行残留: $($file.FullName)"
        }
    }
}

function Assert-RequiredFiles {
    param([string]$Path, [string[]]$RelativePaths)
    foreach ($relativePath in $RelativePaths) {
        if (-not (Test-Path -LiteralPath (Join-Path $Path $relativePath) -PathType Leaf)) {
            throw "发布目录缺少文件: $Path\$relativePath"
        }
    }
}

function Assert-ComponentConfigLayout {
    param([string]$Path)
    $defaultConfig = Join-Path $Path 'payload\default_config'
    foreach ($required in @('OptiScaler.ini', 'OptiScaler-UpscalingFiles.json', 'ReShade.ini', 'ReShadePreset.ini')) {
        if (-not (Test-Path -LiteralPath (Join-Path $defaultConfig $required) -PathType Leaf)) {
            throw "发布目录缺少独立默认配置: payload\default_config\$required"
        }
    }
    foreach ($nested in @('payload\OptiScaler\default_config', 'payload\ReShade\default_config')) {
        if (Test-Path -LiteralPath (Join-Path $Path $nested) -PathType Container) {
            throw "发布目录仍包含组件内部默认配置: $nested"
        }
    }
    foreach ($runtimeConfig in @('payload\ReShade\ReShade.ini', 'payload\ReShade\ReShadePreset.ini')) {
        if (Test-Path -LiteralPath (Join-Path $Path $runtimeConfig) -PathType Leaf) {
            throw "发布目录错误内置 ReShade 运行配置: $runtimeConfig"
        }
    }
}

function Assert-CleanConfigTemplates {
    $presetPath = Join-Path $sharedReShadeDefaultConfig 'ReShadePreset.ini'
    $presetLines = @((Get-Content -LiteralPath $presetPath -Encoding UTF8) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $expectedPresetLines = @('PreprocessorDefinitions=', 'Techniques=', 'TechniqueSorting=')
    if (@(Compare-Object -ReferenceObject $expectedPresetLines -DifferenceObject $presetLines -SyncWindow 0).Count -ne 0) {
        throw "ReShade 默认 preset 不是官方格式的空白模板: $presetPath"
    }
    $reShadeTemplate = Get-Content -LiteralPath (Join-Path $sharedReShadeDefaultConfig 'ReShade.ini') -Raw -Encoding UTF8
    if ($reShadeTemplate -match '(?i)FakeHDR|lilium|genshin-ae|[A-Z]:\\Users\\') {
        throw 'ReShade 默认配置包含旧 preset、本机路径或用户状态。'
    }
}

function Build-FpsUnlockOnlinePackage {
    param([string]$Version)
    $stage = Join-Path $dist '.fps-unlock-online-stage'
    Reset-Directory -Path $stage
    try {
        $rootExcludes = @(
            'unlockfps_nc.exe', 'UnlockerStub.dll', 'fps_config.json', '.installer-state.json',
            '.last-install-error.log', 'backups', 'payload', 'README.md', 'Configure-Launcher.bat',
            'Configure-Launcher.en.bat'
        )
        Get-ChildItem -LiteralPath $source -Force |
            Where-Object { $_.Name -notin $rootExcludes } |
            Copy-Item -Destination $stage -Recurse -Force

        Copy-Item -LiteralPath (Join-Path $source 'Configure-Launcher.bat') -Destination (Join-Path $stage '一键配置.bat') -Force
        Copy-Item -LiteralPath (Join-Path $source 'Configure-Launcher.en.bat') -Destination (Join-Path $stage 'GenshinFSRBridgeTools.bat') -Force
        [IO.File]::WriteAllText((Join-Path $stage 'Package-Version.txt'), $Version + "`r`n", [Text.UTF8Encoding]::new($false))

        $stagePayload = Join-Path $stage 'payload'
        New-Item -ItemType Directory -Force -Path $stagePayload | Out-Null
        Get-ChildItem -LiteralPath (Join-Path $source 'payload') -Force |
            Where-Object { $_.Name -notin @('OptiScaler', 'NVIDIA') } |
            Copy-Item -Destination $stagePayload -Recurse -Force
        $stageOpti = Join-Path $stagePayload 'OptiScaler'
        $stageReShade = Join-Path $stagePayload 'ReShade'
        $stageDefaultConfig = Join-Path $stagePayload 'default_config'
        New-Item -ItemType Directory -Force -Path $stageOpti, $stageReShade, $stageDefaultConfig | Out-Null
        Get-ChildItem -LiteralPath $sharedOptiDefaultConfig -File -Force | Copy-Item -Destination $stageDefaultConfig -Force
        Get-ChildItem -LiteralPath $sharedReShadeDefaultConfig -File -Force | Copy-Item -Destination $stageDefaultConfig -Force
        Remove-Item -LiteralPath (Join-Path $stageReShade 'ReShade.ini'), (Join-Path $stageReShade 'ReShadePreset.ini') -Force -ErrorAction SilentlyContinue

        Assert-RequiredFiles -Path $stage -RelativePaths @(
            '一键配置.bat',
            'GenshinFSRBridgeTools.bat',
            'Installer.ps1',
            'Configure.ps1',
            'Apply-PackageUpdate.ps1',
            'Localization.ps1',
            'component-manifest.json',
            'Feedback.txt',
            'Package-Version.txt',
            'payload\default_config\OptiScaler.ini',
            'payload\default_config\OptiScaler-UpscalingFiles.json',
            'payload\default_config\ReShade.ini',
            'payload\default_config\ReShadePreset.ini',
            'payload\Bridge\Dx11FsrBridge.dll',
            'payload\AntiPlayerMosaic\AntiPlayerMosaic.dll',
            'payload\ReShade\ReShade64.dll',
            'payload\ReShade\reshade-shaders\Addons\renodx-genshin.addon64'
        )
        Assert-ComponentConfigLayout -Path $stage
        Assert-NoForbiddenFiles -Path $stage -ForbiddenNames @(
            'unlockfps_nc.exe', 'UnlockerStub.dll', 'OptiScaler.dll', 'nvngx_dlss.dll',
            'nvngx_dlssg.dll', 'nvngx_dlssd.dll'
        )

        $archivePath = Join-Path $dist ("原神解帧FSR插件包Lite_v{0}.zip" -f $Version)
        New-ZipArchive -SourceDirectory $stage -ArchivePath $archivePath
        $entries = Get-ZipEntries -ArchivePath $archivePath
        foreach ($required in @(
            '一键配置.bat', 'Configure.ps1', 'Feedback.txt', 'Package-Version.txt',
            'payload\default_config\OptiScaler.ini', 'payload\default_config\OptiScaler-UpscalingFiles.json',
            'payload\default_config\ReShade.ini', 'payload\default_config\ReShadePreset.ini',
            'payload\Bridge\Dx11FsrBridge.dll', 'payload\ReShade\ReShade64.dll'
        )) {
            if ($entries -notcontains $required) { throw "FPS Unlock 在线 ZIP 缺少文件: $required" }
        }
        Write-Host "FPS Unlock 在线包: $archivePath" -ForegroundColor Green
        return $archivePath
    }
    finally {
        if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
    }
}

function Build-FufuOnlinePackage {
    param([string]$Version)
    if (-not (Test-Path -LiteralPath (Join-Path $fufuSource 'CMakeLists.txt') -PathType Leaf)) {
        throw "Fufu 插件源码不存在: $fufuSource"
    }
    Invoke-Cmake @('-S', $fufuSource, '-B', $fufuBuild, '-A', 'x64')
    Invoke-Cmake @('--build', $fufuBuild, '--config', $Configuration)
    if (-not (Test-Path -LiteralPath $fufuDll -PathType Leaf)) { throw "Fufu 插件编译输出不存在: $fufuDll" }
    if ((Get-Item -LiteralPath $fufuDll).VersionInfo.FileVersion -ne '0.2.1.0') {
        throw "Fufu 插件版本不正确: $((Get-Item -LiteralPath $fufuDll).VersionInfo.FileVersion)"
    }

    $stage = Join-Path $dist '.fufu-online-stage'
    Reset-Directory -Path $stage
    try {
        Copy-Item -LiteralPath (Join-Path $fufuSource 'Install-FufuPlugin.ps1') -Destination $stage -Force
        Copy-Item -LiteralPath (Join-Path $fufuSource '安装到芙芙启动器.bat') -Destination $stage -Force
        Copy-Item -LiteralPath (Join-Path $source 'Apply-PackageUpdate.ps1') -Destination $stage -Force
        Copy-Item -LiteralPath (Join-Path $source 'Feedback.txt') -Destination $stage -Force
        [IO.File]::WriteAllText((Join-Path $stage 'Package-Version.txt'), $Version + "`r`n", [Text.UTF8Encoding]::new($false))

        Copy-Item -LiteralPath $fufuDll -Destination (Join-Path $stage 'FSR-Bridge-Plugin.dll') -Force
        $pluginConfig = Get-Content -LiteralPath (Join-Path $fufuSource 'config.ini') -Raw -Encoding UTF8
        $pluginConfig = [regex]::Replace($pluginConfig, '(?m)^Version\s*=.*$', "Version = $Version")
        [IO.File]::WriteAllText((Join-Path $stage 'config.ini'), $pluginConfig, [Text.UTF8Encoding]::new($false))

        $stagePayload = Join-Path $stage 'payload'
        $stageBridge = Join-Path $stagePayload 'Bridge'
        $stageReShade = Join-Path $stagePayload 'ReShade'
        New-Item -ItemType Directory -Force -Path $stageBridge, $stageReShade | Out-Null
        Copy-Item -LiteralPath (Join-Path $source 'payload\Bridge\Dx11FsrBridge.dll') -Destination $stageBridge -Force
        Get-ChildItem -LiteralPath (Join-Path $source 'payload\ReShade') -Force |
            Copy-Item -Destination $stageReShade -Recurse -Force
        $stageOpti = Join-Path $stagePayload 'OptiScaler'
        $stageDefaultConfig = Join-Path $stagePayload 'default_config'
        New-Item -ItemType Directory -Force -Path $stageOpti, $stageDefaultConfig | Out-Null
        Get-ChildItem -LiteralPath $sharedOptiDefaultConfig -File -Force | Copy-Item -Destination $stageDefaultConfig -Force
        Get-ChildItem -LiteralPath $sharedReShadeDefaultConfig -File -Force | Copy-Item -Destination $stageDefaultConfig -Force
        Remove-Item -LiteralPath (Join-Path $stageReShade 'ReShade.ini'), (Join-Path $stageReShade 'ReShadePreset.ini') -Force -ErrorAction SilentlyContinue

        Assert-RequiredFiles -Path $stage -RelativePaths @(
            'Install-FufuPlugin.ps1',
            'Apply-PackageUpdate.ps1',
            'Package-Version.txt',
            'Feedback.txt',
            '安装到芙芙启动器.bat',
            'FSR-Bridge-Plugin.dll',
            'config.ini',
            'payload\Bridge\Dx11FsrBridge.dll',
            'payload\ReShade\ReShade64.dll',
            'payload\default_config\OptiScaler.ini',
            'payload\default_config\OptiScaler-UpscalingFiles.json',
            'payload\default_config\ReShade.ini',
            'payload\default_config\ReShadePreset.ini',
            'payload\ReShade\reshade-shaders\Addons\renodx-genshin.addon64'
        )
        Assert-ComponentConfigLayout -Path $stage
        Assert-NoForbiddenFiles -Path $stage -ForbiddenNames @(
            'unlockfps_nc.exe', 'UnlockerStub.dll', 'OptiScaler.dll', 'AntiPlayerMosaic.dll', 'AntiPlayerMosaic.log', 'nvngx_dlss.dll',
            'nvngx_dlssg.dll', 'nvngx_dlssd.dll', 'amd_fidelityfx_framegeneration_dx12.dll',
            'amd_fidelityfx_vk.dll', 'dlssg_to_fsr3_amd_is_better.dll', 'fakenvapi.dll', 'libxess_fg.dll'
        )

        $archivePath = Join-Path $dist ("芙芙启动器插件包Lite_v{0}.zip" -f $Version)
        New-ZipArchive -SourceDirectory $stage -ArchivePath $archivePath
        $entries = Get-ZipEntries -ArchivePath $archivePath
        foreach ($required in @(
            '安装到芙芙启动器.bat', 'Install-FufuPlugin.ps1', 'Apply-PackageUpdate.ps1', 'Package-Version.txt',
            'Feedback.txt', 'payload\default_config\OptiScaler.ini', 'payload\default_config\OptiScaler-UpscalingFiles.json',
            'payload\default_config\ReShade.ini',
            'payload\default_config\ReShadePreset.ini', 'FSR-Bridge-Plugin.dll', 'config.ini',
            'payload\Bridge\Dx11FsrBridge.dll'
        )) {
            if ($entries -notcontains $required) { throw "Fufu 在线 ZIP 缺少文件: $required" }
        }
        foreach ($forbiddenName in @('OptiScaler.dll', 'unlockfps_nc.exe', 'nvngx_dlss.dll')) {
            if ($entries | Where-Object { [IO.Path]::GetFileName($_) -ieq $forbiddenName }) {
                throw "Fufu 在线 ZIP 包含禁止组件: $forbiddenName"
            }
        }
        Write-Host "Fufu 在线包: $archivePath" -ForegroundColor Green
        return $archivePath
    }
    finally {
        if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
    }
}

if (-not (Test-Path -LiteralPath $source -PathType Container)) { throw "一键方案源码不存在: $source" }
Assert-CleanConfigTemplates
$localizationTest = Join-Path $root 'tools\Test-FpsUnlockLocalization.ps1'
if (-not (Test-Path -LiteralPath $localizationTest -PathType Leaf)) { throw "英文本地化覆盖检查不存在: $localizationTest" }
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $localizationTest
if ($LASTEXITCODE -ne 0) { throw "FPS Unlock 英文本地化未同步完整: $LASTEXITCODE" }
New-Item -ItemType Directory -Force -Path $dist | Out-Null
foreach ($pattern in @(
    '原神解帧FSR插件包Lite_*.zip',
    '芙芙启动器插件包Lite_*.zip',
    '原神FSR2桥接插件_*.zip'
)) {
    Get-ChildItem -LiteralPath $dist -File -Filter $pattern -ErrorAction SilentlyContinue | Remove-Item -Force
}
$bridgeVersion = Get-BridgeVersion
$fpsArchive = Build-FpsUnlockOnlinePackage -Version $bridgeVersion
$fufuArchive = Build-FufuOnlinePackage -Version $bridgeVersion

if (Test-Path -LiteralPath $githubReleaseDist) { Remove-Item -LiteralPath $githubReleaseDist -Recurse -Force }
New-Item -ItemType Directory -Force -Path $githubReleaseDist | Out-Null
$githubFpsArchive = Join-Path $githubReleaseDist "GenshinFSRBridge.Lite_v$bridgeVersion.zip"
$githubFufuArchive = Join-Path $githubReleaseDist "FuFuLauncherPlugin.Lite_v$bridgeVersion.zip"
Copy-Item -LiteralPath $fpsArchive -Destination $githubFpsArchive -Force
Copy-Item -LiteralPath $fufuArchive -Destination $githubFufuArchive -Force
if ((Get-FileHash -LiteralPath $fpsArchive -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $githubFpsArchive -Algorithm SHA256).Hash -or
    (Get-FileHash -LiteralPath $fufuArchive -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $githubFufuArchive -Algorithm SHA256).Hash) {
    throw 'GitHub Release 英文别名与中文正式包内容不一致。'
}

Write-Host ''
Write-Host '统一在线发布包构建完成。' -ForegroundColor Green
foreach ($archive in @($fpsArchive, $fufuArchive, $githubFpsArchive, $githubFufuArchive)) {
    $item = Get-Item -LiteralPath $archive
    Write-Host "$($item.Name)  $($item.Length) bytes  SHA256=$((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash)"
}
