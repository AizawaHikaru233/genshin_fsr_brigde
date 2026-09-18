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
    [switch]$All,
    [string]$Use
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$routes = @(
    @{ Name = 'Starward'; Dir = 'D:\miHoYo Games\Starward\原神解帧FSR插件包\payload\Bridge' },
    @{ Name = 'FufuLauncher'; Dir = 'D:\Program Files\FufuLauncher\Plugins\FSR-Bridge-Plugin\payload\Bridge' }
)

# 已归档的候选版本（把需要对比的版本放这里）
$candidates = [ordered]@{}

# 本地构建的 GitHub 基线（bc20442，翻译层 ON）——用于判定"最近改动"是否引入回归
$baselineBuilt = 'D:\FSR-baseline\build\Dx11FsrBridge.dll'
if (Test-Path -LiteralPath $baselineBuilt) {
    $candidates['github-baseline'] = @{
        Path  = $baselineBuilt
        Sha   = (Get-FileHash $baselineBuilt -Algorithm SHA256).Hash.Substring(0, 8)
        Size  = (Get-Item $baselineBuilt).Length
        Time  = (Get-Item $baselineBuilt).LastWriteTime
        Label = 'GitHub 基线 bc20442（翻译层 ON）'
    }
}

# 从 Starward 路线的 .bak-* 备份里挑选候选。
# 始终枚举（-Use 也需要）；-All 列出全部（用于向前二分定位回归引入点）。
$starward = $routes[0].Dir
$take = if ($All -or [string]::IsNullOrWhiteSpace($Use)) { 9999 } else { 9999 }
$bakIndex = 1
if (Test-Path -LiteralPath $starward) {
    foreach ($bak in (Get-ChildItem -LiteralPath $starward -Filter 'Dx11FsrBridge.dll.bak-*' |
                      Sort-Object LastWriteTime -Descending | Select-Object -First $take)) {
        $sha = (Get-FileHash $bak.FullName -Algorithm SHA256).Hash.Substring(0, 8)
        $candidates["$bakIndex"] = @{
            Path  = $bak.FullName
            Sha   = $sha
            Size  = $bak.Length
            Time  = $bak.LastWriteTime
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
    Write-Host "候选：" -ForegroundColor Cyan
    foreach ($k in $candidates.Keys) {
        $c = $candidates[$k]
        $note = ''
        switch ($c.Sha) {
            '56E3EA7D' { $note = '  ← 会话前 main（53ca30f）' }
            'E02F2A7F' { $note = '  ← 轮次 1' }
            'AFA3A69C' { $note = '  ← 轮次 3' }
            '9A20BE9E' { $note = '  ← 轮次 4' }
        }
        if ($k -eq 'github-baseline') { $note = '  ← ' + $c.Label }
        Write-Host ("  [{0}] sha={1}  {2} B  {3}{4}" -f $k, $c.Sha, $c.Size, $c.Time, $note)
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
