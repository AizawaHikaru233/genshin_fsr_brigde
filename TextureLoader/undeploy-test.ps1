# undeploy-test.ps1 — 移除 TextureLoader 部署（两条宿主路线）
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File .\undeploy-test.ps1
#   powershell -ExecutionPolicy Bypass -File .\undeploy-test.ps1 -Route Starward
#
# ⚠️ 2026-09-19 修正（审核报告）：与 deploy-test.ps1 同步改为宿主 payload 路线。
#   旧版本从游戏目录移除 d3d11.dll / TextureLoader.ini / TextureLoader.log，
#   那是**已废弃的 d3d11 代理方案**的残留（当前产物为 TextureLoader.dll，
#   由宿主插件加载器加载，见 TextureLoader/CMakeLists.txt 第 47-48 行）。
#
# 默认只移除 DLL 与 ini；日志默认保留（便于事后排查），加 -RemoveLogs 一并删除。
# 备份文件（*.bak-*）默认保留以便回退，加 -RemoveBackups 一并删除。
param(
    [ValidateSet('Both', 'Starward', 'Fufu')]
    [string]$Route = 'Both',
    [switch]$RemoveLogs,
    [switch]$RemoveBackups
)

$ErrorActionPreference = 'Stop'

$routes = @(
    [pscustomobject]@{ Name = 'Starward'; Dir = 'D:\miHoYo Games\Starward\原神解帧FSR插件包\payload\TextureLoader' },
    [pscustomobject]@{ Name = 'Fufu';     Dir = 'D:\Program Files\FufuLauncher\Plugins\FSR-Bridge-Plugin\payload\TextureLoader' }
)
if ($Route -ne 'Both') {
    $routes = $routes | Where-Object { $_.Name -eq $Route }
}

$removed = 0
foreach ($r in $routes) {
    if (-not (Test-Path -LiteralPath $r.Dir)) { continue }
    $targets = @('TextureLoader.dll', 'TextureLoader.ini')
    if ($RemoveLogs) { $targets += @('TextureLoader.log', 'TextureLoader.log.prev-*') }
    foreach ($t in $targets) {
        Get-ChildItem -LiteralPath $r.Dir -Filter $t -ErrorAction SilentlyContinue | ForEach-Object {
            Remove-Item -LiteralPath $_.FullName -Force
            Write-Host "[$($r.Name)] removed $($_.Name)"
            $removed++
        }
    }
    if ($RemoveBackups) {
        Get-ChildItem -LiteralPath $r.Dir -Filter 'TextureLoader.dll.bak-*' -ErrorAction SilentlyContinue | ForEach-Object {
            Remove-Item -LiteralPath $_.FullName -Force
            Write-Host "[$($r.Name)] removed backup $($_.Name)"
            $removed++
        }
    }
}
if ($removed -eq 0) { Write-Host '没有需要移除的文件。' }
Write-Host '完成。'
