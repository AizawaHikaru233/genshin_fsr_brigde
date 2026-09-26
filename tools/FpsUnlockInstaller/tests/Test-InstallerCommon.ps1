#Requires -Version 5.1
<#
共享模块 InstallerCommon.ps1 的单元测试。

重点覆盖 2026-09-26 的安装失效 bug：
  Set-JsonPropertyValue 用 `-ne` 判断"值是否变化"，而右侧是数组时 PowerShell
  把 `-ne` 当【过滤器】而不是比较 —— `$null -ne @('a','b')` 得到空数组
  （布尔化为 $false）⇒ 被判为"相等"⇒ 跳过写入。
  `DllList` 正是数组值（空数组经 ConvertFrom-Json 后是 $null），
  于是所有写 DllList 的调用全部静默失效，配置里始终是空表 ——
  表现为"装完一个插件都没有，反复安装也无效"。

用法：powershell -NoProfile -File tools\FpsUnlockInstaller\tests\Test-InstallerCommon.ps1
#>
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptDirectory = [IO.Path]::GetFullPath((Split-Path -Parent $PSCommandPath))
$modulePath = Join-Path (Split-Path -Parent $scriptDirectory) 'InstallerCommon.ps1'
if (-not (Test-Path -LiteralPath $modulePath -PathType Leaf)) {
    Write-Host "找不到被测模块: $modulePath" -ForegroundColor Red
    exit 2
}
. $modulePath

$script:Pass = 0
$script:Fail = 0

function Test-Case {
    param([string]$Name, [scriptblock]$Body)
    try {
        $ok = & $Body
        if ($ok) {
            Write-Host ("  [PASS] {0}" -f $Name) -ForegroundColor Green
            $script:Pass++
        }
        else {
            Write-Host ("  [FAIL] {0}" -f $Name) -ForegroundColor Red
            $script:Fail++
        }
    }
    catch {
        Write-Host ("  [FAIL] {0}  —  {1}" -f $Name, $_.Exception.Message) -ForegroundColor Red
        $script:Fail++
    }
}

function New-JsonObject {
    param([string]$Json)
    return ($Json | ConvertFrom-Json)
}

function New-StringList {
    param([string[]]$Items)
    $list = [Collections.Generic.List[string]]::new()
    foreach ($item in $Items) { $list.Add($item) }
    return $list
}

Write-Host ''
Write-Host '=== Set-JsonPropertyValue ===' -ForegroundColor Cyan

# 核心回归：空数组起点写入数组值必须生效。
# 修复前这里是 $false 且 DllList 仍为空 —— 该用例会失败，正是它的价值。
Test-Case '已存在属性（空数组起点）能写入数组' {
    $config = New-JsonObject '{"GamePath":"x","DllList":[]}'
    $changed = Set-JsonPropertyValue -Object $config -Name 'DllList' -Value @(New-StringList @('a.dll', 'b.dll'))
    return ($changed -and @($config.DllList).Count -eq 2 -and $config.DllList[0] -eq 'a.dll')
}

# 非空数组被替换也必须生效（覆盖"内容变了但判断不出来"的情况）。
Test-Case '已存在属性（非空数组）能被替换' {
    $config = New-JsonObject '{"GamePath":"x","DllList":["old.dll"]}'
    $changed = Set-JsonPropertyValue -Object $config -Name 'DllList' -Value @('new1.dll', 'new2.dll')
    return ($changed -and @($config.DllList).Count -eq 2 -and $config.DllList[1] -eq 'new2.dll')
}

# 幂等：值没变时应返回 $false，避免无谓写盘。
Test-Case '数组值相同时返回 $false（幂等）' {
    $config = New-JsonObject '{"GamePath":"x","DllList":["a.dll","b.dll"]}'
    $changed = Set-JsonPropertyValue -Object $config -Name 'DllList' -Value @('a.dll', 'b.dll')
    return (-not $changed -and @($config.DllList).Count -eq 2)
}

