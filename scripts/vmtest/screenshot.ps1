param([string]$out, [string]$front = "")
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName Microsoft.VisualBasic
if ($front -ne "") { (New-Object -ComObject Shell.Application).MinimizeAll(); Start-Sleep -Milliseconds 700; [Microsoft.VisualBasic.Interaction]::AppActivate($front) | Out-Null; Start-Sleep -Milliseconds 700 }
$b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
