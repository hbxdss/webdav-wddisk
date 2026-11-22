# shot.ps1 - capture windows via PrintWindow (no focus dependency)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class SW {
  [DllImport("user32")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder b, int n);
  [DllImport("user32")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  public delegate bool EP(IntPtr h, IntPtr l);
  [DllImport("user32")] public static extern bool EnumWindows(EP cb, IntPtr l);
}
public struct RECT { public int L,T,R,B; }
"@

function killAll { Stop-Process -Name wddisk -Force -ErrorAction SilentlyContinue }
function findInPid([int]$targetPid, [string]$cls) {
    $script:found = [IntPtr]::Zero; $script:want = $cls; $script:wantpid = $targetPid
    $cb = [SW+EP]{
        param($h,$l)
        $p2 = 0; [void][SW]::GetWindowThreadProcessId($h, [ref]$p2)
        if ($p2 -eq $script:wantpid) {
            $b = New-Object System.Text.StringBuilder 128
            [void][SW]::GetClassName($h, $b, 128)
            if ($b.ToString() -eq $script:want) { $script:found = $h; return $false }
        }
        return $true
    }
    [SW]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    return $script:found
}
function shot([IntPtr]$hwnd, [string]$path) {
    $r = New-Object RECT
    [SW]::GetWindowRect($hwnd, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $h = $r.B - $r.T
    if ($w -le 0 -or $h -le 0) { Write-Host "  bad size $w x $h"; return }
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    $ok = [SW]::PrintWindow($hwnd, $hdc, 2)   # PW_RENDERFULLCONTENT
    $g.ReleaseHdc($hdc)
    $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host "  saved $path ($w x $h) printWindow=$ok"
}

killAll
Remove-Item "$env:TEMP\wddisk.log" -Force -ErrorAction SilentlyContinue
$out = Join-Path (Get-Location).Path "tests\shots"
New-Item -ItemType Directory -Force $out | Out-Null
Remove-Item "$out\*.png" -Force -ErrorAction SilentlyContinue

$p = Start-Process ".\build\win\wddisk.exe" -PassThru
Start-Sleep -Milliseconds 2000
$proc = Get-Process wddisk; $hwnd = $proc.MainWindowHandle; $wpid = $proc.Id
Write-Host "main"
shot $hwnd "$out\1-main.png"

# open dialog
$rc = New-Object RECT; [SW]::GetClientRect($hwnd, [ref]$rc) | Out-Null
$cx = $rc.R - 98; $cy = 40; $lp = ($cy -shl 16) -bor $cx
[SW]::PostMessage($hwnd, 0x0201, [IntPtr]1, [IntPtr]$lp) | Out-Null
Start-Sleep -Milliseconds 60
[SW]::PostMessage($hwnd, 0x0202, [IntPtr]0, [IntPtr]$lp) | Out-Null
Start-Sleep -Milliseconds 1200
$dlg = findInPid $wpid "WddiskEditWnd"
Write-Host "dialog hwnd=$dlg"
if ($dlg -ne [IntPtr]::Zero) {
    shot $dlg "$out\2-dialog.png"
    # combo client rect = (24,374)-(144,404) -> center (84,389)
    $dlp = (389 -shl 16) -bor 84
    [SW]::PostMessage($dlg, 0x0201, [IntPtr]1, [IntPtr]$dlp) | Out-Null
    Start-Sleep -Milliseconds 60
    [SW]::PostMessage($dlg, 0x0202, [IntPtr]0, [IntPtr]$dlp) | Out-Null
    Start-Sleep -Milliseconds 900
    $script:found = [IntPtr]::Zero
    $cb = [SW+EP]{
        param($h,$l)
        $p2 = 0; [void][SW]::GetWindowThreadProcessId($h, [ref]$p2)
        if ($p2 -eq $script:wantpid) {
            $b = New-Object System.Text.StringBuilder 128
            [void][SW]::GetClassName($h, $b, 128)
            if ($b.ToString() -eq "STATIC") { $script:found = $h; return $false }
        }
        return $true
    }
    $script:wantpid = $wpid
    [SW]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    if ($script:found -ne [IntPtr]::Zero) {
        Write-Host "dropdown hwnd=$($script:found)"
        shot $script:found "$out\3-dropdown.png"
        shot $dlg "$out\4-dialog-open.png"
    } else { Write-Host "  dropdown not found" }
    [SW]::PostMessage($dlg, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
}
Start-Sleep -Milliseconds 500
killAll
Write-Host "DONE"
