# InstallerCommon.ps1 —— 安装器公共实现（共享模块）
#
# 由 `tools\FpsUnlockInstaller\Configure.ps1` 与 `Installer.ps1` **各自** dot-source。
# ⚠️ Configure.ps1 是被 Installer.ps1 以**独立进程**调用的（`-File`），
#    所以本模块必须两个脚本都加载，不能靠进程继承。
#
# 与 `ReShadeResources.ps1` 同源做法（它是本仓库已有的共享模块先例）。
#
# ============================================================================
# 为什么有这份文件（2026-09-24，审核报告「去重 8 条」）
# ============================================================================
# 审核报告指出 Configure.ps1 与 Installer.ps1（及 Update-UpstreamComponents.ps1）
# 之间有十余函数整段复制，且**已出现键集合漂移**（B71/B73 修的 ReShade 路径键
# 漂移就是这类问题的实例）。
#
# ⚠️ 审核报告的行号已失效多次（本项目已遇到三次）⇒ 本模块的搬迁**全部按函数名
#    与内容重核**，并逐对比对过实现。核对结论：
#
#   真正逐字相同、可安全共享的：
#     Get-GitHubEndpointLatency / Get-GitHubFallbackUrls
#     Get-VideoControllersOnce / Get-NvidiaVideoControllers
#     Test-PathCompatibilityRisk / Show-PathCompatibilityWarning
#
#   审核报告的偏差（如实记录）：
#     * Invoke-GitHubRestMethodWithFallback **只在 Installer.ps1 里**，
#       并非"逐行重复" —— 但仍放进本模块（它与 Get-GitHubFallbackUrls 是一组）。
#     * Set-JsonProperty（Configure）与 Set-JsonPropertyValue（Installer）
#       **返回值语义不同**（后者返回"是否发生变化"的布尔），不是"几乎相同"。
#       本模块只保留**布尔版**（见下）。
#     * Assert-NvidiaSignedFile / Expand-ComponentPackage **未合并** ——
#       它们跨到 Update-UpstreamComponents.ps1（**开发者工具、不随包发布**），
#       依赖足迹不同（`$WorkspaceRoot` vs `$root`；后者无 `Assert-File`、
#       无 `Convert-InstallerText`），且两个 Expand-ComponentPackage 是
#       **真正不同的实现**（Configure 版含"解压后目录不能为空"的静默失败防护）。
#     * Resolve-GamePath/Get-GamePath（Configure）与 Resolve-GameExecutable
#       （Installer）**错误契约不同**（throw vs 返回 $null），
#       且 Get-GamePath 是交互式提示流程 ⇒ **未合并**。
#     * Repair-RuntimePaths 不是"复制"，是 Installer 特有的修复流程 ⇒ 未动。
#
# 行为不变的保证：凡两版语义不同处，一律**保留更严格/更完整**的那版，并在
# 下方每个函数处写明取舍理由；Configure 侧原调用点已核实**忽略返回值**。
# ============================================================================

Set-StrictMode -Version Latest

