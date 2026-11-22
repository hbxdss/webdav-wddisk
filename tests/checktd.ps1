$b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes('18915616199:kkwyn8wx'))
curl.exe -sS -X PROPFIND -H ("Authorization: Basic " + $b64) -H 'Depth: 1' 'https://webdav.123pan.cn/webdav/wddisk-test-dir/' -o (Join-Path $env:TEMP 'td2.xml') -w "PROPFIND: %{http_code}`n"
$xml = [IO.File]::ReadAllText((Join-Path $env:TEMP 'td2.xml'))
[regex]::Matches($xml, '<D:displayname>([^<]+)</D:displayname>|<D:getcontentlength>(\d+)</D:getcontentlength>') | ForEach-Object { Write-Host $_.Value }
