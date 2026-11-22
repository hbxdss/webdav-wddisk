$ErrorActionPreference='Continue'
$exe = Join-Path $PSScriptRoot '..\build\win\wddisk.exe'
$p = Start-Process -FilePath $exe -ArgumentList '--mount','0' -PassThru
Start-Sleep 8
Write-Host '--- children ---'
Get-ChildItem 'Z:\wddisk-test-dir' | ForEach-Object { Write-Host $_.Name }
foreach ($f in (Get-ChildItem 'Z:\wddisk-test-dir')) {
  try { Remove-Item -LiteralPath $f.FullName -Force; Write-Host ('DEL ok: ' + $f.Name) }
  catch { Write-Host ('DEL fail: ' + $f.Name + ' - ' + $_.Exception.Message) }
}
Start-Sleep 3
try { Remove-Item -LiteralPath 'Z:\wddisk-test-dir' -Force; Write-Host 'RMDIR ok' }
catch { Write-Host ('RMDIR fail: ' + $_.Exception.Message) }
Start-Sleep 3
Write-Host ('dir still visible: ' + (Test-Path 'Z:\wddisk-test-dir'))
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Get-Content $env:TEMP\wddisk.log -Tail 20
