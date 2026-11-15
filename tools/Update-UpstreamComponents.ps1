[CmdletBinding()]
param(
    [string]$WorkspaceRoot = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $PSCommandPath) '..')),
    [ValidateSet('All', 'OptiScaler', 'Dlss', 'ReShade', 'FpsUnlocker', 'FfxSdk')]
    [string]$Component = 'All'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$sharedRoot = Join-Path $WorkspaceRoot 'SharedResources'
$optiRuntime = Join-Path $sharedRoot 'OptiScaler\runtime'
$dlssRuntime = Join-Path $sharedRoot 'NVIDIA\DLSS'
$fpsRuntime = Join-Path $sharedRoot 'FpsUnlocker\runtime'
$reshadeRuntime = Join-Path $sharedRoot 'ReShade\runtime'
$ffxRuntime = Join-Path $sharedRoot 'AMD'
$versionsPath = Join-Path $sharedRoot 'upstream-versions.json'
$userAgent = 'GenshinFsrBridge-UpstreamUpdater'

# ---------------------------------------------------------------------------
# 版本基线策略（与安装脚本 Configure.ps1 / ReShadeResources.ps1 共享）：
#   - OptiScaler   : 固定 v0.9.4 正式版（构建基线，含 FFX 2.3 SDK / XeSS / XeLL / D3D12Core）
#   - DLSS         : NVIDIA Streamline 最新正式版（提取 nvngx_dlss.dll + license）
#   - ReShade      : reshade.me 官方最新正式版（Setup exe 提取 ReShade64.dll）
#   - FPS Unlocker : 本项目维护的 fork（AizawaHikaru233/genshin-fps-unlock），固定 v3.6.0
#   - FFX SDK      : GPUOpen-LibrariesAndSDKs/FidelityFX-SDK 最新 tag（AMD 官方渠道，
#                    取 Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll + 许可证）
#   - RenoDX       : 闭源组件，作者书面授权随包分发，不自动获取
# 每次构建前运行本脚本更新 SharedResources 并生成 upstream-versions.json，
# 安装脚本按同一基线下载缺失组件（不自由追 latest）。
# ---------------------------------------------------------------------------

# --- 现有 ReShade shader 基线（由安装器在用户机器上从原作者官方仓库下载，保持锁定）---
$shaderSpec = @{
    Standard = @{ Commit = '6db142b4b1a05c764222e5b0bd9a644b7ccfe1dc'; Url = 'https://github.com/crosire/reshade-shaders/archive/6db142b4b1a05c764222e5b0bd9a644b7ccfe1dc.zip'; Sha256 = '12D082C8AB1DBCB5E221E1B6116A0343F3182EE517F09BB966B117ACC7635312' }
    Lilium   = @{ Commit = '5093d4f7441d8ca793d4a04496d1b78f640418e6'; Url = 'https://github.com/EndlesslyFlowering/ReShade_HDR_shaders/archive/5093d4f7441d8ca793d4a04496d1b78f640418e6.zip'; Sha256 = 'F8972F060A35BA4EFC4F48F8CE7283F8F3B92DD8BFE421C154161539109FE016' }
    SweetFx  = @{ Commit = '16d1a42247cb5baaf660120ee35c9a33bb94649c'; Url = 'https://github.com/CeeJayDK/SweetFX/archive/16d1a42247cb5baaf660120ee35c9a33bb94649c.zip'; Sha256 = '7901037254B06B85E564F5B8774F2F59BF2503143CE1562F9AC704A3F3D74EC6' }
}

