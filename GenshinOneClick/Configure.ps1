param(
    [string]$GamePath,
    [int]$FpsTarget = 0,
    [switch]$DisableOptiScaler,
    [switch]$DisableAntiBlur,
    [switch]$DisableHDR,
    [ValidateSet('Auto', 'Manual', 'Existing')]
    [string]$UnlockerSource,
    [ValidateSet('Auto', 'Manual', 'Existing')]
    [string]$OptiScalerSource,
    [string]$UnlockerPackagePath,
    [string]$OptiScalerPackagePath,
    [switch]$EnsureNvidiaDlssOnly,
    [switch]$NonInteractive,
    [switch]$NoShortcut,
    [switch]$ResetPluginConfigsOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
[Console]::InputEncoding = [System.Text.Encoding]::UTF8
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSCommandPath))
$payload = Join-Path $root 'payload'
$bridgeDir = Join-Path $payload 'Bridge'
$optiDir = Join-Path $payload 'OptiScaler'
$reshadeDir = Join-Path $payload 'ReShade'
$bridgeDll = Join-Path $bridgeDir 'Dx11FsrBridge.dll'
$bridgeIni = Join-Path $bridgeDir 'Dx11FsrBridge.ini'
$bridgeDefaultIni = Join-Path $bridgeDir 'Dx11FsrBridge.default.ini'
$optiDll = Join-Path $optiDir 'OptiScaler.dll'
$optiIni = Join-Path $optiDir 'OptiScaler.ini'
$optiDefaultIni = Join-Path $optiDir 'OptiScaler.default.ini'
$fakeNvapiIni = Join-Path $optiDir 'fakenvapi.ini'
$fakeNvapiDefaultIni = Join-Path $optiDir 'fakenvapi.default.ini'
$optiConfigExistedAtStart = Test-Path -LiteralPath $optiIni -PathType Leaf
$bundledDlss = Join-Path $payload 'NVIDIA\DLSS\nvngx_dlss.dll'
$installedDlss = Join-Path $optiDir 'nvngx_dlss.dll'
$antiBlurDll = Join-Path $payload 'AntiPlayerMosaic.dll'
$reshadeDll = Join-Path $reshadeDir 'ReShade64.dll'
$shaderDir = Join-Path $reshadeDir 'reshade-shaders'
$unlocker = Join-Path $root 'unlockfps_nc.exe'
$fpsConfig = Join-Path $root 'fps_config.json'

function Assert-File {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "缺少文件: $Path"
    }
}

function Assert-Directory {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "缺少目录: $Path"
    }
}

function Set-JsonProperty {
    param(
        [object]$Object,
        [string]$Name,
        [object]$Value
    )
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        $Object | Add-Member -MemberType NoteProperty -Name $Name -Value $Value
    }
    else {
        $property.Value = $Value
    }
}

function Read-YesNo {
    param([string]$Prompt, [bool]$Default = $true)
    $suffix = if ($Default) { '[Y/n]' } else { '[y/N]' }
    while ($true) {
        $answer = (Read-Host "$Prompt $suffix").Trim().ToLowerInvariant()
        if ([string]::IsNullOrWhiteSpace($answer)) { return $Default }
        if ($answer -in @('y', 'yes', '1', '是')) { return $true }
        if ($answer -in @('n', 'no', '0', '否')) { return $false }
        Write-Host '请输入 y 或 n。' -ForegroundColor Yellow
    }
}

function Select-SourceMode {
    param([string]$Label, [string]$RequestedMode, [bool]$ExistingAvailable)
    if (-not [string]::IsNullOrWhiteSpace($RequestedMode)) { return $RequestedMode }
    if ($ExistingAvailable) { return 'Existing' }
    Write-Host ''
    Write-Host "$Label 获取方式：" -ForegroundColor Yellow
    Write-Host '  1. 从官方 GitHub 自动下载最新版（推荐）'
    Write-Host '  2. 使用已经手动下载的文件或目录'
    if ($ExistingAvailable) { Write-Host '  3. 使用当前目录中已有的版本' }
    while ($true) {
        $choice = (Read-Host '请输入选项').Trim()
        if ($choice -eq '1') { return 'Auto' }
        if ($choice -eq '2') { return 'Manual' }
        if ($choice -eq '3' -and $ExistingAvailable) { return 'Existing' }
        Write-Host '无效选项，请重新输入。' -ForegroundColor Yellow
    }
}

function Get-ManualPath {
    param([string]$RequestedPath, [string]$Prompt)
    $path = $RequestedPath
    if ([string]::IsNullOrWhiteSpace($path)) { $path = Read-Host $Prompt }
    $path = $path.Trim().Trim('"')
    if (-not (Test-Path -LiteralPath $path)) { throw "指定的文件或目录不存在: $path" }
    return (Resolve-Path -LiteralPath $path).Path
}

function Get-GitHubLatestAsset {
    param([string]$Repository, [scriptblock]$AssetFilter)
    $headers = @{ 'User-Agent' = 'GenshinOneClick-Installer' }
    try {
        $release = Invoke-RestMethod -Headers $headers -Uri "https://api.github.com/repos/$Repository/releases/latest"
    }
    catch {
        throw "无法查询 $Repository 官方发行版，请检查网络或改用手动下载。$([Environment]::NewLine)$($_.Exception.Message)"
    }
    $asset = @($release.assets | Where-Object $AssetFilter | Select-Object -First 1)
    if ($asset.Count -eq 0) { throw "$Repository 的最新发行版中没有找到需要的文件。" }
    return [pscustomobject]@{
        Tag = [string]$release.tag_name
        Page = [string]$release.html_url
        Name = [string]$asset[0].name
        Url = [string]$asset[0].browser_download_url
    }
}

