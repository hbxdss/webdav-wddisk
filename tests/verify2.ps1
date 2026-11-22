# verify2.ps1 - dialog open/close + drive list + real mount IO
$ErrorActionPreference = 'Stop'
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W {
  [DllImport("user32")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder b, int n);
  public delegate bool EP(IntPtr h, IntPtr l);
  [DllImport("user32")] public static extern bool EnumWindows(EP cb, IntPtr l);
}
public struct RECT { public int L,T,R,B; }
"@
$pass=0; $fail=0
function killAll { Stop-Process -Name wddisk -Force -ErrorAction SilentlyContinue }

# find hwnd of class within pid
function findInPid([int]$targetPid, [string]$cls) {
    $script:found = [IntPtr]::Zero
    $script:want = $cls
    $cb = [W+EP]{
        param($h,$l)
        $p2 = 0; [void][W]::GetWindowThreadProcessId($h, [ref]$p2)
        if ($p2 -eq $script:wantpid) {
            $b = New-Object System.Text.StringBuilder 128
            [void][W]::GetClassName($h, $b, 128)
            if ($b.ToString() -eq $script:want) { $script:found = $h; return $false }
        }
        return $true
    }
    $script:wantpid = $targetPid
    [W]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    return $script:found
}

killAll
Remove-Item "$env:TEMP\wddisk.log" -Force -ErrorAction SilentlyContinue

$p = Start-Process ".\build\win\wddisk.exe" -PassThru
Start-Sleep -Milliseconds 1500
$proc = Get-Process wddisk -ErrorAction SilentlyContinue
if (-not $proc) { Write-Host "FAIL start"; exit 1 }
$hwnd = $proc.MainWindowHandle
$pid0 = $proc.Id
$rc = New-Object RECT; [W]::GetClientRect($hwnd, [ref]$rc) | Out-Null
$cx = $rc.R - 98; $cy = 80; $lp = ($cy -shl 16) -bor $cx

function clickAdd {
    [W]::PostMessage($hwnd, 0x0201, [IntPtr]1, [IntPtr]$lp) | Out-Null
    Start-Sleep -Milliseconds 60
    [W]::PostMessage($hwnd, 0x0202, [IntPtr]0, [IntPtr]$lp) | Out-Null
    Start-Sleep -Milliseconds 800
}

# --- T1 open dialog ---
Write-Host "T1 open dialog"
clickAdd
$dlg = findInPid $pid0 "WddiskEditWnd"
if ($dlg -ne [IntPtr]::Zero) { Write-Host "  PASS dialog opened hwnd=$dlg"; $pass++ }
else { Write-Host "  FAIL dialog not found"; $fail++ }

# --- T2 WM_CLOSE closes it ---
Write-Host "T2 WM_CLOSE"
if ($dlg -ne [IntPtr]::Zero) {
    [W]::PostMessage($dlg, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
    Start-Sleep -Milliseconds 900
    $dlg2 = findInPid $pid0 "WddiskEditWnd"
    if ($dlg2 -eq [IntPtr]::Zero) { Write-Host "  PASS dialog closed"; $pass++ }
    else { Write-Host "  FAIL dialog still open"; $fail++ }
}

# --- T3 reopen + ESC ---
Write-Host "T3 ESC"
clickAdd
$dlg = findInPid $pid0 "WddiskEditWnd"
if ($dlg -ne [IntPtr]::Zero) {
    [W]::PostMessage($dlg, 0x0100, [IntPtr]27, [IntPtr]0) | Out-Null
    Start-Sleep -Milliseconds 900
    $dlg2 = findInPid $pid0 "WddiskEditWnd"
    if ($dlg2 -eq [IntPtr]::Zero) { Write-Host "  PASS ESC closed"; $pass++ }
    else { Write-Host "  FAIL ESC not closed"; $fail++ }
} else { Write-Host "  FAIL reopen"; $fail++ }

# --- T4 process still alive ---
Write-Host "T4 process alive after dialog cycles"
if (Get-Process wddisk -ErrorAction SilentlyContinue) { Write-Host "  PASS alive"; $pass++ }
else { Write-Host "  FAIL dead"; $fail++ }

Write-Host ""
Write-Host "=== driveList log ==="
Get-Content "$env:TEMP\wddisk.log" | Select-String "driveList" | ForEach-Object { Write-Host "  " $_.Line }

killAll
Start-Sleep -Milliseconds 500

# --- T5 real mount Z: + IO ---
Write-Host "T5 real mount"
Remove-Item "$env:TEMP\wddisk.log" -Force -ErrorAction SilentlyContinue
if (Test-Path 'Z:\phasertest') { try { [System.IO.Directory]::Delete('Z:\phasertest', $true) } catch {} }
$mp = Start-Process ".\build\win\wddisk.exe" -ArgumentList "--mount","0" -PassThru
$ok = $false
$sw = [System.Diagnostics.Stopwatch]::StartNew()
while ($sw.ElapsedMilliseconds -lt 20000) { if (Test-Path 'Z:\') { $ok=$true; break }; Start-Sleep -Milliseconds 300 }
if ($ok) { Write-Host "  PASS mounted Z:"; $pass++ } else { Write-Host "  FAIL mount"; $fail++ }

if ($ok) {
    try {
        New-Item -ItemType Directory -Force -Path 'Z:\phasertest' | Out-Null
        'DRIVE FIX TEST' | Out-File 'Z:\phasertest\t.txt' -Encoding UTF8
        Start-Sleep -Milliseconds 2500
        $r = (Get-Content 'Z:\phasertest\t.txt' -Raw).Trim()
        if ($r -eq 'DRIVE FIX TEST') { Write-Host "  PASS read/write [$r]"; $pass++ }
        else { Write-Host "  FAIL content [$r]"; $fail++ }

        $bytes = New-Object byte[] 200000
        [System.Random]::new(7).NextBytes($bytes)
        [System.IO.File]::WriteAllBytes('Z:\phasertest\b.bin', $bytes)
        Start-Sleep -Milliseconds 4000
        $h1 = [System.BitConverter]::ToString((New-Object Security.Cryptography.MD5CryptoServiceProvider).ComputeHash($bytes)).Replace('-','')
        $h2 = (Get-FileHash 'Z:\phasertest\b.bin' -Algorithm MD5).Hash
        if ($h1 -eq $h2) { Write-Host "  PASS MD5 $h1"; $pass++ }
        else { Write-Host "  FAIL MD5 $h1 vs $h2"; $fail++ }

        # .NET 递归删除比 Remove-Item 在 WinFsp 上更可靠
        [System.IO.Directory]::Delete('Z:\phasertest', $true)
    } catch { Write-Host "  FAIL IO $_"; $fail++ }
}

killAll
Start-Sleep -Milliseconds 500
if (Test-Path 'Z:\') { Write-Host "  FAIL Z: still mounted"; $fail++ }
else { Write-Host "  PASS unmounted"; $pass++ }

Write-Host ""
Write-Host "RESULT PASS=$pass FAIL=$fail"
exit $fail
