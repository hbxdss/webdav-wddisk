$b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes('18915616199:kkwyn8wx'))
$t0 = Get-Date
curl.exe -sS -X MKCOL -H ("Authorization: Basic " + $b64) 'https://webdav.123pan.cn/webdav/wddisk-delay-test/' -o NUL -w "MKCOL: %{http_code}`n"
for ($i = 1; $i -le 10; $i++) {
  $code = curl.exe -sS -o NUL -w "%{http_code}" -X PROPFIND -H ("Authorization: Basic " + $b64) -H 'Depth: 0' 'https://webdav.123pan.cn/webdav/wddisk-delay-test/'
  $el = [int]((Get-Date) - $t0).TotalMilliseconds
  Write-Host ("t={0}ms PROPFIND: {1}" -f $el, $code)
  if ($code -eq '207') { break }
  Start-Sleep -Milliseconds 700
}
curl.exe -sS -X DELETE -H ("Authorization: Basic " + $b64) 'https://webdav.123pan.cn/webdav/wddisk-delay-test/' -o NUL -w "DELETE: %{http_code}`n"
