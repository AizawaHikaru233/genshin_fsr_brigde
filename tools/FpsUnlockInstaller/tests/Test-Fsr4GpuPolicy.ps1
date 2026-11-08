#Requires -Version 5.1
<#
FSR4 策略回归（2026-11-08 起口径变了）：
  1) 安装脚本**不再按显卡型号判定 FSR4 支持** ⇒ Set-Fsr4GpuPolicy 三键必须写 auto；
  2) 仍然按显卡型号决定的是「桥的异步交叠」⇒ Test-AmdRdna2Gpu 保持 RDNA2 判定。

做法：从真 Configure.ps1 的 AST 里取真函数 / 真赋值块，只把外部依赖换成桩：
  - Get-VideoControllersOnce → 假显卡列表
  - Set-IniValue            → 记录到哈希表（不碰真 ini）

用法：powershell -NoProfile -File tools\FpsUnlockInstaller\tests\Test-Fsr4GpuPolicy.ps1
#>
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$configurePath = Join-Path (Split-Path -Parent $PSScriptRoot) 'Configure.ps1'
if (-not (Test-Path -LiteralPath $configurePath -PathType Leaf)) {
    Write-Host ("找不到被测脚本: {0}" -f $configurePath) -ForegroundColor Red
    exit 2
}

$script:Pass = 0
$script:Fail = 0
function Chk {
    param([bool]$Condition, [string]$Message)
    if ($Condition) { Write-Host ("  [PASS] {0}" -f $Message) -ForegroundColor Green; $script:Pass++ }
    else { Write-Host ("  [FAIL] {0}" -f $Message) -ForegroundColor Red; $script:Fail++ }
}

$tokens = $null
$parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($configurePath, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -gt 0) { Write-Host 'Configure.ps1 解析失败' -ForegroundColor Red; exit 2 }

$assignments = $ast.FindAll({
    param($node)
    $node -is [System.Management.Automation.Language.AssignmentStatementAst] -and
    $node.Left -is [System.Management.Automation.Language.VariableExpressionAst] -and
    ($node.Left.VariablePath.UserPath -replace '^script:', '') -eq 'AmdRdna2DevIds'
}, $true)
$wanted = @('Test-AmdRdna2Gpu', 'Set-Fsr4GpuPolicy')
$functions = $ast.FindAll({
    param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $wanted -contains $node.Name
}, $true)

Chk ($assignments.Count -eq 1) ("抓到 RDNA2 ID 表 {0} 个" -f $assignments.Count)
Chk ($functions.Count -eq 2) ("抓到真函数 {0} 个（Test-AmdRdna2Gpu / Set-Fsr4GpuPolicy）" -f $functions.Count)
if ($assignments.Count -ne 1 -or $functions.Count -ne 2) { exit 1 }

$script:FakeControllers = @()
$script:Recorded = [ordered]@{}
$preamble = @(
    'function Get-VideoControllersOnce { return $script:FakeControllers }'
    'function Set-IniValue { param([string]$Path, [string]$Section, [string]$Key, [string]$Value) $script:Recorded["$Section.$Key"] = $Value }'
    '$nonFrameGenerationEdition = $true'
)
$body = (@($preamble) + @($assignments | ForEach-Object { $_.Extent.Text }) + @($functions | ForEach-Object { $_.Extent.Text })) -join "`r`n"
$tempPath = Join-Path $env:TEMP ("fsr4policy_" + [guid]::NewGuid().ToString('N') + '.ps1')
Set-Content -LiteralPath $tempPath -Value $body -Encoding UTF8

function Set-FakeGpu {
    param([string]$Vendor, [string]$Device, [string]$Name)
    $pnp = "PCI\VEN_{0}&DEV_{1}&SUBSYS_00000000&REV_C1" -f $Vendor, $Device
    $script:FakeControllers = @([pscustomobject]@{ Name = $Name; PNPDeviceID = $pnp })
}

try {
    . $tempPath

    Write-Host '=== 策略必须全部 auto（不再按显卡型号判定） ==='
    foreach ($gpu in @(
        @{ V = '1002'; D = '73BF'; N = 'AMD Radeon RX 6800 XT' },
        @{ V = '1002'; D = '7550'; N = 'AMD Radeon RX 9070 XT' },
        @{ V = '1002'; D = '150E'; N = 'AMD Radeon(TM) 890M' },
        @{ V = '10DE'; D = '2704'; N = 'NVIDIA GeForce RTX 4070' },
        @{ V = '8086'; D = '56A0'; N = 'Intel(R) Arc(TM) A770' }
    )) {
        Set-FakeGpu -Vendor $gpu.V -Device $gpu.D -Name $gpu.N
        $script:Recorded = [ordered]@{}
        $policy = Set-Fsr4GpuPolicy -Path 'C:\nonexistent\OptiScaler.ini'
        Chk ($policy.Mode -eq 'auto') ("{0}：返回 Mode=auto（实得 {1}）" -f $gpu.N, $policy.Mode)
        Chk ($script:Recorded['FSR.Fsr4Update'] -eq 'auto') ("{0}：Fsr4Update=auto（实得 {1}）" -f $gpu.N, $script:Recorded['FSR.Fsr4Update'])
        Chk ($script:Recorded['FSR.UpscalerIndex'] -eq 'auto') ("{0}：UpscalerIndex=auto（实得 {1}）" -f $gpu.N, $script:Recorded['FSR.UpscalerIndex'])
        Chk ($script:Recorded['FSR.Fsr4ForceEnableInt8'] -eq 'auto') ("{0}：Fsr4ForceEnableInt8=auto（实得 {1}）" -f $gpu.N, $script:Recorded['FSR.Fsr4ForceEnableInt8'])
    }

    Write-Host '=== 仍然按型号判定的是「桥的同步交叠」 ==='
    Set-FakeGpu -Vendor '1002' -Device '73BF' -Name 'AMD Radeon RX 6800 XT'
    . $tempPath
    Chk (Test-AmdRdna2Gpu) 'RDNA2 独显（73BF）→ 同步'
    Set-FakeGpu -Vendor '1002' -Device '744C' -Name 'AMD Radeon RX 7900 XTX'
    . $tempPath
    Chk (-not (Test-AmdRdna2Gpu)) 'RDNA3 独显（744C）→ 异步'
}
finally {
    if (Test-Path -LiteralPath $tempPath) { Remove-Item -LiteralPath $tempPath -Force -ErrorAction SilentlyContinue }
}

Write-Host ("合计: {0} 通过, {1} 失败" -f $script:Pass, $script:Fail)
if ($script:Fail -gt 0) { exit 1 }
Write-Host 'ALL PASS' -ForegroundColor Green
exit 0
