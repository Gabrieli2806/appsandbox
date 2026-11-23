# get_debug_output: DbgPrint / KdPrint text through the kernel logger's DbgPrint
# events (EVENT_TRACE_FLAG_DBGPRINT), no debugger needed. Optionally a second
# session for ETW / WPP providers, kept as an .etl file.
# Parameters: action (start, read, stop, status), providers, enableFilter, max.
param([string]$ParamFile)
$ErrorActionPreference = 'SilentlyContinue'
[Console]::OutputEncoding = New-Object Text.UTF8Encoding $false
$p = Get-Content -Raw -Encoding UTF8 -LiteralPath $ParamFile | ConvertFrom-Json
Remove-Item -LiteralPath $ParamFile -Force
$dir = Join-Path $env:SystemRoot 'Temp\AppSandbox'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$kernel = 'NT Kernel Logger'
$etl = Join-Path $dir 'dbgprint.etl'
$traceName = 'AppSandboxTrace'
$traceEtl = Join-Path $dir 'providers.etl'
$filterKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter'
$r = [ordered]@{ ok = $true; action = $p.action }

function Running($name) { & logman.exe query $name -ets 2>$null | Out-Null; return ($LASTEXITCODE -eq 0) }
function Ours { return ((& logman.exe query $kernel -ets 2>$null | Out-String) -match 'dbgprint\.etl') }
function StartKernel {
    Remove-Item -LiteralPath $etl -Force
    $o = (& logman.exe start $kernel -p 'Windows Kernel Trace' 0x40000 -o $etl -bs 64 -nb 16 128 -ets 2>&1 | Out-String)
    return [ordered]@{ ok = ($LASTEXITCODE -eq 0); output = $o.Trim() }
}
# Turns the stopped .etl into lines. Only events of the DbgPrint class count
# (the file also holds the trace header and system configuration events);
# they carry: component id, level, text.
$dbgPrintGuid = '{13976d09-a327-438c-950b-7f03192815c7}'
function Decode {
    $lines = New-Object Collections.ArrayList
    $raw = 0; $sample = $null; $kinds = @{}
    if (-not (Test-Path -LiteralPath $etl)) { return @{ lines = $lines; undecoded = 0; sample = $null; kinds = $kinds } }
    $xml = Join-Path $dir 'dbgprint.xml'
    Remove-Item -LiteralPath $xml -Force
    & tracerpt.exe $etl -o $xml -of XML -lr -y 2>&1 | Out-Null
    if (Test-Path -LiteralPath $xml) {
        $reader = [Xml.XmlReader]::Create($xml)
        while ($reader.ReadToFollowing('Event')) {
            $e = [xml]$reader.ReadOuterXml()
            $ev = $e.Event
            $guid = [string]$ev.System.Provider.Guid
            $name = [string]$ev.RenderingInfo.EventName + ' ' + [string]$ev.RenderingInfo.Task + ' ' + [string]$ev.RenderingInfo.Opcode
            $isDbg = ($guid -ieq $dbgPrintGuid) -or ($name -match 'DbgPrint|DebugPrint')
            if (-not $isDbg) {
                $kind = ($guid + ' ' + $name).Trim()
                $kinds[$kind] = 1 + [int]$kinds[$kind]
                continue
            }
            $time = [string]$ev.System.TimeCreated.SystemTime
            if ($time.Length -ge 23) { $time = $time.Substring(11, 12) }
            $msg = $null
            foreach ($d in @($ev.EventData.Data)) {
                if ($d.Name -match '^(Message|Text|String)$') { $msg = [string]$d.'#text' }
            }
            if ($null -eq $msg) {   # no field by that name: the last text field
                foreach ($d in @($ev.EventData.Data)) { if ($d.Name -notmatch '^(Component|ComponentId|Level)$' -and $d.'#text') { $msg = [string]$d.'#text' } }
            }
            if ($null -eq $msg -and $ev.BinaryEventData) {
                $hex = [string]$ev.BinaryEventData
                if ($hex.Length -ge 18) {
                    $bytes = New-Object byte[] ($hex.Length / 2)
                    for ($i = 0; $i -lt $bytes.Length; $i++) { $bytes[$i] = [Convert]::ToByte($hex.Substring($i * 2, 2), 16) }
                    $end = [Array]::IndexOf($bytes, [byte]0, 8); if ($end -lt 0) { $end = $bytes.Length }
                    $msg = [Text.Encoding]::Default.GetString($bytes, 8, $end - 8)
                }
            }
            if ($null -ne $msg) { [void]$lines.Add(($time + '  ' + $msg.TrimEnd("`r", "`n"))) }
            else { $raw++; if (-not $sample) { $sample = $e.OuterXml; if ($sample.Length -gt 1500) { $sample = $sample.Substring(0, 1500) } } }
        }
        $reader.Close()
        Remove-Item -LiteralPath $xml -Force
    }
    return @{ lines = $lines; undecoded = $raw; sample = $sample; kinds = $kinds }
}
function Emit($d) {
    $text = ($d.lines -join "`n")
    $r.lineCount = $d.lines.Count
    if ($text.Length -gt [int]$p.max) { $text = $text.Substring($text.Length - [int]$p.max); $r.truncated = $true }
    $r.output = $text
    if ($d.undecoded) { $r.undecodedEvents = $d.undecoded; $r.undecodedSample = $d.sample }
    # With no DbgPrint line at all, say what else was in the trace (to tell "the driver printed nothing" from "not recognised").
    if (-not $d.lines.Count -and -not $d.undecoded) {
        $r.otherTraceEvents = @($d.kinds.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 8 | ForEach-Object { '' + $_.Value + ' x ' + $_.Key })
    }
}

