param([Parameter(Mandatory)][string]$Repo,[Parameter(Mandatory)][string]$DefaultsPath,[Parameter(Mandatory)][datetime]$Since,[Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
$root=(git -C $Repo rev-parse --show-toplevel).Trim()
if (-not (Test-Path -LiteralPath (Join-Path $root 'doc\diagnostics.md'))) { throw 'Diagnostics guide missing' }
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a fresh log output directory' }
$folder=Split-Path -Parent $DefaultsPath
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$records=@()
foreach ($name in @('startup.log','startup.log.1','startup.log.2','startup.log.3','worker-failure.log')) {
  $src=Join-Path $folder $name
  if (Test-Path -LiteralPath $src) {
    $file=Get-Item -LiteralPath $src
    if ($file.LastWriteTime -ge $Since) {
      $dst=Join-Path $OutputDirectory $name
      Copy-Item -LiteralPath $src -Destination $dst -ErrorAction Stop
      $records+=@{source=$src;copy=$dst;sha256=(Get-FileHash -LiteralPath $dst -Algorithm SHA256).Hash.ToLowerInvariant();modified=$file.LastWriteTime.ToString('o')}
    }
  }
}
$manifest=@{script_version='1.0.0';since=$Since.ToString('o');logs=$records}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'manifest.json') -Encoding utf8
$manifest | ConvertTo-Json -Depth 5
