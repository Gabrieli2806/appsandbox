# install_driver: checks the guest's signing state and the package's signatures,
# then installs it. Parameters (JSON file): dir, inf, sys, service, trustCert, checkOnly.
param([string]$ParamFile)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object Text.UTF8Encoding $false
$p = Get-Content -Raw -Encoding UTF8 -LiteralPath $ParamFile | ConvertFrom-Json
Remove-Item -LiteralPath $ParamFile -Force -ErrorAction SilentlyContinue
$warnings = New-Object Collections.ArrayList
$r = [ordered]@{ ok = $false }

# Boot state: test-signed kernel drivers need test signing on, which needs Secure Boot off.
$bcd = (& bcdedit.exe /enum '{current}' 2>$null | Out-String)
$r.testSigning = [bool]($bcd -match '(?im)^\s*testsigning\s+Yes')
$r.secureBoot = $false
try { $r.secureBoot = [bool](Confirm-SecureBootUEFI) } catch {}

# Signatures of the package's catalogs and binaries.
$files = @(Get-ChildItem -LiteralPath $p.dir -Recurse -File | Where-Object { '.cat', '.sys', '.dll', '.exe' -contains $_.Extension.ToLower() })
$certs = @{}
$sigs = New-Object Collections.ArrayList
foreach ($f in $files) {
    $s = Get-AuthenticodeSignature -LiteralPath $f.FullName
    $c = $s.SignerCertificate
    if ($c) { $certs[$c.Thumbprint] = $c }
    [void]$sigs.Add([ordered]@{ file = $f.Name; status = [string]$s.Status; signer = $(if ($c) { $c.Subject } else { $null }) })
}
$r.signatures = $sigs
$hasCat = @($files | Where-Object { $_.Extension -ieq '.cat' }).Count -gt 0
$unsigned = @($sigs | Where-Object { $_.status -eq 'NotSigned' -and $_.file -match '\.(sys|cat)$' })
if ($p.inf -and -not $hasCat) { [void]$warnings.Add('The package has no catalog (.cat). Plug and Play installs need one: create it with Inf2Cat and sign it (signtool) with a test certificate.') }
if ($unsigned.Count) { [void]$warnings.Add('Not signed: ' + (($unsigned | ForEach-Object { $_.file }) -join ', ') + '. A 64-bit kernel driver must be signed, at least with a test certificate.') }

# A test certificate is not trusted by the guest yet: trust it (this is a disposable VM).
$trusted = New-Object Collections.ArrayList
$untrusted = @($sigs | Where-Object { $_.status -ne 'Valid' -and $_.status -ne 'NotSigned' })
if ($untrusted.Count -and $p.trustCert) {
    foreach ($c in $certs.Values) {
        foreach ($storeName in 'Root', 'TrustedPublisher') {
            $store = New-Object Security.Cryptography.X509Certificates.X509Store($storeName, 'LocalMachine')
            $store.Open('ReadWrite'); $store.Add($c); $store.Close()
        }
        [void]$trusted.Add($c.Subject)
    }
    $again = @($files | ForEach-Object { Get-AuthenticodeSignature -LiteralPath $_.FullName } | Where-Object { $_.Status -ne 'Valid' -and $_.Status -ne 'NotSigned' })
    if ($again.Count) { [void]$warnings.Add('Still not valid after trusting the signer: ' + (($again | ForEach-Object { (Split-Path $_.Path -Leaf) + ' (' + $_.Status + ')' }) -join ', ') + '. The file was changed after signing, or the catalog does not cover it.') }
} elseif ($untrusted.Count) {
    [void]$warnings.Add('The signature is not trusted in this guest: ' + (($untrusted | ForEach-Object { $_.file + ' (' + $_.status + ')' }) -join ', ') + '. Call install_driver with trust_certificate true to add the signer to the guest''s Root and TrustedPublisher stores.')
}
$r.trustedCertificates = $trusted
$microsoft = @($certs.Values | Where-Object { $_.Issuer -match 'Microsoft' }).Count -gt 0
if (-not $r.testSigning -and -not $microsoft) {
    $how = if ($r.secureBoot) { 'Secure Boot is on in this VM, so test signing cannot be turned on: create the VM with test_mode true.' } else { 'Turn it on with run_command "bcdedit /set testsigning on" and restart_vm.' }
    [void]$warnings.Add('Test signing is off in this guest, so Windows will not load a test-signed kernel driver (the device shows problem code 52, or the service fails to start with error 577). ' + $how)
}
$r.warnings = $warnings
if ($p.checkOnly) { $r.ok = $true; $r.checkedOnly = $true; $r | ConvertTo-Json -Depth 6 -Compress; exit 0 }

function DeviceInfo($d) {
    [ordered]@{ name = $d.FriendlyName; class = $d.Class; instanceId = $d.InstanceId; status = [string]$d.Status
                problemCode = [int]$d.ConfigManagerErrorCode; problem = [string]$d.Problem }
}

if ($p.inf) {
    $ErrorActionPreference = 'Continue'
    $out = (& pnputil.exe /add-driver $p.inf /install 2>&1 | Out-String)
    $code = $LASTEXITCODE
    $r.pnputilExitCode = $code
    $r.pnputilOutput = $out.Trim()
    # 0 = done, 3010 = done, restart needed, 259 = added, no device used it yet
    $r.ok = ($code -eq 0 -or $code -eq 3010 -or $code -eq 259)
    $r.rebootRequired = ($code -eq 3010)
    $m = [regex]::Match($out, '(?i)\boem\d+\.inf\b')
    $r.publishedName = $(if ($m.Success) { $m.Value } else { $null })
    $devices = New-Object Collections.ArrayList
    if ($r.publishedName) {
        $ids = @(Get-CimInstance Win32_PnPSignedDriver -ErrorAction SilentlyContinue | Where-Object { $_.InfName -ieq $r.publishedName } | ForEach-Object { $_.DeviceID })
        foreach ($id in $ids) {
            $d = Get-PnpDevice -InstanceId $id -ErrorAction SilentlyContinue
            if ($d) { [void]$devices.Add((DeviceInfo $d)) }
        }
    }
    $r.devices = $devices
    if ($r.ok -and -not $devices.Count) { $r.note = 'The package is in the driver store but no present device uses it. For a root-enumerated (software) device create the device node first, e.g. with devcon install <inf> <hardware id> (copy devcon.exe in with copy_to_guest), or use get_devices to look for the device.' }
} elseif ($p.sys -and $p.service) {
    $ErrorActionPreference = 'Continue'
    & sc.exe stop $p.service 2>&1 | Out-Null
    & sc.exe delete $p.service 2>&1 | Out-Null
    $create = (& sc.exe create $p.service type= kernel start= demand binPath= $p.sys 2>&1 | Out-String)
    $createCode = $LASTEXITCODE
    $start = (& sc.exe start $p.service 2>&1 | Out-String)
    $startCode = $LASTEXITCODE
    $query = (& sc.exe query $p.service 2>&1 | Out-String)
    $r.service = [ordered]@{ name = $p.service; createExitCode = $createCode; startExitCode = $startCode
                             running = [bool]($query -match 'RUNNING'); output = ($create + $start + $query).Trim() }
    $r.ok = ($createCode -eq 0 -and $startCode -eq 0)
}
$r.problemDevices = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.ConfigManagerErrorCode -ne 0 -and $_.ConfigManagerErrorCode -ne 'CM_PROB_NONE' } | Select-Object -First 20 | ForEach-Object { DeviceInfo $_ })
$r | ConvertTo-Json -Depth 6 -Compress
