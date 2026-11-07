<#
.SYNOPSIS
  Builds an MSIX package of App Sandbox from a staged release folder.

.DESCRIPTION
  -Stage is the folder that also goes into the release zip (AppSandbox.exe,
  appsandbox_core.dll, web\, resources\, drivers\, ...). The script lays the
  package out, adds LICENSE and THIRD-PARTY-NOTICES.md when the stage lacks
  them, renders the tile images (scale-100/125/150/200/400) from
  src\app_win\appsandbox.ico, fills in AppxManifest.xml, indexes the images
  into resources.pri with MakePri, runs MakeAppx and signs the package.

  Versions: -Version always has four parts. A test build passes
  Major.Minor.Patch.Build and the package gets exactly that. With -Store the
  same input becomes Major.Minor.Build.0: the Store reserves the fourth part
  (it must be 0), and the build counter in the third part keeps every
  submission newer than the last. The Store also needs Major >= 1.

  Signing (the password never goes on signtool's command line; any
  certificate the script puts in Cert:\CurrentUser\My is removed again):
    -Thumbprint      a code-signing certificate already in Cert:\CurrentUser\My
                     (for example on a hardware token).
    -PfxPath         a .pfx file (with -PfxPassword); imported for the signing
                     and removed afterwards.
    neither          test builds: a self-signed test certificate (subject =
                     -Publisher); its .cer and Install-AppSandbox.ps1 are
                     written next to the package. -Store: left unsigned,
                     Partner Center signs Store packages itself.

  -Store also refuses the test identity defaults and a stage with test-signed
  drivers (drivers\*.cer): a Store package must come from the release stage
  with attestation-signed drivers, and use the identity, display name and
  publisher display name reserved in Partner Center.

.EXAMPLE
  .\packaging\msix\make-msix.ps1 -Stage bin\Release -Version 0.1.9.21 -OutDir out

.EXAMPLE
  .\packaging\msix\make-msix.ps1 -Store -Stage C:\stage -Version 1.0.0.215 -OutDir out `
      -Name 'Contoso.MyApp' -Publisher 'CN=1A2B3C4D-...' `
      -DisplayName 'My App' -PublisherDisplayName 'Contoso'
#>
param(
    [Parameter(Mandatory)] [string] $Stage,
    [Parameter(Mandatory)] [string] $Version,
    [Parameter(Mandatory)] [string] $OutDir,
    [string] $Name = 'AppSandbox.Test',
    [string] $Publisher = 'CN=App Sandbox Test',
    [string] $DisplayName = 'App Sandbox',
    [string] $PublisherDisplayName = 'App Sandbox',
    [ValidateSet('x64', 'arm64')] [string] $Arch = 'x64',
    [switch] $Store,
    [string] $Thumbprint,
    [string] $PfxPath,
    [string] $PfxPassword,
    [string] $TimestampUrl
)
$ErrorActionPreference = 'Stop'
$Arch = $Arch.ToLowerInvariant()       # the manifest schema only takes x64 / arm64
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Resolve-Path (Join-Path $here '..\..')

# ---- Version ----
if ($Version -notmatch '^\d+\.\d+\.\d+\.\d+$') { throw "Version must have four parts (a.b.c.d)" }
$parts = @($Version.Split('.') | ForEach-Object { [long]$_ })
if (@($parts | Where-Object { $_ -gt 65535 }).Count) { throw "Each part of the version must be 0..65535 ($Version)" }
if ($Store) {
    if ($parts[0] -lt 1) {
        throw "Store packages need a major version of at least 1 (AsbVersionMajor in Directory.Build.props); got $Version"
    }
    $Version = '{0}.{1}.{2}.0' -f $parts[0], $parts[1], $parts[3]
    Write-Host "Store package version: $Version"

    if ($Name -eq 'AppSandbox.Test' -or $Publisher -eq 'CN=App Sandbox Test') {
        throw "-Store needs -Name and -Publisher from Partner Center (Product identity), not the test defaults"
    }
    foreach ($p in 'DisplayName', 'PublisherDisplayName') {
        if (-not $PSBoundParameters.ContainsKey($p)) {
            throw "-Store needs -$p exactly as reserved in Partner Center"
        }
    }
    $testCer = @(Get-ChildItem -Path (Join-Path $Stage 'drivers') -Filter '*.cer' -File -ErrorAction SilentlyContinue)
    if ($testCer.Count) {
        throw "The stage has test-signed drivers ($($testCer.Name -join ', ')). Build the Store package from the release stage with attestation-signed drivers."
    }
}

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
$makepri  = Find-SdkTool 'makepri.exe'
$signtool = Find-SdkTool 'signtool.exe'

# Machine type of a PE file: 0x8664 = x64, 0xAA64 = ARM64.
function Get-PeMachine([string] $path) {
    $fs = [IO.File]::OpenRead($path)
    try {
        $br = New-Object IO.BinaryReader($fs)
        $fs.Position = 0x3C
        $fs.Position = $br.ReadInt32() + 4
        $br.ReadUInt16()
    } finally { $fs.Dispose() }
}

