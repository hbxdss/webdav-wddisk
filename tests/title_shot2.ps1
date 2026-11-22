$exe = Join-Path $PSScriptRoot '..\build\win\wddisk.exe'
$p = Start-Process -FilePath $exe -PassThru
Start-Sleep 3
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WE {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int w, int hh, uint f);
    public struct RECT { public int L, T, R, B; }
}
"@
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
$h = $p.MainWindowHandle
$r = New-Object WE+RECT
[WE]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $hh = $r.B - $r.T
[WE]::SetWindowPos($h, [IntPtr](-1), 0,0,0,0, 0x53) | Out-Null
[WE]::SetWindowPos($h, [IntPtr](-2), 0,0,0,0, 0x53) | Out-Null
Start-Sleep -Milliseconds 500
$b = New-Object System.Drawing.Bitmap $w, $hh
$g = [System.Drawing.Graphics]::FromImage($b)
$g.CopyFromScreen($r.L, $r.T, 0, 0, $b.Size)
$b.Save((Join-Path $env:TEMP 'ui_title.png'))
$g.Dispose(); $b.Dispose()
Stop-Process -Id $p.Id -Force
Write-Host 'ok'
