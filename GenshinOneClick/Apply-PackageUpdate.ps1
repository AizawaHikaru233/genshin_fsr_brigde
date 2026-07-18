param(
    [Parameter(Mandatory = $true)][int]$ParentProcessId,
    [Parameter(Mandatory = $true)][string]$SourceDirectory,
    [Parameter(Mandatory = $true)][string]$TargetDirectory,
    [Parameter(Mandatory = $true)][string]$RelaunchScript
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$source = [IO.Path]::GetFullPath($SourceDirectory)
$target = [IO.Path]::GetFullPath($TargetDirectory)
$relaunch = [IO.Path]::GetFullPath($RelaunchScript)
$logPath = Join-Path $target '.last-self-update.log'

function Copy-DirectoryContents {
    param([string]$Source, [string]$Destination)
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    foreach ($item in Get-ChildItem -LiteralPath $Source -Force) {
        $destinationPath = Join-Path $Destination $item.Name
        if ($item.PSIsContainer) {
            Copy-DirectoryContents -Source $item.FullName -Destination $destinationPath
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
    Copy-DirectoryContents -Source $source -Destination $target
    [IO.File]::WriteAllText($logPath, "Update completed: $([DateTime]::Now.ToString('s'))`r`n", [Text.UTF8Encoding]::new($false))
    if (Test-Path -LiteralPath $relaunch -PathType Leaf) {
        Start-Process -FilePath 'powershell.exe' -WorkingDirectory $target -ArgumentList @(
            '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $relaunch + '"')
        )
    }
}
catch {
    [IO.File]::WriteAllText($logPath, ($_ | Out-String), [Text.UTF8Encoding]::new($false))
    throw
}
finally {
    try { Remove-Item -LiteralPath $source -Recurse -Force -ErrorAction SilentlyContinue } catch { }
}
