Get-Process wddisk -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 1
$exe = Join-Path $PSScriptRoot '..\build\win\wddisk.exe'
Start-Process -FilePath $exe | Out-Null
Start-Sleep 3
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win2 {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, uint f, UIntPtr e);
    public struct RECT { public int L, T, R, B; }
}
"@
$h = [Win2]::FindWindowW('WddiskMain', $null)
Write-Host ('hwnd: ' + $h)
[Win2]::ShowWindow($h, 9) | Out-Null   # SW_RESTORE
[Win2]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero)  # ALT down 技巧
[Win2]::SetForegroundWindow($h) | Out-Null
[Win2]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)  # ALT up
Start-Sleep 1
$r = New-Object Win2+RECT
[Win2]::GetWindowRect($h, [ref]$r) | Out-Null
Write-Host ("rect: " + $r.L + "," + $r.T + " " + ($r.R - $r.L) + "x" + ($r.B - $r.T))
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
$w = $r.R - $r.L; $hh = $r.B - $r.T
$b = New-Object System.Drawing.Bitmap $w, $hh
$g = [System.Drawing.Graphics]::FromImage($b)
$g.CopyFromScreen($r.L, $r.T, 0, 0, $b.Size)
$b.Save((Join-Path $env:TEMP 'ui3.png'))
$g.Dispose(); $b.Dispose()
Write-Host 'shot3 ok'
