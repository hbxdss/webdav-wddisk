Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WD {
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    public struct RECT { public int L, T, R, B; }
}
"@
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
$p = Get-Process wddisk | Select-Object -First 1
$h = $p.MainWindowHandle
$r = New-Object WD+RECT
[WD]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $hh = $r.B - $r.T
$b = New-Object System.Drawing.Bitmap $w, $hh
$g = [System.Drawing.Graphics]::FromImage($b)
$hdc = $g.GetHdc()
[WD]::PrintWindow($h, $hdc, 2) | Out-Null
$g.ReleaseHdc($hdc)
$b.Save((Join-Path $env:TEMP 'ui_title.png'))
$g.Dispose(); $b.Dispose()
Write-Host 'ok'
