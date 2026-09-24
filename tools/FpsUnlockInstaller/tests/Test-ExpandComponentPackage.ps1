# Expand-ComponentPackage 真实解压测试（2026-09-23）
#
# 审核项 ④ 标注为"需跑一次手动选 .7z 的安装"才能验证 —— 但该函数的风险点
# （7z 解压、退出码、静默失败防护）**可以直接测**，不必跑整个安装器。
#
# 做法：AST 只抽取 `Expand-ComponentPackage`，用真实 7z.exe 造包后实跑。
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$src = Join-Path $repoRoot 'tools\FpsUnlockInstaller\Configure.ps1'
$ast = [System.Management.Automation.Language.Parser]::ParseFile($src, [ref]$null, [ref]$null)
$def = $ast.FindAll({
    param($n)
    $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
    $n.Name -eq 'Expand-ComponentPackage'
}, $true) | Select-Object -First 1
if (-not $def) { throw '找不到 Expand-ComponentPackage' }
Invoke-Expression $def.Extent.Text

# 该函数依赖脚本级 $root（7z 回退候选路径）与本地化函数 —— 用桩替代，
# 因为本测试只关心"是否解出内容 / 是否如实报错"，不关心报错文案。
$root = $repoRoot
function Convert-InstallerText { param([string]$Value) return $Value }

$ok = 0; $bad = 0
function Chk($cond, $msg) {
    if ($cond) { Write-Host "  ok:   $msg"; $script:ok++ }
    else       { Write-Host "  FAIL: $msg"; $script:bad++ }
}

$sz = 'C:\Program Files\7-Zip\7z.exe'
if (-not (Test-Path -LiteralPath $sz)) { throw "缺少 7z.exe: $sz" }

$sandbox = Join-Path $env:TEMP ('expkg_test_' + [guid]::NewGuid().ToString('N'))
$payload = Join-Path $sandbox 'payload'
New-Item -ItemType Directory -Path (Join-Path $payload 'Bridge') -Force | Out-Null
Set-Content -LiteralPath (Join-Path $payload 'Bridge\Dx11FsrBridge.dll') -Value 'fake-dll' -Encoding ASCII
Set-Content -LiteralPath (Join-Path $payload 'readme.txt') -Value 'hello' -Encoding ASCII

$zip = Join-Path $sandbox 'pkg.zip'
$sevenZip = Join-Path $sandbox 'pkg.7z'
$broken = Join-Path $sandbox 'broken.7z'
Compress-Archive -Path (Join-Path $payload '*') -DestinationPath $zip -Force
& $sz a -t7z $sevenZip (Join-Path $payload '*') -y | Out-Null
if ($LASTEXITCODE -ne 0) { throw '造 7z 测试包失败' }
# 损坏包：截断一个合法 7z 的前 64 字节
$bytes = [IO.File]::ReadAllBytes($sevenZip)
[IO.File]::WriteAllBytes($broken, $bytes[0..63])

# ---- 1) .zip ----
$d1 = Join-Path $sandbox 'out_zip'
Expand-ComponentPackage -PackagePath $zip -Destination $d1
Chk (Test-Path -LiteralPath (Join-Path $d1 'Bridge\Dx11FsrBridge.dll')) 'zip：解出 Bridge\Dx11FsrBridge.dll'
Chk (Test-Path -LiteralPath (Join-Path $d1 'readme.txt')) 'zip：解出 readme.txt'

# ---- 2) .7z（审核项的核心）----
$d2 = Join-Path $sandbox 'out_7z'
Expand-ComponentPackage -PackagePath $sevenZip -Destination $d2
Chk (Test-Path -LiteralPath (Join-Path $d2 'Bridge\Dx11FsrBridge.dll')) '7z：解出 Bridge\Dx11FsrBridge.dll'
Chk (Test-Path -LiteralPath (Join-Path $d2 'readme.txt')) '7z：解出 readme.txt'
Chk ((Get-ChildItem -LiteralPath $d2 -Force).Count -gt 0) '7z：解压目录非空（静默失败防护）'

# ---- 3) 损坏的 .7z 必须抛错（不能静默成功）----
$d3 = Join-Path $sandbox 'out_broken'
$threw = $false
try { Expand-ComponentPackage -PackagePath $broken -Destination $d3 } catch { $threw = $true }
Chk $threw '损坏 7z：如实抛错（不静默成功）'
$emptyOrMissing = (-not (Test-Path -LiteralPath $d3)) -or ((Get-ChildItem -LiteralPath $d3 -Force -ErrorAction SilentlyContinue).Count -eq 0)
Chk $emptyOrMissing '损坏 7z：目标目录没有残留半成品'

# ---- 4) 不支持的扩展名必须抛错 ----
$threw2 = $false
try { Expand-ComponentPackage -PackagePath (Join-Path $sandbox 'x.rar') -Destination (Join-Path $sandbox 'out_rar') } catch { $threw2 = $true }
Chk $threw2 '不支持的格式：如实抛错'

Remove-Item -LiteralPath $sandbox -Recurse -Force -ErrorAction SilentlyContinue
Write-Host ''
Write-Host ("  通过 {0} / 失败 {1}" -f $ok, $bad)
if ($bad -gt 0) { exit 1 }
