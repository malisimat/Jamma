param(
	[string]$Configuration = "Debug",
	[string]$Platform = "x64",
	[string]$DefaultsPath = "",
	[string]$ExecutablePath = "",
	[string]$LogPath = "",
	[int]$StationIndex = 0,
	[int]$PluginIndex = 0,
	[int]$TimeoutSeconds = 20,
	[switch]$NoAutoOpen,
	[switch]$NoFileLog
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($ExecutablePath)) {
	$ExecutablePath = Join-Path $repoRoot ("Jamma\bin\{0}\{1}\Jamma.exe" -f $Platform, $Configuration)
}

if ([string]::IsNullOrWhiteSpace($LogPath)) {
	$LogPath = Join-Path $env:APPDATA "Jamma\vst-diagnostic.log"
}

$artifactDir = Join-Path $repoRoot "artifacts\vst-debug"
$summaryPath = Join-Path $artifactDir "last-run-summary.txt"
$stdoutPath = Join-Path $artifactDir "last-run-stdout.txt"
$stderrPath = Join-Path $artifactDir "last-run-stderr.txt"

New-Item -ItemType Directory -Force -Path $artifactDir | Out-Null
if (Test-Path $LogPath) { Remove-Item $LogPath -Force }
if (Test-Path $summaryPath) { Remove-Item $summaryPath -Force }
if (Test-Path $stdoutPath) { Remove-Item $stdoutPath -Force }
if (Test-Path $stderrPath) { Remove-Item $stderrPath -Force }

if (-not (Test-Path $ExecutablePath)) {
	throw "Jamma executable not found at $ExecutablePath"
}

$previousDefaults = $env:JAMMA_DEFAULTS_PATH
$previousAutoOpen = $env:JAMMA_VST_DEBUG_AUTO_OPEN
$previousLogToFile = $env:JAMMA_VST_DEBUG_LOG_TO_FILE
$previousLogPath = $env:JAMMA_VST_DEBUG_LOG_PATH
$previousStationIndex = $env:JAMMA_VST_DEBUG_STATION_INDEX
$previousPluginIndex = $env:JAMMA_VST_DEBUG_PLUGIN_INDEX

try {
	if (-not [string]::IsNullOrWhiteSpace($DefaultsPath)) {
		$env:JAMMA_DEFAULTS_PATH = $DefaultsPath
	}
	$env:JAMMA_VST_DEBUG_AUTO_OPEN = if ($NoAutoOpen) { "0" } else { "1" }
	$env:JAMMA_VST_DEBUG_LOG_TO_FILE = if ($NoFileLog) { "0" } else { "1" }
	$env:JAMMA_VST_DEBUG_LOG_PATH = $LogPath
	$env:JAMMA_VST_DEBUG_STATION_INDEX = $StationIndex.ToString()
	$env:JAMMA_VST_DEBUG_PLUGIN_INDEX = $PluginIndex.ToString()

	$process = Start-Process -FilePath $ExecutablePath -PassThru -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
	$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
	$opened = $false

	while (-not $process.HasExited -and (Get-Date) -lt $deadline) {
		if (Test-Path $LogPath) {
			$logContent = Get-Content $LogPath -Raw
			if ($logContent -match "auto-open-status \| opened") {
				$opened = $true
				break
			}
		}

		Start-Sleep -Milliseconds 250
		$process.Refresh()
	}

	if (-not $process.HasExited) {
		Stop-Process -Id $process.Id -Force
		$process.WaitForExit()
	}

	$logTail = if (Test-Path $LogPath) {
		(Get-Content $LogPath | Select-Object -Last 40) -join [Environment]::NewLine
	} else {
		"<no log created>"
	}

	$summary = @(
		"timestamp=$(Get-Date -Format o)",
		"executable=$ExecutablePath",
		"defaultsPath=$DefaultsPath",
		"logPath=$LogPath",
		"stationIndex=$StationIndex",
		"pluginIndex=$PluginIndex",
		"timeoutSeconds=$TimeoutSeconds",
		"autoOpenEnabled=$(-not $NoAutoOpen)",
		"fileLogEnabled=$(-not $NoFileLog)",
		"opened=$opened",
		"exitCode=$($process.ExitCode)",
		"---- log tail ----",
		$logTail
	) -join [Environment]::NewLine

	Set-Content -Path $summaryPath -Value $summary
	Write-Host "Summary: $summaryPath"
	Write-Host "Log: $LogPath"
}
finally {
	$env:JAMMA_DEFAULTS_PATH = $previousDefaults
	$env:JAMMA_VST_DEBUG_AUTO_OPEN = $previousAutoOpen
	$env:JAMMA_VST_DEBUG_LOG_TO_FILE = $previousLogToFile
	$env:JAMMA_VST_DEBUG_LOG_PATH = $previousLogPath
	$env:JAMMA_VST_DEBUG_STATION_INDEX = $previousStationIndex
	$env:JAMMA_VST_DEBUG_PLUGIN_INDEX = $previousPluginIndex
}