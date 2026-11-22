$m=[BitConverter]::ToString([Security.Cryptography.MD5]::Create().ComputeHash([IO.File]::ReadAllBytes((Join-Path $env:TEMP 'srvrange.bin')))).Replace('-','')
Write-Host ('SRV MD5: ' + $m + '  (FS MD5 was CB4A25CD70A99DDD0470D5B8CEBEBE04)')
