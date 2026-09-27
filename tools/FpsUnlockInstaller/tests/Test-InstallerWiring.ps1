#Requires -Version 5.1
<#
Installer.ps1 侧的两组回归测试。

A. fps_config.json「三条写入语义」的**接线**（① 重新安装 / ③ 只运行安装脚本）
   做法：从 Installer.ps1 的 AST 里取**真函数**（Invoke-FoundationSetup / Invoke-InstallWizard /
   Invoke-UpdateWizard），把函数体末尾的 `& powershell.exe @arguments` 用**同名函数**截获
   ⇒ 断言它究竟给 Configure.ps1 传了什么（该不该带 `-Reinstall`），再用真 powershell.exe
   把记录的参数**原样重放**到沙箱里的真 Configure.ps1 ⇒ 看 fps_config.json 的真实结果。
     · ① 重新安装：FPS Unlocker 缺失 ⇒ 带 `-Reinstall` ⇒ AutoClose:false → true
     · ① 重新安装：菜单 1「安装模块」⇒ 带 `-Reinstall` ⇒ AutoClose:false → true
     · ③ 只运行安装脚本：一切就绪（Invoke-FoundationSetup 的 $ready 短路）⇒ **一次都不调用**
     · ③ 只运行安装脚本：unlocker 在 + GamePath 不匹配（包内陈旧配置）⇒ 调用但**不带** -Reinstall
     · ③ 升级：菜单 2「更新模块」⇒ 调用但**不带** -Reinstall（不覆写用户设置）

B. 一类缺陷的守卫：`$x = if (...) { @(...) }` 之后又按数组用（.Count / 索引 / -contains）
   PowerShell 把这种写法解释为"把 if 的**输出**赋给变量"：输出只有一个对象时退化成**标量**，
   于是 `$x.Count` 在 `Set-StrictMode -Version Latest` 下直接抛
   "The property 'Count' cannot be found on this object"。
   现场影响：菜单 2「更新模块」以前**输入单个模块号或 0 都会崩**（只有输入 "A" 能走通）。
     · B1 运行期：真 Invoke-UpdateWizard + Select-ModuleSet 的真实返回约定 ⇒ 两条都必须无异常
     · B2 静态：全目录 AST 扫描同类写法，命中数必须为 0（另附**负向对照**证明扫描器真能抓到）

沙箱自建自清理（%TEMP%），不依赖真实游戏目录 / 真实 ReShade / 真实 unlockfps_nc.exe，
也不修改仓库里任何被测脚本。

用法：powershell -NoProfile -File tools\FpsUnlockInstaller\tests\Test-InstallerWiring.ps1
#>
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$installerDir = Split-Path -Parent $PSScriptRoot
$installerPath = Join-Path $installerDir 'Installer.ps1'
$requiredScripts = @('Installer.ps1', 'Configure.ps1', 'Localization.ps1', 'InstallerCommon.ps1', 'ReShadeResources.ps1')
foreach ($name in $requiredScripts) {
    if (-not (Test-Path -LiteralPath (Join-Path $installerDir $name) -PathType Leaf)) {
        Write-Host ("找不到被测脚本: {0}" -f (Join-Path $installerDir $name)) -ForegroundColor Red
        exit 2
    }
}
# ⚠️ 必须在定义同名截获函数**之前**解析真 exe 路径（-CommandType Application 只找可执行文件）
$realWindowsPowerShell = (Get-Command -CommandType Application -Name 'powershell.exe' -ErrorAction SilentlyContinue | Select-Object -First 1).Source
if ([string]::IsNullOrWhiteSpace($realWindowsPowerShell)) {
    Write-Host '找不到 Windows PowerShell (powershell.exe)：本测试需要它来重放真实调用。' -ForegroundColor Red
    exit 2
}

$script:Pass = 0
$script:Fail = 0
function Chk {
    param([bool]$Condition, [string]$Message)
    if ($Condition) { Write-Host ("  [PASS] {0}" -f $Message) -ForegroundColor Green; $script:Pass++ }
    else { Write-Host ("  [FAIL] {0}" -f $Message) -ForegroundColor Red; $script:Fail++ }
}