function Invoke-OfficialDownload {
    param([string]$Url, [string]$Destination)
    try {
        Invoke-WebRequest -UseBasicParsing -Headers @{ 'User-Agent' = 'GenshinOneClick-Installer' } -Uri $Url -OutFile $Destination
    }
    catch {
        throw "下载失败，请重试或改用手动下载。$([Environment]::NewLine)$($_.Exception.Message)"
    }
}

function Get-NvidiaVideoControllers {
    $controllers = @()
    try {
        $controllers = @(Get-CimInstance -ClassName Win32_VideoController -ErrorAction Stop)
    }
    catch {
        try { $controllers = @(Get-WmiObject -Class Win32_VideoController -ErrorAction Stop) } catch { return @() }
    }
    return @($controllers | Where-Object {
        ([string]$_.PNPDeviceID -match '(?i)VEN_10DE') -or
        ([string]$_.AdapterCompatibility -match '(?i)NVIDIA') -or
        ([string]$_.Name -match '(?i)NVIDIA')
    })
}

function Assert-NvidiaSignedFile {
    param([string]$Path)
    Assert-File -Path $Path
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne [Management.Automation.SignatureStatus]::Valid -or
        $null -eq $signature.SignerCertificate -or
        $signature.SignerCertificate.Subject -notmatch '(?i)NVIDIA Corporation') {
        throw "NVIDIA DLSS 文件签名验证失败: $Path"
    }
}

