[CmdletBinding()]
param(
    [string]$FufuPath,
    [ValidateSet('Prompt', 'Auto', 'Manual', 'Bundled')]
    [string]$OptiScalerSource = 'Prompt',
    [string]$OptiScalerPackagePath,
    [switch]$NonInteractive
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
[Console]::InputEncoding = [Text.Encoding]::UTF8
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$OutputEncoding = [Text.Encoding]::UTF8

$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSCommandPath))
$bundle = $root
$errorLogPath = Join-Path $root '.last-fufu-install-error.log'
$stateDirectory = if (-not [string]::IsNullOrWhiteSpace($env:FSR_BRIDGE_STATE_DIRECTORY)) {
    [IO.Path]::GetFullPath($env:FSR_BRIDGE_STATE_DIRECTORY)
} else {
    Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'GenshinFsrBridge'
}
$statePath = Join-Path $stateDirectory 'FufuInstallerState.json'
$packageVersionPath = Join-Path $root 'Package-Version.txt'
$selfUpdateRepository = 'AizawaHikaru233/genshin_fsr_brigde'
$selfUpdateHelperPath = Join-Path $root 'Apply-PackageUpdate.ps1'
$packagedOptiDirectory = Join-Path $root 'payload\OptiScaler'
$packagedNvidiaDirectory = Join-Path $root 'payload\NVIDIA\DLSS'
$optiUpscalingManifest = Join-Path $packagedOptiDirectory 'default_config\OptiScaler-UpscalingFiles.json'
$script:SelfUpdateStarted = $false
$pluginDirectoryName = 'FSR-Bridge-Plugin'
$legacyPluginDirectoryNames = @('FSRGraphics', 'FSRBootstrap')
$optiRepository = 'optiscaler/OptiScaler'
$optiTag = 'v0.9.3'
$optiAssetName = 'Optiscaler_0.9.3-final.20260618.7z'
$optiArchiveSha256 = 'E3AC655D60EC11B471AC8CC5F4D3758E4BCE9151C86CAA339D8F0700C00282E3'
$optiBinaryHashes = [ordered]@{
    'OptiScaler.dll' = '2369120927264BB2B120E7FB0940CB0B3242DC788417AB92FB99953555016511'
    'amd_fidelityfx_dx12.dll' = 'D98C027FBD4CC074A1C48574EECD58DED2BC9E4CB6BFC5C28D6595AB5037D371'
    'amd_fidelityfx_upscaler_dx12.dll' = 'EC7ED3CA674E288240E6F04B986342AECE47454C41D9B0959449E82E22BD7F6D'
    'libxell.dll' = '61AEEAB7FDCFB6D1742F86C14C68C663D759607D01A6132645C857291EAEBB86'
    'libxess.dll' = '251659DD84A3E84DE67C886A4186E01F3ECA49B00641906FE38BB6B807E5D5B7'
    'libxess_dx11.dll' = 'C7CFE86F0C9D94E4FB3696D3CD5035E2BBB6A8B1B0572F8B7395A4CDFD0C625E'
    'D3D12_Optiscaler\D3D12Core.dll' = '07D286C306F8117321422AFFD9E6388C12D0FB4BE1C7FC689D9E899324FEEB24'
}

function Write-Header {
    param([string]$Title)
    Clear-Host
    Write-Host '============================================================' -ForegroundColor DarkGray
    Write-Host "  $Title" -ForegroundColor Cyan
    Write-Host '============================================================' -ForegroundColor DarkGray
}

function Pause-Installer {
    if (-not $NonInteractive) {
        Write-Host ''
        Read-Host '按回车键继续' | Out-Null
    }
}

function Resolve-FufuRoot {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }
    $cleanPath = $Path.Trim().Trim('"')
    if (-not (Test-Path -LiteralPath $cleanPath)) { return $null }
    $resolved = (Resolve-Path -LiteralPath $cleanPath).Path
    if (Test-Path -LiteralPath $resolved -PathType Leaf) {
        if ([IO.Path]::GetFileName($resolved) -ine 'FufuLauncher.exe') { return $null }
        $resolved = Split-Path -Parent $resolved
    }
    if (-not (Test-Path -LiteralPath (Join-Path $resolved 'FufuLauncher.exe') -PathType Leaf)) { return $null }
    return [IO.Path]::GetFullPath($resolved)
}

function Select-FufuFolderDialog {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = '请选择 FufuLauncher.exe 所在目录'
    $dialog.ShowNewFolderButton = $false
    if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $dialog.SelectedPath }
    return $null
}

function Test-DirectoryWriteAccess {
    param([string]$Path)
    $testFile = Join-Path $Path ('.fufu-write-test-' + [guid]::NewGuid().ToString('N'))
    try {
        [IO.File]::WriteAllText($testFile, 'test', [Text.UTF8Encoding]::new($false))
        Remove-Item -LiteralPath $testFile -Force
        return $true
    }
    catch {
        Remove-Item -LiteralPath $testFile -Force -ErrorAction SilentlyContinue
        return $false
    }
}

function Ensure-FufuWriteAccess {
    param([string]$FufuRoot)
    $pluginsDirectory = Join-Path $FufuRoot 'Plugins'
    $testDirectory = if (Test-Path -LiteralPath $pluginsDirectory -PathType Container) { $pluginsDirectory } else { $FufuRoot }
    if (Test-DirectoryWriteAccess -Path $testDirectory) { return $true }
    if ($NonInteractive) { throw "没有写入芙芙目录的权限: $FufuRoot" }

    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "即使以管理员身份运行仍无法写入芙芙目录: $FufuRoot"
    }
    Write-Host '芙芙安装目录需要管理员权限，正在请求 UAC 授权...' -ForegroundColor Yellow
    $arguments = [Collections.Generic.List[string]]::new()
    foreach ($argument in @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $PSCommandPath + '"'),
        '-FufuPath', ('"' + $FufuRoot + '"'), '-OptiScalerSource', $OptiScalerSource
    )) { $arguments.Add($argument) }
    if (-not [string]::IsNullOrWhiteSpace($OptiScalerPackagePath)) {
        $arguments.Add('-OptiScalerPackagePath')
        $arguments.Add('"' + $OptiScalerPackagePath + '"')
    }
    $elevated = Start-Process -FilePath 'powershell.exe' -Verb RunAs -Wait -PassThru -ArgumentList @($arguments)
    exit $elevated.ExitCode
}

function Read-InstallerState {
    if (-not (Test-Path -LiteralPath $statePath -PathType Leaf)) { return $null }
    try { return Get-Content -LiteralPath $statePath -Raw -Encoding UTF8 | ConvertFrom-Json }
    catch { return $null }
}

function Save-InstallerState {
    param([string]$FufuRoot)
    New-Item -ItemType Directory -Force -Path $stateDirectory | Out-Null
    [pscustomobject]@{ FufuPath = $FufuRoot } |
        ConvertTo-Json | Set-Content -LiteralPath $statePath -Encoding UTF8
}

