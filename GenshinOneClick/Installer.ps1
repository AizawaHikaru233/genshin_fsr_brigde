param(
    [string]$GamePath,
    [switch]$NoShortcut,
    [ValidateSet('Auto', 'zh-CN', 'en-US')]
    [string]$Language = 'Auto'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
[Console]::InputEncoding = [Text.Encoding]::UTF8
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$OutputEncoding = [Text.Encoding]::UTF8

$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSCommandPath))
$scriptsDirectory = Join-Path $root 'scripts'
$configureScript = Join-Path $scriptsDirectory 'Configure.ps1'
$statePath = Join-Path $root '.installer-state.json'
$fpsConfigPath = Join-Path $root 'fps_config.json'
$errorLogPath = Join-Path $root '.last-install-error.log'
$unlockerPath = Join-Path $root 'unlockfps_nc.exe'
$payloadDirectory = Join-Path $root 'payload'
$optiRootDirectory = Join-Path $payloadDirectory 'OptiScaler'
$optiNestedDirectory = Join-Path $optiRootDirectory 'OptiScaler'
$optiDirectory = if (Test-Path -LiteralPath (Join-Path $optiNestedDirectory 'OptiScaler.dll') -PathType Leaf) {
    $optiNestedDirectory
} else {
    $optiRootDirectory
}
$optiPath = Join-Path $optiDirectory 'OptiScaler.dll'
$bridgePath = Join-Path $payloadDirectory 'Bridge\Dx11FsrBridge.dll'
$antiBlurPath = Join-Path $payloadDirectory 'AntiPlayerMosaic\AntiPlayerMosaic.dll'
$reShadePath = Join-Path $payloadDirectory 'ReShade\ReShade64.dll'
$componentManifestPath = Join-Path $root 'component-manifest.json'
$selfUpdateRepository = 'AizawaHikaru233/genshin_fsr_brigde'
$selfUpdateHelperPath = Join-Path $scriptsDirectory 'Apply-PackageUpdate.ps1'
$script:SelfUpdateStarted = $false
$nonFrameGenerationEdition = Test-Path -LiteralPath (Join-Path $root 'NonFrameGeneration.edition') -PathType Leaf
$shortcutPath = Join-Path ([Environment]::GetFolderPath('Desktop')) '原神.lnk'
$legacyShortcutPath = Join-Path ([Environment]::GetFolderPath('Desktop')) '原神整合版.lnk'

. (Join-Path $scriptsDirectory 'Localization.ps1')
$script:Language = Get-InstallerLanguage -RequestedLanguage $Language -StatePath $statePath
Initialize-InstallerLocalization -Language $script:Language

function Write-Header {
    param([string]$Title)
    Clear-Host
    Write-Host "============================================================" -ForegroundColor DarkGray
    Write-Host "  $Title" -ForegroundColor Cyan
    Write-Host "============================================================" -ForegroundColor DarkGray
}

function Pause-Menu {
    Write-Host ''
    Read-Host '按回车键继续' | Out-Null
}

function Resolve-GameExecutable {
    param([string]$InputPath)
    if ([string]::IsNullOrWhiteSpace($InputPath)) { return $null }
    $cleanPath = $InputPath.Trim().Trim('"')
    if (-not (Test-Path -LiteralPath $cleanPath)) { return $null }
    $resolved = (Resolve-Path -LiteralPath $cleanPath).Path
    if (Test-Path -LiteralPath $resolved -PathType Leaf) {
        if ([IO.Path]::GetFileName($resolved) -in @('YuanShen.exe', 'GenshinImpact.exe')) { return $resolved }
        return $null
    }
    foreach ($name in @('YuanShen.exe', 'GenshinImpact.exe')) {
        $candidate = Join-Path $resolved $name
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return (Resolve-Path -LiteralPath $candidate).Path }
    }
    return $null
}

function Select-GameFolder {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = Convert-InstallerText -Value '请选择包含 YuanShen.exe 或 GenshinImpact.exe 的游戏目录'
    $dialog.ShowNewFolderButton = $false
    if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $dialog.SelectedPath }
    return $null
}

function Read-State {
    if (-not (Test-Path -LiteralPath $statePath -PathType Leaf)) {
        return [pscustomobject]@{ GamePath = $null; FpsTarget = 60; Language = $script:Language }
    }
    try {
        $state = Get-Content -LiteralPath $statePath -Raw -Encoding UTF8 | ConvertFrom-Json
        $fpsTarget = 60
        if ($null -ne $state.FpsTarget -and [int]$state.FpsTarget -gt 0) { $fpsTarget = [int]$state.FpsTarget }
        $savedLanguage = if ([string]$state.Language -in @('zh-CN', 'en-US')) { [string]$state.Language } else { $script:Language }
        return [pscustomobject]@{ GamePath = [string]$state.GamePath; FpsTarget = $fpsTarget; Language = $savedLanguage }
    }
    catch {
        return [pscustomobject]@{ GamePath = $null; FpsTarget = 60; Language = $script:Language }
    }
}

function Save-State {
    param([string]$SelectedGamePath, [int]$FpsTarget, [ValidateSet('zh-CN', 'en-US')][string]$SelectedLanguage = $script:Language)
    [ordered]@{
        GamePath = $SelectedGamePath
        FpsTarget = $FpsTarget
        Language = $SelectedLanguage
    } | ConvertTo-Json | Set-Content -LiteralPath $statePath -Encoding UTF8
}

function Select-GamePath {
    param([string]$InitialPath, [switch]$ForceSelection)
    $current = Resolve-GameExecutable -InputPath $InitialPath
    if ($null -ne $current -and -not $ForceSelection) { return $current }
    while ($true) {
        Write-Header -Title '原神插件管理器 - 选择游戏目录'
        if ($null -ne $current) {
            Write-Host "[√] 当前游戏目录: $(Split-Path -Parent $current)" -ForegroundColor Green
            Write-Host "    游戏程序: $current" -ForegroundColor DarkGray
            Write-Host ''
            Write-Host '输入新路径，或直接按回车打开目录选择窗口。'
        }
        else {
            Write-Host '[×] 尚未选择有效的原神游戏目录' -ForegroundColor Yellow
            Write-Host ''
            Write-Host '输入原神安装目录或游戏程序的完整路径。'
        }
        Write-Host '不输入路径直接按回车，可打开目录选择窗口。' -ForegroundColor DarkGray
        Write-Host '输入 0 退出。' -ForegroundColor DarkGray
        $inputValue = Read-Host '游戏路径'
        if ($inputValue.Trim() -eq '0') { return $null }
        if ([string]::IsNullOrWhiteSpace($inputValue)) {
            $selectedFolder = Select-GameFolder
            if ($null -eq $selectedFolder) { continue }
            $candidate = Resolve-GameExecutable -InputPath $selectedFolder
        }
        else {
            $candidate = Resolve-GameExecutable -InputPath $inputValue
        }
        if ($null -ne $candidate) { return $candidate }
        Write-Host '该路径中没有找到 YuanShen.exe 或 GenshinImpact.exe。' -ForegroundColor Red
        Pause-Menu
    }
}

function Get-FpsConfig {
    if (-not (Test-Path -LiteralPath $fpsConfigPath -PathType Leaf)) { return $null }
    try { return Get-Content -LiteralPath $fpsConfigPath -Raw -Encoding UTF8 | ConvertFrom-Json } catch { return $null }
}

function Set-JsonPropertyValue {
    param(
        [object]$Object,
        [string]$Name,
        [object]$Value
    )
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        $Object | Add-Member -MemberType NoteProperty -Name $Name -Value $Value
        return $true
    }
    if ($property.Value -ne $Value) {
        $property.Value = $Value
        return $true
    }
    return $false
}

