$exe = Join-Path $PSScriptRoot '..\build\win\wddisk.exe'
$p = Start-Process -FilePath $exe -PassThru
Start-Sleep 4
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
$b = New-Object System.Drawing.Bitmap 900, 640
$g = [System.Drawing.Graphics]::FromImage($b)
$g.CopyFromScreen(300, 150, 0, 0, $b.Size)
$b.Save((Join-Path $env:TEMP 'ui1.png'))
$g.Dispose(); $b.Dispose()
Write-Host 'shot1 ok'
