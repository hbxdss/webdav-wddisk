# makeicon.ps1 - generate windows/app.ico with multiple sizes (16/32/48/256)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function New-LogoPng([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.Clear([System.Drawing.Color]::Transparent)

    $r = [Math]::Max(4, [int]($size * 0.21))
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc(0, 0, $r, $r, 180, 90)
    $path.AddArc($size - $r, 0, $r, $r, 270, 90)
    $path.AddArc($size - $r, $size - $r, $r, $r, 0, 90)
    $path.AddArc(0, $size - $r, $r, $r, 90, 90)
    $path.CloseFigure()

    $c1 = [System.Drawing.Color]::FromArgb(255, 132, 170, 255)
    $c2 = [System.Drawing.Color]::FromArgb(255, 48, 90, 216)
    $br = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.Point 0, 0),
        (New-Object System.Drawing.Point 0, $size), $c1, $c2)
    $g.FillPath($br, $path)
    $br.Dispose()

    $px = [int]($size * 0.66)
    $font = New-Object System.Drawing.Font("Segoe UI", $px, ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel))
    $sf = New-Object System.Drawing.StringFormat
    $sf.Alignment = [System.Drawing.StringAlignment]::Center
    $sf.LineAlignment = [System.Drawing.StringAlignment]::Center
    $offset = [single]($size * 0.01)
    $rf = New-Object System.Drawing.RectangleF 0, $offset, $size, $size
    $g.DrawString("W", $font, [System.Drawing.Brushes]::White, $rf, $sf)
    $g.Dispose()

    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bytes = $ms.ToArray()
    $ms.Dispose(); $bmp.Dispose(); $font.Dispose(); $path.Dispose(); $sf.Dispose()
    return ,$bytes
}

$sizes = @(16, 32, 48, 256)
$payloads = @()
foreach ($s in $sizes) { $payloads += ,(New-LogoPng $s) }

$icoPath = Join-Path (Get-Location).Path "windows\app.ico"
$fs = [System.IO.File]::Create($icoPath)
$bw = New-Object System.IO.BinaryWriter $fs
$bw.Write([UInt16]0)
$bw.Write([UInt16]1)
$bw.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]
    $w = if ($s -ge 256) { 0 } else { $s }
    $bw.Write([Byte]$w)
    $bw.Write([Byte]$w)
    $bw.Write([Byte]0)
    $bw.Write([Byte]0)
    $bw.Write([UInt16]1)
    $bw.Write([UInt16]32)
    $bw.Write([UInt32]$payloads[$i].Length)
    $bw.Write([UInt32]$offset)
    $offset += $payloads[$i].Length
}
foreach ($p in $payloads) { $bw.Write($p) }
$bw.Flush(); $bw.Close()
Write-Host "ico written: $icoPath ($((Get-Item $icoPath).Length) bytes, sizes: $($sizes -join ','))"
