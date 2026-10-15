# Regenerates search.png (rounded 3D magnifier) with System.Drawing.
# Usage: powershell -ExecutionPolicy Bypass -File tools/make_search_3d.ps1
# Then rerun tools/make_ico.ps1 to rebuild src/app.ico from it.
param(
  [string]$Dst = "search.png",
  [int]$Size = 512
)
Add-Type -AssemblyName System.Drawing

function NPt([double]$x, [double]$y) {
  return (New-Object System.Drawing.PointF -ArgumentList @([single]$x, [single]$y))
}

[double]$cx = [double]$Size / 2.0
[double]$cy = [double]$Size * 0.47
[double]$R = [double]$Size * 0.39
[double]$ringW = [double]$Size * 0.11

$bmp = New-Object System.Drawing.Bitmap -ArgumentList @($Size, $Size)
$g = [System.Drawing.Graphics]::FromImage($bmp)
try {
  $g.Clear([System.Drawing.Color]::Transparent)
  $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
  $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
  $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
  $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality

  $cyan = [System.Drawing.Color]::FromArgb(255, 35, 210, 228)
  $blue = [System.Drawing.Color]::FromArgb(255, 74, 141, 255)
  $rect = New-Object System.Drawing.RectangleF -ArgumentList @(
    [single]($cx - $R), [single]($cy - $R), [single]($R * 2), [single]($R * 2))
  $grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush -ArgumentList @(
    (NPt ($cx - $R) ($cy - $R)), (NPt ($cx + $R) ($cy + $R)), $cyan, $blue)

  # Ring geometry: gap on the right, handle junction lower-left.
  $start = 50.0
  $sweep = 295.0

  # Soft drop shadow (offset, translucent, slightly wider).
  $shRect = New-Object System.Drawing.RectangleF -ArgumentList @(
    [single]($cx - $R), [single]($cy - $R + $Size * 0.03),
    [single]($R * 2), [single]($R * 2))
  $shPen = New-Object System.Drawing.Pen -ArgumentList @(
    [System.Drawing.Color]::FromArgb(80, 0, 0, 0), [single]($ringW + $Size * 0.012))
  $shPen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
  $shPen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
  $g.DrawArc($shPen, $shRect, $start, $sweep)
  $shPen.Dispose()

  # Main ring with diagonal cyan->blue gradient, round caps.
  $ring = New-Object System.Drawing.Pen -ArgumentList @($grad, [single]$ringW)
  $ring.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
  $ring.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
  $g.DrawArc($ring, $rect, $start, $sweep)

  # Top gloss: thin bright arc over the upper-left of the ring.
  $gloss = New-Object System.Drawing.Pen -ArgumentList @(
    [System.Drawing.Color]::FromArgb(120, 255, 255, 255), [single]($ringW * 0.28))
  $gloss.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
  $gloss.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
  $g.DrawArc($gloss, $rect, 165.0, 70.0)
  $gloss.Dispose()
  $ring.Dispose()

  # Handle: capsule from the ring edge outward to bottom-left.
  $a = 138.0 * [Math]::PI / 180.0  # attach angle (lower-left)
  [double]$dx = [Math]::Cos($a)
  [double]$dy = [Math]::Sin($a)
  $p1 = NPt ($cx + $dx * ($R - $ringW * 0.4)) ($cy + $dy * ($R - $ringW * 0.4))
  $p2 = NPt ($cx + $dx * ($R + $Size * 0.24)) ($cy + $dy * ($R + $Size * 0.24))
  $hgrad = New-Object System.Drawing.Drawing2D.LinearGradientBrush -ArgumentList @($p1, $p2, $cyan, $blue)
  $hsh = New-Object System.Drawing.Pen -ArgumentList @(
    [System.Drawing.Color]::FromArgb(80, 0, 0, 0), [single]($Size * 0.125))
  $hsh.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
  $hsh.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
  $g.DrawLine($hsh, (NPt $p1.X ($p1.Y + $Size * 0.03)), (NPt $p2.X ($p2.Y + $Size * 0.03)))
  $hsh.Dispose()
  $hp = New-Object System.Drawing.Pen -ArgumentList @($hgrad, [single]($Size * 0.115))
  $hp.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
  $hp.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
  $g.DrawLine($hp, $p1, $p2)
  # Handle crest highlight (thin, offset toward upper edge).
  [double]$nx = -$dy
  [double]$ny = $dx
  [double]$off = $Size * 0.028
  $q1 = NPt ($p1.X + $nx * $off) ($p1.Y + $ny * $off)
  $q2 = NPt ($p2.X + $nx * $off) ($p2.Y + $ny * $off)
  $hi = New-Object System.Drawing.Pen -ArgumentList @(
    [System.Drawing.Color]::FromArgb(130, 255, 255, 255), [single]($Size * 0.02))
  $hi.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
  $hi.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
  $g.DrawLine($hi, $q1, $q2)
  $hi.Dispose(); $hp.Dispose(); $hgrad.Dispose(); $grad.Dispose()

  # Inner accent dash (upper-left inside the ring), light ice-blue.
  [double]$inset = $R * 0.42
  $dashRect = New-Object System.Drawing.RectangleF -ArgumentList @(
    [single]($cx - $inset), [single]($cy - $inset),
    [single]($inset * 2), [single]($inset * 2))
  $dash = New-Object System.Drawing.Pen -ArgumentList @(
    [System.Drawing.Color]::FromArgb(255, 170, 225, 255), [single]($Size * 0.055))
  $dash.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
  $dash.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
  $g.DrawArc($dash, $dashRect, 205.0, 50.0)
  $dash.Dispose()

  $bmp.Save($Dst, [System.Drawing.Imaging.ImageFormat]::Png)
  Write-Output "Wrote $Dst"
} finally { $g.Dispose(); $bmp.Dispose() }
