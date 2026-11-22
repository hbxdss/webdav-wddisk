Get-Process wddisk -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 1
$exe = Join-Path $PSScriptRoot '..\build\win\wddisk.exe'
$p = Start-Process -FilePath $exe -PassThru
Start-Sleep 3
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win3 {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hh, uint flags);
    public struct RECT { public int L, T, R, B; }
}
"@
$h = $p.MainWindowHandle
[Win3]::ShowWindow($h, 9) | Out-Null
# AttachThreadInput 强制抢前台
$fg = [Win3]::GetForegroundWindow()
$ft = [Win3]::GetWindowThreadProcessId($fg, [IntPtr]::Zero)
$ct = [Win3]::GetCurrentThreadId()
[Win3]::AttachThreadInput($ft, $ct, $true) | Out-Null
[Win3]::SetWindowPos($h, [IntPtr](-1), 0,0,0,0, 0x53) | Out-Null  # TOPMOST + NOSIZE|NOMOVE|SHOWWINDOW
[Win3]::SetWindowPos($h, [IntPtr](-2), 0,0,0,0, 0x53) | Out-Null  # NOTOPMOST
[Win3]::SetForegroundWindow($h) | Out-Null
[Win3]::AttachThreadInput($ft, $ct, $false) | Out-Null
Start-Sleep 1
$r = New-Object Win3+RECT
[Win3]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $hh = $r.B - $r.T
Write-Host ("rect: " + $r.L + "," + $r.T + " " + $w + "x" + $hh)
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
$b = New-Object System.Drawing.Bitmap $w, $hh
$g = [System.Drawing.Graphics]::FromImage($b)
$g.CopyFromScreen($r.L, $r.T, 0, 0, $b.Size)
$b.Save((Join-Path $env:TEMP 'ui4.png'))
$g.Dispose(); $b.Dispose()
Write-Host 'shot4 ok'