# ---------------------------------------------------------------------------
# 根目录
# ---------------------------------------------------------------------------
# 与 Configure.ps1 的算法**逐字一致**：脚本所在目录名为 `scripts` 时取父目录，
# 否则取自身。因此：
#   包内  —— 本模块在 `scripts\`  ⇒ 得包根目录（= Installer.ps1 的 $root）✓
#   仓库内 —— 本模块在 `tools\FpsUnlockInstaller\`（目录名不是 scripts）⇒ 取自身
#            （= Configure.ps1 / Installer.ps1 的 $root）✓
# 两种布局下都与调用方的 `$root` 相等 —— 已用测试核实。
$script:InstallerCommonRoot = if ([IO.Path]::GetFileName($PSScriptRoot) -ieq 'scripts') {
    [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
} else {
    [IO.Path]::GetFullPath($PSScriptRoot)
}

# ---------------------------------------------------------------------------
# GitHub 访问（原 Configure.ps1 与 Installer.ps1 各一份，逐字相同）
# ---------------------------------------------------------------------------

# 代理测速缓存。**必须在这里初始化**：Set-StrictMode -Version Latest 下，
# 读取未初始化的 $script: 变量会抛错。调用方脚本里原有的同名初始化保留不动
# （幂等，且都在任何使用之前执行）。
$script:GitHubProxyLatency = @{}

function Get-GitHubEndpointLatency {
    param([string]$HostName)
    if ($script:GitHubProxyLatency.ContainsKey($HostName)) { return [double]$script:GitHubProxyLatency[$HostName] }
    $latency = [double]::PositiveInfinity
    try {
        $ping = Test-Connection -ComputerName $HostName -Count 1 -ErrorAction Stop | Select-Object -First 1
        if ($null -ne $ping -and $ping.ResponseTime -ge 0) { $latency = [double]$ping.ResponseTime }
    }
    catch { }
    $script:GitHubProxyLatency[$HostName] = $latency
    return $latency
}

function Get-GitHubFallbackUrls {
    param([string]$Url)
    if ($Url -notmatch '^https://(api\.)?github\.com/') { return @($Url) }
    $proxies = @(
        [pscustomobject]@{ Host = 'ghfast.top'; Prefix = 'https://ghfast.top/' },
        [pscustomobject]@{ Host = 'gh-proxy.com'; Prefix = 'https://gh-proxy.com/' },
        [pscustomobject]@{ Host = 'ghproxy.net'; Prefix = 'https://ghproxy.net/' }
    ) | ForEach-Object {
        [pscustomobject]@{ Host = $_.Host; Prefix = $_.Prefix; Latency = (Get-GitHubEndpointLatency -HostName $_.Host) }
    } | Sort-Object Latency, Host
    $proxyUrls = @($proxies | ForEach-Object { $_.Prefix + $Url })
    return @($proxyUrls + @($Url))
}

# 原仅在 Installer.ps1 中（审核报告称"逐行重复"，实为单一实现）。
# ReShadeResources.ps1 通过 Get-Command 守卫调用 Get-GitHubFallbackUrls，
# 因此它依赖宿主脚本已加载本模块 —— Configure.ps1 / Installer.ps1 都会加载。
function Invoke-GitHubRestMethodWithFallback {
    param([string]$Url, [string]$UserAgent, [string]$RequiredProperty)
    $failures = [Collections.Generic.List[string]]::new()
    foreach ($candidateUrl in @(Get-GitHubFallbackUrls -Url $Url)) {
        try {
            $result = Invoke-RestMethod -Headers @{ 'User-Agent' = $UserAgent } -Uri $candidateUrl -TimeoutSec 20
            if (-not [string]::IsNullOrWhiteSpace($RequiredProperty) -and $null -eq $result.PSObject.Properties[$RequiredProperty]) {
                throw "响应缺少需要的字段: $RequiredProperty"
            }
            return $result
        }
        catch {
            $failures.Add("$candidateUrl : $($_.Exception.Message)")
        }
    }
    throw "GitHub API failed through all routes.$([Environment]::NewLine)$($failures -join [Environment]::NewLine)"
}

# ---------------------------------------------------------------------------
# 显卡枚举（原 Configure.ps1 与 Installer.ps1 各一份，仅注释措辞不同）
# ---------------------------------------------------------------------------

# 查询缓存。同 GitHubProxyLatency，必须在模块内初始化（严格模式）。
$script:CachedVideoControllers = $null

function Get-VideoControllersOnce {
    # 每安装会话只查询一次 Win32_VideoController（CIM/WMI 查询 ~100-500ms）
    if ($null -eq $script:CachedVideoControllers) {
        try {
            $script:CachedVideoControllers = @(Get-CimInstance -ClassName Win32_VideoController -ErrorAction Stop)
        }
        catch {
            try { $script:CachedVideoControllers = @(Get-WmiObject -Class Win32_VideoController -ErrorAction Stop) } catch { $script:CachedVideoControllers = @() }
        }
    }
    return @($script:CachedVideoControllers)
}

function Get-NvidiaVideoControllers {
    $controllers = @(Get-VideoControllersOnce)
    return @($controllers | Where-Object {
        ([string]$_.PNPDeviceID -match '(?i)VEN_10DE') -or
        ([string]$_.AdapterCompatibility -match '(?i)NVIDIA') -or
        ([string]$_.Name -match '(?i)NVIDIA')
    })
}

# ---------------------------------------------------------------------------
# 路径兼容性提醒（原 Configure.ps1 与 Installer.ps1 各一份）
# ---------------------------------------------------------------------------

function Test-PathCompatibilityRisk {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) { return $false }
    foreach ($character in $Path.ToCharArray()) {
        if ([int]$character -gt 127) { return $true }
    }
    return ($Path -match '[^A-Za-z0-9 _:\.\\/\-]')
}

