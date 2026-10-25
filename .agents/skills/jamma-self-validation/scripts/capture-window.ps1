param([Parameter(Mandatory)][string]$Session,[Parameter(Mandatory)][string]$Screenshot,[Parameter(Mandatory)][string]$Output)
$ErrorActionPreference='Stop'
$s=Get-Content -Raw -LiteralPath $Session | ConvertFrom-Json
$p=Get-CimInstance Win32_Process -Filter "ProcessId = $($s.pid)"
if (-not $p -or -not [string]::Equals($p.ExecutablePath,$s.binary,[StringComparison]::OrdinalIgnoreCase)) { throw 'Target PID no longer maps to validated binary' }
if (-not (Test-Path -LiteralPath $Screenshot)) { throw 'Computer-use screenshot missing' }
$shot=Get-Item -LiteralPath $Screenshot
if ($shot.LastWriteTime -lt [datetime]$s.started) { throw 'Screenshot predates validated session' }
$proc=Get-Process -Id $s.pid
$proc.Refresh()
if ($proc.MainWindowHandle -eq 0) { throw 'Validated process has no main window' }
$result=@{script_version='1.0.0';pid=$s.pid;process_path=$p.ExecutablePath;window_handle=$proc.MainWindowHandle;screenshot=(Resolve-Path -LiteralPath $Screenshot).Path;sha256=(Get-FileHash -LiteralPath $Screenshot -Algorithm SHA256).Hash.ToLowerInvariant();captured=(Get-Date).ToString('o')}
$result | ConvertTo-Json | Set-Content -LiteralPath $Output -Encoding utf8
$result | ConvertTo-Json
