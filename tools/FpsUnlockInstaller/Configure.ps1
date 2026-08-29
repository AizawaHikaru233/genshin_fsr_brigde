param(
    [string]$GamePath,
    [int]$FpsTarget = 0,
    [switch]$DisableBridge,
    [switch]$DisableOptiScaler,
    [switch]$DisableAntiBlur,
    [switch]$DisableHDR,
    [ValidateSet('Auto', 'Manual', 'Existing')]
    [string]$UnlockerSource,
    [ValidateSet('Auto', 'Manual', 'Existing')]
    [string]$OptiScalerSource,
    [ValidateSet('Auto', 'Bundled', 'Existing')]
    [string]$ReShadeSource,
    [string]$UnlockerPackagePath,
    [string]$OptiScalerPackagePath,
    [switch]$EnsureNvidiaDlssOnly,
    [switch]$NonInteractive,
    [switch]$NoShortcut,
    [switch]$ResetPluginConfigsOnly,
    [switch]$PreserveExistingConfigs,
    [ValidateSet('Auto', 'zh-CN', 'en-US')]
    [string]$Language = 'Auto'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
[Console]::InputEncoding = [System.Text.Encoding]::UTF8
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
$script:CachedVideoControllers = $null # Win32_VideoController 查询缓存（严格模式下必须先初始化）

$scriptDirectory = [IO.Path]::GetFullPath((Split-Path -Parent $PSCommandPath))
$root = if ([IO.Path]::GetFileName($scriptDirectory) -ieq 'scripts') {
    [IO.Path]::GetFullPath((Split-Path -Parent $scriptDirectory))
} else {
    $scriptDirectory
}
$payload = Join-Path $root 'payload'
$bridgeDir = Join-Path $payload 'Bridge'
$optiRootDir = Join-Path $payload 'OptiScaler'
$optiDir = $optiRootDir
$reshadeDir = Join-Path $payload 'ReShade'
$bridgeDll = Join-Path $bridgeDir 'Dx11FsrBridge.dll'
$optiDll = Join-Path $optiDir 'OptiScaler.dll'
$optiIni = Join-Path $optiDir 'OptiScaler.ini'
$optiDefaultIni = Join-Path $optiDir 'OptiScaler.default.ini'
$packagedDefaultConfigDir = Join-Path $payload 'default_config'
$packagedOptiTemplateDir = $packagedDefaultConfigDir
$sharedOptiTemplateDir = Join-Path (Split-Path -Parent $root) 'SharedResources\OptiScaler\default_config'
$optiTemplateDir = if (Test-Path -LiteralPath $packagedOptiTemplateDir -PathType Container) {
    $packagedOptiTemplateDir
} else {
    $sharedOptiTemplateDir
}
$packagedReShadeTemplateDir = $packagedDefaultConfigDir
$sharedReShadeTemplateDir = Join-Path (Split-Path -Parent $root) 'SharedResources\ReShade\default_config'
$reShadeTemplateDir = if (Test-Path -LiteralPath $packagedReShadeTemplateDir -PathType Container) {
    $packagedReShadeTemplateDir
} else {
    $sharedReShadeTemplateDir
}
$optiFallbackTemplate = Join-Path $optiTemplateDir 'OptiScaler.ini'
$optiUpscalingManifest = Join-Path $optiTemplateDir 'OptiScaler-UpscalingFiles.json'
$reshadeIniTemplate = Join-Path $reShadeTemplateDir 'ReShade.ini'
$reshadePresetTemplate = Join-Path $reShadeTemplateDir 'ReShadePreset.ini'
$fakeNvapiIni = Join-Path $optiDir 'fakenvapi.ini'
$fakeNvapiDefaultIni = Join-Path $optiDir 'fakenvapi.default.ini'
$bundledDlss = Join-Path $payload 'NVIDIA\DLSS\nvngx_dlss.dll'
$installedDlss = Join-Path $optiDir 'nvngx_dlss.dll'
$antiBlurDll = Join-Path $payload 'AntiPlayerMosaic\AntiPlayerMosaic.dll'
$reshadeDll = Join-Path $reshadeDir 'ReShade64.dll'
$shaderDir = Join-Path $reshadeDir 'reshade-shaders'
$unlocker = Join-Path $root 'unlockfps_nc.exe'
$fpsConfig = Join-Path $root 'fps_config.json'
$reShadeResourcesScript = Join-Path $scriptDirectory 'ReShadeResources.ps1'
$nonFrameGenerationEdition = Test-Path -LiteralPath (Join-Path $root 'NonFrameGeneration.edition') -PathType Leaf
# 上游版本基线：由 tools\Update-UpstreamComponents.ps1 在构建时生成并随包内置。
# 安装脚本按同一基线下载缺失组件；旧包无此文件时回退内置默认值。
$script:UpstreamVersions = $null
$upstreamVersionsPath = Join-Path $root 'upstream-versions.json'
if (Test-Path -LiteralPath $upstreamVersionsPath -PathType Leaf) {
    try { $script:UpstreamVersions = Get-Content -LiteralPath $upstreamVersionsPath -Raw -Encoding UTF8 | ConvertFrom-Json } catch { $script:UpstreamVersions = $null }
}
$optiRepository = 'optiscaler/OptiScaler'
if ($null -ne $script:UpstreamVersions -and $null -ne $script:UpstreamVersions.optiscaler -and
    -not [string]::IsNullOrWhiteSpace([string]$script:UpstreamVersions.optiscaler.tag)) {
    $optiTag = [string]$script:UpstreamVersions.optiscaler.tag
    $optiAssetName = [string]$script:UpstreamVersions.optiscaler.asset
    $optiArchiveSha256 = [string]$script:UpstreamVersions.optiscaler.archiveSha256
    $optiFileVersion = [string]$script:UpstreamVersions.optiscaler.fileVersion
}
else {
    $optiTag = 'v0.9.4'
    $optiAssetName = 'Optiscaler_0.9.4-final.20260718._MM.7z'
    $optiArchiveSha256 = '575CB4DF866116093DF75AF607E37FD70E10F5163E0F23FD5C804142E80EF0AD'
    $optiFileVersion = '0.9.4.0'
}

. (Join-Path $scriptDirectory 'Localization.ps1')
. $reShadeResourcesScript
$script:Language = Get-InstallerLanguage -RequestedLanguage $Language
Initialize-InstallerLocalization -Language $script:Language
$script:OptiScalerUpscalingManifestData = $null

function Assert-File {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw (Convert-InstallerText -Value "缺少文件: $Path")
    }
}

function Assert-Directory {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw (Convert-InstallerText -Value "缺少目录: $Path")
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
    if (-not (Test-Path -LiteralPath $path)) { throw (Convert-InstallerText -Value "指定的文件或目录不存在: $path") }
    return (Resolve-Path -LiteralPath $path).Path
}

$script:GitHubProxyLatency = @{}

