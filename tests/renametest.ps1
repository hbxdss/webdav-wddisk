$ErrorActionPreference='Continue'
$exe = Join-Path $PSScriptRoot '..\build\win\wddisk.exe'
$p = Start-Process -FilePath $exe -ArgumentList '--mount','0' -PassThru
Start-Sleep 8
[IO.File]::WriteAllText('Z:\wddisk-test-dir\mv.txt', ('C' * 100000))
Start-Sleep 3
Rename-Item -LiteralPath 'Z:\wddisk-test-dir\mv.txt' -NewName 'mv2.txt'
Write-Host 'RENAME returned ok'
Start-Sleep 4
$vis = Get-ChildItem 'Z:\wddisk-test-dir' | ForEach-Object { $_.Name }
Write-Host ('FS listing: ' + ($vis -join ', '))
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Get-Content $env:TEMP\wddisk.log -Tail 12
