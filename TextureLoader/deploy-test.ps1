# deploy-test.ps1 — 部署 TextureLoader 到游戏目录（observe 模式测试）
# 用法：powershell -ExecutionPolicy Bypass -File .\deploy-test.ps1
# 前提：已运行 build.ps1 生成 build-ninja-tloader\d3d11.dll
param(
    [string]$GameDir = "D:\miHoYo Games\Genshin Impact Game",
    [string]$BuildDir = ""   # 默认 = 本脚本同目录 \ build-ninja-tloader
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $BuildDir) { $BuildDir = Join-Path $root "build-ninja-tloader" }

$srcDll = Join-Path $BuildDir "d3d11.dll"
$srcIni = Join-Path $root "TextureLoader.ini"
if (-not (Test-Path $srcDll)) { throw "未找到 $srcDll，请先运行 build.ps1" }

Write-Host "== 部署前检查 =="
if (-not (Test-Path $GameDir)) { throw "游戏目录不存在: $GameDir" }
$existing = Join-Path $GameDir "d3d11.dll"
if (Test-Path $existing) {
    Write-Host "[警告] 游戏目录已存在 d3d11.dll：$existing"
    Write-Host "        若是 XXMI/ReShade 等注入器部署的 proxy，请勿覆盖，改用注入方式测试。"
    Write-Host "        当前跳过部署。"
    exit 1
}
if (Test-Path (Join-Path $GameDir "TextureLoader.log")) {
    Remove-Item (Join-Path $GameDir "TextureLoader.log") -Force
}

Write-Host "== 部署 =="
Copy-Item $srcDll  $GameDir -Force
Copy-Item $srcIni  $GameDir -Force
Write-Host "已复制:"
Write-Host "  $($GameDir)\d3d11.dll"
Write-Host "  $($GameDir)\TextureLoader.ini"
Write-Host ""
Write-Host "== 下一步 =="
Write-Host "1. 通过 XXMI Launcher 启动游戏（或直接启动 YuanShen.exe）"
Write-Host "2. 进入游戏加载几个角色/NPC/场景（触发贴图加载）"
Write-Host "3. 退出游戏后查看日志：$($GameDir)\TextureLoader.log"
Write-Host "   预期出现 [ok] hooked ... 与大量 [HIT]/[miss] 行"
Write-Host ""
Write-Host "测试完成后运行 .\undeploy-test.ps1 还原。"
