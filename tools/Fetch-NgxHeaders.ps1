<#
.SYNOPSIS
    拉取 NVIDIA NGX（DLSS）头文件到 Dx11FsrBridge/third_party/ngx/。

.DESCRIPTION
    这些头文件是 NVIDIA 专有许可（LicenseRef-NvidiaProprietary），与仓库的
    GPL-3.0-or-later 不兼容，因此**不入库**（已 gitignore）。

    ── 只拉 2 个文件 ──────────────────────────────────────────────────────
    Dx11FsrBridge/NgxBackend.cpp:57-58 只 include 这两个头：
        nvsdk_ngx_defs.h    —— 类型、NVSDK_NGX_Parameter_* 字符串键、结果码
        nvsdk_ngx_params.h  —— NVSDK_NGX_Parameter 纯虚接口

    nvsdk_ngx.h 故意不拉：它用 extern "C" 且无 dllexport 声明入口，include 后
    会去找 __imp_NVSDK_NGX_D3D11_* ⇒ 直接链接失败；所有入口走 GetProcAddress。
    nvsdk_ngx_helpers*.h 三个文件全仓库无人 include（已 grep 确认），故也不拉。

    ── 固定 commit，不跟分支 ─────────────────────────────────────────────
    2026-11-09（时间炸弹修复）：原实现默认 -Ref main，即**分支名**。分支会
    移动：上游一更新头文件，下面的哈希校验就会失败关闭 ⇒ CI 的 NGX ON job 必红，
    直到有人手动改表。改为固定 commit 后，同一个 URL 永远返回同样的字节。

    本脚本锁的是 commit a291cc7（提交信息 "DLSS 310.7.0 SDK"），对应 NGX API
    1.5.0（nvsdk_ngx_defs.h: NVSDK_NGX_VERSION_API_MACRO = 0x0000015）。

    ── 怎么更新到更新的 SDK ─────────────────────────────────────────────
      1. 在 NVIDIA/DLSS 找到目标 commit 的完整 40 位 SHA，改下面的 $Commit；
      2. 跑 pwsh -File tools/Fetch-NgxHeaders.ps1 -Update
         （只打印新哈希，不落盘、不与期望值比对）；
      3. 人工核对打印出来的哈希确实是预期的那批，再更新 $expected 表；
      4. 正常跑一次，确认两个文件都 ok。
    改 $Commit 必须同时改 $expected，否则下一次运行必然失败关闭（这是有意的）。

.PARAMETER Update
    重新计算并打印当前下载内容的 SHA-256 表，供人工核对后更新本脚本。
    此模式仍校验文件大小下限，但不与期望值比对，也不落盘。

.EXAMPLE
    pwsh -File tools/Fetch-NgxHeaders.ps1
    pwsh -File tools/Fetch-NgxHeaders.ps1 -Update
#>
[CmdletBinding()]
param(
    [switch]$Update
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# 固定到 DLSS 310.7.0 SDK 的 commit。
# ★ 不用分支名（main）：上游一更新头文件，哈希校验就会失败关闭 ⇒ CI 必红。
# 改这里必须同时更新 $expected 表（用 -Update 模式重算）。
$Commit = 'a291cc7d2cc642a51566f3dfd5376f635cd1b284'
$baseUri = "https://raw.githubusercontent.com/NVIDIA/DLSS/$Commit/include"

# ── 逐文件 SHA-256 期望值（失败关闭）──────────────────────────────────────
# 只列 NgxBackend.cpp 真正 include 的 2 个文件；helpers 系列无人 include，不拉。
# 改表前必须：① 用 -Update 打印新值；② 人工核对 diff 确认是预期更新；③ 再改这里。
$expected = @{
    'nvsdk_ngx_defs.h'   = 'ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e'
    'nvsdk_ngx_params.h' = '943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc'
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$destDir  = Join-Path $repoRoot 'Dx11FsrBridge/third_party/ngx'

Write-Host "Fetching NGX headers"
Write-Host "  commit: $Commit"
Write-Host "  dest  : $destDir"
if ($Update) { Write-Host "  mode  : UPDATE (prints new hashes, does not verify)" }

if (-not (Test-Path -LiteralPath $destDir)) {
    New-Item -ItemType Directory -Path $destDir -Force | Out-Null
}

# 下载到临时目录，全部校验通过后才落盘
$tmpDir = Join-Path ([IO.Path]::GetTempPath()) ("ngx-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmpDir -Force | Out-Null

$downloaded = @{}
$newHashes  = @{}

try {
    foreach ($name in ($expected.Keys | Sort-Object)) {
        $uri    = "$baseUri/$name"
        $tmpOut = Join-Path $tmpDir $name

        Write-Host "  get   $name"
        Invoke-WebRequest -Uri $uri -OutFile $tmpOut -UseBasicParsing

        $size = (Get-Item -LiteralPath $tmpOut).Length
        if ($size -lt 256) {
            throw "$name came back as $size bytes, which looks like an error page rather than a header"
        }

        $hash = (Get-FileHash -LiteralPath $tmpOut -Algorithm SHA256).Hash.ToLowerInvariant()
        $newHashes[$name] = $hash

        if (-not $Update) {
            $want = $expected[$name]
            if ($hash -ne $want) {
                throw @"
SHA-256 mismatch for $name
  expected: $want
  actual  : $hash
Refusing to install anything. The pinned commit is supposed to be immutable, so a
mismatch means either this table was written for a different commit (fix it with
-Update after reviewing the diff) or the download was tampered with (investigate).
"@
            }
        }

        $downloaded[$name] = $tmpOut
    }

    if ($Update) {
        Write-Host ""
        Write-Host "=== new SHA-256 table (verify manually before committing) ==="
        foreach ($name in ($newHashes.Keys | Sort-Object)) {
            Write-Host ("    '{0}' = '{1}'" -f $name, $newHashes[$name])
        }
        Write-Host ""
        Write-Host "Not installing in -Update mode."
        return
    }

    foreach ($name in ($downloaded.Keys | Sort-Object)) {
        $target = Join-Path $destDir $name
        Copy-Item -LiteralPath $downloaded[$name] -Destination $target -Force
        $size = (Get-Item -LiteralPath $target).Length
        Write-Host "  ok    $name ($size bytes, sha256 verified)"
    }

    Write-Host ""
    Write-Host "Both headers fetched and verified against pinned SHA-256."
}
finally {
    Remove-Item -LiteralPath $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
}
