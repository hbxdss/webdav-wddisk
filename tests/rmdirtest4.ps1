$ErrorActionPreference='Continue'
$exe = Join-Path $PSScriptRoot '..\build\win\wddisk.exe'
$p = Start-Process -FilePath $exe -ArgumentList '--mount','0' -PassThru
Start-Sleep 8
$env:SEE_MASK_NOZONECHECKS='1'
cmd /c "rd /s /q Z:\wddisk-rmtest" 2>&1 | Write-Host
Write-Host ('rd exit: ' + $LASTEXITCODE)
Start-Sleep 4
Write-Host ('dir still visible: ' + (Test-Path 'Z:\wddisk-rmtest'))
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Get-Content $env:TEMP\wddisk.log -Tail 15
