$ErrorActionPreference='Continue'
$exe = Join-Path $PSScriptRoot '..\build\win\wddisk.exe'
$p = Start-Process -FilePath $exe -ArgumentList '--mount','0' -PassThru
Start-Sleep 8
$zip = (Get-ChildItem 'Z:\' | Where-Object { $_.Extension -eq '.zip' } | Select-Object -First 1).FullName
Write-Host ('TARGET: ' + $zip)
$sw = [Diagnostics.Stopwatch]::StartNew()
$fs = [IO.File]::OpenRead($zip)
$fs.Seek(100000000, 'Begin') | Out-Null
$buf = New-Object byte[] 65536
$n = $fs.Read($buf, 0, 65536)
$fs.Close()
$sw.Stop()
Write-Host ('FS READ bytes: ' + $n + '  elapsed_ms: ' + $sw.ElapsedMilliseconds)
$script:md5fs = [BitConverter]::ToString([Security.Cryptography.MD5]::Create().ComputeHash($buf)).Replace('-','')
Write-Host ('FS MD5: ' + $script:md5fs)
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
# 服务器端同区间（跟随 302）
$b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes('18915616199:kkwyn8wx'))
curl.exe -s -L -H ("Authorization: Basic " + $b64) -H 'Range: bytes=100000000-100065535' 'https://webdav.123pan.cn/webdav/%E7%AC%94%E8%AE%B0%E6%9C%AC%E8%AF%B4%E7%94%B5%E8%84%91%E7%9A%84%E8%99%9A%E6%8B%9F%E6%9C%BA.zip' -o "$env:TEMP\srvrange.bin"
Write-Host ('CURL size: ' + (Get-Item "$env:TEMP\srvrange.bin").Length)
$md5srv = [BitConverter]::ToString([Security.Cryptography.MD5]::Create().ComputeHash([IO.File]::ReadAllBytes("$env:TEMP\srvrange.bin"))).Replace('-','')
Write-Host ('SRV MD5: ' + $md5srv)
if ($script:md5fs -eq $md5srv) { Write-Host 'MATCH: YES' } else { Write-Host 'MATCH: NO' }
