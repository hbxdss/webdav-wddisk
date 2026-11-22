$ErrorActionPreference='Continue'
$exe = Join-Path $PSScriptRoot '..\build\win\wddisk.exe'
$p = Start-Process -FilePath $exe -ArgumentList '--mount','0' -PassThru
Start-Sleep 8
cmd /c "rd /q Z:\wddisk-test-dir" 2>&1 | Write-Host
Write-Host ('rd exit: ' + $LASTEXITCODE)
Start-Sleep 3
Write-Host ('visible: ' + (Test-Path 'Z:\wddisk-test-dir'))
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
