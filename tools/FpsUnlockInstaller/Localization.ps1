Set-StrictMode -Version Latest

function Get-InstallerDefaultLanguage {
    $cultures = @(
        [Globalization.CultureInfo]::CurrentUICulture,
        [Globalization.CultureInfo]::CurrentCulture
    )
    foreach ($culture in $cultures) {
        if ($null -ne $culture -and $culture.TwoLetterISOLanguageName -eq 'zh') { return 'zh-CN' }
    }
    return 'en-US'
}

function Get-InstallerLanguage {
    param(
        [ValidateSet('Auto', 'zh-CN', 'en-US')]
        [string]$RequestedLanguage = 'Auto',
        [string]$StatePath
    )

    if ($RequestedLanguage -ne 'Auto') { return $RequestedLanguage }
    if (-not [string]::IsNullOrWhiteSpace($StatePath) -and (Test-Path -LiteralPath $StatePath -PathType Leaf)) {
        try {
            $state = Get-Content -LiteralPath $StatePath -Raw -Encoding UTF8 | ConvertFrom-Json
            if ([string]$state.Language -in @('zh-CN', 'en-US')) { return [string]$state.Language }
        }
        catch { }
    }
    return Get-InstallerDefaultLanguage
}

function Initialize-InstallerLocalization {
    param(
        [ValidateSet('zh-CN', 'en-US')]
        [string]$Language
    )
    $script:InstallerLanguage = $Language
}

