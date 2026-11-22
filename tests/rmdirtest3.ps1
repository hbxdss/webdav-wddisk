$ErrorActionPreference='Continue'
$exe = Join-Path $PSScriptRoot '..\build\win\wddisk.exe'
$p = Start-Process -FilePath $exe -ArgumentList '--mount','0' -PassThru
Start-Sleep 8
# 建新目录+文件用于删除测试
New-Item -ItemType Directory -Path 'Z:\wddisk-rmtest' -Force | Out-Null
[IO.File]::WriteAllText('Z:\wddisk-rmtest\f1.txt', 'x')
Start-Sleep 4
try { Remove-Item -LiteralPath 'Z:\wddisk-rmtest' -Recurse -Force; Write-Host 'RM -r ok' }
catch { Write-Host ('RM -r fail: ' + $_.Exception.Message) }
Start-Sleep 4
Write-Host ('dir still visible: ' + (Test-Path 'Z:\wddisk-rmtest'))
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Get-Content $env:TEMP\wddisk.log -Tail 20
