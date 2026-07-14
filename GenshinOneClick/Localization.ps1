Set-StrictMode -Version Latest

function Get-InstallerDefaultLanguage {
    if ([Globalization.CultureInfo]::CurrentUICulture.TwoLetterISOLanguageName -eq 'zh') { return 'zh-CN' }
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
        '安装状态' = 'Status'
        '反虚化 / 隐藏 UID' = 'Anti-Mosaic / Hide UID'
        '语言 / Language' = 'Language / 语言'
        '  1. 中文' = '  1. Chinese'
        '  2. English' = '  2. English'
        '  8. 语言 / Language' = '  8. Language / 语言'
        '  0. 返回上一层' = '  0. Back'
        '  0. 退出' = '  0. Exit'
        '关于 / 作者主页' = 'About / Author Pages'
        '停止加载模块' = 'Disable Modules'
        '安装 / 更新模块' = 'Install / Update Modules'
        '安装必需组件' = 'Install Required Components'
        '恢复所有插件默认配置' = 'Restore All Plugin Defaults'
        '一键配置完成。' = 'Configuration complete.'
        'DLL 加载顺序:' = 'DLL load order:'
        'FPS Unlocker 尚未安装。' = 'FPS Unlocker is not installed.'
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
        '请输入需要停止加载的模块 ID' = 'Enter the module ID to disable'
        '安装模块：' = 'Install modules:'
        '停止加载模块：' = 'Disable modules:'
        '全部可选模块（默认，直接回车）' = 'All optional modules (default; press Enter)'
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
        '所选模块已停止加载，本地文件和配置均已保留。' = 'Selected modules have been disabled. Local files and settings were kept.'
        '请输入帧率上限（直接回车使用 ' = 'Enter the FPS limit (press Enter to use '
        '请输入帧率上限（当前 ' = 'Enter the FPS limit (current: '
        '，直接回车保持不变）' = '; press Enter to keep it)'
        '作品: ' = 'Projects: '
        '已安装 ' = 'Installed '
        '游戏: ' = 'Game: '
        '帧率上限: ' = 'FPS limit: '
        '  1. 安装 / 更新模块' = '  1. Install / Update Modules'
        '  2. 停止加载模块' = '  2. Disable Modules'
        '  3. 更换游戏目录' = '  3. Change Game Directory'
        '  4. 修改帧率上限' = '  4. Change FPS Limit'
        '  5. 启动游戏' = '  5. Launch Game'
        '  6. 恢复所有插件默认配置' = '  6. Restore All Plugin Defaults'
        '  7. 关于 / 作者主页' = '  7. About / Author Pages'
    }
    foreach ($entry in $replacements.GetEnumerator()) { $text = $text.Replace($entry.Key, $entry.Value) }
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
