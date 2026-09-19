# deploy-test.ps1 — 部署 TextureLoader.dll 到宿主插件路线（本地测试用）
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File .\deploy-test.ps1
#   powershell -ExecutionPolicy Bypass -File .\deploy-test.ps1 -Route Starward
#   powershell -ExecutionPolicy Bypass -File .\deploy-test.ps1 -Route Fufu
#
# 前提：已运行 build.ps1，产物为 <root>\build\TextureLoader.dll
#
# ⚠️ 2026-09-19 修正（审核报告高严重度）：
#   旧版本脚本的前提与产物**完全不符**，按现状无法部署：
#     - 旧前提：build-ninja-tloader\d3d11.dll
#     - 实际产物：build\TextureLoader.dll（CMakeLists 第 47-48 行
#       `set_target_properties(... OUTPUT_NAME "TextureLoader")`，并注明
#       "作为普通 DLL 由宿主插件加载器（如 Starward DllList）加载"）
#   即本项目**不再**以 d3d11.dll 代理形式部署，而是作为独立 DLL 由宿主加载。
#   因此改为部署到宿主插件的 payload 目录（两条真实路线），与
#   scripts/Deploy-Bridge.ps1 的路线定义保持一致。
param(
    # 目标路线：Both（默认）/ Starward / Fufu
    [ValidateSet('Both', 'Starward', 'Fufu')]
    [string]$Route = 'Both',
    # 覆盖构建目录（默认 = 本脚本同目录 \ build）
    [string]$BuildDir = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $BuildDir) { $BuildDir = Join-Path $root 'build' }

$srcDll = Join-Path $BuildDir 'TextureLoader.dll'
# ini 的**唯一权威源**在本目录（组件源码目录），与 Bridge 的做法一致
# （Bridge 的权威源是 `Dx11FsrBridge\Dx11FsrBridge.package.ini`，`SharedResources` 下不留副本）。
# 2026-09-19：原先 `SharedResources\TextureLoader\runtime\` 另有一份同名副本，
# 两份独立维护 → 漂移后打包出去的是副本，改源码这份会**静默不生效**。
# 已删除该副本并把打包脚本一并改为从本目录取用，此处与打包脚本同源。
$srcIni = Join-Path $root 'TextureLoader.ini'
if (-not (Test-Path -LiteralPath $srcDll)) {
    throw "未找到 $srcDll —— 请先运行 build.ps1（产物为 build\TextureLoader.dll）"
}
if (-not (Test-Path -LiteralPath $srcIni)) {
    throw "未找到 $srcIni（TextureLoader.ini 权威源）"
}

# 与 scripts/Deploy-Bridge.ps1 一致的路线定义（payload\TextureLoader 子目录）
$routes = @(
    [pscustomobject]@{ Name = 'Starward'; Dir = 'D:\miHoYo Games\Starward\原神解帧FSR插件包\payload\TextureLoader' },
    [pscustomobject]@{ Name = 'Fufu';     Dir = 'D:\Program Files\FufuLauncher\Plugins\FSR-Bridge-Plugin\payload\TextureLoader' }
)
if ($Route -ne 'Both') {
    $routes = $routes | Where-Object { $_.Name -eq $Route }
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$summary = New-Object System.Collections.ArrayList

foreach ($r in $routes) {
    if (-not (Test-Path -LiteralPath $r.Dir)) {
        [void]$summary.Add("[$($r.Name)] 跳过：目录不存在 $($r.Dir)")
        continue
    }
    $dstDll = Join-Path $r.Dir 'TextureLoader.dll'
    # 覆盖前备份（可回退）
    if (Test-Path -LiteralPath $dstDll) {
        Copy-Item -LiteralPath $dstDll -Destination "$dstDll.bak-$stamp" -Force
    }
    Copy-Item -LiteralPath $srcDll -Destination $dstDll -Force
    Copy-Item -LiteralPath $srcIni -Destination (Join-Path $r.Dir 'TextureLoader.ini') -Force
    $i = Get-Item -LiteralPath $dstDll
    [void]$summary.Add(("[{0}] TextureLoader.dll {1} B sha={2}" -f $r.Name, $i.Length,
        (Get-FileHash $dstDll -Algorithm SHA256).Hash.Substring(0, 8)))
}

Write-Host ''
Write-Host '部署结果 / Deploy summary:' -ForegroundColor Green
foreach ($s in $summary) { Write-Host "  $s" }
Write-Host ''
Write-Host '== 下一步 =='
Write-Host '1. 启动游戏（通过对应启动器）'
Write-Host '2. 进入游戏加载几个角色/NPC/场景（触发贴图加载）'
Write-Host '3. 退出游戏后查看日志：<路线>\payload\TextureLoader\TextureLoader.log'
Write-Host '   预期出现 [ok] hooked ... 与大量 [MATCH]/[bind] 行、cache= 增长'
Write-Host ''
Write-Host "回退：把同目录的 TextureLoader.dll.bak-$stamp 复制回 TextureLoader.dll"
Write-Host '卸载：运行 .\undeploy-test.ps1'
