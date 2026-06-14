[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("JammaLib", "Jamma", "JammaLib_Tests", "Solution")]
    [string]$Target,

    [ValidateSet("Build", "Rebuild", "Clean")]
    [string]$Action = "Build",

    [string]$Configuration = "Debug",
    [string]$Platform = "x64",
    [switch]$RunTests,
    [string]$TestFilter = ""
)

$ErrorActionPreference = "Stop"

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

function Get-MSBuildPath {
    $fromPath = Get-Command msbuild.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($fromPath -and -not [string]::IsNullOrWhiteSpace($fromPath.Source)) {
        return $fromPath.Source
    }

    $vswhereCandidates = @(
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"),
        (Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe")
    )

    $vswhere = $vswhereCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (Test-Path $vswhere) {
        $resolved = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
        if (-not [string]::IsNullOrWhiteSpace($resolved)) {
            return $resolved
        }
    }

    $fallbacks = @(
        (Join-Path $env:ProgramFiles "Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"),
        (Join-Path $env:ProgramFiles "Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"),
        (Join-Path $env:ProgramFiles "Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe"),
        (Join-Path $env:ProgramFiles "Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe")
    )

    foreach ($candidate in $fallbacks) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    throw "MSBuild.exe not found. Install Visual Studio with the C++ desktop workload."
}

$repoRoot = Get-RepoRoot
$msbuild = Get-MSBuildPath
$solutionDirArg = "/p:SolutionDir=$($repoRoot.TrimEnd('\'))\"

$projectMap = @{
    JammaLib = Join-Path $repoRoot "JammaLib\JammaLib.vcxproj"
    Jamma = Join-Path $repoRoot "Jamma\Jamma.vcxproj"
    JammaLib_Tests = Join-Path $repoRoot "test\JammaLib_Tests\JammaLib_Tests.vcxproj"
    Solution = Join-Path $repoRoot "Jamma.sln"
}

$targetPath = $projectMap[$Target]
if (-not (Test-Path $targetPath)) {
    throw "Build target not found: $targetPath"
}

$buildArgs = @(
    $targetPath,
    "/m",
    "/t:$Action",
    "/p:Configuration=$Configuration",
    "/p:Platform=$Platform"
)

if ($Target -eq "Solution") {
    $buildArgs += "/p:VcpkgEnableManifest=true"
}
else {
    $buildArgs += $solutionDirArg
}

Write-Host "MSBuild: $msbuild"
Write-Host "Target: $targetPath"
Write-Host "Action: $Action"
& $msbuild @buildArgs

if (($RunTests -or $Target -eq "JammaLib_Tests") -and $Action -ne "Clean") {
    if ($Target -ne "JammaLib_Tests") {
        $testsBuildArgs = @(
            $projectMap["JammaLib_Tests"],
            "/m",
            "/t:Build",
            "/p:Configuration=$Configuration",
            "/p:Platform=$Platform",
            $solutionDirArg
        )

        Write-Host "Building tests before execution..."
        & $msbuild @testsBuildArgs
    }

    $testsExe = Join-Path $repoRoot "test\JammaLib_Tests\bin\$Platform\$Configuration\JammaLib_Tests.exe"
    if (-not (Test-Path $testsExe)) {
        throw "Test executable not found: $testsExe"
    }

    $testArgs = @()
    if (-not [string]::IsNullOrWhiteSpace($TestFilter)) {
        $testArgs += "--gtest_filter=$TestFilter"
    }

    Write-Host "Tests: $testsExe"
    & $testsExe @testArgs
}