function Select-FufuRoot {
    param([string]$RequestedPath, [switch]$ForceSelection)
    $resolved = Resolve-FufuRoot -Path $RequestedPath
    if (-not [string]::IsNullOrWhiteSpace($RequestedPath) -and $null -eq $resolved) {
        throw "指定的芙芙启动器目录无效: $RequestedPath"
    }
    if ($null -ne $resolved) {
        Save-InstallerState -FufuRoot $resolved
        return $resolved
    }
    if (-not $ForceSelection) {
        $state = Read-InstallerState
        $remembered = if ($null -ne $state) { Resolve-FufuRoot -Path ([string]$state.FufuPath) } else { $null }
        if ($null -ne $remembered) { return $remembered }
    }
    if ($NonInteractive) { throw '无人值守安装必须通过 -FufuPath 手动指定 FufuLauncher 目录。' }
    while ($true) {
        Write-Header -Title '原神FSR2桥接插件 - 芙芙安装器'
        Write-Host '请手动输入 FufuLauncher.exe 所在目录。' -ForegroundColor Cyan
        Write-Host '直接按回车可打开目录选择窗口。' -ForegroundColor DarkGray
        Write-Host '输入 0 退出。' -ForegroundColor DarkGray
        $inputValue = (Read-Host '芙芙启动器路径').Trim()
        if ($inputValue -eq '0') { return $null }
        if ([string]::IsNullOrWhiteSpace($inputValue)) {
            $selected = Select-FufuFolderDialog
            if ($null -eq $selected) { continue }
            $candidateRoot = Resolve-FufuRoot -Path $selected
        }
        else {
            $candidateRoot = Resolve-FufuRoot -Path $inputValue
        }
        if ($null -ne $candidateRoot) {
            Save-InstallerState -FufuRoot $candidateRoot
            return $candidateRoot
        }
        Write-Host '所选目录中没有找到 FufuLauncher.exe。' -ForegroundColor Red
        Pause-Installer
    }
}

function Assert-FileHash {
    param([string]$Path, [string]$ExpectedSha256, [string]$Label)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "缺少文件: $Label ($Path)" }
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    if ($actual -ne $ExpectedSha256) { throw "$Label SHA-256 不匹配。实际: $actual" }
}

function Invoke-OfficialDownload {
    param([string]$Url, [string]$Destination)
    try {
        Invoke-WebRequest -UseBasicParsing -Headers @{ 'User-Agent' = 'GenshinFsrBridge-FufuInstaller' } -Uri $Url -OutFile $Destination
    }
    catch {
        throw "下载失败，请检查网络或使用 -OptiScalerSource Manual。$([Environment]::NewLine)$($_.Exception.Message)"
    }
}

function Select-OptiScalerSource {
    if ($OptiScalerSource -ne 'Prompt') { return $OptiScalerSource }
    if (Test-Path -LiteralPath (Join-Path $packagedOptiDirectory 'OptiScaler.dll') -PathType Leaf) { return 'Bundled' }
    if ($NonInteractive) { return 'Auto' }
    while ($true) {
        Write-Host ''
        Write-Host '选择 OptiScaler 0.9.3 安装来源：' -ForegroundColor Cyan
        Write-Host '  1. 从官方 GitHub 自动下载（推荐）'
        Write-Host '  2. 使用已下载的 7z、ZIP 或解压目录'
        $choice = (Read-Host '请输入选项').Trim()
        if ([string]::IsNullOrWhiteSpace($choice) -or $choice -eq '1') { return 'Auto' }
        if ($choice -eq '2') { return 'Manual' }
        Write-Host '无效选项，请重新输入。' -ForegroundColor Yellow
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
        if ($null -eq $tar) { throw '系统缺少 tar.exe，无法解压 OptiScaler 7z。' }
        & $tar.Source -xf $PackagePath -C $Destination
        if ($LASTEXITCODE -ne 0) { throw "OptiScaler 解压失败，退出代码: $LASTEXITCODE" }
        return
    }
    throw "不支持的 OptiScaler 压缩包格式: $extension"
}

function Get-NvidiaVideoControllers {
    try {
        $controllers = @(Get-CimInstance -ClassName Win32_VideoController -ErrorAction Stop)
    }
    catch {
        try { $controllers = @(Get-WmiObject -Class Win32_VideoController -ErrorAction Stop) }
        catch { return @() }
    }
    return @($controllers | Where-Object {
        ([string]$_.PNPDeviceID -match '(?i)VEN_10DE') -or
        ([string]$_.AdapterCompatibility -match '(?i)NVIDIA') -or
        ([string]$_.Name -match '(?i)NVIDIA')
    })
}

function Assert-NvidiaSignedFile {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "NVIDIA DLL 不存在: $Path" }
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne [Management.Automation.SignatureStatus]::Valid -or
        $null -eq $signature.SignerCertificate -or
        $signature.SignerCertificate.Subject -notmatch '(?i)NVIDIA Corporation') {
        throw "NVIDIA DLL 数字签名验证失败: $Path"
    }
}

