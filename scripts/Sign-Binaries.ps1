# 用**自签名代码签名证书**给本项目自己编译的二进制签名（Authenticode），签完逐个校验。
#
# 为什么要这一步：发布物不签名时，Windows 只显示"未知发布者"；签名后至少能证明
#   「这个文件出自我们这把证书」，配合证书分发还能消掉提示（见文件末尾的命令）。
#   ⚠️ 自签名**不等于**可信：未安装根证书的机器上仍显示"未知发布者"，也拿不到 SmartScreen 信任。
#
# 证书从哪来（优先级从上到下）：
#   1) -Pfx <路径>（配合 -PfxPassword 或环境变量 FSR_SIGN_PFX_PASSWORD）
#   2) -Thumbprint <SHA1>（证书+私钥装在证书存储里，例如硬件令牌）
#   3) 环境变量 FSR_SIGN_PFX / FSR_SIGN_THUMBPRINT
#   4) 默认文件 %LOCALAPPDATA%\FSR\signing\fsr-codesign.pfx（由 scripts\New-SigningCertificate.ps1 生成）
# 口令永远从环境变量/参数取，**不要写进仓库**。
#
# 用法（仓库根目录）：
#   $env:FSR_SIGN_PFX_PASSWORD = '<口令>'
#   pwsh -File scripts\Sign-Binaries.ps1                       # 签 4 个自有 DLL + GUI 安装器
#   pwsh -File scripts\Sign-Binaries.ps1 -Paths build-package-bridge\Dx11FsrBridge.dll
#   pwsh -File scripts\Sign-Binaries.ps1 -VerifyOnly           # 只校验现有签名
#   pwsh -File scripts\Sign-Binaries.ps1 -Thumbprint <sha1>    # 用证书存储里的证书
#
# 时间戳：默认打 DigiCert 的 RFC3161（证书过期后旧版本签名仍有效）。离线环境加 -NoTimestamp，
#   但要知道后果：不打时间戳的签名在证书过期后**全部失效**。
[CmdletBinding()]
param(
    # 要签的文件；留空 = 本项目的 4 个自有组件产物 + GUI 安装器（存在哪个签哪个）
    [string[]]$Paths,
    [string]$Pfx = $env:FSR_SIGN_PFX,
    [string]$PfxPassword = $env:FSR_SIGN_PFX_PASSWORD,
    [string]$Thumbprint = $env:FSR_SIGN_THUMBPRINT,
    [string]$TimestampUrl = 'http://timestamp.digicert.com',
    # 备用 RFC3161 服务器：单个 TSA 偶发抽风不该让整轮发版失败（2026-11-28 实测 DigiCert
    # 一次 "could not be reached or returned an invalid response" 直接中止了打包）
    [string[]]$FallbackTimestampUrls = @('http://timestamp.sectigo.com', 'http://time.certum.pl'),
    [switch]$NoTimestamp,
    # 只校验、不签名（用于确认发布物确实带签名）
    [switch]$VerifyOnly
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))

# ---- 默认目标：只签**我们自己编译**的东西（上游组件一律不签：签了等于冒充来源）----
if (-not $Paths -or @($Paths).Count -eq 0)
{
    $Paths = @(
        (Join-Path $root 'build-package-bridge\Dx11FsrBridge.dll'),
        (Join-Path $root 'build-package-fufu-graphics\FSR-Bridge-Plugin.dll'),
        (Join-Path $root 'build-package-antiplayermosaic\AntiPlayerMosaic.dll'),
        (Join-Path $root 'build-tloader-gdds\TextureLoader.dll'),
        (Join-Path $root 'Installer\app\GenshinFSRBridgeInstaller.exe')
    )
}
$targets = @()
foreach ($candidate in $Paths)
{
    if (Test-Path -LiteralPath $candidate -PathType Leaf) { $targets += [IO.Path]::GetFullPath($candidate) }
    else { Write-Host "跳过（不存在）：$candidate" -ForegroundColor DarkYellow }
}
if ($targets.Count -eq 0) { throw '没有可签的文件：请先编译，或用 -Paths 指定。' }

