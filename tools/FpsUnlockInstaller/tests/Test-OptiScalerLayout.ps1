# OptiScaler 双布局判定测试（2026-09-23）
#
# 审核项 ①「Fufu 双布局」原标注为"两条路线各跑一次确认"。但该风险的**判定核心**
# 是 `Get-OptiScalerLayout`（安装器侧）与插件侧的 `optiscaler_component_directory`，
# 两者都可直接测 —— 不必跑两次真实部署。
#
# 背景（`Configure.ps1:496-519`）：`[Libraries] OptiDllPath` 是 OptiScaler 查找
# 超分组件的**唯一依据**。嵌套布局下若写成根层，OptiScaler 会"注入成功却找不到
# 任何超分后端"，且退出时 DETACH 卡死 → 进程残留。
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$installerDir = Join-Path $repoRoot 'tools\FpsUnlockInstaller'
# ⚠️ 2026-09-24：INI 工具已搬到公共模块 `InstallerCommon.ps1`（审核报告「去重 8 条」）。
# 本测试原先只解析 Configure.ps1，搬家后会「找不到 Set-IniValue」而失败 ——
# 故按来源分别解析：Get-OptiScalerLayout 仍在 Configure.ps1，
# Get-IniValue / Set-IniValue 现由 InstallerCommon.ps1 提供。
$sources = @(
    @{ File = (Join-Path $installerDir 'Configure.ps1');       Functions = @('Get-OptiScalerLayout') },
    @{ File = (Join-Path $installerDir 'InstallerCommon.ps1'); Functions = @('Get-IniValue', 'Set-IniValue') }
)
foreach ($entry in $sources) {
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($entry.File, [ref]$null, [ref]$null)
    foreach ($fn in $entry.Functions) {
        $def = $ast.FindAll({
            param($n)
            $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $fn
        }, $true) | Select-Object -First 1
        if (-not $def) { throw "在 $($entry.File) 中找不到 $fn" }
        Invoke-Expression $def.Extent.Text
    }
}
# Get-IniValue 依赖模块内部的扫描函数，一并注入
$a = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $installerDir 'InstallerCommon.ps1'), [ref]$null, [ref]$null)
$core = $a.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Get-IniValueCore' }, $true) | Select-Object -First 1
if (-not $core) { throw '找不到 Get-IniValueCore' }
Invoke-Expression $core.Extent.Text

$ok = 0; $bad = 0
function Chk($cond, $msg) {
    if ($cond) { Write-Host "  ok:   $msg"; $script:ok++ }
    else       { Write-Host "  FAIL: $msg"; $script:bad++ }
}

$sandbox = Join-Path $env:TEMP ('optilayout_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $sandbox -Force | Out-Null
$marker = 'amd_fidelityfx_upscaler_dx12.dll'

function New-Layout {
    param([string]$Name)
    $root = Join-Path $sandbox $Name
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    return $root
}
function Touch($p) {
    New-Item -ItemType Directory -Path (Split-Path -Parent $p) -Force | Out-Null
    Set-Content -LiteralPath $p -Value 'x' -Encoding ASCII
}

# ---- 1) 平铺（0.9.x）：组件与主 DLL 同层 ----
$flat = New-Layout 'flat'
Touch (Join-Path $flat 'OptiScaler.dll')
Touch (Join-Path $flat $marker)
Touch (Join-Path $flat 'OptiScaler.ini')
$r1 = Get-OptiScalerLayout -Root $flat
Chk ($r1.Kind -eq 'flat') '平铺：Kind=flat'
Chk ($r1.ComponentDir -eq $flat) '平铺：ComponentDir = 根层'
Chk ($r1.MainDll -eq (Join-Path $flat 'OptiScaler.dll')) '平铺：MainDll = 根层主 DLL'

# ---- 2) 嵌套（0.10 每夜版）：组件在 OptiScaler\ 子目录，主 DLL 在根层 ----
$nested = New-Layout 'nested'
Touch (Join-Path $nested 'OptiScaler.dll')
Touch (Join-Path $nested 'OptiScaler.ini')
Touch (Join-Path $nested "OptiScaler\$marker")
$r2 = Get-OptiScalerLayout -Root $nested
Chk ($r2.Kind -eq 'nested') '嵌套：Kind=nested'
Chk ($r2.ComponentDir -eq (Join-Path $nested 'OptiScaler')) '嵌套：ComponentDir = OptiScaler\ 子目录'
Chk ($r2.MainDll -eq (Join-Path $nested 'OptiScaler.dll')) '嵌套：MainDll 取根层（新版发行包如此）'

# ---- 3) 嵌套容错：根层没有主 DLL，只有子目录有 ----
$tolerant = New-Layout 'tolerant'
Touch (Join-Path $tolerant "OptiScaler\$marker")
Touch (Join-Path $tolerant 'OptiScaler\OptiScaler.dll')
$r3 = Get-OptiScalerLayout -Root $tolerant
Chk ($r3.Kind -eq 'nested') '嵌套容错：Kind=nested'
Chk ($r3.MainDll -eq (Join-Path $tolerant 'OptiScaler\OptiScaler.dll')) '嵌套容错：MainDll 回退到子目录'

# ---- 4) 都缺失：Kind=empty，按平铺处理由后续断言报错 ----
$empty = New-Layout 'empty'
$r4 = Get-OptiScalerLayout -Root $empty
Chk ($r4.Kind -eq 'empty') '未安装：Kind=empty'
Chk ($r4.ComponentDir -eq $empty) '未安装：ComponentDir 按平铺（根层）'

# ---- 5) 两种布局下 OptiDllPath 应写成 ComponentDir（而非 ini 所在目录）----
foreach ($case in @(
    @{ Root = $flat;   Expect = $flat;                          Label = '平铺' },
    @{ Root = $nested; Expect = (Join-Path $nested 'OptiScaler'); Label = '嵌套' }
)) {
    $layout = Get-OptiScalerLayout -Root $case.Root
    $ini = Join-Path $case.Root 'OptiScaler.ini'
    Set-IniValue -Path $ini -Section 'Libraries' -Key 'OptiDllPath' -Value ([IO.Path]::GetFullPath($layout.ComponentDir))
    $written = Get-IniValue -Path $ini -Section 'Libraries' -Key 'OptiDllPath'
    Chk ($written -eq [IO.Path]::GetFullPath($case.Expect)) "$($case.Label)：OptiDllPath 写成组件目录"
    Chk ($written -ne [IO.Path]::GetFullPath($case.Root) -or $case.Label -eq '平铺') "$($case.Label)：OptiDllPath 不是 ini 所在目录（嵌套下两者必须不同）"
}

Remove-Item -LiteralPath $sandbox -Recurse -Force -ErrorAction SilentlyContinue
Write-Host ''
Write-Host ("  通过 {0} / 失败 {1}" -f $ok, $bad)
if ($bad -gt 0) { exit 1 }
