$ErrorActionPreference='Continue'
$exe = Join-Path $PSScriptRoot '..\build\win\wddisk.exe'
$p = Start-Process -FilePath $exe -ArgumentList '--mount','0' -PassThru
Start-Sleep 8
# 1. 新建目录 + 写文件
New-Item -ItemType Directory -Path 'Z:\wddisk-test-dir' -Force | Out-Null
Write-Host 'MKDIR ok'
$data = ('A' * 300000)
[IO.File]::WriteAllText('Z:\wddisk-test-dir\big.txt', $data)
Write-Host 'WRITE big.txt ok (300KB)'
# 2. 改写已有网盘文件（真实覆盖）：用已有 flac 副本？不动用户文件。改为对本目录文件两次写
[IO.File]::WriteAllText('Z:\wddisk-test-dir\big.txt', ('B' * 500000))
Write-Host 'OVERWRITE big.txt ok (500KB)'
Start-Sleep 6
# 3. 重命名
Rename-Item -LiteralPath 'Z:\wddisk-test-dir\big.txt' -NewName 'renamed.txt'
Write-Host 'RENAME ok'
Start-Sleep 5
# 4. 服务器端校验（PROPFIND 测试目录）
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
$b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes('18915616199:kkwyn8wx'))
curl.exe -s -X PROPFIND -H ("Authorization: Basic " + $b64) -H 'Depth: 1' 'https://webdav.123pan.cn/webdav/wddisk-test-dir/' -o (Join-Path $env:TEMP 'testdir.xml')
$xml = [IO.File]::ReadAllText((Join-Path $env:TEMP 'testdir.xml'))
Write-Host '--- server test-dir listing ---'
[regex]::Matches($xml, '<D:displayname>([^<]+)</D:displayname>|<D:getcontentlength>(\d+)</D:getcontentlength>') | ForEach-Object { Write-Host $_.Value }