function Set-IniPathValue {
    param(
        [string]$Path,
        [string]$Section,
        [string]$Key,
        [string]$Value
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $lines = [Collections.Generic.List[string]]::new()
    foreach ($line in @(Get-Content -LiteralPath $Path -Encoding UTF8)) { $lines.Add([string]$line) }
    $sectionStart = -1
    $sectionEnd = $lines.Count
    for ($index = 0; $index -lt $lines.Count; $index++) {
        if ($lines[$index].Trim() -ieq "[$Section]") {
            $sectionStart = $index
            for ($next = $index + 1; $next -lt $lines.Count; $next++) {
                if ($lines[$next].Trim() -match '^\[.+\]$') { $sectionEnd = $next; break }
            }
            break
        }
    }
    if ($sectionStart -lt 0) {
        if ($lines.Count -gt 0 -and -not [string]::IsNullOrWhiteSpace($lines[$lines.Count - 1])) { $lines.Add('') }
        $lines.Add("[$Section]")
        $lines.Add("$Key = $Value")
        [IO.File]::WriteAllLines($Path, $lines, [Text.UTF8Encoding]::new($false))
        return $true
    }
    for ($index = $sectionStart + 1; $index -lt $sectionEnd; $index++) {
        if ($lines[$index] -match ('^\s*' + [regex]::Escape($Key) + '\s*=')) {
            $expected = "$Key = $Value"
            if ($lines[$index] -ceq $expected) { return $false }
            $lines[$index] = $expected
            [IO.File]::WriteAllLines($Path, $lines, [Text.UTF8Encoding]::new($false))
            return $true
        }
    }
    $lines.Insert($sectionEnd, "$Key = $Value")
    [IO.File]::WriteAllLines($Path, $lines, [Text.UTF8Encoding]::new($false))
    return $true
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
    $riskyPaths = [Collections.Generic.List[string]]::new()
    if (Test-PathCompatibilityRisk -Path $gameDirectory) { $riskyPaths.Add("游戏目录: $gameDirectory") }
    if (Test-PathCompatibilityRisk -Path $root) { $riskyPaths.Add("插件目录: $root") }
    if ($riskyPaths.Count -eq 0) { return $false }

    Write-Host ''
    Write-Host '路径兼容性提醒：检测到游戏或插件路径包含中文或特殊符号。' -ForegroundColor Yellow
    foreach ($entry in $riskyPaths) {
        Write-Host "  $entry" -ForegroundColor DarkYellow
    }
    Write-Host '若遇到无法注入、插件不加载或日志目录乱码，建议将游戏和插件移动到仅包含英文、数字、下划线和短横线的路径。' -ForegroundColor DarkGray
    return $true
}

function Repair-RuntimePaths {
    param([string]$SelectedGamePath)
    $defaultConfigDirectory = Join-Path $payloadDirectory 'default_config'
    $config = Get-FpsConfig
    if ($null -ne $config) {
        $changed = -not [string]::Equals([string]$config.GamePath, $SelectedGamePath, [StringComparison]::OrdinalIgnoreCase)
        if (Set-JsonPropertyValue -Object $config -Name 'GamePath' -Value $SelectedGamePath) { $changed = $true }
        if ($null -ne $config.DllList) {
            $managedPaths = @{
                'dx11fsrbridge.dll' = $bridgePath
                'optiscaler.dll' = $optiPath
                'antiplayermosaic.dll' = $antiBlurPath
                'reshade64.dll' = $reShadePath
            }
            $repairedList = [Collections.Generic.List[string]]::new()
            $seenPaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
            foreach ($entry in @($config.DllList)) {
                $configuredPath = [string]$entry
                $fileName = try { [IO.Path]::GetFileName($configuredPath).ToLowerInvariant() } catch { '' }
                if ($managedPaths.ContainsKey($fileName) -and (Test-Path -LiteralPath $managedPaths[$fileName] -PathType Leaf)) {
                    $repairedPath = [IO.Path]::GetFullPath($managedPaths[$fileName])
                    if (-not [string]::Equals($configuredPath, $repairedPath, [StringComparison]::OrdinalIgnoreCase)) { $changed = $true }
                    if ($seenPaths.Add($repairedPath)) { $repairedList.Add($repairedPath) }
                }
                elseif ($seenPaths.Add($configuredPath)) {
                    $repairedList.Add($configuredPath)
                }
                else {
                    $changed = $true
                }
            }
            $config.DllList = @($repairedList)
        }
        $hdrEnabled = Test-ConfiguredDll -Config $config -Path $reShadePath
        if (Set-JsonPropertyValue -Object $config -Name 'UseHDR' -Value $hdrEnabled) { $changed = $true }
        if ($changed) { Save-FpsConfig -Config $config }
    }

    $optiIni = Join-Path $optiDirectory 'OptiScaler.ini'
    if (Test-Path -LiteralPath $optiPath -PathType Leaf) {
        if (-not (Test-Path -LiteralPath $optiIni -PathType Leaf)) {
            $optiTemplate = Join-Path $defaultConfigDirectory 'OptiScaler.ini'
            if (-not (Test-Path -LiteralPath $optiTemplate -PathType Leaf)) {
                throw "缺少 OptiScaler 官方默认配置模板: $optiTemplate"
            }
            Copy-Item -LiteralPath $optiTemplate -Destination $optiIni -Force
        }
        Set-IniPathValue -Path $optiIni -Section 'Libraries' -Key 'OptiDllPath' `
            -Value ([IO.Path]::GetFullPath($optiDirectory).TrimEnd('\')) | Out-Null
        foreach ($libraryKey in @(
            'NvngxDlssPath',
            'FfxDx12Path',
            'FfxDx12SRPath',
            'FfxDx12FGPath',
            'XeSSPath',
            'XeFGPath',
            'XeLLPath',
            'XeSSDx11Path'
        )) {
            Set-IniPathValue -Path $optiIni -Section 'Libraries' -Key $libraryKey -Value 'auto' | Out-Null
        }
        Set-IniPathValue -Path $optiIni -Section 'Log' -Key 'LogToFile' -Value 'true' | Out-Null
        Set-IniPathValue -Path $optiIni -Section 'Log' -Key 'LogLevel' -Value '2' | Out-Null
        Set-IniPathValue -Path $optiIni -Section 'Log' -Key 'SingleFile' -Value 'true' | Out-Null
        Set-IniPathValue -Path $optiIni -Section 'Log' -Key 'LogFileName' -Value 'OptiScaler.log' | Out-Null
        Set-IniPathValue -Path $optiIni -Section 'Log' -Key 'LogAsync' -Value 'false' | Out-Null
        Set-IniPathValue -Path $optiIni -Section 'Log' -Key 'LogAsyncThreads' -Value '1' | Out-Null
        if ($nonFrameGenerationEdition) {
            Set-IniPathValue -Path $optiIni -Section 'FrameGen' -Key 'Enabled' -Value 'false' | Out-Null
            Set-IniPathValue -Path $optiIni -Section 'FrameGen' -Key 'FGInput' -Value 'nofg' | Out-Null
            Set-IniPathValue -Path $optiIni -Section 'FrameGen' -Key 'FGOutput' -Value 'nofg' | Out-Null
        }
    }

    $gameDirectory = Split-Path -Parent $SelectedGamePath
    $reShadeDirectory = Split-Path -Parent $reShadePath
    $reShadeIni = Join-Path $gameDirectory 'ReShade.ini'
    $reShadePreset = Join-Path $gameDirectory 'ReShadePreset.ini'
    if (Test-Path -LiteralPath $reShadePath -PathType Leaf) {
        $redirectIni = $false
        if (Test-Path -LiteralPath $reShadeIni -PathType Leaf) {
            $existingIniText = Get-Content -LiteralPath $reShadeIni -Raw -Encoding UTF8
            $redirectIni = $existingIniText -match '(?im)^\s*BasePath\s*='
        }
        if ((-not (Test-Path -LiteralPath $reShadeIni -PathType Leaf)) -or $redirectIni) {
            $legacyIni = Join-Path $reShadeDirectory 'ReShade.ini'
            $sourceIni = if (Test-Path -LiteralPath $legacyIni -PathType Leaf) {
                $legacyIni
            } else {
                Join-Path $defaultConfigDirectory 'ReShade.ini'
            }
            if (Test-Path -LiteralPath $sourceIni -PathType Leaf) {
                Copy-Item -LiteralPath $sourceIni -Destination $reShadeIni -Force
            }
        }
        if (-not (Test-Path -LiteralPath $reShadePreset -PathType Leaf)) {
            $legacyPreset = Join-Path $reShadeDirectory 'ReShadePreset.ini'
            $sourcePreset = if (Test-Path -LiteralPath $legacyPreset -PathType Leaf) {
                $legacyPreset
            } else {
                Join-Path $defaultConfigDirectory 'ReShadePreset.ini'
            }
            if (Test-Path -LiteralPath $sourcePreset -PathType Leaf) {
                Copy-Item -LiteralPath $sourcePreset -Destination $reShadePreset -Force
            }
        }
    }
    if ((Test-Path -LiteralPath $reShadePath -PathType Leaf) -and (Test-Path -LiteralPath $reShadeIni -PathType Leaf)) {
        $shaderDirectory = Join-Path $reShadeDirectory 'reshade-shaders'
        Set-IniPathValue -Path $reShadeIni -Section 'ADDON' -Key 'AddonPath' `
            -Value ([IO.Path]::GetFullPath((Join-Path $shaderDirectory 'Addons'))) | Out-Null
        Set-IniPathValue -Path $reShadeIni -Section 'GENERAL' -Key 'EffectSearchPaths' `
            -Value ([IO.Path]::GetFullPath((Join-Path $shaderDirectory 'Shaders'))) | Out-Null
        Set-IniPathValue -Path $reShadeIni -Section 'GENERAL' -Key 'TextureSearchPaths' `
            -Value ([IO.Path]::GetFullPath((Join-Path $shaderDirectory 'Textures'))) | Out-Null
        Set-IniPathValue -Path $reShadeIni -Section 'GENERAL' -Key 'PresetPath' -Value $reShadePreset | Out-Null
        Set-IniPathValue -Path $reShadeIni -Section 'SCREENSHOT' -Key 'SavePath' `
            -Value (Join-Path $gameDirectory 'Screenshots') | Out-Null
        Remove-Item -LiteralPath (Join-Path $reShadeDirectory 'ReShade.ini'), `
            (Join-Path $reShadeDirectory 'ReShadePreset.ini') -Force -ErrorAction SilentlyContinue
    }
}

function Reset-AllPluginConfigurations {
    param([string]$SelectedGamePath)
    Write-Header -Title '恢复所有插件默认配置'
    Write-Host '这会清除所有插件的当前设置，并恢复到首次运行状态。' -ForegroundColor Yellow
    Write-Host '游戏路径和当前插件加载状态会保留。' -ForegroundColor DarkGray
    $confirmation = (Read-Host '输入 Y 确认，其他输入返回').Trim()
    if ($confirmation.ToUpperInvariant() -ne 'Y') { return $false }

    $arguments = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $configureScript,
        '-ResetPluginConfigsOnly', '-GamePath', $SelectedGamePath, '-NonInteractive', '-Language', $script:Language
    )
    $backendOutput = @(& powershell.exe @arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        $backendOutput | Set-Content -LiteralPath $errorLogPath -Encoding UTF8
        Write-Host '恢复默认配置失败，请重试。' -ForegroundColor Red
        return $false
    }
    Remove-Item -LiteralPath $errorLogPath -Force -ErrorAction SilentlyContinue
    Save-State -SelectedGamePath $SelectedGamePath -FpsTarget 60
    Write-Host ''
    Write-Host '所有插件配置已恢复到首次运行状态。' -ForegroundColor Green
    Pause-Menu
    return $true
}

