# Sync-TextureLoaderIni.ps1 — 消除 TextureLoader.ini 双份漂移
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File .\Sync-TextureLoaderIni.ps1           # 校验（默认）
#   powershell -ExecutionPolicy Bypass -File .\Sync-TextureLoaderIni.ps1 -Apply    # 同步
#
# 背景（2026-09-19，审核报告高严重度）：
#   `TextureLoader.ini` 在仓库里有**两份内容完全相同**的副本：
#     A) TextureLoader/TextureLoader.ini                        （源码目录，开发者就近编辑）
#     B) SharedResources/TextureLoader/runtime/TextureLoader.ini（**打包模板**）
#   `Build-OnlineInstaller.ps1` 有 4 处从 **B** 取用（第 264/327/465/490 行），
#   即 **B 是权威源**；A 仅供本地 deploy-test.ps1 使用。
#   两份内容相同但各自独立维护 → 迟早漂移，且漂移后**打包出去的是 B**，
#   开发者改 A 会以为生效了实际没有（静默失效）。
#
# 策略：B 为权威源，A 必须与 B 一致。本脚本用于校验与同步。
#   - 默认只校验，不一致时**以非零退出码**报告（可接 CI / 构建前置检查）
#   - `-Apply` 从 B 覆盖 A（单向：B → A）
param(
    [switch]$Apply
)

$ErrorActionPreference = 'Stop'
# 本脚本位于 <repo>\TextureLoader\，而两份 ini 分别在 <repo>\TextureLoader\ 与
# <repo>\SharedResources\TextureLoader\runtime\ —— 故 $repoRoot 取本脚本目录的父目录。
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir

$authoritative = Join-Path $repoRoot 'SharedResources\TextureLoader\runtime\TextureLoader.ini'
$derived       = Join-Path $scriptDir 'TextureLoader.ini'

foreach ($p in @($authoritative, $derived)) {
    if (-not (Test-Path -LiteralPath $p)) { throw "缺少文件: $p" }
}

$hAuth = (Get-FileHash -LiteralPath $authoritative -Algorithm SHA256).Hash
$hDer  = (Get-FileHash -LiteralPath $derived -Algorithm SHA256).Hash

if ($hAuth -eq $hDer) {
    Write-Host "OK：两份 TextureLoader.ini 内容一致（sha=$($hAuth.Substring(0,16))）"
    exit 0
}

Write-Host "不一致：" -ForegroundColor Yellow
Write-Host "  权威源(B) $authoritative"
Write-Host "            sha=$($hAuth.Substring(0,16))"
Write-Host "  派生(A)   $derived"
Write-Host "            sha=$($hDer.Substring(0,16))"
Write-Host ''
Write-Host '注意：打包流水线用的是 **B**（Build-OnlineInstaller.ps1 第 264/327/465/490 行）。'
Write-Host '      若你只改了 A，打包出去的内容不会变 —— 这是静默失效。'

if ($Apply) {
    Copy-Item -LiteralPath $authoritative -Destination $derived -Force
    Write-Host ''
    Write-Host "已同步：B -> A（$derived）" -ForegroundColor Green
    exit 0
}

Write-Host ''
Write-Host '运行加 -Apply 从权威源同步：' -ForegroundColor Yellow
Write-Host '  .\Sync-TextureLoaderIni.ps1 -Apply'
exit 1
