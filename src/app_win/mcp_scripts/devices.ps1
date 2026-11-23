# get_devices. Parameters: problemOnly, filter, max.
param([string]$ParamFile)
$ErrorActionPreference = 'SilentlyContinue'
[Console]::OutputEncoding = New-Object Text.UTF8Encoding $false
$p = Get-Content -Raw -Encoding UTF8 -LiteralPath $ParamFile | ConvertFrom-Json
Remove-Item -LiteralPath $ParamFile -Force
$all = @(Get-PnpDevice -PresentOnly)
$sel = $all
if ($p.problemOnly) { $sel = @($sel | Where-Object { $_.ConfigManagerErrorCode -ne 0 -and $_.ConfigManagerErrorCode -ne 'CM_PROB_NONE' }) }
if ($p.filter) { $f = [string]$p.filter; $sel = @($sel | Where-Object { ([string]$_.FriendlyName).IndexOf($f, [StringComparison]::OrdinalIgnoreCase) -ge 0 -or ([string]$_.InstanceId).IndexOf($f, [StringComparison]::OrdinalIgnoreCase) -ge 0 -or ([string]$_.Class).IndexOf($f, [StringComparison]::OrdinalIgnoreCase) -ge 0 }) }
$drivers = @{}
Get-CimInstance Win32_PnPSignedDriver | Where-Object { $_.InfName -like 'oem*.inf' } | ForEach-Object {
    if (-not $drivers.ContainsKey($_.InfName)) {
        $drivers[$_.InfName] = [ordered]@{ publishedName = $_.InfName; provider = $_.DriverProviderName; version = $_.DriverVersion
                                          signer = $_.Signer; device = $_.DeviceName }
    }
}
$r = [ordered]@{
    presentDevices = $all.Count
    matching = $sel.Count
    devices = @($sel | Select-Object -First ([int]$p.max) | ForEach-Object {
        [ordered]@{ name = $_.FriendlyName; class = $_.Class; instanceId = $_.InstanceId; status = [string]$_.Status
                    problemCode = [int]$_.ConfigManagerErrorCode; problem = [string]$_.Problem } })
    thirdPartyDrivers = @($drivers.Values | Sort-Object { $_.publishedName })
}
$r | ConvertTo-Json -Depth 5 -Compress