function Test-ConfiguredDll {
    param([object]$Config, [string]$Path)
    if ($null -eq $Config -or $null -eq $Config.DllList) { return $false }
    foreach ($configuredPath in @($Config.DllList)) {
        if ([string]::Equals([IO.Path]::GetFullPath([string]$configuredPath), [IO.Path]::GetFullPath($Path), [StringComparison]::OrdinalIgnoreCase)) { return $true }
    }
    return $false
}

function Get-ModuleState {
    param([string]$SelectedGamePath)
    $config = Get-FpsConfig
    $gameMatches = $null -ne $config -and [string]::Equals([string]$config.GamePath, $SelectedGamePath, [StringComparison]::OrdinalIgnoreCase)
    $unlockerInstalled = Test-Path -LiteralPath $unlockerPath -PathType Leaf
    return [ordered]@{
        Unlocker = $unlockerInstalled -and $gameMatches
        OptiScaler = $unlockerInstalled -and $gameMatches -and (Test-Path -LiteralPath $optiPath -PathType Leaf) -and (Test-ConfiguredDll -Config $config -Path $bridgePath) -and (Test-ConfiguredDll -Config $config -Path $optiPath)
        AntiBlur = $unlockerInstalled -and $gameMatches -and (Test-ConfiguredDll -Config $config -Path $antiBlurPath)
        HDR = $unlockerInstalled -and $gameMatches -and (Test-ConfiguredDll -Config $config -Path $reShadePath) -and (Test-Path -LiteralPath (Join-Path (Split-Path -Parent $SelectedGamePath) 'ReShade.ini') -PathType Leaf)
    }
}

function Write-ModuleLine {
    param([int]$Number, [string]$Name, [bool]$Installed, [string]$Path, [string]$Extra = '')
    $status = if ($Installed) { '已安装' } else { '未安装' }
    $color = if ($Installed) { 'Green' } else { 'DarkGray' }
    $suffix = if ([string]::IsNullOrWhiteSpace($Extra)) { '' } else { "  $Extra" }
    Write-Host -NoNewline ("  {0}. " -f $Number) -ForegroundColor $color
    Write-Host -NoNewline $Name -ForegroundColor $color
    Write-Host "  [$status]$suffix" -ForegroundColor $color
    Write-Host "     $Path" -ForegroundColor DarkGray
}

function Get-DisplayWidth {
    param([string]$Text)
    $width = 0
    foreach ($character in $Text.ToCharArray()) {
        $code = [int]$character
        if (($code -ge 0x1100 -and $code -le 0x115F) -or
            ($code -ge 0x2E80 -and $code -le 0xA4CF) -or
            ($code -ge 0xAC00 -and $code -le 0xD7A3) -or
            ($code -ge 0xF900 -and $code -le 0xFAFF) -or
            ($code -ge 0xFE10 -and $code -le 0xFE6F) -or
            ($code -ge 0xFF01 -and $code -le 0xFF60) -or
            ($code -ge 0xFFE0 -and $code -le 0xFFE6)) { $width += 2 }
        else { $width++ }
    }
    return $width
}

function Split-DisplayText {
    param([string]$Text, [int]$Width)
    if ([string]::IsNullOrEmpty($Text)) { return @('') }
    if ($Text.Contains("`n")) {
        $explicitLines = [Collections.Generic.List[string]]::new()
        foreach ($part in ($Text -split "`r?`n")) {
            foreach ($wrappedPart in @(Split-DisplayText -Text $part -Width $Width)) { $explicitLines.Add($wrappedPart) }
        }
        return @($explicitLines)
    }
    $lines = [Collections.Generic.List[string]]::new()
    $builder = [Text.StringBuilder]::new()
    $currentWidth = 0
    foreach ($character in $Text.ToCharArray()) {
        $characterWidth = Get-DisplayWidth -Text ([string]$character)
        if ($currentWidth + $characterWidth -gt $Width -and $builder.Length -gt 0) {
            $lines.Add($builder.ToString())
            $builder.Clear() | Out-Null
            $currentWidth = 0
        }
        $builder.Append($character) | Out-Null
        $currentWidth += $characterWidth
    }
    if ($builder.Length -gt 0) { $lines.Add($builder.ToString()) }
    return @($lines)
}

function Pad-DisplayText {
    param([string]$Text, [int]$Width)
    $padding = [Math]::Max(0, $Width - (Get-DisplayWidth -Text $Text))
    return $Text + (' ' * $padding)
}

