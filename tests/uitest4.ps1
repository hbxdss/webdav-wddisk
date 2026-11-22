Get-Process wddisk -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 1
$exe = Join-Path $PSScriptRoot '..\build\win\wddisk.exe'
$p = Start-Process -FilePath $exe -PassThru
Start-Sleep 3
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class WC {
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hh, uint flags);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    public struct RECT { public int L, T, R, B; }
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
$main = [WC]::FindByClass($p.Id, 'WddiskMain')
Write-Host ('main: ' + $main)
[WC]::SetWindowPos($main, [IntPtr](-1), 0,0,0,0, 0x53) | Out-Null
[WC]::SetWindowPos($main, [IntPtr](-2), 0,0,0,0, 0x53) | Out-Null
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
function SendClick($hwnd, $cx, $cy) {
    $lp = [IntPtr](($cy -shl 16) -bor ($cx -band 0xFFFF))
    [WC]::PostMessageW($hwnd, 0x201, [IntPtr]1, $lp) | Out-Null
    Start-Sleep -Milliseconds 60
    [WC]::PostMessageW($hwnd, 0x202, [IntPtr]0, $lp) | Out-Null
}
function Shot($hwnd, $name) {
    $r = New-Object WC+RECT
    [WC]::GetWindowRect($hwnd, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $h2 = $r.B - $r.T
    if ($w -le 0 -or $h2 -le 0) { Write-Host ($name + ' SKIP bad rect'); return }
    $b = New-Object System.Drawing.Bitmap $w, $h2
    $g = [System.Drawing.Graphics]::FromImage($b)
    $hdc = $g.GetHdc()
    [WC]::PrintWindow($hwnd, $hdc, 2) | Out-Null   # PW_RENDERFULLCONTENT
    $g.ReleaseHdc($hdc)
    $b.Save((Join-Path $env:TEMP $name))
    $g.Dispose(); $b.Dispose()
    Write-Host ($name + ' ok')
}
$cr = New-Object WC+RECT
[WC]::GetClientRect($main, [ref]$cr) | Out-Null
# 1. 打开添加对话框（重试直到出现）
$dlg = [IntPtr]::Zero
foreach ($i in 1..5) {
    SendClick $main ($cr.R - 98) 40
    Start-Sleep 2
    $dlg = [WC]::FindByClass($p.Id, 'WddiskEditWnd')
    if ($dlg -ne [IntPtr]::Zero) { break }
    Write-Host ('retry ' + $i)
}
Write-Host ('dialog: ' + $dlg)
Shot $dlg 'ui_add.png'
# 2. ESC 关闭
[WC]::PostMessageW($dlg, 0x100, [IntPtr]27, [IntPtr]0) | Out-Null   # WM_KEYDOWN ESC
Start-Sleep 1
# 3. 挂载
SendClick $main ($cr.R - 28 - 16 - 32) 130
Start-Sleep 6
Shot $main 'ui_mounted.png'