# ⚠️ 取舍：两版**逐字相同**，只有返回值不同 ——
#   Installer 版：命中时 `return $true` / 未命中 `return $false`
#   Configure 版：命中时 `return`（$null）/ 未命中 `return`
# 已核实调用点：Installer.ps1 的两处**使用返回值**（`if (Show-... ) { Pause-Menu }`），
# Configure.ps1 的那处**在语句位置、忽略返回值** ⇒ **保留 Installer 的布尔版**，
# 两个调用方行为都不变。
#
# 插件目录原引用脚本级 `$root`；模块内改用上面算出的 $script:InstallerCommonRoot
# （两种布局下都与调用方的 $root 相等，已核实）。仍保留 -PluginRoot 参数以便显式覆盖。
function Show-PathCompatibilityWarning {
    param([string]$GameExePath, [string]$PluginRoot)
    if ([string]::IsNullOrWhiteSpace($PluginRoot)) { $PluginRoot = $script:InstallerCommonRoot }
    $gameDirectory = if ([string]::IsNullOrWhiteSpace($GameExePath)) { $null } else { Split-Path -Parent $GameExePath }
    $riskyPaths = [Collections.Generic.List[string]]::new()
    if (Test-PathCompatibilityRisk -Path $gameDirectory) { $riskyPaths.Add("游戏目录: $gameDirectory") }
    if (Test-PathCompatibilityRisk -Path $PluginRoot) { $riskyPaths.Add("插件目录: $PluginRoot") }
    if ($riskyPaths.Count -eq 0) { return $false }

    Write-Host ''
    Write-Host '路径兼容性提醒：检测到游戏或插件路径包含中文或特殊符号。' -ForegroundColor Yellow
    foreach ($entry in $riskyPaths) {
        Write-Host "  $entry" -ForegroundColor DarkYellow
    }
    Write-Host '若遇到无法注入、插件不加载或日志目录乱码，建议将游戏和插件移动到仅包含英文、数字、下划线和短横线的路径。' -ForegroundColor DarkGray
    return $true
}

# ---------------------------------------------------------------------------
# JSON 工具
# ---------------------------------------------------------------------------
# ⚠️ 取舍：Configure 的 `Set-JsonProperty` 无返回值，Installer 的
# `Set-JsonPropertyValue` 返回"是否发生了变化"的布尔 —— **语义不同**，
# 审核报告说的"几乎相同"不准确。已核实调用点：
#   Installer 三处**使用**返回值（`if (Set-... ) { $changed = $true }`）
#   Configure 四处**在语句位置、忽略返回值**
# ⇒ **只保留布尔版**（下方），Configure 的四处调用点已改为调用本函数。
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
    if (-not (Test-JsonPropertyValueEqual -Left $property.Value -Right $Value)) {
        $property.Value = $Value
        return $true
    }
    return $false
}

# 内部：判断两个 JSON 属性值是否相等，供上面的 setter 决定"是否需要写回"。
#
# ⚠️ 不能直接用 `-ne` 比较：右侧是数组时 PowerShell 把 `-ne` 当【过滤器】而不是比较。
# `"DllList": []` 经 ConvertFrom-Json 之后属性值是**空字符串**（不是 $null），
# 于是 `'' -ne @('a.dll','b.dll')` 的结果是空数组（布尔化为 $false）⇒ 被判为"相等"
# ⇒ **跳过写入**。所有写 DllList 的调用因此全部静默失效，配置里始终是空表，
# 表现为"安装完成后一个插件都没装上，且反复安装也无效"。
#
# 判定规则：一侧是列表时，另一侧的空字符串/$null 视为**空列表**（JSON 空数组的两种
# 读取结果都表示"空"）；列表按元素逐个比较，标量沿用 `-eq` 语义。
function Test-JsonPropertyValueEqual {
    param(
        [object]$Left,
        [object]$Right
    )
    $leftIsEmpty = ($null -eq $Left) -or ($Left -is [string] -and [string]::IsNullOrEmpty($Left))
    $rightIsEmpty = ($null -eq $Right) -or ($Right -is [string] -and [string]::IsNullOrEmpty($Right))
    $leftIsList = ($Left -is [System.Collections.IList]) -and ($Left -isnot [string])
    $rightIsList = ($Right -is [System.Collections.IList]) -and ($Right -isnot [string])
    if ($leftIsList -or $rightIsList) {
        # ⚠️ 全部用 `@(...)` 包住再取 .Count：`$x = if (...) { @() } else { @() }` 里
        # 空数组经 if 输出后变成 $null，而 `[string[]]$x = $null` **仍然是 $null** ——
        # 强转救不了，`$null.Count` 在 Set-StrictMode 下直接报错。
        # `@($null).Count` 恒为 0，是这里唯一稳的写法（同一族的 PowerShell 数组陷阱）。
        $leftItems = @(if ($leftIsList) { @($Left) })
        $rightItems = @(if ($rightIsList) { @($Right) })
        if ($leftItems.Count -ne $rightItems.Count) { return $false }
        for ($index = 0; $index -lt $leftItems.Count; $index++) {
            if ($leftItems[$index] -ne $rightItems[$index]) { return $false }
        }
        return $true
    }
    if ($leftIsEmpty -and $rightIsEmpty) { return $true }
    return $Left -eq $Right
}

