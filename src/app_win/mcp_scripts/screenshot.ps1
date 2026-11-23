# screenshot (fallback): copies the signed-in user's primary screen to a PNG.
# Runs in the desktop session. Parameters: out, maxWidth.
param([string]$ParamFile)
$ErrorActionPreference = 'Stop'
$p = Get-Content -Raw -Encoding UTF8 -LiteralPath $ParamFile | ConvertFrom-Json
Remove-Item -LiteralPath $ParamFile -Force -ErrorAction SilentlyContinue
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
Add-Type -Namespace Asb -Name Dpi -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();'
[Asb.Dpi]::SetProcessDPIAware() | Out-Null
$b = [Windows.Forms.Screen]::PrimaryScreen.Bounds
$bmp = New-Object Drawing.Bitmap $b.Width, $b.Height
$g = [Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.Location, [Drawing.Point]::Empty, $b.Size)
$g.Dispose()
$w = $b.Width; $h = $b.Height
if ($w -gt [int]$p.maxWidth) {
    $w = [int]$p.maxWidth; $h = [int]($b.Height * $w / $b.Width)
    $small = New-Object Drawing.Bitmap $w, $h
    $g = [Drawing.Graphics]::FromImage($small)
    $g.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.DrawImage($bmp, 0, 0, $w, $h)
    $g.Dispose(); $bmp.Dispose(); $bmp = $small
}
$bmp.Save($p.out, [Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
'{"width":' + $w + ',"height":' + $h + ',"screenWidth":' + $b.Width + ',"screenHeight":' + $b.Height + '}'
