# set_verifier. Parameters: preset (standard, reset, custom, query), flags, drivers.
param([string]$ParamFile)
$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = New-Object Text.UTF8Encoding $false
$p = Get-Content -Raw -Encoding UTF8 -LiteralPath $ParamFile | ConvertFrom-Json
Remove-Item -LiteralPath $ParamFile -Force -ErrorAction SilentlyContinue
$r = [ordered]@{ ok = $true; preset = $p.preset }
$out = ''
switch ($p.preset) {
    'standard' { $out = (& verifier.exe /standard /driver @($p.drivers) 2>&1 | Out-String) }
    'custom'   { $out = (& verifier.exe /flags $p.flags /driver @($p.drivers) 2>&1 | Out-String) }
    'reset'    { $out = (& verifier.exe /reset 2>&1 | Out-String) }
}
if ($p.preset -ne 'query') {
    $r.exitCode = $LASTEXITCODE
    $r.output = $out.Trim()
    # verifier.exe answers 2 when the settings changed and take effect after a restart
    $r.ok = ($LASTEXITCODE -eq 0 -or $LASTEXITCODE -eq 2)
    $r.rebootNeeded = $r.ok
}
$r.settings = ((& verifier.exe /querysettings 2>&1 | Out-String).Trim())
$r | ConvertTo-Json -Depth 4 -Compress