function Install-NvidiaDlssIfNeeded {
    if (-not (Test-Path -LiteralPath $optiDll -PathType Leaf)) { return }
    $nvidiaControllers = @(Get-NvidiaVideoControllers)
    if ($nvidiaControllers.Count -eq 0) { return }
    if (Test-Path -LiteralPath $installedDlss -PathType Leaf) { return }

    $gpuNames = @($nvidiaControllers | ForEach-Object { [string]$_.Name } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -Unique)
    $gpuLabel = if ($gpuNames.Count -gt 0) { $gpuNames -join ' / ' } else { 'NVIDIA GPU' }
    Write-Host "检测到 $gpuLabel，正在补齐 DLSS 超分组件..." -ForegroundColor Cyan
    New-Item -ItemType Directory -Force -Path $optiDir | Out-Null

    if (Test-Path -LiteralPath $bundledDlss -PathType Leaf) {
        Assert-NvidiaSignedFile -Path $bundledDlss
        Copy-Item -LiteralPath $bundledDlss -Destination $installedDlss -Force
        $bundledLicense = Join-Path (Split-Path -Parent $bundledDlss) 'nvngx_dlss.license.txt'
        if (Test-Path -LiteralPath $bundledLicense -PathType Leaf) {
            Copy-Item -LiteralPath $bundledLicense -Destination (Join-Path $optiDir 'nvngx_dlss.license.txt') -Force
        }
        Write-Host '已安装内置 NVIDIA DLSS 超分组件。' -ForegroundColor Green
        return
    }

    $temporaryDirectory = Join-Path ([IO.Path]::GetTempPath()) ("GenshinOneClick-DLSS-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $temporaryDirectory | Out-Null
    try {
        $asset = Get-GitHubLatestAsset -Repository 'NVIDIA-RTX/Streamline' -AssetFilter { $_.name -match '^streamline-sdk-v[0-9.]+\.zip$' }
        Write-Host "正在从 NVIDIA 官方 Streamline $($asset.Tag) 下载 DLSS 组件..." -ForegroundColor Cyan
        $packagePath = Join-Path $temporaryDirectory $asset.Name
        Invoke-OfficialDownload -Url $asset.Url -Destination $packagePath
        $expanded = Join-Path $temporaryDirectory 'expanded'
        Expand-ComponentPackage -PackagePath $packagePath -Destination $expanded
        $sourceDll = Get-ChildItem -LiteralPath $expanded -Recurse -File -Filter 'nvngx_dlss.dll' |
            Where-Object { $_.FullName -notmatch '(?i)[\\/]development[\\/]' } |
            Sort-Object @{ Expression = { if ($_.FullName -match '(?i)[\\/]bin[\\/]x64[\\/]nvngx_dlss\.dll$') { 0 } else { 1 } } }, FullName |
            Select-Object -First 1
        if ($null -eq $sourceDll) { throw 'NVIDIA Streamline 官方包中没有找到生产版 nvngx_dlss.dll。' }
        Assert-NvidiaSignedFile -Path $sourceDll.FullName
        Copy-Item -LiteralPath $sourceDll.FullName -Destination $installedDlss -Force
        $sourceLicense = Get-ChildItem -LiteralPath $expanded -Recurse -File -Filter 'nvngx_dlss.license.txt' | Select-Object -First 1
        if ($null -ne $sourceLicense) {
            Copy-Item -LiteralPath $sourceLicense.FullName -Destination (Join-Path $optiDir 'nvngx_dlss.license.txt') -Force
        }
        Write-Host '已从 NVIDIA 官方来源安装 DLSS 超分组件。' -ForegroundColor Green
    }
    finally {
        if (Test-Path -LiteralPath $temporaryDirectory) { Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force }
    }
}

function Expand-ComponentPackage {
    param([string]$PackagePath, [string]$Destination)
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    $extension = [IO.Path]::GetExtension($PackagePath).ToLowerInvariant()
    if ($extension -eq '.zip') {
        Expand-Archive -LiteralPath $PackagePath -DestinationPath $Destination -Force
        return
    }
    if ($extension -eq '.7z') {
        $tar = Get-Command tar.exe -ErrorAction SilentlyContinue
        if ($null -eq $tar) { throw '系统中没有 tar.exe，无法解压 7z；请手动解压后选择解压目录。' }
        & $tar.Source -xf $PackagePath -C $Destination
        if ($LASTEXITCODE -ne 0) { throw "7z 解压失败，退出代码: $LASTEXITCODE" }
        return
    }
    throw "不支持的压缩包格式: $extension"
}

function Copy-DirectoryContents {
    param([string]$Source, [string]$Destination)
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    Get-ChildItem -LiteralPath $Source -Force | Copy-Item -Destination $Destination -Recurse -Force
}

function Install-Unlocker {
    param([string]$Mode, [string]$ManualPath)
    if ($Mode -eq 'Existing') {
        Assert-File -Path $unlocker
        return
    }
    $temporaryDirectory = Join-Path ([IO.Path]::GetTempPath()) ("GenshinOneClick-Unlocker-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $temporaryDirectory | Out-Null
    try {
        if ($Mode -eq 'Auto') {
            $asset = Get-GitHubLatestAsset -Repository '34736384/genshin-fps-unlock' -AssetFilter { $_.name -eq 'unlockfps_nc.exe' }
            Write-Host "正在从官方发行版下载 FPS Unlocker $($asset.Tag)..." -ForegroundColor Cyan
            $source = Join-Path $temporaryDirectory 'unlockfps_nc.exe'
            Invoke-OfficialDownload -Url $asset.Url -Destination $source
            Copy-Item -LiteralPath $source -Destination $unlocker -Force
            Write-Host "官方页面: $($asset.Page)"
        }
        else {
            $source = Get-ManualPath -RequestedPath $ManualPath -Prompt '请输入 unlockfps_nc.exe 或自包含 ZIP 的路径'
            if (Test-Path -LiteralPath $source -PathType Container) {
                $sourceDirectory = $source
            }
            elseif ([IO.Path]::GetExtension($source).ToLowerInvariant() -eq '.zip') {
                $sourceDirectory = Join-Path $temporaryDirectory 'expanded'
                Expand-ComponentPackage -PackagePath $source -Destination $sourceDirectory
            }
            else {
                if ([IO.Path]::GetFileName($source) -notin @('unlockfps_nc.exe', 'unlockfps_nc_signed.exe')) {
                    throw '手动选择的文件不是官方 FPS Unlocker 可执行文件。'
                }
                Copy-Item -LiteralPath $source -Destination $unlocker -Force
                Write-Host "FPS Unlocker SHA256: $((Get-FileHash -LiteralPath $unlocker -Algorithm SHA256).Hash)"
                return
            }
            $sourceExe = Get-ChildItem -LiteralPath $sourceDirectory -Recurse -File | Where-Object { $_.Name -in @('unlockfps_nc.exe', 'unlockfps_nc_signed.exe') } | Select-Object -First 1
            if ($null -eq $sourceExe) { throw '所选目录或 ZIP 中没有找到 FPS Unlocker。' }
            Copy-DirectoryContents -Source $sourceExe.Directory.FullName -Destination $root
            if ($sourceExe.Name -eq 'unlockfps_nc_signed.exe') {
                Copy-Item -LiteralPath (Join-Path $root $sourceExe.Name) -Destination $unlocker -Force
            }
        }
    }
    finally {
        if (Test-Path -LiteralPath $temporaryDirectory) { Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force }
    }
    Assert-File -Path $unlocker
    Write-Host "FPS Unlocker SHA256: $((Get-FileHash -LiteralPath $unlocker -Algorithm SHA256).Hash)"
}

function Install-OptiScaler {
    param([string]$Mode, [string]$ManualPath)
    if ($Mode -eq 'Existing') {
        Assert-File -Path $optiDll
        return
    }
    $temporaryDirectory = Join-Path ([IO.Path]::GetTempPath()) ("GenshinOneClick-OptiScaler-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $temporaryDirectory | Out-Null
    try {
        if ($Mode -eq 'Auto') {
            $asset = Get-GitHubLatestAsset -Repository 'optiscaler/OptiScaler' -AssetFilter { $_.name -match '\.7z$' }
            Write-Host "正在从官方发行版下载 OptiScaler $($asset.Tag)..." -ForegroundColor Cyan
            $packagePath = Join-Path $temporaryDirectory $asset.Name
            Invoke-OfficialDownload -Url $asset.Url -Destination $packagePath
            $expanded = Join-Path $temporaryDirectory 'expanded'
            Expand-ComponentPackage -PackagePath $packagePath -Destination $expanded
            Write-Host "官方页面: $($asset.Page)"
        }
        else {
            $source = Get-ManualPath -RequestedPath $ManualPath -Prompt '请输入 OptiScaler 完整解压目录、ZIP 或 7z 路径'
            if (Test-Path -LiteralPath $source -PathType Container) {
                $expanded = $source
            }
            else {
                $expanded = Join-Path $temporaryDirectory 'expanded'
                Expand-ComponentPackage -PackagePath $source -Destination $expanded
            }
        }
        $mainDll = Get-ChildItem -LiteralPath $expanded -Recurse -File -Filter 'OptiScaler.dll' | Select-Object -First 1
        if ($null -eq $mainDll) { throw '所选 OptiScaler 包中没有找到 OptiScaler.dll。' }
        $sourceDirectory = $mainDll.Directory.FullName
        if (-not (Test-Path -LiteralPath (Join-Path $sourceDirectory 'amd_fidelityfx_upscaler_dx12.dll') -PathType Leaf)) {
            throw 'OptiScaler 包不完整：缺少 amd_fidelityfx_upscaler_dx12.dll。'
        }
        $preservedNvidiaDirectory = Join-Path $temporaryDirectory 'preserved-nvidia'
        $preservedConfigDirectory = Join-Path $temporaryDirectory 'preserved-config'
        foreach ($configName in @('OptiScaler.ini', 'OptiScaler.default.ini', 'fakenvapi.ini', 'fakenvapi.default.ini')) {
            $existingConfig = Join-Path $optiDir $configName
            if (Test-Path -LiteralPath $existingConfig -PathType Leaf) {
                New-Item -ItemType Directory -Force -Path $preservedConfigDirectory | Out-Null
                Copy-Item -LiteralPath $existingConfig -Destination (Join-Path $preservedConfigDirectory $configName) -Force
            }
        }
        foreach ($fileName in @('nvngx_dlss.dll', 'nvngx_dlssg.dll', 'nvngx_dlssd.dll', 'nvngx_dlss.license.txt')) {
            $existingFile = Join-Path $optiDir $fileName
            if (Test-Path -LiteralPath $existingFile -PathType Leaf) {
                New-Item -ItemType Directory -Force -Path $preservedNvidiaDirectory | Out-Null
                Copy-Item -LiteralPath $existingFile -Destination (Join-Path $preservedNvidiaDirectory $fileName) -Force
            }
        }
        if (Test-Path -LiteralPath $optiDir) { Remove-Item -LiteralPath $optiDir -Recurse -Force }
        Copy-DirectoryContents -Source $sourceDirectory -Destination $optiDir
        if (Test-Path -LiteralPath $optiIni -PathType Leaf) {
            Copy-Item -LiteralPath $optiIni -Destination $optiDefaultIni -Force
        }
        if (Test-Path -LiteralPath $fakeNvapiIni -PathType Leaf) {
            Copy-Item -LiteralPath $fakeNvapiIni -Destination $fakeNvapiDefaultIni -Force
        }
        $preservedDefaultIni = Join-Path $preservedConfigDirectory 'OptiScaler.default.ini'
        if (-not (Test-Path -LiteralPath $optiDefaultIni -PathType Leaf) -and
            (Test-Path -LiteralPath $preservedDefaultIni -PathType Leaf)) {
            Copy-Item -LiteralPath $preservedDefaultIni -Destination $optiDefaultIni -Force
        }
        $preservedUserIni = Join-Path $preservedConfigDirectory 'OptiScaler.ini'
        if (Test-Path -LiteralPath $preservedUserIni -PathType Leaf) {
            Copy-Item -LiteralPath $preservedUserIni -Destination $optiIni -Force
        }
        $preservedFakeDefaultIni = Join-Path $preservedConfigDirectory 'fakenvapi.default.ini'
        if (-not (Test-Path -LiteralPath $fakeNvapiDefaultIni -PathType Leaf) -and
            (Test-Path -LiteralPath $preservedFakeDefaultIni -PathType Leaf)) {
            Copy-Item -LiteralPath $preservedFakeDefaultIni -Destination $fakeNvapiDefaultIni -Force
        }
        $preservedFakeUserIni = Join-Path $preservedConfigDirectory 'fakenvapi.ini'
        if (Test-Path -LiteralPath $preservedFakeUserIni -PathType Leaf) {
            Copy-Item -LiteralPath $preservedFakeUserIni -Destination $fakeNvapiIni -Force
        }
        if (Test-Path -LiteralPath $preservedNvidiaDirectory -PathType Container) {
            foreach ($preservedFile in @(Get-ChildItem -LiteralPath $preservedNvidiaDirectory -File)) {
                $destination = Join-Path $optiDir $preservedFile.Name
                if (-not (Test-Path -LiteralPath $destination -PathType Leaf)) {
                    Copy-Item -LiteralPath $preservedFile.FullName -Destination $destination -Force
                }
            }
        }
    }
    finally {
        if (Test-Path -LiteralPath $temporaryDirectory) { Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force }
    }
    Assert-File -Path $optiDll
    Write-Host "OptiScaler SHA256: $((Get-FileHash -LiteralPath $optiDll -Algorithm SHA256).Hash)"
}

function Resolve-GamePath {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "游戏路径不存在: $Path"
    }
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    if (Test-Path -LiteralPath $resolved -PathType Container) {
        foreach ($name in @('YuanShen.exe', 'GenshinImpact.exe')) {
            $candidate = Join-Path $resolved $name
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                return $candidate
            }
        }
        throw "目录中没有找到 YuanShen.exe 或 GenshinImpact.exe: $resolved"
    }
    if ([System.IO.Path]::GetFileName($resolved) -notin @('YuanShen.exe', 'GenshinImpact.exe')) {
        throw "不支持的游戏程序: $resolved"
    }
    return $resolved
}

function Get-GamePath {
    param(
        [string]$RequestedPath,
        [string]$ConfigPath
    )
    if (-not [string]::IsNullOrWhiteSpace($RequestedPath)) {
        return Resolve-GamePath -Path $RequestedPath
    }
    $defaultPath = $null
    if (Test-Path -LiteralPath $ConfigPath -PathType Leaf) {
        try {
            $existingConfig = Get-Content -LiteralPath $ConfigPath -Encoding UTF8 -Raw | ConvertFrom-Json
            if (-not [string]::IsNullOrWhiteSpace([string]$existingConfig.GamePath) -and
                (Test-Path -LiteralPath ([string]$existingConfig.GamePath))) {
                $defaultPath = [string]$existingConfig.GamePath
            }
        }
        catch {
        }
    }
    $prompt = '请输入 YuanShen.exe、GenshinImpact.exe 或游戏目录路径'
    if (-not [string]::IsNullOrWhiteSpace($defaultPath)) {
        $prompt += "（直接回车使用 $defaultPath）"
    }
    $inputPath = Read-Host $prompt
    if ([string]::IsNullOrWhiteSpace($inputPath)) {
        if (-not [string]::IsNullOrWhiteSpace($defaultPath)) {
            return Resolve-GamePath -Path $defaultPath
        }
        throw '未提供游戏路径。'
    }
    return Resolve-GamePath -Path $inputPath.Trim('"')
}

function Get-PathKind {
    param([string]$Path)
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
    if ($null -eq $item) {
        $parent = Split-Path -Parent $Path
        $leaf = Split-Path -Leaf $Path
        if (Test-Path -LiteralPath $parent -PathType Container) {
            $item = Get-ChildItem -LiteralPath $parent -Force | Where-Object { $_.Name -eq $leaf } | Select-Object -First 1
        }
    }
    if ($null -eq $item) {
        return 'Missing'
    }
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        return 'Junction'
    }
    if ($item.PSIsContainer) {
        return 'Directory'
    }
    return 'File'
}