$layout = Join-Path ([IO.Path]::GetTempPath()) ("asb-msix-" + [Guid]::NewGuid().ToString('N'))
$priWork = Join-Path ([IO.Path]::GetTempPath()) ("asb-pri-" + [Guid]::NewGuid().ToString('N'))
$addedCerts = @()          # certificates this script added to Cert:\CurrentUser\My
try {
    # ---- Layout ----
    New-Item -ItemType Directory -Force -Path $layout | Out-Null
    robocopy $Stage $layout /E /XF *.pdb *.lib *.exp *.ilk *.iobj *.ipdb *.log /XD headless-api | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
    $global:LASTEXITCODE = 0
    $exePath = Join-Path $layout 'AppSandbox.exe'
    if (-not (Test-Path $exePath)) { throw "AppSandbox.exe not found in $Stage" }
    $machine = Get-PeMachine $exePath
    $expected = if ($Arch -eq 'arm64') { 0xAA64 } else { 0x8664 }
    if ($machine -ne $expected) {
        throw ("AppSandbox.exe in {0} is machine 0x{1:X4}, which does not match -Arch {2}" -f $Stage, $machine, $Arch)
    }

    # The licence texts travel with every copy (MIT and the bundled components).
    foreach ($doc in 'LICENSE', 'THIRD-PARTY-NOTICES.md') {
        if (-not (Test-Path (Join-Path $layout $doc))) {
            Copy-Item (Join-Path $repo $doc) (Join-Path $layout $doc) -ErrorAction Stop
        }
    }

    # ---- Tile images from the app icon ----
    # Only scale- and targetsize-qualified files, as Visual Studio produces:
    # the manifest names the plain file (Assets\StoreLogo.png) and
    # resources.pri maps it to the best variant for the display.
    Add-Type -AssemblyName System.Drawing
    $assets = Join-Path $layout 'Assets'
    New-Item -ItemType Directory -Force -Path $assets | Out-Null
    $icoPath = Join-Path $repo 'src\app_win\appsandbox.ico'
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
    $tiles = @(
        @{ Name = 'Square44x44Logo';   W = 44;  H = 44;  Fill = 1.0 },
        @{ Name = 'Square150x150Logo'; W = 150; H = 150; Fill = 0.7 },
        @{ Name = 'Wide310x150Logo';   W = 310; H = 150; Fill = 0.7 },
        @{ Name = 'StoreLogo';         W = 50;  H = 50;  Fill = 1.0 }
    )
    foreach ($t in $tiles) {
        foreach ($scale in 100, 125, 150, 200, 400) {
            $w = [int][Math]::Round($t.W * $scale / 100.0, [MidpointRounding]::AwayFromZero)
            $h = [int][Math]::Round($t.H * $scale / 100.0, [MidpointRounding]::AwayFromZero)
            Save-Tile ("{0}.scale-{1}.png" -f $t.Name, $scale) $w $h $t.Fill
        }
    }
    # Taskbar/Start icon sizes without the coloured plate.
    foreach ($s in 16, 24, 32, 48, 256) {
        Save-Tile ("Square44x44Logo.targetsize-{0}_altform-unplated.png" -f $s) $s $s 1.0
    }
    $src.Dispose(); $icon.Dispose()

    # ---- Manifest ----
    # Longer placeholders first, so none is replaced inside another.
    $values = [ordered]@{
        '__PUBLISHERDISPLAYNAME__' = [Security.SecurityElement]::Escape($PublisherDisplayName)
        '__DISPLAYNAME__'          = [Security.SecurityElement]::Escape($DisplayName)
        '__PUBLISHER__'            = [Security.SecurityElement]::Escape($Publisher)
        '__VERSION__'              = $Version
        '__NAME__'                 = [Security.SecurityElement]::Escape($Name)
        '__ARCH__'                 = $Arch
    }
    $manifest = Get-Content (Join-Path $here 'AppxManifest.xml') -Raw
    foreach ($k in $values.Keys) { $manifest = $manifest.Replace($k, $values[$k]) }
    $manifestPath = Join-Path $layout 'AppxManifest.xml'
    Set-Content -Path $manifestPath -Value $manifest -Encoding UTF8

    # ---- resources.pri ----
    # Index a copy of Assets\ only (the rest of the payload is not resources);
    # the resource names are the same relative paths as in the layout. The
    # <packaging> section that createconfig writes would split the scales
    # into extra resource packs (for bundles); one resources.pri is wanted.
    $priRoot = Join-Path $priWork 'root'
    New-Item -ItemType Directory -Force -Path $priRoot | Out-Null
    Copy-Item -Recurse $assets (Join-Path $priRoot 'Assets')
    $priConfig = Join-Path $priWork 'priconfig.xml'
    $priFile = Join-Path $priWork 'resources.pri'
    & $makepri createconfig /cf $priConfig /dq en-US /pv 10.0.0 /o | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "MakePri createconfig failed ($LASTEXITCODE)" }
    [xml] $cfg = Get-Content $priConfig -Raw
    foreach ($node in @($cfg.DocumentElement.ChildNodes | Where-Object { $_.LocalName -eq 'packaging' })) {
        [void] $cfg.DocumentElement.RemoveChild($node)
    }
    $cfg.Save($priConfig)
    & $makepri new /pr $priRoot /cf $priConfig /mn $manifestPath /of $priFile /o | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "MakePri new failed ($LASTEXITCODE)" }
    if (-not (Test-Path $priFile)) { throw "MakePri did not write $priFile" }
    Copy-Item $priFile (Join-Path $layout 'resources.pri')

    # ---- Pack ----
    $msix = Join-Path $OutDir ("AppSandbox-{0}-{1}.msix" -f $Version, $Arch)
    if (Test-Path $msix) { Remove-Item $msix }
    & $makeappx pack /d $layout /p $msix /o | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "MakeAppx failed ($LASTEXITCODE)" }

    # ---- Sign ----
    $cerOut = $null
    $signWith = $null      # thumbprint passed to signtool /sha1
    if ($Thumbprint) {
        $signWith = $Thumbprint
    } elseif ($PfxPath) {
        # Imported into the user store so signtool can use it by thumbprint
        # instead of taking the password on its command line.
        $before = @(Get-ChildItem Cert:\CurrentUser\My | ForEach-Object Thumbprint)
        $import = @{ FilePath = $PfxPath; CertStoreLocation = 'Cert:\CurrentUser\My' }
        if ($PfxPassword) { $import.Password = ConvertTo-SecureString $PfxPassword -AsPlainText -Force }
        $imported = @(Import-PfxCertificate @import)
        $addedCerts = @($imported | Where-Object { $before -notcontains $_.Thumbprint })
        $leaf = $imported | Where-Object HasPrivateKey | Select-Object -First 1
        if (-not $leaf) { throw "$PfxPath has no certificate with a private key" }
        $signWith = $leaf.Thumbprint
    } elseif (-not $Store) {
        # Test certificate. Its subject must equal the manifest Publisher.
        $testCert = New-SelfSignedCertificate -Type Custom -Subject $Publisher -KeyUsage DigitalSignature `
            -FriendlyName 'App Sandbox MSIX test signing' -CertStoreLocation 'Cert:\CurrentUser\My' `
            -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}') `
            -NotAfter (Get-Date).AddYears(2)
        $addedCerts = @($testCert)
        $cerOut = Join-Path $OutDir 'AppSandbox-MSIX-test.cer'
        Export-Certificate -Cert $testCert -FilePath $cerOut | Out-Null
        $signWith = $testCert.Thumbprint
    }
    if ($signWith) {
        $signArgs = @('sign', '/fd', 'SHA256', '/s', 'My', '/sha1', $signWith)
        if ($TimestampUrl) { $signArgs += @('/tr', $TimestampUrl, '/td', 'SHA256') }
        & $signtool @signArgs $msix | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "SignTool failed ($LASTEXITCODE)" }
    } else {
        Write-Host "Store package left unsigned: Partner Center signs it."
    }

    # ---- Installer helper for test builds ----
    if ($cerOut) {
        $install = @'
# Installs the App Sandbox MSIX test build. Run in an elevated PowerShell:
#   powershell -ExecutionPolicy Bypass -File .\Install-AppSandbox.ps1
$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$cer = Join-Path $dir 'AppSandbox-MSIX-test.cer'
$msix = Get-ChildItem $dir -Filter 'AppSandbox-*-__ARCH__.msix' | Select-Object -First 1
if (-not $msix) { throw "No AppSandbox-*.msix next to this script." }
# The test certificate must be trusted machine-wide for MSIX to install.
Import-Certificate -FilePath $cer -CertStoreLocation 'Cert:\LocalMachine\TrustedPeople' | Out-Null
Add-AppxPackage -Path $msix.FullName -ForceUpdateFromAnyVersion
Write-Host "App Sandbox installed. Start it from the Start menu."
'@
        Set-Content -Path (Join-Path $OutDir 'Install-AppSandbox.ps1') -Value $install.Replace('__ARCH__', $Arch) -Encoding UTF8
    }
    Write-Host "MSIX: $msix"
} finally {
    # Never leave a signing certificate (or its private key) behind.
    foreach ($c in $addedCerts) {
        $certPath = 'Cert:\CurrentUser\My\' + $c.Thumbprint
        if ($c.HasPrivateKey) {
            try { Remove-Item $certPath -DeleteKey -ErrorAction Stop; continue } catch { }
        }
        Remove-Item $certPath -ErrorAction SilentlyContinue
    }
    foreach ($dir in $layout, $priWork) {
        if (Test-Path $dir) { Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue }
    }
}
