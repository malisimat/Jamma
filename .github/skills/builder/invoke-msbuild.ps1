[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Arguments
)

$ErrorActionPreference = "Stop"

function ConvertTo-ProcessArgument {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Argument
    )

    # ProcessStartInfo.ArgumentList is unavailable in Windows PowerShell 5.1.
    # Quote each argument with the CommandLineToArgvW backslash rules so the
    # Arguments fallback is equivalent on both Windows PowerShell and pwsh.
    $quoted = [System.Text.StringBuilder]::new()
    [void]$quoted.Append('"')
    $backslashes = 0

    foreach ($character in $Argument.ToCharArray()) {
        if ($character -eq [char]92) {
            $backslashes++
            continue
        }

        if ($character -eq [char]34) {
            for ($index = 0; $index -lt ((2 * $backslashes) + 1); $index++) {
                [void]$quoted.Append('\\')
            }
            [void]$quoted.Append('"')
            $backslashes = 0
            continue
        }

        for ($index = 0; $index -lt $backslashes; $index++) {
            [void]$quoted.Append('\\')
        }
        [void]$quoted.Append($character)
        $backslashes = 0
    }

    for ($index = 0; $index -lt (2 * $backslashes); $index++) {
        [void]$quoted.Append('\\')
    }
    [void]$quoted.Append('"')
    return $quoted.ToString()
}

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

$startInfo.Arguments = ($Arguments | ForEach-Object { ConvertTo-ProcessArgument $_ }) -join ' '

$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $startInfo
if (-not $process.Start()) {
    throw "Failed to start MSBuild: $Executable"
}
$process.WaitForExit()
$exitCode = $process.ExitCode
$process.Dispose()
if ($exitCode -ne 0) {
    throw "MSBuild failed with exit code $exitCode."
}