function Remove-ManagedPath {
    param([string]$Path)
    $kind = Get-PathKind -Path $Path
    if ($kind -eq 'Missing') {
        return
    }
    if ($kind -eq 'Junction') {
        [System.IO.Directory]::Delete($Path)
        return
    }
    if (Test-Path -LiteralPath $Path -PathType Container) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
    else {
        Remove-Item -LiteralPath $Path -Force
    }
}

function Set-IniValue {
    param(
        [string]$Path,
        [string]$Section,
        [string]$Key,
        [string]$Value
    )
    $lines = [System.Collections.Generic.List[string]]::new()
    if (Test-Path -LiteralPath $Path) {
        foreach ($line in Get-Content -LiteralPath $Path -Encoding UTF8) {
            $lines.Add($line)
        }
    }
    $sectionStart = -1
    $sectionEnd = $lines.Count
    for ($index = 0; $index -lt $lines.Count; $index++) {
        if ($lines[$index] -match '^\s*\[(.+)\]\s*$') {
            if ($sectionStart -ge 0) {
                $sectionEnd = $index
                break
            }
            if ($matches[1] -eq $Section) {
                $sectionStart = $index
            }
        }
    }
    if ($sectionStart -lt 0) {
        if ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -ne '') {
            $lines.Add('')
        }
        $lines.Add("[$Section]")
        $lines.Add("$Key = $Value")
    }
    else {
        $keyIndex = -1
        for ($index = $sectionStart + 1; $index -lt $sectionEnd; $index++) {
            if ($lines[$index] -match ('^\s*' + [regex]::Escape($Key) + '\s*=')) {
                $keyIndex = $index
                break
            }
        }
        if ($keyIndex -ge 0) {
            $lines[$keyIndex] = "$Key = $Value"
        }
        else {
            $lines.Insert($sectionEnd, "$Key = $Value")
        }
    }
    [IO.File]::WriteAllLines($Path, $lines, [Text.UTF8Encoding]::new($false))
}