switch ($p.action) {
    'start' {
        if ($p.enableFilter) {
            New-Item -Path $filterKey -Force | Out-Null
            Set-ItemProperty -Path $filterKey -Name 'DEFAULT' -Value 0xF -Type DWord
            Set-ItemProperty -Path $filterKey -Name 'IHVDRIVER' -Value 0xF -Type DWord
            $r.filterSet = 'DEFAULT and IHVDRIVER set to 0xF (all levels). The filter is read at boot: restart_vm, then start again.'
        }
        if ((Running $kernel) -and -not (Ours)) { $r.ok = $false; $r.error = 'Another tool in the guest is using the NT Kernel Logger session. Stop it (logman stop "NT Kernel Logger" -ets) and start again.' }
        else {
            if (Running $kernel) { & logman.exe stop $kernel -ets 2>&1 | Out-Null }
            $s = StartKernel
            $r.ok = $s.ok
            if (-not $s.ok) { $r.error = $s.output }
        }
        if ($r.ok -and @($p.providers).Count) {
            if (Running $traceName) { & logman.exe stop $traceName -ets 2>&1 | Out-Null }
            Remove-Item -LiteralPath $traceEtl -Force
            $first = $true; $po = ''
            foreach ($prov in @($p.providers)) {
                if ($first) { $po += (& logman.exe start $traceName -p $prov 0xffffffffffffffff 0xff -o $traceEtl -ets 2>&1 | Out-String); $first = $false }
                else { $po += (& logman.exe update $traceName -p $prov 0xffffffffffffffff 0xff -ets 2>&1 | Out-String) }
                if ($LASTEXITCODE -ne 0) { $r.ok = $false; $r.error = 'Could not enable provider ' + $prov + ': ' + $po.Trim() }
            }
            $r.providers = @($p.providers)
        }
    }
    'read' {
        if (-not (Ours)) { $r.ok = $false; $r.error = 'Capture is not running. Call get_debug_output with action "start" first (and again after every restart of the guest).' }
        else {
            # Stop, decode what was captured and start again: the file cannot be read while the session writes it.
            & logman.exe stop $kernel -ets 2>&1 | Out-Null
            $d = Decode
            $s = StartKernel
            if (-not $s.ok) { $r.restartError = $s.output }
            Emit $d
        }
    }
    'stop' {
        if (Ours) { & logman.exe stop $kernel -ets 2>&1 | Out-Null; Emit (Decode); Remove-Item -LiteralPath $etl -Force }
        else { $r.output = ''; $r.lineCount = 0 }
        if (Running $traceName) {
            & logman.exe stop $traceName -ets 2>&1 | Out-Null
            if (Test-Path -LiteralPath $traceEtl) { $r.providerTrace = [ordered]@{ guestPath = $traceEtl; bytes = (Get-Item -LiteralPath $traceEtl).Length } }
        }
    }
}
$r.capturing = [bool](Ours)
$f = Get-ItemProperty -Path $filterKey
$r.debugPrintFilter = [ordered]@{ DEFAULT = $(if ($null -ne $f.DEFAULT) { '0x{0:X}' -f $f.DEFAULT } else { $null })
                                  IHVDRIVER = $(if ($null -ne $f.IHVDRIVER) { '0x{0:X}' -f $f.IHVDRIVER } else { $null }) }
$r | ConvertTo-Json -Depth 5 -Compress