function Write-CatalogRow {
    param([string]$Id, [string]$Name, [string]$Author, [string]$Version, [string]$Status, [switch]$Header)
    $windowWidth = 120
    try { if ([Console]::WindowWidth -gt 0) { $windowWidth = [Console]::WindowWidth } } catch { }
    $availableWidth = [Math]::Min(130, [Math]::Max(48, $windowWidth - 2))
    $columnGap = 2
    $contentWidth = $availableWidth - ($columnGap * 4)
    $idWidth = 8
    $remainingWidth = $contentWidth - $idWidth
    $nameWidth = [Math]::Max(8, [Math]::Floor($remainingWidth * 0.32))
    $authorWidth = [Math]::Max(8, [Math]::Floor($remainingWidth * 0.22))
    $versionWidth = [Math]::Max(8, [Math]::Floor($remainingWidth * 0.27))
    $statusWidth = $remainingWidth - $nameWidth - $authorWidth - $versionWidth
    $idLines = @(Split-DisplayText -Text $Id -Width $idWidth)
    $nameLines = @(Split-DisplayText -Text $Name -Width $nameWidth)
    $authorLines = @(Split-DisplayText -Text $Author -Width $authorWidth)
    $versionLines = @(Split-DisplayText -Text $Version -Width $versionWidth)
    $statusLines = @(Split-DisplayText -Text $Status -Width $statusWidth)
    $lineCounts = @($idLines.Count, $nameLines.Count, $authorLines.Count, $versionLines.Count, $statusLines.Count)
    $lineCount = ($lineCounts | Measure-Object -Maximum).Maximum
    for ($index = 0; $index -lt $lineCount; $index++) {
        $idPart = if ($index -lt $idLines.Count) { $idLines[$index] } else { '' }
        $namePart = if ($index -lt $nameLines.Count) { $nameLines[$index] } else { '' }
        $authorPart = if ($index -lt $authorLines.Count) { $authorLines[$index] } else { '' }
        $versionPart = if ($index -lt $versionLines.Count) { $versionLines[$index] } else { '' }
        $statusPart = if ($index -lt $statusLines.Count) { $statusLines[$index] } else { '' }
        $prefix = '  ' +
            (Pad-DisplayText -Text $idPart -Width $idWidth) + (' ' * $columnGap) +
            (Pad-DisplayText -Text $namePart -Width $nameWidth) + (' ' * $columnGap) +
            (Pad-DisplayText -Text $authorPart -Width $authorWidth) + (' ' * $columnGap)
        Write-Host -NoNewline $prefix
        Write-Host -NoNewline (Pad-DisplayText -Text $versionPart -Width $versionWidth) -ForegroundColor $(if ($Header) { 'DarkGray' } else { 'Green' })
        Write-Host -NoNewline (' ' * $columnGap)
        $statusColor = if ($Header) { 'DarkGray' } elseif ($Status -eq '已安装') { 'Green' } else { 'DarkGray' }
        Write-Host $statusPart -ForegroundColor $statusColor
    }
}

function Get-FileVersionLabel {
    param([string]$Path, [string]$Fallback = '未安装')
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $Fallback }
    $version = (Get-Item -LiteralPath $Path).VersionInfo.FileVersion
    if ([string]::IsNullOrWhiteSpace($version)) { return $Fallback }
    return "v$version"
}

function Get-ComponentVersionLabel {
    param([string]$Name, [string]$Fallback = '未知')
    if (-not (Test-Path -LiteralPath $componentManifestPath -PathType Leaf)) { return $Fallback }
    try {
        $manifest = Get-Content -LiteralPath $componentManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
        $component = @($manifest | Where-Object { $_.Name -eq $Name } | Select-Object -First 1)
        if ($component.Count -ne 1 -or [string]::IsNullOrWhiteSpace([string]$component[0].Version)) { return $Fallback }
        return "v$([string]$component[0].Version)"
    }
    catch {
        return $Fallback
    }
}

function Write-InstallCatalog {
    param([string]$SelectedGamePath)
    $renoDxPath = Join-Path $root 'payload\ReShade\reshade-shaders\Addons\renodx-genshin.addon64'
    $unlockerVersion = Get-FileVersionLabel -Path $unlockerPath
    $optiVersion = Get-FileVersionLabel -Path $optiPath
    $bridgeVersion = Get-ComponentVersionLabel -Name 'Dx11FsrBridge.dll' -Fallback (Get-FileVersionLabel -Path $bridgePath)
    $antiVersion = Get-ComponentVersionLabel -Name 'AntiPlayerMosaic.dll' -Fallback (Get-FileVersionLabel -Path $antiBlurPath)
    $reShadeVersion = Get-FileVersionLabel -Path $reShadePath
    $renoDxVersion = Get-FileVersionLabel -Path $renoDxPath
    $managerVersion = Get-ComponentVersionLabel -Name 'Dx11FsrBridge.dll' -Fallback '未知'
    $state = Get-ModuleState -SelectedGamePath $SelectedGamePath
    $unlockerStatus = if ($state.Unlocker) { '已安装' } else { '未安装' }
    $optiStatus = if ($state.OptiScaler) { '已安装' } else { '未安装' }
    $antiStatus = if ($state.AntiBlur) { '已安装' } else { '未安装' }
    $hdrStatus = if ($state.HDR) { '已安装' } else { '未安装' }
    Write-Host ''
    Write-CatalogRow -Id '模块 ID' -Name '插件名' -Author '作者' -Version '当前版本' -Status '安装状态' -Header
    Write-CatalogRow -Id '------' -Name '--------------------' -Author '----------------' -Version '--------------------' -Status '------' -Header
    Write-CatalogRow -Id '1.' -Name 'FPS Unlocker' -Author '34736384' -Version $unlockerVersion -Status $unlockerStatus
    Write-CatalogRow -Id '2.' -Name 'FSR Bridge + OptiScaler' -Author 'シリアCelia / OptiScaler' -Version "Bridge $bridgeVersion`nOptiScaler $optiVersion" -Status $optiStatus
    Write-CatalogRow -Id '3.' -Name '反虚化 / 隐藏 UID' -Author 'シリアCelia' -Version $antiVersion -Status $antiStatus
    Write-CatalogRow -Id '4.' -Name 'ReShade + RenoDX HDR' -Author 'crosire / Bilibili UID 3461582765951639' -Version "ReShade $reShadeVersion`nRenoDX $renoDxVersion" -Status $hdrStatus
    Write-CatalogRow -Id '5.' -Name '管理脚本 / 发行资源' -Author 'シリアCelia' -Version $managerVersion -Status '已安装'
}

function Select-ModuleSet {
    param([string]$ActionName, [switch]$IncludeFoundation, [switch]$IncludeSelfUpdate)
    $allowed = [Collections.Generic.List[int]]::new()
    if ($IncludeFoundation) { $allowed.Add(1) }
    foreach ($id in @(2, 3, 4)) { $allowed.Add($id) }
    if ($IncludeSelfUpdate) { $allowed.Add(5) }
    while ($true) {
        Write-Host "请输入需要${ActionName}的模块 ID" -ForegroundColor Yellow
        Write-Host ''
        Write-Host "$ActionName 模块："
        Write-Host '  A. 全部可选模块（默认，直接回车）'
        Write-Host '  0. 返回上一层'
        $choice = (Read-Host '请输入选项').Trim().ToUpperInvariant()
        if ([string]::IsNullOrWhiteSpace($choice) -or $choice -eq 'A') {
            return @($allowed)
        }
        if ($choice -eq '0') { return @() }
        $selectedId = 0
        if ([int]::TryParse($choice, [ref]$selectedId) -and $allowed.Contains($selectedId)) { return @($selectedId) }
        Write-Host '无效选项。' -ForegroundColor Red
    }
}

function Select-LocalInstallPath {
    param([string]$Name, [string]$Filter)
    Write-Host ''
    Write-Host "请输入 $Name 安装文件或解压目录。" -ForegroundColor Yellow
    Write-Host '直接按回车可打开文件选择窗口，输入 0 返回。'
    $inputPath = (Read-Host '请输入选项').Trim().Trim('"')
    if ($inputPath -eq '0') { return $null }
    if ([string]::IsNullOrWhiteSpace($inputPath)) {
        Add-Type -AssemblyName System.Windows.Forms
        $dialog = New-Object System.Windows.Forms.OpenFileDialog
        $dialog.Title = Convert-InstallerText -Value "选择 $Name 安装文件"
        $dialog.Filter = $Filter
        $dialog.CheckFileExists = $true
        $dialog.CheckPathExists = $true
        if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
            Write-Host '未找到目录或文件。' -ForegroundColor Red
            Start-Sleep -Milliseconds 900
            return $null
        }
        $inputPath = $dialog.FileName
    }
    if (-not (Test-Path -LiteralPath $inputPath)) {
        Write-Host '未找到目录或文件。' -ForegroundColor Red
        Start-Sleep -Milliseconds 900
        return $null
    }
    return (Resolve-Path -LiteralPath $inputPath).Path
}

function Get-LatestRelease {
    param([string]$Repository, [string]$AssetPattern)
    try {
        $release = Invoke-RestMethod -Headers @{ 'User-Agent' = 'GenshinOneClick-Installer' } -Uri "https://api.github.com/repos/$Repository/releases/latest"
        $asset = $release.assets | Where-Object { $_.name -match $AssetPattern } | Select-Object -First 1
        if ($null -eq $asset) { return $null }
        return [pscustomobject]@{ Tag = [string]$release.tag_name; Digest = [string]$asset.digest }
    }
    catch { return $null }
}

