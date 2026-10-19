# 构建 Tauri 版安装器
#   产物：app\src-tauri\target\release\gfsr-installer.exe
#   再复制到 app\GenshinFSRBridgeInstaller.exe；打包脚本从这里取（payload 在包根同级）

$ErrorActionPreference = 'Stop'
Push-Location (Join-Path $PSScriptRoot 'src-tauri')
try {
    # ui\ 是编译期嵌进 exe 的：不碰一下源码，cargo 会认为没变化，改了界面也不会生效
    (Get-Item 'src\main.rs').LastWriteTime = (Get-Date).AddMinutes(-1)
    cargo build --release
    if ($LASTEXITCODE -ne 0) { throw 'cargo build 失败' }
} finally {
    Pop-Location
}

$exe = Join-Path $PSScriptRoot 'src-tauri\target\release\gfsr-installer.exe'
if (-not (Test-Path $exe)) { throw "构建产物不存在: $exe" }
Copy-Item $exe (Join-Path $PSScriptRoot 'GenshinFSRBridgeInstaller.exe') -Force
"已输出: $(Join-Path $PSScriptRoot 'GenshinFSRBridgeInstaller.exe')"