function Convert-InstallerText {
    param([AllowNull()][object]$Value)
    if ($null -eq $Value -or $script:InstallerLanguage -ne 'en-US' -or $Value -isnot [string]) { return $Value }

    $text = [string]$Value
    $exact = @{
        '按回车键继续' = 'Press Enter to continue'
        '请输入 y 或 n。' = 'Please enter y or n.'
        '请输入选项' = 'Enter an option'
        '请输入数值' = 'Enter a value'
        '无效选项。' = 'Invalid option.'
        '无效选项，请重新输入。' = 'Invalid option. Please try again.'
        '未找到目录或文件。' = 'The selected file or directory was not found.'
        '正在检查更新，请稍候...' = 'Checking for updates...'
        '检查更新失败，请稍后重试。' = 'Update check failed. Please try again later.'
        '当前已经是最新版本。' = 'The latest version is already installed.'
        '正在安装，请稍候...' = 'Installing...'
        '安装完成。' = 'Installation complete.'
        '安装失败，请重试。' = 'Installation failed. Please try again.'
        '请输入大于 0 的整数。' = 'Enter an integer greater than 0.'
        '已安装' = 'Installed'
        '未安装' = 'Not installed'
        '模块 ID' = 'Module ID'
        '插件名' = 'Plugin'
        '作者' = 'Author'
        '当前版本' = 'Version'
        'OptiScaler 官方资产名称不符合预期: ' = 'Unexpected OptiScaler official asset name: '
        'OptiScaler 官方压缩包 SHA256 校验失败。实际值: ' = 'OptiScaler official archive SHA256 verification failed. Actual value: '
        'OptiScaler 版本不是 ' = 'OptiScaler version is not '
        '安装状态' = 'Status'
        '反虚化 / 隐藏 UID' = 'Anti-Mosaic / Hide UID'
        '语言 / Language' = 'Language / 语言'
        '  1. 中文' = '  1. Chinese'
        '  2. English' = '  2. English'
        '  8. 语言 / Language' = '  8. Language / 语言'
        '  9. 语言 / Language' = '  9. Language / 语言'
        '  0. 返回上一层' = '  0. Back'
        '  0. 退出' = '  0. Exit'
        '关于 / 作者主页' = 'About / Author Pages'
        '停止加载模块' = 'Disable Modules'
        '停止加载 / 卸载模块' = 'Disable / Uninstall Modules'
        '安装模块' = 'Install Modules'
        '更新模块' = 'Update Modules'
        '安装 / 更新模块' = 'Install / Update Modules'
        '安装必需组件' = 'Install Required Components'
        '恢复所有插件默认配置' = 'Restore All Plugin Defaults'
        '一键配置完成。' = 'Configuration complete.'
        'DLL 加载顺序:' = 'DLL load order:'
        'FPS Unlocker 尚未安装。' = 'FPS Unlocker is not installed.'
        '最新 Release 中没有找到 FPS Unlock Lite 更新包。' = 'The latest release does not contain an FPS Unlock Lite package.'
        '当前包缺少自更新替换程序，请手动下载最新发布包。' = 'The self-update helper is missing. Download the latest package manually.'
        '模块更新失败，请查看错误日志。' = 'Module update failed. Check the error log.'
        '反虚化组件已同步为当前发布包版本。' = 'The anti-mosaic component now matches the current package.'
        'ReShade 与 RenoDX 已同步为当前发布包版本。' = 'ReShade and RenoDX now match the current package.'
        '所选模块更新完成。' = 'Selected modules updated.'
        '未知' = 'Unknown'
        '安装' = 'Install'
        '更新' = 'Update'
        '停止加载' = 'Disable'
        '  1. 从官方 GitHub 自动下载最新版（推荐）' = '  1. Download the latest official GitHub release (recommended)'
        '  2. 使用已经手动下载的文件或目录' = '  2. Use a manually downloaded file or folder'
        '  3. 使用当前目录中已有的版本' = '  3. Use the version already in this folder'
        '路径兼容性提醒：检测到游戏或插件路径包含中文或特殊符号。' = 'Path compatibility warning: the game or plugin path contains non-ASCII or special characters.'
        '若遇到无法注入、插件不加载或日志目录乱码，建议将游戏和插件移动到仅包含英文、数字、下划线和短横线的路径。' = 'If injection, plugin loading, or log paths fail, move the game and plugin to paths containing only English letters, numbers, underscores, and hyphens.'
        '请选择需要安装的组件：' = 'Select the components to install:'
        '启用 FSR Bridge + OptiScaler' = 'Enable FSR Bridge + OptiScaler'
        '启用反虚化/隐藏 UID' = 'Enable Anti-Mosaic / Hide UID'
        '启用 ReShade + RenoDX HDR' = 'Enable ReShade + RenoDX HDR'
        '已安装内置 NVIDIA DLSS 超分组件。' = 'Installed the bundled NVIDIA DLSS upscaling component.'
        '已从 NVIDIA 官方来源安装 DLSS 超分组件。' = 'Installed the DLSS upscaling component from an official NVIDIA source.'
        '已从官方来源安装 ReShade 与效果库。' = 'Installed ReShade and shader packages from their official upstream sources.'
        'ReShade 与效果库安装来源：' = 'ReShade and shader package source:'
        '  1. 从 ReShade 及效果作者的官方来源下载（推荐）' = '  1. Download from the official ReShade and shader author sources (recommended)'
        '  2. 使用安装包内置 ReShade 与效果库（支持离线安装）' = '  2. Use the bundled ReShade and shader packages (offline installation supported)'
        '  2. 仅使用安装包内置 ReShade + RenoDX（不含效果库）' = '  2. Use only the bundled ReShade + RenoDX (shader packages not included)'
        '请输入选项，直接回车使用官方下载' = 'Enter an option, or press Enter to download from official sources'
    }
    if ($exact.ContainsKey($text)) { return $exact[$text] }

    $replacements = [ordered]@{
        '模块 ID' = 'Module ID'
        '插件名' = 'Plugin'
        '作者' = 'Author'
        '当前版本' = 'Version'
        '安装状态' = 'Status'
        '已安装' = 'Installed'
        '未安装' = 'Not installed'
        '反虚化 / 隐藏 UID' = 'Anti-Mosaic / Hide UID'
        '原神插件管理器' = 'Genshin Plugin Manager'
        '请选择包含 YuanShen.exe 或 GenshinImpact.exe 的游戏目录' = 'Select the game folder containing YuanShen.exe or GenshinImpact.exe'
        '选择游戏目录' = 'Select Game Folder'
        '当前游戏目录' = 'Current game directory'
        '游戏目录' = 'Game directory'
        '游戏程序' = 'Game executable'
        '插件目录' = 'Plugin directory'
        '尚未选择有效的原神游戏目录' = 'No valid Genshin Impact game directory has been selected'
        '输入新路径，或直接按回车打开目录选择窗口。' = 'Enter a new path, or press Enter to open the folder picker.'
        '不输入路径直接按回车，可打开目录选择窗口。' = 'Press Enter without a path to open the folder picker.'
        '输入原神安装目录或游戏程序的完整路径。' = 'Enter the full path to the Genshin Impact folder or game executable.'
        '输入 0 退出。' = 'Enter 0 to exit.'
        '游戏路径' = 'Game path'
        '该路径中没有找到 YuanShen.exe 或 GenshinImpact.exe。' = 'YuanShen.exe or GenshinImpact.exe was not found at this path.'
        '这会清除所有插件的当前设置，并恢复到首次运行状态。' = 'This clears current settings for all plugins and restores first-run defaults.'
        '游戏路径和当前插件加载状态会保留。' = 'The game path and current module load state will be kept.'
        '输入 Y 确认，其他输入返回' = 'Enter Y to confirm; any other input returns'
        '恢复默认配置失败，请重试。' = 'Failed to restore default settings. Please try again.'
        '所有插件配置已恢复到首次运行状态。' = 'All plugin settings have been restored to first-run defaults.'
        '当前帧率上限' = 'Current FPS limit'
        '请输入需要安装的模块 ID' = 'Enter the module ID to install'
        '请输入需要更新的模块 ID' = 'Enter the module ID to update'
        '请输入需要停止加载的模块 ID' = 'Enter the module ID to disable'
        '安装模块：' = 'Install modules:'
        '更新模块：' = 'Update modules:'
        '停止加载模块：' = 'Disable modules:'
        ' 模块：' = ' modules:'
        '请输入 ' = 'Enter '
        ' 安装文件或解压目录。' = ' installer file or extracted directory.'
        '直接按回车可打开文件选择窗口，输入 0 返回。' = 'Press Enter to open the file picker, or enter 0 to go back.'
        '选择 ' = 'Select '
        ' 安装文件' = ' installer file'
        '尚未安装。' = ' is not installed.'
        '发现新版本 ' = 'New version found: '
        '是否现在安装？直接回车确认，输入 n 取消' = 'Install now? Press Enter to confirm, or enter n to cancel'
        '安装方式：' = ' installation method:'
        '联网安装' = 'Install online'
        '检查更新' = 'Check for updates'
        '手动安装' = 'Manual install'
        'NVIDIA DLSS 组件安装失败，可在“安装 / 更新模块”中重试。' = 'NVIDIA DLSS component installation failed. Retry from “Install / Update Modules”.'
        'NVIDIA DLSS 组件安装失败，可在“安装模块”或“更新模块”中重试。' = 'NVIDIA DLSS component installation failed. Retry from Install Modules or Update Modules.'
        '正在检查管理脚本和发行资源更新...' = 'Checking manager scripts and release resources...'
        '检查脚本更新失败: ' = 'Script update check failed: '
        '管理脚本和发行资源已经是最新版本 ' = 'Manager scripts and release resources are already current: '
        '已下载管理脚本 ' = 'Downloaded manager scripts '
        '，当前窗口关闭后自动替换并重新打开。' = '; files will be replaced and the manager reopened after this window closes.'
        '管理脚本更新失败: ' = 'Manager self-update failed: '
        '模块 ' = 'Module '
        ' 尚未安装，已跳过。' = ' is not installed and was skipped.'
        '正在更新所选模块，请稍候...' = 'Updating selected modules...'
        '管理脚本 / 发行资源' = 'Manager Scripts / Release Resources'
        ' 获取方式：' = ' source:'
        '检测到 ' = 'Detected '
        '，正在补齐 DLSS 超分组件...' = '; installing the required DLSS upscaling component...'
        '正在从 NVIDIA 官方 Streamline ' = 'Downloading the DLSS component from NVIDIA Streamline '
        ' 下载 DLSS 组件...' = '...'
        '正在从官方发行版下载 FPS Unlocker ' = 'Downloading FPS Unlocker from the official release '
        '正在从官方发行版下载 OptiScaler ' = 'Downloading OptiScaler from the official release '
        '官方页面: ' = 'Official page: '
        '请输入 unlockfps_nc.exe 或自包含 ZIP 的路径' = 'Enter the path to unlockfps_nc.exe or a self-contained ZIP'
        '请输入 OptiScaler 完整解压目录、ZIP 或 7z 路径' = 'Enter the path to a fully extracted OptiScaler folder, ZIP, or 7z package'
        '请输入 YuanShen.exe、GenshinImpact.exe 或游戏目录路径' = 'Enter the path to YuanShen.exe, GenshinImpact.exe, or the game folder'
        '（直接回车使用 ' = ' (press Enter to use '
        '）' = ')'
        '  A. 全部可选模块（默认，直接回车）' = '  A. All optional modules (default; press Enter)'
        '全部可选模块（默认，直接回车）' = 'All optional modules (default; press Enter)'
        '所有文件 (*.*)' = 'All files (*.*)'
        'FSR Bridge、AntiPlayerMosaic、安装管理脚本' = 'FSR Bridge, AntiPlayerMosaic, installer manager scripts'
        '缺少文件: ' = 'Missing file: '
        '缺少目录: ' = 'Missing folder: '
        '指定的文件或目录不存在: ' = 'The specified file or folder does not exist: '
        '无法查询 ' = 'Could not query '
        'OptiScaler 官方资产名称不符合预期: ' = 'Unexpected OptiScaler official asset name: '
        'OptiScaler 官方压缩包 SHA256 校验失败。实际值: ' = 'OptiScaler official archive SHA256 verification failed. Actual value: '
        'OptiScaler 版本不是 ' = 'OptiScaler version is not '
        ' 官方发行版，请检查网络或改用手动下载。' = ' official releases. Check your network connection or use a manual download.'
        ' 的最新发行版中没有找到需要的文件。' = ' latest release does not contain the required file.'
        '下载失败，请重试或改用手动下载。' = 'Download failed. Try again or use a manual download.'
        'NVIDIA DLSS 文件签名验证失败: ' = 'NVIDIA DLSS file signature verification failed: '
        'NVIDIA Streamline 官方包中没有找到生产版 nvngx_dlss.dll。' = 'The official NVIDIA Streamline package does not contain a production nvngx_dlss.dll.'
        '系统中没有 tar.exe，无法解压 7z；请手动解压后选择解压目录。' = 'tar.exe is unavailable, so the 7z package cannot be extracted. Extract it manually and select the extracted folder.'
        '7z 解压失败，退出代码: ' = '7z extraction failed with exit code: '
        '不支持的压缩包格式: ' = 'Unsupported archive format: '
        '手动选择的文件不是官方 FPS Unlocker 可执行文件。' = 'The selected file is not an official FPS Unlocker executable.'
        '所选目录或 ZIP 中没有找到 FPS Unlocker。' = 'FPS Unlocker was not found in the selected folder or ZIP.'
        '所选 OptiScaler 包中没有找到 OptiScaler.dll。' = 'OptiScaler.dll was not found in the selected OptiScaler package.'
        'OptiScaler 包不完整：缺少 amd_fidelityfx_upscaler_dx12.dll。' = 'The OptiScaler package is incomplete: amd_fidelityfx_upscaler_dx12.dll is missing.'
        '手动安装源不能选择当前已安装的 payload\OptiScaler 目录；请选择独立的解压目录或压缩包。' = 'The currently installed payload\OptiScaler folder cannot be used as a manual source. Select a separate extracted folder or archive.'
        '游戏路径不存在: ' = 'Game path does not exist: '
        '目录中没有找到 YuanShen.exe 或 GenshinImpact.exe: ' = 'YuanShen.exe or GenshinImpact.exe was not found in the folder: '
        '不支持的游戏程序: ' = 'Unsupported game executable: '
        '未提供游戏路径。' = 'No game path was provided.'
        'OptiScaler 依赖不完整：请将完整发行包解压到 ' = 'OptiScaler dependencies are incomplete. Extract the complete release package to '
        'OptiScaler 超分文件清单无效: ' = 'Invalid OptiScaler upscaling file manifest: '
        '管理脚本更新包 SHA-256 校验失败。' = 'Manager update package SHA-256 verification failed.'
        '更新包缺少文件: ' = 'The update package is missing: '
        '更新包包含不应内置的第三方组件: ' = 'The update package contains a third-party component that must not be bundled: '
        '安装' = 'Install'
        '更新' = 'Update'
        '停止加载' = 'Disable'
        '所选模块已停止加载，本地文件和配置均已保留。' = 'Selected modules have been disabled. Local files and settings were kept.'
        '请输入帧率上限（直接回车使用 ' = 'Enter the FPS limit (press Enter to use '
        '请输入帧率上限（当前 ' = 'Enter the FPS limit (current: '
        '，直接回车保持不变）' = '; press Enter to keep it)'
        '作品: ' = 'Projects: '
        '已安装 ' = 'Installed '
        '游戏: ' = 'Game: '
        '帧率上限: ' = 'FPS limit: '
        '  1. 安装 / 更新模块' = '  1. Install / Update Modules'
        '  1. 安装模块' = '  1. Install Modules'
        '  2. 更新模块' = '  2. Update Modules'
        '  3. 停止加载 / 卸载模块' = '  3. Disable / Uninstall Modules'
        '  4. 更换游戏目录' = '  4. Change Game Directory'
        '  5. 修改帧率上限' = '  5. Change FPS Limit'
        '  6. 启动游戏' = '  6. Launch Game'
        '  7. 恢复所有插件默认配置' = '  7. Restore All Plugin Defaults'
        '  8. 关于 / 作者主页' = '  8. About / Author Pages'
        '  9. 语言 / Language' = '  9. Language / 语言'
        '  2. 停止加载模块' = '  2. Disable Modules'
        '  3. 更换游戏目录' = '  3. Change Game Directory'
        '  4. 修改帧率上限' = '  4. Change FPS Limit'
        '  5. 启动游戏' = '  5. Launch Game'
        '  6. 恢复所有插件默认配置' = '  6. Restore All Plugin Defaults'
        '  7. 关于 / 作者主页' = '  7. About / Author Pages'
    }
    foreach ($entry in @($replacements.GetEnumerator() | Sort-Object { $_.Key.Length } -Descending)) {
        $text = $text.Replace($entry.Key, $entry.Value)
    }
    return $text
}

function Write-Host {
    [CmdletBinding()]
    param(
        [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
        [object[]]$Object,
        [ConsoleColor]$ForegroundColor,
        [ConsoleColor]$BackgroundColor,
        [switch]$NoNewline,
        [object]$Separator
    )

    $parameters = @{}
    foreach ($name in @('ForegroundColor', 'BackgroundColor', 'NoNewline', 'Separator')) {
        if ($PSBoundParameters.ContainsKey($name)) { $parameters[$name] = $PSBoundParameters[$name] }
    }
    $translated = @($Object | ForEach-Object { Convert-InstallerText -Value $_ })
    Microsoft.PowerShell.Utility\Write-Host -Object $translated @parameters
}

function Read-Host {
    [CmdletBinding()]
    param(
        [Parameter(Position = 0)]
        [string]$Prompt,
        [switch]$AsSecureString
    )

    $parameters = @{}
    if ($PSBoundParameters.ContainsKey('AsSecureString')) { $parameters.AsSecureString = $true }
    Microsoft.PowerShell.Utility\Read-Host -Prompt (Convert-InstallerText -Value $Prompt) @parameters
}