function Install-NvidiaDlssIfNeeded {
    param(
        [string]$Destination,
        [string]$TemporaryDirectory,
        [string]$ExistingOptiDirectory,
        [string]$BundledNvidiaDirectory
    )
    $destinationDll = Join-Path $Destination 'nvngx_dlss.dll'
    $destinationLicense = Join-Path $Destination 'nvngx_dlss.license.txt'
    if (-not [string]::IsNullOrWhiteSpace($ExistingOptiDirectory)) {
        $existingDll = Join-Path $ExistingOptiDirectory 'nvngx_dlss.dll'
        if (Test-Path -LiteralPath $existingDll -PathType Leaf) {
            Assert-NvidiaSignedFile -Path $existingDll
            Copy-Item -LiteralPath $existingDll -Destination $destinationDll -Force
            $existingLicense = Join-Path $ExistingOptiDirectory 'nvngx_dlss.license.txt'
            if (Test-Path -LiteralPath $existingLicense -PathType Leaf) {
                Copy-Item -LiteralPath $existingLicense -Destination $destinationLicense -Force
            }
            Write-Host '已保留现有 NVIDIA DLSS 超分组件。' -ForegroundColor Green
            return
        }
    }
    $nvidiaControllers = @(Get-NvidiaVideoControllers)
    if ($nvidiaControllers.Count -eq 0) { return }

    if (-not [string]::IsNullOrWhiteSpace($BundledNvidiaDirectory)) {
        $bundledDll = Join-Path $BundledNvidiaDirectory 'nvngx_dlss.dll'
        $bundledLicense = Join-Path $BundledNvidiaDirectory 'nvngx_dlss.license.txt'
        if (Test-Path -LiteralPath $bundledDll -PathType Leaf) {
            Assert-NvidiaSignedFile -Path $bundledDll
            if (-not (Test-Path -LiteralPath $bundledLicense -PathType Leaf)) {
                throw "内置 NVIDIA DLSS 组件缺少许可证: $bundledLicense"
            }
            Copy-Item -LiteralPath $bundledDll -Destination $destinationDll -Force
            Copy-Item -LiteralPath $bundledLicense -Destination $destinationLicense -Force
            Write-Host '已安装内置 NVIDIA DLSS 超分组件。' -ForegroundColor Green
            return
        }
    }

    $gpuNames = @($nvidiaControllers | ForEach-Object { [string]$_.Name } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -Unique)
    $gpuLabel = if ($gpuNames.Count -gt 0) { $gpuNames -join ' / ' } else { 'NVIDIA GPU' }
    Write-Host "检测到 $gpuLabel，正在补齐 NVIDIA DLSS 超分组件..." -ForegroundColor Cyan
    try {
        $release = Invoke-RestMethod -Headers @{ 'User-Agent' = 'GenshinFsrBridge-FufuInstaller' } `
            -Uri 'https://api.github.com/repos/NVIDIA-RTX/Streamline/releases/latest'
    }
    catch {
        throw "无法查询 NVIDIA Streamline 官方发行版。$([Environment]::NewLine)$($_.Exception.Message)"
    }
    $asset = @($release.assets | Where-Object { $_.name -match '^streamline-sdk-v[0-9.]+\.zip$' } | Select-Object -First 1)
    if ($asset.Count -ne 1) { throw 'NVIDIA Streamline 最新发行版没有找到正式 SDK ZIP。' }
    $downloadUrl = [string]$asset[0].browser_download_url
    if ($downloadUrl -notmatch '^https://github\.com/NVIDIA-RTX/Streamline/releases/download/') {
        throw "NVIDIA Streamline 下载地址不是预期的官方地址: $downloadUrl"
    }
    $packagePath = Join-Path $TemporaryDirectory ([string]$asset[0].name)
    Invoke-OfficialDownload -Url $downloadUrl -Destination $packagePath
    $expanded = Join-Path $TemporaryDirectory 'streamline-expanded'
    Expand-ComponentPackage -PackagePath $packagePath -Destination $expanded
    $sourceDll = Get-ChildItem -LiteralPath $expanded -Recurse -File -Filter 'nvngx_dlss.dll' |
        Where-Object { $_.FullName -notmatch '(?i)[\\/]development[\\/]' } |
        Sort-Object @{ Expression = { if ($_.FullName -match '(?i)[\\/]bin[\\/]x64[\\/]nvngx_dlss\.dll$') { 0 } else { 1 } } }, FullName |
        Select-Object -First 1
    if ($null -eq $sourceDll) { throw 'NVIDIA Streamline 官方包中没有找到生产版 nvngx_dlss.dll。' }
    Assert-NvidiaSignedFile -Path $sourceDll.FullName
    Copy-Item -LiteralPath $sourceDll.FullName -Destination $destinationDll -Force
    $sourceLicense = Get-ChildItem -LiteralPath $expanded -Recurse -File |
        Where-Object { $_.Name -ieq 'nvngx_dlss.license.txt' -or $_.FullName -match '(?i)[\\/]external[\\/]ngx-sdk[\\/]license\.txt$' } |
        Select-Object -First 1
    if ($null -eq $sourceLicense) { throw 'NVIDIA Streamline 官方包缺少 NGX/DLSS 许可证。' }
    Copy-Item -LiteralPath $sourceLicense.FullName -Destination $destinationLicense -Force
    Write-Host '已从 NVIDIA 官方来源安装 DLSS 超分组件。' -ForegroundColor Green
}

function Get-OptiScalerSourceDirectory {
    param([string]$TemporaryDirectory, [string]$SourceMode)
    if ($SourceMode -eq 'Bundled') {
        $expanded = $packagedOptiDirectory
    }
    elseif ($SourceMode -eq 'Manual') {
        $manualPath = $OptiScalerPackagePath
        if ([string]::IsNullOrWhiteSpace($manualPath) -and -not $NonInteractive) {
            $manualPath = Read-Host '请输入 OptiScaler 0.9.3 解压目录、ZIP 或 7z 路径'
        }
        if ([string]::IsNullOrWhiteSpace($manualPath) -or -not (Test-Path -LiteralPath $manualPath)) {
            throw '没有提供有效的 OptiScaler 0.9.3 本地路径。'
        }
        $manualPath = (Resolve-Path -LiteralPath $manualPath.Trim().Trim('"')).Path
        if (Test-Path -LiteralPath $manualPath -PathType Container) {
            $expanded = $manualPath
        }
        else {
            $expanded = Join-Path $TemporaryDirectory 'optiscaler-expanded'
            Expand-ComponentPackage -PackagePath $manualPath -Destination $expanded
        }
    }
    else {
        Write-Host '正在查询 OptiScaler 官方 v0.9.3 发行版...' -ForegroundColor Cyan
        try {
            $release = Invoke-RestMethod -Headers @{ 'User-Agent' = 'GenshinFsrBridge-FufuInstaller' } `
                -Uri "https://api.github.com/repos/$optiRepository/releases/tags/$optiTag"
        }
        catch {
            throw "无法查询 OptiScaler 官方发行版。$([Environment]::NewLine)$($_.Exception.Message)"
        }
        $asset = @($release.assets | Where-Object { $_.name -ceq $optiAssetName } | Select-Object -First 1)
        if ($asset.Count -ne 1) { throw "OptiScaler $optiTag 未找到固定资产 $optiAssetName。" }
        $downloadUrl = [string]$asset[0].browser_download_url
        if ($downloadUrl -notmatch '^https://github\.com/optiscaler/OptiScaler/releases/download/v0\.9\.3/') {
            throw "OptiScaler 下载地址不是预期的官方地址: $downloadUrl"
        }
        $packagePath = Join-Path $TemporaryDirectory $optiAssetName
        Write-Host '正在从 OptiScaler 官方 GitHub 下载 0.9.3...' -ForegroundColor Cyan
        Invoke-OfficialDownload -Url $downloadUrl -Destination $packagePath
        Assert-FileHash -Path $packagePath -ExpectedSha256 $optiArchiveSha256 -Label 'OptiScaler 0.9.3 官方压缩包'
        $expanded = Join-Path $TemporaryDirectory 'optiscaler-expanded'
        Expand-ComponentPackage -PackagePath $packagePath -Destination $expanded
    }

    $mainDll = Get-ChildItem -LiteralPath $expanded -Recurse -File -Filter 'OptiScaler.dll' | Select-Object -First 1
    if ($null -eq $mainDll) { throw 'OptiScaler 包中没有找到 OptiScaler.dll。' }
    if ($mainDll.VersionInfo.FileVersion -ne '0.9.3.0') {
        throw "OptiScaler 版本不是 0.9.3.0: $($mainDll.VersionInfo.FileVersion)"
    }
    $sourceDirectory = $mainDll.Directory.FullName
    foreach ($entry in $optiBinaryHashes.GetEnumerator()) {
        Assert-FileHash -Path (Join-Path $sourceDirectory $entry.Key) -ExpectedSha256 $entry.Value -Label $entry.Key
    }
    return $sourceDirectory
}

function Set-IniValue {
    param([string]$Path, [string]$Section, [string]$Key, [string]$Value)
    $lines = [Collections.Generic.List[string]]::new()
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        foreach ($line in [IO.File]::ReadAllLines($Path, [Text.Encoding]::UTF8)) { $lines.Add($line) }
    }
    $sectionStart = -1
    $sectionEnd = $lines.Count
    for ($index = 0; $index -lt $lines.Count; $index++) {
        if ($lines[$index].Trim() -ieq "[$Section]") {
            $sectionStart = $index
            for ($next = $index + 1; $next -lt $lines.Count; $next++) {
                if ($lines[$next].Trim() -match '^\[.+\]$') { $sectionEnd = $next; break }
            }
            break
        }
    }
    if ($sectionStart -lt 0) {
        if ($lines.Count -gt 0 -and -not [string]::IsNullOrWhiteSpace($lines[$lines.Count - 1])) { $lines.Add('') }
        $lines.Add("[$Section]")
        $lines.Add("$Key = $Value")
    }
    else {
        $keyIndex = -1
        for ($index = $sectionStart + 1; $index -lt $sectionEnd; $index++) {
            if ($lines[$index] -match ('^\s*' + [regex]::Escape($Key) + '\s*=')) { $keyIndex = $index; break }
        }
        if ($keyIndex -ge 0) { $lines[$keyIndex] = "$Key = $Value" }
        else { $lines.Insert($sectionEnd, "$Key = $Value") }
    }
    [IO.File]::WriteAllLines($Path, $lines, [Text.UTF8Encoding]::new($false))
}