function Get-GitHubEndpointLatency {
    param([string]$HostName)
    if ($script:GitHubProxyLatency.ContainsKey($HostName)) { return [double]$script:GitHubProxyLatency[$HostName] }
    $latency = [double]::PositiveInfinity
    try {
        $ping = Test-Connection -ComputerName $HostName -Count 1 -ErrorAction Stop | Select-Object -First 1
        if ($null -ne $ping -and $ping.ResponseTime -ge 0) { $latency = [double]$ping.ResponseTime }
    }
    catch { }
    $script:GitHubProxyLatency[$HostName] = $latency
    return $latency
}

function Get-GitHubFallbackUrls {
    param([string]$Url)
    if ($Url -notmatch '^https://(api\.)?github\.com/') { return @($Url) }
    $proxies = @(
        [pscustomobject]@{ Host = 'ghfast.top'; Prefix = 'https://ghfast.top/' },
        [pscustomobject]@{ Host = 'gh-proxy.com'; Prefix = 'https://gh-proxy.com/' },
        [pscustomobject]@{ Host = 'ghproxy.net'; Prefix = 'https://ghproxy.net/' }
    ) | ForEach-Object {
        [pscustomobject]@{ Host = $_.Host; Prefix = $_.Prefix; Latency = (Get-GitHubEndpointLatency -HostName $_.Host) }
    } | Sort-Object Latency, Host
    $proxyUrls = @($proxies | ForEach-Object { $_.Prefix + $Url })
    return @($proxyUrls + @($Url))
}

function Get-GitHubLatestAsset {
    param([string]$Repository, [scriptblock]$AssetFilter, [string]$Tag)
    $headers = @{ 'User-Agent' = 'GenshinOneClick-Installer' }
    $releaseApi = if ([string]::IsNullOrWhiteSpace($Tag)) {
        "https://api.github.com/repos/$Repository/releases/latest"
    } else {
        "https://api.github.com/repos/$Repository/releases/tags/$Tag"
    }
    $failures = [Collections.Generic.List[string]]::new()
    foreach ($candidateUrl in @(Get-GitHubFallbackUrls -Url $releaseApi)) {
        try {
            $release = Invoke-RestMethod -Headers $headers -Uri $candidateUrl -TimeoutSec 20
            $asset = @($release.assets | Where-Object $AssetFilter | Select-Object -First 1)
            if ($asset.Count -eq 0) { throw "$Repository 的发行数据中没有需要的文件。" }
            return [pscustomobject]@{
                Tag = [string]$release.tag_name
                Page = [string]$release.html_url
                Name = [string]$asset[0].name
                Url = [string]$asset[0].browser_download_url
                Digest = [string]$asset[0].digest
            }
        }
        catch {
            $failures.Add("$candidateUrl : $($_.Exception.Message)")
        }
    }
    throw (Convert-InstallerText -Value "无法查询 $Repository 官方发行版。已尝试直连与全部备用下载路线。$([Environment]::NewLine)$($failures -join [Environment]::NewLine)")
}

function Invoke-OfficialDownload {
    param([string]$Url, [string]$Destination, [string]$ExpectedSha256)
    $failures = [Collections.Generic.List[string]]::new()
    foreach ($candidateUrl in @(Get-GitHubFallbackUrls -Url $Url)) {
        try {
            Remove-Item -LiteralPath $Destination -Force -ErrorAction SilentlyContinue
            Invoke-WebRequest -UseBasicParsing -Headers @{ 'User-Agent' = 'GenshinOneClick-Installer' } -Uri $candidateUrl -OutFile $Destination -TimeoutSec 180
            if (-not [string]::IsNullOrWhiteSpace($ExpectedSha256)) {
                $expectedHash = $ExpectedSha256 -replace '^sha256:', ''
                $actualHash = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash
                if (-not [string]::Equals($actualHash, $expectedHash, [StringComparison]::OrdinalIgnoreCase)) {
                    throw '下载文件 SHA-256 校验失败。'
                }
            }
            return
        }
        catch {
            $failures.Add("$candidateUrl : $($_.Exception.Message)")
        }
    }
    throw (Convert-InstallerText -Value "下载失败：直连与全部备用下载路线均不可用。$([Environment]::NewLine)$($failures -join [Environment]::NewLine)")
}

function Get-VideoControllersOnce {
    # 每安装会话只查询一次 Win32_VideoController（CIM/WMI 查询 ~100-500ms）
    if ($null -eq $script:CachedVideoControllers) {
        try {
            $script:CachedVideoControllers = @(Get-CimInstance -ClassName Win32_VideoController -ErrorAction Stop)
        }
        catch {
            try { $script:CachedVideoControllers = @(Get-WmiObject -Class Win32_VideoController -ErrorAction Stop) } catch { $script:CachedVideoControllers = @() }
        }
    }
    return @($script:CachedVideoControllers)
}

function Get-NvidiaVideoControllers {
    $controllers = @(Get-VideoControllersOnce)
    return @($controllers | Where-Object {
        ([string]$_.PNPDeviceID -match '(?i)VEN_10DE') -or
        ([string]$_.AdapterCompatibility -match '(?i)NVIDIA') -or
        ([string]$_.Name -match '(?i)NVIDIA')
    })
}

# Device ID 精确分类（核显名字通常只是 "AMD Radeon(TM) Graphics"，
# 不含型号，名字匹配识别不到）。Device ID 来源于 GPU 硬件 ID 汇总表。
$script:AmdFp8DevIds = @('7550', '7551', '7590')
$script:AmdInt8DevIds = @(
    '744C','7448','7449','744A','744B','745E','7460','7461','7470','747E','73F0','7480','7481','7483','7487','748B','7489','7499','749F',
    '15BF','15C8','164F','1900','1901',
    '150E','1586','1114','1902'
)
$script:NvidiaInt8DevIds = @(
    '1E02','1E03','1E04','1E07','1E81','1E82','1E84','1E87','1E89','1EC2','1EC7','1F02','1F03','1F06','1F07','1F08','1F42','1F47',
    '2182','2184','2187','2188','21C4','1F82','1F83',
    '1E90','1ED0','1E91','1ED1','1E93','1ED3','1F10','1F50','1F54','1F11','1F15','1F51','1F55','1F12','1F14',
    '2191','21D1','2192','1F91','1F92','1F94','1F96','1F99','1F9D','1F95','1FD9','1FDD','1F97','1F98','1F9C','1F9F','1FA0',
    '1E30','1FB0','1FF0','1FB1','1FB2','1FF2','1FB6','1FBA','1FB7','1FBB','1FBC','1FB8','1FB9','1FF9',
    '2203','2204','2205','2206','2207','2208','220A','2216','2414','2482','2484','2486','2487','2488','2489','248C','248D','248E',
    '24C7','24C8','24C9','2501','2503','2509','2544','2508','2582','2583','2584','25A7','25A9','25AD','25ED','25A6','25AA',
    '2420','2460','249C','24DC','249D','24DD','2520','2521','2560','2561','2523','2563','25A0','25E0','25A2','25A5','25E2','25E5','25AB','25AC','25EC',
    '2230','2231','2232','2233','24B0','2531','2571','25B0','25B2','25B6','2438','24B6','24B7','24B8','24B9','24BB','24BA','25B8','25B9','25BA','25BB','25BD','25BC',
    '2684','2685','2702','2704','2705','2709','2782','2783','2786','2788','2803','2805','2808','2882','2717','2757','27A0','27E0',
    '2820','2860','28A0','28E0','28A1','28E1','2822','28A3','28E3','26B1','26B2','26B3','27B0','27B1','27B2','28B0','2730','27BA','27BB','28B8','28B9','28BA','28BB',
    '2B85','2B87','2B8C','2C02','2C05','2F04','2F06','2D04','2D05','2D83','2C18','2C58','2C19','2C59','2F18','2F58','2D18','2D58','2D19','2D59','2D98','2DD8','2BB1','2BB4'
)
$script:NvidiaGtx16DevIds = @(
    '2182','2184','2187','2188','21C4','1F82','1F83',
    '2191','21D1','2192','1F91','1F92','1F94','1F96','1F99','1F9D','1F95','1FD9','1FDD','1F97','1F98','1F9C','1F9F','1FA0'
)

