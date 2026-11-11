param([string]$Out, [string]$Preview, [switch]$RegenOriginal)
Add-Type -AssemblyName System.Drawing
$src = "<workspace>\sonic3air_online_stable_source\Oxygen\sonic3air\data\sprites\sonic3air_logo.png"
$b = [Drawing.Bitmap]::new($src)
$SW = $b.Width; $SH = $b.Height
function IsBlue($c) { return ($c.A -eq 255 -and $c.B -gt 120 -and $c.R -lt 80) }
function IsFill($c) { return ($c.A -eq 255 -and -not (IsBlue $c)) }

# ---- fill pixels of the original glyphs, as [x,y] -> color, grouped by x ranges found with the analyzer ----
function GetGlyph($x0, $x1, $y0, $y1) {
  $g = @{}
  for ($y = $y0; $y -le $y1; $y++) { for ($x = $x0; $x -le $x1; $x++) { $c = $b.GetPixel($x, $y); if (IsFill $c) { $g["$($x - $x0),$($y - $y0)"] = $c } } }
  return @{ w = $x1 - $x0 + 1; h = $y1 - $y0 + 1; y0 = $y0; px = $g }
}
# glyph boxes from the original logo (fill component bounding boxes)
$S = GetGlyph 8 17 6 17
$O = GetGlyph 20 32 6 17
$N = GetGlyph 35 46 6 17
$I = GetGlyph 49 53 6 17
$C = GetGlyph 56 66 6 17
$G3 = GetGlyph 77 86 5 18
$A = GetGlyph 97 110 6 15       # rows 6..15 only, the dots of "A.I.R." are not part of the letter
$A2 = GetGlyph 97 110 6 17
$R = GetGlyph 128 139 6 15
$R2 = GetGlyph 128 139 6 17
# letters of A.I.R. above are cut at row 15 to avoid the dots; the real bodies reach row 17, so read them fully but skip dot pixels
function GetGlyphNoDots($x0, $x1, $y0, $y1, $dotX0, $dotX1) {
  $g = @{}
  for ($y = $y0; $y -le $y1; $y++) { for ($x = $x0; $x -le $x1; $x++) {
    if ($x -ge $dotX0 -and $x -le $dotX1 -and $y -ge 16) { continue }
    $c = $b.GetPixel($x, $y); if (IsFill $c) { $g["$($x - $x0),$($y - $y0)"] = $c } } }
  return @{ w = $x1 - $x0 + 1; h = $y1 - $y0 + 1; y0 = $y0; px = $g }
}

# ---- new letters E and T, built from the colours of the original "I" (5 wide) so they share its shading ----
# column class of the I: 0 = highlight edge ... 4 = shadow edge; rows 0..11 = the cap height of the logo
function IColor($col, $row) { return $b.GetPixel(49 + $col, 6 + $row) }
function MakeGlyph($rows, $width) {
  # $rows: array of 12 strings of '#' and '.', each $width long. Colour of a pixel from the I by its place in its horizontal run.
  $g = @{}
  for ($r = 0; $r -lt 12; $r++) {
    $line = $rows[$r]
    $x = 0
    while ($x -lt $width) {
      if ($line[$x] -eq '#') {
        $s = $x; while ($x -lt $width -and $line[$x] -eq '#') { $x++ }
        $e = $x - 1; $len = $e - $s + 1
        for ($k = 0; $k -lt $len; $k++) {
          $cls = if ($k -eq 0) { 0 } elseif ($k -eq $len - 1) { 4 } elseif ($k -eq 1) { 1 } elseif ($k -eq $len - 2) { 3 } else { 2 }
          $g["$($s + $k),$r"] = IColor $cls $r
        }
      } else { $x++ }
    }
  }
  return @{ w = $width; h = 12; y0 = 6; px = $g }
}
$Emask = @(
  '###########',
  '###########',
  '###########',
  '#####......',
  '#####......',
  '#########..',
  '#########..',
  '#####......',
  '#####......',
  '###########',
  '###########',
  '###########')
$Tmask = @(
  '#############',
  '#############',
  '#############',
  '....#####....',
  '....#####....',
  '....#####....',
  '....#####....',
  '....#####....',
  '....#####....',
  '....#####....',
  '....#####....',
  '....#####....')
