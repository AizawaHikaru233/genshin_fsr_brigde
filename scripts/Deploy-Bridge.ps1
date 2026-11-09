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
# 只依赖构建产物（默认 $BuildDll = <仓库>\build-package-bridge\Dx11FsrBridge.dll，可用 -BuildDll 覆盖）。

[CmdletBinding()]
param(
    [string]$BuildDll = 'D:\FSR\build-package-bridge\Dx11FsrBridge.dll',
    [switch]$SkipConfig,
    [switch]$Rollback
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# 已知的桥加载路线（新增路线时在此登记）
#
# 2026-11-10（实机路线已迁移，旧表整体失效）：启动器日志 `FSR-Bridge-Plugin.log` 里的
#   `bridge_path` 与 `bridge_load_success module=0x7FF984140000` 证明本机真正加载的是
#   FufuLauncher 的**用户级**安装目录（%LOCALAPPDATA%\Programs\...），不是全机安装的
#   D:\Program Files\...。本机实测原两条路线 Test-Path 均为 False ⇒ 旧脚本一条都铺不到
#   （循环里全部「跳过」然后 exit 0，见文件末尾的兜底）。
#   故新增 FufuLauncherUser 一条；旧的 Starward / FufuLauncher **保留**——别的机器上它们
#   可能仍然有效，删掉会让那些机器部署不到。Legacy=$true 只影响「跳过」时的提示文案。
#   FufuLauncherUser 用 %LOCALAPPDATA% 环境变量拼接（用户级安装目录随用户而异），
#   不写死任何本机绝对路径。该变量为空时自动跳过这条路线（不报错）。
$routes = @()

    # 用户级安装目录由 %LOCALAPPDATA% 拼接；变量为空（非 Windows 或被清空）时跳过本条，不抛错。
    if (-not [string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
        $routes += @{ Name = 'FufuLauncherUser'; Dir = (Join-Path $env:LOCALAPPDATA 'Programs\FufuLauncher\Plugins\FSR-Bridge-Plugin\payload\Bridge'); Legacy = $false }
    }

    $routes += @(
        @{ Name = 'Starward';         Dir = 'D:\miHoYo Games\Starward\原神解帧FSR插件包\payload\Bridge'; Legacy = $true },
        @{ Name = 'FufuLauncher';     Dir = 'D:\Program Files\FufuLauncher\Plugins\FSR-Bridge-Plugin\payload\Bridge'; Legacy = $true }
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
# ⚠️ 三个**故意不写**的例外：
#    · Ffx12AsyncUpscale —— 由安装器 / 芙芙启动器按显卡自动写入（RDNA2→0、其他→1），
#      本脚本写死 1 会与它的按显卡判定打架；
#    · Ffx12DllPath     —— 路径键：留空即"随包 payload\AMD 的 SDK"，本脚本不覆盖
#      用户自定义路径；
#    · Ffx12Backend     —— **部署脚本不得静默把用户切回 native 后端**。ngx 是 opt-in 后端：
#      本脚本每次部署都跑，若在这里写死 native，每次部署都会把用户已选的 ngx 清掉，
#      而 ngx 失效是**静默**的（回落到 native 只是性能/画质变化，不会崩、不会报错）。
#      缺键时代码内建缺省就是 native（Dx11FsrBridge.cpp:4110 lpDefault=L"native"），
#      所以不写这个键也天然安全，用户手改的 ngx 也就保得住。
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
    # ---- 7. NGX(DLSS) 后端 / NGX backend（opt-in，默认全程 native）----
    # 默认值逐键与 Dx11FsrBridge.package.ini 的 [Dx11FsrBridge] 段、以及代码内建缺省一致
    # （Dx11FsrBridge.cpp:4107-4195）。改这里必须三处同步改。
    # ⚠️ Ffx12Backend **故意不在本表里**：见文件顶部"三个故意不写的例外"。
    'NgxApplicationId'          = '1337'
    'NgxFeatureDirs'            = ''    # 空 = 自动探测含 nvngx_dlss.dll 的目录
    'NgxHdr'                    = 'auto' # 三态：auto（默认）/ 1 强制 HDR / 0 强制 SDR
    'NgxRenderPreset'           = '0'   # 0 = 六个 preset 参数都不设（跟随 NGX 自身规则）；5..15 可显式指定
    'NgxSetFrameTimeDelta'      = '1'
    'NgxUseReactiveMask'        = '0'
    'NgxLogLevel'               = '1'   # 0=全关 1=error（默认，取证行已可见）2=info 3=debug
    'NgxPassthrough'            = '0'
    # ---- 日志（[Log] 段）/ Logging ----
    # 必须在这里写：Add-LogSection 只在**没有** [Log] 段时才补整段，
    # 而已部署的 ini 都已有 [Log] 段 → 段内的新键永远不会被补上（实测 queue_capacity 缺失）。
    'queue_capacity'            = '8192'
}
$logSection = @(
    '[Log]',
    '; level: off / error / warn / info / debug / trace（或 0..5：0=off）；越靠后越详细，只写该级及更严重的行；可用下方 [Log.Categories] 按子系统单独覆盖',
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

# 2026-11-10（与仓库规范对齐）：规范 §8.5 要求备份名 `Xxx.dll.bak-YYYYMMDD-HHMM`（时分
#   HHMM 两位、不带秒），原格式带秒。改前已 grep 全仓库确认**没有任何代码解析这个 stamp**：
#   它只用于拼备份文件名，而 -Rollback 与 Toggle-BridgeDll.ps1 都用 'Dx11FsrBridge.dll.bak-*'
#   通配枚举、不解析时间串 ⇒ 改短不影响任何调用方，历史带秒的备份仍能被通配匹配到。
$stamp = Get-Date -Format 'yyyyMMdd-HHmm'
$summary = New-Object System.Collections.ArrayList
# 2026-11-10（核心改进）：统计**真正成功**的路线数。原脚本在一条都铺不到时只打印一串
#   「跳过」然后正常 exit 0 —— 使用者看到一堆「跳过」很容易误以为已部署。现在 0 条即
#   失败退出（见文件末尾），让「看起来跑完了其实什么都没做」不可能静默通过。
$modeLabel = if ($Rollback) { '回滚' } else { '部署' }
$appliedCount = 0

foreach ($route in $routes) {
    $dir = $route.Dir
    if (-not (Test-Path -LiteralPath $dir)) {
        # 2026-11-10：历史路线（Legacy）在本机已不存在属预期，文案要与「新路线也找不到」
        #   区分开，否则真实的登记错误会被淹没在一片正常的「跳过」里。
        $why = if ($route.Legacy) { '路线已不存在则跳过（历史路线，本机可能已迁移）' } else { '**跳过：登记路线不存在**' }
        [void]$summary.Add("[$($route.Name)] $($why)：$dir")
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
        $rbSize = (Get-Item $dll).Length
        $rbSha = (Get-FileHash $dll -Algorithm SHA256).Hash.Substring(0, 16)
        [void]$summary.Add(("[{0}] 已回滚到 {1}  ->  {2}  {3} B  sha256={4}" -f $route.Name, $bak.Name, $dll, $rbSize, $rbSha))
        $appliedCount++
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
    # 2026-11-10：摘要取 SHA256 前 16 位（原为 8 位）。8 位只有 32 bit，多份构建撞车概率不可忽略，
    #   16 位足以人工比对又不至于把 summary 撑爆。
    $sha = (Get-FileHash $dll -Algorithm SHA256).Hash.Substring(0, 16)

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

    # 2026-11-10：成功行必须能一眼核对"到底写了哪个文件"——打印完整 DLL 路径 + 大小 +
    #   SHA256 前 16 位（原先是 8 位，且不含路径，只能靠路线名反推）。多路线时同名 DLL
    #   分散在不同目录，路径是唯一能区分它们的字段。
    [void]$summary.Add(("[{0}] 已写入 {1}  {2} B  sha256={3}  (原 {4} B)  {5}" -f $route.Name, $dll, $after, $sha, $before, $cfgNote))
    $appliedCount++
}

Write-Host ""
Write-Host "$($modeLabel)结果 / Deploy summary:" -ForegroundColor Cyan
foreach ($line in $summary) { Write-Host "  $line" }
Write-Host "  合计：$appliedCount / $($routes.Count) 条路线已$($modeLabel)" -ForegroundColor Cyan

# 2026-11-10（本改动的核心目的）：0 条成功必须以非 0 退出码失败。
#   原来的行为是一条都没铺时也只打印一串「跳过」然后正常退出，使用者极易误判成
#   「跑完了、已部署」——实测本机两条旧路线都不存在时正是这种静默空转。
#   现在：0 条 → Write-Error（醒目）+ exit 1。注意这里**不能**用 throw：$ErrorActionPreference
#   已是 Stop，throw 会在打印前把控制权交给调用方，退出码不可控；显式 exit 1 才确定。
#   同时提示"至少有一条"是真实条件（不是永远成立），故只陈述当前判定，不承诺将来。
if ($appliedCount -eq 0) {
    Write-Host ""
    Write-Host "已登记路线（逐条 Test-Path 复核）:" -ForegroundColor Yellow
    foreach ($route in $routes) {
        $state = if (Test-Path -LiteralPath $route.Dir) { '存在' } else { '不存在' }
        Write-Host ("    [{0}] {1}  ({2})" -f $route.Name, $route.Dir, $state) -ForegroundColor Yellow
    }
    Write-Host "  这不是成功：一条都没写。请先确认实机加载路线，再把正确目录登记进路线表。" -ForegroundColor Red
    # -ErrorAction Continue 必须显式给出：$ErrorActionPreference 已是 Stop，否则 Write-Error
    #   会立刻抛终止错误、跳过下一行的 exit 1，退出码就落到调用方/宿主手里，不再是本脚本决定。
    Write-Error ("[$modeLabel] 没有任何路线成功：0 / {0} 条。所有登记路线的目录都不存在，或 DLL 全部被占用。" -f $routes.Count) -ErrorAction Continue
    exit 1
}

if (-not $Rollback) {
    Write-Host ""
    Write-Host "校验清单（下次采集时先看这几行）:" -ForegroundColor Cyan
    Write-Host "  1. logger effective level=... categories=... file=... queue_capacity=...  ← 日志器生效确认"
    Write-Host "  2. swapchain_hook_decision install=1 present=.. controls=.. color=..      ← 交换链钩子决策（本轮修复）"
    Write-Host "  3. hooked D3D11CreateDevice / rtv_bind_target                            ← D3D11 钩子是否装上"
    Write-Host "  4. transparent_jitter_installed ... force=1                              ← 透明队列抖动修复是否生效（正式功能）"
}

