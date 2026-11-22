Get-Process wddisk -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 1
$exe = Join-Path $PSScriptRoot '..\build\win\wddisk.exe'
$p = Start-Process -FilePath $exe -PassThru
Start-Sleep 3
$alive = -not $p.HasExited
Write-Host ('alive=' + $alive + ' exit=' + $p.ExitCode + ' mwh=' + $p.MainWindowHandle)
Get-Content $env:TEMP\wddisk.log -Tail 3