function Set-OptiScalerConfiguration {
    param([string]$Path, [string]$FinalOptiDirectory)
    Set-IniValue -Path $Path -Section 'Inputs' -Key 'EnableFsr2Inputs' -Value 'true'
    Set-IniValue -Path $Path -Section 'Inputs' -Key 'UseFsr2Dx11Inputs' -Value 'true'
    Set-IniValue -Path $Path -Section 'Inputs' -Key 'UseFsr2Inputs' -Value 'true'
    Set-IniValue -Path $Path -Section 'Inputs' -Key 'EnableFsr3Inputs' -Value 'false'
    Set-IniValue -Path $Path -Section 'FSR' -Key 'Fsr4Update' -Value 'true'
    Set-IniValue -Path $Path -Section 'FrameGen' -Key 'Enabled' -Value 'false'
    Set-IniValue -Path $Path -Section 'FrameGen' -Key 'FGInput' -Value 'nofg'
    Set-IniValue -Path $Path -Section 'FrameGen' -Key 'FGOutput' -Value 'nofg'
    Set-IniValue -Path $Path -Section 'Libraries' -Key 'OptiDllPath' -Value $FinalOptiDirectory
    Set-IniValue -Path $Path -Section 'Menu' -Key 'LoadReshade' -Value 'false'
    Set-IniValue -Path $Path -Section 'Plugins' -Key 'LoadAsiPlugins' -Value 'false'
    Set-IniValue -Path $Path -Section 'Plugins' -Key 'LoadReshade' -Value 'false'
    Set-IniValue -Path $Path -Section 'Log' -Key 'LogToFile' -Value 'true'
    Set-IniValue -Path $Path -Section 'Log' -Key 'LogLevel' -Value '4'
    Set-IniValue -Path $Path -Section 'Log' -Key 'SingleFile' -Value 'true'
    Set-IniValue -Path $Path -Section 'Log' -Key 'LogFileName' -Value 'OptiScaler.log'
    Set-IniValue -Path $Path -Section 'Log' -Key 'LogAsync' -Value 'false'
    Set-IniValue -Path $Path -Section 'Log' -Key 'LogAsyncThreads' -Value '1'
}

function Get-OptiScalerUpscalingManifest {
    if (-not (Test-Path -LiteralPath $optiUpscalingManifest -PathType Leaf)) {
        throw "缺少 OptiScaler 超分文件清单: $optiUpscalingManifest"
    }
    $manifest = Get-Content -LiteralPath $optiUpscalingManifest -Raw -Encoding UTF8 | ConvertFrom-Json
    if ([int]$manifest.SchemaVersion -ne 1 -or @($manifest.RuntimeFiles).Count -eq 0) {
        throw "OptiScaler 超分文件清单无效: $optiUpscalingManifest"
    }
    $manifestFiles = @($manifest.RuntimeFiles | ForEach-Object { [string]$_ } | Sort-Object)
    $hashedFiles = @($optiBinaryHashes.Keys | ForEach-Object { [string]$_ } | Sort-Object)
    if (@(Compare-Object -ReferenceObject $hashedFiles -DifferenceObject $manifestFiles).Count -ne 0) {
        throw 'OptiScaler 超分文件清单与固定哈希清单不一致。'
    }
    return $manifest
}

function Copy-CuratedOptiScaler {
    param([string]$SourceDirectory, [string]$Destination, [string]$FinalOptiDirectory)
    $manifest = Get-OptiScalerUpscalingManifest
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    foreach ($relativePath in @($manifest.RuntimeFiles)) {
        $sourcePath = Join-Path $SourceDirectory $relativePath
        $destinationPath = Join-Path $Destination $relativePath
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destinationPath) | Out-Null
        Copy-Item -LiteralPath $sourcePath -Destination $destinationPath -Force
    }
    foreach ($licenseName in @($manifest.LicenseFiles)) {
        $sourcePath = Join-Path $SourceDirectory "Licenses\$licenseName"
        if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) { throw "OptiScaler 官方包缺少许可证: $licenseName" }
        $licenseDirectory = Join-Path $Destination 'Licenses'
        New-Item -ItemType Directory -Force -Path $licenseDirectory | Out-Null
        Copy-Item -LiteralPath $sourcePath -Destination (Join-Path $licenseDirectory $licenseName) -Force
    }
    foreach ($licenseName in @($manifest.OptionalLicenseFiles)) {
        $sourcePath = Join-Path $SourceDirectory "Licenses\$licenseName"
        if (Test-Path -LiteralPath $sourcePath -PathType Leaf) {
            $licenseDirectory = Join-Path $Destination 'Licenses'
            New-Item -ItemType Directory -Force -Path $licenseDirectory | Out-Null
            Copy-Item -LiteralPath $sourcePath -Destination (Join-Path $licenseDirectory $licenseName) -Force
        }
    }
    $packagedDefaultConfig = Join-Path $packagedOptiDirectory 'default_config'
    if (-not (Test-Path -LiteralPath $packagedDefaultConfig -PathType Container)) {
        throw "缺少 OptiScaler 默认配置目录: $packagedDefaultConfig"
    }
    Copy-Item -LiteralPath $packagedDefaultConfig -Destination (Join-Path $Destination 'default_config') -Recurse -Force
    $sourceIni = Join-Path $SourceDirectory 'OptiScaler.default.ini'
    if (-not (Test-Path -LiteralPath $sourceIni -PathType Leaf)) {
        $sourceIni = Join-Path $SourceDirectory 'OptiScaler.ini'
    }
    if (-not (Test-Path -LiteralPath $sourceIni -PathType Leaf)) {
        $sourceIni = Join-Path $packagedOptiDirectory 'default_config\OptiScaler.ini'
    }
    if (-not (Test-Path -LiteralPath $sourceIni -PathType Leaf)) { throw 'OptiScaler 包和脚本资源均缺少 OptiScaler.ini 模板。' }
    $defaultIni = Join-Path $Destination 'OptiScaler.default.ini'
    Copy-Item -LiteralPath $sourceIni -Destination $defaultIni -Force
    Set-OptiScalerConfiguration -Path $defaultIni -FinalOptiDirectory $FinalOptiDirectory
    Copy-Item -LiteralPath $defaultIni -Destination (Join-Path $Destination 'OptiScaler.ini') -Force

}

function Assert-Bundle {
    foreach ($relativePath in @(
        'FSR-Bridge-Plugin.dll',
        'config.ini',
        'payload\Bridge\Dx11FsrBridge.dll',
        'payload\ReShade\ReShade64.dll',
        'payload\OptiScaler\default_config\OptiScaler.ini',
        'payload\OptiScaler\default_config\OptiScaler-UpscalingFiles.json',
        'payload\ReShade\default_config\ReShade.ini',
        'payload\ReShade\default_config\ReShadePreset.ini',
        'payload\ReShade\LICENSE-ReShade-BSD-3-Clause.txt',
        'payload\ReShade\reshade-shaders\Addons\renodx-genshin.addon64'
    )) {
        if (-not (Test-Path -LiteralPath (Join-Path $bundle $relativePath) -PathType Leaf)) {
            throw "Fufu 安装包不完整，缺少: $relativePath"
        }
    }
}

function Assert-NoRunningProcesses {
    param([string]$FufuRoot)
    $selectedLauncher = [IO.Path]::GetFullPath((Join-Path $FufuRoot 'FufuLauncher.exe'))
    $running = [Collections.Generic.List[string]]::new()
    foreach ($process in @(Get-Process -ErrorAction SilentlyContinue)) {
        if ($process.ProcessName -in @('YuanShen', 'GenshinImpact')) {
            if (-not ($running -contains $process.ProcessName)) { $running.Add($process.ProcessName) }
            continue
        }
        if ($process.ProcessName -eq 'FufuLauncher') {
            $processPath = try { [string]$process.Path } catch { '' }
            if ([string]::IsNullOrWhiteSpace($processPath) -or
                [string]::Equals([IO.Path]::GetFullPath($processPath), $selectedLauncher, [StringComparison]::OrdinalIgnoreCase)) {
                if (-not ($running -contains $process.ProcessName)) { $running.Add($process.ProcessName) }
            }
        }
    }
    if ($running.Count -gt 0) { throw "安装前请退出以下程序: $($running -join ', ')" }
}

