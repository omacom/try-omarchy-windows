param([string]$out, [switch]$set, [switch]$get, [string]$text = "")
Add-Type -AssemblyName System.Windows.Forms; Add-Type -AssemblyName System.Drawing
if ($set) { $bmp = New-Object System.Drawing.Bitmap 123, 45; $g = [System.Drawing.Graphics]::FromImage($bmp); $g.Clear([System.Drawing.Color]::FromArgb(255, 200, 30, 90)); $g.Dispose(); [System.Windows.Forms.Clipboard]::SetImage($bmp); "set image 123x45" | Set-Content $out; exit }
if ($text -ne "") { [System.Windows.Forms.Clipboard]::SetText($text); "set text" | Set-Content $out; exit }
$has = [System.Windows.Forms.Clipboard]::ContainsImage(); $line = "containsImage=$has containsText=$([System.Windows.Forms.Clipboard]::ContainsText())"
if ($has) { $img = [System.Windows.Forms.Clipboard]::GetImage(); $line += " size=$($img.Width)x$($img.Height) pixel=$($img.GetPixel(0,0))" }
$line | Set-Content $out
