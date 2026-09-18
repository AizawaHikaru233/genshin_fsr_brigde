# Toggle-BridgeDll.ps1 — 一键在"当前版本"与"上一版本"之间切换 DLL，用于 A/B 对比。
#
# 为什么需要它：帧率差异无法从日志可靠判定（dispatch 速率强依赖游戏场景，
# 实测历史各轮在 107~270/s 之间波动，跨度比"回归"本身还大）。
# 唯一可靠的办法是**在同一场景、同一地点、背靠背对比两个 DLL**。
#
# 用法：
#   .\Toggle-BridgeDll.ps1 -List          # 列出可切换的版本
#   .\Toggle-BridgeDll.ps1 -Use 1         # 切到编号 1 的版本
#   .\Toggle-BridgeDll.ps1 -Use current   # 切回当前版本
#
# 切换前请先完全退出游戏（DLL 被占用时无法替换）。

[CmdletBinding()]
param(
    [switch]$List,
    [string]$Use
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$routes = @(
    @{ Name = 'Starward'; Dir = 'D:\miHoYo Games\Starward\原神解帧FSR插件包\payload\Bridge' },
    @{ Name = 'FufuLauncher'; Dir = 'D:\Program Files\FufuLauncher\Plugins\FSR-Bridge-Plugin\payload\Bridge' }
)

# 已归档的候选版本（把需要对比的版本放这里）
$candidates = [ordered]@{
    'current' = $null   # 占位：当前部署的 DLL 自身
}

# 从 Starward 路线的 .bak-* 备份里挑选候选（按时间倒序，取最近的几个）
$starward = $routes[0].Dir
$bakIndex = 1
if (Test-Path -LiteralPath $starward) {
    foreach ($bak in (Get-ChildItem -LiteralPath $starward -Filter 'Dx11FsrBridge.dll.bak-*' |
                      Sort-Object LastWriteTime -Descending | Select-Object -First 5)) {
        $sha = (Get-FileHash $bak.FullName -Algorithm SHA256).Hash.Substring(0, 8)
        $candidates["$bakIndex"] = @{
            Path = $bak.FullName
            Sha  = $sha
            Size = $bak.Length
            Time = $bak.LastWriteTime
            Label = $bak.Name
        }
        $bakIndex++
    }
}

if ($List -or [string]::IsNullOrWhiteSpace($Use)) {
    Write-Host "可切换的 DLL 版本：" -ForegroundColor Cyan
    foreach ($route in $routes) {
        $dll = Join-Path $route.Dir 'Dx11FsrBridge.dll'
        if (Test-Path -LiteralPath $dll) {
            $sha = (Get-FileHash $dll -Algorithm SHA256).Hash.Substring(0, 8)
            Write-Host ("  [{0}] 当前 = {1} B sha={2}" -f $route.Name, (Get-Item $dll).Length, $sha)
        }
    }
    Write-Host ""
    Write-Host "候选（备份）：" -ForegroundColor Cyan
    foreach ($k in $candidates.Keys) {
        if ($k -eq 'current') { continue }
        $c = $candidates[$k]
        Write-Host ("  [{0}] sha={1}  {2} B  {3}" -f $k, $c.Sha, $c.Size, $c.Time)
    }
    Write-Host ""
    Write-Host "切换： .\Toggle-BridgeDll.ps1 -Use 1" -ForegroundColor Yellow
    return
}

if ($Use -eq 'current') {
    Write-Host "已选择 current（无需切换）。" -ForegroundColor Green
    return
}

if (-not $candidates.Contains($Use)) { throw "未知版本编号: $Use（先运行 -List 查看）" }
$src = $candidates[$Use]

# 占用检查
foreach ($route in $routes) {
    $dll = Join-Path $route.Dir 'Dx11FsrBridge.dll'
    if (-not (Test-Path -LiteralPath $dll)) { continue }
    try {
        $fs = [IO.File]::Open($dll, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        $fs.Close()
    } catch {
        throw "[$($route.Name)] DLL 被占用——请先完全退出游戏。"
    }
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
foreach ($route in $routes) {
    $dll = Join-Path $route.Dir 'Dx11FsrBridge.dll'
    if (-not (Test-Path -LiteralPath $dll)) { continue }
    Copy-Item -LiteralPath $dll -Destination "$dll.bak-$stamp" -Force
    Copy-Item -LiteralPath $src.Path -Destination $dll -Force
    $sha = (Get-FileHash $dll -Algorithm SHA256).Hash.Substring(0, 8)
    Write-Host ("  [{0}] -> sha={1}" -f $route.Name, $sha) -ForegroundColor Green
}

Write-Host ""
Write-Host "已切换到 sha=$($src.Sha)。" -ForegroundColor Cyan
Write-Host "对比建议：同一地点、同一视角、静止观察 30 秒，记录两次的帧数。" -ForegroundColor Yellow
