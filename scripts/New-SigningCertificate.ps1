# 生成一把**自签名代码签名证书**，导出成 PFX（含私钥）与 CER（公开部分）。
#
# 为什么自签名：
#   公 CA 的代码签名证书自 2023-06 起要求私钥放硬件令牌/云签名，买与用都贵；本项目的产物是
#   自编译自分发，用自签名就够 —— 但必须知道它的边界：**未把证书装进「受信任的根证书颁发机构」
#   的机器上，签名仍显示"未知发布者"**，也拿不到 SmartScreen 信任。它提供的价值是：
#     ① 发布物可校验、可防篡改（配合 signtool/Get-AuthenticodeSignature）；
#     ② 装过一次根证书的用户不再看到"未知发布者"；
#     ③ 可以在自己的安装器/桥里 pin 指纹做自检。
#
# 私钥怎么存：只写进 PFX 文件（默认放 `%LOCALAPPDATA%\FSR\signing\`，仓库外，不会被误提交），
#   **不写入任何证书存储** —— 免得在别人的账户里留下私钥副本。PFX 口令从环境变量
#   `FSR_SIGN_PFX_PASSWORD` 读，或用 -PfxPassword 传；**不要把口令写进仓库或脚本**。
#
# 用法（在仓库根目录）：
#   $env:FSR_SIGN_PFX_PASSWORD = '<你的口令>'
#   pwsh -File scripts\New-SigningCertificate.ps1
#   # 只想换主题名 / 输出位置：
#   pwsh -File scripts\New-SigningCertificate.ps1 -Subject 'CN=...' -OutPfx D:\certs\me.pfx
#
# 生成后用 `pwsh -File scripts\Sign-Binaries.ps1` 签名（该脚本会读同一份 PFX 与口令）。
[CmdletBinding()]
param(
    # 主题 DN（默认与仓库现有发布物一致：CN=出版者名）。CN 就是签名后显示在
    # 属性/安全提示里的"发布者"名称。
    [string]$Subject = 'CN=シリアCelia, O=シリアCelia, OU=Celia, C=CN, S=Zhejiang, L=Zhejiang',
    # PFX 输出路径；留空 = `%LOCALAPPDATA%\FSR\signing\fsr-codesign.pfx`（仓库外，避免误提交）
    [string]$OutPfx = '',
    # CER 输出路径（公开部分，可随发布物给出，供用户装进受信任根）；留空 = 与 PFX 同名 .cer
    [string]$OutCer = '',
    # PFX 口令。**不要**写死在脚本/仓库里；默认从环境变量读。
    [string]$PfxPassword = $env:FSR_SIGN_PFX_PASSWORD,
    [int]$KeySize = 4096,
    [int]$ValidYears = 10,
    # 已存在时覆盖（默认拒绝，避免把正在用的私钥清掉）
    [switch]$Force
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
if ([string]::IsNullOrWhiteSpace($OutPfx))
{
    $OutPfx = Join-Path $env:LOCALAPPDATA 'FSR\signing\fsr-codesign.pfx'
}
if ([string]::IsNullOrWhiteSpace($OutCer))
{
    $OutCer = [IO.Path]::ChangeExtension($OutPfx, '.cer')
}
if ([string]::IsNullOrWhiteSpace($PfxPassword))
{
    throw '缺少 PFX 口令：请设环境变量 FSR_SIGN_PFX_PASSWORD，或用 -PfxPassword 传入（不要写进仓库）。'
}
if ($KeySize -lt 2048)
{
    throw "密钥长度 $KeySize 太短：代码签名建议 >= 3072（默认 4096）。"
}
foreach ($target in @($OutPfx, $OutCer))
{
    if ((Test-Path -LiteralPath $target) -and -not $Force)
    {
        throw "输出已存在：$target。确认要覆盖时加 -Force（覆盖会丢掉旧私钥）。"
    }
}
foreach ($target in @($OutPfx, $OutCer))
{
    $dir = Split-Path -Parent $target
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
}

# ---- 组装证书：自签名 + 代码签名 EKU + DigitalSignature ----
# 为什么显式写 EKU/KeyUsage/BC：`signtool` 与 Windows 只认「EKU 含 1.3.6.1.5.5.7.3.3」的证书；
# 缺了它签出来的东西连"签过"都不算（signtool sign 会直接报错）。BC 必须 CA=false（End Entity），
# 否则会被当成根证书，行为与预期不同。
$rsa = [System.Security.Cryptography.RSA]::Create($KeySize)
try
{
    $dn = [System.Security.Cryptography.X509Certificates.X500DistinguishedName]::new($Subject)
    $request = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new(
        $dn, $rsa,
        [System.Security.Cryptography.HashAlgorithmName]::SHA256,
        [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)

    $ekuOids = [System.Security.Cryptography.OidCollection]::new()
    $null = $ekuOids.Add([System.Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.3', 'Code Signing'))
    $request.CertificateExtensions.Add(
        [System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($ekuOids, $false))
    $request.CertificateExtensions.Add(
        [System.Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new(
            [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature, $true))
    $request.CertificateExtensions.Add(
        [System.Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($false, $false, 0, $false))

    $notBefore = [DateTimeOffset]::Now.AddDays(-1)
    $notAfter = [DateTimeOffset]::Now.AddYears($ValidYears)
    $cert = $request.CreateSelfSigned($notBefore, $notAfter)
    try
    {
        if (-not $cert.HasPrivateKey)
        {
            throw '生成的证书没有私钥：后续无法签名（不应发生，属环境异常）。'
        }
        $pfxBytes = $cert.Export(
            [System.Security.Cryptography.X509Certificates.X509ContentType]::Pfx, $PfxPassword)
        [IO.File]::WriteAllBytes($OutPfx, $pfxBytes)
        [IO.File]::WriteAllBytes(
            $OutCer,
            $cert.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert))

        Write-Host '自签名代码签名证书已生成：' -ForegroundColor Green
        Write-Host "  Subject     : $($cert.Subject)"
        Write-Host "  Issuer      : $($cert.Issuer)   （与 Subject 相同 = 自签名）"
        Write-Host "  Thumbprint  : $($cert.Thumbprint)"
        Write-Host "  有效期      : $($cert.NotBefore.ToString('u')) → $($cert.NotAfter.ToString('u'))"
        Write-Host "  密钥        : RSA $KeySize"
        Write-Host "  PFX（含私钥）: $OutPfx"
        Write-Host "  CER（公开）  : $OutCer"
        Write-Host ''
        Write-Host '下一步：' -ForegroundColor Cyan
        Write-Host '  1) 签名：pwsh -File scripts\Sign-Binaries.ps1            （读默认 PFX + FSR_SIGN_PFX_PASSWORD）'
        Write-Host '  2) 让别人的机器认这个发布者：把 CER 装进「受信任的根证书颁发机构」与「受信任的发布者」'
        Write-Host '     导入命令（管理员）：certutil -addstore Root "<cer 路径>"'
        Write-Host '                        certutil -addstore TrustedPublisher "<cer 路径>"'
        Write-Host '  ⚠️ PFX 是私钥：不要提交、不要放进发布包、不要发给别人。'
    }
    finally
    {
        $cert.Dispose()
    }
}
finally
{
    $rsa.Dispose()
}
