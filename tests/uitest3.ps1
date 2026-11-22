Get-Process wddisk -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 1
$exe = Join-Path $PSScriptRoot '..\build\win\wddisk.exe'
$p = Start-Process -FilePath $exe -PassThru
Start-Sleep 3
Add-Type @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class WB {
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder sb, int max);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    public struct RECT { public int L, T, R, B; }
    public static IntPtr FindByClass(uint pid, string cls) {
        Result = IntPtr.Zero; TargetPid = pid; TargetCls = cls;
        EnumWindows(new EnumProc(Cb), IntPtr.Zero);
        return Result;
    }
    public static IntPtr Result; public static uint TargetPid; public static string TargetCls;
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
Start-Sleep 1
$main = [WB]::FindByClass($p.Id, 'WddiskMain')
Write-Host ('main: ' + $main)
function SendClick($hwnd, $cx, $cy) {
    $lp = [IntPtr](($cy -shl 16) -bor ($cx -band 0xFFFF))
    $r1 = [WB]::SendMessageW($hwnd, 0x201, [IntPtr]1, $lp)
    Start-Sleep -Milliseconds 60
    $r2 = [WB]::SendMessageW($hwnd, 0x202, [IntPtr]0, $lp)
    Write-Host ('send result: ' + $r1 + ',' + $r2)
}
function Shot($hwnd, $name) {
    $r = New-Object WB+RECT
    [WB]::GetWindowRect($hwnd, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $h2 = $r.B - $r.T
    $b = New-Object System.Drawing.Bitmap $w, $h2
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.CopyFromScreen($r.L, $r.T, 0, 0, $b.Size)
    $b.Save((Join-Path $env:TEMP $name))
    $g.Dispose(); $b.Dispose()
    Write-Host ($name + ' ok ' + $w + 'x' + $h2)
}
$cr = New-Object WB+RECT
[WB]::GetClientRect($main, [ref]$cr) | Out-Null

# 1. 打开添加对话框
SendClick $main ($cr.R - 98) 40
Start-Sleep 2
$dlg = [WB]::FindByClass($p.Id, 'WddiskEditWnd')
Write-Host ('dialog: ' + $dlg)
[WB]::SetForegroundWindow($dlg) | Out-Null
Start-Sleep -Milliseconds 500
Shot $dlg 'ui_add.png'
# 2. 取消关闭
SendClick $dlg 422 466
Start-Sleep 1
# 3. 挂载（主按钮）
SendClick $main ($cr.R - 28 - 16 - 32) 130
Start-Sleep 6
Shot $main 'ui_mounted.png'