function Get-GitHubReleaseAsset {
    param([string]$Repository, [scriptblock]$AssetFilter, [string]$Tag)
    $headers = @{ 'User-Agent' = $userAgent }
    $releaseApi = if ([string]::IsNullOrWhiteSpace($Tag)) {
        "https://api.github.com/repos/$Repository/releases/latest"
    }
    else {
        "https://api.github.com/repos/$Repository/releases/tags/$Tag"
    }
    $release = Invoke-RestMethod -Headers $headers -Uri $releaseApi -TimeoutSec 30
    $asset = @($release.assets | Where-Object $AssetFilter | Select-Object -First 1)
    if ($asset.Count -eq 0) { throw "$Repository ($Tag) 的发行数据中没有匹配资产。" }
    return [pscustomobject]@{
        Tag    = [string]$release.tag_name
        Name   = [string]$asset[0].name
        Url    = [string]$asset[0].browser_download_url
        Digest = [string]$asset[0].digest
    }
}

function Invoke-Download {
    param([string]$Url, [string]$Destination)
    Remove-Item -LiteralPath $Destination -Force -ErrorAction SilentlyContinue
    Invoke-WebRequest -UseBasicParsing -Headers @{ 'User-Agent' = $userAgent } -Uri $Url -OutFile $Destination -TimeoutSec 300
    if (-not (Test-Path -LiteralPath $Destination -PathType Leaf)) { throw "下载失败: $Url" }
}

function Get-FileSha256 {
    param([string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

function Expand-ComponentPackage {
    param([string]$PackagePath, [string]$Destination)
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    if ($PackagePath -match '\.(zip)$') {
        Expand-Archive -LiteralPath $PackagePath -DestinationPath $Destination -Force
    }
    elseif ($PackagePath -match '\.(7z)$') {
        $sz = Get-Command 7z.exe -ErrorAction SilentlyContinue
        if (-not $sz) {
            foreach ($candidate in @((Join-Path $WorkspaceRoot 'tools\7zip\7z.exe'), "$env:LOCALAPPDATA\Programs\7-Zip\7z.exe", 'C:\Program Files\7-Zip\7z.exe')) {
                if (Test-Path -LiteralPath $candidate -PathType Leaf) { $sz = Get-Item -LiteralPath $candidate; break }
            }
        }
        if (-not $sz) { throw '需要 7-Zip 解压 OptiScaler 官方包。' }
        $szPath = if ($sz -is [Management.Automation.CommandInfo]) { $sz.Source } else { $sz.FullName }
        & $szPath x $PackagePath ("-o$Destination") -y | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "7-Zip 解压失败: $PackagePath" }
    }
    else {
        throw "不支持的压缩格式: $PackagePath"
    }
}

function Assert-NvidiaSignedFile {
    param([string]$Path)
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne [Management.Automation.SignatureStatus]::Valid -or
        $null -eq $signature.SignerCertificate -or
        $signature.SignerCertificate.Subject -notmatch '(?i)NVIDIA Corporation') {
        throw "NVIDIA DLSS 文件签名验证失败: $Path"
    }
}

function Update-OptiScaler {
    # 固定 v0.9.4 正式版。官方包内含 FFX 2.3 SDK（FSR 4.1.1）/ XeSS / XeLL / D3D12Core / Licenses。
    Write-Host '--- OptiScaler (固定 v0.9.4) ---' -ForegroundColor Cyan
    $asset = Get-GitHubReleaseAsset -Repository 'optiscaler/OptiScaler' -Tag 'v0.9.4' -AssetFilter { $_.name -match '^Optiscaler_.*\.7z$' }
    $work = Join-Path $env:TEMP ("GenshinUpstream-Opti-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    try {
        $package = Join-Path $work $asset.Name
        Write-Host "下载 OptiScaler $($asset.Tag) ($($asset.Name))..."
        Invoke-Download -Url $asset.Url -Destination $package
        $archiveHash = Get-FileSha256 -Path $package
        if ($asset.Digest -match '^sha256:(.+)$' -and
            -not [string]::Equals($archiveHash, $matches[1], [StringComparison]::OrdinalIgnoreCase)) {
            throw 'OptiScaler 官方包 SHA-256 校验失败。'
        }
        $expanded = Join-Path $work 'expanded'
        Expand-ComponentPackage -PackagePath $package -Destination $expanded
        $mainDll = Get-ChildItem -LiteralPath $expanded -Recurse -File -Filter 'OptiScaler.dll' | Select-Object -First 1
        if ($null -eq $mainDll) { throw '官方包中没有找到 OptiScaler.dll。' }
        $source = $mainDll.Directory.FullName

        New-Item -ItemType Directory -Force -Path $optiRuntime | Out-Null
        # 覆盖二进制/许可与官方第一手 OptiScaler.ini（default_config 模板来源，随上游版本同步；
        # 关键键由 Configure.ps1 / bootstrap 的托管设置覆写）。仅保留定制的 OptiScaler-UpscalingFiles.json
        # （curated 组件清单，Configure.ps1 读取，不随上游覆盖）。
        # 排除官方包中的帧生成/代理/其他后端组件与安装脚本——这些不进入发布包（与打包禁止名单一致）。
        $preserve = @('OptiScaler-UpscalingFiles.json')
        $exclude = @(
            'amd_fidelityfx_framegeneration_dx12.dll', 'amd_fidelityfx_vk.dll',
            'dlssg_to_fsr3_amd_is_better.dll', 'fakenvapi.dll', 'fakenvapi.ini',
            'libxess_fg.dll', 'nvngx_dlssg.dll', 'nvngx_dlssd.dll',
            'setup_linux.sh', 'setup_windows.bat', '!! README_EXTRACT ALL FILES TO GAME FOLDER !!.txt'
        )
        foreach ($item in Get-ChildItem -LiteralPath $source -Force) {
            if ($item.Name -in $exclude) { continue }
            if ($item.PSIsContainer) {
                $dest = Join-Path $optiRuntime $item.Name
                # 先删目标同名目录再复制：多次更新时避免 Copy-Item 递归到已存在
                # 目标目录产生嵌套（如 Licenses\Licenses\）。
                if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest -Recurse -Force }
                Copy-Item -LiteralPath $item.FullName -Destination $dest -Recurse -Force
            }
            elseif ($item.Name -notin $preserve) {
                Copy-Item -LiteralPath $item.FullName -Destination (Join-Path $optiRuntime $item.Name) -Force
            }
        }
        $fileVersion = [string](Get-Item -LiteralPath (Join-Path $optiRuntime 'OptiScaler.dll')).VersionInfo.FileVersion
        Write-Host "OptiScaler.dll FileVersion = $fileVersion" -ForegroundColor Green
        return [pscustomobject]@{ Tag = $asset.Tag; Asset = $asset.Name; ArchiveSha256 = $archiveHash; FileVersion = $fileVersion }
    }
    finally {
        if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
    }
}

