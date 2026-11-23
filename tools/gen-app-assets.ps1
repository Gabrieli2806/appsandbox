# Renders the package images of the app (app\AppSandbox\Assets) from
# src\app_win\appsandbox.ico. Run it again after the icon changes and commit
# the result:  powershell -ExecutionPolicy Bypass -File tools\gen-app-assets.ps1
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$repo = Split-Path -Parent $PSScriptRoot
$icoPath = Join-Path $repo 'src\app_win\appsandbox.ico'
$assets = Join-Path $repo 'app\AppSandbox\Assets'
New-Item -ItemType Directory -Force -Path $assets | Out-Null
Get-ChildItem $assets -Filter *.png | Remove-Item

$icon = New-Object System.Drawing.Icon($icoPath, 256, 256)
$src = $icon.ToBitmap()
function Save-Tile([string] $file, [int] $w, [int] $h, [double] $fill) {
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.Clear([System.Drawing.Color]::Transparent)
    $side = [int]([Math]::Min($w, $h) * $fill)
    $g.DrawImage($src, [int](($w - $side) / 2), [int](($h - $side) / 2), $side, $side)
    $g.Dispose()
    $bmp.Save((Join-Path $assets $file), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

# The manifest names the plain file (Assets\StoreLogo.png); resources.pri maps
# it to the best scale for the display.
$tiles = @(
    @{ Name = 'Square44x44Logo';   W = 44;  H = 44;  Fill = 1.0 },
    @{ Name = 'Square150x150Logo'; W = 150; H = 150; Fill = 0.7 },
    @{ Name = 'Wide310x150Logo';   W = 310; H = 150; Fill = 0.7 },
    @{ Name = 'StoreLogo';         W = 50;  H = 50;  Fill = 1.0 },
    @{ Name = 'SplashScreen';      W = 620; H = 300; Fill = 0.5 }
)
foreach ($t in $tiles) {
    foreach ($scale in 100, 200) {
        $w = [int][Math]::Round($t.W * $scale / 100.0, [MidpointRounding]::AwayFromZero)
        $h = [int][Math]::Round($t.H * $scale / 100.0, [MidpointRounding]::AwayFromZero)
        Save-Tile ("{0}.scale-{1}.png" -f $t.Name, $scale) $w $h $t.Fill
    }
}
# Taskbar and Start sizes without the coloured plate.
foreach ($s in 16, 24, 32, 48, 256) {
    Save-Tile ("Square44x44Logo.targetsize-{0}_altform-unplated.png" -f $s) $s $s 1.0
}
$src.Dispose(); $icon.Dispose()
Copy-Item $icoPath (Join-Path $assets 'AppSandbox.ico') -Force
Get-ChildItem $assets | Select-Object Name, Length
