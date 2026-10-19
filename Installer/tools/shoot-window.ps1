# 抓取安装器窗口截图（原生窗口，不是网页）
param(
  [string]$Exe = (Join-Path $PSScriptRoot '..\app\GenshinFSRBridgeInstaller.exe'),
  [string]$Out = (Join-Path $PSScriptRoot 'shots'),
  [string]$Name = 'shot.png',
  [int]$WaitMs = 2500,
  [switch]$NoLaunch,
  [switch]$Kill
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

# 本脚本自身必须是 DPI 感知进程：否则 GetWindowRect / PrintWindow 拿到的是被系统虚拟化过的
# 逻辑尺寸，在 200% 缩放下抓出来的「截图」其实只是真窗口左上角的 1/4，看起来像是界面被放大,
# 而且会让人误判清晰度。必须在创建任何窗口/DC 之前设置。
Add-Type -Namespace W -Name Dpi -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
'@
[void][W.Dpi]::SetProcessDpiAwarenessContext([IntPtr](-4))  # PER_MONITOR_AWARE_V2

Add-Type -Namespace W -Name N -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
'@

function Get-App($procName) {
  Get-Process -Name $procName -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
}

$procName = [System.IO.Path]::GetFileNameWithoutExtension($Exe)
$p = Get-App $procName

if (-not $p -and -not $NoLaunch) {
  Start-Process -FilePath $Exe -WorkingDirectory (Split-Path $Exe) | Out-Null
  $deadline = (Get-Date).AddSeconds(20)
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 300
    $p = Get-App $procName
    if ($p) { break }
  }
}
if (-not $p) { throw "没有找到窗口（$procName）" }

Start-Sleep -Milliseconds $WaitMs
$h = $p.MainWindowHandle
[void][W.N]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 400

$r = New-Object W.N+RECT
[void][W.N]::GetWindowRect($h, [ref]$r)
$w = $r.Right - $r.Left; $hh = $r.Bottom - $r.Top
"窗口句柄 $h  尺寸 ${w}x${hh}  位置 $($r.Left),$($r.Top)"

New-Item -ItemType Directory -Force -Path $Out | Out-Null
$bmp = New-Object System.Drawing.Bitmap($w, $hh)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[void][W.N]::PrintWindow($h, $hdc, 2)
$g.ReleaseHdc($hdc)
$g.Dispose()
$path = Join-Path $Out $Name
$bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
"saved: $path  ({0:N0} bytes)" -f (Get-Item $path).Length

if ($Kill) { Stop-Process -Id $p.Id -Force }
