$ErrorActionPreference='Continue'
$exe = Join-Path $PSScriptRoot '..\build\win\wddisk.exe'
$p = Start-Process -FilePath $exe -ArgumentList '--mount','0' -PassThru
Start-Sleep 8
$results = @()
# 1. 列目录
$n = @(Get-ChildItem 'Z:\' -ErrorAction SilentlyContinue).Count
$results += ('LIST: ' + $n + ' items (expect 11)')
# 2. 写入
$data = ('X' * 200000)
[IO.File]::WriteAllText('Z:\final-test.txt', $data)
Start-Sleep 6
# 3. 读回
$back = [IO.File]::ReadAllText('Z:\final-test.txt')
$results += ('READBACK: ' + ($back.Length -eq 200000) + ' len=' + $back.Length)
# 4. 大文件分块读
$zip = (Get-ChildItem 'Z:\' | Where-Object { $_.Extension -eq '.zip' } | Select-Object -First 1).FullName
$fs = [IO.File]::OpenRead($zip)
$fs.Seek(100000000, 'Begin') | Out-Null
$buf = New-Object byte[] 65536
[void]$fs.Read($buf, 0, 65536)
$fs.Close()
$m1 = [BitConverter]::ToString([Security.Cryptography.MD5]::Create().ComputeHash($buf)).Replace('-','')
$results += ('BIGREAD MD5: ' + $m1)
# 5. 删除
Remove-Item 'Z:\final-test.txt' -Force
Start-Sleep 3
$results += ('DELETE: gone=' + (-not (Test-Path 'Z:\final-test.txt')))
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
$results | ForEach-Object { Write-Host $_ }
# 6. 服务器端校验
$b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes('18915616199:kkwyn8wx'))
curl.exe -s -o NUL -w "SERVER upload: %{http_code} (200=ok)`n" -I -H ("Authorization: Basic " + $b64) 'https://webdav.123pan.cn/webdav/final-test.txt'
curl.exe -s -L -H ("Authorization: Basic " + $b64) -H 'Range: bytes=100000000-100065535' 'https://webdav.123pan.cn/webdav/%E7%AC%94%E8%AE%B0%E6%9C%AC%E8%AF%B4%E7%94%B5%E8%84%91%E7%9A%84%E8%99%9A%E6%8B%9F%E6%9C%BA.zip' -o (Join-Path $env:TEMP 'frv.bin')
$m2 = [BitConverter]::ToString([Security.Cryptography.MD5]::Create().ComputeHash([IO.File]::ReadAllBytes((Join-Path $env:TEMP 'frv.bin')))).Replace('-','')
Write-Host ('SERVER MD5: ' + $m2)
Write-Host ('MATCH: ' + ($m1 -eq $m2))
# 7. 清理服务器端
curl.exe -s -X DELETE -H ("Authorization: Basic " + $b64) 'https://webdav.123pan.cn/webdav/final-test.txt' -o NUL -w "cleanup: %{http_code}`n"