function Assert-ChildPath {
    param([string]$Parent, [string]$Child)
    $parentFull = [IO.Path]::GetFullPath($Parent).TrimEnd('\') + '\'
    $childFull = [IO.Path]::GetFullPath($Child)
    if (-not $childFull.StartsWith($parentFull, [StringComparison]::OrdinalIgnoreCase)) {
        throw "目标路径不在预期目录内: $childFull"
    }
}

function Install-FufuPlugin {
    param(
        [string]$FufuRoot,
        [string]$OptiSourceMode = 'Auto',
        [ValidateSet('Install', 'Update')][string]$Operation = 'Install',
        [switch]$Core,
        [switch]$ReShade
    )
    if (-not $Core -and -not $ReShade) { return $null }
    Assert-Bundle
    Assert-NoRunningProcesses -FufuRoot $FufuRoot

    $pluginsDirectory = Join-Path $FufuRoot 'Plugins'
    New-Item -ItemType Directory -Force -Path $pluginsDirectory | Out-Null
    $targetDirectory = Join-Path $pluginsDirectory $pluginDirectoryName
    $legacyDirectories = @($legacyPluginDirectoryNames | ForEach-Object { Join-Path $pluginsDirectory $_ } | Where-Object {
        (Test-Path -LiteralPath (Join-Path $_ 'config.ini') -PathType Leaf) -and
        ((Test-Path -LiteralPath (Join-Path $_ 'FufuGraphicsPlugin.dll') -PathType Leaf) -or
         (Test-Path -LiteralPath (Join-Path $_ 'FSR-Bridge-Plugin.dll') -PathType Leaf))
    })
    if ($legacyDirectories.Count -gt 1 -or ((Test-Path -LiteralPath $targetDirectory) -and $legacyDirectories.Count -gt 0)) {
        throw '检测到多个旧版或正式版桥接插件目录，请只保留一份后再安装。'
    }
    $existingDirectory = if (Test-Path -LiteralPath $targetDirectory -PathType Container) {
        $targetDirectory
    } elseif ($legacyDirectories.Count -eq 1) {
        [string]$legacyDirectories[0]
    } else { '' }
    if (-not [string]::IsNullOrWhiteSpace($existingDirectory) -and
        -not [string]::Equals([IO.Path]::GetFullPath($existingDirectory), [IO.Path]::GetFullPath($targetDirectory), [StringComparison]::OrdinalIgnoreCase)) {
        Write-Host "检测到旧版目录，将迁移为 $pluginDirectoryName：$existingDirectory" -ForegroundColor Yellow
    }
    Assert-ChildPath -Parent $pluginsDirectory -Child $targetDirectory

    $temporaryDirectory = Join-Path ([IO.Path]::GetTempPath()) ("FSR-Bridge-Plugin-" + [guid]::NewGuid().ToString('N'))
    $stageDirectory = Join-Path $pluginsDirectory ('.fsi-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    $backupDirectory = Join-Path $pluginsDirectory ('.fsb-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    Assert-ChildPath -Parent $pluginsDirectory -Child $stageDirectory
    Assert-ChildPath -Parent $pluginsDirectory -Child $backupDirectory
    New-Item -ItemType Directory -Force -Path $temporaryDirectory, $stageDirectory | Out-Null
    $installed = $false
    try {
        if (-not [string]::IsNullOrWhiteSpace($existingDirectory)) {
            Get-ChildItem -LiteralPath $existingDirectory -Force |
                Copy-Item -Destination $stageDirectory -Recurse -Force
        }
        foreach ($staleAntiBlurFile in @('AntiPlayerMosaic.dll', 'AntiPlayerMosaic.log')) {
            Remove-Item -LiteralPath (Join-Path $stageDirectory $staleAntiBlurFile) -Force -ErrorAction SilentlyContinue
        }
        $stagePayloadDirectory = Join-Path $stageDirectory 'payload'
        $stageDefaultConfigDirectory = Join-Path $stagePayloadDirectory 'default_config'
        if (Test-Path -LiteralPath $stageDefaultConfigDirectory) {
            Remove-Item -LiteralPath $stageDefaultConfigDirectory -Recurse -Force
        }
        New-Item -ItemType Directory -Force -Path $stagePayloadDirectory | Out-Null

        $stagePluginConfig = Join-Path $stageDirectory 'config.ini'
        if ($Core) {
            Copy-Item -LiteralPath (Join-Path $bundle 'FSR-Bridge-Plugin.dll') -Destination $stageDirectory -Force
            Remove-Item -LiteralPath (Join-Path $stageDirectory 'FufuGraphicsPlugin.dll') -Force -ErrorAction SilentlyContinue
            if (-not (Test-Path -LiteralPath $stagePluginConfig -PathType Leaf)) {
                Copy-Item -LiteralPath (Join-Path $bundle 'config.ini') -Destination $stagePluginConfig -Force
            }
            $stageBridgeDirectory = Join-Path $stagePayloadDirectory 'Bridge'
            New-Item -ItemType Directory -Force -Path $stageBridgeDirectory | Out-Null
            Copy-Item -LiteralPath (Join-Path $bundle 'payload\Bridge\Dx11FsrBridge.dll') -Destination $stageBridgeDirectory -Force
            Remove-Item -LiteralPath (Join-Path $stagePayloadDirectory 'Dx11FsrBridge.dll') -Force -ErrorAction SilentlyContinue

            $optiSourceDirectory = Get-OptiScalerSourceDirectory -TemporaryDirectory $temporaryDirectory -SourceMode $OptiSourceMode
            $finalOptiDirectory = Join-Path $targetDirectory 'payload\OptiScaler'
            $stageOptiDirectory = Join-Path $stageDirectory 'payload\OptiScaler'
            $existingOptiDirectory = if (-not [string]::IsNullOrWhiteSpace($existingDirectory) -and
                (Test-Path -LiteralPath (Join-Path $existingDirectory 'payload\OptiScaler') -PathType Container)) {
                Join-Path $existingDirectory 'payload\OptiScaler'
            } else { '' }
            if (Test-Path -LiteralPath $stageOptiDirectory) { Remove-Item -LiteralPath $stageOptiDirectory -Recurse -Force }
            Copy-CuratedOptiScaler -SourceDirectory $optiSourceDirectory -Destination $stageOptiDirectory -FinalOptiDirectory $finalOptiDirectory
            $nvidiaSourceDirectory = if (-not [string]::IsNullOrWhiteSpace($existingOptiDirectory)) { $existingOptiDirectory } else { '' }
            Install-NvidiaDlssIfNeeded `
                -Destination $stageOptiDirectory `
                -TemporaryDirectory $temporaryDirectory `
                -ExistingOptiDirectory $nvidiaSourceDirectory `
                -BundledNvidiaDirectory $packagedNvidiaDirectory
            Set-OptiScalerConfiguration -Path (Join-Path $stageOptiDirectory 'OptiScaler.ini') -FinalOptiDirectory $finalOptiDirectory

            foreach ($entry in $optiBinaryHashes.GetEnumerator()) {
                Assert-FileHash -Path (Join-Path $stageOptiDirectory $entry.Key) -ExpectedSha256 $entry.Value -Label "已部署 $($entry.Key)"
            }
            $upscalingManifest = Get-OptiScalerUpscalingManifest
            foreach ($forbidden in @($upscalingManifest.ForbiddenFiles)) {
                if (Test-Path -LiteralPath (Join-Path $stageOptiDirectory $forbidden)) { throw "非帧生成包包含禁止组件: $forbidden" }
            }
        }

        if ($ReShade) {
            $stageReShadeDirectory = Join-Path $stageDirectory 'payload\ReShade'
            if (Test-Path -LiteralPath $stageReShadeDirectory) { Remove-Item -LiteralPath $stageReShadeDirectory -Recurse -Force }
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $stageReShadeDirectory) | Out-Null
            Copy-Item -LiteralPath (Join-Path $bundle 'payload\ReShade') -Destination $stageReShadeDirectory -Recurse -Force
            foreach ($fileName in @('ReShade.ini', 'ReShadePreset.ini')) {
                $stageConfig = Join-Path $stageReShadeDirectory $fileName
                Copy-Item -LiteralPath (Join-Path $stageReShadeDirectory "default_config\$fileName") -Destination $stageConfig -Force
            }
            $finalReShadeDirectory = Join-Path $targetDirectory 'payload\ReShade'
            $finalShaderDirectory = Join-Path $finalReShadeDirectory 'reshade-shaders'
            $stageReShadeIni = Join-Path $stageReShadeDirectory 'ReShade.ini'
            Set-IniValue -Path $stageReShadeIni -Section 'ADDON' -Key 'AddonPath' -Value (Join-Path $finalShaderDirectory 'Addons')
            Set-IniValue -Path $stageReShadeIni -Section 'GENERAL' -Key 'EffectSearchPaths' -Value (Join-Path $finalShaderDirectory 'Shaders')
            Set-IniValue -Path $stageReShadeIni -Section 'GENERAL' -Key 'TextureSearchPaths' -Value (Join-Path $finalShaderDirectory 'Textures')
            Set-IniValue -Path $stageReShadeIni -Section 'GENERAL' -Key 'PresetPath' -Value (Join-Path $finalReShadeDirectory 'ReShadePreset.ini')
            Set-IniValue -Path $stageReShadeIni -Section 'SCREENSHOT' -Key 'SavePath' -Value (Join-Path $finalReShadeDirectory 'Screenshots')
        }

        if (-not (Test-Path -LiteralPath $stagePluginConfig -PathType Leaf)) {
            throw '必须先安装原神FSR2桥接插件核心模块。'
        }
        Set-IniValue -Path $stagePluginConfig -Section 'General' -Key 'Name' -Value '原神FSR2桥接插件'
        Set-IniValue -Path $stagePluginConfig -Section 'General' -Key 'Description' -Value '按顺序加载 FSR2 Bridge、OptiScaler 与 ReShade'
        Set-IniValue -Path $stagePluginConfig -Section 'General' -Key 'File' -Value 'FSR-Bridge-Plugin.dll'
        Set-IniValue -Path $stagePluginConfig -Section 'General' -Key 'Version' -Value (Get-CurrentPackageVersion)
        Set-IniValue -Path $stagePluginConfig -Section 'BridgePath' -Key 'Value' -Value 'payload\Bridge\Dx11FsrBridge.dll'
        if ($Core -and ($Operation -eq 'Install' -or [string]::IsNullOrWhiteSpace($existingDirectory))) {
            Set-IniValue -Path $stagePluginConfig -Section 'EnableBridge' -Key 'Value' -Value '1'
            Set-IniValue -Path $stagePluginConfig -Section 'EnableOptiScaler' -Key 'Value' -Value '1'
        }
        if ($ReShade -and $Operation -eq 'Install') {
            Set-IniValue -Path $stagePluginConfig -Section 'EnableReShade' -Key 'Value' -Value '1'
        }
        elseif ([string]::IsNullOrWhiteSpace($existingDirectory)) {
            Set-IniValue -Path $stagePluginConfig -Section 'EnableReShade' -Key 'Value' -Value '0'
        }

        if (-not [string]::IsNullOrWhiteSpace($existingDirectory)) {
            Move-Item -LiteralPath $existingDirectory -Destination $backupDirectory
        }
        try {
            Move-Item -LiteralPath $stageDirectory -Destination $targetDirectory
            $installed = $true
        }
        catch {
            if ((Test-Path -LiteralPath $backupDirectory) -and
                -not [string]::IsNullOrWhiteSpace($existingDirectory) -and
                -not (Test-Path -LiteralPath $existingDirectory)) {
                Move-Item -LiteralPath $backupDirectory -Destination $existingDirectory
            }
            throw
        }
        if (Test-Path -LiteralPath $backupDirectory) { Remove-Item -LiteralPath $backupDirectory -Recurse -Force }
    }
    finally {
        if (Test-Path -LiteralPath $temporaryDirectory) { Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force }
        if (-not $installed -and (Test-Path -LiteralPath $stageDirectory)) { Remove-Item -LiteralPath $stageDirectory -Recurse -Force }
    }
    return $targetDirectory
}

function Get-FufuModuleState {
    param([string]$FufuRoot)
    $pluginDirectory = Join-Path $FufuRoot "Plugins\$pluginDirectoryName"
    $coreFiles = @(
        'FSR-Bridge-Plugin.dll',
        'config.ini',
        'payload\Bridge\Dx11FsrBridge.dll',
        'payload\OptiScaler\OptiScaler.dll'
    )
    $coreInstalled = $true
    foreach ($relativePath in $coreFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $pluginDirectory $relativePath) -PathType Leaf)) {
            $coreInstalled = $false
            break
        }
    }
    $reShadeInstalled = Test-Path -LiteralPath (Join-Path $pluginDirectory 'payload\ReShade\ReShade64.dll') -PathType Leaf
    return [pscustomobject]@{
        PluginDirectory = $pluginDirectory
        Core = $coreInstalled
        ReShade = $reShadeInstalled
    }
}

