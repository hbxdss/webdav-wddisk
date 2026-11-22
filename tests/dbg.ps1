$ErrorActionPreference='Continue'
Set-Location (Join-Path $PSScriptRoot '..')
$p = Start-Process -FilePath '.\build\win\wddisk.exe' -ArgumentList '--mount','0' -PassThru
Start-Sleep 6
$alive = -not $p.HasExited
Write-Host ('alive=' + $alive + ' exit=' + $p.ExitCode)
Get-Content $env:TEMP\wddisk.log -Tail 6
if ($alive) { Stop-Process -Id $p.Id -Force }
