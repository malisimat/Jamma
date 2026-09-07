[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    throw "git is required for this audit script."
}

function Get-RepoRoot {
    $repoRoot = (Get-Location).Path
    while (-not (Test-Path (Join-Path $repoRoot "Jamma.sln"))) {
        $parent = Split-Path $repoRoot -Parent
        if ($parent -eq $repoRoot) {
            throw "Could not find Jamma.sln. Start in this repository or set the working directory inside it."
        }

        $repoRoot = $parent
    }

    return $repoRoot
}

$repoRoot = Get-RepoRoot
Set-Location $repoRoot

$insideWorkTree = (& git rev-parse --is-inside-work-tree 2>$null).Trim()
if ($LASTEXITCODE -ne 0 -or $insideWorkTree -ne "true") {
    throw "Current directory is not a git work tree: $repoRoot"
}

$hotPathFiles = @(
    "JammaLib\src\engine\Scene.cpp",
    "JammaLib\src\engine\Loop.cpp",
    "JammaLib\src\engine\LoopTake.cpp",
    "JammaLib\src\engine\Station.cpp",
    "JammaLib\src\engine\Trigger.cpp",
    "JammaLib\src\engine\NinjamConnection.cpp"
)

$bannedPattern = 'std::mutex|std::shared_mutex|std::scoped_lock|std::lock_guard|std::unique_lock|std::condition_variable|EnterCriticalSection|WaitForSingleObject|SleepConditionVariableCS|SleepConditionVariableSRW'
$sharedStatePattern = 'std::atomic|memory_order|mutex|shared_mutex|lock_guard|scoped_lock|unique_lock|condition_variable|_audioMutex|Interlocked|volatile|std::thread|std::jthread|SRWLOCK'

$hotPathDiff = & git --no-pager diff --unified=0 HEAD -- @hotPathFiles
$repoDiff = & git --no-pager diff --unified=0 HEAD -- "Jamma" "JammaLib" "test"

$hotPathAddedLines = $hotPathDiff -split "`r?`n" | Where-Object { $_ -match '^\+' -and $_ -notmatch '^\+\+\+' }
$sharedStateAddedLines = $repoDiff -split "`r?`n" | Where-Object { $_ -match '^\+' -and $_ -notmatch '^\+\+\+' }

$bannedHits = $hotPathAddedLines | Where-Object { $_ -match $bannedPattern }
$sharedStateHits = $sharedStateAddedLines | Where-Object { $_ -match $sharedStatePattern }

if ($bannedHits.Count -gt 0) {
    Write-Host "Banned lock/wait additions detected in hot-path diff:" -ForegroundColor Red
    $bannedHits | ForEach-Object { Write-Host $_ }
    exit 1
}

Write-Host "No banned lock/wait additions detected in hot-path diff."

if ($sharedStateHits.Count -gt 0) {
    Write-Host ""
    Write-Host "Shared-state additions to review manually:" -ForegroundColor Yellow
    $sharedStateHits | ForEach-Object { Write-Host $_ }
}
else {
    Write-Host "No new shared-state keywords found in the current diff."
}