$E = MakeGlyph $Emask 11
$T = MakeGlyph $Tmask 13
$Ai = GetGlyphNoDots 97 110 6 17 112 114
$Rr = GetGlyphNoDots 128 139 6 17 141 143
$Ii = GetGlyph 117 121 6 17

# ---- layout ----
$placements = @()
if ($RegenOriginal) {
  $placements = @(@($S, 8), @($O, 20), @($N, 35), @($I, 49), @($C, 56), @($G3, 77), @($Ai, 97), @($Ii, 117), @($Rr, 128))
  $W = 150; $H = 24
  # dots (original)
  foreach ($d in @(112, 123, 141)) { $dg = GetGlyph $d ($d + 2) 16 17; $placements += , @($dg, $d, 16) }
} else {
  $placements = @(@($S, 8), @($O, 20), @($N, 35), @($I, 49), @($C, 56), @($G3, 77))
  $x = 97
  foreach ($gl in @($Ai, $Ii, $Rr, $N, $E, $T)) { $placements += , @($gl, $x); $x += $gl.w + 2 }
  $W = $x + 4; $H = 24
}

# fill layer
$fillC = New-Object 'Drawing.Color[,]' $W, $H
$isFill = New-Object 'bool[,]' $W, $H
foreach ($pl in $placements) {
  $g = $pl[0]; $ox = $pl[1]
  $oy = if ($pl.Count -gt 2) { $pl[2] } else { $g.y0 }
  foreach ($k in $g.px.Keys) { $p = $k -split ','; $x = $ox + [int]$p[0]; $y = $oy + [int]$p[1]; $fillC[$x, $y] = $g.px[$k]; $isFill[$x, $y] = $true }
}
# outline: blue for squared distance <= 5, soft white ring beyond
$bmp = [Drawing.Bitmap]::new($W, $H, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
for ($y = 0; $y -lt $H; $y++) { for ($x = 0; $x -lt $W; $x++) {
  if ($isFill[$x, $y]) { $bmp.SetPixel($x, $y, $fillC[$x, $y]); continue }
  $best = 9999
  for ($dy = -4; $dy -le 4; $dy++) { for ($dx = -4; $dx -le 4; $dx++) {
    $nx = $x + $dx; $ny = $y + $dy
    if ($nx -ge 0 -and $ny -ge 0 -and $nx -lt $W -and $ny -lt $H -and $isFill[$nx, $ny]) { $d = $dx * $dx + $dy * $dy; if ($d -lt $best) { $best = $d } } } }
  if ($best -le 5) { $bmp.SetPixel($x, $y, [Drawing.Color]::FromArgb(255, 12, 47, 146)) }
  else {
    $al = if ($best -le 10) { 128 } elseif ($best -le 13) { 85 } else { 0 }
    if ($al -gt 0) { $bmp.SetPixel($x, $y, [Drawing.Color]::FromArgb($al, 255, 255, 255)) }
  } } }
if ($Out) { $bmp.Save($Out, [Drawing.Imaging.ImageFormat]::Png) }
if ($Preview) {
  $s = 7; $pv = [Drawing.Bitmap]::new($W * $s, $H * $s); $gr = [Drawing.Graphics]::FromImage($pv)
  $gr.Clear([Drawing.Color]::FromArgb(255, 40, 120, 232)); $gr.InterpolationMode = 'NearestNeighbor'; $gr.PixelOffsetMode = 'Half'
  $gr.DrawImage($bmp, 0, 0, $pv.Width, $pv.Height); $pv.Save($Preview, [Drawing.Imaging.ImageFormat]::Png)
}
if ($RegenOriginal) {
  $diff = 0; $diffA = 0
  for ($y = 0; $y -lt $H; $y++) { for ($x = 0; $x -lt $W; $x++) {
    $c1 = $b.GetPixel($x, $y); $c2 = $bmp.GetPixel($x, $y)
    if ($c1.A -eq 255 -or $c2.A -eq 255) { if ($c1.ToArgb() -ne $c2.ToArgb()) { $diff++ } }
    elseif (($c1.A -gt 0) -ne ($c2.A -gt 0)) { $diffA++ } } }
  "opaque pixel mismatches vs original: $diff ; ring presence mismatches: $diffA"
}
"size: $W x $H"