function New-ReShadeConfig {
    param(
        [string]$ShaderPath,
        [string]$TexturePath,
        [string]$PresetPath,
        [string]$ScreenshotPath
    )
    @"
[ADDON]
AddonPath=
DisabledAddons=
OverlayCollapsed=

[DEPTH]
DepthCopyAtClearIndex=0
DepthCopyBeforeClears=0
UseAspectRatioHeuristics=1

[GENERAL]
EffectSearchPaths=$ShaderPath
TextureSearchPaths=$TexturePath
PresetPath=$PresetPath
IntermediateCachePath=%TEMP%\ReShade
NoDebugInfo=1
NoEffectCache=0
NoReloadOnInit=0
PerformanceMode=0
PreprocessorDefinitions=RESHADE_DEPTH_LINEARIZATION_FAR_PLANE=1000.0,RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN=1,RESHADE_DEPTH_INPUT_IS_REVERSED=1,RESHADE_DEPTH_INPUT_IS_LOGARITHMIC=0
SkipLoadingDisabledEffects=1
StartupPresetPath=
TutorialProgress=4

[INPUT]
ForceShortcutModifiers=1
InputProcessing=2
KeyEffects=189,0,0,0
KeyMenu=36,0,0,0
KeyOverlay=36,0,0,0
KeyReload=187,0,0,0
KeyScreenshot=44,0,0,0

[OVERLAY]
AutoSavePreset=1
FPSPosition=1
HdrOverlayBrightness=203.000000
SaveWindowState=0
ShowFPS=0
ShowFrameTime=0
ShowPresetName=0
TutorialProgress=4

[SCREENSHOT]
ClearAlpha=1
FileFormat=1
FileNaming=%AppName% %Date% %Time%_%TimeMS%
JPEGQuality=100
SaveBeforeShot=1
SaveOverlayShot=1
SavePath=$ScreenshotPath
SavePresetFile=0
"@
}

