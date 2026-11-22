$b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes('18915616199:kkwyn8wx'))
$u = 'https://webdav.123pan.cn/webdav/wddisk-move-test.txt'
curl.exe -sS -X PUT -H ("Authorization: Basic " + $b64) -d 'hello-move' $u -o NUL -w "PUT: %{http_code}`n"
curl.exe -sS -X MOVE -H ("Authorization: Basic " + $b64) -H 'Destination: https://webdav.123pan.cn/webdav/wddisk-move-test2.txt' -H 'Overwrite: T' $u -o NUL -w "MOVE: %{http_code}`n"
curl.exe -sS -o NUL -w "old: %{http_code}`n" -I -H ("Authorization: Basic " + $b64) $u
curl.exe -sS -o NUL -w "new: %{http_code}`n" -I -H ("Authorization: Basic " + $b64) 'https://webdav.123pan.cn/webdav/wddisk-move-test2.txt'
curl.exe -sS -X DELETE -H ("Authorization: Basic " + $b64) 'https://webdav.123pan.cn/webdav/wddisk-move-test2.txt' -o NUL -w "cleanup: %{http_code}`n"
