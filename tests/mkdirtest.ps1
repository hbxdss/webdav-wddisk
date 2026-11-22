$ErrorActionPreference='Continue'
$exe = Join-Path $PSScriptRoot '..\build\win\wddisk.exe'
$p = Start-Process -FilePath $exe -ArgumentList '--mount','0' -PassThru
Start-Sleep 8
New-Item -ItemType Directory -Path 'Z:\wddisk-test-dir' -Force | Out-Null
Write-Host 'MKDIR returned ok'
Start-Sleep 2
$exists = Test-Path 'Z:\wddisk-test-dir'
Write-Host ('dir visible: ' + $exists)
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Get-Content $env:TEMP\wddisk.log -Tail 15