function Reset-PluginConfigurations {
    param([string]$RequestedGamePath)
    $resolvedGameExe = Get-GamePath -RequestedPath $RequestedGamePath -ConfigPath $fpsConfig
    $gameDirectory = Split-Path -Parent $resolvedGameExe
    $reshadeIniPath = Join-Path $gameDirectory 'ReShade.ini'
    $reshadePresetPath = Join-Path $gameDirectory 'ReShadePreset.ini'
    $screenshotsPath = Join-Path $gameDirectory 'Screenshots'

    $existingConfig = $null
    if (Test-Path -LiteralPath $fpsConfig -PathType Leaf) {
        try { $existingConfig = Get-Content -LiteralPath $fpsConfig -Raw -Encoding UTF8 | ConvertFrom-Json } catch { $existingConfig = $null }
    }
    $loadedDlls = [Collections.Generic.List[string]]::new()
    if ($null -ne $existingConfig -and $null -ne $existingConfig.PSObject.Properties['DllList']) {
        foreach ($entry in @($existingConfig.DllList)) { $loadedDlls.Add([string]$entry) }
    }
    else {
        foreach ($candidate in @($bridgeDll, $optiDll, $antiBlurDll, $reshadeDll)) {
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { $loadedDlls.Add($candidate) }
        }
    }
    $hdrEnabled = $false
    foreach ($entry in $loadedDlls) {
        try {
            if ([string]::Equals([IO.Path]::GetFullPath($entry), [IO.Path]::GetFullPath($reshadeDll), [StringComparison]::OrdinalIgnoreCase)) {
                $hdrEnabled = $true
                break
            }
        }
        catch {}
    }

    if (Test-Path -LiteralPath $optiDll -PathType Leaf) {
        Assert-File -Path $optiDefaultIni
        Copy-Item -LiteralPath $optiDefaultIni -Destination $optiIni -Force
        foreach ($setting in @(
            @{ Section = 'Upscalers'; Key = 'Dx11Upscaler'; Value = 'auto' },
            @{ Section = 'Upscalers'; Key = 'Dx12Upscaler'; Value = 'auto' },
            @{ Section = 'Upscalers'; Key = 'VulkanUpscaler'; Value = 'auto' },
            @{ Section = 'FrameGen'; Key = 'Enabled'; Value = 'auto' },
            @{ Section = 'FrameGen'; Key = 'FGInput'; Value = 'auto' },
            @{ Section = 'FrameGen'; Key = 'FGOutput'; Value = 'auto' },
            @{ Section = 'FrameGen'; Key = 'FTInput'; Value = 'auto' },
            @{ Section = 'Inputs'; Key = 'EnableFsr2Inputs'; Value = 'true' },
            @{ Section = 'Inputs'; Key = 'UseFsr2Dx11Inputs'; Value = 'true' },
            @{ Section = 'Inputs'; Key = 'UseFsr2Inputs'; Value = 'true' },
            @{ Section = 'Inputs'; Key = 'EnableFsr3Inputs'; Value = 'false' },
            @{ Section = 'FSR'; Key = 'Fsr4Update'; Value = 'true' },
            @{ Section = 'Log'; Key = 'LogToFile'; Value = 'true' },
            @{ Section = 'Log'; Key = 'LogLevel'; Value = '2' },
            @{ Section = 'Log'; Key = 'SingleFile'; Value = 'true' },
            @{ Section = 'Log'; Key = 'LogFileName'; Value = 'OptiScaler.log' },
            @{ Section = 'Log'; Key = 'LogAsync'; Value = 'true' },
            @{ Section = 'Log'; Key = 'LogAsyncThreads'; Value = '1' },
            @{ Section = 'Libraries'; Key = 'OptiDllPath'; Value = $optiDir },
            @{ Section = 'Plugins'; Key = 'Path'; Value = 'auto' },
            @{ Section = 'Plugins'; Key = 'LoadAsiPlugins'; Value = 'false' },
            @{ Section = 'Plugins'; Key = 'LoadReshade'; Value = 'false' }
        )) {
            Set-IniValue -Path $optiIni -Section $setting.Section -Key $setting.Key -Value $setting.Value
        }
        if (Test-Path -LiteralPath $fakeNvapiDefaultIni -PathType Leaf) {
            Copy-Item -LiteralPath $fakeNvapiDefaultIni -Destination $fakeNvapiIni -Force
        }
    }
    if (Test-Path -LiteralPath $bridgeDll -PathType Leaf) {
        Assert-File -Path $bridgeDefaultIni
        Copy-Item -LiteralPath $bridgeDefaultIni -Destination $bridgeIni -Force
    }
    if (Test-Path -LiteralPath $reshadeDll -PathType Leaf) {
        New-Item -ItemType Directory -Force -Path $screenshotsPath | Out-Null
        $reshadeConfig = New-ReShadeConfig `
            -ShaderPath (Join-Path $shaderDir 'Shaders') `
            -TexturePath (Join-Path $shaderDir 'Textures') `
            -PresetPath $reshadePresetPath `
            -ScreenshotPath $screenshotsPath
        Set-Content -LiteralPath $reshadeIniPath -Value $reshadeConfig -Encoding UTF8
        Set-Content -LiteralPath $reshadePresetPath -Value "[GENERAL]`nPreprocessorDefinitions=`nTechniqueSorting=" -Encoding UTF8
    }

    [ordered]@{
        GamePath = $resolvedGameExe
        AutoStart = $true
        AutoClose = $true
        PopupWindow = $true
        Fullscreen = $false
        UseCustomRes = $false
        IsExclusiveFullscreen = $false
        StartMinimized = $true
        UsePowerSave = $false
        SuspendLoad = $false
        UseMobileUI = $false
        UseHDR = $hdrEnabled
        FPSTarget = 60
        CustomResX = 1920
        CustomResY = 1080
        MonitorNum = 1
        Priority = 3
        AdditionalCommandLine = ''
        LastVersionNotify = 0
        DllList = @($loadedDlls)
    } | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $fpsConfig -Encoding UTF8
    [ordered]@{ GamePath = $resolvedGameExe; FpsTarget = 60 } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root '.installer-state.json') -Encoding UTF8
}