# ---- 定位 signtool：PATH 优先，其次 Windows SDK 里最新的 x64 ----
function Get-SignToolPath
{
    $cmd = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($null -ne $cmd) { return $cmd.Path }
    $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    if (Test-Path -LiteralPath $kits)
    {
        $found = Get-ChildItem -LiteralPath $kits -Directory |
            Sort-Object Name -Descending |
            ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' } |
            Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
            Select-Object -First 1
        if ($null -ne $found) { return $found }
    }
    throw '找不到 signtool.exe：装 Windows SDK（Signing Tools），或把 signtool 加进 PATH。'
}

# ---- 解析证书来源 ----
# 默认 PFX 只在"没指定证书"时兜底。`-VerifyOnly` 允许完全没有证书（此时只校验签名存在 /
# 哈希一致 / 有时间戳，不比对签名者）—— 这样别人拿到发布包也能就地验签名，不必持有私钥。
if ([string]::IsNullOrWhiteSpace($Pfx) -and [string]::IsNullOrWhiteSpace($Thumbprint))
{
    $defaultPfx = Join-Path $env:LOCALAPPDATA 'FSR\signing\fsr-codesign.pfx'
    if (Test-Path -LiteralPath $defaultPfx -PathType Leaf) { $Pfx = $defaultPfx }
}
if ([string]::IsNullOrWhiteSpace($Pfx) -and [string]::IsNullOrWhiteSpace($Thumbprint))
{
    if (-not $VerifyOnly)
    {
        throw ('没有可用证书：请用 -Pfx <pfx> 或 -Thumbprint <sha1>，或先跑 ' +
            'scripts\New-SigningCertificate.ps1 生成默认 PFX，或设 FSR_SIGN_PFX / FSR_SIGN_THUMBPRINT。')
    }
    Write-Host '未指定证书：只校验"签名存在 / 哈希一致 / 有时间戳"，不比对签名者指纹。' -ForegroundColor Yellow
}
if (-not [string]::IsNullOrWhiteSpace($Pfx))
{
    if (-not (Test-Path -LiteralPath $Pfx -PathType Leaf)) { throw "PFX 不存在：$Pfx" }
    if ([string]::IsNullOrWhiteSpace($PfxPassword) -and -not $VerifyOnly)
    {
        throw 'PFX 需要口令：设环境变量 FSR_SIGN_PFX_PASSWORD，或用 -PfxPassword 传入。'
    }
}

