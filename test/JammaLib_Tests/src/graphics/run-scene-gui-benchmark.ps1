param(
    [string]$RepoRoot = (Join-Path $PSScriptRoot '../../../..'),
    [ValidateRange(1, 59)][int]$TimeoutSeconds = 45
)
$ErrorActionPreference = 'Stop'
$benchmarkRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
Get-Content (Join-Path $benchmarkRoot '.vscode/tasks.json') | Out-Null
$benchmarkExe = Join-Path $benchmarkRoot 'test/JammaLib_Tests/bin/x64/Debug/JammaLib_Tests.exe'
if (-not (Test-Path -LiteralPath $benchmarkExe)) { throw 'Build the Debug native tests using the local task first.' }
$outputRoot = Join-Path $benchmarkRoot 'test/JammaLib_Tests/bin/x64/Debug/gui-benchmark'
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
# Some launch environments publish both Path and PATH. Normalize this launcher
# process before .NET constructs its case-insensitive inherited dictionary.
$environment = [Environment]::GetEnvironmentVariables('Process')
$pathKeys = @($environment.Keys | Where-Object { $_ -ieq 'PATH' })
if ($pathKeys.Count -gt 1) {
    $inheritedPath = (@($pathKeys | ForEach-Object { $environment[$_] }) -join ';')
    $env:PATH = $null
    $env:Path = $inheritedPath
}
$startInfo = New-Object System.Diagnostics.ProcessStartInfo
$startInfo.FileName = $benchmarkExe
$startInfo.Arguments = '--gtest_filter=GuiBenchmark.SameSceneFrameDistribution'
$startInfo.WorkingDirectory = $benchmarkRoot
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
$environment = [Environment]::GetEnvironmentVariables('Process')
$startInfo.EnvironmentVariables.Clear()
foreach ($entry in $environment.GetEnumerator()) { $startInfo.EnvironmentVariables[$entry.Key] = $entry.Value }
$startInfo.EnvironmentVariables['JAMMA_GUI_BENCHMARK_DIR'] = $outputRoot
$benchmarkProcess = New-Object System.Diagnostics.Process
$benchmarkProcess.StartInfo = $startInfo
try {
    $startedUtc = [DateTime]::UtcNow
    if (-not $benchmarkProcess.Start()) { throw 'Could not start the owned benchmark process.' }
    $stdout = $benchmarkProcess.StandardOutput.ReadToEndAsync()
    $stderr = $benchmarkProcess.StandardError.ReadToEndAsync()
    $timedOut = -not $benchmarkProcess.WaitForExit($TimeoutSeconds * 1000)
    if ($timedOut) { $benchmarkProcess.Kill() }
    $benchmarkProcess.WaitForExit()
    $stdoutText = $stdout.Result
    $stderrText = $stderr.Result
    [IO.File]::WriteAllText((Join-Path $outputRoot 'run.log'), $stdoutText)
    [IO.File]::WriteAllText((Join-Path $outputRoot 'errors.log'), $stderrText)
    Get-Content (Join-Path $outputRoot 'run.log') -Tail 8
    if ($stderrText) { Get-Content (Join-Path $outputRoot 'errors.log') -Tail 8 }
    if ($timedOut) { throw "Owned benchmark timed out after $TimeoutSeconds seconds and was stopped." }
    if ($benchmarkProcess.ExitCode -ne 0) { throw "Benchmark exited with code $($benchmarkProcess.ExitCode)." }
    if ($stdoutText -notmatch '\[\s+PASSED\s+\]\s+1 test\.') { throw 'The benchmark test did not report one passing test.' }
    $frameFile = Get-Item -LiteralPath (Join-Path $outputRoot 'frames.csv')
    if ($frameFile.LastWriteTimeUtc -lt $startedUtc) { throw 'Frame measurements were not refreshed by this run.' }
    $frames = @(Import-Csv -LiteralPath $frameFile.FullName)
    if ($frames.Count -ne 600) { throw "Expected 600 measured frames; found $($frames.Count)." }
} finally {
    $benchmarkProcess.Dispose()
}