function Get-CurrentPackageVersion {
    if (-not (Test-Path -LiteralPath $componentManifestPath -PathType Leaf)) { return '0.0.0' }
    try {
        $manifest = Get-Content -LiteralPath $componentManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
        $bridge = @($manifest | Where-Object { $_.Name -eq 'Dx11FsrBridge.dll' } | Select-Object -First 1)
        if ($bridge.Count -eq 1 -and [string]$bridge[0].Version -match '^\d+(\.\d+)+$') { return [string]$bridge[0].Version }
    }
    catch { }
    return '0.0.0'
}

function Start-PackageSelfUpdate {
    Write-Host ''
    Write-Host '正在检查管理脚本和发行资源更新...' -ForegroundColor Cyan
    try {
        $release = Invoke-RestMethod -Headers @{ 'User-Agent' = 'GenshinOneClick-SelfUpdater' } `
            -Uri "https://api.github.com/repos/$selfUpdateRepository/releases/latest"
    }
    catch {
        Write-Host "检查脚本更新失败: $($_.Exception.Message)" -ForegroundColor Red
        return $false
    }
    $assets = @($release.assets | Where-Object {
        $_.name -match '^(GenshinFSRBridge\.Lite_v|原神解帧FSR插件包Lite_|FSR\.Lite_v).+\.zip$'
    })
    if ($assets.Count -eq 0) {
        Write-Host '最新 Release 中没有找到 FPS Unlock Lite 更新包。' -ForegroundColor Red
        return $false
    }
    $asset = @($assets | Sort-Object @{ Expression = {
        if ($_.name -like 'GenshinFSRBridge.Lite_v*') { 0 }
        elseif ($_.name -like '原神解帧FSR插件包Lite_*') { 1 }
        else { 2 }
    } } | Select-Object -First 1)[0]
    $currentVersion = Get-CurrentPackageVersion
    $latestVersion = ([string]$release.tag_name).TrimStart('v')
    if ($latestVersion -match '^\d+(\.\d+)+$' -and
        [version]$currentVersion -ge [version]$latestVersion) {
        Write-Host "管理脚本和发行资源已经是最新版本 v$currentVersion。" -ForegroundColor Green
        return $false
    }
    if (-not (Test-Path -LiteralPath $selfUpdateHelperPath -PathType Leaf)) {
        Write-Host '当前包缺少自更新替换程序，请手动下载最新发布包。' -ForegroundColor Red
        return $false
    }
    $temporaryDirectory = Join-Path ([IO.Path]::GetTempPath()) ('GenshinOneClick-SelfUpdate-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $temporaryDirectory | Out-Null
    try {
        $packagePath = Join-Path $temporaryDirectory ([string]$asset.name)
        Invoke-WebRequest -UseBasicParsing -Headers @{ 'User-Agent' = 'GenshinOneClick-SelfUpdater' } `
            -Uri ([string]$asset.browser_download_url) -OutFile $packagePath
        if ([string]$asset.digest -match '^sha256:(.+)$') {
            $actualHash = (Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash
            if (-not [string]::Equals($actualHash, $matches[1], [StringComparison]::OrdinalIgnoreCase)) {
                throw (Convert-InstallerText -Value '管理脚本更新包 SHA-256 校验失败。')
            }
        }
        $expanded = Join-Path $temporaryDirectory 'expanded'
        Expand-Archive -LiteralPath $packagePath -DestinationPath $expanded -Force
        foreach ($required in @(
            'Installer.ps1', 'scripts\Configure.ps1', 'scripts\Localization.ps1', 'scripts\ReShadeResources.ps1', 'scripts\Apply-PackageUpdate.ps1',
            'payload\default_config\OptiScaler.ini', 'payload\default_config\OptiScaler-UpscalingFiles.json',
            'payload\default_config\ReShade.ini', 'payload\default_config\ReShadePreset.ini',
            'payload\Bridge\Dx11FsrBridge.dll'
        )) {
            if (-not (Test-Path -LiteralPath (Join-Path $expanded $required) -PathType Leaf)) { throw (Convert-InstallerText -Value "更新包缺少文件: $required") }
        }
        foreach ($forbidden in @('unlockfps_nc.exe', 'OptiScaler.dll', 'nvngx_dlss.dll')) {
            if (Get-ChildItem -LiteralPath $expanded -Recurse -File | Where-Object { $_.Name -ieq $forbidden }) {
                throw (Convert-InstallerText -Value "更新包包含不应内置的第三方组件: $forbidden")
            }
        }
        $helperCopy = Join-Path $temporaryDirectory 'Apply-PackageUpdate.ps1'
        Copy-Item -LiteralPath $selfUpdateHelperPath -Destination $helperCopy -Force
        Start-Process -FilePath 'powershell.exe' -ArgumentList @(
            '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $helperCopy + '"'),
            '-ParentProcessId', $PID,
            '-SourceDirectory', ('"' + $expanded + '"'),
            '-TargetDirectory', ('"' + $root + '"'),
            '-RelaunchScript', ('"' + (Join-Path $root 'Installer.ps1') + '"')
        ) | Out-Null
        Write-Host "已下载管理脚本 v$latestVersion，当前窗口关闭后自动替换并重新打开。" -ForegroundColor Green
        $script:SelfUpdateStarted = $true
        return $true
    }
    catch {
        Write-Host "管理脚本更新失败: $($_.Exception.Message)" -ForegroundColor Red
        if (Test-Path -LiteralPath $temporaryDirectory) { Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force -ErrorAction SilentlyContinue }
        return $false
    }
}

function Test-ComponentUpdate {
    param([string]$Name, [string]$Repository, [string]$AssetPattern, [string]$LocalPath, [ValidateSet('Hash', 'Version')] [string]$CompareMode, [switch]$NoConfirm, [switch]$NoPause)
    Write-Host ''
    Write-Host '正在检查更新，请稍候...' -ForegroundColor Cyan
    $release = Get-LatestRelease -Repository $Repository -AssetPattern $AssetPattern
    if ($null -eq $release) {
        Write-Host '检查更新失败，请稍后重试。' -ForegroundColor Red
        if (-not $NoPause) { Pause-Menu }
        return 'Retry'
    }
    if (-not (Test-Path -LiteralPath $LocalPath -PathType Leaf)) {
        Write-Host "$Name 尚未安装。" -ForegroundColor Yellow
        if (-not $NoPause) { Pause-Menu }
        return 'Retry'
    }
    $isCurrent = $false
    if ($CompareMode -eq 'Hash' -and $release.Digest -match '^sha256:(.+)$') {
        $isCurrent = [string]::Equals((Get-FileHash -LiteralPath $LocalPath -Algorithm SHA256).Hash, $matches[1], [StringComparison]::OrdinalIgnoreCase)
    }
    elseif ($CompareMode -eq 'Version') {
        $localVersion = (Get-Item -LiteralPath $LocalPath).VersionInfo.FileVersion
        $latestVersion = $release.Tag.TrimStart('v')
        $isCurrent = $localVersion.StartsWith($latestVersion, [StringComparison]::OrdinalIgnoreCase)
    }
    if ($isCurrent) {
        Write-Host '当前已经是最新版本。' -ForegroundColor Green
        if (-not $NoPause) { Pause-Menu }
        return 'Existing'
    }
    Write-Host "发现新版本 $($release.Tag)。" -ForegroundColor Yellow
    if ($NoConfirm) { return 'Auto' }
    $answer = (Read-Host '是否现在安装？直接回车确认，输入 n 取消').Trim().ToLowerInvariant()
    if ([string]::IsNullOrWhiteSpace($answer) -or $answer -in @('y', 'yes')) { return 'Auto' }
    return 'Retry'
}

function Select-ComponentSource {
    param(
        [string]$Name,
        [string]$Repository,
        [string]$AssetPattern,
        [string]$LocalPath,
        [ValidateSet('Hash', 'Version')] [string]$CompareMode,
        [string]$FileFilter
    )
    if (Test-Path -LiteralPath $LocalPath -PathType Leaf) {
        return [pscustomobject]@{ Mode = 'Existing'; Path = $null }
    }
    while ($true) {
        Write-Host ''
        Write-Host "$Name 安装方式：" -ForegroundColor Yellow
        Write-Host '  1. 联网安装'
        Write-Host '  2. 手动安装'
        Write-Host '  0. 返回上一层'
        $choice = (Read-Host '请输入选项').Trim()
        if ($choice -eq '1') { return [pscustomobject]@{ Mode = 'Auto'; Path = $null } }
        if ($choice -eq '2') {
            $selectedPath = Select-LocalInstallPath -Name $Name -Filter $FileFilter
            if ($null -ne $selectedPath) { return [pscustomobject]@{ Mode = 'Manual'; Path = $selectedPath } }
            continue
        }
        if ($choice -eq '0') { return $null }
        Write-Host '无效选项。' -ForegroundColor Red
    }
}

function Select-ReShadeSource {
    $bundledEffectsAvailable =
        (Test-Path -LiteralPath (Join-Path $payloadDirectory 'ReShade\reshade-shaders\Shaders\FakeHDR.fx') -PathType Leaf) -or
        (Test-Path -LiteralPath (Join-Path $payloadDirectory 'ReShade\reshade-shaders\Shaders\lilium__tone_mapping.fx') -PathType Leaf)
    Write-Host ''
    Write-Host 'ReShade 与效果库安装来源：' -ForegroundColor Yellow
    Write-Host '  1. 从 ReShade 及效果作者的官方来源下载（推荐）'
    if ($bundledEffectsAvailable) {
        Write-Host '  2. 使用安装包内置 ReShade 与效果库（支持离线安装）'
    }
    else {
        Write-Host '  2. 仅使用安装包内置 ReShade + RenoDX（不含效果库）'
    }
    Write-Host '  0. 返回上一层'
    while ($true) {
        $choice = (Read-Host '请输入选项，直接回车使用官方下载').Trim()
        if ([string]::IsNullOrWhiteSpace($choice) -or $choice -eq '1') { return 'Auto' }
        if ($choice -eq '2') { return 'Bundled' }
        if ($choice -eq '0') { return $null }
        Write-Host '无效选项。' -ForegroundColor Red
    }
}

function Get-ConfiguredPluginState {
    $config = Get-FpsConfig
    return [ordered]@{
        OptiScaler = (Test-Path -LiteralPath $optiPath -PathType Leaf) -and (Test-ConfiguredDll -Config $config -Path $bridgePath) -and (Test-ConfiguredDll -Config $config -Path $optiPath)
        AntiBlur = Test-ConfiguredDll -Config $config -Path $antiBlurPath
        HDR = Test-ConfiguredDll -Config $config -Path $reShadePath
    }
}

function Invoke-FoundationSetup {
    param([string]$SelectedGamePath, [ref]$FpsTarget)
    $config = Get-FpsConfig
    $firstRun = $null -eq $config
    $unlockerWasMissing = -not (Test-Path -LiteralPath $unlockerPath -PathType Leaf)
    $ready = -not $unlockerWasMissing -and $null -ne $config -and [string]::Equals([string]$config.GamePath, $SelectedGamePath, [StringComparison]::OrdinalIgnoreCase)
    if ($ready) { return $true }
    Write-Header -Title '安装必需组件'
    if ($unlockerWasMissing) {
        $source = Select-ComponentSource `
            -Name 'FPS Unlocker' `
            -Repository '34736384/genshin-fps-unlock' `
            -AssetPattern '^unlockfps_nc\.exe$' `
            -LocalPath $unlockerPath `
            -CompareMode 'Hash' `
            -FileFilter 'FPS Unlocker (*.exe;*.zip)|*.exe;*.zip|所有文件 (*.*)|*.*'
    }
    else { $source = [pscustomobject]@{ Mode = 'Existing'; Path = $null } }
    if ($null -eq $source) { return $false }
    $plugins = Get-ConfiguredPluginState
    $arguments = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $configureScript,
        '-GamePath', $SelectedGamePath, '-FpsTarget', $FpsTarget.Value,
        '-UnlockerSource', $source.Mode, '-NonInteractive', '-Language', $script:Language
    )
    if ($plugins.OptiScaler) { $arguments += @('-OptiScalerSource', 'Existing') } else { $arguments += '-DisableOptiScaler' }
    if (-not $plugins.AntiBlur) { $arguments += '-DisableAntiBlur' }
    if (-not $plugins.HDR) { $arguments += '-DisableHDR' }
    if ($source.Mode -eq 'Manual') { $arguments += @('-UnlockerPackagePath', $source.Path) }
    if ($NoShortcut) { $arguments += '-NoShortcut' }
    Write-Host ''
    Write-Host '正在安装，请稍候...' -ForegroundColor Cyan
    $backendOutput = @(& powershell.exe @arguments 2>&1)
    $backendExitCode = $LASTEXITCODE
    if ($backendExitCode -ne 0) {
        $backendOutput | Set-Content -LiteralPath $errorLogPath -Encoding UTF8
        Write-Host '安装失败，请重试。' -ForegroundColor Red
        Pause-Menu
        return $false
    }
    Remove-Item -LiteralPath $errorLogPath -Force -ErrorAction SilentlyContinue
    Write-Host '安装完成。' -ForegroundColor Green
    if ($unlockerWasMissing -or $firstRun) {
        Write-Host ''
        $FpsTarget.Value = Set-FpsTarget -CurrentValue 60 -DefaultValue 60 -FirstRun
        Update-FpsTarget -FpsTarget $FpsTarget.Value
        Save-State -SelectedGamePath $SelectedGamePath -FpsTarget $FpsTarget.Value
    }
    else { Pause-Menu }
    return $true
}