function Write-FufuModuleCatalog {
    param([string]$FufuRoot, [switch]$IncludeSelfUpdate)
    $state = Get-FufuModuleState -FufuRoot $FufuRoot
    $coreMark = if ($state.Core) { '[√]' } else { '[ ]' }
    $hdrMark = if ($state.ReShade) { '[√]' } else { '[ ]' }
    Write-Host "  1. $coreMark 原神FSR2桥接插件（Bridge + OptiScaler 0.9.3）"
    Write-Host "  2. $hdrMark ReShade + RenoDX HDR"
    if ($IncludeSelfUpdate) { Write-Host '  3. [√] 管理脚本 / 发行资源' }
}

function Select-FufuModuleSet {
    param([string]$ActionName, [switch]$IncludeSelfUpdate)
    $maximum = if ($IncludeSelfUpdate) { 3 } else { 2 }
    Write-Host ''
    Write-Host "输入要${ActionName}的模块序号，多个序号可用空格或逗号分隔。" -ForegroundColor Cyan
    Write-Host '输入 A 选择全部，输入 0 返回。' -ForegroundColor DarkGray
    while ($true) {
        $value = (Read-Host '请输入模块序号').Trim()
        if ($value -eq '0') { return @() }
        if ($value -match '(?i)^A$') { return @(1..$maximum) }
        $items = @($value -split '[,，\s]+' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
        $selection = [Collections.Generic.List[int]]::new()
        $valid = $items.Count -gt 0
        foreach ($item in $items) {
            $number = 0
            if (-not [int]::TryParse($item, [ref]$number) -or $number -lt 1 -or $number -gt $maximum) {
                $valid = $false
                break
            }
            if (-not $selection.Contains($number)) { $selection.Add($number) }
        }
        if ($valid) { return @($selection) }
        Write-Host '模块序号无效，请重新输入。' -ForegroundColor Yellow
    }
}

function Get-CurrentPackageVersion {
    if (Test-Path -LiteralPath $packageVersionPath -PathType Leaf) {
        $versionText = (Get-Content -LiteralPath $packageVersionPath -Raw -Encoding UTF8).Trim()
        if ($versionText -match '^\d+(\.\d+)+$') { return $versionText }
    }
    $bridgePath = Join-Path $bundle 'payload\Bridge\Dx11FsrBridge.dll'
    if (Test-Path -LiteralPath $bridgePath -PathType Leaf) {
        $versionText = [Diagnostics.FileVersionInfo]::GetVersionInfo($bridgePath).ProductVersion
        if ([string]$versionText -match '^\d+(\.\d+)+') { return $matches[0] }
    }
    return '0.0.0'
}

function Start-PackageSelfUpdate {
    Write-Host ''
    Write-Host '正在检查芙芙管理脚本和发行资源更新...' -ForegroundColor Cyan
    $temporaryDirectory = $null
    try {
        $release = Invoke-RestMethod -Headers @{ 'User-Agent' = 'FSR-Bridge-Plugin-SelfUpdater' } `
            -Uri "https://api.github.com/repos/$selfUpdateRepository/releases/latest"
        $assets = @($release.assets | Where-Object {
            $_.name -match '^(FuFuLauncherPlugin\.Lite_v|芙芙启动器插件包Lite_|原神FSR2桥接插件_).+\.zip$'
        })
        $asset = @($assets | Sort-Object @{ Expression = {
            if ($_.name -like 'FuFuLauncherPlugin.Lite_v*') { 0 }
            elseif ($_.name -like '芙芙启动器插件包Lite_*') { 1 }
            else { 2 }
        } } | Select-Object -First 1)
        if ($asset.Count -eq 0) {
            Write-Host '最新 Release 尚未提供芙芙插件更新包。' -ForegroundColor Yellow
            return $false
        }
        $currentVersion = Get-CurrentPackageVersion
        $latestVersion = ([string]$release.tag_name).TrimStart('v')
        if ($latestVersion -match '^\d+(\.\d+)+$' -and [version]$currentVersion -ge [version]$latestVersion) {
            Write-Host "管理脚本和发行资源已经是最新版本 v$currentVersion。" -ForegroundColor Green
            return $false
        }
        if (-not (Test-Path -LiteralPath $selfUpdateHelperPath -PathType Leaf)) {
            throw '当前包缺少 Apply-PackageUpdate.ps1，请手动下载最新发布包。'
        }

        $temporaryDirectory = Join-Path ([IO.Path]::GetTempPath()) ('FSR-Bridge-Plugin-SelfUpdate-' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Force -Path $temporaryDirectory | Out-Null
        $packagePath = Join-Path $temporaryDirectory ([string]$asset[0].name)
        Invoke-WebRequest -UseBasicParsing -Headers @{ 'User-Agent' = 'FSR-Bridge-Plugin-SelfUpdater' } `
            -Uri ([string]$asset[0].browser_download_url) -OutFile $packagePath
        if ([string]$asset[0].digest -match '^sha256:(.+)$') {
            $actualHash = (Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash
            if (-not [string]::Equals($actualHash, $matches[1], [StringComparison]::OrdinalIgnoreCase)) {
                throw '芙芙插件更新包 SHA-256 校验失败。'
            }
        }
        $expanded = Join-Path $temporaryDirectory 'expanded'
        Expand-Archive -LiteralPath $packagePath -DestinationPath $expanded -Force
        foreach ($required in @(
            'Install-FufuPlugin.ps1', 'Apply-PackageUpdate.ps1', 'Package-Version.txt',
            'FSR-Bridge-Plugin.dll', 'config.ini', 'payload\Bridge\Dx11FsrBridge.dll',
            'payload\OptiScaler\default_config\OptiScaler.ini', 'payload\OptiScaler\default_config\OptiScaler-UpscalingFiles.json',
            'payload\ReShade\default_config\ReShade.ini',
            'payload\ReShade\default_config\ReShadePreset.ini'
        )) {
            if (-not (Test-Path -LiteralPath (Join-Path $expanded $required) -PathType Leaf)) {
                throw "更新包缺少文件: $required"
            }
        }
        foreach ($forbidden in @('OptiScaler.dll', 'unlockfps_nc.exe', 'nvngx_dlss.dll')) {
            if (Get-ChildItem -LiteralPath $expanded -Recurse -File | Where-Object { $_.Name -ieq $forbidden }) {
                throw "更新包包含不应内置的第三方组件: $forbidden"
            }
        }
        $helperCopy = Join-Path $temporaryDirectory 'Apply-PackageUpdate.ps1'
        Copy-Item -LiteralPath $selfUpdateHelperPath -Destination $helperCopy -Force
        Start-Process -FilePath 'powershell.exe' -ArgumentList @(
            '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $helperCopy + '"'),
            '-ParentProcessId', $PID,
            '-SourceDirectory', ('"' + $expanded + '"'),
            '-TargetDirectory', ('"' + $root + '"'),
            '-RelaunchScript', ('"' + (Join-Path $root 'Install-FufuPlugin.ps1') + '"')
        ) | Out-Null
        Write-Host "已下载管理脚本 v$latestVersion，当前窗口关闭后自动替换并重新打开。" -ForegroundColor Green
        $script:SelfUpdateStarted = $true
        return $true
    }
    catch {
        Write-Host "管理脚本更新失败: $($_.Exception.Message)" -ForegroundColor Red
        if (-not [string]::IsNullOrWhiteSpace($temporaryDirectory) -and (Test-Path -LiteralPath $temporaryDirectory)) {
            Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force -ErrorAction SilentlyContinue
        }
        return $false
    }
}

function Invoke-FufuInstallWizard {
    param([string]$FufuRoot)
    Write-Header -Title '安装模块'
    Write-FufuModuleCatalog -FufuRoot $FufuRoot
    $selection = @(Select-FufuModuleSet -ActionName '安装')
    if ($selection.Count -eq 0) { return }
    $state = Get-FufuModuleState -FufuRoot $FufuRoot
    if ((2 -in $selection) -and -not $state.Core -and -not (1 -in $selection)) {
        Write-Host 'HDR 模块依赖核心模块，已同时选择模块 1。' -ForegroundColor Yellow
        $selection += 1
    }
    Ensure-FufuWriteAccess -FufuRoot $FufuRoot | Out-Null
    $source = if (1 -in $selection) { Select-OptiScalerSource } else { 'Auto' }
    $installedPath = Install-FufuPlugin -FufuRoot $FufuRoot -OptiSourceMode $source -Operation Install `
        -Core:(1 -in $selection) -ReShade:(2 -in $selection)
    Remove-Item -LiteralPath $errorLogPath -Force -ErrorAction SilentlyContinue
    Write-Host ''
    Write-Host '[√] 所选模块安装完成' -ForegroundColor Green
    Write-Host "插件目录: $installedPath"
    Pause-Installer
}

function Invoke-FufuUpdateWizard {
    param([string]$FufuRoot)
    Write-Header -Title '更新模块'
    Write-FufuModuleCatalog -FufuRoot $FufuRoot -IncludeSelfUpdate
    $selection = @(Select-FufuModuleSet -ActionName '更新' -IncludeSelfUpdate)
    if ($selection.Count -eq 0) { return }
    $state = Get-FufuModuleState -FufuRoot $FufuRoot
    $core = (1 -in $selection) -and $state.Core
    $reShade = (2 -in $selection) -and $state.ReShade
    if ((1 -in $selection) -and -not $state.Core) { Write-Host '核心模块尚未安装，已跳过；请从“安装模块”进入。' -ForegroundColor Yellow }
    if ((2 -in $selection) -and -not $state.ReShade) { Write-Host 'HDR 模块尚未安装，已跳过；请从“安装模块”进入。' -ForegroundColor Yellow }
    if ($core -or $reShade) {
        Ensure-FufuWriteAccess -FufuRoot $FufuRoot | Out-Null
        $source = if ($core) { Select-OptiScalerSource } else { 'Auto' }
        Install-FufuPlugin -FufuRoot $FufuRoot -OptiSourceMode $source -Operation Update -Core:$core -ReShade:$reShade | Out-Null
        Remove-Item -LiteralPath $errorLogPath -Force -ErrorAction SilentlyContinue
        Write-Host '[√] 所选插件模块更新完成。' -ForegroundColor Green
    }
    if (3 -in $selection) {
        if (Start-PackageSelfUpdate) { return }
    }
    Pause-Installer
}

function Invoke-FufuUninstallWizard {
    param([string]$FufuRoot)
    Write-Header -Title '卸载模块'
    Write-FufuModuleCatalog -FufuRoot $FufuRoot
    $selection = @(Select-FufuModuleSet -ActionName '卸载')
    if ($selection.Count -eq 0) { return }
    $state = Get-FufuModuleState -FufuRoot $FufuRoot
    if (-not $state.Core -and -not $state.ReShade) {
        Write-Host '没有检测到已安装的插件模块。' -ForegroundColor Yellow
        Pause-Installer
        return
    }
    Ensure-FufuWriteAccess -FufuRoot $FufuRoot | Out-Null
    Assert-NoRunningProcesses -FufuRoot $FufuRoot
    $pluginsDirectory = Join-Path $FufuRoot 'Plugins'
    $targetDirectory = $state.PluginDirectory
    Assert-ChildPath -Parent $pluginsDirectory -Child $targetDirectory
    if (1 -in $selection) {
        $confirm = (Read-Host '卸载核心模块会同时移除 HDR 模块，输入 YES 确认').Trim()
        if ($confirm -cne 'YES') { return }
        if (Test-Path -LiteralPath $targetDirectory -PathType Container) {
            $backup = Join-Path $pluginsDirectory ('.fsu-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
            Assert-ChildPath -Parent $pluginsDirectory -Child $backup
            Move-Item -LiteralPath $targetDirectory -Destination $backup
            try { Remove-Item -LiteralPath $backup -Recurse -Force }
            catch {
                if ((Test-Path -LiteralPath $backup) -and -not (Test-Path -LiteralPath $targetDirectory)) {
                    Move-Item -LiteralPath $backup -Destination $targetDirectory
                }
                throw
            }
        }
        Write-Host '[√] 原神FSR2桥接插件已卸载。' -ForegroundColor Green
        Pause-Installer
        return
    }
    if ((2 -in $selection) -and $state.ReShade) {
        $stage = Join-Path $pluginsDirectory ('.fsi-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
        $backup = Join-Path $pluginsDirectory ('.fsb-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
        Assert-ChildPath -Parent $pluginsDirectory -Child $stage
        Assert-ChildPath -Parent $pluginsDirectory -Child $backup
        New-Item -ItemType Directory -Force -Path $stage | Out-Null
        Get-ChildItem -LiteralPath $targetDirectory -Force | Copy-Item -Destination $stage -Recurse -Force
        Remove-Item -LiteralPath (Join-Path $stage 'payload\ReShade') -Recurse -Force -ErrorAction SilentlyContinue
        Set-IniValue -Path (Join-Path $stage 'config.ini') -Section 'EnableReShade' -Key 'Value' -Value '0'
        Move-Item -LiteralPath $targetDirectory -Destination $backup
        try { Move-Item -LiteralPath $stage -Destination $targetDirectory }
        catch {
            if ((Test-Path -LiteralPath $backup) -and -not (Test-Path -LiteralPath $targetDirectory)) {
                Move-Item -LiteralPath $backup -Destination $targetDirectory
            }
            throw
        }
        Remove-Item -LiteralPath $backup -Recurse -Force
        Write-Host '[√] ReShade + RenoDX HDR 已卸载。' -ForegroundColor Green
    }
    Pause-Installer
}

function Show-FufuAbout {
    Write-Header -Title '关于原神FSR2桥接插件'
    Write-Host "发行资源版本: v$(Get-CurrentPackageVersion)"
    Write-Host '核心后端: OptiScaler 0.9.3 正式版（非帧生成）'
    Write-Host '插件目录名: FSR-Bridge-Plugin'
    Write-Host '项目主页: https://github.com/AizawaHikaru233/genshin_fsr_brigde'
    Pause-Installer
}

function Write-ManagedError {
    param([Management.Automation.ErrorRecord]$ErrorRecord)
    [IO.File]::WriteAllText($errorLogPath, ($ErrorRecord | Out-String), [Text.UTF8Encoding]::new($false))
    Write-Host ''
    Write-Host '[×] 操作失败' -ForegroundColor Red
    Write-Host $ErrorRecord.Exception.Message -ForegroundColor Red
    Write-Host "详细错误已保存到: $errorLogPath" -ForegroundColor DarkGray
    Pause-Installer
}

try {
    $selectedFufuRoot = Select-FufuRoot -RequestedPath $FufuPath
    if ($null -eq $selectedFufuRoot) { exit 0 }
}
catch {
    Write-ManagedError -ErrorRecord $_
    exit 1
}

if ($NonInteractive) {
    try {
        Ensure-FufuWriteAccess -FufuRoot $selectedFufuRoot | Out-Null
        $selectedOptiSource = Select-OptiScalerSource
        Install-FufuPlugin -FufuRoot $selectedFufuRoot -OptiSourceMode $selectedOptiSource -Operation Install -Core -ReShade | Out-Null
        Remove-Item -LiteralPath $errorLogPath -Force -ErrorAction SilentlyContinue
        exit 0
    }
    catch {
        Write-ManagedError -ErrorRecord $_
        exit 1
    }
}

while ($true) {
    $moduleState = Get-FufuModuleState -FufuRoot $selectedFufuRoot
    Write-Header -Title '原神FSR2桥接插件管理器'
    Write-Host "[√] 芙芙目录: $selectedFufuRoot" -ForegroundColor Green
    Write-Host "[√] 插件目录: $($moduleState.PluginDirectory)" -ForegroundColor Green
    Write-Host ''
    Write-FufuModuleCatalog -FufuRoot $selectedFufuRoot
    Write-Host ''
    Write-Host '  1. 安装模块' -ForegroundColor Cyan
    Write-Host '  2. 更新模块'
    Write-Host '  3. 卸载模块'
    Write-Host '  4. 更换芙芙启动器目录'
    Write-Host '  5. 关于'
    Write-Host '  0. 退出'
    $choice = (Read-Host '请输入选项').Trim()
    try {
        switch ($choice) {
            '1' { Invoke-FufuInstallWizard -FufuRoot $selectedFufuRoot }
            '2' { Invoke-FufuUpdateWizard -FufuRoot $selectedFufuRoot }
            '3' { Invoke-FufuUninstallWizard -FufuRoot $selectedFufuRoot }
            '4' {
                $newRoot = Select-FufuRoot -ForceSelection
                if ($null -ne $newRoot) { $selectedFufuRoot = $newRoot }
            }
            '5' { Show-FufuAbout }
            '0' { break }
            default { Write-Host '无效选项。' -ForegroundColor Red; Start-Sleep -Milliseconds 700 }
        }
    }
    catch { Write-ManagedError -ErrorRecord $_ }
    if ($choice -eq '0' -or $script:SelfUpdateStarted) { break }
}
