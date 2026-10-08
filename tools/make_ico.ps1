# Regenerates src/app.ico (multi-size PNG-compressed ICO) from search.png.
# Usage: powershell -ExecutionPolicy Bypass -File tools/make_ico.ps1
param(
  [string]$Src = "search.png",
  [string]$Dst = "src/app.ico"
)
Add-Type -AssemblyName System.Drawing
$sizes = @(16, 24, 32, 48, 64, 128, 256)
$srcBmp = [System.Drawing.Bitmap]::FromFile((Resolve-Path $Src).Path)
try {
  $pngs = @()
  foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap($s, $s)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try {
      $g.Clear([System.Drawing.Color]::Transparent)
      $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
      $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
      $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
      $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
      $g.DrawImage($srcBmp, 0, 0, $s, $s)
    } finally { $g.Dispose() }
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    $pngs += @{ Size = $s; Data = $ms.ToArray() }
    $ms.Dispose()
  }
  $fs = [System.IO.File]::Create($Dst)
  try {
    $bw = New-Object System.IO.BinaryWriter($fs)
    $bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$pngs.Count)
    $offset = 6 + 16 * $pngs.Count
    foreach ($p in $pngs) {
      $dim = if ($p.Size -ge 256) { [byte]0 } else { [byte]$p.Size }
      $bw.Write($dim); $bw.Write($dim)
      $bw.Write([byte]0); $bw.Write([byte]0)
      $bw.Write([uint16]1); $bw.Write([uint16]32)
      $bw.Write([uint32]$p.Data.Length); $bw.Write([uint32]$offset)
      $offset += $p.Data.Length
    }
    foreach ($p in $pngs) { $bw.Write($p.Data) }
    $bw.Flush()
  } finally { $fs.Close() }
  Write-Output "Wrote $Dst ($((Get-Item $Dst).Length) bytes, $($pngs.Count) images)"
} finally { $srcBmp.Dispose() }
