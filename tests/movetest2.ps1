$b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes('18915616199:kkwyn8wx'))
curl.exe -sS -X DELETE -H ("Authorization: Basic " + $b64) 'https://webdav.123pan.cn/webdav/wddisk-move-test.txt' -o NUL -w "cleanup old: %{http_code}`n"
# 重新测 MOVE + 延迟观察
$u = 'https://webdav.123pan.cn/webdav/wddisk-move-test.txt'
curl.exe -sS -X PUT -H ("Authorization: Basic " + $b64) -d 'hello-move2' $u -o NUL -w "PUT: %{http_code}`n"
curl.exe -sS -X MOVE -H ("Authorization: Basic " + $b64) -H 'Destination: https://webdav.123pan.cn/webdav/wddisk-move-test2.txt' -H 'Overwrite: T' $u -o NUL -w "MOVE: %{http_code}`n"
foreach ($ms in @(0, 2000, 5000, 10000)) {
  Start-Sleep -Milliseconds $ms
  $c1 = curl.exe -sS -o NUL -w "%{http_code}" -I -H ("Authorization: Basic " + $b64) $u
  $c2 = curl.exe -sS -o NUL -w "%{http_code}" -I -H ("Authorization: Basic " + $b64) 'https://webdav.123pan.cn/webdav/wddisk-move-test2.txt'
  Write-Host ("t=+{0}ms  old={1}  new={2}" -f $ms, $c1, $c2)
}
curl.exe -sS -X DELETE -H ("Authorization: Basic " + $b64) 'https://webdav.123pan.cn/webdav/wddisk-move-test.txt' -o NUL -w "del old: %{http_code}`n"
curl.exe -sS -X DELETE -H ("Authorization: Basic " + $b64) 'https://webdav.123pan.cn/webdav/wddisk-move-test2.txt' -o NUL -w "del new: %{http_code}`n"