# ---------------------------------------------------------------------------
# INI 工具
# ---------------------------------------------------------------------------
# ⚠️ 审核报告说"两套实现应合并"，但**逐对比对后不是等价的**，如实记录：
#
#   Set-IniValue（Configure）    Set-IniPathValue（Installer）
#   --------------------------   ----------------------------
#   文件不存在 → 创建             文件不存在 → return $false（不写）
#   段头用正则容忍 `[ Sec ]`      段头要求 trim 后精确等于 `[Sec]`
#   总是写盘                      未变化时提前返回 $false（跳过写盘）
#   无返回值                      返回布尔
#
# 已核实 Installer 的全部 15 处调用点**都不使用返回值**（`| Out-Null` 或语句位置），
# 因此"返回布尔"不构成差异；但**"文件不存在时是否创建"与"段头匹配宽容度"是
# 可观察的行为差异** ⇒ 强行统一会改变行为。按任务要求"行为必须完全不变"，
# **两个 setter 均原样保留**（仅位置搬到本模块），差异写在上面这张表里。
#
# getter 则只差"未找到时的返回值"（Configure 返回 $null / Installer 返回 ''），
# 扫描逻辑逐字相同 ⇒ **抽出一份内部扫描**，两个公开函数各自保留原约定。

# 内部：定位 `[Section]` 下 `Key` 的值。找到返回 $true，$Value 为 `=` 右侧 trim 后的内容。
# 语义与两个原实现逐条一致：段名/键名 trim 后比较（PowerShell `-eq`/`-ieq` 均大小写不敏感）、
# 跳过空行与 `;` `#` 注释行、以第一个 `=` 分隔。
function Get-IniValueCore {
    param(
        [string]$Path,
        [string]$Section,
        [string]$Key,
        [ref]$Value
    )
    $Value.Value = $null
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $inSection = $false
    foreach ($line in @(Get-Content -LiteralPath $Path -Encoding UTF8)) {
        $text = [string]$line
        if ($text.Trim() -match '^\[(.+)\]$') {
            $inSection = $matches[1].Trim() -ieq $Section
            continue
        }
        if (-not $inSection) { continue }
        if ($text -match ('^\s*' + [regex]::Escape($Key) + '\s*=\s*(.*)$')) {
            $Value.Value = $matches[1].Trim()
            return $true
        }
    }
    return $false
}

# Configure 契约：未找到 / 文件不存在 ⇒ $null（调用方用 IsNullOrWhiteSpace 判定）
function Get-IniValue {
    param(
        [string]$Path,
        [string]$Section,
        [string]$Key
    )
    $value = $null
    if (-not (Get-IniValueCore -Path $Path -Section $Section -Key $Key -Value ([ref]$value))) { return $null }
    return $value
}

# Installer 契约：未找到 / 文件不存在 ⇒ ''（调用方同样用 IsNullOrWhiteSpace 判定）
function Get-IniPathValue {
    param(
        [string]$Path,
        [string]$Section,
        [string]$Key
    )
    $value = $null
    if (-not (Get-IniValueCore -Path $Path -Section $Section -Key $Key -Value ([ref]$value))) { return '' }
    return $value
}

# Configure 契约：文件不存在则创建；段头正则容忍；总是写盘；无返回值。
function Set-IniValue {
    param(
        [string]$Path,
        [string]$Section,
        [string]$Key,
        [string]$Value
    )
    $lines = [System.Collections.Generic.List[string]]::new()
    if (Test-Path -LiteralPath $Path) {
        foreach ($line in Get-Content -LiteralPath $Path -Encoding UTF8) {
            $lines.Add($line)
        }
    }
    $sectionStart = -1
    $sectionEnd = $lines.Count
    for ($index = 0; $index -lt $lines.Count; $index++) {
        if ($lines[$index] -match '^\s*\[(.+)\]\s*$') {
            if ($sectionStart -ge 0) {
                $sectionEnd = $index
                break
            }
            if ($matches[1] -eq $Section) {
                $sectionStart = $index
            }
        }
    }
    if ($sectionStart -lt 0) {
        if ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -ne '') {
            $lines.Add('')
        }
        $lines.Add("[$Section]")
        $lines.Add("$Key = $Value")
    }
    else {
        $keyIndex = -1
        for ($index = $sectionStart + 1; $index -lt $sectionEnd; $index++) {
            if ($lines[$index] -match ('^\s*' + [regex]::Escape($Key) + '\s*=')) {
                $keyIndex = $index
                break
            }
        }
        if ($keyIndex -ge 0) {
            $lines[$keyIndex] = "$Key = $Value"
        }
        else {
            $lines.Insert($sectionEnd, "$Key = $Value")
        }
    }
    [IO.File]::WriteAllLines($Path, $lines, [Text.UTF8Encoding]::new($false))
}

# Installer 契约：文件不存在 → $false；未变化 → $false（跳过写盘）；发生变化 → $true。
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
