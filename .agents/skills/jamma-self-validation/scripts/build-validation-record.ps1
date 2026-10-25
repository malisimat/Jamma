param(
    [Parameter(Mandatory)][string]$Repo,
    [Parameter(Mandatory)][ValidateSet('Debug', 'Release')][string]$Configuration,
    [Parameter(Mandatory)][string]$BuildRecord,
    [Parameter(Mandatory)][string]$BuildLog
)

$ErrorActionPreference = 'Stop'
$root = (git -C $Repo rev-parse --show-toplevel).Trim()
if ((Resolve-Path -LiteralPath $Repo).Path -ne (Resolve-Path -LiteralPath $root).Path) { throw 'Repo argument must be the repository root' }
if (-not (Test-Path -LiteralPath (Join-Path $root 'Jamma.sln'))) { throw 'Jamma.sln missing' }
if (-not (Test-Path -LiteralPath (Join-Path $root 'AGENTS.md'))) { throw 'AGENTS.md missing' }

$tasksPath = Join-Path $root '.vscode\tasks.json'
if (-not (Test-Path -LiteralPath $tasksPath)) { throw 'Active .vscode\tasks.json missing' }
$tasks = Get-Content -Raw -LiteralPath $tasksPath | ConvertFrom-Json
$taskLabel = "Build Solution ($Configuration x64)"
$task = @($tasks.tasks | Where-Object { $_.label -eq $taskLabel })
if ($task.Count -ne 1 -or $task[0].type -ne 'shell') { throw "Expected exactly one shell task: $taskLabel" }

$command = [string]$task[0].command
$match = [regex]::Match($command, "^&\s*'(?<msbuild>[^']+MSBuild\.exe)'\s+Jamma\.sln\s+/m\s+/t:Build\s+/p:Configuration=(?<config>Debug|Release)\s+/p:Platform=x64$")
if (-not $match.Success -or $match.Groups['config'].Value -ne $Configuration) { throw "Unsupported task syntax; inspect $tasksPath" }
$msbuild = $match.Groups['msbuild'].Value
if (-not (Test-Path -LiteralPath $msbuild)) { throw "MSBuild not found: $msbuild" }

$recordPath = [IO.Path]::GetFullPath($BuildRecord)
$logPath = [IO.Path]::GetFullPath($BuildLog)
if ([string]::Equals($recordPath, $logPath, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'BuildRecord and BuildLog must be different paths'
}
$recordDirectory = Split-Path -Parent $recordPath
$logDirectory = Split-Path -Parent $logPath
New-Item -ItemType Directory -Force -Path $recordDirectory, $logDirectory | Out-Null

$arguments = @('Jamma.sln', '/m', '/t:Build', "/p:Configuration=$Configuration", '/p:Platform=x64')
& $msbuild @arguments *> $logPath
$exitCode = $LASTEXITCODE
$binary = Join-Path $root "Jamma\bin\x64\$Configuration\Jamma.exe"
$binaryHash = if ($exitCode -eq 0 -and (Test-Path -LiteralPath $binary)) { (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash.ToLowerInvariant() } else { $null }
if ($exitCode -eq 0 -and $null -eq $binaryHash) { throw "Build succeeded but expected binary is missing: $binary" }

$record = [ordered]@{
    script_version = '1.0.0'
    command = @($msbuild) + $arguments
    cwd = $root
    exit_code = $exitCode
    log = $logPath
    task = $taskLabel
    task_sha256 = (Get-FileHash -LiteralPath $tasksPath -Algorithm SHA256).Hash.ToLowerInvariant()
    binary = if (Test-Path -LiteralPath $binary) { (Resolve-Path -LiteralPath $binary).Path } else { $binary }
    binary_sha256 = $binaryHash
}
$record | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $recordPath -Encoding utf8
Get-Content -Raw -LiteralPath $recordPath | ConvertFrom-Json | Out-Null
if ($exitCode -ne 0) { throw "Build failed; see $logPath" }
