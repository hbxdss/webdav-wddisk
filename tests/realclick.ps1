$ErrorActionPreference='Continue'
$exe = Join-Path $PSScriptRoot '..\build\win\wddisk.exe'
$cfgBak = Join-Path $env:TEMP 'config.json.bak'
Copy-Item (Join-Path $env:LOCALAPPDATA 'Wddisk\config.json') $cfgBak -Force
$p = Start-Process -FilePath $exe -PassThru
Start-Sleep 3
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class RT {
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int w, int hh, uint f);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool SetWindowTextW(IntPtr h, string s);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
    [DllImport("user32.dll")] public static extern bool SendMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
    public struct RECT { public int L, T, R, B; }
    public struct POINT { public int X, Y; }
    public static IntPtr Result; public static uint TargetPid; public static string TargetCls;
    public static IntPtr FindByClass(uint pid, string cls) {
        Result = IntPtr.Zero; TargetPid = pid; TargetCls = cls;
        EnumWindows(new EnumProc(Cb), IntPtr.Zero);
        return Result;
    }
    public static bool Cb(IntPtr h, IntPtr lp) {
        uint pid; GetWindowThreadProcessId(h, out pid);
        if (pid == TargetPid) {
            StringBuilder c = new StringBuilder(128);
            GetClassNameW(h, c, 128);
            if (c.ToString() == TargetCls) { Result = h; return false; }
        }
        return true;
    }
}
"@
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
$script:fail = 0
function Check($name, $cond) {
    if ($cond) { Write-Host ("PASS " + $name) }
    else { $script:fail++; Write-Host ("FAIL " + $name) }
}
function FindWin($cls) { [RT]::FindByClass($p.Id, $cls) }
function TopWin($hwnd) {
    [RT]::SetWindowPos($hwnd, [IntPtr](-1), 0,0,0,0, 0x53) | Out-Null
    [RT]::SetWindowPos($hwnd, [IntPtr](-2), 0,0,0,0, 0x53) | Out-Null
    Start-Sleep -Milliseconds 300
}
function Click($hwnd, $cx, $cy) {
    # 保持 TOPMOST 直到点击完成（防止被其他置顶窗口挡住）
    [RT]::SetWindowPos($hwnd, [IntPtr](-1), 0,0,0,0, 0x53) | Out-Null
    Start-Sleep -Milliseconds 200
    $o = New-Object RT+POINT
    $o.X = $cx; $o.Y = $cy
    [RT]::ClientToScreen($hwnd, [ref]$o) | Out-Null
    [RT]::SetCursorPos($o.X, $o.Y) | Out-Null
    Start-Sleep -Milliseconds 200
    [RT]::mouse_event(2,0,0,0,[UIntPtr]::Zero)
    Start-Sleep -Milliseconds 60
    [RT]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
    Start-Sleep -Milliseconds 300
    [RT]::SetWindowPos($hwnd, [IntPtr](-2), 0,0,0,0, 0x53) | Out-Null
    Start-Sleep -Milliseconds 200
}
$main = [IntPtr]::Zero
foreach ($i in 1..10) {
    $main = FindWin 'WddiskMain'
    if ($main -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 500
}
Check '1-main-window' ($main -ne [IntPtr]::Zero)
$cr = New-Object RT+RECT
[RT]::GetClientRect($main, [ref]$cr) | Out-Null
$cw = $cr.R

# 2. add dialog: real click add btn, fill fields, real click save
Click $main ($cw - 98) 40
$dlg = FindWin 'WddiskEditWnd'
Check '2a-add-dialog-open' ($dlg -ne [IntPtr]::Zero)
if ($dlg -ne [IntPtr]::Zero) {
    [RT]::SetWindowTextW([RT]::GetDlgItem($dlg, 101), 'zz-test-tmp') | Out-Null
    [RT]::SetWindowTextW([RT]::GetDlgItem($dlg, 102), 'https://webdav.123pan.cn/webdav') | Out-Null
    [RT]::SetWindowTextW([RT]::GetDlgItem($dlg, 103), '18915616199') | Out-Null
    [RT]::SetWindowTextW([RT]::GetDlgItem($dlg, 104), 'kkwyn8wx') | Out-Null
    Start-Sleep -Milliseconds 300
    Click $dlg 318 466   # save button center
    Start-Sleep 1
}
$h2 = FindWin 'WddiskEditWnd'
Check '2b-dialog-closed-after-save' ($h2 -eq [IntPtr]::Zero)

# 3. real click mount on card idx=1
Click $main ($cw - 28 - 16 - 32) 236
Start-Sleep 1
Check '3a-window-alive-during-mount' (-not $p.HasExited)
Start-Sleep 6
$drives = (Get-PSDrive -PSProvider FileSystem).Name -join ','
Write-Host ('   drives: ' + $drives)
Check '3b-mount-ok' ($drives -match 'Z:')

# 4. real click unmount
Click $main ($cw - 28 - 16 - 32) 236
Start-Sleep 5
$drives2 = (Get-PSDrive -PSProvider FileSystem).Name -join ','
Write-Host ('   drives after unmount: ' + $drives2)
Check '4-unmount-ok' (-not ($drives2 -match 'Z:'))

# 5. WM_CLOSE (X button) -> hide to tray, process alive
TopWin $main
[RT]::SendMessageW($main, 0x10, [IntPtr]0, [IntPtr]0) | Out-Null
Start-Sleep 1
Check '5a-close-hides-window' (-not [RT]::IsWindowVisible($main))
Check '5b-process-alive' (-not $p.HasExited)

# 6. tray click restore (WM_TRAY=0x8002, lp=WM_LBUTTONUP=0x202)
[RT]::PostMessageW($main, 0x8002, [IntPtr]0, [IntPtr]0x0202) | Out-Null
Start-Sleep 1
Check '6-tray-restore' ([RT]::IsWindowVisible($main))

# 7. WM_CLOSE while dialog open -> main stays visible, dialog keeps focus
Click $main ($cw - 98) 40
Start-Sleep 1
$dlg2 = FindWin 'WddiskEditWnd'
Check '7a-dialog-open' ($dlg2 -ne [IntPtr]::Zero)
[RT]::SendMessageW($main, 0x10, [IntPtr]0, [IntPtr]0) | Out-Null
Start-Sleep 1
Check '7b-main-still-visible' ([RT]::IsWindowVisible($main))
$dlg3 = FindWin 'WddiskEditWnd'
Check '7c-dialog-still-open' ($dlg3 -ne [IntPtr]::Zero)
# close dialog via its save with empty?? no - click cancel (W-58, H-42)
Click $dlg3 422 466
Start-Sleep 1
$dlg4 = FindWin 'WddiskEditWnd'
Check '7d-cancel-closes-dialog' ($dlg4 -eq [IntPtr]::Zero)

# 8. single instance
$p2 = Start-Process -FilePath $exe -PassThru
Start-Sleep 2
Check '8-single-instance' ($p2.HasExited)

# 9. tray exit (WM_COMMAND 4004) -> process ends
[RT]::PostMessageW($main, 0x111, [IntPtr]4004, [IntPtr]0) | Out-Null
Start-Sleep 3
Check '9-tray-exit' ($p.HasExited)

Write-Host ('---- FAIL count: ' + $script:fail + ' ----')
Copy-Item $cfgBak (Join-Path $env:LOCALAPPDATA 'Wddisk\config.json') -Force
Write-Host 'config restored'