function Get-Fsr4GpuPolicy {
    param([object[]]$InputControllers)
    if ($null -ne $InputControllers) {
        $controllers = @($InputControllers)
    }
    else {
        $controllers = @(Get-VideoControllersOnce)
    }

    # 遍历全部显卡取"最优"策略（fp8 优先于 int8），不依赖 AdapterRAM 排序：
    # WMI 的 AdapterRAM 在 4GB 以上会截断，混合平台（RDNA4 独显 + RDNA3 核显）
    # 按内存排序可能先命中核显而错发 int8。
    # 型号匹配统一用 (?!\d) 结尾而非 \b：RX 7600M / 7700S / RTX 4090D 这类
    # 数字后紧跟字母的型号在 \b 下会漏配。
    $bestPolicy = $null
    foreach ($controller in $controllers) {
        $name = [string]$controller.Name
        $pnp = [string]$controller.PNPDeviceID
        $vendor = if ($pnp -match '(?i)VEN_([0-9A-F]{4})') { $matches[1].ToUpperInvariant() } else { '' }
        $mode = 'auto'
        $reason = 'unsupported'

        if ($vendor -eq '1002' -or $name -match '(?i)AMD|Radeon') {
            $dev = if ($pnp -match '(?i)DEV_([0-9A-F]{4})') { $matches[1].ToUpperInvariant() } else { '' }
            if ($dev -in $script:AmdFp8DevIds) {
                $mode = 'fp8'; $reason = 'AMD RDNA4'
            }
            elseif ($dev -in $script:AmdInt8DevIds) {
                $mode = 'int8'; $reason = 'AMD RDNA3/3.5'
            }
            elseif ($name -match '(?i)\bRX\s*9\d{3}(?!\d)|\bPRO\s+W9\d{3}(?!\d)') {
                $mode = 'fp8'; $reason = 'AMD RDNA4'
            }
            elseif ($name -match '(?i)\bRX\s*7\d{3}(?!\d)|\bPRO\s+W7\d{3}(?!\d)|\b(7[468]0|8[4689]0)M\b|\b80[456]0S\b') {
                $mode = 'int8'; $reason = 'AMD RDNA3/3.5'
            }
        }
        elseif ($vendor -eq '10DE' -or $name -match '(?i)NVIDIA|GeForce') {
            $dev = if ($pnp -match '(?i)DEV_([0-9A-F]{4})') { $matches[1].ToUpperInvariant() } else { '' }
            if ($dev -in $script:NvidiaInt8DevIds) {
                $mode = 'int8'; $reason = 'NVIDIA 16-50 系'
            }
            elseif ($name -match '(?i)\bRTX\s*(20|30|40|50)\d{2}(?!\d)' -or
                $name -match '(?i)\bGTX\s*16\d{2}(?!\d)') {
                $mode = 'int8'; $reason = 'NVIDIA RTX 20+ / GTX 16 Turing'
            }
        }
        elseif ($vendor -eq '8086' -or $name -match '(?i)Intel') {
            $dev = if ($pnp -match '(?i)DEV_([0-9A-F]{4})') { $matches[1].ToUpperInvariant() } else { '' }
            $devInt = if ($dev -ne '') { [Convert]::ToInt32($dev, 16) } else { -1 }
            if (($devInt -ge 0x5600 -and $devInt -le 0x56FF) -or ($devInt -ge 0xE200 -and $devInt -le 0xE2FF)) {
                $mode = 'int8'; $reason = 'Intel Arc'
            }
            elseif ($name -match '(?i)\bArc\b') {
                $mode = 'int8'; $reason = 'Intel Arc'
            }
        }

        if ($mode -eq 'auto') { continue }
        $candidate = [pscustomobject]@{
            Mode = $mode
            Reason = $reason
            Name = $name
            Vendor = $vendor
            PnpDeviceId = $pnp
        }
        if ($null -eq $bestPolicy -or ($mode -eq 'fp8' -and $bestPolicy.Mode -ne 'fp8')) {
            $bestPolicy = $candidate
        }
    }
    if ($null -ne $bestPolicy) { return $bestPolicy }

    return [pscustomobject]@{
        Mode = 'auto'
        Reason = 'no whitelist match'
        Name = ''
        Vendor = ''
        PnpDeviceId = ''
    }
}

function Set-Fsr4GpuPolicy {
    param([string]$Path)
    $policy = Get-Fsr4GpuPolicy
    $supported = $policy.Mode -ne 'auto'
    Set-IniValue -Path $Path -Section 'FSR' -Key 'Fsr4Update' -Value ($(if ($supported) { 'true' } else { 'false' }))
    Set-IniValue -Path $Path -Section 'FSR' -Key 'UpscalerIndex' -Value ($(if ($supported) { '0' } else { 'auto' }))
    Set-IniValue -Path $Path -Section 'FSR' -Key 'Fsr4ForceEnableInt8' -Value ($(if ($policy.Mode -eq 'int8') { 'true' } else { 'false' }))
    Set-IniValue -Path $Path -Section 'FSR' -Key 'FsrNonLinearColorSpace' -Value 'true'
    $name = if ([string]::IsNullOrWhiteSpace($policy.Name)) { 'unknown' } else { $policy.Name }
    Write-Host "FSR4 GPU policy: $($policy.Mode) / $($policy.Reason) / $name" -ForegroundColor Cyan
    return $policy
}