# 长度不同必须判为"变化"。
Test-Case '数组长度不同判为变化' {
    $config = New-JsonObject '{"GamePath":"x","DllList":["a.dll"]}'
    $changed = Set-JsonPropertyValue -Object $config -Name 'DllList' -Value @('a.dll', 'b.dll')
    return ($changed -and @($config.DllList).Count -eq 2)
}

# 属性不存在时新增。
Test-Case '属性不存在时新增' {
    $config = New-JsonObject '{"GamePath":"x"}'
    $changed = Set-JsonPropertyValue -Object $config -Name 'DllList' -Value @('a.dll')
    return ($changed -and @($config.DllList).Count -eq 1)
}

# 标量行为不得被改动（原实现就是靠 -eq 工作的）。
Test-Case '标量：变化时写入并返回 $true' {
    $config = New-JsonObject '{"GamePath":"x","FPSTarget":60}'
    $changed = Set-JsonPropertyValue -Object $config -Name 'FPSTarget' -Value 300
    return ($changed -and $config.FPSTarget -eq 300)
}

Test-Case '标量：未变化时返回 $false' {
    $config = New-JsonObject '{"GamePath":"x","FPSTarget":300}'
    $changed = Set-JsonPropertyValue -Object $config -Name 'FPSTarget' -Value 300
    return (-not $changed)
}

Write-Host ''
Write-Host '=== Test-JsonPropertyValueEqual ===' -ForegroundColor Cyan

Test-Case 'null 与单元素数组判为不等（修复前的误判点）' {
    return (-not (Test-JsonPropertyValueEqual -Left $null -Right @('a.dll')))
}

Test-Case '两个空数组判为相等' {
    return (Test-JsonPropertyValueEqual -Left @() -Right @())
}

Test-Case '元素顺序不同判为不等' {
    return (-not (Test-JsonPropertyValueEqual -Left @('a', 'b') -Right @('b', 'a')))
}

Test-Case '字符串不被当成数组处理' {
    return (Test-JsonPropertyValueEqual -Left 'abc' -Right 'abc')
}

Test-Case 'List[string] 与 Array 可比' {
    return (Test-JsonPropertyValueEqual -Left (New-StringList @('a', 'b')) -Right @('a', 'b'))
}

# ConvertFrom-Json 把 `[]` 读成空字符串，它必须等价于空数组，
# 否则"配置里本来就是空表"的场景会被判为有变化而反复写盘。
Test-Case '空字符串等价于空数组' {
    $config = New-JsonObject '{"DllList":[]}'
    $raw = $config.PSObject.Properties['DllList'].Value
    return ((Test-JsonPropertyValueEqual -Left $raw -Right @()) -and
            (Test-JsonPropertyValueEqual -Left $raw -Right (New-StringList @())))
}

Test-Case '空字符串与单元素数组判为不等' {
    $config = New-JsonObject '{"DllList":[]}'
    $raw = $config.PSObject.Properties['DllList'].Value
    return (-not (Test-JsonPropertyValueEqual -Left $raw -Right @('a.dll')))
}

Write-Host ''
Write-Host '=== 负向对照：确认本测试确实能抓住那个缺陷 ===' -ForegroundColor Cyan

# 复现修复前的判断方式。若它在这里也判成"需要写入"，说明上面的用例并没有覆盖
# 真正的缺陷，测试就失去了意义。
# 注意：缺陷的左侧是**空字符串**（ConvertFrom-Json 读 `[]` 的结果），不是 $null。
Test-Case '旧写法（-ne）在 空字符串 vs 数组 时误判为"相等"（缺陷可复现）' {
    $rawEmpty = (New-JsonObject '{"DllList":[]}').PSObject.Properties['DllList'].Value
    $oldStyleSaysChanged = [bool]($rawEmpty -ne @('a.dll', 'b.dll'))
    return (-not $oldStyleSaysChanged)
}

Write-Host ''
Write-Host ("合计: {0} 通过, {1} 失败" -f $script:Pass, $script:Fail) -ForegroundColor $(if ($script:Fail -eq 0) { 'Green' } else { 'Red' })
if ($script:Fail -eq 0) { Write-Host 'ALL PASS' -ForegroundColor Green }
exit $(if ($script:Fail -eq 0) { 0 } else { 1 })
