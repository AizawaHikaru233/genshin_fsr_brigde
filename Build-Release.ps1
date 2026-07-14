Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$root = Split-Path -Parent $PSCommandPath
$source = Join-Path $root 'GenshinOneClick'
$dist = Join-Path $root 'dist'
$liteOutput = Join-Path $dist 'GenshinOneClick-Online'

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

function Copy-ReleaseTree {
    param(
        [string]$Destination,
        [bool]$IncludeUnlocker,
        [bool]$IncludeOptiScaler
    )
    Reset-OutputDirectory -Path $Destination
    $excludedRootFiles = @(
        '.installer-state.json',
        '.last-install-error.log',
        'fps_config.json',
        'Repair-Paths.ps1',
        '启动原神.bat',
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
}

function Assert-ReleaseLayout {
    param(
        [string]$Path,
        [bool]$ExpectUnlocker,
        [bool]$ExpectOptiScaler,
        [bool]$ExpectNvidiaDlss
    )
    foreach ($required in @(
        '一键配置.bat',
        'Installer.ps1',
        'Configure.ps1',
        'component-manifest.json',
        '日志与反馈.txt',
        'payload\Bridge\Dx11FsrBridge.dll',
        'payload\Bridge\Dx11FsrBridge.ini',
        'payload\Bridge\Dx11FsrBridge.default.ini',
        'payload\AntiPlayerMosaic.dll',
        'payload\ReShade\ReShade64.dll',
        'payload\ReShade\LICENSE-ReShade-BSD-3-Clause.txt',
        'payload\ReShade\reshade-shaders\LICENSE-ReShade_HDR_shaders-GPL-3.0.txt',
        'payload\ReShade\reshade-shaders\NOTICE-ReShade_HDR_shaders.txt'
    )) {
        if (-not (Test-Path -LiteralPath (Join-Path $Path $required))) {
            throw "发布目录缺少文件: $Path\$required"
        }
    }
    foreach ($forbidden in @(
        '启动原神.bat',
        'Repair-Paths.ps1',
        'fps_config.json',
        '.installer-state.json',
        'README.md',
        'README.txt',
        'SOURCE.md',
        'SOURCE.txt',
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
        if ($entryNames -notcontains '日志与反馈.txt') {
            throw "ZIP 缺少日志反馈文档: $ArchivePath"
        }
        foreach ($forbidden in @(
            'README.md',
            'README.txt',
            'SOURCE.md',
            'SOURCE.txt',
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

Copy-ReleaseTree -Destination $liteOutput -IncludeUnlocker $false -IncludeOptiScaler $false
Assert-ReleaseLayout -Path $liteOutput -ExpectUnlocker $false -ExpectOptiScaler $false -ExpectNvidiaDlss $false

$bridgeVersion = Get-BridgeVersion
$liteArchive = Join-Path $dist ("原神解帧FSR插件包Lite_v{0}.zip" -f $bridgeVersion)
New-ReleaseArchive -SourceDirectory $liteOutput -ArchivePath $liteArchive
Assert-ReleaseArchive -ArchivePath $liteArchive

foreach ($obsoletePath in @(
    (Join-Path $dist 'GenshinOneClick-Full'),
    (Join-Path $dist ("原神解帧FSR插件包Full_v{0}.zip" -f $bridgeVersion))
)) {
    if (Test-Path -LiteralPath $obsoletePath) {
        Remove-Item -LiteralPath $obsoletePath -Recurse -Force
    }
}

Write-Host '正式发布目录构建完成。' -ForegroundColor Green
Write-Host "联网精简版: $liteOutput"
Write-Host "Lite ZIP: $liteArchive"