function Set-OptiScalerManagedSettings {
    param([string]$Path)
    # OptiScaler.ini 托管设置（与芙芙 bootstrap 保持一致）：
    #   - FSR4 策略（Fsr4Update/UpscalerIndex/Fsr4ForceEnableInt8/FsrNonLinearColorSpace）
    #   - Libraries.OptiDllPath 绝对路径
    #   - Log：LogToFile=true / LogLevel=2 / LogFileName=OptiScaler.log
    #   - FrameGen 版型（非帧生成版强制关闭）
    # Upscalers/Inputs/Plugins 等其余设置由 default_config 模板自足，脚本不覆写。
    Set-Fsr4GpuPolicy -Path $Path | Out-Null
    Set-IniValue -Path $Path -Section 'Libraries' -Key 'OptiDllPath' -Value ([IO.Path]::GetFullPath($optiDir).TrimEnd('\'))
    Set-IniValue -Path $Path -Section 'Log' -Key 'LogToFile' -Value 'true'
    Set-IniValue -Path $Path -Section 'Log' -Key 'LogLevel' -Value '2'
    Set-IniValue -Path $Path -Section 'Log' -Key 'LogFileName' -Value 'OptiScaler.log'
    if ($nonFrameGenerationEdition) {
        Set-IniValue -Path $Path -Section 'FrameGen' -Key 'Enabled' -Value 'false'
        Set-IniValue -Path $Path -Section 'FrameGen' -Key 'FGInput' -Value 'nofg'
        Set-IniValue -Path $Path -Section 'FrameGen' -Key 'FGOutput' -Value 'nofg'
    }
    else {
        Set-IniValue -Path $Path -Section 'FrameGen' -Key 'Enabled' -Value 'auto'
        Set-IniValue -Path $Path -Section 'FrameGen' -Key 'FGInput' -Value 'auto'
        Set-IniValue -Path $Path -Section 'FrameGen' -Key 'FGOutput' -Value 'auto'
    }
}

function Assert-NvidiaSignedFile {
    param([string]$Path)
    Assert-File -Path $Path
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne [Management.Automation.SignatureStatus]::Valid -or
        $null -eq $signature.SignerCertificate -or
        $signature.SignerCertificate.Subject -notmatch '(?i)NVIDIA Corporation') {
        throw (Convert-InstallerText -Value "NVIDIA DLSS 文件签名验证失败: $Path")
    }
}

function Install-NvidiaDlssIfNeeded {
    if (-not (Test-Path -LiteralPath $optiDll -PathType Leaf)) { return }
    # GTX 16 也可能使用 FSR4 INT8，但不应自动部署 DLSS Runtime。
    # RTX 精确判定：Device ID 属 16-50 且非 GTX 16；名字匹配仅兜底。
    $rtxControllers = @(
        Get-NvidiaVideoControllers | Where-Object {
            $pnp = [string]$_.PNPDeviceID
            $dev = if ($pnp -match '(?i)DEV_([0-9A-F]{4})') { $matches[1].ToUpperInvariant() } else { '' }
            ($dev -in $script:NvidiaInt8DevIds -and $dev -notin $script:NvidiaGtx16DevIds) -or
            [string]$_.Name -match '(?i)\bRTX\b'
        }
    )
    if ($rtxControllers.Count -eq 0) {
        Write-Host '未检测到 RTX 显卡，跳过 DLSS 超分组件补齐。' -ForegroundColor DarkGray
        return
    }
    if (Test-Path -LiteralPath $installedDlss -PathType Leaf) { return }

    $gpuNames = @($rtxControllers | ForEach-Object { [string]$_.Name } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -Unique)
    $gpuLabel = if ($gpuNames.Count -gt 0) { $gpuNames -join ' / ' } else { 'NVIDIA RTX GPU' }
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
        # 构建基线：Streamline 版本与 SHA 取自包内 upstream-versions.json（与 Update-UpstreamComponents.ps1 同步）。
        $dlssTag = $null
        $dlssExpectedSha = $null
        if ($null -ne $script:UpstreamVersions -and $null -ne $script:UpstreamVersions.dlss -and
            -not [string]::IsNullOrWhiteSpace([string]$script:UpstreamVersions.dlss.tag)) {
            $dlssTag = [string]$script:UpstreamVersions.dlss.tag
            $dlssExpectedSha = [string]$script:UpstreamVersions.dlss.archiveSha256
        }
        $asset = if ([string]::IsNullOrWhiteSpace($dlssTag)) {
            Get-GitHubLatestAsset -Repository 'NVIDIA-RTX/Streamline' -AssetFilter { $_.name -match '^streamline-sdk-v[0-9.]+\.zip$' }
        }
        else {
            Get-GitHubLatestAsset -Repository 'NVIDIA-RTX/Streamline' -Tag $dlssTag -AssetFilter { $_.name -match '^streamline-sdk-v[0-9.]+\.zip$' }
        }
        Write-Host "正在从 NVIDIA 官方 Streamline $($asset.Tag) 下载 DLSS 组件..." -ForegroundColor Cyan
        $packagePath = Join-Path $temporaryDirectory $asset.Name
        $expectedSha = if (-not [string]::IsNullOrWhiteSpace($dlssExpectedSha)) { $dlssExpectedSha } else { $asset.Digest }
        Invoke-OfficialDownload -Url $asset.Url -Destination $packagePath -ExpectedSha256 $expectedSha
        $expanded = Join-Path $temporaryDirectory 'expanded'
        Expand-ComponentPackage -PackagePath $packagePath -Destination $expanded
        $sourceDll = Get-ChildItem -LiteralPath $expanded -Recurse -File -Filter 'nvngx_dlss.dll' |
            Where-Object { $_.FullName -notmatch '(?i)[\\/]development[\\/]' } |
            Sort-Object @{ Expression = { if ($_.FullName -match '(?i)[\\/]bin[\\/]x64[\\/]nvngx_dlss\.dll$') { 0 } else { 1 } } }, FullName |
            Select-Object -First 1
        if ($null -eq $sourceDll) { throw (Convert-InstallerText -Value 'NVIDIA Streamline 官方包中没有找到生产版 nvngx_dlss.dll。') }
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
        if ($null -eq $tar) { throw (Convert-InstallerText -Value '系统中没有 tar.exe，无法解压 7z；请手动解压后选择解压目录。') }
        & $tar.Source -xf $PackagePath -C $Destination
        if ($LASTEXITCODE -ne 0) { throw (Convert-InstallerText -Value "7z 解压失败，退出代码: $LASTEXITCODE") }
        return
    }
    throw (Convert-InstallerText -Value "不支持的压缩包格式: $extension")
}

function Copy-DirectoryContents {
    param([string]$Source, [string]$Destination)
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    Get-ChildItem -LiteralPath $Source -Force | Copy-Item -Destination $Destination -Recurse -Force
}

