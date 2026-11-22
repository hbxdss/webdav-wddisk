$b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes('18915616199:kkwyn8wx'))
curl.exe -sS -X MKCOL -H ("Authorization: Basic " + $b64) 'https://webdav.123pan.cn/webdav/wddisk-mkcol-test/' -o NUL -w "MKCOL: %{http_code}`n"
curl.exe -sS -X DELETE -H ("Authorization: Basic " + $b64) 'https://webdav.123pan.cn/webdav/wddisk-mkcol-test/' -o NUL -w "DELETE: %{http_code}`n"
