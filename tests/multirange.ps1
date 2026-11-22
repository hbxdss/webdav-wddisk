$ErrorActionPreference='Continue'
$exe = Join-Path $PSScriptRoot '..\build\win\wddisk.exe'
$p = Start-Process -FilePath $exe -ArgumentList '--mount','0' -PassThru
Start-Sleep 8
$zip = (Get-ChildItem 'Z:\' | Where-Object { $_.Extension -eq '.zip' } | Select-Object -First 1).FullName
$offsets = @(0, 500000000, 2000000000, 4413391159)
$fs = [IO.File]::OpenRead($zip)
$idx = 0
foreach ($off in $offsets) {
  $fs.Seek($off, 'Begin') | Out-Null
  $buf = New-Object byte[] 65536
  $n = $fs.Read($buf, 0, 65536)
  $m = [BitConverter]::ToString([Security.Cryptography.MD5]::Create().ComputeHash($buf)).Replace('-','')
  [IO.File]::WriteAllBytes((Join-Path $env:TEMP ("fsrange{0}.bin" -f $idx)), $buf)
  Write-Host ('OFF ' + $off + ' n=' + $n + ' MD5=' + $m)
  $idx++
}
$fs.Close()
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
$b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes('18915616199:kkwyn8wx'))
$idx = 0
foreach ($off in $offsets) {
  $end = $off + 65535
  curl.exe -s -L -H ("Authorization: Basic " + $b64) -H ("Range: bytes={0}-{1}" -f $off, $end) 'https://webdav.123pan.cn/webdav/%E7%AC%94%E8%AE%B0%E6%9C%AC%E8%AF%B4%E7%94%B5%E8%84%91%E7%9A%84%E8%99%9A%E6%8B%9F%E6%9C%BA.zip' -o (Join-Path $env:TEMP ("srvrange{0}.bin" -f $idx))
  $m = [BitConverter]::ToString([Security.Cryptography.MD5]::Create().ComputeHash([IO.File]::ReadAllBytes((Join-Path $env:TEMP ("srvrange{0}.bin" -f $idx))))).Replace('-','')
  Write-Host ('SRV OFF ' + $off + ' MD5=' + $m)
  $idx++
}
