[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Arguments
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw "MSBuild executable not found: $Executable"
}

# Some hosts expose both PATH and Path. Windows treats those names as the same,
# but MSBuild imports them as properties and fails before invoking the compiler.
# Start every build tool in a fresh child environment containing one canonical
# Path entry. This leaves the editor/agent process untouched.
$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $Executable
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
# `Environment` is lazily initialized on some PowerShell/.NET combinations.
# The legacy EnvironmentVariables collection is always materialized and is the
# right fallback for the VS Code Windows PowerShell host.
$environment = $startInfo.Environment
if ($null -eq $environment) {
    $environment = $startInfo.EnvironmentVariables
}
$environment.Clear()

$canonicalPath = $env:Path
foreach ($entry in [Environment]::GetEnvironmentVariables('Process').GetEnumerator()) {
    if ([string]$entry.Key -ieq 'PATH') {
        continue
    }
    $environment[[string]$entry.Key] = [string]$entry.Value
}
$environment['Path'] = $canonicalPath

foreach ($argument in $Arguments) {
    [void]$startInfo.ArgumentList.Add($argument)
}

$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $startInfo
if (-not $process.Start()) {
    throw "Failed to start MSBuild: $Executable"
}
$process.WaitForExit()
$exitCode = $process.ExitCode
$process.Dispose()
if ($exitCode -ne 0) {
    exit $exitCode
}
