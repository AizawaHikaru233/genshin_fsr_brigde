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

# 必须显式写入的功能键 —— **与 `Dx11FsrBridge\Dx11FsrBridge.package.ini` 的
# [Dx11FsrBridge] 段逐键一致**（改模板时同步改这里）。
#
# 为什么"全量写"而不是只挑几个关键键：本项目踩过多次"键不存在 ⇒ 依赖代码内建
# 缺省 ⇒ 缺省值一变无人察觉"的坑（RenderScaleMenu 曾整段消失、TransparentJitter
# 的旧键曾让修复静默失效）。逐键显式写入后，部署结果与发布模板逐字对齐。
#
# ⚠️ 诊断键（性能探针 / FSR2 输入纹理转储 / 逐 draw 与纹理追踪 / 着色器 dump 等）
#    已从发布版整块剔除：本清单与发布模板都不再包含它们；需要诊断请用开发构建。
#    Diagnostic keys are excluded from the release build - do not add them back here.
#
# ⚠️ 两个**故意不写**的例外：
#    · Ffx12AsyncUpscale —— 由安装器 / 芙芙启动器按显卡自动写入（RDNA2→0、其他→1），
#      本脚本写死 1 会与它的按显卡判定打架；
#    · Ffx12DllPath     —— 路径键：留空即"随包 payload\AMD 的 SDK"，本脚本不覆盖
#      用户自定义路径。
$configValues = [ordered]@{
    # ---- 1. 核心功能 / Core features ----
    'Enabled'                   = '1'
    'Ffx12'                     = '1'   # 代码内建缺省是 0 ⇒ 必须显式写 1（删掉 = 静默关闭）
    'Ffx12Version'              = 'ffx12-fsr4.x'
    'RenderScaleMenu'           = '1'   # 渲染精度 hook 总开关；显式写入避免"键不存在→依赖缺省"
    # ---- 2. 透明队列抖动修复（正式功能，默认开）/ TransparentJitter ----
    # 旧键 JitterFlag* 系列（Probe / Force / ProbeFrames / Hotkey）**已从源码移除**：
    # 正式版 DLL 里连键名都不存在，写进 ini **没有任何效果**，也不会再打印迁移告警。
    # 移除原因：命名像"只读诊断探针"，实际承担修复（用户为了关诊断写 Probe=0 会让修复
    # **静默失效**），且其热键在游戏里被吃掉。迁移目标就是下面的 TransparentJitter。
    'TransparentJitter'         = '1'   # 1 = 修复生效（默认，实机已验证）；0 = 回退游戏原行为
    # ---- 3. 绘制入口过滤（正式功能，默认开）/ Draw-entry filter ----
    'DrawEntryFilter'           = '1'   # 1 = 用 element_count==3 在调用入口早退（默认）
    'DrawEntryFilterCanary'     = '64'  # 保险抽样：每 64 个非 3 绘制放行 1 个（默认）
    # ---- 4. 传输与性能 / Transport & performance ----
    'Ffx12GpuInterop'           = '1'
    'Fsr2FastStateTracking'     = '1'   # 学到目标 PS hash 后旁路状态镜像钩子（必须为 1）
    # ---- 5. 画面参数（已按本游戏调优，勿改）/ Frame parameters ----
    # 运动解码：**必须为 1**。2026-09-18 实测 Ffx12MotionDecode=0（raw 变体）会**大幅加剧闪烁**
    # （所有切线明显的线条都闪）——游戏 motion 确为 R10G10B10A2 平方编码，raw 变体量纲错误。
    'Ffx12MotionDecode'         = '1'
    # 抖动模式 3 = 零中心（-(norm*width)+0.5）。模式 4 的值域是 [-1,0]、恒定带 -0.5px
    # 偏置 → 时域历史朝错误方向累积（DLSS M/L 上表现为网格黑线，其他模型上表现为
    # 高对比边缘闪烁）。这是 v2.0.0 起的已知缺陷，勿改回 4。
    'Ffx12JitterMode'           = '3'
    # 抖动延迟：**退回 0（当前帧 jitter）**。2026-09-18 改为 1 后用户实测"画面整体变糊"
    # 且闪烁仍在 ⇒ 变糊来源是这一项（"预录滞后一帧"的判断在本游戏不成立）。
    'Ffx12JitterDelay'          = '0'
    'Ffx12NonLinear'            = '0'   # 色彩空间：保持 0（线性）；代码内建缺省是 1
    'Ffx12PqChain'              = '0'   # 1 会白屏，勿改
    'Ffx12Hdr'                  = '0'   # 代码内建缺省是 1
    'Ffx12AutoExposure'         = '1'
    'Ffx12MotionScale'          = '1.0'
    'Ffx12VelocityFactor'       = '50'
    'Ffx12DepthInverted'        = '1'
    'Ffx12FovScale'             = '1.0'
    'Ffx12CameraNear'           = '0.5' # 代码内建缺省是 0.25 —— 两者不同，勿删行
    'Ffx12CameraFar'            = '6000.0'
    # ---- 6. FSR2 输入 / 蒙版 / 合成族 / FSR2 input, masks, compositing ----
    'Fsr2MotionVectorScaleMode' = '1'
    'Fsr2MotionVectorsJittered' = '0'
    'Fsr2JitterMode'            = '3'
    'Fsr2UseReactiveMask'       = '0'   # 代码内建缺省是 1
    'Fsr2UseTransparencyMask'   = '0'
    'Fsr2FamilySkip'            = '1'   # 代码内建缺省是 0 ⇒ 删行会失去该优化
    'Fsr2Il2CppHook'            = '1'   # 代码内建缺省是 0 ⇒ 删行会让桥接失效
    'Ffx12ReuseSameGeneration'  = '0'   # 开启会产生残像
    'Ffx12FeatureFallback'      = '1'
    # ---- 日志（[Log] 段）/ Logging ----
    # 必须在这里写：Add-LogSection 只在**没有** [Log] 段时才补整段，
    # 而已部署的 ini 都已有 [Log] 段 → 段内的新键永远不会被补上（实测 queue_capacity 缺失）。
    'queue_capacity'            = '8192'
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
    Write-Host "  4. transparent_jitter_installed ... force=1                              ← 透明队列抖动修复是否生效（正式功能）"
}