function Invoke-NvidiaDlssSetup {
    if (-not (Test-Path -LiteralPath $optiPath -PathType Leaf)) { return }
    $arguments = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $configureScript,
        '-EnsureNvidiaDlssOnly', '-NonInteractive', '-Language', $script:Language
    )
    & powershell.exe @arguments
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'NVIDIA DLSS 组件安装失败，可在“安装模块”或“更新模块”中重试。' -ForegroundColor Red
        Pause-Menu
    }
}

function Invoke-InstallWizard {
    param([string]$SelectedGamePath, [int]$FpsTarget)
    Write-Header -Title '安装模块'
    Write-InstallCatalog -SelectedGamePath $SelectedGamePath
    $selection = @(Select-ModuleSet -ActionName '安装')
    if ($selection.Count -eq 0) { return }
    $state = Get-ModuleState -SelectedGamePath $SelectedGamePath
    $desired = [ordered]@{
        OptiScaler = [bool]$state.OptiScaler
        AntiBlur = [bool]$state.AntiBlur
        HDR = [bool]$state.HDR
    }
    foreach ($module in $selection) {
        if ($module -eq 2) { $desired.OptiScaler = $true }
        if ($module -eq 3) { $desired.AntiBlur = $true }
        if ($module -eq 4) { $desired.HDR = $true }
    }
    $unlockerSource = 'Existing'
    $optiSource = 'Existing'
    $reShadeSource = 'Existing'
    $optiPackagePath = $null
    if ((2 -in $selection) -and -not (Test-Path -LiteralPath $optiPath -PathType Leaf)) {
        $optiSelection = Select-ComponentSource `
            -Name 'OptiScaler' `
            -Repository 'optiscaler/OptiScaler' `
            -AssetPattern '\.7z$' `
            -LocalPath $optiPath `
            -CompareMode 'Version' `
            -FileFilter 'OptiScaler (*.7z;*.zip)|*.7z;*.zip|所有文件 (*.*)|*.*'
        if ($null -eq $optiSelection) { return }
        $optiSource = $optiSelection.Mode
        $optiPackagePath = $optiSelection.Path
    }
    elseif ($desired.OptiScaler) {
        $optiSource = 'Existing'
    }
    if (4 -in $selection) {
        $reShadeSource = Select-ReShadeSource
        if ($null -eq $reShadeSource) { return }
    }
    if ($desired.OptiScaler -and $optiSource -eq 'Existing' -and -not (Test-Path -LiteralPath $optiPath -PathType Leaf)) {
        $optiSelection = Select-ComponentSource `
            -Name 'OptiScaler' `
            -Repository 'optiscaler/OptiScaler' `
            -AssetPattern '\.7z$' `
            -LocalPath $optiPath `
            -CompareMode 'Version' `
            -FileFilter 'OptiScaler (*.7z;*.zip)|*.7z;*.zip|所有文件 (*.*)|*.*'
        if ($null -eq $optiSelection) { return }
        $optiSource = $optiSelection.Mode
        $optiPackagePath = $optiSelection.Path
    }
    $arguments = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $configureScript,
        '-GamePath', $SelectedGamePath, '-FpsTarget', $FpsTarget,
        '-UnlockerSource', $unlockerSource, '-NonInteractive', '-Language', $script:Language
    )
    if ($desired.OptiScaler) { $arguments += @('-OptiScalerSource', $optiSource) } else { $arguments += '-DisableOptiScaler' }
    if (-not $desired.AntiBlur) { $arguments += '-DisableAntiBlur' }
    if ($desired.HDR) { $arguments += @('-ReShadeSource', $reShadeSource) } else { $arguments += '-DisableHDR' }
    if ($desired.OptiScaler -and $optiSource -eq 'Manual') { $arguments += @('-OptiScalerPackagePath', $optiPackagePath) }
    if ($NoShortcut) { $arguments += '-NoShortcut' }
    Write-Host ''
    Write-Host '正在安装，请稍候...' -ForegroundColor Cyan
    $backendOutput = @(& powershell.exe @arguments 2>&1)
    $backendExitCode = $LASTEXITCODE
    if ($backendExitCode -ne 0) {
        $backendOutput | Set-Content -LiteralPath $errorLogPath -Encoding UTF8
        Write-Host '安装失败，请重试。' -ForegroundColor Red
    }
    else {
        Remove-Item -LiteralPath $errorLogPath -Force -ErrorAction SilentlyContinue
        Write-Host '安装完成。' -ForegroundColor Green
    }
    Pause-Menu
}

