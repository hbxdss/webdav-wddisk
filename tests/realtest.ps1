$ErrorActionPreference='Continue'
$exe = Join-Path $PSScriptRoot '..\build\win\wddisk.exe'
$p = Start-Process -FilePath $exe -ArgumentList '--mount','0' -PassThru
Start-Sleep -Seconds 8
Write-Host '=== 盘符 ==='
Get-PSDrive -PSProvider FileSystem | ForEach-Object { Write-Host ($_.Name + ': ' + $_.Root) }
# 写入测试
try {
  [IO.File]::WriteAllText('Z:\wddisk-test-hello.txt', 'hello from wddisk 123pan test')
  Write-Host 'WRITE: ok'
} catch { Write-Host ('WRITE ERR: ' + $_.Exception.Message) }
Start-Sleep -Seconds 6
try {
  $back = [IO.File]::ReadAllText('Z:\wddisk-test-hello.txt')
  Write-Host ('READBACK: ' + $back)
} catch { Write-Host ('READ ERR: ' + $_.Exception.Message) }
# 列目录
try { Get-ChildItem 'Z:\' | Select-Object -First 8 | ForEach-Object { Write-Host ('ITEM: ' + $_.Name + '  len=' + $_.Length) } } catch { Write-Host ('LIST ERR: ' + $_.Exception.Message) }
# 清理
try { Remove-Item 'Z:\wddisk-test-hello.txt' -Force; Write-Host 'DELETE: ok' } catch { Write-Host ('DELETE ERR: ' + $_.Exception.Message) }
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Write-Host 'DONE'
