#Requires -Version 5.1
<#
FSR4 平台策略回归：RDNA2 独显 / RDNA3.5 核显不再封锁，且不会误伤其他档位。

做法：从真 Configure.ps1 的 AST 里取**真赋值块与真函数**（$script:Amd*DevIds、
Get-Fsr4GpuPolicy、Test-AmdRdna2Gpu），拼成一个临时脚本，仅把 `Get-VideoControllersOnce`
换成测试用的假显卡列表 ⇒ 断言策略输出的 Mode / Reason。

跑不到的边界：这里喂的是假 Device ID / 假型号名，不读注册表，也不证明真机上另有
厂商字符串；真机验证仍需实卡。

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

# ---------------------------------------------------------------- 从真脚本里取片段
$tokens = $null
$parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($configurePath, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -gt 0) {
    Write-Host 'Configure.ps1 解析失败' -ForegroundColor Red
    exit 2
}

$wantedVariables = @(
    'AmdRdna2DevIds', 'AmdRdna2DgpuDevIds', 'AmdRdna35IgpuDevIds',
    'AmdFp8DevIds', 'AmdInt8DevIds'
)
$assignments = $ast.FindAll({
    param($node)
    $node -is [System.Management.Automation.Language.AssignmentStatementAst] -and
    $node.Left -is [System.Management.Automation.Language.VariableExpressionAst] -and
    $wantedVariables -contains ($node.Left.VariablePath.UserPath -replace '^script:', '')
}, $true)

$wantedFunctions = @('Get-Fsr4GpuPolicy', 'Test-AmdRdna2Gpu')
$functions = $ast.FindAll({
    param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
    $wantedFunctions -contains $node.Name
}, $true)

Chk ($assignments.Count -ge $wantedVariables.Count - 0) ("抓到 Device ID 赋值块 {0} 个" -f $assignments.Count)
Chk ($functions.Count -eq $wantedFunctions.Count) ("抓到真函数 {0} 个（Get-Fsr4GpuPolicy / Test-AmdRdna2Gpu）" -f $functions.Count)
if ($assignments.Count -lt $wantedVariables.Count -or $functions.Count -ne $wantedFunctions.Count) {
    Write-Host '片段抽取不完整，后续断言无意义' -ForegroundColor Red
    exit 1
}

$script:FakeControllers = @()
$stub = 'function Get-VideoControllersOnce { return $script:FakeControllers }'
$tempPath = Join-Path $env:TEMP ("fsr4policy_" + [guid]::NewGuid().ToString('N') + '.ps1')
$body = @(
    $stub
    ($assignments | ForEach-Object { $_.Extent.Text })
    ($functions | ForEach-Object { $_.Extent.Text })
) -join "`r`n"
Set-Content -LiteralPath $tempPath -Value $body -Encoding UTF8

function Set-FakeGpu {
    param([string]$Vendor, [string]$Device, [string]$Name)
    $pnp = "PCI\VEN_{0}&DEV_{1}&SUBSYS_00000000&REV_C1" -f $Vendor, $Device
    $script:FakeControllers = @([pscustomobject]@{ Name = $Name; PNPDeviceID = $pnp })
}

function Get-Policy {
    . $tempPath
    return (Get-Fsr4GpuPolicy)
}