if ($ResetPluginConfigsOnly) {
    Reset-PluginConfigurations -RequestedGamePath $GamePath
    exit 0
}

if ($EnsureNvidiaDlssOnly) {
    Install-NvidiaDlssIfNeeded
    exit 0
}

$gameExe = Get-GamePath -RequestedPath $GamePath -ConfigPath $fpsConfig

if (-not $NonInteractive) {
    Write-Host ''
    Write-Host '请选择需要安装的组件：' -ForegroundColor Yellow
    $DisableOptiScaler = -not (Read-YesNo -Prompt '启用 FSR Bridge + OptiScaler' -Default $true)
    $DisableAntiBlur = -not (Read-YesNo -Prompt '启用反虚化/隐藏 UID' -Default $true)
    $DisableHDR = -not (Read-YesNo -Prompt '启用 ReShade HDR' -Default $true)
    while ($FpsTarget -le 0) {
        $fpsInput = (Read-Host '请输入帧率上限（直接回车使用 300）').Trim()
        if ([string]::IsNullOrWhiteSpace($fpsInput)) {
            $FpsTarget = 300
        }
        elseif (-not [int]::TryParse($fpsInput, [ref]$FpsTarget) -or $FpsTarget -le 0) {
            $FpsTarget = 0
            Write-Host '请输入大于 0 的整数。' -ForegroundColor Yellow
        }
    }
}
elseif ($FpsTarget -le 0) {
    $FpsTarget = 300
}

if ($NonInteractive -and [string]::IsNullOrWhiteSpace($UnlockerSource)) {
    $UnlockerSource = if (Test-Path -LiteralPath $unlocker -PathType Leaf) { 'Existing' } else { 'Auto' }
}
if ($NonInteractive -and [string]::IsNullOrWhiteSpace($OptiScalerSource)) {
    $OptiScalerSource = if (Test-Path -LiteralPath $optiDll -PathType Leaf) { 'Existing' } else { 'Auto' }
}
$unlockerMode = Select-SourceMode -Label 'FPS Unlocker' -RequestedMode $UnlockerSource -ExistingAvailable (Test-Path -LiteralPath $unlocker -PathType Leaf)
Install-Unlocker -Mode $unlockerMode -ManualPath $UnlockerPackagePath

if (-not $DisableOptiScaler) {
    $optiMode = Select-SourceMode -Label 'OptiScaler' -RequestedMode $OptiScalerSource -ExistingAvailable (Test-Path -LiteralPath $optiDll -PathType Leaf)
    Install-OptiScaler -Mode $optiMode -ManualPath $OptiScalerPackagePath
    Install-NvidiaDlssIfNeeded
    Assert-File -Path $bridgeDll
    Assert-File -Path $bridgeIni
    $ffxMainCandidates = @(
        (Join-Path $optiDir 'amd_fidelityfx_dx12.dll'),
        (Join-Path $optiDir 'amd_fidelityfx_loader_dx12.dll')
    )
    if (-not ($ffxMainCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf })) {
        throw "OptiScaler 依赖不完整：请将完整发行包解压到 $optiDir"
    }
    Assert-File -Path (Join-Path $optiDir 'amd_fidelityfx_upscaler_dx12.dll')
}
if (-not $DisableAntiBlur) {
    Assert-File -Path $antiBlurDll
}
if (-not $DisableHDR) {
    Assert-File -Path $reshadeDll
    Assert-Directory -Path $shaderDir
}

$gameDir = Split-Path -Parent $gameExe
$legacyRuntimeLink = Join-Path $gameDir 'GIUnifiedRuntime'
$reshadeIni = Join-Path $gameDir 'ReShade.ini'
$reshadePreset = Join-Path $gameDir 'ReShadePreset.ini'
$screenshots = Join-Path $gameDir 'Screenshots'
$shortcutPath = Join-Path ([Environment]::GetFolderPath('Desktop')) '原神.lnk'
$legacyShortcutPath = Join-Path ([Environment]::GetFolderPath('Desktop')) '原神整合版.lnk'

if ((Get-PathKind -Path $legacyRuntimeLink) -eq 'Junction') {
    Remove-ManagedPath -Path $legacyRuntimeLink
}

