# screenrgn.ps1 - real screen capture (not PrintWindow) + window region type
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class SR {
  [DllImport("user32")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32")] public static extern int GetWindowRgn(IntPtr h, IntPtr rgn);
  [DllImport("gdi32")] public static extern IntPtr CreateRectRgn(int l,int t,int r,int b);
  [DllImport("gdi32")] public static extern bool DeleteObject(IntPtr o);
  [DllImport("user32")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("dwmapi")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out int v, int sz);
}
public struct RECT { public int L,T,R,B; }
"@

Stop-Process -Name wddisk -Force -ErrorAction SilentlyContinue
$out = Join-Path (Get-Location).Path "tests\shots"
$p = Start-Process ".\build\win\wddisk.exe" -PassThru
Start-Sleep -Milliseconds 2400
$proc = Get-Process wddisk
$hwnd = $proc.MainWindowHandle
[SR]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 500

$r = New-Object RECT; [SR]::GetWindowRect($hwnd, [ref]$r) | Out-Null
$rgn = [SR]::CreateRectRgn(0,0,1,1)
$type = [SR]::GetWindowRgn($hwnd, $rgn)
[SR]::DeleteObject($rgn) | Out-Null
$dark = 0
$hr = [SR]::DwmGetWindowAttribute($hwnd, 20, [ref]$dark, 4)
"rgnType=$type (0=ERROR,1=NULL,2=SIMPLE,3=COMPLEX)"
"DwmDark hr=$hr value=$dark"
"rect=$($r.L),$($r.T) $($r.R)x$($r.B)"

$bmp = New-Object System.Drawing.Bitmap ($r.R-$r.L), ($r.B-$r.T)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
$g.Dispose()
$bmp.Save("$out\screen.png", [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
"saved $out\screen.png"
Stop-Process -Name wddisk -Force -ErrorAction SilentlyContinue
