# shot2.ps1 - capture sidebar UI pages
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class SH {
  [DllImport("user32")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder b, int n);
  public delegate bool EP(IntPtr h, IntPtr l);
  [DllImport("user32")] public static extern bool EnumWindows(EP cb, IntPtr l);
}
public struct RECT { public int L,T,R,B; }
"@

function killAll { Stop-Process -Name wddisk -Force -ErrorAction SilentlyContinue }
function findInPid([int]$targetPid, [string]$cls) {
    $script:found = [IntPtr]::Zero; $script:want = $cls; $script:wantpid = $targetPid
    $cb = [SH+EP]{
        param($h,$l)
        $q = 0; [void][SH]::GetWindowThreadProcessId($h, [ref]$q)
        if ($q -eq $script:wantpid) {
            $b = New-Object System.Text.StringBuilder 128
            [void][SH]::GetClassName($h, $b, 128)
            if ($b.ToString() -eq $script:want) { $script:found = $h; return $false }
        }
        return $true
    }
    [SH]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    return $script:found
}
function shot([IntPtr]$hwnd, [string]$path) {
    $r = New-Object RECT; [SH]::GetWindowRect($hwnd, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $h = $r.B - $r.T
    if ($w -le 0) { Write-Host "  bad size"; return }
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [SH]::PrintWindow($hwnd, $hdc, 2) | Out-Null
    $g.ReleaseHdc($hdc); $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    Write-Host "  saved $path ($w x $h)"
}
function click([IntPtr]$hwnd, [int]$x, [int]$y) {
    $lp = ($y -shl 16) -bor $x
    [SH]::PostMessage($hwnd, 0x0201, [IntPtr]1, [IntPtr]$lp) | Out-Null
    Start-Sleep -Milliseconds 60
    [SH]::PostMessage($hwnd, 0x0202, [IntPtr]0, [IntPtr]$lp) | Out-Null
    Start-Sleep -Milliseconds 600
}

killAll
$out = Join-Path (Get-Location).Path "tests\shots"
New-Item -ItemType Directory -Force $out | Out-Null
Remove-Item "$out\*.png" -Force -ErrorAction SilentlyContinue

$p = Start-Process ".\build\win\wddisk.exe" -PassThru
Start-Sleep -Milliseconds 2200
$proc = Get-Process wddisk; $hwnd = $proc.MainWindowHandle; $wpid = $proc.Id
Write-Host "main page"
shot $hwnd "$out\1-drives.png"

Write-Host "settings page"
click $hwnd 48 237
shot $hwnd "$out\2-settings.png"

Write-Host "about page"
click $hwnd 48 311
shot $hwnd "$out\3-about.png"

Write-Host "back to drives + add dialog"
click $hwnd 48 163
$rc = New-Object RECT; [SH]::GetClientRect($hwnd, [ref]$rc) | Out-Null
click $hwnd ($rc.R - 94) 80
$dlg = findInPid $wpid "WddiskEditWnd"
if ($dlg -ne [IntPtr]::Zero) {
    shot $dlg "$out\4-dialog.png"
    # drive dropdown: combo client (24,374)-(144,404)
    click $dlg 84 389
    $pop = findInPid $wpid "STATIC"
    if ($pop -ne [IntPtr]::Zero) { shot $pop "$out\5-dropdown.png" }
    [SH]::PostMessage($dlg, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
}
Start-Sleep -Milliseconds 400
killAll
Write-Host "DONE"