try {
    Write-Host '=== 架构档位：RDNA2 独显 / RDNA3.5 核显 必须 int8，RDNA4 必须 fp8 ==='
    $cases = @(
        @{ V = '1002'; D = '7550'; N = 'AMD Radeon RX 9070 XT'; Mode = 'fp8'; Reason = 'AMD RDNA4' },
        @{ V = '1002'; D = '73BF'; N = 'AMD Radeon RX 6800 XT'; Mode = 'int8'; Reason = 'AMD RDNA2' },
        @{ V = '1002'; D = '73F0'; N = 'AMD Radeon RX 6600'; Mode = 'int8'; Reason = 'AMD RDNA2' },
        @{ V = '1002'; D = '743F'; N = 'AMD Radeon RX 6500 XT'; Mode = 'int8'; Reason = 'AMD RDNA2' },
        @{ V = '1002'; D = '744C'; N = 'AMD Radeon RX 7900 XTX'; Mode = 'int8'; Reason = 'AMD RDNA3/3.5' },
        @{ V = '1002'; D = '150E'; N = 'AMD Radeon(TM) 890M Graphics'; Mode = 'int8'; Reason = 'AMD RDNA3.5 核显' },
        @{ V = '1002'; D = '1586'; N = 'AMD Radeon(TM) Graphics'; Mode = 'int8'; Reason = 'AMD RDNA3.5 核显' },
        @{ V = '1002'; D = '1681'; N = 'AMD Radeon(TM) 680M'; Mode = 'auto'; Reason = 'no whitelist match' },
        @{ V = '1002'; D = '1234'; N = 'AMD Radeon(TM) Graphics'; Mode = 'auto'; Reason = 'no whitelist match' }
    )
    foreach ($case in $cases) {
        Set-FakeGpu -Vendor $case.V -Device $case.D -Name $case.N
        $policy = Get-Policy
        Chk ($policy.Mode -eq $case.Mode -and $policy.Reason -eq $case.Reason) (`
            "DEV_{0} → {1} / {2}（实得 {3} / {4}）" -f $case.D, $case.Mode, $case.Reason, $policy.Mode, $policy.Reason)
    }

    Write-Host '=== 名称兜底：Device ID 没收录时 RX 6000 系也要放行 ==='
    Set-FakeGpu -Vendor '1002' -Device '0000' -Name 'AMD Radeon RX 6800 XT'
    $policy = Get-Policy
    Chk ($policy.Mode -eq 'int8' -and $policy.Reason -eq 'AMD RDNA2') ("RX 6800 XT（无 ID）→ int8 / AMD RDNA2（实得 {0} / {1}）" -f $policy.Mode, $policy.Reason)

    Set-FakeGpu -Vendor '1002' -Device '0000' -Name 'AMD Radeon(TM) 610M'
    $policy = Get-Policy
    Chk ($policy.Mode -eq 'auto') ("RDNA2 核显型号名（610M）仍不启用 FSR4（实得 {0}）" -f $policy.Mode)

    Write-Host '=== 多卡择优：RDNA4 与 RDNA3 同时存在时必须取 fp8 ==='
    $script:FakeControllers = @(
        [pscustomobject]@{ Name = 'AMD Radeon RX 7900 XTX'; PNPDeviceID = 'PCI\VEN_1002&DEV_744C&SUBSYS_00000000&REV_C1' },
        [pscustomobject]@{ Name = 'AMD Radeon RX 9070 XT'; PNPDeviceID = 'PCI\VEN_1002&DEV_7550&SUBSYS_00000000&REV_C1' }
    )
    $policy = Get-Policy
    Chk ($policy.Mode -eq 'fp8') ("fp8 优先于 int8（实得 {0}）" -f $policy.Mode)

    Write-Host '=== Ffx12AsyncUpscale：RDNA2 独显仍要同步 ==='
    Set-FakeGpu -Vendor '1002' -Device '73BF' -Name 'AMD Radeon RX 6800 XT'
    . $tempPath
    Chk (Test-AmdRdna2Gpu) 'RDNA2 独显（73BF）→ 同步交叠'
    Set-FakeGpu -Vendor '1002' -Device '744C' -Name 'AMD Radeon RX 7900 XTX'
    . $tempPath
    Chk (-not (Test-AmdRdna2Gpu)) 'RDNA3 独显（744C）→ 异步交叠（不受影响）'
}
finally {
    if (Test-Path -LiteralPath $tempPath) { Remove-Item -LiteralPath $tempPath -Force -ErrorAction SilentlyContinue }
}

Write-Host ("合计: {0} 通过, {1} 失败" -f $script:Pass, $script:Fail)
if ($script:Fail -gt 0) { exit 1 }
Write-Host 'ALL PASS' -ForegroundColor Green
exit 0