# 期望的签名者指纹（校验用）：指纹模式直接用传入值；PFX 模式从 PFX 里读（需要口令）。
# 拿不到就是 $null ⇒ 校验时跳过指纹比对（只有 -VerifyOnly 允许这种情况）。
$expectedThumbprint = $null
if (-not [string]::IsNullOrWhiteSpace($Thumbprint))
{
    $expectedThumbprint = $Thumbprint.Replace(' ', '').ToUpperInvariant()
}
elseif (-not [string]::IsNullOrWhiteSpace($Pfx) -and -not [string]::IsNullOrWhiteSpace($PfxPassword))
{
    $pfxProbe = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new(
        $Pfx, $PfxPassword,
        [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet)
    try { $expectedThumbprint = $pfxProbe.Thumbprint } finally { $pfxProbe.Dispose() }
}

$signTool = Get-SignToolPath
if ($VerifyOnly) { $signMode = '只校验（-VerifyOnly，不签名）' }
elseif (-not [string]::IsNullOrWhiteSpace($Thumbprint)) { $signMode = "thumbprint $expectedThumbprint" }
else { $signMode = "pfx $(Split-Path -Leaf $Pfx)" }
Write-Host "签名工具：$signTool" -ForegroundColor Cyan
Write-Host "证书来源：$signMode" -ForegroundColor Cyan
Write-Host "时间戳  ：$(if ($NoTimestamp) { '不打（-NoTimestamp）' } else { $TimestampUrl })" -ForegroundColor Cyan
Write-Host ''

# ---- 签名 ----
if (-not $VerifyOnly)
{
    foreach ($file in $targets)
    {
        # 参数逐个试：主 TSA 失败就换备用（只换 `/tr`，证书与文件路径不变）。
        # 2026-11-28 实测：DigiCert 偶发 "could not be reached" 曾直接中止整轮打包，
        # 而同一时刻另外三个 TSA 都可用 ⇒ 单点故障不该阻断发版。
        $urls = @()
        if (-not $NoTimestamp) { $urls = @($TimestampUrl) + @($FallbackTimestampUrls) }
        else { $urls = @($null) }
        $lastOut = @()
        $signed = $false
        foreach ($url in $urls)
        {
            $tryArgs = @('sign', '/fd', 'sha256')
            if (-not $NoTimestamp) { $tryArgs += @('/td', 'sha256', '/tr', $url) }
            if (-not [string]::IsNullOrWhiteSpace($Thumbprint))
            {
                $tryArgs += @('/sha1', $Thumbprint.Replace(' ', ''))
            }
            else
            {
                $tryArgs += @('/f', $Pfx, '/p', $PfxPassword)
            }
            $tryArgs += $file
            $lastOut = & $signTool @tryArgs 2>&1
            if ($LASTEXITCODE -eq 0) { $signed = $true; break }
            if ($urls.Count -gt 1) { Write-Host "  时间戳服务器不可用，换下一个：$url" -ForegroundColor DarkYellow }
        }
        if (-not $signed)
        {
            throw ("signtool sign 失败（exit=$LASTEXITCODE）：$(Split-Path -Leaf $file)`n" + ($lastOut -join "`n"))
        }
    }
    Write-Host ''
}

# ---- 校验 ----
# ⚠️ 这里**不用** `signtool verify /pa`：它按 Authenticode 策略要求整条链到受信任根，
#   自签名证书在没装根证书的本机必定失败（"chain terminated in untrusted root"）。
#   真正要断言的是三件事：① 签名存在；② 文件内容没被改（哈希一致）；③ 签名者是我们这把证书。
$rows = @()
$failed = @()
foreach ($file in $targets)
{
    $sig = Get-AuthenticodeSignature -LiteralPath $file
    $name = Split-Path -Leaf $file
    $ok = $true
    $why = ''

    if ($null -eq $sig -or $sig.Status -eq 'NotSigned')
    {
        $ok = $false; $why = '没有签名'
    }
    elseif ($sig.Status -eq 'HashMismatch')
    {
        $ok = $false; $why = '哈希不匹配（签名后文件被改过）'
    }
    elseif ($null -eq $sig.SignerCertificate)
    {
        $ok = $false; $why = '无签名者证书'
    }
    elseif ($null -ne $expectedThumbprint -and $sig.SignerCertificate.Thumbprint -ne $expectedThumbprint)
    {
        $ok = $false; $why = "签名者指纹 $($sig.SignerCertificate.Thumbprint) ≠ 期望 $expectedThumbprint"
    }
    elseif (-not $NoTimestamp -and $null -eq $sig.TimeStamperCertificate)
    {
        $ok = $false; $why = '没有时间戳'
    }

    $rows += [pscustomobject]@{
        File        = $name
        Status      = [string]$sig.Status
        Signer      = if ($null -ne $sig.SignerCertificate) { $sig.SignerCertificate.GetNameInfo('SimpleName', $false) } else { '' }
        Thumbprint  = if ($null -ne $sig.SignerCertificate) { $sig.SignerCertificate.Thumbprint.Substring(0, 8) } else { '' }
        Timestamped = ($null -ne $sig.TimeStamperCertificate)
        Ok          = $ok
        Why         = $why
    }
    if (-not $ok) { $failed += "$name：$why" }
}

$rows | Format-Table File, Status, Signer, Thumbprint, Timestamped, Ok -AutoSize | Out-String | Write-Host

if ($failed.Count -gt 0)
{
    throw ("签名校验失败：`n  " + ($failed -join "`n  "))
}

if ($null -ne $expectedThumbprint) {
    Write-Host '全部文件签名校验通过：签名存在、哈希一致、签名者是期望证书。' -ForegroundColor Green
}
else {
    Write-Host '全部文件签名校验通过：签名存在、哈希一致、有时间戳（未比对签名者指纹）。' -ForegroundColor Green
}
Write-Host ('提示：自签名证书未装入「受信任的根证书颁发机构」的机器上，这些文件仍显示"未知发布者"；' +
    '要让提示消失，把生成的 .cer 导入 Root 与 TrustedPublisher（管理员）：') -ForegroundColor DarkYellow
Write-Host '  certutil -addstore Root "<xxx.cer>"' -ForegroundColor DarkYellow
Write-Host '  certutil -addstore TrustedPublisher "<xxx.cer>"' -ForegroundColor DarkYellow