if (-not $DisableOptiScaler) {
    if (-not $optiConfigExistedAtStart) {
        Set-IniValue -Path $optiIni -Section 'Upscalers' -Key 'Dx11Upscaler' -Value 'auto'
        Set-IniValue -Path $optiIni -Section 'Upscalers' -Key 'Dx12Upscaler' -Value 'auto'
        Set-IniValue -Path $optiIni -Section 'Upscalers' -Key 'VulkanUpscaler' -Value 'auto'
        Set-IniValue -Path $optiIni -Section 'FrameGen' -Key 'Enabled' -Value 'auto'
        Set-IniValue -Path $optiIni -Section 'FrameGen' -Key 'FGInput' -Value 'auto'
        Set-IniValue -Path $optiIni -Section 'FrameGen' -Key 'FGOutput' -Value 'auto'
        Set-IniValue -Path $optiIni -Section 'FrameGen' -Key 'FTInput' -Value 'auto'
        Set-IniValue -Path $optiIni -Section 'Inputs' -Key 'EnableFsr2Inputs' -Value 'true'
        Set-IniValue -Path $optiIni -Section 'Inputs' -Key 'UseFsr2Dx11Inputs' -Value 'true'
        Set-IniValue -Path $optiIni -Section 'Inputs' -Key 'UseFsr2Inputs' -Value 'true'
        Set-IniValue -Path $optiIni -Section 'Inputs' -Key 'EnableFsr3Inputs' -Value 'false'
        Set-IniValue -Path $optiIni -Section 'FSR' -Key 'Fsr4Update' -Value 'true'
        Set-IniValue -Path $optiIni -Section 'Plugins' -Key 'Path' -Value 'auto'
        Set-IniValue -Path $optiIni -Section 'Plugins' -Key 'LoadAsiPlugins' -Value 'false'
        Set-IniValue -Path $optiIni -Section 'Plugins' -Key 'LoadReshade' -Value 'false'
    }
    Set-IniValue -Path $optiIni -Section 'Libraries' -Key 'OptiDllPath' -Value $optiDir
    Set-IniValue -Path $optiIni -Section 'Log' -Key 'LogToFile' -Value 'true'
    Set-IniValue -Path $optiIni -Section 'Log' -Key 'LogLevel' -Value '2'
    Set-IniValue -Path $optiIni -Section 'Log' -Key 'SingleFile' -Value 'true'
    Set-IniValue -Path $optiIni -Section 'Log' -Key 'LogFileName' -Value 'OptiScaler.log'
    Set-IniValue -Path $optiIni -Section 'Log' -Key 'LogAsync' -Value 'true'
    Set-IniValue -Path $optiIni -Section 'Log' -Key 'LogAsyncThreads' -Value '1'
    Set-IniValue -Path $bridgeIni -Section 'Dx11FsrBridge' -Key 'EnableLogging' -Value '1'
}

$dllList = [System.Collections.Generic.List[string]]::new()
if (-not $DisableOptiScaler) {
    $dllList.Add($bridgeDll)
    $dllList.Add($optiDll)
}
if (-not $DisableAntiBlur) {
    $dllList.Add($antiBlurDll)
}
if (-not $DisableHDR) {
    $dllList.Add($reshadeDll)
}

$config = $null
if (Test-Path -LiteralPath $fpsConfig -PathType Leaf) {
    try { $config = Get-Content -LiteralPath $fpsConfig -Raw -Encoding UTF8 | ConvertFrom-Json } catch { $config = $null }
}
if ($null -eq $config) {
    $config = [pscustomobject][ordered]@{
        GamePath = $gameExe
        AutoStart = $true
        AutoClose = $true
        PopupWindow = $true
        Fullscreen = $false
        UseCustomRes = $false
        IsExclusiveFullscreen = $false
        StartMinimized = $true
        UsePowerSave = $false
        SuspendLoad = $false
        UseMobileUI = $false
        UseHDR = (-not [bool]$DisableHDR)
        FPSTarget = $FpsTarget
        CustomResX = 1920
        CustomResY = 1080
        MonitorNum = 1
        Priority = 3
        AdditionalCommandLine = ''
        LastVersionNotify = 0
        DllList = @($dllList)
    }
}
Set-JsonProperty -Object $config -Name 'GamePath' -Value $gameExe
Set-JsonProperty -Object $config -Name 'FPSTarget' -Value $FpsTarget
Set-JsonProperty -Object $config -Name 'DllList' -Value @($dllList)
Set-JsonProperty -Object $config -Name 'UseHDR' -Value (-not [bool]$DisableHDR)
$config | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $fpsConfig -Encoding UTF8

if (-not $DisableHDR) {
    New-Item -ItemType Directory -Force -Path $screenshots | Out-Null
    $reshadeConfig = New-ReShadeConfig `
        -ShaderPath (Join-Path $shaderDir 'Shaders') `
        -TexturePath (Join-Path $shaderDir 'Textures') `
        -PresetPath $reshadePreset `
        -ScreenshotPath $screenshots
    if (-not (Test-Path -LiteralPath $reshadeIni -PathType Leaf)) {
        Set-Content -LiteralPath $reshadeIni -Value $reshadeConfig -Encoding UTF8
    }
    if (-not (Test-Path -LiteralPath $reshadePreset)) {
        Set-Content -LiteralPath $reshadePreset -Value "[GENERAL]`nPreprocessorDefinitions=`nTechniqueSorting=" -Encoding UTF8
    }
}

if (-not $NoShortcut) {
    if (Test-Path -LiteralPath $legacyShortcutPath -PathType Leaf) {
        Remove-Item -LiteralPath $legacyShortcutPath -Force
    }
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($shortcutPath)
    $shortcut.TargetPath = $unlocker
    $shortcut.WorkingDirectory = $root
    $shortcut.IconLocation = "$gameExe,0"
    $shortcut.Save()
}

Write-Host '一键配置完成。' -ForegroundColor Green
Write-Host "游戏: $gameExe"
Write-Host "帧率上限: $FpsTarget"
Write-Host 'DLL 加载顺序:'
for ($index = 0; $index -lt $dllList.Count; $index++) {
    Write-Host ("  {0}. {1}" -f ($index + 1), $dllList[$index])
}
