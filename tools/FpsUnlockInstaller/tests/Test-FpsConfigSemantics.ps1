#Requires -Version 5.1
<#
fps_config.json 的「三条写入语义」回归测试。

守住的语义（权威说明见 Configure.ps1 的「fps_config.json 的三条写入语义」）：
  ① 【重新安装】  `-Reinstall`              ⇒ **全部覆写**为默认值
                                             （AutoStart / AutoClose / PopupWindow / StartMinimized ...）
  ② 【恢复设置】  `-ResetPluginConfigsOnly` ⇒ **全部覆写**（FPSTarget=60，DllList 保留既有选择）
  ③ 【只运行安装脚本】无开关                 ⇒ **只强制 4 个键**
                                             （GamePath / FPSTarget / DllList / UseHDR），其余键原样保留
另守住：UseHDR 的判据 = **ReShade64.dll 是否在本次写出的 DllList 里**（不是无脑 true / false）。

为什么需要这个测试：① 与 ③ 的区别**只在传不传 `-Reinstall`**，很容易被后来者"顺手统一"改坏；
而现场症状（用户改的 AutoClose=false 被悄悄覆写成 true，或反过来"重新安装后一个键都没写"）
都不会报错，只会在用户手里表现为"设置莫名其妙没了"。故用真脚本跑真写入来钉死。

做法：在 %TEMP% 下自建临时沙箱（dummy 游戏程序 / dummy ReShade64.dll / dummy unlocker），
把仓库里的**真** Configure.ps1 及其依赖拷进去运行，读回 fps_config.json 断言；
跑完自清理。不依赖真实游戏目录、真实 ReShade、真实 unlockfps_nc.exe，也不修改仓库任何文件。

用法：powershell -NoProfile -File tools\FpsUnlockInstaller\tests\Test-FpsConfigSemantics.ps1
#>
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$installerDir = Split-Path -Parent $PSScriptRoot
$requiredScripts = @('Configure.ps1', 'Localization.ps1', 'InstallerCommon.ps1', 'ReShadeResources.ps1')
foreach ($name in $requiredScripts) {
    if (-not (Test-Path -LiteralPath (Join-Path $installerDir $name) -PathType Leaf)) {
        Write-Host ("找不到被测脚本: {0}" -f (Join-Path $installerDir $name)) -ForegroundColor Red
        exit 2
    }
}

$script:Pass = 0
$script:Fail = 0
function Chk {
    param([bool]$Condition, [string]$Message)
    if ($Condition) { Write-Host ("  [PASS] {0}" -f $Message) -ForegroundColor Green; $script:Pass++ }
    else { Write-Host ("  [FAIL] {0}" -f $Message) -ForegroundColor Red; $script:Fail++ }
}

