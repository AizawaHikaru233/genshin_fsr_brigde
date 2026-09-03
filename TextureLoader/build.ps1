# build.ps1 — 构建 TextureLoader.dll（自包含，不依赖仓库其他目录）
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
Write-Host "OK -> $dll"
