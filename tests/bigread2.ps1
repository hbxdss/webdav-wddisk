$ErrorActionPreference='Continue'
$exe = Join-Path $PSScriptRoot '..\build\win\wddisk.exe'
$p = Start-Process -FilePath $exe -ArgumentList '--mount','0' -PassThru
Start-Sleep 10
Get-PSDrive -PSProvider FileSystem | ForEach-Object { Write-Host ('DRIVE: ' + $_.Name) }
Write-Host '--- Z: listing ---'
Get-ChildItem 'Z:\' -ErrorAction SilentlyContinue | ForEach-Object { Write-Host ('ITEM: ' + $_.Name + ' len=' + $_.Length) }
$zips = Get-ChildItem 'Z:\*.zip' -ErrorAction SilentlyContinue
Write-Host ('zip count: ' + (@($zips).Count))
if ($zips) {
  $zip = @($zips)[0].FullName
  Write-Host ('TARGET: ' + $zip)
  $fs = [IO.File]::OpenRead($zip)
  $fs.Seek(100000000, 'Begin') | Out-Null
  $buf = New-Object byte[] 65536
  $n = $fs.Read($buf, 0, 65536)
  $fs.Close()
  Write-Host ('FS READ bytes: ' + $n)
  $script:md5fs = [BitConverter]::ToString([Security.Cryptography.MD5]::Create().ComputeHash($buf)).Replace('-','')
  Write-Host ('FS MD5: ' + $script:md5fs)
}
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Write-Host '--- curl raw response (718B from last run) ---'
[IO.File]::ReadAllText("$env:TEMP\srvrange.bin") | Write-Host
