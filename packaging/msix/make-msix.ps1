<#
.SYNOPSIS
  Builds an MSIX package of App Sandbox from a staged release folder.

.DESCRIPTION
  -Stage is the folder that also goes into the release zip (AppSandbox.exe,
  appsandbox_core.dll, web\, resources\, drivers\, ...). The script lays the
  package out, renders the tile images from src\app_win\appsandbox.ico,
  fills in AppxManifest.xml, runs MakeAppx and signs the package.

  Signing: pass -PfxPath/-PfxPassword for a real certificate. Without one, a
  self-signed test certificate (subject = -Publisher) is created and its
  .cer is written next to the package; Install-AppSandbox.ps1 (also written
  there) trusts it and installs the package.

.EXAMPLE
  .\packaging\msix\make-msix.ps1 -Stage bin\Release -Version 0.1.9.21 -OutDir out
#>
param(
    [Parameter(Mandatory)] [string] $Stage,
    [Parameter(Mandatory)] [string] $Version,
    [Parameter(Mandatory)] [string] $OutDir,
    [string] $Name = 'AppSandbox.Test',
    [string] $Publisher = 'CN=App Sandbox Test',
    [string] $PfxPath,
    [string] $PfxPassword
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Resolve-Path (Join-Path $here '..\..')

if ($Version -notmatch '^\d+\.\d+\.\d+\.\d+$') { throw "Version must have four parts (a.b.c.d)" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path

# ---- Tools from the Windows SDK ----
$kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
function Find-SdkTool([string] $exe) {
    $t = Get-ChildItem -Path $kits -Recurse -Filter $exe -ErrorAction SilentlyContinue |
         Where-Object { $_.FullName -match '\\x64\\' } |
         Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $t) { throw "$exe not found under $kits (install the Windows SDK)" }
    $t.FullName
}
$makeappx = Find-SdkTool 'makeappx.exe'
$signtool = Find-SdkTool 'signtool.exe'

# ---- Layout ----
$layout = Join-Path ([IO.Path]::GetTempPath()) ("asb-msix-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $layout | Out-Null
robocopy $Stage $layout /E /XF *.pdb *.lib *.exp *.ilk *.iobj *.ipdb *.log /XD headless-api | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
$global:LASTEXITCODE = 0
if (-not (Test-Path (Join-Path $layout 'AppSandbox.exe'))) { throw "AppSandbox.exe not found in $Stage" }

# ---- Tile images from the app icon ----
Add-Type -AssemblyName System.Drawing
$assets = Join-Path $layout 'Assets'
New-Item -ItemType Directory -Force -Path $assets | Out-Null
$icoPath = Join-Path $repo 'src\app_win\appsandbox.ico'
$icon = New-Object System.Drawing.Icon($icoPath, 256, 256)
$src = $icon.ToBitmap()
function Save-Tile([string] $file, [int] $w, [int] $h, [double] $scale) {
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.Clear([System.Drawing.Color]::Transparent)
    $side = [int]([Math]::Min($w, $h) * $scale)
    $g.DrawImage($src, [int](($w - $side) / 2), [int](($h - $side) / 2), $side, $side)
    $g.Dispose()
    $bmp.Save((Join-Path $assets $file), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}
Save-Tile 'Square44x44Logo.png'   44  44 1.0
Save-Tile 'Square150x150Logo.png' 150 150 0.7
Save-Tile 'Wide310x150Logo.png'   310 150 0.7
Save-Tile 'StoreLogo.png'         50  50 1.0
# Unplated taskbar/Start icon sizes
foreach ($s in 16, 24, 32, 48, 256) {
    Save-Tile ("Square44x44Logo.targetsize-{0}_altform-unplated.png" -f $s) $s $s 1.0
}
$src.Dispose(); $icon.Dispose()

# ---- Manifest ----
$manifest = Get-Content (Join-Path $here 'AppxManifest.xml') -Raw
$manifest = $manifest.Replace('__VERSION__', $Version).Replace('__PUBLISHER__', $Publisher).Replace('__NAME__', $Name)
Set-Content -Path (Join-Path $layout 'AppxManifest.xml') -Value $manifest -Encoding UTF8

# ---- Pack ----
$msix = Join-Path $OutDir ("AppSandbox-{0}-x64.msix" -f $Version)
if (Test-Path $msix) { Remove-Item $msix }
& $makeappx pack /d $layout /p $msix /o | Out-Host
if ($LASTEXITCODE -ne 0) { throw "MakeAppx failed ($LASTEXITCODE)" }

# ---- Sign ----
$cerOut = $null
if (-not $PfxPath) {
    # Test certificate. Its subject must equal the manifest Publisher.
    $cert = New-SelfSignedCertificate -Type Custom -Subject $Publisher -KeyUsage DigitalSignature `
        -FriendlyName 'App Sandbox MSIX test signing' -CertStoreLocation 'Cert:\CurrentUser\My' `
        -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}') `
        -NotAfter (Get-Date).AddYears(2)
    $PfxPassword = [Guid]::NewGuid().ToString('N')
    $PfxPath = Join-Path ([IO.Path]::GetTempPath()) 'asb-msix-test.pfx'
    Export-PfxCertificate -Cert $cert -FilePath $PfxPath `
        -Password (ConvertTo-SecureString $PfxPassword -AsPlainText -Force) | Out-Null
    $cerOut = Join-Path $OutDir 'AppSandbox-MSIX-test.cer'
    Export-Certificate -Cert $cert -FilePath $cerOut | Out-Null
    Remove-Item ("Cert:\CurrentUser\My\" + $cert.Thumbprint)
}
& $signtool sign /fd SHA256 /f $PfxPath /p $PfxPassword $msix | Out-Host
if ($LASTEXITCODE -ne 0) { throw "SignTool failed ($LASTEXITCODE)" }

# ---- Installer helper for test builds ----
if ($cerOut) {
    $install = @'
# Installs the App Sandbox MSIX test build. Run in an elevated PowerShell:
#   powershell -ExecutionPolicy Bypass -File .\Install-AppSandbox.ps1
$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$cer = Join-Path $dir 'AppSandbox-MSIX-test.cer'
$msix = Get-ChildItem $dir -Filter 'AppSandbox-*-x64.msix' | Select-Object -First 1
if (-not $msix) { throw "No AppSandbox-*.msix next to this script." }
# The test certificate must be trusted machine-wide for MSIX to install.
Import-Certificate -FilePath $cer -CertStoreLocation 'Cert:\LocalMachine\TrustedPeople' | Out-Null
Add-AppxPackage -Path $msix.FullName -ForceUpdateFromAnyVersion
Write-Host "App Sandbox installed. Start it from the Start menu."
'@
    Set-Content -Path (Join-Path $OutDir 'Install-AppSandbox.ps1') -Value $install -Encoding UTF8
}

Remove-Item -Recurse -Force $layout
Write-Host "MSIX: $msix"
