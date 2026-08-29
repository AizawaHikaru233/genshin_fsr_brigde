param(
    [Parameter(Mandatory = $true)][int]$ParentProcessId,
    [Parameter(Mandatory = $true)][string]$SourceDirectory,
    [Parameter(Mandatory = $true)][string]$TargetDirectory,
    [Parameter(Mandatory = $true)][string]$RelaunchScript,
    [string]$GamePath,
    [ValidateSet('Auto', 'zh-CN', 'en-US')][string]$Language = 'Auto',
    [switch]$ResumeUpdateAll
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$source = [IO.Path]::GetFullPath($SourceDirectory)
$target = [IO.Path]::GetFullPath($TargetDirectory)
$relaunch = [IO.Path]::GetFullPath($RelaunchScript)
$logPath = Join-Path $target '.last-self-update.log'
$preservedRelativePaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($relativePath in @(
    'fps_config.json',
    'payload\Bridge\Dx11FsrBridge.ini',
    'payload\OptiScaler\OptiScaler.ini',
    'payload\OptiScaler\fakenvapi.ini',
    'payload\OptiScaler\OptiScaler\OptiScaler.ini',
    'payload\OptiScaler\OptiScaler\fakenvapi.ini',
    'payload\ReShade\ReShade.ini',
    'payload\ReShade\ReShadePreset.ini'
)) {
    $preservedRelativePaths.Add($relativePath) | Out-Null
}

function Copy-DirectoryContents {
    param([string]$Source, [string]$Destination, [string]$RelativeDirectory = '')
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    foreach ($item in Get-ChildItem -LiteralPath $Source -Force) {
        $destinationPath = Join-Path $Destination $item.Name
        $relativePath = if ([string]::IsNullOrWhiteSpace($RelativeDirectory)) { $item.Name } else { Join-Path $RelativeDirectory $item.Name }
        if ($preservedRelativePaths.Contains($relativePath) -and (Test-Path -LiteralPath $destinationPath -PathType Leaf)) {
            Add-Content -LiteralPath $logPath -Value "Preserved user configuration: $relativePath" -Encoding UTF8
            continue
        }
        if ($item.PSIsContainer) {
            Copy-DirectoryContents -Source $item.FullName -Destination $destinationPath -RelativeDirectory $relativePath
        }
        else {
            Copy-Item -LiteralPath $item.FullName -Destination $destinationPath -Force
        }
    }
}

try {
    if (-not (Test-Path -LiteralPath $source -PathType Container)) { throw "Update source folder does not exist: $source" }
    if (-not (Test-Path -LiteralPath $target -PathType Container)) { throw "Update target folder does not exist: $target" }
    try { Wait-Process -Id $ParentProcessId -Timeout 60 -ErrorAction Stop } catch { Start-Sleep -Seconds 2 }
    [IO.File]::WriteAllText($logPath, "Update started: $([DateTime]::Now.ToString('s'))`r`n", [Text.UTF8Encoding]::new($false))
    Copy-DirectoryContents -Source $source -Destination $target
    Add-Content -LiteralPath $logPath -Value "Update completed: $([DateTime]::Now.ToString('s'))" -Encoding UTF8
    if (Test-Path -LiteralPath $relaunch -PathType Leaf) {
        $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $relaunch + '"'))
        if (-not [string]::IsNullOrWhiteSpace($GamePath)) { $arguments += @('-GamePath', ('"' + $GamePath + '"')) }
        $arguments += @('-Language', $Language)
        if ($ResumeUpdateAll) { $arguments += '-ResumeUpdateAll' }
        Start-Process -FilePath 'powershell.exe' -WorkingDirectory $target -ArgumentList $arguments
    }
}
catch {
    [IO.File]::WriteAllText($logPath, ($_ | Out-String), [Text.UTF8Encoding]::new($false))
    throw
}
finally {
    try { Remove-Item -LiteralPath $source -Recurse -Force -ErrorAction SilentlyContinue } catch { }
}
