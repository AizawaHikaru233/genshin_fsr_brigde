# Deploy-Bridge.ps1 — 把构建好的 Bridge 铺到实机路线（含备份与配置校验）
#
# 为什么需要它：实测踩过"只铺了一条路线"的坑——桥实际从芙芙插件路线加载（ReShade.ini
# 指向它），而我只铺了 Starward 路线，导致整场采集零输出、白排查一轮。
# 本脚本把所有**已知加载路线**一次铺齐，并显式校验关键开关是否生效。
#
# 用法：
#   .\Deploy-Bridge.ps1                      # 铺 DLL + 应用采集配置到所有路线
#   .\Deploy-Bridge.ps1 -SkipConfig          # 只铺 DLL，不动 ini
#   .\Deploy-Bridge.ps1 -Rollback            # 回滚到最近一次备份
#
# 只依赖构建产物 D:\FSR\build-package-bridge\Dx11FsrBridge.dll。

[CmdletBinding()]
param(
    [string]$BuildDll = 'D:\FSR\build-package-bridge\Dx11FsrBridge.dll',
    [switch]$SkipConfig,
    [switch]$Rollback
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# 已知的桥加载路线（新增路线时在此登记）
$routes = @(
    @{ Name = 'Starward'; Dir = 'D:\miHoYo Games\Starward\原神解帧FSR插件包\payload\Bridge' },
    @{ Name = 'FufuLauncher'; Dir = 'D:\Program Files\FufuLauncher\Plugins\FSR-Bridge-Plugin\payload\Bridge' }
)

# 采集期需要的配置（探针 + 新日志器）
$configValues = [ordered]@{
    'LogLevel'             = '1'      # 旧键：保留兼容（新日志器由 [Log] level 控制）
    # 运动解码：**必须为 1**。2026-09-18 实测 Ffx12MotionDecode=0（raw 变体）会**大幅加剧闪烁**
    # （所有切线明显的线条都闪）——游戏 motion 确为 R10G10B10A2 平方编码，raw 变体量纲错误。
    # 该变体保留仅供诊断，不要在生产配置里使用。
    'Ffx12MotionDecode'    = '1'
    'TraceTextureCreates'  = '0'      # 纹理/目标清单诊断（探针不依赖它）
    # 抖动模式 3 = 零中心（-(norm*width)+0.5）。模式 4 的值域是 [-1,0]、
    # 恒定带 -0.5px 偏置 → 时域历史朝错误方向累积（DLSS M/L 上表现为网格黑线，
    # 其他模型上表现为高对比边缘闪烁）。这是 v2.0.0 起的已知缺陷，勿改回 4。
    'Ffx12JitterMode'      = '3'
    # 抖动延迟：**退回 0（当前帧 jitter）**。2026-09-18 改为 1 后用户实测"画面整体变糊"
    # 且闪烁仍在；单独退回 NonLinear 无效 ⇒ 变糊来源是这一项（预录滞后判断在本游戏不成立）。
    'Ffx12JitterDelay'     = '0'
    # 色彩空间：保持 0（线性）。
    'Ffx12NonLinear'       = '0'
    # 渲染精度 hook 总开关：1=开启（默认行为）。此键曾被死代码清理整段删除，导致
    # 渲染精度接管**无条件常开、无法隔离**；显式写入可避免它再次因"键不存在"而
    # 依赖代码默认值（那样一旦默认值变动就无人察觉）。
    'RenderScaleMenu'      = '1'
    # 异步写队列深度。必须在这里写：Add-LogSection 只在**没有** [Log] 段时才补整段，
    # 而已部署的 ini 都已有 [Log] 段 → 段内的新键永远不会被补上（实测 queue_capacity 缺失）。
    'queue_capacity'       = '8192'
    # 诊断探针（默认关）。JitterFlagProbe=1 只读观测
    # useJitteredProjectionMatrixForTransparentRendering 的实参，**不修改任何行为**。
    # 放在这里是为了避免"键不存在 ⇒ 依赖代码默认值 ⇒ 默认值一变无人察觉"（同 RenderScaleMenu 的教训）。
    'JitterFlagProbe'      = '0'
    'JitterFlagProbeFrames'= '0'      # 0 = 一直记录；N = 只记录前 N 帧
    # FSR2 输入纹理转储（默认关）。抓帧用热键，**跑完务必改回 0**（每组 3 帧约 130 MB）。
    'Fsr2InputDump'          = '0'
    'Fsr2InputDumpFrames'    = '3'
    'Fsr2InputDumpIntervalMs'= '0'
    'Fsr2InputDumpHotkey'    = '122'
    'Fsr2InputDumpAutoStartSec' = '0'
    'Fsr2InputDumpRaw'       = '1'
    'Fsr2InputDumpPng'       = '1'
    'Fsr2InputDumpMaxDim'    = '2048'
}
$logSection = @(
    '[Log]',
    '; level: error / warn / info / debug / trace（或 0..5）。详见 docs/logging-design.md',
    'level=debug',
    'to_file=1',
    'to_debugger=0',
    'truncate_on_start=1',
    'max_file_kb=16384',
    'rotate_keep=2',
    'queue_capacity=8192',
    'compat_prefix=0',
    '',
    '[Log.Categories]',
    '; 按子系统覆盖全局等级；off 可彻底静默',
    'probe=debug',
    'upscale=debug'
)

function Set-IniValue {
    param([string]$Path, [string]$Key, [string]$Value)
    $lines = [IO.File]::ReadAllLines($Path, [Text.Encoding]::UTF8)
    $found = $false
    $out = New-Object System.Collections.ArrayList
    foreach ($line in $lines) {
        if ($line -match "^\s*$([regex]::Escape($Key))\s*=") {
            [void]$out.Add("$Key=$Value")
            $found = $true
        } else {
            [void]$out.Add($line)
        }
    }
    if (-not $found) {
        # ⚠️ 新键必须写进它所属的段，不能无脑追加到文件末尾。
        # 旧实现直接 Add 到末尾：而 Add-LogSection 会把 [Log] / [Log.Categories] 追加到
        # 文件最后 → 之后所有新键都落进 [Log.Categories] 段，被当成"分类名=等级"。
        # 实测后果：queue_capacity / Ffx12PresentProbe 落进 [Log.Categories]，
        # 日志里出现 `categories=...,Ffx12PresentProbe=ERROR`，而桥**从未读到这两个键**。
        $section = if ($Key -in @('level','to_file','to_debugger','truncate_on_start','max_file_kb','rotate_keep','queue_capacity','compat_prefix','debugger_max_per_sec')) { '[Log]' } else { '[Dx11FsrBridge]' }
        $sectionStart = -1
        $sectionEnd = $out.Count
        for ($i = 0; $i -lt $out.Count; ++$i) {
            if ($out[$i] -match '^\s*\[') {
                if ($sectionStart -ge 0) { $sectionEnd = $i; break }
                if ($out[$i] -match ('^\s*' + [regex]::Escape($section) + '\s*$')) { $sectionStart = $i }
            }
        }
        if ($sectionStart -lt 0) {
            [void]$out.Add("$section")
            [void]$out.Add("$Key=$Value")
        } else {
            # 插到该段末尾（下一个段头之前），并跳过段尾空行
            $insertAt = $sectionEnd
            while ($insertAt -gt $sectionStart + 1 -and $out[$insertAt - 1].Trim() -eq '') { $insertAt-- }
            $out.Insert($insertAt, "$Key=$Value")
        }
    }
    [IO.File]::WriteAllLines($Path, $out.ToArray(), [Text.UTF8Encoding]::new($false))
}

function Add-LogSection {
    param([string]$Path)
    $lines = [IO.File]::ReadAllLines($Path, [Text.Encoding]::UTF8)
    $text = $lines -join "`n"
    if ($text -match '(?m)^\s*\[Log\]\s*$') { return $false }  # 已有，不重复加
    # ⚠️ 必须插到 [Log.*] 子段（如 [Log.Categories]）**之前**，不能追加到文件末尾：
    # 追加到末尾会让 [Log] 位于 [Log.Categories] 之后，语义颠倒；
    # 且后续 Set-IniValue 的新键会落进 [Log.Categories]（见 Set-IniValue 注释）。
    $block = @(
        '; ==================== 日志系统（取代按内容猜等级）===================='
    ) + $logSection
    $insertAt = -1
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        if ($lines[$i] -match '^\s*\[Log\.') { $insertAt = $i; break }
    }
    $out = New-Object System.Collections.ArrayList
    if ($insertAt -lt 0) {
        foreach ($line in $lines) { [void]$out.Add($line) }
        [void]$out.Add('')
        foreach ($line in $block) { [void]$out.Add($line) }
    } else {
        for ($i = 0; $i -lt $lines.Count; ++$i) {
            if ($i -eq $insertAt) {
                foreach ($line in $block) { [void]$out.Add($line) }
                [void]$out.Add('')
            }
            [void]$out.Add($lines[$i])
        }
    }
    [IO.File]::WriteAllLines($Path, $out.ToArray(), [Text.UTF8Encoding]::new($false))
    return $true
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$summary = New-Object System.Collections.ArrayList

foreach ($route in $routes) {
    $dir = $route.Dir
    if (-not (Test-Path -LiteralPath $dir)) {
        [void]$summary.Add("[$($route.Name)] 跳过：目录不存在 $dir")
        continue
    }
    $dll = Join-Path $dir 'Dx11FsrBridge.dll'
    $ini = Join-Path $dir 'Dx11FsrBridge.ini'
    # default_config 与 Bridge 同级（都在 payload\ 下）：<dir>\..\default_config。
    # 旧写法 Split-Path(Split-Path $dir -Parent) -Parent 多推了一级——Starward 布局
    # （payload\Bridge）恰好凑对，芙芙布局（payload\Bridge）却落到 FSR-Bridge-Plugin\ 下，
    # Test-Path 为假 → 模板**静默不同步**（实测模板长期停留在 Ffx12JitterMode=4、无 [Log] 段）。
    $defIni = Join-Path (Split-Path $dir -Parent) 'default_config\Dx11FsrBridge.ini'

    if ($Rollback) {
        $bak = Get-ChildItem $dir -Filter 'Dx11FsrBridge.dll.bak-*' |
               Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($null -eq $bak) { [void]$summary.Add("[$($route.Name)] 无备份可回滚"); continue }
        Copy-Item -LiteralPath $bak.FullName -Destination $dll -Force
        [void]$summary.Add("[$($route.Name)] 已回滚到 $($bak.Name)")
        continue
    }

    # 冲突检查：DLL 被占用说明游戏在跑，替换会失败或产生半写文件
    try {
        $fs = [IO.File]::Open($dll, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        $fs.Close()
    } catch {
        [void]$summary.Add("[$($route.Name)] **跳过：DLL 被占用（游戏在运行）**")
        continue
    }

    $before = if (Test-Path $dll) { (Get-Item $dll).Length } else { 0 }
    Copy-Item -LiteralPath $dll -Destination (Join-Path $dir "Dx11FsrBridge.dll.bak-$stamp") -Force -ErrorAction SilentlyContinue
    Copy-Item -LiteralPath $BuildDll -Destination $dll -Force
    $after = (Get-Item $dll).Length
    $sha = (Get-FileHash $dll -Algorithm SHA256).Hash.Substring(0, 8)

    $cfgNote = 'config 未改'
    if (-not $SkipConfig -and (Test-Path -LiteralPath $ini)) {
        Copy-Item -LiteralPath $ini -Destination (Join-Path $dir "Dx11FsrBridge.ini.bak-$stamp") -Force
        foreach ($k in $configValues.Keys) { Set-IniValue -Path $ini -Key $k -Value $configValues[$k] }
        $added = Add-LogSection -Path $ini
        # 重置模板同步（否则 FufuLauncher 的 reset_file 会把配置打回没有探针的版本）
        if (Test-Path -LiteralPath $defIni) {
            Copy-Item -LiteralPath $defIni -Destination "$defIni.bak-$stamp" -Force
            foreach ($k in $configValues.Keys) { Set-IniValue -Path $defIni -Key $k -Value $configValues[$k] }
            [void](Add-LogSection -Path $defIni)
            $cfgNote = if ($added) { 'config 已改（含新增 [Log] 段）+ default_config 同步' } else { 'config 已改 + default_config 同步' }
        } else {
            $cfgNote = if ($added) { 'config 已改（含新增 [Log] 段）' } else { 'config 已改' }
        }
    }

    [void]$summary.Add(("[{0}] DLL {1} -> {2} B  sha={3}  {4}" -f $route.Name, $before, $after, $sha, $cfgNote))
}

Write-Host ""
Write-Host "部署结果 / Deploy summary:" -ForegroundColor Cyan
foreach ($line in $summary) { Write-Host "  $line" }

if (-not $Rollback) {
    Write-Host ""
    Write-Host "校验清单（下次采集时先看这几行）:" -ForegroundColor Cyan
    Write-Host "  1. logger effective level=... categories=... file=... queue_capacity=...  ← 日志器生效确认"
    Write-Host "  2. swapchain_hook_decision install=1 present=.. controls=.. color=..      ← 交换链钩子决策（本轮修复）"
    Write-Host "  3. hooked D3D11CreateDevice / rtv_bind_target                            ← D3D11 钩子是否装上"
}

