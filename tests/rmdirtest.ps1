$ErrorActionPreference='Continue'
$exe = Join-Path $PSScriptRoot '..\build\win\wddisk.exe'
$p = Start-Process -FilePath $exe -ArgumentList '--mount','0' -PassThru
Start-Sleep 8
Remove-Item -LiteralPath 'Z:\wddisk-test-dir' -Recurse -Force
Write-Host 'RM -r returned ok'
Start-Sleep 5
Write-Host ('FS still visible: ' + (Test-Path 'Z:\wddisk-test-dir'))
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Get-Content $env:TEMP\wddisk.log -Tail 15
