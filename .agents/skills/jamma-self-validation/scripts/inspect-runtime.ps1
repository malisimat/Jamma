param([Parameter(Mandatory)][string]$Session,[Parameter(Mandatory)][string]$Capture,[Parameter(Mandatory)][string]$Output)
$ErrorActionPreference='Stop'
$s=Get-Content -Raw -LiteralPath $Session | ConvertFrom-Json
$c=Get-Content -Raw -LiteralPath $Capture | ConvertFrom-Json
if ($s.pid -ne $c.pid -or $s.binary -ne $c.process_path) { throw 'Capture does not match validated process' }
if ([datetime]$c.captured -lt [datetime]$s.started) { throw 'Capture predates validated session' }
if ((Get-FileHash -LiteralPath $c.screenshot -Algorithm SHA256).Hash.ToLowerInvariant() -ne $c.sha256) { throw 'Screenshot hash mismatch' }
$result=@{script_version='1.0.0';pid=$s.pid;binary=$s.binary;binary_sha256=$s.sha256;screenshot=$c.screenshot;screenshot_sha256=$c.sha256;visual_review='pending computer-use inspection';manual_hardware_checks='unknown'}
$result | ConvertTo-Json | Set-Content -LiteralPath $Output -Encoding utf8
$result | ConvertTo-Json
