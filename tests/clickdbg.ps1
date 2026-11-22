Get-Process wddisk -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 1
$exe = Join-Path $PSScriptRoot '..\build\win\wddisk.exe'
$p = Start-Process -FilePath $exe -PassThru
Start-Sleep 3
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W7 {
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
    public struct RECT { public int L, T, R, B; }
    public struct POINT { public int X, Y; }
}
"@
$h = $p.MainWindowHandle
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W8 {
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hh, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SetWindowPos2(IntPtr h, IntPtr after, int x, int y, int w, int hh, uint flags);
}
"@
$fgw = [W8]::GetForegroundWindow()
if ($fgw -ne $h -and $fgw -ne [IntPtr]::Zero) { [W8]::ShowWindow($fgw, 6) | Out-Null }  # 最小化挡路窗口
Start-Sleep -Milliseconds 300
[W8]::SetWindowPos($h, [IntPtr](-1), 0,0,0,0, 0x53) | Out-Null
[W8]::SetWindowPos($h, [IntPtr](-2), 0,0,0,0, 0x53) | Out-Null
[W8]::SetForegroundWindow($h) | Out-Null
Start-Sleep 1
Write-Host ('fg now: ' + [W8]::GetForegroundWindow() + ' main: ' + $h)
# 直接 PostMessage 模拟点击（绕开 Z 序）
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W9 {
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
}
"@
function SendClick($hwnd, $cx, $cy) {
    $lp = [IntPtr](($cy -shl 16) -bor ($cx -band 0xFFFF))
    [W9]::PostMessageW($hwnd, 0x201, [IntPtr]1, $lp) | Out-Null   # WM_LBUTTONDOWN
    Start-Sleep -Milliseconds 60
    [W9]::PostMessageW($hwnd, 0x202, [IntPtr]0, $lp) | Out-Null   # WM_LBUTTONUP
}
$cr = New-Object W7+RECT
[W7]::GetClientRect($h, [ref]$cr) | Out-Null
$org = New-Object W7+POINT
$org.X = 0; $org.Y = 0
[W7]::ClientToScreen($h, [ref]$org) | Out-Null
# 移动到添加按钮位置，但用 PostMessage 点击
$cx = $cr.R - 98; $cy = 40
SendClick $h $cx $cy
Start-Sleep 2
$dlg = [W7]::FindWindowW('WddiskEditWnd', $null)
Write-Host ('dialog hwnd: ' + $dlg)
# 关闭对话框：取消按钮 (W-58, H-42)
if ($dlg -ne [IntPtr]::Zero) {
    SendClick $dlg ($cr.R - 58) (508 - 42)
    Start-Sleep 1
}