function Invoke-UpdateWizard {
    param([string]$SelectedGamePath, [int]$FpsTarget)
    Write-Header -Title '更新模块'
    Write-InstallCatalog -SelectedGamePath $SelectedGamePath
    $selection = @(Select-ModuleSet -ActionName '更新' -IncludeFoundation -IncludeSelfUpdate)
    if ($selection.Count -eq 0) { return }

    $state = Get-ModuleState -SelectedGamePath $SelectedGamePath
    $installed = @{
        1 = [bool]$state.Unlocker
        2 = [bool]$state.OptiScaler
        3 = [bool]$state.AntiBlur
        4 = [bool]$state.HDR
        5 = $true
    }
    $validSelection = [Collections.Generic.List[int]]::new()
    foreach ($module in $selection) {
        if ($installed[$module]) { $validSelection.Add($module) }
        else { Write-Host "模块 $module 尚未安装，已跳过。" -ForegroundColor Yellow }
    }
    if ($validSelection.Count -eq 0) { Pause-Menu; return }

    $unlockerSource = 'Existing'
    $optiSource = 'Existing'
    $reShadeSource = 'Existing'
    if ($validSelection.Contains(1)) {
        $unlockerSource = Test-ComponentUpdate `
            -Name 'FPS Unlocker' `
            -Repository '34736384/genshin-fps-unlock' `
            -AssetPattern '^unlockfps_nc\.exe$' `
            -LocalPath $unlockerPath `
            -CompareMode 'Hash' `
            -NoConfirm `
            -NoPause
        if ($unlockerSource -eq 'Retry') { $unlockerSource = 'Existing' }
    }
    if ($validSelection.Contains(2)) {
        $optiSource = Test-ComponentUpdate `
            -Name 'OptiScaler' `
            -Repository 'optiscaler/OptiScaler' `
            -AssetPattern '\.7z$' `
            -LocalPath $optiPath `
            -CompareMode 'Version' `
            -NoConfirm `
            -NoPause
        if ($optiSource -eq 'Retry') { $optiSource = 'Existing' }
    }
    if ($validSelection.Contains(4)) {
        $reShadeSource = Select-ReShadeSource
        if ($null -eq $reShadeSource) { return }
    }

    $componentSelection = @($validSelection | Where-Object { $_ -in @(1, 2, 3, 4) })
    if ($componentSelection.Count -gt 0) {
        $desired = [ordered]@{
            OptiScaler = [bool]$state.OptiScaler
            AntiBlur = [bool]$state.AntiBlur
            HDR = [bool]$state.HDR
        }
        $arguments = @(
            '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $configureScript,
            '-GamePath', $SelectedGamePath, '-FpsTarget', $FpsTarget,
            '-UnlockerSource', $unlockerSource, '-NonInteractive', '-Language', $script:Language
        )
        if ($desired.OptiScaler) { $arguments += @('-OptiScalerSource', $optiSource) } else { $arguments += '-DisableOptiScaler' }
        if (-not $desired.AntiBlur) { $arguments += '-DisableAntiBlur' }
        if ($desired.HDR) { $arguments += @('-ReShadeSource', $reShadeSource) } else { $arguments += '-DisableHDR' }
        if ($NoShortcut) { $arguments += '-NoShortcut' }
        Write-Host ''
        Write-Host '正在更新所选模块，请稍候...' -ForegroundColor Cyan
        $backendOutput = @(& powershell.exe @arguments 2>&1)
        if ($LASTEXITCODE -ne 0) {
            $backendOutput | Set-Content -LiteralPath $errorLogPath -Encoding UTF8
            Write-Host '模块更新失败，请查看错误日志。' -ForegroundColor Red
        }
        else {
            Remove-Item -LiteralPath $errorLogPath -Force -ErrorAction SilentlyContinue
            if ($validSelection.Contains(3)) { Write-Host '反虚化组件已同步为当前发布包版本。' -ForegroundColor Green }
            if ($validSelection.Contains(4)) { Write-Host 'ReShade 与 RenoDX 已同步为当前发布包版本。' -ForegroundColor Green }
            Write-Host '所选模块更新完成。' -ForegroundColor Green
            if ($validSelection.Contains(2)) { Invoke-NvidiaDlssSetup }
        }
    }

    if ($validSelection.Contains(5)) {
        if (Start-PackageSelfUpdate) { return }
    }
    Pause-Menu
}

function Save-FpsConfig {
    param([object]$Config)
    $Config | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $fpsConfigPath -Encoding UTF8
}

function Remove-DllFromConfig {
    param([object]$Config, [string[]]$Paths)
    if ($null -eq $Config) { return }
    $newList = [Collections.Generic.List[string]]::new()
    foreach ($entry in @($Config.DllList)) {
        $remove = $false
        foreach ($path in $Paths) {
            if ([string]::Equals([IO.Path]::GetFullPath([string]$entry), [IO.Path]::GetFullPath($path), [StringComparison]::OrdinalIgnoreCase)) { $remove = $true; break }
        }
        if (-not $remove) { $newList.Add([string]$entry) }
    }
    $Config.DllList = @($newList)
}

function Invoke-UninstallWizard {
    param([string]$SelectedGamePath)
    Write-Header -Title '停止加载模块'
    Write-InstallCatalog -SelectedGamePath $SelectedGamePath
    $selection = @(Select-ModuleSet -ActionName '停止加载' -Uninstall)
    if ($selection.Count -eq 0) { return }
    $config = Get-FpsConfig
    if (2 -in $selection) {
        Remove-DllFromConfig -Config $config -Paths @($bridgePath, $optiPath)
    }
    if (3 -in $selection) { Remove-DllFromConfig -Config $config -Paths @($antiBlurPath) }
    if (4 -in $selection) {
        Remove-DllFromConfig -Config $config -Paths @($reShadePath)
        if ($null -ne $config) { Set-JsonPropertyValue -Object $config -Name 'UseHDR' -Value $false | Out-Null }
    }
    if ($null -ne $config) { Save-FpsConfig -Config $config }
    Write-Host ''
    Write-Host '所选模块已停止加载，本地文件和配置均已保留。' -ForegroundColor Green
    Pause-Menu
}

