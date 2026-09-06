# undeploy-test.ps1 — 从游戏目录移除 TextureLoader 测试文件
param(
    [string]$GameDir = "D:\miHoYo Games\Genshin Impact Game"
)
$ErrorActionPreference = "Stop"
foreach ($f in @("d3d11.dll", "TextureLoader.ini", "TextureLoader.log")) {
    $p = Join-Path $GameDir $f
    if (Test-Path $p) { Remove-Item $p -Force; Write-Host "removed $p" }
}
Write-Host "完成。"
