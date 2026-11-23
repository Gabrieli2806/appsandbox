# uninstall_driver. Parameters: publishedName (oemNN.inf), service.
param([string]$ParamFile)
$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = New-Object Text.UTF8Encoding $false
$p = Get-Content -Raw -Encoding UTF8 -LiteralPath $ParamFile | ConvertFrom-Json
Remove-Item -LiteralPath $ParamFile -Force -ErrorAction SilentlyContinue
$r = [ordered]@{ ok = $true }
if ($p.publishedName) {
    $out = (& pnputil.exe /delete-driver $p.publishedName /uninstall /force 2>&1 | Out-String)
    $r.pnputilExitCode = $LASTEXITCODE
    $r.pnputilOutput = $out.Trim()
    $r.rebootRequired = ($LASTEXITCODE -eq 3010)
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010) { $r.ok = $false }
}
if ($p.service) {
    $stop = (& sc.exe stop $p.service 2>&1 | Out-String)
    $delete = (& sc.exe delete $p.service 2>&1 | Out-String)
    $r.service = [ordered]@{ name = $p.service; deleteExitCode = $LASTEXITCODE; output = ($stop + $delete).Trim() }
    if ($LASTEXITCODE -ne 0) { $r.ok = $false }
}
$r | ConvertTo-Json -Depth 5 -Compress
