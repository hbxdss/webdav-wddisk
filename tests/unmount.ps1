Get-Process wddisk -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 1
# 清理可能残留的挂载盘符（进程被强杀时 WinFsp 卷可能残留）
$z = Get-PSDrive Z -ErrorAction SilentlyContinue
if ($z) {
  Write-Host 'Z: still present, removing...'
  Start-Process -FilePath 'C:\Program Files (x86)\WinFsp\bin\fsreg.bat' -ArgumentList 'unmount','Z:' -Wait -ErrorAction SilentlyContinue
  cmd /c 'mountvol Z: /D' 2>$null
}
$z2 = Get-PSDrive Z -ErrorAction SilentlyContinue
Write-Host ('Z after cleanup: ' + ($z2 -ne $null))
