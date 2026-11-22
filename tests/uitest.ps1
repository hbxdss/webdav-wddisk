Get-Process wddisk -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 1
$exe = Join-Path $PSScriptRoot '..\build\win\wddisk.exe'
$p = Start-Process -FilePath $exe -PassThru
Start-Sleep 3
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win4 {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
    public struct RECT { public int L, T, R, B; }
}
"@
$h = $p.MainWindowHandle
[Win4]::SetForegroundWindow($h) | Out-Null
Start-Sleep 1
$r = New-Object Win4+RECT
[Win4]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $hh = $r.B - $r.T
Write-Host ("rect: " + $r.L + "," + $r.T + " " + $w + "x" + $hh)
Add-Type -AssemblyName System.Windows.Forms,System.Drawing

function Shot($name) {
  $b = New-Object System.Drawing.Bitmap $w, $hh
  $g = [System.Drawing.Graphics]::FromImage($b)
  $g.CopyFromScreen($r.L, $r.T, 0, 0, $b.Size)
  $b.Save((Join-Path $env:TEMP $name))
  $g.Dispose(); $b.Dispose()
  Write-Host ($name + ' ok')
}
function Click($x, $y) {
  [Win4]::SetCursorPos($x, $y) | Out-Null
  Start-Sleep -Milliseconds 150
  [Win4]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)  # LEFTDOWN
  [Win4]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)  # LEFTUP
}
# 1. 点击「＋ 添加服务器」(窗口内 682, 40)
Click ($r.L + $w - 28 - 140 + 70) ($r.T + 40)
Start-Sleep 2
Shot 'ui_add.png'
# 2. 关闭对话框（取消按钮 窗口内 W-104+46=722, H-58+16=518）
Click ($r.L + 722) ($r.T + 518)
Start-Sleep 1
# 3. 点击「挂载」(主按钮: 卡片右侧第3个按钮 x≈ W-16-64+32=732? 计算: bx从W-16=764递减; 挂载按钮 700..764 中心 732, y 卡片1中心 78+46=... 卡片 top=84, +46=130)
Click ($r.L + 732) ($r.T + 130)
Start-Sleep 4
Shot 'ui_mounted.png'
