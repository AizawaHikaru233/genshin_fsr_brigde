# Build-OnlineInstaller.ps1 — 发布包构建入口（本地 7z 包 / GitHub 发布包 / 芙芙商城包）。
#
# ── 打包产物清理规则（每次运行都执行；实现见 Remove-DistArtifacts）────────────
# ① 打包产物**每次编译都先清理再重建**：本脚本在编译**之前**按**模式**删除 dist 下
#    所有由本流程产生的产物文件与中间 stage 目录（模式清单见 $script:distArtifactPatterns）。
#    清理必须按模式做、不能列死文件名 —— 历史缺陷正是清理 glob 漏了 GitHub 包的命名前缀
#    `GenshinFSRBridge_v*`（含被解压出来的同名目录），于是 2.2.0 的旧包与旧目录在出
#    2.3.0 时仍留在 dist 里（有误发风险）。
#    清理失败（产物被解压工具 / 启动器 / 资源管理器预览占用）会**直接报错中止**，
#    绝不静默跳过 —— 静默跳过会让人以为"清了"其实没清。
# ② 已解压、已部署的安装目录**不清理**：本脚本只动 dist 下的构建产物，不触碰任何已部署
#    目录（例如 Starward 启动器的插件包目录、芙芙启动器的插件目录）。
#    这些目录升级一律走**手动增量替换**（覆盖同名文件、保留用户配置与用户新增文件），
#    不做整目录清空 —— 清掉会毁掉一个正在用的安装。
#    安装器侧对应说明见 README.md「构建」与 tools/FpsUnlockInstaller/README.md。
# ③ 只清 dist 下**本流程自有**的模式，**不删整个 dist**：dist 里可能还有人工放入的调试包、
#    发布笔记或验证目录，它们不属于本流程。整个删除会牵连这些无关内容。
#    唯一例外是 $script:distOwnedDirectories：完全由本脚本生成、每轮都会重建的目录。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File .\Build-OnlineInstaller.ps1 [-Configuration Release]
#   [-GithubOnly] [-FetchUpstream] [-SevenZipPath <7z.exe>]
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release',
    [switch]$GithubOnly,
    [switch]$FetchUpstream,
    [string]$SevenZipPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSCommandPath))
$installerSource = Join-Path $root 'tools\FpsUnlockInstaller'
$packageAssets = Join-Path $root 'assets\FpsUnlockPackage'
$fufuSource = Join-Path $root 'FufuGraphicsPlugin'
$fufuBuild = Join-Path $root 'build-package-fufu-graphics'
$fufuDll = Join-Path $fufuBuild 'FSR-Bridge-Plugin.dll'
$dist = Join-Path $root 'dist'
$optiRuntime = Join-Path $root 'SharedResources\OptiScaler\runtime'
$dlssRuntime = Join-Path $root 'SharedResources\NVIDIA\DLSS'
$unlockerRuntime = Join-Path $root 'SharedResources\FpsUnlocker\runtime'
$reshadeRuntime = Join-Path $root 'SharedResources\ReShade\runtime'
$bridgePackageConfig = Join-Path $root 'Dx11FsrBridge\Dx11FsrBridge.package.ini'
$bridgeBuild = Join-Path $root 'build-package-bridge'
$antiBuild = Join-Path $root 'build-package-antiplayermosaic'
$tloaderSource = Join-Path $root 'TextureLoader'
$tloaderBuild = Join-Path $root 'build-tloader-gdds'
# TextureLoader.ini 的**唯一权威源**在组件源码目录（`TextureLoader\TextureLoader.ini`），
# 与 Bridge 的做法一致（Bridge 的权威源是 `Dx11FsrBridge\Dx11FsrBridge.package.ini`，
# 打包脚本第 256/324/454/487 行从那里取用，`SharedResources` 下**不保留副本**）。
# 2026-09-19：原先 SharedResources 下另有一份同名副本，已删除并统一到源码目录。
$tloaderConfig = Join-Path $tloaderSource 'TextureLoader.ini'
$script:bridgeDll = $null
$script:antiDll = $null
$script:tloaderDll = $null

# ── 规则①：本流程在 dist 下产生的产物 / 中间目录的清理模式 ──────────────────────
# 全部按**模式**匹配（而非列死版本号文件名），新增版本号无需改这里。
# 已知产物清单（脚本内 Join-Path $dist 的全部落点）：
#   dist\原神解帧FSR插件包_v<ver>.7z                 本地发布包（7z；历史版本曾用 .zip）
#   dist\原神解帧FSR插件包Lite_* / Full_*.7z|.zip     历史命名（保留兼容）
#   dist\芙芙启动器插件包Lite_* / Full_*.zip          历史命名（保留兼容）
#   dist\FSR-Bridge-Plugin.v<ver>.zip                芙芙启动器商城包
#   dist\GenshinFSRBridge_v<ver>.zip                 GitHub 发布包在 dist 的中间产物
#                                                    （-GithubOnly 时会留在 dist）
#   dist\GenshinFSRBridge_v<ver>\                    同名目录：手工解压该包做验证时的遗留
#   dist\github-release\GenshinFSRBridge_v<ver>.zip  GitHub 发布目录（整目录由本脚本重建）
#   dist\.fps-full-stage\ .fps-github-stage\ .fufu-marketplace-stage\  中间 stage 目录
$script:distArtifactPatterns = @(
    '原神解帧FSR插件包_v*',
    '原神解帧FSR插件包Lite_*',
    '原神解帧FSR插件包Full_*',
    '芙芙启动器插件包Lite_*',
    '芙芙启动器插件包Full_*',
    # ⚠️ 这一条是历史缺陷的根因：旧清理 glob 只覆盖 `原神解帧FSR插件包_v*` 与
    # `FSR-Bridge-Plugin.v*`，漏了 GitHub 包的命名前缀，于是旧包/旧解压目录无限累积。
    # 该模式同时匹配文件（.zip）与目录（解压出来的同名目录）。
    'GenshinFSRBridge_v*',
    'FSR-Bridge-Plugin.v*',
    # 任意中间 stage：`.fps-full-stage` 以 -stage 结尾，`*.stage` 匹配不到它，
    # 故两个模式都要有（覆盖 `*.stage` 与 `*-stage` 两种命名习惯）。
    '*.stage',
    '*-stage'
)
# 完全由本脚本生成、每轮都会重建的目录：连目录本身一起删（含 github-release\GenshinFSRBridge_v*）。
$script:distOwnedDirectories = @('github-release')
$script:cleanupFailureHint = '常见原因：该文件正被占用（资源管理器预览、解压工具 / 7-Zip 窗口、' +
    '启动器或游戏正在读取该包）。请关闭占用程序后重新运行本脚本。清理失败**不会**被静默跳过 —— ' +
    '否则 dist 会同时留有旧包与新包，存在误发风险。'

