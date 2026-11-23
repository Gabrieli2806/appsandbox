# send_input: which window has the keyboard focus. Runs in the desktop session.
param([string]$ParamFile)
$ErrorActionPreference = 'SilentlyContinue'
[Console]::OutputEncoding = New-Object Text.UTF8Encoding $false
Remove-Item -LiteralPath $ParamFile -Force
Add-Type -Namespace Asb -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
'@
$h = [Asb.Win]::GetForegroundWindow()
$sb = New-Object Text.StringBuilder 512
[Asb.Win]::GetWindowText($h, $sb, 512) | Out-Null
$procId = 0
[Asb.Win]::GetWindowThreadProcessId($h, [ref]$procId) | Out-Null
$name = (Get-Process -Id $procId).ProcessName
[ordered]@{ title = $sb.ToString(); process = [string]$name } | ConvertTo-Json -Compress
