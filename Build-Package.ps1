[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$root = Split-Path -Parent $PSCommandPath
$cmake = Get-Command cmake -ErrorAction Stop
$bridgeBuild = Join-Path $root 'build-package-bridge'
$antiBuild = Join-Path $root 'build-package-antiplayermosaic'
$packageRoot = Join-Path $root 'GenshinOneClick'
$bridgeOutput = Join-Path $bridgeBuild "$Configuration\Dx11FsrBridge.dll"
$antiOutput = Join-Path $antiBuild "$Configuration\AntiPlayerMosaic.dll"

function Invoke-Cmake {
    param([string[]]$Arguments)

    & $cmake.Path @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "CMake 命令失败: cmake $($Arguments -join ' ')"
    }
}

function Update-ManifestEntry {
    param(
        [object[]]$Manifest,
        [string]$Name,
        [string]$Path
    )

    $entry = @($Manifest | Where-Object { $_.Name -eq $Name } | Select-Object -First 1)
    if ($entry.Count -ne 1) {
        throw "组件清单缺少条目: $Name"
    }
    $file = Get-Item -LiteralPath $Path -ErrorAction Stop
    $entry[0].SHA256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    $entry[0].Bytes = $file.Length
}

Invoke-Cmake @(
    '-S', $root,
    '-B', $bridgeBuild,
    '-A', 'x64',
    '-DDX11FSRBRIDGE_RELEASE_RUNTIME=ON',
    '-DDX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL=ON'
)
Invoke-Cmake @('--build', $bridgeBuild, '--config', $Configuration)

Invoke-Cmake @('-S', (Join-Path $root 'AntiPlayerMosaic'), '-B', $antiBuild, '-A', 'x64')
Invoke-Cmake @('--build', $antiBuild, '--config', $Configuration)

foreach ($output in @($bridgeOutput, $antiOutput)) {
    if (-not (Test-Path -LiteralPath $output -PathType Leaf)) {
        throw "未找到编译输出: $output"
    }
}

$bridgeDestination = Join-Path $packageRoot 'payload\Bridge\Dx11FsrBridge.dll'
$antiDestination = Join-Path $packageRoot 'payload\AntiPlayerMosaic.dll'
Copy-Item -LiteralPath $bridgeOutput -Destination $bridgeDestination -Force
Copy-Item -LiteralPath $antiOutput -Destination $antiDestination -Force
Copy-Item -LiteralPath (Join-Path $packageRoot 'Configure-Launcher.bat') -Destination (Join-Path $packageRoot '一键配置.bat') -Force
Copy-Item -LiteralPath (Join-Path $packageRoot 'Feedback.txt') -Destination (Join-Path $packageRoot '日志与反馈.txt') -Force

$manifestPath = Join-Path $packageRoot 'component-manifest.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
Update-ManifestEntry -Manifest $manifest -Name 'Dx11FsrBridge.dll' -Path $bridgeDestination
Update-ManifestEntry -Manifest $manifest -Name 'AntiPlayerMosaic.dll' -Path $antiDestination
[IO.File]::WriteAllText(
    $manifestPath,
    ($manifest | ConvertTo-Json -Depth 4),
    [Text.UTF8Encoding]::new($false)
)

& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'Build-Release.ps1')
if ($LASTEXITCODE -ne 0) {
    throw "Lite 发布包构建失败，退出码: $LASTEXITCODE"
}