function Update-Dlss {
    # NVIDIA Streamline 最新正式版，提取生产版 nvngx_dlss.dll + license（NVIDIA 数字签名验证）。
    Write-Host '--- NVIDIA DLSS (Streamline 最新正式版) ---' -ForegroundColor Cyan
    $asset = Get-GitHubReleaseAsset -Repository 'NVIDIA-RTX/Streamline' -AssetFilter { $_.name -match '^streamline-sdk-v[0-9.]+\.zip$' }
    $work = Join-Path $env:TEMP ("GenshinUpstream-Dlss-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    try {
        $package = Join-Path $work $asset.Name
        Write-Host "下载 Streamline $($asset.Tag) ($($asset.Name), 约 230MB)..."
        Invoke-Download -Url $asset.Url -Destination $package
        $archiveHash = Get-FileSha256 -Path $package
        if ($asset.Digest -match '^sha256:(.+)$' -and
            -not [string]::Equals($archiveHash, $matches[1], [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Streamline 官方包 SHA-256 校验失败。'
        }
        $expanded = Join-Path $work 'expanded'
        Expand-ComponentPackage -PackagePath $package -Destination $expanded
        $sourceDll = Get-ChildItem -LiteralPath $expanded -Recurse -File -Filter 'nvngx_dlss.dll' |
            Where-Object { $_.FullName -notmatch '(?i)[\\/]development[\\/]' } |
            Sort-Object @{ Expression = { if ($_.FullName -match '(?i)[\\/]bin[\\/]x64[\\/]nvngx_dlss\.dll$') { 0 } else { 1 } } }, FullName |
            Select-Object -First 1
        if ($null -eq $sourceDll) { throw 'Streamline 官方包中没有找到生产版 nvngx_dlss.dll。' }
        Assert-NvidiaSignedFile -Path $sourceDll.FullName
        New-Item -ItemType Directory -Force -Path $dlssRuntime | Out-Null
        Copy-Item -LiteralPath $sourceDll.FullName -Destination (Join-Path $dlssRuntime 'nvngx_dlss.dll') -Force
        $sourceLicense = Get-ChildItem -LiteralPath $expanded -Recurse -File -Filter 'nvngx_dlss.license.txt' | Select-Object -First 1
        if ($null -ne $sourceLicense) {
            Copy-Item -LiteralPath $sourceLicense.FullName -Destination (Join-Path $dlssRuntime 'nvngx_dlss.license.txt') -Force
        }
        $dllHash = Get-FileSha256 -Path (Join-Path $dlssRuntime 'nvngx_dlss.dll')
        Write-Host "nvngx_dlss.dll 已更新 ($($sourceDll.Name), SHA256=$dllHash)" -ForegroundColor Green
        return [pscustomobject]@{ Tag = $asset.Tag; Asset = $asset.Name; ArchiveSha256 = $archiveHash; DllSha256 = $dllHash }
    }
    finally {
        if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
    }
}

function Update-FpsUnlocker {
    # AizawaHikaru233/genshin-fps-unlock：本项目的 fork（上游 34736384，MIT，基线 netcore @ 2b85d61 = v3.5.0），
    # 从 v3.6.0 起带「高帧防检测」（客户端读到的帧率夹到 60/45/30，实际帧率不变）。
    # 固定 tag 而不是追 latest：打包基线要可复现，换版本只改这一行。
    # 必须用未签名版（unlockfps_nc.exe）才能用 DllList 加载第三方 DLL。
    Write-Host '--- FPS Unlocker (fork v3.6.0) ---' -ForegroundColor Cyan
    $asset = Get-GitHubReleaseAsset -Repository 'AizawaHikaru233/genshin-fps-unlock' -Tag 'v3.6.0' -AssetFilter { $_.name -eq 'unlockfps_nc.exe' }
    $work = Join-Path $env:TEMP ("GenshinUpstream-Unlocker-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    try {
        $package = Join-Path $work $asset.Name
        Write-Host "下载 FPS Unlocker $($asset.Tag) ($($asset.Name))..."
        Invoke-Download -Url $asset.Url -Destination $package
        $fileHash = Get-FileSha256 -Path $package
        if ($asset.Digest -match '^sha256:(.+)$' -and
            -not [string]::Equals($fileHash, $matches[1], [StringComparison]::OrdinalIgnoreCase)) {
            throw 'FPS Unlocker 发行包 SHA-256 校验失败。'
        }
        New-Item -ItemType Directory -Force -Path $fpsRuntime | Out-Null
        Copy-Item -LiteralPath $package -Destination (Join-Path $fpsRuntime 'unlockfps_nc.exe') -Force
        Write-Host "unlockfps_nc.exe 已更新 (SHA256=$fileHash)" -ForegroundColor Green
        return [pscustomobject]@{ Tag = $asset.Tag; Asset = $asset.Name; Sha256 = $fileHash }
    }
    finally {
        if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
    }
}

function Get-LatestReShadeVersion {
    # reshade.me 官网下载页：优先解析 Addon Setup 链接版本号，其次 'Version X.Y.Z' 文本。
    $page = (Invoke-WebRequest -UseBasicParsing -Headers @{ 'User-Agent' = $userAgent } -Uri 'https://reshade.me' -TimeoutSec 60).Content
    $match = [regex]::Match($page, 'ReShade_Setup_(\d+\.\d+\.\d+)_Addon\.exe')
    if (-not $match.Success) { $match = [regex]::Match($page, 'Version\s+(\d+\.\d+\.\d+)') }
    if (-not $match.Success) { throw '无法从 reshade.me 解析最新 ReShade 版本号。' }
    return $match.Groups[1].Value
}

function Expand-ReShadeSetupModule {
    param([string]$SetupPath, [string]$Destination)
    $bytes = [IO.File]::ReadAllBytes($SetupPath)
    $archiveOffset = -1
    for ($offset = 0; $offset -le $bytes.Length - 30; $offset += 512) {
        if ($bytes[$offset] -eq 0x50 -and $bytes[$offset + 1] -eq 0x4B -and
            $bytes[$offset + 2] -eq 0x03 -and $bytes[$offset + 3] -eq 0x04) {
            $archiveOffset = $offset
            break
        }
    }
    if ($archiveOffset -lt 0) { throw '官方 ReShade Setup 中未找到内嵌 ZIP。' }
    Add-Type -AssemblyName System.IO.Compression
    $memory = [IO.MemoryStream]::new($bytes, $archiveOffset, $bytes.Length - $archiveOffset, $false)
    $archive = [IO.Compression.ZipArchive]::new($memory, [IO.Compression.ZipArchiveMode]::Read, $false)
    try {
        $entry = $archive.GetEntry('ReShade64.dll')
        if ($null -eq $entry) { throw '官方 ReShade Setup 中未找到 ReShade64.dll。' }
        $source = $entry.Open()
        $target = [IO.File]::Create($Destination)
        try { $source.CopyTo($target) }
        finally { $target.Dispose(); $source.Dispose() }
    }
    finally { $archive.Dispose(); $memory.Dispose() }
}

function Update-ReShade {
    # reshade.me 官方最新正式版（含 add-on 支持版）。官方指引"Do NOT share the binaries"，
    # 故只更新本地发布包内置文件与版本基线；GitHub 发布包由安装器在用户机器上从官网下载。
    Write-Host '--- ReShade (reshade.me 最新正式版) ---' -ForegroundColor Cyan
    $version = Get-LatestReShadeVersion
    $setupUrl = "https://reshade.me/downloads/ReShade_Setup_${version}_Addon.exe"
    $work = Join-Path $env:TEMP ("GenshinUpstream-ReShade-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    try {
        $setup = Join-Path $work 'ReShade_Setup_Addon.exe'
        Write-Host "下载 ReShade $version Addon Setup ($setupUrl)..."
        try {
            Invoke-Download -Url $setupUrl -Destination $setup
        }
        catch {
            Write-Host "reshade.me 下载失败（$($_.Exception.Message)），保留现有 ReShade64.dll 与版本基线。" -ForegroundColor Yellow
            return $null
        }
        $setupHash = Get-FileSha256 -Path $setup
        $dll = Join-Path $work 'ReShade64.dll'
        Expand-ReShadeSetupModule -SetupPath $setup -Destination $dll
        $dllHash = Get-FileSha256 -Path $dll
        $fileVersion = [string](Get-Item -LiteralPath $dll).VersionInfo.FileVersion
        New-Item -ItemType Directory -Force -Path $reshadeRuntime | Out-Null
        Copy-Item -LiteralPath $dll -Destination (Join-Path $reshadeRuntime 'ReShade64.dll') -Force
        Write-Host "ReShade64.dll 已更新 (FileVersion=$fileVersion, SHA256=$dllHash)" -ForegroundColor Green
        return [pscustomobject]@{
            Version     = $version
            SetupUrl    = $setupUrl
            SetupSha256 = $setupHash
            DllSha256   = $dllHash
            DllVersion  = $fileVersion
        }
    }
    finally {
        if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
    }
}

function Update-FfxSdk {
    # FFX 运行时只认 AMD 官方渠道：FidelityFX-SDK 的 release tag（signedbin 是 AMD 签名版）。
    Write-Host '--- FFX SDK (AMD 官方 release) ---' -ForegroundColor Cyan
    $release = Invoke-RestMethod -Uri 'https://api.github.com/repos/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/latest' -Headers @{ 'User-Agent' = $userAgent } -TimeoutSec 60
    $tag = [string]$release.tag_name
    if ([string]::IsNullOrWhiteSpace($tag)) { throw '无法解析 FidelityFX-SDK 最新 tag。' }
    $rawRoot = "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/$tag"
    $work = Join-Path $env:TEMP ("GenshinUpstream-Ffx-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    try {
        $dll = Join-Path $work 'amd_fidelityfx_upscaler_dx12.dll'
        Write-Host "下载 FFX SDK $tag (signedbin/amd_fidelityfx_upscaler_dx12.dll)..."
        Invoke-Download -Url "$rawRoot/Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll" -Destination $dll
        # AMD 签名版：验签失败直接中止（这是"确实来自 AMD 官方渠道"的凭据）。
        $sig = Get-AuthenticodeSignature -LiteralPath $dll
        if ($sig.Status -ne [Management.Automation.SignatureStatus]::Valid) {
            throw "FFX 运行时签名无效（status=$($sig.Status)），已中止。"
        }
        Write-Host "  签名者: $($sig.SignerCertificate.Subject)" -ForegroundColor DarkGray
        New-Item -ItemType Directory -Force -Path $ffxRuntime | Out-Null
        Copy-Item -LiteralPath $dll -Destination (Join-Path $ffxRuntime 'amd_fidelityfx_upscaler_dx12.dll') -Force
        $licenseTarget = Join-Path $ffxRuntime 'FidelityFX-SDK-LICENSE.md'
        foreach ($candidate in @('LICENSE.md', 'LICENSE', 'Kits/FidelityFX/LICENSE.md')) {
            try {
                Invoke-Download -Url "$rawRoot/$candidate" -Destination $licenseTarget
                break
            }
            catch { }
        }
        $dllHash = Get-FileSha256 -Path (Join-Path $ffxRuntime 'amd_fidelityfx_upscaler_dx12.dll')
        $dllVersion = [string](Get-Item -LiteralPath (Join-Path $ffxRuntime 'amd_fidelityfx_upscaler_dx12.dll')).VersionInfo.FileVersion
        Write-Host "FFX 运行时已更新 ($tag, FileVersion=$dllVersion, SHA256=$dllHash)" -ForegroundColor Green
        return [pscustomobject]@{ Tag = $tag; DllVersion = $dllVersion; DllSha256 = $dllHash }
    }
    finally {
        if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
    }
}

# ---------------------------------------------------------------------------

Write-Host '==============================================' -ForegroundColor Cyan
Write-Host " 更新上游组件到 SharedResources（构建基线）[$Component]" -ForegroundColor Cyan
Write-Host '==============================================' -ForegroundColor Cyan

$opti = $null; $dlss = $null; $fps = $null; $reshade = $null; $ffx = $null
if ($Component -in @('All', 'OptiScaler')) { $opti = Update-OptiScaler }
if ($Component -in @('All', 'Dlss')) { $dlss = Update-Dlss }
if ($Component -in @('All', 'FpsUnlocker')) { $fps = Update-FpsUnlocker }
if ($Component -in @('All', 'ReShade')) { $reshade = Update-ReShade }
if ($Component -in @('All', 'FfxSdk')) { $ffx = Update-FfxSdk }

# RenoDX：闭源，作者书面授权随包分发（NOTICE-RenoDX-genshin.txt + permission.png 归档于
# SharedResources\ReShade\runtime\reshade-shaders\），不自动获取。
$renodx = [pscustomobject]@{
    Version = '0.2026.1012.0135'
    Source  = 'authorized-redistribution'
}

# 合并现有基线：单组件更新时保留其他组件的上一基线。
$existing = $null
if (Test-Path -LiteralPath $versionsPath -PathType Leaf) {
    try { $existing = Get-Content -LiteralPath $versionsPath -Raw -Encoding UTF8 | ConvertFrom-Json } catch { $existing = $null }
}
if ($null -eq $opti -and $null -ne $existing) { $opti = $existing.optiscaler }
if ($null -eq $dlss -and $null -ne $existing) { $dlss = $existing.dlss }
if ($null -eq $fps -and $null -ne $existing) { $fps = $existing.fpsunlock }
if ($null -eq $reshade -and $null -ne $existing -and $null -ne $existing.reshade) { $reshade = $existing.reshade }
if ($null -eq $ffx -and $null -ne $existing -and $null -ne $existing.ffx) { $ffx = $existing.ffx }

$versions = [ordered]@{
    generatedAt = [DateTime]::UtcNow.ToString('o')
    optiscaler  = $opti
    dlss        = $dlss
    fpsunlock   = $fps
    reshade     = $reshade
    ffx         = $ffx
    renodx      = $renodx
}
$versions | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $versionsPath -Encoding UTF8

# 自检：清单里写的哈希必须与**刚落盘的文件**一致。
# GUI 的下载校验、打包脚本的"清单 ↔ 包内实物"断言都以这份清单为准 ⇒ 这里不一致就立刻失败。
$selfChecks = @(
    @{ Path = (Join-Path $dlssRuntime 'nvngx_dlss.dll'); Expected = [string]$versions.dlss.DllSha256; Label = 'nvngx_dlss.dll' },
    @{ Path = (Join-Path $reshadeRuntime 'ReShade64.dll'); Expected = [string]$versions.reshade.DllSha256; Label = 'ReShade64.dll' },
    @{ Path = (Join-Path $fpsRuntime 'unlockfps_nc.exe'); Expected = [string]$versions.fpsunlock.Sha256; Label = 'unlockfps_nc.exe' },
    @{ Path = (Join-Path $ffxRuntime 'amd_fidelityfx_upscaler_dx12.dll'); Expected = [string]$versions.ffx.DllSha256; Label = 'amd_fidelityfx_upscaler_dx12.dll (FFX)' }
)
foreach ($item in $selfChecks) {
    if ([string]::IsNullOrWhiteSpace($item.Expected)) { throw "清单自检失败：$($item.Label) 没有 SHA-256" }
    if (-not (Test-Path -LiteralPath $item.Path -PathType Leaf)) { throw "清单自检失败：找不到 $($item.Path)" }
    $actual = (Get-FileHash -LiteralPath $item.Path -Algorithm SHA256).Hash
    if (-not [string]::Equals($actual, $item.Expected, [StringComparison]::OrdinalIgnoreCase)) {
        throw "清单自检失败：$($item.Label) 清单 $($item.Expected)，实际 $actual"
    }
    Write-Host "  清单自检通过: $($item.Label)" -ForegroundColor DarkGray
}
Write-Host ''
Write-Host "上游版本基线已写入: $versionsPath" -ForegroundColor Green
if ($null -ne $opti) { Write-Host "  OptiScaler : $($opti.Tag) (FileVersion $($opti.FileVersion))" }
if ($null -ne $dlss) { Write-Host "  DLSS       : Streamline $($dlss.Tag)" }
if ($null -ne $reshade) { Write-Host "  ReShade    : $($reshade.Version) (FileVersion $($reshade.DllVersion))" }
if ($null -ne $fps) { Write-Host "  FPS Unlock : $($fps.Tag)" }