$sandbox = Join-Path $env:TEMP ('fpsconfig_semantics_' + [guid]::NewGuid().ToString('N'))
$exitCode = 0
try {
    # ---------------------------------------------------------------- 沙箱
    $scriptsDir = Join-Path $sandbox 'scripts'
    $payload = Join-Path $sandbox 'payload'
    $gameDir = Join-Path $sandbox 'game'
    New-Item -ItemType Directory -Force -Path $scriptsDir, $payload, $gameDir, (Join-Path $payload 'ReShade\reshade-shaders') | Out-Null
    foreach ($name in $requiredScripts) {
        Copy-Item -LiteralPath (Join-Path $installerDir $name) -Destination $scriptsDir -Force
    }
    Set-Content -LiteralPath (Join-Path $sandbox 'unlockfps_nc.exe') -Value 'dummy' -Encoding ASCII
    Set-Content -LiteralPath (Join-Path $gameDir 'YuanShen.exe') -Value 'dummy' -Encoding ASCII
    # S5/S6 用的 dummy ReShade（-ReShadeSource Existing 只做存在性断言，不下载）
    Set-Content -LiteralPath (Join-Path $payload 'ReShade\ReShade64.dll') -Value 'dummy' -Encoding ASCII
    $bridgeDll = Join-Path $payload 'Bridge\Dx11FsrBridge.dll'

    $gameExe = Join-Path $gameDir 'YuanShen.exe'
    $fpsConfig = Join-Path $sandbox 'fps_config.json'
    $configure = Join-Path $scriptsDir 'Configure.ps1'
    $otherGame = 'D:\OtherGame\YuanShen.exe'   # 配置里的旧 GamePath：模拟"包内陈旧配置"

    function New-Fixture {
        param([string]$DllEntry)
        $dllList = @()
        if (-not [string]::IsNullOrWhiteSpace($DllEntry)) { $dllList = @($DllEntry) }
        [ordered]@{
            GamePath = $otherGame
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
            DllList = $dllList
        } | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $fpsConfig -Encoding UTF8
    }
    function Invoke-Configure {
        param([string[]]$Extra)
        $allArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $configure,
            '-GamePath', $gameExe, '-NonInteractive', '-NoShortcut', '-Language', 'zh-CN') + $Extra
        Write-Host ("    > powershell " + ($allArgs -join ' ')) -ForegroundColor DarkGray
        $output = & powershell.exe @allArgs 2>&1 | Out-String
        $code = $LASTEXITCODE
        $config = Get-Content -LiteralPath $fpsConfig -Raw -Encoding UTF8 | ConvertFrom-Json
        return [pscustomobject]@{ Output = $output; ExitCode = $code; Config = $config }
    }

    $off = @('-UnlockerSource', 'Existing', '-DisableBridge', '-DisableOptiScaler', '-DisableAntiBlur', '-DisableHDR', '-DisableTextureLoader')

    Write-Host ''
    Write-Host '=== ① 重新安装（-Reinstall）⇒ 全部覆写 ===' -ForegroundColor Cyan
    New-Fixture
    $r = Invoke-Configure -Extra (@('-Reinstall', '-FpsTarget', '240') + $off)
    Write-Host ("    结果: exit={0} AutoStart={1} AutoClose={2} PopupWindow={3} StartMinimized={4} Fullscreen={5} UsePowerSave={6} UseMobileUI={7} CustomResX={8} Priority={9} AddCmd='{10}' LastVersionNotify={11} FPSTarget={12} UseHDR={13} DllList={14}" -f `
            $r.ExitCode, $r.Config.AutoStart, $r.Config.AutoClose, $r.Config.PopupWindow, $r.Config.StartMinimized, $r.Config.Fullscreen, $r.Config.UsePowerSave, $r.Config.UseMobileUI, $r.Config.CustomResX, $r.Config.Priority, $r.Config.AdditionalCommandLine, $r.Config.LastVersionNotify, $r.Config.FPSTarget, $r.Config.UseHDR, (@($r.Config.DllList).Count)) -ForegroundColor DarkGray
    Chk ($r.ExitCode -eq 0) 'exit code = 0'
    Chk ($r.Config.AutoStart -eq $true -and $r.Config.AutoClose -eq $true) 'AutoStart/AutoClose 回到 true'
    Chk ($r.Config.PopupWindow -eq $true -and $r.Config.StartMinimized -eq $true) 'PopupWindow/StartMinimized 回到 true'
    Chk ($r.Config.Fullscreen -eq $false -and $r.Config.UsePowerSave -eq $false -and $r.Config.UseMobileUI -eq $false) 'Fullscreen/UsePowerSave/UseMobileUI 回到 false'
    Chk ($r.Config.CustomResX -eq 1920 -and $r.Config.CustomResY -eq 1080 -and $r.Config.Priority -eq 3) '分辨率/优先级回到默认'
    Chk ($r.Config.AdditionalCommandLine -eq '' -and $r.Config.LastVersionNotify -eq 0) 'AdditionalCommandLine/LastVersionNotify 回到默认'
    Chk ($r.Config.FPSTarget -eq 240) 'FPSTarget 用调用方传入值（不是 60）'
    Chk ($r.Config.GamePath -eq $gameExe) 'GamePath 修为当前游戏'
    Chk ($r.Config.UseHDR -eq $false) 'UseHDR = ReShade 不在 DllList ⇒ false（不是无脑 true）'

    Write-Host ''
    Write-Host '=== ③ 只运行安装脚本（无开关）⇒ 只强制 4 个键，其余原样保留 ===' -ForegroundColor Cyan
    New-Fixture
    $r = Invoke-Configure -Extra (@('-FpsTarget', '240') + $off)
    Write-Host ("    结果: exit={0} AutoClose={1} PopupWindow={2} StartMinimized={3} Fullscreen={4} UsePowerSave={5} CustomResX={6} Priority={7} AddCmd='{8}' LastVersionNotify={9} FPSTarget={10} GamePath='{11}'" -f `
            $r.ExitCode, $r.Config.AutoClose, $r.Config.PopupWindow, $r.Config.StartMinimized, $r.Config.Fullscreen, $r.Config.UsePowerSave, $r.Config.CustomResX, $r.Config.Priority, $r.Config.AdditionalCommandLine, $r.Config.LastVersionNotify, $r.Config.FPSTarget, $r.Config.GamePath) -ForegroundColor DarkGray
    Chk ($r.ExitCode -eq 0) 'exit code = 0'
    Chk ($r.Config.AutoClose -eq $false -and $r.Config.PopupWindow -eq $false -and $r.Config.StartMinimized -eq $false) 'AutoClose/PopupWindow/StartMinimized 保持 false（未覆写）'
    Chk ($r.Config.Fullscreen -eq $true -and $r.Config.UsePowerSave -eq $true -and $r.Config.UseMobileUI -eq $true) 'Fullscreen/UsePowerSave/UseMobileUI 保持 true（未覆写）'
    Chk ($r.Config.CustomResX -eq 1280 -and $r.Config.Priority -eq 5) '分辨率/优先级保持原值（未覆写）'
    Chk ($r.Config.AdditionalCommandLine -eq '--user-flag' -and $r.Config.LastVersionNotify -eq 7) 'AdditionalCommandLine/LastVersionNotify 保持原值（未覆写）'
    Chk ($r.Config.FPSTarget -eq 240) 'FPSTarget 被强制为 240（4 键之一）'
    Chk ($r.Config.GamePath -eq $gameExe) 'GamePath 被修为当前游戏（4 键之一）'
    Chk (@($r.Config.DllList).Count -eq 0) 'DllList 被刷新为当前选择（4 键之一）'

    Write-Host ''
    Write-Host '=== 首次安装（无配置文件）⇒ 全量默认值 ===' -ForegroundColor Cyan
    Remove-Item -LiteralPath $fpsConfig -Force
    $r = Invoke-Configure -Extra (@('-FpsTarget', '240') + $off)
    Write-Host ("    结果: exit={0} AutoStart={1} AutoClose={2} PopupWindow={3} StartMinimized={4} Fullscreen={5} FPSTarget={6}" -f `
            $r.ExitCode, $r.Config.AutoStart, $r.Config.AutoClose, $r.Config.PopupWindow, $r.Config.StartMinimized, $r.Config.Fullscreen, $r.Config.FPSTarget) -ForegroundColor DarkGray
    Chk ($r.ExitCode -eq 0) 'exit code = 0'
    Chk ($r.Config.AutoStart -eq $true -and $r.Config.AutoClose -eq $true -and $r.Config.PopupWindow -eq $true) 'AutoStart/AutoClose/PopupWindow = true'
    Chk ($r.Config.StartMinimized -eq $true -and $r.Config.Fullscreen -eq $false) 'StartMinimized=true / Fullscreen=false'
    Chk ($r.Config.FPSTarget -eq 240 -and $r.Config.GamePath -eq $gameExe) 'FPSTarget/GamePath 正确'

    Write-Host ''
    Write-Host '=== ② 恢复设置（-ResetPluginConfigsOnly）⇒ 全部覆写 + FPSTarget=60 + DllList 保留 ===' -ForegroundColor Cyan
    New-Fixture -DllEntry $bridgeDll
    $r = Invoke-Configure -Extra @('-ResetPluginConfigsOnly')
    Write-Host ("    结果: exit={0} AutoClose={1} PopupWindow={2} StartMinimized={3} CustomResX={4} Priority={5} FPSTarget={6} DllList={7}" -f `
            $r.ExitCode, $r.Config.AutoClose, $r.Config.PopupWindow, $r.Config.StartMinimized, $r.Config.CustomResX, $r.Config.Priority, $r.Config.FPSTarget, (@($r.Config.DllList) -join ';')) -ForegroundColor DarkGray
    Chk ($r.ExitCode -eq 0) 'exit code = 0'
    Chk ($r.Config.AutoClose -eq $true -and $r.Config.StartMinimized -eq $true -and $r.Config.PopupWindow -eq $true) 'AutoClose/StartMinimized/PopupWindow 回到 true'
    Chk ($r.Config.CustomResX -eq 1920 -and $r.Config.Priority -eq 3) '分辨率/优先级回到默认'
    Chk ($r.Config.FPSTarget -eq 60) 'FPSTarget = 60（恢复设置的既有行为）'
    Chk (@($r.Config.DllList).Count -eq 1) 'DllList 保留既有已配置项'

    Write-Host ''
    Write-Host '=== ① + ReShade 已装 ⇒ UseHDR 判据 ===' -ForegroundColor Cyan
    New-Fixture
    $r = Invoke-Configure -Extra @('-Reinstall', '-FpsTarget', '240', '-UnlockerSource', 'Existing', '-DisableBridge', '-DisableOptiScaler', '-DisableAntiBlur', '-DisableTextureLoader', '-ReShadeSource', 'Existing')
    Write-Host ("    结果: exit={0} AutoClose={1} UseHDR={2} DllList={3}" -f $r.ExitCode, $r.Config.AutoClose, $r.Config.UseHDR, (@($r.Config.DllList) -join ';')) -ForegroundColor DarkGray
    Chk ($r.ExitCode -eq 0) 'exit code = 0'
    Chk ($r.Config.AutoClose -eq $true) 'AutoClose 被覆写为 true'
    Chk ($r.Config.UseHDR -eq $true) 'UseHDR = ReShade 在 DllList ⇒ true'
    Chk (@($r.Config.DllList).Count -eq 1 -and ([string]$r.Config.DllList[0]) -match 'ReShade64\.dll$') 'DllList 只含 ReShade64.dll'

    Write-Host ''
    Write-Host '=== ③ + ReShade 已装 ⇒ 不覆写，但 UseHDR 跟随 DllList ===' -ForegroundColor Cyan
    New-Fixture
    $r = Invoke-Configure -Extra @('-FpsTarget', '240', '-UnlockerSource', 'Existing', '-DisableBridge', '-DisableOptiScaler', '-DisableAntiBlur', '-DisableTextureLoader', '-ReShadeSource', 'Existing')
    Write-Host ("    结果: exit={0} AutoClose={1} UseHDR={2} DllList={3}" -f $r.ExitCode, $r.Config.AutoClose, $r.Config.UseHDR, (@($r.Config.DllList) -join ';')) -ForegroundColor DarkGray
    Chk ($r.ExitCode -eq 0) 'exit code = 0'
    Chk ($r.Config.AutoClose -eq $false) 'AutoClose 保持 false（不覆写）'
    Chk ($r.Config.UseHDR -eq $true) 'UseHDR 跟随 DllList（ReShade 在 ⇒ true）'
}
finally {
    if (Test-Path -LiteralPath $sandbox) { Remove-Item -LiteralPath $sandbox -Recurse -Force -ErrorAction SilentlyContinue }
    if ($script:Fail -ne 0) { $exitCode = 1 }
}

Write-Host ''
Write-Host ("合计: {0} 通过, {1} 失败" -f $script:Pass, $script:Fail) -ForegroundColor $(if ($script:Fail -eq 0) { 'Green' } else { 'Red' })
if ($script:Fail -eq 0) { Write-Host 'ALL PASS' -ForegroundColor Green }
exit $exitCode
