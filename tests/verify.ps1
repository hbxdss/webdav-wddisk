$pair = '18915616199:kkwyn8wx'
$b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($pair))
curl.exe -s -o NUL -w "HEAD wddisk-test-hello.txt: %{http_code} (404=deleted ok)`n" -I -H ("Authorization: Basic " + $b64) 'https://webdav.123pan.cn/webdav/wddisk-test-hello.txt'
curl.exe -s -X PROPFIND -H ("Authorization: Basic " + $b64) -H 'Depth: 1' 'https://webdav.123pan.cn/webdav/' -o "$env:TEMP\props.xml"
Write-Host '--- root PROPFIND (first 1500 chars) ---'
$xml = [IO.File]::ReadAllText("$env:TEMP\props.xml")
Write-Host $xml.Substring(0, [Math]::Min(1500, $xml.Length))
Write-Host '--- wddisk.log tail ---'
Get-Content $env:TEMP\wddisk.log -Tail 20
