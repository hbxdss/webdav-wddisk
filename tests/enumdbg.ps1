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
public class WA {
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder sb, int max);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    public struct RECT { public int L, T, R, B; }
    public static List<string> Found = new List<string>();
    public static uint TargetPid;
    public static bool Cb(IntPtr h, IntPtr lp) {
        uint pid; GetWindowThreadProcessId(h, out pid);
        if (pid == TargetPid) {
            StringBuilder t = new StringBuilder(128), c = new StringBuilder(128);
            GetWindowTextW(h, t, 128); GetClassNameW(h, c, 128);
            Found.Add(h + " class=" + c + " title=" + t);
        }
        return true;
    }
}
"@
[WA]::TargetPid = $p.Id
$p2 = $p.MainWindowHandle
# 点击添加按钮
$cr = New-Object WA+RECT
[WA]::GetClientRect($p2, [ref]$cr) | Out-Null
$cx = $cr.R - 98; $cy = 40
$lp = [IntPtr](($cy -shl 16) -bor ($cx -band 0xFFFF))
[WA]::PostMessageW($p2, 0x201, [IntPtr]1, $lp) | Out-Null
Start-Sleep -Milliseconds 60
[WA]::PostMessageW($p2, 0x202, [IntPtr]0, $lp) | Out-Null
Start-Sleep 2
[WA]::EnumWindows([WA+EnumProc]{ param($h, $l) [WA]::Cb($h, $l) }, [IntPtr]::Zero) | Out-Null
[WA]::Found | ForEach-Object { Write-Host $_ }
