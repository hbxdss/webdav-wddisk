$ErrorActionPreference='Continue'
$exe = Join-Path $PSScriptRoot '..\build\win\wddisk.exe'
$p = Start-Process -FilePath $exe -ArgumentList '--mount','0' -PassThru
Start-Sleep 8
$items = Get-ChildItem 'Z:\' -ErrorAction SilentlyContinue
Write-Host ('COUNT: ' + @($items).Count)
$items | ForEach-Object { Write-Host ('ITEM: ' + $_.Name + ' len=' + $_.Length) }
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Write-Host '--- log ---'
Get-Content $env:TEMP\wddisk.log -Tail 12
