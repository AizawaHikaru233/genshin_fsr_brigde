# build.ps1 — 构建 TextureLoader.dll
#
# ⚠️ 2026-09-19（审核报告）：原先这里写"自包含，不依赖仓库其他目录"，**是错的**。
# 本模块的 hook 依赖 Detours，而 Detours **只存在于 `Dx11FsrBridge/third_party/detours`**
#（本目录内没有副本）。故构建**必须**在完整仓库检出中进行，或显式传
# `-DDETOURS_ROOT=<...>`（见 CMakeLists 的 TEXTURELOADER_DETOURS_ROOT）。
# CMake 配置阶段会显式校验该依赖并给出可操作的报错。
#
# 用法：powershell -ExecutionPolicy Bypass -File .\build.ps1 [-Configuration Release]
# 前提：已安装 Visual Studio（含 C++ 桌面工作负载）与 CMake；脚本用 vswhere 自动定位 VS。
param(
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $root "build"

# 用 vswhere 定位 Visual Studio 安装并导入 vcvars64 环境
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere 未找到（需要 Visual Studio 2019/2022+）。"
}
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    throw "未找到含 C++ 工具的 Visual Studio 安装。"
}
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat 未找到：$vcvars"
}
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($matches[1])" -Value $matches[2] }
}

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "cmake 不在 PATH（请安装 CMake 或从 VS 开发命令行运行）。"
}

$cmakeArgs = @(
    "-S", $root, "-B", $buildDir, "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=$Configuration"
)
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

& cmake --build $buildDir --config $Configuration
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

$dll = Join-Path $buildDir "TextureLoader.dll"
# 2026-09-19（审核报告）：显式校验产物存在。此前只打印路径不检查，
# 产物缺失时脚本仍"成功"退出（调用方以为可以部署，实际 deploy 才报错）。
if (-not (Test-Path -LiteralPath $dll)) {
    throw "构建报告成功但未找到产物: $dll"
}
$item = Get-Item -LiteralPath $dll
Write-Host ("OK -> {0}  {1} B  sha={2}" -f $dll, $item.Length,
    (Get-FileHash $dll -Algorithm SHA256).Hash.Substring(0, 8))
Write-Host '下一步：.\deploy-test.ps1（部署到宿主插件路线；产物即此 TextureLoader.dll）'
