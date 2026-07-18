param(
    [string[]]$SourceFiles = @(
        'GenshinOneClick\Installer.ps1',
        'GenshinOneClick\Configure.ps1',
        'GenshinOneClick\Apply-PackageUpdate.ps1'
    )
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $PSCommandPath) '..'))
. (Join-Path $repositoryRoot 'GenshinOneClick\Localization.ps1')
Initialize-InstallerLocalization -Language 'en-US'

$remaining = [Collections.Generic.List[object]]::new()
$intentionalChinese = @(
    '^(\u662F|\u5426)$',
    '^\u539F\u795E(\u6574\u5408\u7248)?\.lnk$',
    '^(crosire / )?\u526A\u5200\u59B9\u4E3D\u4E3D$',
    '\u539F\u795E\u89E3\u5E27FSR\u63D2\u4EF6\u5305Lite_',
    '^\u539F\u795E\u89E3\u5E27FSR\u63D2\u4EF6\u5305Lite_\*$',
    '^\u8BED\u8A00 / Language$',
    '^  9\. \u8BED\u8A00 / Language$',
    '\$\{?ActionName\}?'
)
foreach ($relativePath in $SourceFiles) {
    $path = Join-Path $repositoryRoot $relativePath
    $tokens = $null
    $parseErrors = $null
    [Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$parseErrors) | Out-Null
    if (@($parseErrors).Count -gt 0) {
        throw "PowerShell parse failed: $relativePath"
    }

    foreach ($token in $tokens) {
        if ($null -eq $token.PSObject.Properties['Value'] -or $null -eq $token.Value) { continue }
        $sourceText = [string]$token.Value
        if ($sourceText -notmatch '[\u4e00-\u9fff]') { continue }
        if (@($intentionalChinese | Where-Object { $sourceText -match $_ }).Count -gt 0) { continue }
        $translatedText = Convert-InstallerText -Value $sourceText
        if ($translatedText -match '[\u4e00-\u9fff]') {
            $remaining.Add([pscustomobject]@{
                File = $relativePath
                Line = $token.Extent.StartLineNumber
                Source = $sourceText
                Result = $translatedText
            })
        }
    }
}

function ConvertFrom-CodePoints {
    param([int[]]$Values)
    return -join @($Values | ForEach-Object { [char]$_ })
}

$actionNames = @(
    (ConvertFrom-CodePoints @(0x5B89, 0x88C5)),
    (ConvertFrom-CodePoints @(0x66F4, 0x65B0)),
    (ConvertFrom-CodePoints @(0x505C, 0x6B62, 0x52A0, 0x8F7D))
)
$selectPrefix = ConvertFrom-CodePoints @(0x8BF7, 0x8F93, 0x5165, 0x9700, 0x8981)
$moduleSuffix = ConvertFrom-CodePoints @(0x7684, 0x6A21, 0x5757)
$moduleLabel = ConvertFrom-CodePoints @(0x6A21, 0x5757, 0xFF1A)
foreach ($actionName in $actionNames) {
    foreach ($sourceText in @("${selectPrefix}${actionName}${moduleSuffix} ID", "$actionName $moduleLabel")) {
        $translatedText = Convert-InstallerText -Value $sourceText
        if ($translatedText -match '[\u4e00-\u9fff]') {
            $remaining.Add([pscustomobject]@{
                File = 'GenshinOneClick\Installer.ps1'
                Line = 0
                Source = $sourceText
                Result = $translatedText
            })
        }
    }
}

$remaining |
    Sort-Object File, Line |
    Format-Table File, Line, Source, Result -Wrap -AutoSize |
    Out-String -Width 240 |
    Microsoft.PowerShell.Utility\Write-Host

if ($remaining.Count -gt 0) { exit 1 }
Microsoft.PowerShell.Utility\Write-Host 'FPS Unlock English localization coverage: PASS'