function Install-ReShadeResources {
    param([ValidateSet('Auto', 'Bundled', 'Existing')][string]$Mode)
    if ($Mode -in @('Bundled', 'Existing')) {
        Assert-File -Path $reshadeDll
        Assert-Directory -Path $shaderDir
        return
    }

    $temporaryDirectory = Join-Path ([IO.Path]::GetTempPath()) ("GenshinOneClick-ReShade-" + [guid]::NewGuid().ToString('N'))
    $stageDirectory = Join-Path $temporaryDirectory 'ReShade'
    New-Item -ItemType Directory -Force -Path $temporaryDirectory | Out-Null
    try {
        New-OfficialReShadePayload `
            -DestinationDirectory $stageDirectory `
            -BundledSourceDirectory $reshadeDir `
            -TemporaryDirectory $temporaryDirectory `
            -UserAgent 'GenshinOneClick-ReShadeInstaller' | Out-Null

        foreach ($replaceDirectory in @('Shaders', 'Textures')) {
            Remove-Item -LiteralPath (Join-Path $shaderDir $replaceDirectory) -Recurse -Force -ErrorAction SilentlyContinue
        }
        New-Item -ItemType Directory -Force -Path $reshadeDir | Out-Null
        Copy-DirectoryContents -Source $stageDirectory -Destination $reshadeDir
        Assert-File -Path $reshadeDll
        Assert-ReShadeResourceHash `
            -Path $reshadeDll `
            -ExpectedSha256 (Get-ReShadeResourceSpec).ReShadeDllSha256 `
            -Label 'ReShade64.dll'
        Assert-Directory -Path $shaderDir
        Write-Host '已从官方来源安装 ReShade 与效果库。' -ForegroundColor Green
    }
    finally {
        if (Test-Path -LiteralPath $temporaryDirectory) {
            Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

function Get-OptiScalerUpscalingManifest {
    if ($null -ne $script:OptiScalerUpscalingManifestData) { return $script:OptiScalerUpscalingManifestData }
    Assert-File -Path $optiUpscalingManifest
    $manifest = Get-Content -LiteralPath $optiUpscalingManifest -Raw -Encoding UTF8 | ConvertFrom-Json
    if ([int]$manifest.SchemaVersion -ne 1 -or @($manifest.RuntimeFiles).Count -eq 0) {
        throw (Convert-InstallerText -Value "OptiScaler 超分文件清单无效: $optiUpscalingManifest")
    }
    $script:OptiScalerUpscalingManifestData = $manifest
    return $script:OptiScalerUpscalingManifestData
}

function Copy-CuratedOptiScaler {
    param([string]$SourceDirectory, [string]$Destination)
    $manifest = Get-OptiScalerUpscalingManifest
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    foreach ($relativePath in @($manifest.RuntimeFiles)) {
        $sourcePath = Join-Path $SourceDirectory ([string]$relativePath)
        Assert-File -Path $sourcePath
        $destinationPath = Join-Path $Destination ([string]$relativePath)
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destinationPath) | Out-Null
        Copy-Item -LiteralPath $sourcePath -Destination $destinationPath -Force
    }
    foreach ($licenseName in @($manifest.LicenseFiles)) {
        $sourcePath = Join-Path $SourceDirectory ("Licenses\" + [string]$licenseName)
        if (Test-Path -LiteralPath $sourcePath -PathType Leaf) {
            $licenseDirectory = Join-Path $Destination 'Licenses'
            New-Item -ItemType Directory -Force -Path $licenseDirectory | Out-Null
            Copy-Item -LiteralPath $sourcePath -Destination (Join-Path $licenseDirectory ([string]$licenseName)) -Force
        }
    }
    foreach ($licenseName in @($manifest.OptionalLicenseFiles)) {
        $sourcePath = Join-Path $SourceDirectory ("Licenses\" + [string]$licenseName)
        if (Test-Path -LiteralPath $sourcePath -PathType Leaf) {
            $licenseDirectory = Join-Path $Destination 'Licenses'
            New-Item -ItemType Directory -Force -Path $licenseDirectory | Out-Null
            Copy-Item -LiteralPath $sourcePath -Destination (Join-Path $licenseDirectory ([string]$licenseName)) -Force
        }
    }
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
            # 构建基线：版本与 SHA 取自包内 upstream-versions.json（与 Update-UpstreamComponents.ps1 同步）。
            $unlockTag = $null
            $unlockExpectedSha = $null
            if ($null -ne $script:UpstreamVersions -and $null -ne $script:UpstreamVersions.fpsunlock -and
                -not [string]::IsNullOrWhiteSpace([string]$script:UpstreamVersions.fpsunlock.tag)) {
                $unlockTag = [string]$script:UpstreamVersions.fpsunlock.tag
                $unlockExpectedSha = [string]$script:UpstreamVersions.fpsunlock.sha256
            }
            $asset = if ([string]::IsNullOrWhiteSpace($unlockTag)) {
                Get-GitHubLatestAsset -Repository '34736384/genshin-fps-unlock' -AssetFilter { $_.name -eq 'unlockfps_nc.exe' }
            }
            else {
                Get-GitHubLatestAsset -Repository '34736384/genshin-fps-unlock' -Tag $unlockTag -AssetFilter { $_.name -eq 'unlockfps_nc.exe' }
            }
            Write-Host "正在从官方发行版下载 FPS Unlocker $($asset.Tag)..." -ForegroundColor Cyan
            $source = Join-Path $temporaryDirectory 'unlockfps_nc.exe'
            $expectedSha = if (-not [string]::IsNullOrWhiteSpace($unlockExpectedSha)) { $unlockExpectedSha } else { $asset.Digest }
            Invoke-OfficialDownload -Url $asset.Url -Destination $source -ExpectedSha256 $expectedSha
            if ($expectedSha -match '^sha256:(.+)$') {
                $actualHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
                if (-not [string]::Equals($actualHash, $matches[1], [StringComparison]::OrdinalIgnoreCase)) {
                    throw (Convert-InstallerText -Value 'FPS Unlocker 下载文件 SHA-256 校验失败。')
                }
            }
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
                    throw (Convert-InstallerText -Value '手动选择的文件不是官方 FPS Unlocker 可执行文件。')
                }
                Copy-Item -LiteralPath $source -Destination $unlocker -Force
                Write-Host "FPS Unlocker SHA256: $((Get-FileHash -LiteralPath $unlocker -Algorithm SHA256).Hash)"
                return
            }
            $sourceExe = Get-ChildItem -LiteralPath $sourceDirectory -Recurse -File | Where-Object { $_.Name -in @('unlockfps_nc.exe', 'unlockfps_nc_signed.exe') } | Select-Object -First 1
            if ($null -eq $sourceExe) { throw (Convert-InstallerText -Value '所选目录或 ZIP 中没有找到 FPS Unlocker。') }
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
    param([string]$Mode, [string]$ManualPath, [switch]$PreserveExistingConfig)
    if ($Mode -eq 'Existing') {
        Assert-File -Path $optiDll
        Assert-File -Path $optiFallbackTemplate
        if (-not (Test-Path -LiteralPath $optiDefaultIni -PathType Leaf)) {
            Copy-Item -LiteralPath $optiFallbackTemplate -Destination $optiDefaultIni -Force
        }
        if (-not $PreserveExistingConfig -or -not (Test-Path -LiteralPath $optiIni -PathType Leaf)) {
            Copy-Item -LiteralPath $optiFallbackTemplate -Destination $optiIni -Force
        }
        return
    }
    $temporaryDirectory = Join-Path ([IO.Path]::GetTempPath()) ("GenshinOneClick-OptiScaler-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $temporaryDirectory | Out-Null
    try {
        if ($Mode -eq 'Auto') {
            $asset = Get-GitHubLatestAsset -Repository $optiRepository -Tag $optiTag -AssetFilter { $_.name -eq $optiAssetName }
            Write-Host "正在从官方发行版下载 OptiScaler $($asset.Tag)..." -ForegroundColor Cyan
            if ($asset.Name -ne $optiAssetName) {
                throw (Convert-InstallerText -Value "OptiScaler 官方资产名称不符合预期: $($asset.Name)")
            }
            $packagePath = Join-Path $temporaryDirectory $asset.Name
            Invoke-OfficialDownload -Url $asset.Url -Destination $packagePath -ExpectedSha256 $optiArchiveSha256
            $archiveHash = (Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash
            if ($archiveHash -ne $optiArchiveSha256) {
                throw (Convert-InstallerText -Value "OptiScaler 官方压缩包 SHA256 校验失败。实际值: $archiveHash")
            }
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
        if ($null -eq $mainDll) { throw (Convert-InstallerText -Value '所选 OptiScaler 包中没有找到 OptiScaler.dll。') }
        if ($Mode -eq 'Auto' -and $mainDll.VersionInfo.FileVersion -ne $optiFileVersion) {
            # 仅要求必要组件存在：版本与预置不一致时提示但不中断（用户可自行使用其他 OptiScaler 版本）
            Write-Warning (Convert-InstallerText -Value "OptiScaler 版本 $($mainDll.VersionInfo.FileVersion) 与预置 $optiFileVersion 不同，继续安装（必要组件存在即可）")
        }
        $sourceDirectory = $mainDll.Directory.FullName
        if (-not (Test-Path -LiteralPath (Join-Path $sourceDirectory 'amd_fidelityfx_upscaler_dx12.dll') -PathType Leaf)) {
            throw (Convert-InstallerText -Value 'OptiScaler 包不完整：缺少 amd_fidelityfx_upscaler_dx12.dll。')
        }
        $componentDefaultIni = Join-Path $sourceDirectory 'OptiScaler.default.ini'
        $componentIni = Join-Path $sourceDirectory 'OptiScaler.ini'
        # 优先使用包内 default_config 的官方模板；只有开发包未携带模板时，
        # 才使用 OptiScaler 自带的 default/config 文件。
        $configTemplate = if (Test-Path -LiteralPath $optiFallbackTemplate -PathType Leaf) {
            $optiFallbackTemplate
        } elseif (Test-Path -LiteralPath $componentDefaultIni -PathType Leaf) {
            $componentDefaultIni
        } elseif (Test-Path -LiteralPath $componentIni -PathType Leaf) {
            $componentIni
        } else {
            throw '缺少 OptiScaler 官方默认配置模板。'
        }
        Assert-File -Path $configTemplate
        if ([string]::Equals([IO.Path]::GetFullPath($sourceDirectory), [IO.Path]::GetFullPath($optiDir), [StringComparison]::OrdinalIgnoreCase)) {
            throw (Convert-InstallerText -Value '手动安装源不能选择当前已安装的 payload\OptiScaler 目录；请选择独立的解压目录或压缩包。')
        }
        $stagedConfigTemplate = Join-Path $temporaryDirectory 'OptiScaler.template.ini'
        Copy-Item -LiteralPath $configTemplate -Destination $stagedConfigTemplate -Force
        $preservedUserConfig = Join-Path $temporaryDirectory 'preserved-OptiScaler.ini'
        $preservedFakeNvapiConfig = Join-Path $temporaryDirectory 'preserved-fakenvapi.ini'
        if ($PreserveExistingConfig -and (Test-Path -LiteralPath $optiIni -PathType Leaf)) {
            Copy-Item -LiteralPath $optiIni -Destination $preservedUserConfig -Force
        }
        if ($PreserveExistingConfig -and (Test-Path -LiteralPath $fakeNvapiIni -PathType Leaf)) {
            Copy-Item -LiteralPath $fakeNvapiIni -Destination $preservedFakeNvapiConfig -Force
        }
        Get-OptiScalerUpscalingManifest | Out-Null
        $preservedNvidiaDirectory = Join-Path $temporaryDirectory 'preserved-nvidia'
        foreach ($fileName in @('nvngx_dlss.dll', 'nvngx_dlssg.dll', 'nvngx_dlssd.dll', 'nvngx_dlss.license.txt')) {
            $existingFile = Join-Path $optiDir $fileName
            if (Test-Path -LiteralPath $existingFile -PathType Leaf) {
                New-Item -ItemType Directory -Force -Path $preservedNvidiaDirectory | Out-Null
                Copy-Item -LiteralPath $existingFile -Destination (Join-Path $preservedNvidiaDirectory $fileName) -Force
            }
        }
        if (Test-Path -LiteralPath $optiDir) { Remove-Item -LiteralPath $optiDir -Recurse -Force }
        Copy-CuratedOptiScaler -SourceDirectory $sourceDirectory -Destination $optiDir
        Copy-Item -LiteralPath $stagedConfigTemplate -Destination $optiDefaultIni -Force
        if (Test-Path -LiteralPath $preservedUserConfig -PathType Leaf) {
            Copy-Item -LiteralPath $preservedUserConfig -Destination $optiIni -Force
        }
        else {
            Copy-Item -LiteralPath $stagedConfigTemplate -Destination $optiIni -Force
        }
        if (Test-Path -LiteralPath $preservedFakeNvapiConfig -PathType Leaf) {
            Copy-Item -LiteralPath $preservedFakeNvapiConfig -Destination $fakeNvapiIni -Force
        }
        elseif (Test-Path -LiteralPath $fakeNvapiIni -PathType Leaf) {
            Copy-Item -LiteralPath $fakeNvapiIni -Destination $fakeNvapiDefaultIni -Force
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
        throw (Convert-InstallerText -Value "游戏路径不存在: $Path")
    }
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    if (Test-Path -LiteralPath $resolved -PathType Container) {
        foreach ($name in @('YuanShen.exe', 'GenshinImpact.exe')) {
            $candidate = Join-Path $resolved $name
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                return $candidate
            }
        }
        throw (Convert-InstallerText -Value "目录中没有找到 YuanShen.exe 或 GenshinImpact.exe: $resolved")
    }
    if ([System.IO.Path]::GetFileName($resolved) -notin @('YuanShen.exe', 'GenshinImpact.exe')) {
        throw (Convert-InstallerText -Value "不支持的游戏程序: $resolved")
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
        throw (Convert-InstallerText -Value '未提供游戏路径。')
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

function Test-PathCompatibilityRisk {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) { return $false }
    foreach ($character in $Path.ToCharArray()) {
        if ([int]$character -gt 127) { return $true }
    }
    return ($Path -match '[^A-Za-z0-9 _:\.\\/\-]')
}

function Show-PathCompatibilityWarning {
    param([string]$GameExePath)
    $gameDirectory = if ([string]::IsNullOrWhiteSpace($GameExePath)) { $null } else { Split-Path -Parent $GameExePath }
    $riskyPaths = [System.Collections.Generic.List[string]]::new()
    if (Test-PathCompatibilityRisk -Path $gameDirectory) { $riskyPaths.Add("游戏目录: $gameDirectory") }
    if (Test-PathCompatibilityRisk -Path $root) { $riskyPaths.Add("插件目录: $root") }
    if ($riskyPaths.Count -eq 0) { return }

    Write-Host ''
    Write-Host '路径兼容性提醒：检测到游戏或插件路径包含中文或特殊符号。' -ForegroundColor Yellow
    foreach ($entry in $riskyPaths) {
        Write-Host "  $entry" -ForegroundColor DarkYellow
    }
    Write-Host '若遇到无法注入、插件不加载或日志目录乱码，建议将游戏和插件移动到仅包含英文、数字、下划线和短横线的路径。' -ForegroundColor DarkGray
}

function Add-UniquePath {
    param([System.Collections.Generic.List[string]]$List, [string]$Path)
    foreach ($entry in $List) {
        if ([string]::Equals([System.IO.Path]::GetFullPath($entry), [System.IO.Path]::GetFullPath($Path), [StringComparison]::OrdinalIgnoreCase)) { return }
    }
    $List.Add($Path)
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

function Get-IniValue {
    param(
        [string]$Path,
        [string]$Section,
        [string]$Key
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    $inSection = $false
    foreach ($line in Get-Content -LiteralPath $Path -Encoding UTF8) {
        if ($line -match '^\s*\[(.+)\]\s*$') {
            $inSection = $matches[1] -ieq $Section
            continue
        }
        if ($inSection -and $line -match ('^\s*' + [regex]::Escape($Key) + '\s*=\s*(.*)$')) {
            return $matches[1].Trim()
        }
    }
    return $null
}

function Test-ReShadeIniHasEffectFiles {
    param([string]$IniPath)
    $effectSearchPaths = Get-IniValue -Path $IniPath -Section 'GENERAL' -Key 'EffectSearchPaths'
    if ([string]::IsNullOrWhiteSpace($effectSearchPaths)) { return $false }

    $iniDirectory = Split-Path -Parent ([IO.Path]::GetFullPath($IniPath))
    foreach ($path in $effectSearchPaths -split ';') {
        $candidate = [Environment]::ExpandEnvironmentVariables($path.Trim().Trim('"'))
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        if (-not [IO.Path]::IsPathRooted($candidate)) {
            $candidate = Join-Path $iniDirectory $candidate
        }
        if (-not (Test-Path -LiteralPath $candidate -PathType Container)) { continue }
        if (Get-ChildItem -LiteralPath $candidate -Filter '*.fx' -File -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1) {
            return $true
        }
    }
    return $false
}

function New-ReShadeConfig {
    param(
        [string]$AddonPath,
        [string]$ShaderPath,
        [string]$TexturePath,
        [string]$PresetPath,
        [string]$ScreenshotPath
    )
    @"
[ADDON]
AddonPath=$AddonPath
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

[genshin-ae711f61-renodx-preset1]
blackRangeEndNits=203
blackRangeStartNits=3.5
blackReductionNits=4
colorGradeContrast=50
colorGradeExposure=1
colorGradeHighlights=50
colorGradeSaturation=52
colorGradeShadows=50
toneMapPeakNits=1000
toneMapType=1
toneMapUINits=203

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

function Initialize-ReShadeConfiguration {
    param(
        [string]$IniPath,
        [string]$PresetPath,
        [string]$AddonPath,
        [string]$ShaderPath,
        [string]$TexturePath,
        [string]$ScreenshotPath,
        [switch]$Force,
        [switch]$ForceOverwriteIni,
        [switch]$PreservePreset
    )
    # ReShade.ini lives beside the game executable while effects stay with ReShade64.dll.
    $configAddonPath = [IO.Path]::GetFullPath($AddonPath)
    $configShaderPath = [IO.Path]::GetFullPath($ShaderPath)
    $configTexturePath = [IO.Path]::GetFullPath($TexturePath)
    $configPresetPath = [IO.Path]::GetFullPath($PresetPath)
    $configScreenshotPath = [IO.Path]::GetFullPath($ScreenshotPath)
    $preserveExistingIni = -not $ForceOverwriteIni -and (Test-ReShadeIniHasEffectFiles -IniPath $IniPath)
    if (-not $preserveExistingIni) {
        if (Test-Path -LiteralPath $reshadeIniTemplate -PathType Leaf) {
            Copy-Item -LiteralPath $reshadeIniTemplate -Destination $IniPath -Force
        }
        else {
            $generated = New-ReShadeConfig `
                -AddonPath $configAddonPath `
                -ShaderPath $configShaderPath `
                -TexturePath $configTexturePath `
                -PresetPath $configPresetPath `
                -ScreenshotPath $configScreenshotPath
            Set-Content -LiteralPath $IniPath -Value $generated -Encoding UTF8
        }
    }
    # 路径键每次运行都覆写：组件目录/安装位置可能移动，必须刷新到当前实际路径。
    # （preserveExistingIni 只决定是否从模板重置整份 ini，不再保护路径键。）
    Set-IniValue -Path $IniPath -Section 'ADDON' -Key 'AddonPath' -Value $configAddonPath
    Set-IniValue -Path $IniPath -Section 'GENERAL' -Key 'EffectSearchPaths' -Value $configShaderPath
    Set-IniValue -Path $IniPath -Section 'GENERAL' -Key 'TextureSearchPaths' -Value $configTexturePath
    Set-IniValue -Path $IniPath -Section 'GENERAL' -Key 'PresetPath' -Value $configPresetPath
    Set-IniValue -Path $IniPath -Section 'SCREENSHOT' -Key 'SavePath' -Value $configScreenshotPath
    if (-not $PreservePreset -and ($Force -or -not (Test-Path -LiteralPath $PresetPath -PathType Leaf))) {
        if (Test-Path -LiteralPath $reshadePresetTemplate -PathType Leaf) {
            Copy-Item -LiteralPath $reshadePresetTemplate -Destination $PresetPath -Force
        }
        else {
            Set-Content -LiteralPath $PresetPath -Value "[GENERAL]`nPreprocessorDefinitions=`nTechniqueSorting=" -Encoding UTF8
        }
    }
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
        foreach ($entry in @($existingConfig.DllList)) {
            Add-UniquePath -List $loadedDlls -Path ([string]$entry)
        }
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
    # ReShade must initialize its D3D hooks before Bridge and OptiScaler.
    # Preserve every existing non-ReShade entry, but move the managed ReShade
    # module to the front whenever configurations are reset.
    $orderedDlls = [Collections.Generic.List[string]]::new()
    foreach ($entry in $loadedDlls) {
        try {
            if ([string]::Equals([IO.Path]::GetFullPath($entry), [IO.Path]::GetFullPath($reshadeDll), [StringComparison]::OrdinalIgnoreCase)) {
                Add-UniquePath -List $orderedDlls -Path $entry
            }
        }
        catch {}
    }
    foreach ($entry in $loadedDlls) {
        try {
            if (-not [string]::Equals([IO.Path]::GetFullPath($entry), [IO.Path]::GetFullPath($reshadeDll), [StringComparison]::OrdinalIgnoreCase)) {
                Add-UniquePath -List $orderedDlls -Path $entry
            }
        }
        catch { Add-UniquePath -List $orderedDlls -Path $entry }
    }
    $loadedDlls = $orderedDlls

    if (Test-Path -LiteralPath $optiDll -PathType Leaf) {
        Assert-File -Path $optiDefaultIni
        Copy-Item -LiteralPath $optiDefaultIni -Destination $optiIni -Force
        # 恢复默认 = 与首次安装相同的托管写法（FSR4 策略 + OptiDllPath + Log + 帧生成版型）。
        Set-OptiScalerManagedSettings -Path $optiIni
        if (Test-Path -LiteralPath $fakeNvapiDefaultIni -PathType Leaf) {
            Copy-Item -LiteralPath $fakeNvapiDefaultIni -Destination $fakeNvapiIni -Force
        }
    }
    if (Test-Path -LiteralPath $reshadeDll -PathType Leaf) {
        New-Item -ItemType Directory -Force -Path $screenshotsPath | Out-Null
        Initialize-ReShadeConfiguration `
            -IniPath $reshadeIniPath `
            -PresetPath $reshadePresetPath `
            -AddonPath (Join-Path $shaderDir 'Addons') `
            -ShaderPath (Join-Path $shaderDir 'Shaders') `
            -TexturePath (Join-Path $shaderDir 'Textures') `
            -ScreenshotPath $screenshotsPath `
            -Force `
            -ForceOverwriteIni
        Remove-Item -LiteralPath (Join-Path $reshadeDir 'ReShade.ini'), `
            (Join-Path $reshadeDir 'ReShadePreset.ini') -Force -ErrorAction SilentlyContinue
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
Show-PathCompatibilityWarning -GameExePath $gameExe

if (-not $NonInteractive) {
    Write-Host ''
    Write-Host '请选择需要安装的组件：' -ForegroundColor Yellow
    $DisableOptiScaler = -not (Read-YesNo -Prompt '启用 FSR Bridge + OptiScaler' -Default $true)
    $DisableAntiBlur = -not (Read-YesNo -Prompt '启用反虚化/隐藏 UID' -Default $true)
    $DisableHDR = -not (Read-YesNo -Prompt '启用 ReShade + RenoDX HDR' -Default $true)
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
if ([string]::IsNullOrWhiteSpace($ReShadeSource)) {
    $ReShadeSource = if (Test-Path -LiteralPath $reshadeDll -PathType Leaf) { 'Existing' } else { 'Auto' }
}
$unlockerMode = Select-SourceMode -Label 'FPS Unlocker' -RequestedMode $UnlockerSource -ExistingAvailable (Test-Path -LiteralPath $unlocker -PathType Leaf)
Install-Unlocker -Mode $unlockerMode -ManualPath $UnlockerPackagePath

# Bridge 独立（FSR4——A 卡 7000/9000）：OptiScaler 开启时捆绑强制启用 Bridge
$bridgeEnabled = -not $DisableBridge -or -not $DisableOptiScaler
if ($bridgeEnabled) {
    Assert-File -Path $bridgeDll
    # Bridge 独立依赖：FFX12 SDK（payload\AMD）——A 卡 FSR4 必需
    $amdUpscalerDll = Join-Path $payload 'AMD\amd_fidelityfx_upscaler_dx12.dll'
    if (-not (Test-Path -LiteralPath $amdUpscalerDll -PathType Leaf)) {
        throw (Convert-InstallerText -Value "FSR Bridge 依赖不完整：缺少 $amdUpscalerDll")
    }
}
if (-not $DisableOptiScaler) {
    $optiMode = Select-SourceMode -Label 'OptiScaler' -RequestedMode $OptiScalerSource -ExistingAvailable (Test-Path -LiteralPath $optiDll -PathType Leaf)
    Install-OptiScaler -Mode $optiMode -ManualPath $OptiScalerPackagePath -PreserveExistingConfig:$PreserveExistingConfigs
    try {
        Install-NvidiaDlssIfNeeded
    }
    catch {
        # DLSS 为可选组件（仅 NVIDIA RTX 需要）：失败不影响主流程，说明原因与临时办法后继续。
        Write-Warning (Convert-InstallerText -Value "NVIDIA DLSS 组件安装失败（可选，不影响其他组件）：$($_.Exception.Message)。临时办法：A 卡无需安装 DLSS；N 卡可稍后在「安装模块」或「更新模块」中重试，或从 NVIDIA Streamline 官方发布页手动下载 nvngx_dlss.dll 放入 $optiDir")
    }
    Assert-File -Path $bridgeDll
    $ffxMainCandidates = @(
        (Join-Path $optiDir 'amd_fidelityfx_dx12.dll'),
        (Join-Path $optiDir 'amd_fidelityfx_loader_dx12.dll')
    )
    if (-not ($ffxMainCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf })) {
        throw (Convert-InstallerText -Value "OptiScaler 依赖不完整：请将完整发行包解压到 $optiDir")
    }
    Assert-File -Path (Join-Path $optiDir 'amd_fidelityfx_upscaler_dx12.dll')
}
if (-not $DisableAntiBlur) {
    Assert-File -Path $antiBlurDll
}
if (-not $DisableHDR) {
    Install-ReShadeResources -Mode $ReShadeSource
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
    # 每次运行都覆写 OptiScaler 托管设置（FSR4 策略 + OptiDllPath + Log + 帧生成版型），
    # 保证路径与策略与当前安装一致；其余设置由模板自足。
    Set-OptiScalerManagedSettings -Path $optiIni
}

$dllList = [System.Collections.Generic.List[string]]::new()
if (-not $DisableHDR) {
    $dllList.Add($reshadeDll)
}
if ($bridgeEnabled) {
    $dllList.Add($bridgeDll)
}
if (-not $DisableOptiScaler) {
    $dllList.Add($optiDll)
}
if (-not $DisableAntiBlur) {
    $dllList.Add($antiBlurDll)
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
    Initialize-ReShadeConfiguration `
        -IniPath $reshadeIni `
        -PresetPath $reshadePreset `
        -AddonPath (Join-Path $shaderDir 'Addons') `
        -ShaderPath (Join-Path $shaderDir 'Shaders') `
        -TexturePath (Join-Path $shaderDir 'Textures') `
        -ScreenshotPath $screenshots `
        -Force:$(-not $PreserveExistingConfigs) `
        -PreservePreset
    Remove-Item -LiteralPath (Join-Path $reshadeDir 'ReShade.ini'), `
        (Join-Path $reshadeDir 'ReShadePreset.ini') -Force -ErrorAction SilentlyContinue
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