# ---------------------------------------------------------------- 一类缺陷的静态扫描
function Test-InAstRange {
    param([int]$Start, [int]$End, [object[]]$Ranges)
    foreach ($range in $Ranges) {
        if ($Start -ge $range.Start -and $End -le $range.End) { return $true }
    }
    return $false
}
function Get-IfAssignArrayHits {
    param($RootAst)
    $hits = [Collections.Generic.List[string]]::new()
    $functions = @($RootAst.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $true))
    $scopes = @([pscustomobject]@{ Name = '<script>'; Ast = $RootAst; Nested = $functions })
    foreach ($function in $functions) {
        # FindAll 含自身 ⇒ 必须排除，否则"嵌套函数"会覆盖整个函数体、把命中全过滤掉
        $inner = @($function.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $true) |
            Where-Object { $_.Extent.StartOffset -ne $function.Extent.StartOffset })
        $scopes += [pscustomobject]@{ Name = $function.Name; Ast = $function; Nested = $inner }
    }
    foreach ($scope in $scopes) {
        $ranges = @($scope.Nested | ForEach-Object { [pscustomobject]@{ Start = $_.Extent.StartOffset; End = $_.Extent.EndOffset } })
        $assignments = $scope.Ast.FindAll({
                param($n)
                $n -is [System.Management.Automation.Language.AssignmentStatementAst] -and
                $n.Left -is [System.Management.Automation.Language.VariableExpressionAst] -and
                $n.Right -is [System.Management.Automation.Language.IfStatementAst] -and
                -not (Test-InAstRange -Start $n.Extent.StartOffset -End $n.Extent.EndOffset -Ranges $ranges)
            }, $true)
        foreach ($assignment in $assignments) {
            $variableName = $assignment.Left.VariablePath.UserPath
            $usages = [Collections.Generic.List[string]]::new()
            foreach ($use in $scope.Ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.VariableExpressionAst] }, $true)) {
                if ($use.VariablePath.UserPath -ne $variableName) { continue }
                if (Test-InAstRange -Start $use.Extent.StartOffset -End $use.Extent.EndOffset -Ranges $ranges) { continue }
                $parent = $use.Parent
                if ($parent -is [System.Management.Automation.Language.MemberExpressionAst] -and $parent.Expression -eq $use) {
                    $memberName = $parent.Member.Extent.Text
                    if ($memberName -in @('Count', 'Length')) { $usages.Add(("line {0}: .{1}" -f $use.Extent.StartLineNumber, $memberName)) }
                }
                elseif ($parent -is [System.Management.Automation.Language.IndexExpressionAst] -and $parent.Target -eq $use) {
                    $usages.Add(("line {0}: 索引 [...]" -f $use.Extent.StartLineNumber))
                }
                elseif ($parent -is [System.Management.Automation.Language.BinaryExpressionAst] -and
                        $parent.Operator -in @('Contains', 'NotContains', 'In', 'NotIn') -and
                        ($parent.Left -eq $use -or $parent.Right -eq $use)) {
                    $usages.Add(("line {0}: -{1}" -f $use.Extent.StartLineNumber, $parent.Operator.ToString().ToLowerInvariant()))
                }
            }
            if ($usages.Count -gt 0) {
                $hits.Add(("函数 {0} 第 {1} 行: `${2} = if (...) ⇒ 数组语义用法 [{3}]" -f `
                            $scope.Name, $assignment.Extent.StartLineNumber, $variableName, (@($usages) -join '; ')))
            }
        }
    }
    return $hits
}

# ---------------------------------------------------------------- 沙箱 + 真函数
$sandbox = Join-Path $env:TEMP ('installer_wiring_' + [guid]::NewGuid().ToString('N'))
$exitCode = 0
try {
    $scriptsDir = Join-Path $sandbox 'scripts'
    $payloadDirectory = Join-Path $sandbox 'payload'
    $gameDir = Join-Path $sandbox 'game'
    New-Item -ItemType Directory -Force -Path $scriptsDir, (Join-Path $payloadDirectory 'Bridge'), (Join-Path $payloadDirectory 'AMD'), $gameDir | Out-Null
    foreach ($name in @('Configure.ps1', 'Localization.ps1', 'InstallerCommon.ps1', 'ReShadeResources.ps1')) {
        Copy-Item -LiteralPath (Join-Path $installerDir $name) -Destination $scriptsDir -Force
    }
    Set-Content -LiteralPath (Join-Path $gameDir 'YuanShen.exe') -Value 'dummy' -Encoding ASCII
    Set-Content -LiteralPath (Join-Path $payloadDirectory 'Bridge\Dx11FsrBridge.dll') -Value 'dummy' -Encoding ASCII
    Set-Content -LiteralPath (Join-Path $payloadDirectory 'AMD\amd_fidelityfx_upscaler_dx12.dll') -Value 'dummy' -Encoding ASCII

    $unlockerPath = Join-Path $sandbox 'unlockfps_nc.exe'
    $configureScript = Join-Path $scriptsDir 'Configure.ps1'
    $fpsConfigPath = Join-Path $sandbox 'fps_config.json'
    $statePath = $fpsConfigPath
    $errorLogPath = Join-Path $sandbox '.last-install-error.log'
    $bridgePath = Join-Path $payloadDirectory 'Bridge\Dx11FsrBridge.dll'
    $optiPath = Join-Path $payloadDirectory 'OptiScaler\OptiScaler.dll'
    $antiBlurPath = Join-Path $payloadDirectory 'AntiPlayerMosaic\AntiPlayerMosaic.dll'
    $reShadePath = Join-Path $payloadDirectory 'ReShade\ReShade64.dll'
    $textureLoaderPath = Join-Path $payloadDirectory 'TextureLoader\TextureLoader.dll'
    $gameExe = Join-Path $gameDir 'YuanShen.exe'
    $otherGame = 'D:\OtherGame\YuanShen.exe'
    $NoShortcut = $true
    $script:Language = 'zh-CN'
    $script:nvidiaGpu = $false
    $script:SelfUpdateStarted = $false

    $ast = [System.Management.Automation.Language.Parser]::ParseFile($installerPath, [ref]$null, [ref]$null)
    foreach ($name in 'Invoke-FoundationSetup', 'Invoke-InstallWizard', 'Invoke-UpdateWizard') {
        $definition = $ast.FindAll({
                param($n)
                $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name
            }, $true) | Select-Object -First 1
        if (-not $definition) { throw "在 Installer.ps1 中找不到 $name" }
        Invoke-Expression $definition.Extent.Text
    }

    # 截获 `& powershell.exe @arguments`（函数优先于外部程序）⇒ 记录实参，不真的执行
    $script:Calls = 0
    $global:Captured = $null
    function powershell.exe {
        param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments)
        $global:Captured = $Arguments
        $script:Calls++
        $global:LASTEXITCODE = 0   # 调用方会读 $LASTEXITCODE，名字被函数占用后必须自己补
        return 0
    }
    function Invoke-Replay {
        param([string[]]$Arguments)
        $output = & $realWindowsPowerShell @Arguments 2>&1 | Out-String
        return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $output }
    }

    # 桩：只保留被测入口本身的行为
    function Write-Header { param([string]$Title) }
    function Pause-Menu { }
    function Write-InstallCatalog { param([string]$SelectedGamePath) }
    function Save-State { param([string]$SelectedGamePath, [int]$FpsTarget) }
    function Update-FpsTarget { param([int]$FpsTarget) }
    function Set-FpsTarget { param([int]$CurrentValue, [int]$DefaultValue = 0, [switch]$FirstRun) return 60 }
    function Invoke-NvidiaDlssSetup { }
    function Start-PackageSelfUpdate { param([string]$ResumeGamePath, [switch]$ResumeUpdateAll) return $false }
    function Select-ComponentSource {
        param([string]$Name, [string]$Repository, [string]$AssetPattern, [string]$LocalPath, [string]$CompareMode, [string]$FileFilter, [switch]$NoConfirm, [switch]$NoPause)
        return [pscustomobject]@{ Mode = 'Existing'; Path = $null }
    }
    function Select-ReShadeSource { return 'Existing' }
    function Test-ComponentUpdate {
        param([string]$Name, [string]$Repository, [string]$AssetPattern, [string]$LocalPath, [string]$CompareMode, [switch]$NoConfirm, [switch]$NoPause)
        return 'Existing'
    }
    # Select-ModuleSet 的**真实返回约定**：单个模块号 ⇒ @($selectedId)；返回上一层 ⇒ @()
    function Select-ModuleSet {
        param([string]$ActionName, [switch]$ExcludeTextureLoaderFromAll)
        return @($global:ModuleSelection)
    }
    function Get-FpsConfig {
        if (-not (Test-Path -LiteralPath $fpsConfigPath -PathType Leaf)) { return $null }
        try { return Get-Content -LiteralPath $fpsConfigPath -Raw -Encoding UTF8 | ConvertFrom-Json } catch { return $null }
    }
    function Test-ConfiguredDll {
        param([object]$Config, [string]$Path)
        if ($null -eq $Config -or $null -eq $Config.DllList) { return $false }
        foreach ($configuredPath in @($Config.DllList)) {
            if ([string]::Equals([IO.Path]::GetFullPath([string]$configuredPath), [IO.Path]::GetFullPath($Path), [StringComparison]::OrdinalIgnoreCase)) { return $true }
        }
        return $false
    }
    function Get-ConfiguredPluginState {
        $config = Get-FpsConfig
        return [ordered]@{
            Bridge = Test-ConfiguredDll -Config $config -Path $bridgePath
            OptiScaler = Test-ConfiguredDll -Config $config -Path $optiPath
            AntiBlur = Test-ConfiguredDll -Config $config -Path $antiBlurPath
            HDR = Test-ConfiguredDll -Config $config -Path $reShadePath
        }
    }
    function Get-ModuleState {
        param([string]$SelectedGamePath)
        return [ordered]@{ Unlocker = $true; Bridge = $true; OptiScaler = $false; AntiBlur = $false; HDR = $false; TextureLoader = $false }
    }

    function New-Fixture {
        param([string]$GamePathValue = $otherGame)
        [ordered]@{
            GamePath = $GamePathValue
            AutoStart = $true
            AutoClose = $false
            PopupWindow = $false
            Fullscreen = $true
            UseCustomRes = $true
            IsExclusiveFullscreen = $true
            StartMinimized = $false
            UsePowerSave = $true
            SuspendLoad = $true
            UseMobileUI = $true
            UseHDR = $false
            FPSTarget = 400
            CustomResX = 1280
            CustomResY = 720
            MonitorNum = 1
            Priority = 5
            AdditionalCommandLine = '--user-flag'
            LastVersionNotify = 7
            DllList = @()
        } | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $fpsConfigPath -Encoding UTF8
    }
    function Get-CapturedArguments {
        if ($null -eq $global:Captured) { return @() }
        return @($global:Captured)
    }

    Write-Host ''
    Write-Host '=== A1 ① 重新安装：FPS Unlocker 缺失 ⇒ Invoke-FoundationSetup 传 -Reinstall ===' -ForegroundColor Cyan
    New-Fixture
    Remove-Item -LiteralPath $unlockerPath -Force -ErrorAction SilentlyContinue
    $global:Captured = $null
    $script:Calls = 0
    $returned = Invoke-FoundationSetup -SelectedGamePath $gameExe -FpsTarget ([ref]60)
    $arguments = Get-CapturedArguments
    Write-Host ("    实际传给 Configure.ps1: {0}" -f ($arguments -join ' ')) -ForegroundColor DarkGray
    Chk ($returned -eq $true) 'Invoke-FoundationSetup 返回 true'
    Chk ($arguments -contains '-Reinstall') '带了 -Reinstall'
    Set-Content -LiteralPath $unlockerPath -Value 'dummy' -Encoding ASCII   # 重放前补回（真实流程里由 Configure.ps1 安装）
    $replay = Invoke-Replay -Arguments $arguments
    $config = Get-FpsConfig
    Write-Host ("    重放结果: exit={0} AutoClose={1} PopupWindow={2} StartMinimized={3} FPSTarget={4}" -f $replay.ExitCode, $config.AutoClose, $config.PopupWindow, $config.StartMinimized, $config.FPSTarget) -ForegroundColor DarkGray
    Chk ($replay.ExitCode -eq 0) '重放的 Configure.ps1 退出码 0'
    Chk ($config.AutoClose -eq $true -and $config.PopupWindow -eq $true -and $config.StartMinimized -eq $true) 'AutoClose/PopupWindow/StartMinimized 由 false ⇒ true（全部覆写已生效）'
    Chk ($config.FPSTarget -ne 400) 'FPSTarget 被刷新（不是陈旧的 400）'

    Write-Host ''
    Write-Host '=== A2 ③ 只运行安装脚本：一切就绪 ⇒ 短路，连 Configure.ps1 都不调用 ===' -ForegroundColor Cyan
    New-Fixture -GamePathValue $gameExe
    Set-Content -LiteralPath $unlockerPath -Value 'dummy' -Encoding ASCII
    $global:Captured = $null
    $script:Calls = 0
    $returned = Invoke-FoundationSetup -SelectedGamePath $gameExe -FpsTarget ([ref]240)
    $config = Get-FpsConfig
    Write-Host ("    调用 Configure.ps1 次数: {0}" -f $script:Calls) -ForegroundColor DarkGray
    Chk ($returned -eq $true) 'Invoke-FoundationSetup 返回 true（无需安装）'
    Chk ($script:Calls -eq 0) '一次都没有调用 Configure.ps1（一个键都不写）'
    Chk ($config.AutoClose -eq $false -and $config.PopupWindow -eq $false -and $config.CustomResX -eq 1280) 'AutoClose/PopupWindow/CustomResX 原样保留（不覆写）'

    Write-Host ''
    Write-Host '=== A3 ③ 只运行安装脚本：unlocker 在 + GamePath 不匹配 ⇒ 修路径但不带 -Reinstall ===' -ForegroundColor Cyan
    New-Fixture
    $global:Captured = $null
    $script:Calls = 0
    $returned = Invoke-FoundationSetup -SelectedGamePath $gameExe -FpsTarget ([ref]240)
    $arguments = Get-CapturedArguments
    Write-Host ("    实际传给 Configure.ps1: {0}" -f ($arguments -join ' ')) -ForegroundColor DarkGray
    Chk ($returned -eq $true) 'Invoke-FoundationSetup 返回 true'
    Chk ($arguments.Count -gt 0) '调用了 Configure.ps1（要修 GamePath）'
    Chk (-not ($arguments -contains '-Reinstall')) '**没有** -Reinstall'
    $replay = Invoke-Replay -Arguments $arguments
    $config = Get-FpsConfig
    Write-Host ("    重放结果: exit={0} AutoClose={1} PopupWindow={2} CustomResX={3} GamePath='{4}'" -f $replay.ExitCode, $config.AutoClose, $config.PopupWindow, $config.CustomResX, $config.GamePath) -ForegroundColor DarkGray
    Chk ($config.GamePath -eq $gameExe) 'GamePath 被修正为当前游戏'
    Chk ($config.AutoClose -eq $false -and $config.CustomResX -eq 1280) 'AutoClose/CustomResX 原样保留（③ 不覆写）'

    Write-Host ''
    Write-Host '=== A4 ① 重新安装：菜单 1「安装模块」⇒ Invoke-InstallWizard 传 -Reinstall ===' -ForegroundColor Cyan
    New-Fixture -GamePathValue $gameExe
    $global:ModuleSelection = 1
    $global:Captured = $null
    $script:Calls = 0
    Invoke-InstallWizard -SelectedGamePath $gameExe -FpsTarget 240
    $arguments = Get-CapturedArguments
    Write-Host ("    实际传给 Configure.ps1: {0}" -f ($arguments -join ' ')) -ForegroundColor DarkGray
    Chk ($arguments -contains '-Reinstall') '带了 -Reinstall'
    Chk ($arguments -contains '-PreserveExistingConfigs') '仍带 -PreserveExistingConfigs（只管 OptiScaler/ReShade ini）'
    $replay = Invoke-Replay -Arguments $arguments
    $config = Get-FpsConfig
    Write-Host ("    重放结果: exit={0} AutoClose={1} PopupWindow={2} StartMinimized={3} DllList={4}" -f $replay.ExitCode, $config.AutoClose, $config.PopupWindow, $config.StartMinimized, (@($config.DllList) -join ';')) -ForegroundColor DarkGray
    Chk ($replay.ExitCode -eq 0) '重放的 Configure.ps1 退出码 0'
    Chk ($config.AutoClose -eq $true -and $config.PopupWindow -eq $true -and $config.StartMinimized -eq $true) 'AutoClose/PopupWindow/StartMinimized 由 false ⇒ true'
    Chk (@($config.DllList).Count -eq 1) 'DllList = 本次选择的模块（Bridge）'

    Write-Host ''
    Write-Host '=== A5 ③ 升级：菜单 2「更新模块」⇒ Invoke-UpdateWizard 不带 -Reinstall ===' -ForegroundColor Cyan
    New-Fixture -GamePathValue $gameExe
    $global:Captured = $null
    $script:Calls = 0
    Invoke-UpdateWizard -SelectedGamePath $gameExe -FpsTarget 240 -SkipSelfUpdate -PreselectedModules @(1, 2)
    $arguments = Get-CapturedArguments
    Write-Host ("    实际传给 Configure.ps1: {0}" -f ($arguments -join ' ')) -ForegroundColor DarkGray
    Chk ($arguments.Count -gt 0) '更新流程调用了 Configure.ps1'
    Chk (-not ($arguments -contains '-Reinstall')) '**没有** -Reinstall（升级 = ③ 不覆写）'
    $replay = Invoke-Replay -Arguments $arguments
    $config = Get-FpsConfig
    Write-Host ("    重放结果: exit={0} AutoClose={1} PopupWindow={2} CustomResX={3}" -f $replay.ExitCode, $config.AutoClose, $config.PopupWindow, $config.CustomResX) -ForegroundColor DarkGray
    Chk ($config.AutoClose -eq $false -and $config.PopupWindow -eq $false -and $config.CustomResX -eq 1280) 'AutoClose/PopupWindow/CustomResX 原样保留（不覆写）'

    Write-Host ''
    Write-Host '=== B1 菜单 2 选择分支：单个模块号 / 0（if 赋值退化回归）===' -ForegroundColor Cyan
    # 以前 `$selection = if (...) { @(...) }` 会把单元素数组退化成标量 ⇒ $selection.Count 抛错 ⇒ 菜单 2 崩
    foreach ($case in @(
            @{ Name = '输入单个模块号（Select-ModuleSet 返回 @($selectedId)）'; Selection = 1; ExpectedCalls = 1 },
            @{ Name = '输入 0 返回上一层（Select-ModuleSet 返回 @()）'; Selection = @(); ExpectedCalls = 0 }
        )) {
        New-Fixture -GamePathValue $gameExe
        $global:ModuleSelection = $case.Selection
        $global:Captured = $null
        $script:Calls = 0
        $threw = $null
        try { Invoke-UpdateWizard -SelectedGamePath $gameExe -FpsTarget 240 -SkipSelfUpdate }
        catch { $threw = $_.Exception.Message }
        if ($null -ne $threw) {
            Write-Host ("    异常: {0}" -f $threw) -ForegroundColor DarkGray
        }
        else {
            Write-Host ("    调用 Configure.ps1 次数: {0}" -f $script:Calls) -ForegroundColor DarkGray
        }
        Chk ($null -eq $threw) ("{0} ⇒ 不抛异常" -f $case.Name)
        Chk ($script:Calls -eq $case.ExpectedCalls) ("{0} ⇒ 调用 Configure.ps1 {1} 次" -f $case.Name, $case.ExpectedCalls)
    }

    Write-Host ''
    Write-Host '=== B2 静态守卫：全目录不得再有 `$x = if (...) { @(...) }` + 数组语义用法 ===' -ForegroundColor Cyan
    $scannedFiles = @(Get-ChildItem -LiteralPath $installerDir -Recurse -File -Filter '*.ps1' | Sort-Object FullName)
    $allHits = [Collections.Generic.List[string]]::new()
    foreach ($file in $scannedFiles) {
        $fileAst = [System.Management.Automation.Language.Parser]::ParseFile($file.FullName, [ref]$null, [ref]$null)
        foreach ($hit in @(Get-IfAssignArrayHits -RootAst $fileAst)) {
            $allHits.Add(("{0}: {1}" -f $file.Name, $hit))
        }
    }
    foreach ($hit in $allHits) { Write-Host ("    命中: {0}" -f $hit) -ForegroundColor DarkGray }
    Write-Host ("    扫描 {0} 个 .ps1 文件，命中 {1} 处" -f $scannedFiles.Count, $allHits.Count) -ForegroundColor DarkGray
    Chk ($allHits.Count -eq 0) '同类写法 0 命中'
    # 负向对照：扫描器必须能抓到"故意的"坏样本，否则上面的 0 命中毫无意义
    $buggySample = @'
Set-StrictMode -Version Latest
function Sample-Bug {
    $items = if ($true) { @(1) } else { @(1, 2) }
    if ($items.Count -eq 0) { return }
}
'@
    $buggyAst = [System.Management.Automation.Language.Parser]::ParseInput($buggySample, [ref]$null, [ref]$null)
    $buggyHits = @(Get-IfAssignArrayHits -RootAst $buggyAst)
    Write-Host ("    负向对照命中 {0} 处（应为 ≥1）" -f $buggyHits.Count) -ForegroundColor DarkGray
    Chk ($buggyHits.Count -ge 1) '扫描器能抓到故意的坏样本（负向对照）'
}
finally {
    if (Test-Path -LiteralPath $sandbox) { Remove-Item -LiteralPath $sandbox -Recurse -Force -ErrorAction SilentlyContinue }
    if ($script:Fail -ne 0) { $exitCode = 1 }
}

Write-Host ''
Write-Host ("合计: {0} 通过, {1} 失败" -f $script:Pass, $script:Fail) -ForegroundColor $(if ($script:Fail -eq 0) { 'Green' } else { 'Red' })
if ($script:Fail -eq 0) { Write-Host 'ALL PASS' -ForegroundColor Green }
exit $exitCode
