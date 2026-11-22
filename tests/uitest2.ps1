Get-Process wddisk -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 1
$exe = Join-Path $PSScriptRoot '..\build\win\wddisk.exe'
$p = Start-Process -FilePath $exe -PassThru
Start-Sleep 3
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win5 {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    public struct RECT { public int L, T, R, B; }
    public struct POINT { public int X, Y; }
}
"@
$h = $p.MainWindowHandle
[Win5]::ShowWindow($h, 9) | Out-Null
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win6 {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hh, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
"@
$fg = [Win6]::GetForegroundWindow()
$ft = [Win6]::GetWindowThreadProcessId($fg, [IntPtr]::Zero)
$ct = [Win6]::GetCurrentThreadId()
[Win6]::AttachThreadInput($ft, $ct, $true) | Out-Null
[Win6]::SetWindowPos($h, [IntPtr](-1), 0,0,0,0, 0x53) | Out-Null
[Win6]::SetWindowPos($h, [IntPtr](-2), 0,0,0,0, 0x53) | Out-Null
[Win6]::SetForegroundWindow($h) | Out-Null
[Win6]::AttachThreadInput($ft, $ct, $false) | Out-Null
Start-Sleep 1
$cr = New-Object Win5+RECT
[Win5]::GetClientRect($h, [ref]$cr) | Out-Null
$org = New-Object Win5+POINT
$org.X = 0; $org.Y = 0
[Win5]::ClientToScreen($h, [ref]$org) | Out-Null
$cw = $cr.R; $ch = $cr.B
Write-Host ("client: " + $cw + "x" + $ch + " at " + $org.X + "," + $org.Y)
$wr = New-Object Win5+RECT
[Win5]::GetWindowRect($h, [ref]$wr) | Out-Null
$ww = $wr.R - $wr.L; $wh = $wr.B - $wr.T
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
function Shot($name) {
  $b = New-Object System.Drawing.Bitmap $ww, $wh
  $g = [System.Drawing.Graphics]::FromImage($b)
  $g.CopyFromScreen($wr.L, $wr.T, 0, 0, $b.Size)
  $b.Save((Join-Path $env:TEMP $name))
  $g.Dispose(); $b.Dispose()
  Write-Host ($name + ' ok')
}
function Click($cx, $cy) {
  $sx = $org.X + $cx; $sy = $org.Y + $cy
  [Win5]::SetCursorPos($sx, $sy) | Out-Null
  Start-Sleep -Milliseconds 150
  [Win5]::mouse_event(2,0,0,0,[UIntPtr]::Zero)
  [Win5]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
}
# 1. 添加按钮（client: W-28-140 .. W-28, y 22..58 → 中心 (W-98, 40)）
Click ($cw - 98) 40
Start-Sleep 2
Shot 'ui_add.png'
# 2. 取消按钮（client: W-104..W-12, H-58..H-26 → 中心 (W-58, H-42)）
Click ($cw - 58) ($ch - 42)
Start-Sleep 1
# 3. 挂载按钮（client: 卡片 right=W-28, 按钮区 bx=W-28-16=.. 主按钮 (bx-64..bx) 中心 (W-28-16-32, 卡片y 84+46=130)）
Click ($cw - 28 - 16 - 32) 130
Start-Sleep 5
Shot 'ui_mounted.png'
