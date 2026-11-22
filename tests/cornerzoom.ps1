# cornerzoom.ps1 - crop all four window corners from screen.png and zoom 6x
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$src = [System.Drawing.Bitmap]::FromFile((Join-Path (Get-Location).Path "tests\shots\screen.png"))
$sw = 56; $zoom = 6
$out = New-Object System.Drawing.Bitmap ($sw * 2 * $zoom + 20), ($sw * $zoom)
$g = [System.Drawing.Graphics]::FromImage($out)
$g.Clear([System.Drawing.Color]::White)
$g.InterpolationMode = 'NearestNeighbor'
$g.PixelOffsetMode = 'Half'
$g.DrawImage($src, (New-Object System.Drawing.Rectangle 0, 0, ($sw*$zoom), ($sw*$zoom)), (New-Object System.Drawing.Rectangle 0, 0, $sw, $sw), 'Pixel')
$g.DrawImage($src, (New-Object System.Drawing.Rectangle ($sw*$zoom+20), 0, ($sw*$zoom), ($sw*$zoom)), (New-Object System.Drawing.Rectangle ($src.Width-$sw), 0, $sw, $sw), 'Pixel')
$g.Dispose()
$out.Save((Join-Path (Get-Location).Path "tests\shots\corners.png"), [System.Drawing.Imaging.ImageFormat]::Png)
$out.Dispose(); $src.Dispose()
"src=$($src.Width)x$($src.Height) saved corners.png"
