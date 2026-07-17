Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$root = Split-Path -Parent $PSCommandPath
$source = Join-Path $root 'GenshinOneClick'
$dist = Join-Path $root 'dist'
$nonFgOutput = Join-Path $dist 'GenshinOneClick-Online'

function Get-BridgeVersion {
    $manifestPath = Join-Path $source 'component-manifest.json'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "组件清单不存在: $manifestPath"
    }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $bridge = @($manifest | Where-Object { $_.Name -eq 'Dx11FsrBridge.dll' } | Select-Object -First 1)
    if ($bridge.Count -ne 1 -or [string]::IsNullOrWhiteSpace([string]$bridge[0].Version)) {
        throw '组件清单缺少 Dx11FsrBridge.dll 版本号。'
    }
    $version = [string]$bridge[0].Version
    if ($version -notmatch '^\d+(\.\d+)+$') {
        throw "Bridge 版本号格式无效: $version"
    }
    return $version
}

function Reset-OutputDirectory {
    param([string]$Path)
    $distPath = [IO.Path]::GetFullPath($dist).TrimEnd('\') + '\'
    $targetPath = [IO.Path]::GetFullPath($Path)
    if (-not $targetPath.StartsWith($distPath, [StringComparison]::OrdinalIgnoreCase)) {
        throw "发布目录不在 dist 下: $targetPath"
    }
    if (Test-Path -LiteralPath $targetPath) {
        Remove-Item -LiteralPath $targetPath -Recurse -Force
    }
    New-Item -ItemType Directory -Path $targetPath | Out-Null
}

function Remove-DistItem {
    param([string]$Path)
    $distPath = [IO.Path]::GetFullPath($dist).TrimEnd('\') + '\'
    $targetPath = [IO.Path]::GetFullPath($Path)
    if (-not $targetPath.StartsWith($distPath, [StringComparison]::OrdinalIgnoreCase)) {
        throw "清理目标不在 dist 下: $targetPath"
    }
    if (Test-Path -LiteralPath $targetPath) {
        Remove-Item -LiteralPath $targetPath -Recurse -Force
    }
}

function Set-IniSectionValue {
    param(
        [string]$Path,
        [string]$Section,
        [string]$Key,
        [string]$Value
    )
    $lines = [Collections.Generic.List[string]]::new()
    $lines.AddRange([string[]][IO.File]::ReadAllLines($Path, [Text.Encoding]::UTF8))
    $currentSection = ''
    $updated = $false
    for ($index = 0; $index -lt $lines.Count; $index++) {
        if ($lines[$index] -match '^\s*\[(?<name>[^\]]+)\]\s*$') {
            $currentSection = $Matches.name.Trim()
            continue
        }
        if ([string]::Equals($currentSection, $Section, [StringComparison]::OrdinalIgnoreCase) -and
            $lines[$index] -match ('^\s*' + [regex]::Escape($Key) + '\s*=')) {
            $lines[$index] = "$Key = $Value"
            $updated = $true
            break
        }
    }
    if (-not $updated) {
        throw "INI 缺少设置: $Path [$Section] $Key"
    }
    [IO.File]::WriteAllLines($Path, $lines, [Text.UTF8Encoding]::new($false))
}

function Get-IniSectionValue {
    param(
        [string]$Path,
        [string]$Section,
        [string]$Key
    )
    $currentSection = ''
    foreach ($line in [IO.File]::ReadAllLines($Path, [Text.Encoding]::UTF8)) {
        if ($line -match '^\s*\[(?<name>[^\]]+)\]\s*$') {
            $currentSection = $Matches.name.Trim()
            continue
        }
        if ([string]::Equals($currentSection, $Section, [StringComparison]::OrdinalIgnoreCase) -and
            $line -match ('^\s*' + [regex]::Escape($Key) + '\s*=\s*(?<value>.*)$')) {
            return $Matches.value.Trim()
        }
    }
    return $null
}

function Copy-ReleaseTree {
    param(
        [string]$Destination,
        [bool]$IncludeUnlocker,
        [bool]$IncludeOptiScaler,
        [bool]$NonFrameGeneration
    )
    Reset-OutputDirectory -Path $Destination
    $excludedRootFiles = @(
        '.installer-state.json',
        '.last-install-error.log',
        'fps_config.json',
        'Repair-Paths.ps1',
        '启动原神.bat',
        'Configure-Launcher.bat',
        'Configure-Launcher.en.bat',
        'Feedback.txt',
        '一键配置.bat',
        'GenshinFSRBridgeTools.bat',
        '日志与反馈.txt',
        'Logs and Feedback.txt',
        'README.md',
        'README.txt',
        'SOURCE.md',
        'SOURCE.txt'
    )
    if (-not $IncludeUnlocker) {
        $excludedRootFiles += @('unlockfps_nc.exe', 'UnlockerStub.dll')
    }
    Get-ChildItem -LiteralPath $source -Force |
        Where-Object {
            $_.Name -ne 'payload' -and
            $_.Name -ne 'backups' -and
            $_.Name -notin $excludedRootFiles
        } |
        Copy-Item -Destination $Destination -Recurse -Force

    $payloadSource = Join-Path $source 'payload'
    $payloadDestination = Join-Path $Destination 'payload'
    New-Item -ItemType Directory -Path $payloadDestination | Out-Null
    Get-ChildItem -LiteralPath $payloadSource -Force |
        Where-Object { $IncludeOptiScaler -or $_.Name -notin @('OptiScaler', 'NVIDIA') } |
        Copy-Item -Destination $payloadDestination -Recurse -Force

    if ($NonFrameGeneration) {
        [IO.File]::WriteAllText(
            (Join-Path $Destination 'NonFrameGeneration.edition'),
            "OptiScaler 0.9.3 stable; frame generation disabled.`r`n",
            [Text.UTF8Encoding]::new($false))
        if ($IncludeOptiScaler) {
            $optiDestination = Join-Path $payloadDestination 'OptiScaler'
            foreach ($configName in @('OptiScaler.ini', 'OptiScaler.default.ini')) {
                $configPath = Join-Path $optiDestination $configName
                Set-IniSectionValue -Path $configPath -Section 'FrameGen' -Key 'Enabled' -Value 'false'
                Set-IniSectionValue -Path $configPath -Section 'FrameGen' -Key 'FGInput' -Value 'nofg'
                Set-IniSectionValue -Path $configPath -Section 'FrameGen' -Key 'FGOutput' -Value 'nofg'
            }
            foreach ($frameGenerationFile in @(
                'amd_fidelityfx_framegeneration_dx12.dll',
                'libxess_fg.dll',
                'dlssg_to_fsr3_amd_is_better.dll'
            )) {
                $frameGenerationPath = Join-Path $optiDestination $frameGenerationFile
                if (Test-Path -LiteralPath $frameGenerationPath -PathType Leaf) {
                    Remove-Item -LiteralPath $frameGenerationPath -Force
                }
            }
        }
    }

    Get-ChildItem -LiteralPath $Destination -Recurse -File -Force |
        Where-Object {
            $_.Name -like '*.log' -or
            $_.Name -like '*.dmp' -or
            $_.Name -like '*.bak' -or
            $_.Name -like '*.jsonl' -or
            $_.Name -like '*.similarity.txt' -or
            $_.Name -ieq 'desktop.ini' -or
            $_.Name -ieq 'Thumbs.db'
        } |
        Remove-Item -Force

    foreach ($documentationPath in @(
        'payload\NVIDIA\DLSS\SOURCE.txt',
        'payload\NVIDIA\DLSS\README.md',
        'payload\NVIDIA\DLSS\README.txt'
    )) {
        $path = Join-Path $Destination $documentationPath
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            Remove-Item -LiteralPath $path -Force
        }
    }

    Copy-Item -LiteralPath (Join-Path $source 'Configure-Launcher.bat') -Destination (Join-Path $Destination '一键配置.bat') -Force
    Copy-Item -LiteralPath (Join-Path $source 'Configure-Launcher.en.bat') -Destination (Join-Path $Destination 'GenshinFSRBridgeTools.bat') -Force
    Copy-Item -LiteralPath (Join-Path $source 'Feedback.txt') -Destination (Join-Path $Destination 'Feedback.txt') -Force
}

function Assert-ReleaseLayout {
    param(
        [string]$Path,
        [bool]$ExpectUnlocker,
        [bool]$ExpectOptiScaler,
        [bool]$ExpectNvidiaDlss,
        [bool]$ExpectNonFrameGeneration
    )
    foreach ($required in @(
        '一键配置.bat',
        'GenshinFSRBridgeTools.bat',
        'Installer.ps1',
        'Configure.ps1',
        'Localization.ps1',
        'component-manifest.json',
        'Feedback.txt',
        'payload\Bridge\Dx11FsrBridge.dll',
        'payload\AntiPlayerMosaic.dll',
        'payload\ReShade\ReShade64.dll',
        'payload\ReShade\LICENSE-ReShade-BSD-3-Clause.txt',
        'payload\ReShade\reshade-shaders\Addons\renodx-genshin.addon64',
        'payload\ReShade\reshade-shaders\NOTICE-RenoDX-genshin.txt',
        'payload\ReShade\reshade-shaders\LICENSE-ReShade_HDR_shaders-GPL-3.0.txt',
        'payload\ReShade\reshade-shaders\NOTICE-ReShade_HDR_shaders.txt'
    )) {
        if (-not (Test-Path -LiteralPath (Join-Path $Path $required))) {
            throw "发布目录缺少文件: $Path\$required"
        }
    }
    foreach ($forbidden in @(
        '启动原神.bat',
        'Configure-Launcher.bat',
        'Configure-Launcher.en.bat',
        'Repair-Paths.ps1',
        'fps_config.json',
        '.installer-state.json',
        '日志与反馈.txt',
        'Logs and Feedback.txt',
        'README.md',
        'README.txt',
        'SOURCE.md',
        'SOURCE.txt',
        'payload\Bridge\Dx11FsrBridge.ini',
        'payload\Bridge\Dx11FsrBridge.default.ini',
        'payload\NVIDIA\DLSS\SOURCE.txt',
        'payload\NVIDIA\DLSS\README.md',
        'payload\NVIDIA\DLSS\README.txt'
    )) {
        if (Test-Path -LiteralPath (Join-Path $Path $forbidden)) {
            throw "发布目录包含遗留文件: $Path\$forbidden"
        }
    }
    $unlockerExists = Test-Path -LiteralPath (Join-Path $Path 'unlockfps_nc.exe') -PathType Leaf
    $optiExists = Test-Path -LiteralPath (Join-Path $Path 'payload\OptiScaler\OptiScaler.dll') -PathType Leaf
    $optiDefaultConfigExists = Test-Path -LiteralPath (Join-Path $Path 'payload\OptiScaler\OptiScaler.default.ini') -PathType Leaf
    $fakeNvapiDefaultConfigExists = Test-Path -LiteralPath (Join-Path $Path 'payload\OptiScaler\fakenvapi.default.ini') -PathType Leaf
    $nvidiaDlssExists = Test-Path -LiteralPath (Join-Path $Path 'payload\NVIDIA\DLSS\nvngx_dlss.dll') -PathType Leaf
    if ($unlockerExists -ne $ExpectUnlocker) { throw "FPS Unlocker 发布状态不正确: $Path" }
    if ($optiExists -ne $ExpectOptiScaler) { throw "OptiScaler 发布状态不正确: $Path" }
    if ($optiDefaultConfigExists -ne $ExpectOptiScaler) { throw "OptiScaler 默认配置模板状态不正确: $Path" }
    if ($fakeNvapiDefaultConfigExists -ne $ExpectOptiScaler) { throw "fakenvapi 默认配置模板状态不正确: $Path" }
    if ($nvidiaDlssExists -ne $ExpectNvidiaDlss) { throw "NVIDIA DLSS 内置状态不正确: $Path" }
    $nonFrameGenerationMarkerExists = Test-Path -LiteralPath (Join-Path $Path 'NonFrameGeneration.edition') -PathType Leaf
    if ($nonFrameGenerationMarkerExists -ne $ExpectNonFrameGeneration) {
        throw "非帧生成版本标记状态不正确: $Path"
    }
    if ($ExpectOptiScaler) {
        $optiPath = Join-Path $Path 'payload\OptiScaler\OptiScaler.dll'
        $optiFile = Get-Item -LiteralPath $optiPath
        if ($optiFile.VersionInfo.FileVersion -ne '0.9.3.0') {
            throw "OptiScaler 版本不是 0.9.3.0: $optiPath"
        }
        if ((Get-FileHash -LiteralPath $optiPath -Algorithm SHA256).Hash -ne
            '2369120927264BB2B120E7FB0940CB0B3242DC788417AB92FB99953555016511') {
            throw "OptiScaler 0.9.3 哈希不匹配: $optiPath"
        }
    }
    if ($ExpectNonFrameGeneration -and $ExpectOptiScaler) {
        foreach ($configName in @('OptiScaler.ini', 'OptiScaler.default.ini')) {
            $configPath = Join-Path $Path "payload\OptiScaler\$configName"
            if ((Get-IniSectionValue -Path $configPath -Section 'FrameGen' -Key 'Enabled') -ne 'false' -or
                (Get-IniSectionValue -Path $configPath -Section 'FrameGen' -Key 'FGInput') -ne 'nofg' -or
                (Get-IniSectionValue -Path $configPath -Section 'FrameGen' -Key 'FGOutput') -ne 'nofg') {
                throw "非帧生成配置未锁定: $configPath"
            }
        }
        foreach ($forbiddenFrameGenerationFile in @(
            'payload\OptiScaler\amd_fidelityfx_framegeneration_dx12.dll',
            'payload\OptiScaler\libxess_fg.dll',
            'payload\OptiScaler\dlssg_to_fsr3_amd_is_better.dll'
        )) {
            if (Test-Path -LiteralPath (Join-Path $Path $forbiddenFrameGenerationFile)) {
                throw "非帧生成包包含 FG 后端: $forbiddenFrameGenerationFile"
            }
        }
    }

    $scriptText = @(
        Get-Content -LiteralPath (Join-Path $Path 'Installer.ps1') -Raw -Encoding UTF8
        Get-Content -LiteralPath (Join-Path $Path 'Configure.ps1') -Raw -Encoding UTF8
    ) -join "`n"
    foreach ($forbiddenPattern in @(
        'HKEY_CURRENT_USER',
        'HKEY_LOCAL_MACHINE',
        'PlayerPrefs',
        'GENERAL_DATA',
        'DeleteFile.*login',
        'reg\.exe'
    )) {
        if ($scriptText -match $forbiddenPattern) {
            throw "安装脚本包含禁止的游戏设置或登录信息操作: $forbiddenPattern"
        }
    }
}

function New-ReleaseArchive {
    param(
        [string]$SourceDirectory,
        [string]$ArchivePath
    )
    if (Test-Path -LiteralPath $ArchivePath -PathType Leaf) {
        Remove-Item -LiteralPath $ArchivePath -Force
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory(
        $SourceDirectory,
        $ArchivePath,
        [IO.Compression.CompressionLevel]::Optimal,
        $false)
    if (-not (Test-Path -LiteralPath $ArchivePath -PathType Leaf)) {
        throw "ZIP 创建失败: $ArchivePath"
    }
}

function Assert-ReleaseArchive {
    param([string]$ArchivePath)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        $entryNames = @($archive.Entries | ForEach-Object { $_.FullName.Replace('/', '\') })
        if ($entryNames -notcontains 'Feedback.txt') {
            throw "ZIP 缺少日志反馈文档: $ArchivePath\Feedback.txt"
        }
        if ($entryNames -notcontains 'NonFrameGeneration.edition') {
            throw "ZIP 缺少非帧生成版本标记: $ArchivePath"
        }
        foreach ($forbidden in @(
            'Configure-Launcher.bat',
            'Configure-Launcher.en.bat',
            '日志与反馈.txt',
            'Logs and Feedback.txt',
            'README.md',
            'README.txt',
            'SOURCE.md',
            'SOURCE.txt',
            'payload\Bridge\Dx11FsrBridge.ini',
            'payload\Bridge\Dx11FsrBridge.default.ini',
            'payload\OptiScaler\amd_fidelityfx_framegeneration_dx12.dll',
            'payload\OptiScaler\libxess_fg.dll',
            'payload\OptiScaler\dlssg_to_fsr3_amd_is_better.dll',
            'payload\NVIDIA\DLSS\SOURCE.txt',
            'payload\NVIDIA\DLSS\README.md',
            'payload\NVIDIA\DLSS\README.txt'
        )) {
            if ($entryNames -contains $forbidden) {
                throw "ZIP 包含应排除的文档: $ArchivePath\$forbidden"
            }
        }
    }
    finally {
        $archive.Dispose()
    }
}

if (-not (Test-Path -LiteralPath $source -PathType Container)) {
    throw "源目录不存在: $source"
}

$bridgeVersion = Get-BridgeVersion
$nonFgArchive = Join-Path $dist ("原神解帧FSR插件包Lite_v{0}.zip" -f $bridgeVersion)
$obsoleteNonFgArchive = Join-Path $dist ("原神解帧FSR插件包-非帧生成-OptiScaler0.9.3_v{0}-pre.zip" -f $bridgeVersion)

foreach ($obsoletePath in @(
    $obsoleteNonFgArchive,
    (Join-Path $dist 'GenshinOneClick-FG-OptiScaler-0.10.0-pre1-Test'),
    (Join-Path $dist 'GenshinOneClick-FG-OptiScaler-0.10.0-pre1-Test.zip')
)) {
    Remove-DistItem -Path $obsoletePath
}

Copy-ReleaseTree -Destination $nonFgOutput -IncludeUnlocker $false -IncludeOptiScaler $false -NonFrameGeneration $true
Assert-ReleaseLayout -Path $nonFgOutput -ExpectUnlocker $false -ExpectOptiScaler $false -ExpectNvidiaDlss $false -ExpectNonFrameGeneration $true
New-ReleaseArchive -SourceDirectory $nonFgOutput -ArchivePath $nonFgArchive
Assert-ReleaseArchive -ArchivePath $nonFgArchive

Write-Host '非帧生成正式发布目录构建完成。' -ForegroundColor Green
Write-Host "目录: $nonFgOutput"
Write-Host "ZIP: $nonFgArchive"
