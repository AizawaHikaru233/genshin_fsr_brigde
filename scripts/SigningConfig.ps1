# 解析"用哪把证书签名" —— `Build-OnlineInstaller.ps1` 与 `Sign-Binaries.ps1` 共用这一份实现。
#
# 为什么单独一份：两处各自解析必然漂移（打包认为"已配置"、签名脚本认为"没有证书"这类故障
# 排查代价极高）。解析顺序固定如下，先命中先用：
#   1) 显式参数 -Thumbprint（证书+私钥在证书存储里，例如硬件令牌）
#   2) 显式参数 -Pfx
#   3) 环境变量 FSR_SIGN_THUMBPRINT / FSR_SIGN_PFX
#   4) `%LOCALAPPDATA%\FSR\signing\pfx-path.txt` 里的一行路径（换仓库、换盘符只需改这一行）
#   5) 默认 `%LOCALAPPDATA%\FSR\signing\fsr-codesign.pfx`
#
# 私钥位置**不写进仓库**：仓库只放解析规则，具体路径属于本机配置。

function Resolve-SigningCertificate {
    param(
        [string]$Pfx,
        [string]$Thumbprint,
        # 允许"没有证书"（返回 Configured=$false 而不是抛错），供打包脚本决定是否告警
        [switch]$AllowMissing
    )
    $baseDir = Join-Path $env:LOCALAPPDATA 'FSR\signing'
    $source = 'none'

    if (-not [string]::IsNullOrWhiteSpace($Thumbprint))
    {
        $source = '参数 -Thumbprint'
    }
    elseif (-not [string]::IsNullOrWhiteSpace($Pfx))
    {
        $source = '参数 -Pfx'
    }
    elseif (-not [string]::IsNullOrWhiteSpace($env:FSR_SIGN_THUMBPRINT))
    {
        $Thumbprint = $env:FSR_SIGN_THUMBPRINT
        $source = '环境变量 FSR_SIGN_THUMBPRINT'
    }
    elseif (-not [string]::IsNullOrWhiteSpace($env:FSR_SIGN_PFX))
    {
        $Pfx = $env:FSR_SIGN_PFX
        $source = '环境变量 FSR_SIGN_PFX'
    }
    else
    {
        $pathFile = Join-Path $baseDir 'pfx-path.txt'
        if (Test-Path -LiteralPath $pathFile -PathType Leaf)
        {
            $line = @(Get-Content -LiteralPath $pathFile -Encoding UTF8 |
                Where-Object { -not [string]::IsNullOrWhiteSpace($_) }) | Select-Object -First 1
            if ($null -ne $line)
            {
                $Pfx = ([string]$line).Trim().Trim('"')
                if (-not [string]::IsNullOrWhiteSpace($Pfx)) { $source = 'pfx-path.txt' }
            }
        }
        if ([string]::IsNullOrWhiteSpace($Pfx))
        {
            $defaultPfx = Join-Path $baseDir 'fsr-codesign.pfx'
            if (Test-Path -LiteralPath $defaultPfx -PathType Leaf)
            {
                $Pfx = $defaultPfx
                $source = '默认 PFX 路径'
            }
        }
    }

    $thumbprintOut = $null
    if (-not [string]::IsNullOrWhiteSpace($Thumbprint))
    {
        $thumbprintOut = $Thumbprint.Replace(' ', '').ToUpperInvariant()
    }

    $pfxOut = $null
    if ([string]::IsNullOrWhiteSpace($thumbprintOut) -and -not [string]::IsNullOrWhiteSpace($Pfx))
    {
        if (-not (Test-Path -LiteralPath $Pfx -PathType Leaf))
        {
            if (-not $AllowMissing) { throw "PFX 不存在：$Pfx（来源：$source）" }
            $source = "$source；文件不存在"
            $Pfx = $null
        }
        else
        {
            $pfxOut = [IO.Path]::GetFullPath($Pfx)
        }
    }

    $configured = ($null -ne $thumbprintOut) -or ($null -ne $pfxOut)
    if (-not $configured -and -not $AllowMissing)
    {
        throw ('没有可用签名证书：用 -Pfx <pfx> / -Thumbprint <sha1>，或设环境变量 FSR_SIGN_PFX / ' +
            'FSR_SIGN_THUMBPRINT，或在 `%LOCALAPPDATA%\FSR\signing\pfx-path.txt` 写一行 PFX 路径，' +
            '或先跑 scripts\New-SigningCertificate.ps1 生成默认 PFX。')
    }

    return [pscustomobject]@{
        Pfx        = $pfxOut
        Thumbprint = $thumbprintOut
        Configured = $configured
        Source     = $source
    }
}
