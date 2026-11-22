# titlebar.ps1 - verify custom caption: minimize button + close button (hide to tray)
$ErrorActionPreference = 'Stop'
Add-Type @"
using System; using System.Runtime.InteropServices;
public class TB {
  [DllImport("user32")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32")] public static extern bool ShowWindow(IntPtr h, int c);
}
public struct RECT { public int L,T,R,B; }
"@
Stop-Process -Name wddisk -Force -ErrorAction SilentlyContinue
$p = Start-Process ".\build\win\wddisk.exe" -PassThru
Start-Sleep -Milliseconds 2000
$proc = Get-Process wddisk; $hwnd = $proc.MainWindowHandle
$rc = New-Object RECT; [TB]::GetClientRect($hwnd, [ref]$rc) | Out-Null
function click([int]$x, [int]$y) {
    $lp = ($y -shl 16) -bor $x
    [TB]::PostMessage($hwnd, 0x0201, [IntPtr]1, [IntPtr]$lp) | Out-Null
    Start-Sleep -Milliseconds 80
    [TB]::PostMessage($hwnd, 0x0202, [IntPtr]0, [IntPtr]$lp) | Out-Null
    Start-Sleep -Milliseconds 700
}
click ($rc.R - 69) 19
$mini = [TB]::IsIconic($hwnd)
[TB]::ShowWindow($hwnd, 9) | Out-Null   # SW_RESTORE
Start-Sleep -Milliseconds 700
click ($rc.R - 23) 19
$vis = [TB]::IsWindowVisible($hwnd)
"minimized=$mini"
"visibleAfterClose=$vis (false = 已最小化到托盘)"
Stop-Process -Name wddisk -Force -ErrorAction SilentlyContinue
