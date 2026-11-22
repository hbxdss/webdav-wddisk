$b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes('18915616199:kkwyn8wx'))
curl.exe -sS -o NUL -w "dir /wddisk-rmtest: %{http_code} (期望 404)`n" -I -X PROPFIND -H ("Authorization: Basic " + $b64) -H 'Depth: 0' 'https://webdav.123pan.cn/webdav/wddisk-rmtest/'
curl.exe -sS -o NUL -w "file f1.txt: %{http_code} (期望 404)`n" -I -H ("Authorization: Basic " + $b64) 'https://webdav.123pan.cn/webdav/wddisk-rmtest/f1.txt'
curl.exe -sS -o NUL -w "dir /wddisk-test-dir: %{http_code} (期望 404)`n" -I -X PROPFIND -H ("Authorization: Basic " + $b64) -H 'Depth: 0' 'https://webdav.123pan.cn/webdav/wddisk-test-dir/'
curl.exe -sS -o NUL -w "file big.txt: %{http_code} (期望 404)`n" -I -H ("Authorization: Basic " + $b64) 'https://webdav.123pan.cn/webdav/wddisk-test-dir/big.txt'
