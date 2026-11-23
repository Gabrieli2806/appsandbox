# analyze_dump: which dump to analyse, and whether the guest has a debugger.
# Parameters: path (optional). Without one: the newest minidump, else MEMORY.DMP.
# Live kernel reports (often hundreds of MB) are never picked by default.
param([string]$ParamFile)
$ErrorActionPreference = 'SilentlyContinue'
[Console]::OutputEncoding = New-Object Text.UTF8Encoding $false
$p = Get-Content -Raw -Encoding UTF8 -LiteralPath $ParamFile | ConvertFrom-Json
Remove-Item -LiteralPath $ParamFile -Force
$r = [ordered]@{ found = $false }
$f = $null
if ($p.path) { $f = Get-Item -LiteralPath $p.path }
else {
    $f = Get-ChildItem (Join-Path $env:SystemRoot 'Minidump\*.dmp') | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $f) { $f = Get-Item -LiteralPath (Join-Path $env:SystemRoot 'MEMORY.DMP') }
}
if ($f -and -not $f.PSIsContainer) { $r.found = $true; $r.path = $f.FullName; $r.bytes = $f.Length; $r.written = $f.LastWriteTime.ToString('s') }
$dbg = $null
foreach ($root in ${env:ProgramFiles(x86)}, $env:ProgramFiles) {
    foreach ($exe in 'cdb.exe', 'kd.exe') {
        $c = Join-Path $root ('Windows Kits\10\Debuggers\x64\' + $exe)
        if (-not $dbg -and (Test-Path -LiteralPath $c)) { $dbg = $c }
    }
}
if (-not $dbg) { $w = Get-Command cdb.exe, kd.exe | Select-Object -First 1; if ($w) { $dbg = $w.Source } }
$r.guestDebugger = $dbg
$r | ConvertTo-Json -Compress