function Set-FpsTarget {
    param([int]$CurrentValue, [int]$DefaultValue = 0, [switch]$FirstRun)
    while ($true) {
        $prompt = if ($FirstRun) { "请输入帧率上限（直接回车使用 $DefaultValue）" } else { "请输入帧率上限（当前 $CurrentValue，直接回车保持不变）" }
        Write-Host $prompt -ForegroundColor Yellow
        $value = (Read-Host '请输入数值').Trim()
        if ([string]::IsNullOrWhiteSpace($value)) {
            if ($DefaultValue -gt 0) { return $DefaultValue }
            return $CurrentValue
        }
        $parsed = 0
        if ([int]::TryParse($value, [ref]$parsed)) {
            if ($parsed -gt 0) { return $parsed }
        }
        Write-Host '请输入大于 0 的整数。' -ForegroundColor Red
    }
}

function Update-FpsTarget {
    param([int]$FpsTarget)
    $config = Get-FpsConfig
    if ($null -eq $config) { return }
    $config.FPSTarget = $FpsTarget
    Save-FpsConfig -Config $config
}

function Show-AboutMenu {
    $entries = @(
        [pscustomobject]@{ Name = 'シリアCelia'; Projects = 'FSR Bridge、AntiPlayerMosaic、安装管理脚本'; Url = 'https://space.bilibili.com/8218484' },
        [pscustomobject]@{ Name = 'RenoDX 作者（Bilibili UID 3461582765951639）'; Projects = 'RenoDX HDR'; Url = 'https://space.bilibili.com/3461582765951639' },
        [pscustomobject]@{ Name = 'EndlesslyFlowering'; Projects = 'ReShade HDR shaders'; Url = 'https://github.com/EndlesslyFlowering/ReShade_HDR_shaders' },
        [pscustomobject]@{ Name = '34736384'; Projects = 'FPS Unlocker'; Url = 'https://github.com/34736384/genshin-fps-unlock' },
        [pscustomobject]@{ Name = 'OptiScaler contributors'; Projects = 'OptiScaler'; Url = 'https://github.com/optiscaler/OptiScaler' },
        [pscustomobject]@{ Name = 'crosire'; Projects = 'ReShade'; Url = 'https://reshade.me' }
    )
    while ($true) {
        Write-Header -Title '关于 / 作者主页'
        for ($index = 0; $index -lt $entries.Count; $index++) {
            Write-Host ("  {0}. {1}" -f ($index + 1), $entries[$index].Name) -ForegroundColor Cyan
            Write-Host ("     作品: {0}" -f $entries[$index].Projects)
        }
        Write-Host ''
        Write-Host '  0. 返回上一层'
        $choice = (Read-Host '请输入选项').Trim()
        if ($choice -eq '0') { return }
        $selected = 0
        if ([int]::TryParse($choice, [ref]$selected) -and $selected -ge 1 -and $selected -le $entries.Count) {
            Start-Process -FilePath $entries[$selected - 1].Url
            continue
        }
        Write-Host '无效选项。' -ForegroundColor Red
        Start-Sleep -Milliseconds 700
    }
}

function Select-InterfaceLanguage {
    param([string]$SelectedGamePath, [int]$FpsTarget)
    Write-Header -Title '语言 / Language'
    Write-Host '  1. 中文'
    Write-Host '  2. English'
    Write-Host '  0. 返回上一层'
    $choice = (Read-Host '请输入选项').Trim()
    $selectedLanguage = switch ($choice) {
        '1' { 'zh-CN' }
        '2' { 'en-US' }
        default { return }
    }
    $script:Language = $selectedLanguage
    Initialize-InstallerLocalization -Language $script:Language
    Save-State -SelectedGamePath $SelectedGamePath -FpsTarget $FpsTarget
}

$state = Read-State
$initialGamePath = if (-not [string]::IsNullOrWhiteSpace($GamePath)) { $GamePath } else { [string]$state.GamePath }
$selectedGamePath = Select-GamePath -InitialPath $initialGamePath
if ($null -eq $selectedGamePath) { exit 0 }
$shouldShowPathWarning = [string]::IsNullOrWhiteSpace([string]$state.GamePath) -or
    -not [string]::Equals([string]$state.GamePath, $selectedGamePath, [StringComparison]::OrdinalIgnoreCase)
if ($shouldShowPathWarning -and (Show-PathCompatibilityWarning -GameExePath $selectedGamePath)) { Pause-Menu }
$fpsTarget = [int]$state.FpsTarget
Repair-RuntimePaths -SelectedGamePath $selectedGamePath
if (-not (Invoke-FoundationSetup -SelectedGamePath $selectedGamePath -FpsTarget ([ref]$fpsTarget))) { exit 1 }
Invoke-NvidiaDlssSetup
Save-State -SelectedGamePath $selectedGamePath -FpsTarget $fpsTarget

while ($true) {
    $moduleState = Get-ModuleState -SelectedGamePath $selectedGamePath
    $installedCount = @(@($moduleState.OptiScaler, $moduleState.AntiBlur, $moduleState.HDR) | Where-Object { $_ }).Count
    Write-Header -Title '原神插件管理器'
    Write-Host "[√] 游戏目录: $(Split-Path -Parent $selectedGamePath)" -ForegroundColor Green
    Write-Host "[√] 插件目录: $root" -ForegroundColor Green
    $installedCount++
    Write-Host "    已安装 $installedCount / 4" -ForegroundColor DarkGray
    Write-Host ''
    Write-ModuleLine -Number 1 -Name 'FPS Unlocker' -Installed $moduleState.Unlocker -Path $unlockerPath -Extra "当前帧率上限 $fpsTarget"
    Write-ModuleLine -Number 2 -Name 'FSR Bridge + OptiScaler' -Installed $moduleState.OptiScaler -Path $optiPath
    Write-ModuleLine -Number 3 -Name '反虚化 / 隐藏 UID' -Installed $moduleState.AntiBlur -Path $antiBlurPath
    Write-ModuleLine -Number 4 -Name 'ReShade + RenoDX HDR' -Installed $moduleState.HDR -Path $reShadePath
    Write-Host ''
    Write-Host '  1. 安装模块' -ForegroundColor Cyan
    Write-Host '  2. 更新模块'
    Write-Host '  3. 停止加载 / 卸载模块'
    Write-Host '  4. 更换游戏目录'
    Write-Host '  5. 修改帧率上限'
    Write-Host '  6. 启动游戏'
    Write-Host '  7. 恢复所有插件默认配置'
    Write-Host '  8. 关于 / 作者主页'
    Write-Host '  9. 语言 / Language'
    Write-Host '  0. 退出'
    $choice = (Read-Host '请输入选项').Trim()
    switch ($choice) {
        '1' { Invoke-InstallWizard -SelectedGamePath $selectedGamePath -FpsTarget $fpsTarget }
        '2' {
            Invoke-UpdateWizard -SelectedGamePath $selectedGamePath -FpsTarget $fpsTarget
            if ($script:SelfUpdateStarted) { break }
        }
        '3' { Invoke-UninstallWizard -SelectedGamePath $selectedGamePath }
        '4' {
            $newPath = Select-GamePath -InitialPath $selectedGamePath -ForceSelection
            if ($null -ne $newPath -and (Invoke-FoundationSetup -SelectedGamePath $newPath -FpsTarget ([ref]$fpsTarget))) {
                $selectedGamePath = $newPath
                if (Show-PathCompatibilityWarning -GameExePath $selectedGamePath) { Pause-Menu }
                Repair-RuntimePaths -SelectedGamePath $selectedGamePath
                Save-State -SelectedGamePath $selectedGamePath -FpsTarget $fpsTarget
            }
        }
        '5' {
            $fpsTarget = Set-FpsTarget -CurrentValue $fpsTarget
            Update-FpsTarget -FpsTarget $fpsTarget
            Save-State -SelectedGamePath $selectedGamePath -FpsTarget $fpsTarget
        }
        '6' {
            if (Test-Path -LiteralPath $unlockerPath -PathType Leaf) { Start-Process -FilePath $unlockerPath -WorkingDirectory $root }
            else { Write-Host 'FPS Unlocker 尚未安装。' -ForegroundColor Red; Pause-Menu }
        }
        '7' {
            if (Reset-AllPluginConfigurations -SelectedGamePath $selectedGamePath) { $fpsTarget = 60 }
        }
        '8' { Show-AboutMenu }
        '9' { Select-InterfaceLanguage -SelectedGamePath $selectedGamePath -FpsTarget $fpsTarget }
        '0' { break }
        default { Write-Host '无效选项。' -ForegroundColor Red; Start-Sleep -Milliseconds 700 }
    }
    if ($choice -eq '0' -or $script:SelfUpdateStarted) { break }
}
