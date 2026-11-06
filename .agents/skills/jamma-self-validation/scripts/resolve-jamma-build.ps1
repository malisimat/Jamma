param([Parameter(Mandatory)][string]$Repo,[Parameter(Mandatory)][string]$BuildRecord,[Parameter(Mandatory)][string]$Output)
$ErrorActionPreference='Stop'
$root=(git -C $Repo rev-parse --show-toplevel).Trim()
if ((Resolve-Path -LiteralPath $Repo).Path -ne (Resolve-Path -LiteralPath $root).Path) { throw 'Repo argument must be verified root' }
if (-not ((Test-Path -LiteralPath (Join-Path $root 'Jamma.sln')) -and (Test-Path -LiteralPath (Join-Path $root 'AGENTS.md')))) { throw 'Jamma markers missing' }
$build=Get-Content -Raw -LiteralPath $BuildRecord | ConvertFrom-Json
if ($build.exit_code -ne 0 -or -not $build.binary_sha256) { throw 'Successful build record and binary hash required' }
if (-not $build.task -or -not $build.task_sha256) { throw 'Build record lacks task evidence' }
$tasks=Join-Path $root '.vscode\tasks.json'
if (-not (Test-Path -LiteralPath $tasks) -or (Get-FileHash -LiteralPath $tasks -Algorithm SHA256).Hash.ToLowerInvariant() -ne $build.task_sha256.ToLowerInvariant()) { throw 'Build task changed since the recorded build' }
$configuration=if ($build.task -match 'Release') {'Release'} else {'Debug'}
$expected=Join-Path $root "Jamma\bin\x64\$configuration\Jamma.exe"
if (-not (Test-Path -LiteralPath $expected)) { throw "Build output missing: $expected" }
if (-not $build.binary -or -not (Test-Path -LiteralPath $build.binary) -or -not [string]::Equals((Resolve-Path -LiteralPath $expected).Path,(Resolve-Path -LiteralPath $build.binary).Path,[StringComparison]::OrdinalIgnoreCase)) { throw 'Build record points to another binary' }
$hash=(Get-FileHash -LiteralPath $expected -Algorithm SHA256).Hash.ToLowerInvariant()
if ($hash -ne $build.binary_sha256.ToLowerInvariant()) { throw 'Binary changed since successful build' }
$result=[ordered]@{script_version='1.0.0';root=$root;binary=(Resolve-Path -LiteralPath $expected).Path;sha256=$hash;modified=(Get-Item -LiteralPath $expected).LastWriteTime.ToString('o');branch=(git -C $root branch --show-current).Trim();commit=(git -C $root rev-parse HEAD).Trim();configuration=$configuration}
$result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $Output -Encoding utf8
$result | ConvertTo-Json -Depth 5