function Get-BridgeVersion {
    $version = [string](Get-Item -LiteralPath $bridgeDll).VersionInfo.FileVersion
    if ($version -notmatch '^(\d+\.\d+\.\d+)') {
        throw "编译产物 Dx11FsrBridge.dll 缺少有效文件版本：$version"
    }
    return $matches[1]
}

function Invoke-Cmake {
    param([string[]]$Arguments)
    $cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
    $cmakePath = if ($null -ne $cmake) { $cmake.Path } else { $null }
    $visualStudioPath = $null
    if ($null -eq $cmakePath) {
        $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
            $visualStudioPath = & $vswhere -latest -products * -property installationPath
            $candidate = Join-Path $visualStudioPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { $cmakePath = $candidate }
        }
    }
    if ($null -eq $cmakePath) { throw '没有找到 CMake。' }
    # Ninja 生成器 + cl 直连依赖 VC 工具链环境（LIB/INCLUDE）——若当前进程未初始化
    # （如从普通 PowerShell 直接运行），先导入 vcvars64.bat 的变量再调用 cmake/ninja。
    if ([string]::IsNullOrWhiteSpace($env:LIB)) {
        if ([string]::IsNullOrWhiteSpace($visualStudioPath)) {
            $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
            if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
                $visualStudioPath = & $vswhere -latest -products * -property installationPath
            }
        }
        $vcvars = Join-Path $visualStudioPath 'VC\Auxiliary\Build\vcvars64.bat'
        if (Test-Path -LiteralPath $vcvars -PathType Leaf) {
            $envSnapshot = & cmd.exe /c "`"$vcvars`" >nul 2>&1 && set"
            foreach ($line in $envSnapshot) {
                if ($line -match '^([^=]+)=(.*)$') {
                    try { Set-Item -Path "Env:$($matches[1])" -Value $matches[2] } catch { }
                }
            }
        }
        else {
            throw "缺少 VC 工具链环境且未找到 vcvars64.bat: $vcvars"
        }
    }
    & $cmakePath @Arguments | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "CMake 命令失败: $($Arguments -join ' ')" }
}

function Build-PackageComponents {
    # VS 18 的 MSBuild 编译器检测与 CMake 4.3 不兼容（最小项目也复现
    # "compiler identification unknown"），改用 Ninja 生成器（cl 直连，不依赖 MSBuild）。
    Invoke-Cmake @('-S', (Join-Path $root 'Dx11FsrBridge'), '-B', $bridgeBuild, '-G', 'Ninja',
        "-DCMAKE_BUILD_TYPE=$Configuration",
        '-DDX11FSRBRIDGE_RELEASE_RUNTIME=ON',
        '-DDX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL=ON'
    )
    Invoke-Cmake @('--build', $bridgeBuild)
    Invoke-Cmake @('-S', (Join-Path $root 'AntiPlayerMosaic'), '-B', $antiBuild, '-G', 'Ninja', "-DCMAKE_BUILD_TYPE=$Configuration")
    Invoke-Cmake @('--build', $antiBuild)
    Invoke-Cmake @('-S', $tloaderSource, '-B', $tloaderBuild, '-G', 'Ninja', "-DCMAKE_BUILD_TYPE=$Configuration")
    Invoke-Cmake @('--build', $tloaderBuild)

    $script:bridgeDll = Get-ChildItem -LiteralPath $bridgeBuild -Recurse -File -Filter 'Dx11FsrBridge.dll' |
        Select-Object -First 1 -ExpandProperty FullName
    $script:antiDll = Get-ChildItem -LiteralPath $antiBuild -Recurse -File -Filter 'AntiPlayerMosaic.dll' |
        Select-Object -First 1 -ExpandProperty FullName
    $script:tloaderDll = Get-ChildItem -LiteralPath $tloaderBuild -Recurse -File -Filter 'TextureLoader.dll' |
        Select-Object -First 1 -ExpandProperty FullName
    if ([string]::IsNullOrWhiteSpace($script:bridgeDll) -or [string]::IsNullOrWhiteSpace($script:antiDll) -or
        [string]::IsNullOrWhiteSpace($script:tloaderDll)) {
        throw 'CMake 未生成必要的 FPS Unlock 包 DLL。'
    }
}

function Remove-BuildOutputPath {
    # 删除一个产物路径（文件或目录）。返回 $null 表示已删除或本就不存在；
    # 返回错误说明字符串表示删除失败（调用方决定是抛错还是告警）——不静默吞掉失败。
    param([Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    try {
        Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
        return $null
    }
    catch {
        return ("{0} — {1}" -f $Path, $_.Exception.Message)
    }
}

function Remove-DistArtifacts {
    # 规则①：每次运行（编译前）按模式删除 dist 下本流程产生的产物与中间 stage 目录。
    # 幂等：目录/文件不存在时跳过；连续运行不会报错。
    # 失败即中止并列出全部失败项 + 可操作提示（绝不静默跳过）。
    param([Parameter(Mandatory)][string]$DistPath)

    New-Item -ItemType Directory -Path $DistPath -Force | Out-Null
    $targets = [Collections.Generic.Dictionary[string, string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($pattern in $script:distArtifactPatterns) {
        foreach ($item in @(Get-ChildItem -LiteralPath $DistPath -Force -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -like $pattern })) {
            $targets[$item.FullName] = $item.FullName
        }
    }
    foreach ($name in $script:distOwnedDirectories) {
        $owned = Join-Path $DistPath $name
        if (Test-Path -LiteralPath $owned) { $targets[$owned] = $owned }
    }

    $failures = [Collections.Generic.List[string]]::new()
    foreach ($path in @($targets.Values | Sort-Object)) {
        $removeFailure = Remove-BuildOutputPath -Path $path
        if ($null -ne $removeFailure) { $failures.Add($removeFailure) }
        else { Write-Host "  已清理 $path" -ForegroundColor DarkGray }
    }
    if ($failures.Count -gt 0) {
        throw ("打包产物清理失败，共 $($failures.Count) 项（未静默跳过）：" +
            [Environment]::NewLine + ($failures -join [Environment]::NewLine) +
            [Environment]::NewLine + $script:cleanupFailureHint)
    }
    Write-Host "打包产物清理完成：已删除 $($targets.Count) 项旧产物 / 中间目录。" -ForegroundColor Cyan
}

function Reset-Stage {
    param([string]$Path)
    $distRoot = [IO.Path]::GetFullPath($dist).TrimEnd('\') + '\'
    $fullPath = [IO.Path]::GetFullPath($Path)
    if (-not $fullPath.StartsWith($distRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "构建目录不在 dist 下: $fullPath"
    }
    # 规则①：重建前必须先删掉旧 stage（幂等：不存在则跳过；删不掉则报错中止）
    $removeFailure = Remove-BuildOutputPath -Path $fullPath
    if ($null -ne $removeFailure) {
        throw ("中间 stage 目录清理失败：$removeFailure" + [Environment]::NewLine + $script:cleanupFailureHint)
    }
    New-Item -ItemType Directory -Path $fullPath -Force | Out-Null
}

function Copy-DirectoryContents {
    param([string]$Source, [string]$Destination)
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    Get-ChildItem -LiteralPath $Source -Force | Copy-Item -Destination $Destination -Recurse -Force
}

function Remove-NonBundledReShadeEffects {
    param([string]$ReShadeDirectory)
    $shaderRoot = Join-Path $ReShadeDirectory 'reshade-shaders'
    foreach ($name in @('Shaders', 'Textures')) {
        $path = Join-Path $shaderRoot $name
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
        New-Item -ItemType Directory -Path $path -Force | Out-Null
    }
    foreach ($pattern in @('LICENSE-ReShade_HDR_shaders-*', 'NOTICE-ReShade_HDR_shaders.txt', 'LICENSE-SweetFX-*', 'NOTICE-Downloaded-Upstream-Sources.txt')) {
        Get-ChildItem -LiteralPath $shaderRoot -File -Filter $pattern -ErrorAction SilentlyContinue | Remove-Item -Force
    }
}

function Assert-RequiredFiles {
    param([string]$Path, [string[]]$RelativePaths)
    foreach ($relativePath in $RelativePaths) {
        if (-not (Test-Path -LiteralPath (Join-Path $Path $relativePath) -PathType Leaf)) {
            throw "商城包缺少文件: $relativePath"
        }
    }
}

function Assert-CleanPackage {
    param([string]$Path)
    $forbiddenNames = @(
        'amd_fidelityfx_framegeneration_dx12.dll', 'amd_fidelityfx_vk.dll',
        'dlssg_to_fsr3_amd_is_better.dll', 'fakenvapi.dll', 'fakenvapi.ini', 'libxess_fg.dll',
        'nvngx_dlssg.dll', 'nvngx_dlssd.dll'
    )
    foreach ($file in Get-ChildItem -LiteralPath $Path -Recurse -File -Force) {
        if ($file.Name -in $forbiddenNames) { throw "商城包包含禁止文件: $($file.FullName)" }
        if ($file.Extension -in @('.log', '.dmp', '.bak')) { throw "商城包包含运行残留: $($file.FullName)" }
    }
}

function New-ZipArchive {
    param([string]$SourceDirectory, [string]$ArchivePath)
    if (Test-Path -LiteralPath $ArchivePath) { Remove-Item -LiteralPath $ArchivePath -Force }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory(
        $SourceDirectory,
        $ArchivePath,
        [IO.Compression.CompressionLevel]::Optimal,
        $false)
}

function Find-SevenZip {
    if (-not [string]::IsNullOrWhiteSpace($SevenZipPath)) {
        if (-not (Test-Path -LiteralPath $SevenZipPath -PathType Leaf)) {
            throw "指定的 7-Zip 路径不存在: $SevenZipPath"
        }
        return (Resolve-Path -LiteralPath $SevenZipPath).Path
    }

    foreach ($commandName in @('7z.exe', '7zz.exe', '7za.exe')) {
        $command = Get-Command $commandName -ErrorAction SilentlyContinue
        if ($null -ne $command) { return $command.Source }
    }

    foreach ($candidate in @(
        (Join-Path $root 'tools\7zip\7z.exe'),
        'C:\Program Files\7-Zip\7z.exe',
        'C:\Program Files (x86)\7-Zip\7z.exe',
        (Join-Path $env:LOCALAPPDATA 'Programs\7-Zip\7z.exe')
    )) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }

    throw '本地 FPS Unlock 包使用 7z 格式。请安装 7-Zip，或使用 -SevenZipPath 指定 7z.exe。'
}

function New-SevenZipArchive {
    param([string]$SourceDirectory, [string]$ArchivePath)
    $sevenZip = Find-SevenZip
    if (Test-Path -LiteralPath $ArchivePath) { Remove-Item -LiteralPath $ArchivePath -Force }

    Push-Location -LiteralPath $SourceDirectory
    try {
        # 固实压缩配合 LZMA2 优先减小本地包体积，便于满足发布渠道的体积限制。
        & $sevenZip a -t7z -mx=9 -m0=lzma2 -ms=on -mmt=on $ArchivePath '.\*' | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "7-Zip 打包失败: $ArchivePath" }
    }
    finally {
        Pop-Location
    }
}

function Assert-PackageSizeLimit {
    param([Parameter(Mandatory)][string]$ArchivePath)
    $limit = [long]100000000
    $size = (Get-Item -LiteralPath $ArchivePath).Length
    if ($size -gt $limit) {
        throw "本地 7z 包仍为 $size bytes，超过 100 MB 体积限制。请进一步精简组件后再发布。"
    }
}

function Prepare-FpsStage {
    param([string]$Stage, [switch]$LocalFull, [Parameter(Mandatory)][string]$Version)
    Reset-Stage -Path $Stage
    Copy-Item -LiteralPath (Join-Path $installerSource 'Installer.ps1') -Destination $Stage -Force
    # 打包产物根目录 README 与仓库根 README 保持同步（单一事实来源）
    Copy-Item -LiteralPath (Join-Path $root 'README.md') -Destination (Join-Path $Stage 'README.md') -Force
    Copy-Item -LiteralPath (Join-Path $installerSource 'Configure-Launcher.bat') -Destination (Join-Path $Stage '一键配置.bat') -Force
    Copy-Item -LiteralPath (Join-Path $installerSource 'Configure-Launcher.en.bat') -Destination (Join-Path $Stage 'GenshinFSRBridgeTools.bat') -Force
    $stageScripts = Join-Path $Stage 'scripts'
    New-Item -ItemType Directory -Path $stageScripts -Force | Out-Null
    foreach ($scriptName in @('Configure.ps1', 'Localization.ps1', 'InstallerCommon.ps1', 'ReShadeResources.ps1', 'Apply-PackageUpdate.ps1')) {
        Copy-Item -LiteralPath (Join-Path $installerSource $scriptName) -Destination (Join-Path $stageScripts $scriptName) -Force
    }
    Copy-Item -LiteralPath (Join-Path $packageAssets 'Feedback.txt') -Destination $Stage -Force
    [IO.File]::WriteAllText((Join-Path $Stage 'Package-Version.txt'), "$Version`r`n", [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $Stage 'NonFrameGeneration.edition'), "`r`n", [Text.UTF8Encoding]::new($false))
    # 上游版本基线（Configure.ps1 / ReShadeResources.ps1 按此下载缺失组件，保持构建基线一致）。
    $upstreamVersions = Join-Path $root 'SharedResources\upstream-versions.json'
    if (Test-Path -LiteralPath $upstreamVersions -PathType Leaf) {
        Copy-Item -LiteralPath $upstreamVersions -Destination (Join-Path $Stage 'upstream-versions.json') -Force
    }
    # FPS Unlocker（MIT 许可，随包分发合规）。UnlockerStub.dll 由 unlockfps 运行时自动生成，不随包。
    Copy-Item -LiteralPath (Join-Path $unlockerRuntime 'unlockfps_nc.exe') -Destination $Stage -Force

    $payload = Join-Path $Stage 'payload'
    $stagePayloadBridge = Join-Path $payload 'Bridge'
    $stagePayloadAnti = Join-Path $payload 'AntiPlayerMosaic'
    $stagePayloadReShade = Join-Path $payload 'ReShade'
    $stagePayloadTextureLoader = Join-Path $payload 'TextureLoader'
    $stageOpti = Join-Path $payload 'OptiScaler'
    $stageAmd = Join-Path $payload 'AMD'
    $stageDefaults = Join-Path $payload 'default_config'
    New-Item -ItemType Directory -Path $payload, $stagePayloadBridge, $stagePayloadAnti, $stagePayloadReShade, $stagePayloadTextureLoader, $stageOpti, $stageAmd, $stageDefaults -Force | Out-Null
    Copy-Item -LiteralPath $bridgeDll -Destination (Join-Path $stagePayloadBridge 'Dx11FsrBridge.dll') -Force
    Copy-Item -LiteralPath $bridgePackageConfig -Destination (Join-Path $stagePayloadBridge 'Dx11FsrBridge.ini') -Force
    Copy-Item -LiteralPath $antiDll -Destination (Join-Path $stagePayloadAnti 'AntiPlayerMosaic.dll') -Force
    # TextureLoader（纹理/Mod 加载器）：DLL + ini + DirectStorage 运行时 + 空 Mods 目录。
    # 许可文件收进模块目录内 licenses/ 子文件夹（自身 GPL-3.0 许可/溯源 +
    # 内部依赖 DirectStorage MIT/MS 许可文本）。
    # license 集中目录只放"外部单模块"（OptiScaler/ReShade/FPSUnlocker），
    # 自有模块（Bridge/AntiPlayerMosaic/TextureLoader）与项目整体同为 GPL-3.0（根 LICENSE）。
    Copy-Item -LiteralPath $tloaderDll -Destination (Join-Path $stagePayloadTextureLoader 'TextureLoader.dll') -Force
    Copy-Item -LiteralPath ($tloaderConfig) -Destination (Join-Path $stagePayloadTextureLoader 'TextureLoader.ini') -Force
    foreach ($name in @('dstorage.dll', 'dstoragecore.dll')) {
        $source = Join-Path $tloaderBuild $name
        if (Test-Path -LiteralPath $source -PathType Leaf) {
            Copy-Item -LiteralPath $source -Destination (Join-Path $stagePayloadTextureLoader $name) -Force
        }
    }
    $stageTloaderLicenses = Join-Path $stagePayloadTextureLoader 'licenses'
    New-Item -ItemType Directory -Path $stageTloaderLicenses -Force | Out-Null
    foreach ($name in @('LICENSE.GPL.txt', 'NOTICE.md', 'AUTHORS.txt')) {
        $source = Join-Path $tloaderSource $name
        if (Test-Path -LiteralPath $source -PathType Leaf) {
            Copy-Item -LiteralPath $source -Destination (Join-Path $stageTloaderLicenses $name) -Force
        }
    }
    foreach ($name in @('LICENSE.txt', 'LICENSE-CODE.txt', 'NOTICES.txt')) {
        $source = Join-Path $tloaderSource "third_party\dstorage\$name"
        if (Test-Path -LiteralPath $source -PathType Leaf) {
            Copy-Item -LiteralPath $source -Destination (Join-Path $stageTloaderLicenses "DirectStorage-$name") -Force
        }
    }
    New-Item -ItemType Directory -Path (Join-Path $stagePayloadTextureLoader 'Mods') -Force | Out-Null
    # ReShade：本地发布包内置 ReShade64.dll；GitHub 发布包不内置（ReShade 官方指引
    # "Do NOT share the binaries"，由 Configure.ps1 在用户机器上从 reshade.me 官方下载）。
    # 两种包都携带 renodx Add-on（作者书面授权）与 ReShade BSD-3 许可文本。
    if ($LocalFull) {
        Copy-DirectoryContents -Source $reshadeRuntime -Destination $stagePayloadReShade
    }
    else {
        $stageShaderRoot = Join-Path $stagePayloadReShade 'reshade-shaders'
        $stageAddons = Join-Path $stageShaderRoot 'Addons'
        New-Item -ItemType Directory -Path $stageAddons -Force | Out-Null
        foreach ($name in @('renodx-genshin.addon64')) {
            $source = Join-Path $reshadeRuntime "reshade-shaders\Addons\$name"
            if (Test-Path -LiteralPath $source -PathType Leaf) {
                Copy-Item -LiteralPath $source -Destination (Join-Path $stageAddons $name) -Force
            }
        }
        foreach ($name in @('NOTICE-RenoDX-genshin.txt', 'NOTICE-RenoDX-genshin-permission.png')) {
            $source = Join-Path $reshadeRuntime "reshade-shaders\$name"
            if (Test-Path -LiteralPath $source -PathType Leaf) {
                Copy-Item -LiteralPath $source -Destination (Join-Path $stageShaderRoot $name) -Force
            }
        }
        foreach ($name in @('LICENSE-ReShade-BSD-3-Clause.txt')) {
            $source = Join-Path $reshadeRuntime $name
            if (Test-Path -LiteralPath $source -PathType Leaf) {
                Copy-Item -LiteralPath $source -Destination (Join-Path $stagePayloadReShade $name) -Force
            }
        }
    }
    Remove-NonBundledReShadeEffects -ReShadeDirectory $stagePayloadReShade
    Remove-Item -LiteralPath (Join-Path $stagePayloadReShade 'ReShade.ini'), (Join-Path $stagePayloadReShade 'ReShadePreset.ini') -Force -ErrorAction SilentlyContinue
    # 完整组件包：OptiScaler 运行时全家桶（FFX SDK / XeSS / XeLL / D3D12Core / Licenses）始终内置。
    Copy-DirectoryContents -Source $optiRuntime -Destination $stageOpti
    # 桥默认 FSR SDK 路径（Ffx12DllPath 留空时使用 payload\AMD\）。
    Copy-Item -LiteralPath (Join-Path $optiRuntime 'amd_fidelityfx_upscaler_dx12.dll') -Destination (Join-Path $stageAmd 'amd_fidelityfx_upscaler_dx12.dll') -Force
    # AMD FFX12 SDK license（FSR4 = FFX API 2.x，从 OptiScaler\Licenses 复制）。
    Copy-Item -LiteralPath (Join-Path $optiRuntime 'Licenses\FidelityFX_v2_LICENSE.md') -Destination (Join-Path $stageAmd 'FidelityFX_v2_LICENSE.md') -Force
    # The package defaults are generated from the exact component resources used in this build.
    Copy-Item -LiteralPath $bridgePackageConfig -Destination (Join-Path $stageDefaults 'Dx11FsrBridge.ini') -Force
    Copy-Item -LiteralPath (Join-Path $optiRuntime 'OptiScaler.ini'), (Join-Path $optiRuntime 'OptiScaler-UpscalingFiles.json') -Destination $stageDefaults -Force
    Copy-Item -LiteralPath (Join-Path $reshadeRuntime 'ReShade.ini'), (Join-Path $reshadeRuntime 'ReShadePreset.ini') -Destination $stageDefaults -Force
    Copy-Item -LiteralPath ($tloaderConfig) -Destination (Join-Path $stageDefaults 'TextureLoader.ini') -Force

    # 外部组件 license 集中到独立 license 文件夹：只放"外部单模块"的 license
    # （OptiScaler/ReShade/FPSUnlocker）；模块内部依赖的 license 留在模块文件夹内
    # （OptiScaler\Licenses；TextureLoader\ 内 DirectStorage-*）；自有模块
    # （Bridge/AntiPlayerMosaic/TextureLoader）与项目整体同为 GPL-3.0（根 LICENSE）。
    $stageLicense = Join-Path $stage 'license'
    New-Item -ItemType Directory -Path $stageLicense -Force | Out-Null
    $licenseSources = @(
        @{ Source = (Join-Path $root 'SharedResources\OptiScaler-LICENSE.txt'); Target = 'OptiScaler-LICENSE.txt' },
        @{ Source = (Join-Path $root 'SharedResources\FpsUnlocker-LICENSE.txt'); Target = 'FPSUnlocker-LICENSE.txt' },
        @{ Source = (Join-Path $reshadeRuntime 'LICENSE-ReShade-BSD-3-Clause.txt'); Target = 'ReShade-LICENSE.txt' }
    )
    foreach ($entry in $licenseSources) {
        if (Test-Path -LiteralPath $entry.Source -PathType Leaf) {
            Copy-Item -LiteralPath $entry.Source -Destination (Join-Path $stageLicense $entry.Target) -Force
        }
    }

    # NVIDIA DLSS Runtime：仅本地发布包内置；GitHub 发布包不内置（Configure.ps1 首次配置时从
    # NVIDIA 官方 Streamline 发行版下载，分发主体为 NVIDIA 自身，避免第三方分发灰色）。
    if ($LocalFull) {
        $stageNvidia = Join-Path $payload 'NVIDIA\DLSS'
        New-Item -ItemType Directory -Path $stageNvidia -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $dlssRuntime 'nvngx_dlss.dll') -Destination $stageNvidia -Force
        Copy-Item -LiteralPath (Join-Path $dlssRuntime 'nvngx_dlss.license.txt') -Destination $stageNvidia -Force
    }
}

function Build-FpsPackage {
    param(
        [switch]$LocalFull,
        [Parameter(Mandatory)][string]$Version,
        [ValidateSet('Zip', 'SevenZip')][string]$ArchiveFormat = 'Zip'
    )
    $stageName = if ($LocalFull) { '.fps-full-stage' } else { '.fps-github-stage' }
    $stage = Join-Path $dist $stageName
    Prepare-FpsStage -Stage $stage -LocalFull:$LocalFull -Version $Version
    try {
        $required = @(
            '一键配置.bat', 'GenshinFSRBridgeTools.bat', 'scripts\Configure.ps1', 'scripts\ReShadeResources.ps1',
            'scripts\Apply-PackageUpdate.ps1', 'scripts\Localization.ps1', 'scripts\InstallerCommon.ps1',
            'Feedback.txt', 'Package-Version.txt', 'NonFrameGeneration.edition', 'upstream-versions.json',
            'unlockfps_nc.exe',
            'license\OptiScaler-LICENSE.txt', 'license\FPSUnlocker-LICENSE.txt', 'license\ReShade-LICENSE.txt',
            'payload\Bridge\Dx11FsrBridge.dll', 'payload\Bridge\Dx11FsrBridge.ini',
            'payload\AntiPlayerMosaic\AntiPlayerMosaic.dll',
            'payload\TextureLoader\TextureLoader.dll', 'payload\TextureLoader\TextureLoader.ini',
            'payload\TextureLoader\dstorage.dll', 'payload\TextureLoader\dstoragecore.dll',
            'payload\TextureLoader\licenses\LICENSE.GPL.txt', 'payload\TextureLoader\licenses\NOTICE.md',
            'payload\TextureLoader\licenses\DirectStorage-LICENSE.txt',
            'payload\TextureLoader\licenses\DirectStorage-LICENSE-CODE.txt',
            'payload\TextureLoader\licenses\DirectStorage-NOTICES.txt',
            'payload\ReShade\reshade-shaders\Addons\renodx-genshin.addon64',
            'payload\ReShade\reshade-shaders\NOTICE-RenoDX-genshin.txt',
            'payload\ReShade\reshade-shaders\NOTICE-RenoDX-genshin-permission.png',
            'payload\default_config\Dx11FsrBridge.ini', 'payload\default_config\OptiScaler.ini',
            'payload\default_config\OptiScaler-UpscalingFiles.json', 'payload\default_config\ReShade.ini',
            'payload\default_config\ReShadePreset.ini', 'payload\default_config\TextureLoader.ini',
            'payload\OptiScaler\OptiScaler.dll',
            'payload\OptiScaler\amd_fidelityfx_dx12.dll', 'payload\OptiScaler\amd_fidelityfx_upscaler_dx12.dll',
            'payload\OptiScaler\libxell.dll', 'payload\OptiScaler\libxess.dll',
            'payload\OptiScaler\libxess_dx11.dll', 'payload\OptiScaler\D3D12_Optiscaler\D3D12Core.dll',
            'payload\AMD\amd_fidelityfx_upscaler_dx12.dll', 'payload\AMD\FidelityFX_v2_LICENSE.md'
        )
        if ($LocalFull) {
            $required += @(
                'payload\ReShade\ReShade64.dll',
                'payload\NVIDIA\DLSS\nvngx_dlss.dll', 'payload\NVIDIA\DLSS\nvngx_dlss.license.txt'
            )
        }
        Assert-RequiredFiles -Path $stage -RelativePaths $required
        Assert-CleanPackage -Path $stage
        $extension = if ($ArchiveFormat -eq 'SevenZip') { '7z' } else { 'zip' }
        $name = if ($LocalFull) { "原神解帧FSR插件包_v$Version.$extension" } else { "GenshinFSRBridge_v$Version.$extension" }
        $archive = Join-Path $dist $name
        if ($ArchiveFormat -eq 'SevenZip') {
            New-SevenZipArchive -SourceDirectory $stage -ArchivePath $archive
            Assert-PackageSizeLimit -ArchivePath $archive
        }
        else {
            New-ZipArchive -SourceDirectory $stage -ArchivePath $archive
        }
        return $archive
    }
    finally {
        # 清理中间 stage：失败时告警而非抛错，避免覆盖 finally 之前真正的打包异常。
        $removeFailure = Remove-BuildOutputPath -Path $stage
        if ($null -ne $removeFailure) {
            Write-Warning ("中间 stage 目录未能删除：$removeFailure" + [Environment]::NewLine + $script:cleanupFailureHint)
        }
    }
}

function Build-FufuMarketplacePackage {
param([Parameter(Mandatory)][string]$Version)
$version = $Version
New-Item -ItemType Directory -Path $dist -Force | Out-Null
# 旧的 FSR-Bridge-Plugin.v*.zip 已由编译前的 Remove-DistArtifacts 按模式清除（规则①），
# 这里不再重复清理（同一套模式只保留一个权威实现）。

Invoke-Cmake @('-S', $fufuSource, '-B', $fufuBuild, '-G', 'Ninja', "-DCMAKE_BUILD_TYPE=$Configuration")
Invoke-Cmake @('--build', $fufuBuild)
if (-not (Test-Path -LiteralPath $fufuDll -PathType Leaf)) { throw "插件编译输出不存在: $fufuDll" }
if ((Get-Item -LiteralPath $fufuDll).VersionInfo.FileVersion -ne "$version.0") {
    throw "FSR-Bridge-Plugin.dll 版本未同步：实际 $((Get-Item $fufuDll).VersionInfo.FileVersion)，期望 $version.0"
}

$stage = Join-Path $dist '.fufu-marketplace-stage'
Reset-Stage -Path $stage
try {
    Copy-Item -LiteralPath $fufuDll -Destination (Join-Path $stage 'FSR-Bridge-Plugin.dll') -Force
    Copy-Item -LiteralPath (Join-Path $packageAssets 'Feedback.txt') -Destination $stage -Force
    [IO.File]::WriteAllText((Join-Path $stage 'Package-Version.txt'), "$version`r`n", [Text.UTF8Encoding]::new($false))

    $config = Get-Content -LiteralPath (Join-Path $fufuSource 'config.ini') -Raw -Encoding UTF8
    $config = ([regex]'(?m)^Name\s*=[^\r\n]*').Replace($config, 'Name = 原神FSR2桥接插件', 1)
    $config = ([regex]'(?m)^Developer\s*=[^\r\n]*').Replace($config, 'Developer = シリアCelia', 1)
    $config = ([regex]'(?m)^Version\s*=[^\r\n]*').Replace($config, "Version = $version", 1)
    [IO.File]::WriteAllText((Join-Path $stage 'config.ini'), $config, [Text.UTF8Encoding]::new($false))

    $payload = Join-Path $stage 'payload'
    $bridge = Join-Path $payload 'Bridge'
    $opti = Join-Path $payload 'OptiScaler'
    $amd = Join-Path $payload 'AMD'
    $nvidia = Join-Path $payload 'NVIDIA\DLSS'
    $reshade = Join-Path $payload 'ReShade'
    $textureLoader = Join-Path $payload 'TextureLoader'
    $defaults = Join-Path $payload 'default_config'

    New-Item -ItemType Directory -Path $bridge, $opti, $amd, $nvidia, $reshade, $textureLoader, $defaults -Force | Out-Null
    Copy-Item -LiteralPath $bridgeDll -Destination (Join-Path $bridge 'Dx11FsrBridge.dll') -Force
    Copy-Item -LiteralPath $bridgePackageConfig -Destination (Join-Path $bridge 'Dx11FsrBridge.ini') -Force
    Copy-DirectoryContents -Source $optiRuntime -Destination $opti
    Copy-Item -LiteralPath (Join-Path $optiRuntime 'amd_fidelityfx_upscaler_dx12.dll') -Destination (Join-Path $amd 'amd_fidelityfx_upscaler_dx12.dll') -Force
    # AMD FFX12 SDK license（FSR4 = FFX API 2.x，从 OptiScaler\Licenses 复制）。
    Copy-Item -LiteralPath (Join-Path $optiRuntime 'Licenses\FidelityFX_v2_LICENSE.md') -Destination (Join-Path $amd 'FidelityFX_v2_LICENSE.md') -Force
    Copy-Item -LiteralPath (Join-Path $dlssRuntime 'nvngx_dlss.dll') -Destination $nvidia -Force
    Copy-Item -LiteralPath (Join-Path $dlssRuntime 'nvngx_dlss.license.txt') -Destination $nvidia -Force
    Copy-DirectoryContents -Source $reshadeRuntime -Destination $reshade
    Remove-NonBundledReShadeEffects -ReShadeDirectory $reshade
    # TextureLoader（纹理/Mod 加载器）；许可收进模块内 licenses/ 子文件夹
    Copy-Item -LiteralPath $tloaderDll -Destination (Join-Path $textureLoader 'TextureLoader.dll') -Force
    Copy-Item -LiteralPath ($tloaderConfig) -Destination (Join-Path $textureLoader 'TextureLoader.ini') -Force
    foreach ($name in @('dstorage.dll', 'dstoragecore.dll')) {
        $source = Join-Path $tloaderBuild $name
        if (Test-Path -LiteralPath $source -PathType Leaf) {
            Copy-Item -LiteralPath $source -Destination (Join-Path $textureLoader $name) -Force
        }
    }
    $textureLoaderLicenses = Join-Path $textureLoader 'licenses'
    New-Item -ItemType Directory -Path $textureLoaderLicenses -Force | Out-Null
    foreach ($name in @('LICENSE.GPL.txt', 'NOTICE.md', 'AUTHORS.txt')) {
        $source = Join-Path $tloaderSource $name
        if (Test-Path -LiteralPath $source -PathType Leaf) {
            Copy-Item -LiteralPath $source -Destination (Join-Path $textureLoaderLicenses $name) -Force
        }
    }
    foreach ($name in @('LICENSE.txt', 'LICENSE-CODE.txt', 'NOTICES.txt')) {
        $source = Join-Path $tloaderSource "third_party\dstorage\$name"
        if (Test-Path -LiteralPath $source -PathType Leaf) {
            Copy-Item -LiteralPath $source -Destination (Join-Path $textureLoaderLicenses "DirectStorage-$name") -Force
        }
    }
    New-Item -ItemType Directory -Path (Join-Path $textureLoader 'Mods') -Force | Out-Null
    Copy-Item -LiteralPath $bridgePackageConfig -Destination (Join-Path $defaults 'Dx11FsrBridge.ini') -Force
    Copy-Item -LiteralPath (Join-Path $optiRuntime 'OptiScaler.ini'), (Join-Path $optiRuntime 'OptiScaler-UpscalingFiles.json') -Destination $defaults -Force
    Copy-Item -LiteralPath (Join-Path $reshadeRuntime 'ReShade.ini'), (Join-Path $reshadeRuntime 'ReShadePreset.ini') -Destination $defaults -Force
    Copy-Item -LiteralPath ($tloaderConfig) -Destination (Join-Path $defaults 'TextureLoader.ini') -Force
    Remove-Item -LiteralPath (Join-Path $reshade 'ReShade.ini'), (Join-Path $reshade 'ReShadePreset.ini') -Force -ErrorAction SilentlyContinue

    # 外部组件 license 集中（同 FPS 包：只放外部单模块的 license）
    $stageLicense = Join-Path $stage 'license'
    New-Item -ItemType Directory -Path $stageLicense -Force | Out-Null
    $licenseSources = @(
        @{ Source = (Join-Path $root 'SharedResources\OptiScaler-LICENSE.txt'); Target = 'OptiScaler-LICENSE.txt' },
        @{ Source = (Join-Path $root 'SharedResources\FpsUnlocker-LICENSE.txt'); Target = 'FPSUnlocker-LICENSE.txt' },
        @{ Source = (Join-Path $reshadeRuntime 'LICENSE-ReShade-BSD-3-Clause.txt'); Target = 'ReShade-LICENSE.txt' }
    )
    foreach ($entry in $licenseSources) {
        if (Test-Path -LiteralPath $entry.Source -PathType Leaf) {
            Copy-Item -LiteralPath $entry.Source -Destination (Join-Path $stageLicense $entry.Target) -Force
        }
    }

    Assert-RequiredFiles -Path $stage -RelativePaths @(
        'FSR-Bridge-Plugin.dll', 'config.ini', 'Feedback.txt', 'Package-Version.txt',
        'license\OptiScaler-LICENSE.txt', 'license\FPSUnlocker-LICENSE.txt', 'license\ReShade-LICENSE.txt',
        'payload\Bridge\Dx11FsrBridge.dll', 'payload\Bridge\Dx11FsrBridge.ini',
        'payload\OptiScaler\OptiScaler.dll', 'payload\OptiScaler\amd_fidelityfx_dx12.dll',
        'payload\OptiScaler\amd_fidelityfx_upscaler_dx12.dll', 'payload\OptiScaler\libxell.dll',
        'payload\OptiScaler\libxess.dll', 'payload\OptiScaler\libxess_dx11.dll',
        'payload\OptiScaler\D3D12_Optiscaler\D3D12Core.dll',
        'payload\AMD\amd_fidelityfx_upscaler_dx12.dll', 'payload\AMD\FidelityFX_v2_LICENSE.md',
        'payload\NVIDIA\DLSS\nvngx_dlss.dll', 'payload\NVIDIA\DLSS\nvngx_dlss.license.txt',
        'payload\TextureLoader\TextureLoader.dll', 'payload\TextureLoader\TextureLoader.ini',
        'payload\TextureLoader\dstorage.dll', 'payload\TextureLoader\dstoragecore.dll',
        'payload\TextureLoader\licenses\LICENSE.GPL.txt', 'payload\TextureLoader\licenses\NOTICE.md',
        'payload\TextureLoader\licenses\DirectStorage-LICENSE.txt',
        'payload\TextureLoader\licenses\DirectStorage-LICENSE-CODE.txt',
        'payload\TextureLoader\licenses\DirectStorage-NOTICES.txt',
        'payload\ReShade\ReShade64.dll', 'payload\ReShade\reshade-shaders\Addons\renodx-genshin.addon64',
        'payload\ReShade\reshade-shaders\NOTICE-RenoDX-genshin.txt',
        'payload\ReShade\reshade-shaders\NOTICE-RenoDX-genshin-permission.png',
        'payload\default_config\Dx11FsrBridge.ini', 'payload\default_config\OptiScaler.ini',
        'payload\default_config\OptiScaler-UpscalingFiles.json', 'payload\default_config\ReShade.ini',
        'payload\default_config\ReShadePreset.ini', 'payload\default_config\TextureLoader.ini'
    )
    Assert-CleanPackage -Path $stage

    $archive = Join-Path $dist "FSR-Bridge-Plugin.v$version.zip"
    New-ZipArchive -SourceDirectory $stage -ArchivePath $archive
    $item = Get-Item -LiteralPath $archive
    Write-Host ''
    Write-Host 'FufuLauncher 官方商城包构建完成。' -ForegroundColor Green
    Write-Host "$($item.Name)  $($item.Length) bytes  SHA256=$((Get-FileHash $archive -Algorithm SHA256).Hash)"
}
finally {
    # 清理中间 stage：失败时告警而非抛错，避免覆盖 finally 之前真正的打包异常。
    $removeFailure = Remove-BuildOutputPath -Path $stage
    if ($null -ne $removeFailure) {
        Write-Warning ("中间 stage 目录未能删除：$removeFailure" + [Environment]::NewLine + $script:cleanupFailureHint)
    }
}

return (Join-Path $dist "FSR-Bridge-Plugin.v$version.zip")
}

if ($FetchUpstream) {
    Write-Host '正在更新上游组件（构建基线）...' -ForegroundColor Cyan
    & (Join-Path $root 'tools\Update-UpstreamComponents.ps1') -WorkspaceRoot $root
    if ($LASTEXITCODE -ne 0) { throw '上游组件更新失败。' }
}

# 规则①：**编译前**先清掉本流程会产生的 dist 产物与中间 stage 目录（按模式，见
# Remove-DistArtifacts）。放在编译之前有两个好处：清理逻辑不再依赖编译产物的版本号；
# 产物被占用时会立刻报错，不会先花时间编译再失败。
Remove-DistArtifacts -DistPath $dist

Build-PackageComponents

# TextureLoader.ini 的**唯一权威源**在组件源码目录 `TextureLoader\TextureLoader.ini`
# （见上方 $tloaderConfig 与 4 处 Copy-Item）—— 与 Bridge 的做法一致：
# Bridge 的权威源是 `Dx11FsrBridge\Dx11FsrBridge.package.ini`，`SharedResources` 下不留副本。
# 2026-09-19：原先 `SharedResources\TextureLoader\runtime\` 另有一份同名副本，已删除
# 并统一到源码目录（此前两份独立维护 → 漂移后改源码那份不影响打包，属静默失效）。
# 这里做存在性断言，避免"打包时才发现模板缺失"。
if (-not (Test-Path -LiteralPath $tloaderConfig -PathType Leaf)) {
    throw "缺少 TextureLoader.ini 权威源：$tloaderConfig"
}

# 上游版本一致性由 tools\Update-UpstreamComponents.ps1 保证（官方包 SHA-256 校验 + versions.json 记录 FileVersion）。
$version = Get-BridgeVersion
# 产物清理已在编译前由 Remove-DistArtifacts 完成（规则①）。旧实现是这里的一段
# 列死文件名的清理循环：它漏掉了 `dist\GenshinFSRBridge_v*`（GitHub 包及其解压目录）
# 与 stage 目录模式，导致旧版本包被永久留在 dist 中（有误发风险）。
# 不要把清理挪回这里：清理放在编译前，不依赖编译产物版本号，且产物被占用时能早失败。

$localArchive = $null
$fufuArchive = $null
if (-not $GithubOnly) {
    # 本地发布包：内置全部组件（含 NVIDIA DLSS Runtime 与 ReShade64.dll）。
    $localArchive = Build-FpsPackage -Version $version -LocalFull -ArchiveFormat SevenZip
    $fufuArchive = Build-FufuMarketplacePackage -Version $version
}

# GitHub 发布包：组件齐全但不内置 NVIDIA DLSS 与 ReShade64.dll（两者分发均为灰色地带：
# DLSS 由 Configure.ps1 从 NVIDIA 官方 Streamline 下载；ReShade 官方指引 "Do NOT share the
# binaries"，由 Configure.ps1 从 reshade.me 官方下载）。
$githubReleaseDist = Join-Path $dist 'github-release'
# 该目录完全由本脚本生成（$script:distOwnedDirectories），其旧内容已由编译前的
# Remove-DistArtifacts 整目录清除（含 github-release\GenshinFSRBridge_v*）。这里只保证存在。
New-Item -ItemType Directory -Path $githubReleaseDist -Force | Out-Null
$githubArchive = Join-Path $githubReleaseDist "GenshinFSRBridge_v$version.zip"
$githubSourceArchive = Build-FpsPackage -Version $version -ArchiveFormat Zip
Copy-Item -LiteralPath $githubSourceArchive -Destination $githubArchive -Force
if (-not $GithubOnly) { Remove-Item -LiteralPath $githubSourceArchive -Force }

Write-Host ''
if ($GithubOnly) {
    Write-Host 'GitHub 发布包构建完成。' -ForegroundColor Green
}
else {
    Write-Host '本地发布包、GitHub 发布包与 FufuLauncher 商城包构建完成。' -ForegroundColor Green
}
foreach ($archive in @($localArchive, $fufuArchive, $githubArchive) | Where-Object { $null -ne $_ }) {
    $item = Get-Item -LiteralPath $archive
    Write-Host "$($item.FullName)  $($item.Length) bytes  SHA256=$((Get-FileHash $archive -Algorithm SHA256).Hash)"
}
