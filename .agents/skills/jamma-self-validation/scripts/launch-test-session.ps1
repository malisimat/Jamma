param([Parameter(Mandatory)][string]$ResolvedBuild,[Parameter(Mandatory)][string]$DefaultsPath,[Parameter(Mandatory)][string]$Output)
$ErrorActionPreference='Stop'
$b=Get-Content -Raw -LiteralPath $ResolvedBuild | ConvertFrom-Json
if ($b.script_version -ne '1.0.0' -or -not (Test-Path -LiteralPath $b.binary)) { throw 'Validated build record required' }
if ((Get-FileHash -LiteralPath $b.binary -Algorithm SHA256).Hash.ToLowerInvariant() -ne $b.sha256) { throw 'Binary changed' }
$others=Get-CimInstance Win32_Process -Filter "Name = 'Jamma.exe'" | Select-Object ProcessId,ExecutablePath,CreationDate
if ($others) { throw ('Existing Jamma instance; inspect unsaved work first: ' + ($others | ConvertTo-Json -Compress)) }
if (Test-Path -LiteralPath $DefaultsPath) { throw 'Disposable defaults path already exists; use a fresh run directory' }
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $DefaultsPath) | Out-Null
$defaultsAbs=[IO.Path]::GetFullPath($DefaultsPath)
$startInfo=[System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName=$b.binary
$startInfo.WorkingDirectory=Split-Path -Parent $b.binary
$startInfo.UseShellExecute=$false
$startInfo.EnvironmentVariables['JAMMA_DEFAULTS_PATH']=$defaultsAbs
$p=[System.Diagnostics.Process]::Start($startInfo)
if (-not $p) { throw 'Jamma process did not start' }
Start-Sleep -Milliseconds 700
$live=Get-CimInstance Win32_Process -Filter "ProcessId = $($p.Id)"
if (-not $live -or -not [string]::Equals($live.ExecutablePath,$b.binary,[StringComparison]::OrdinalIgnoreCase)) { throw 'Launched process path does not match validated binary' }
$result=[ordered]@{script_version='1.0.0';pid=$p.Id;binary=$b.binary;sha256=$b.sha256;started=(Get-Date).ToString('o');process_path=$live.ExecutablePath;defaults_path=$defaultsAbs}
$result | ConvertTo-Json | Set-Content -LiteralPath $Output -Encoding utf8
$result | ConvertTo-Json
