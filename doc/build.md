# Build and Test Guide

## Environment

- Windows
- Visual Studio 2022 with the C++ desktop workload
- Windows SDK 10.0
- Toolset `v145`, standard `stdcpplatest`, platform `x64`

## Fresh Setup

Before building, install dependencies with vcpkg from the repository root:

```powershell
vcpkg integrate install
vcpkg install
```

This project uses `vcpkg.json` manifest mode to install dependencies (including Google Test).

Windows builds also compile VST3 hosting support by default via the `vst3sdk` vcpkg dependency declared in `vcpkg.json`.

## Build Rules

1. Use incremental `Build` by default. Avoid `Clean` and `Rebuild` unless necessary.
2. Build only affected projects:
   - `Jamma\src` changes -> `Jamma\Jamma.vcxproj`
   - `JammaLib\src` or `JammaLib\include` changes -> `JammaLib\JammaLib.vcxproj`, then dependents as needed
   - `test\JammaLib_Tests\src` changes -> `test\JammaLib_Tests\JammaLib_Tests.vcxproj`
3. Use solution builds only when project targeting is unclear.
4. For direct `.vcxproj` builds, pass absolute paths and `/p:SolutionDir=<repo-root>\` with exactly one trailing backslash.
5. If you hit `C1041` PDB contention, apply `/FS` and a project-specific `ProgramDataBaseFileName` in the affected project.

`Directory.Build.props` backfills `SolutionDir` and the vcpkg manifest properties when they are unset, but direct project builds should still pass `SolutionDir` explicitly so `.tlog` state stays stable.

## Preferred PowerShell Build Snippet

```powershell
$msbuild = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"

$repoRoot = (Get-Location).Path
while (-not (Test-Path (Join-Path $repoRoot "Jamma.sln"))) {
    $parent = Split-Path $repoRoot -Parent
    if ($parent -eq $repoRoot) {
        throw "Could not find Jamma.sln. Start in this repository or set `$repoRoot explicitly."
    }
    $repoRoot = $parent
}

$sln = Join-Path $repoRoot "Jamma.sln"
$jammaLibProj = Join-Path $repoRoot "JammaLib\JammaLib.vcxproj"
$jammaProj = Join-Path $repoRoot "Jamma\Jamma.vcxproj"
$testsProj = Join-Path $repoRoot "test\JammaLib_Tests\JammaLib_Tests.vcxproj"
$solutionDirArg = "/p:SolutionDir=$($repoRoot.TrimEnd('\'))\"

& $msbuild $jammaLibProj /m /t:Build /p:Configuration=Debug /p:Platform=x64 $solutionDirArg
& $msbuild $jammaProj /m /t:Build /p:Configuration=Debug /p:Platform=x64 $solutionDirArg
& $msbuild $testsProj /m /t:Build /p:Configuration=Debug /p:Platform=x64 $solutionDirArg

# Optional: use the solution only when target selection is unclear.
# & $msbuild $sln /m /t:Build /p:Configuration=Debug /p:Platform=x64 /p:VcpkgEnableManifest=true
```

## Running Tests

Build and run the native tests:

```powershell
$msbuild = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"

$repoRoot = (Get-Location).Path
while (-not (Test-Path (Join-Path $repoRoot "Jamma.sln"))) {
    $parent = Split-Path $repoRoot -Parent
    if ($parent -eq $repoRoot) {
        throw "Could not find Jamma.sln. Start in this repository or set `$repoRoot explicitly."
    }
    $repoRoot = $parent
}

$testsProj = Join-Path $repoRoot "test\JammaLib_Tests\JammaLib_Tests.vcxproj"
$testsExe = Join-Path $repoRoot "test\JammaLib_Tests\bin\x64\Debug\JammaLib_Tests.exe"
$solutionDirArg = "/p:SolutionDir=$($repoRoot.TrimEnd('\'))\"

& $msbuild $testsProj /m /t:Build /p:Configuration=Debug /p:Platform=x64 $solutionDirArg
& $testsExe
```

Run a specific test:

```powershell
$repoRoot = (Get-Location).Path
while (-not (Test-Path (Join-Path $repoRoot "Jamma.sln"))) {
    $parent = Split-Path $repoRoot -Parent
    if ($parent -eq $repoRoot) {
        throw "Could not find Jamma.sln. Start in this repository or set `$repoRoot explicitly."
    }
    $repoRoot = $parent
}

$testsExe = Join-Path $repoRoot "test\JammaLib_Tests\bin\x64\Debug\JammaLib_Tests.exe"
& $testsExe --gtest_filter="SuiteName.TestName"
```

## VS Code Tasks

`.vscode\tasks.json` is ignored by git so each developer can keep local tweaks. To bootstrap a local copy from the tracked starter:

```powershell
New-Item -ItemType Directory -Force .vscode | Out-Null
Copy-Item doc\vscode-tasks.example.json .vscode\tasks.json
```

Starter content lives in [vscode-tasks.example.json](vscode-tasks.example.json). It assumes the workspace root is the repository root and uses PowerShell plus `vswhere.exe` to locate `MSBuild.exe`.

## Troubleshooting

- **Google Test missing headers/libraries**: Verify `vcpkg integrate install`, `vcpkg install`, and that `vcpkg_installed\` contains `gtest`.
- **Silent test failures / crash on startup**: If the test exe exits with code `1` and no output, stale Release gtest DLLs may be sitting in the Debug output folder. Rebuild both Debug and Release to refresh the copied runtime files. You can also manually copy the debug DLLs from `vcpkg_installed`:

```powershell
$src = ".\vcpkg_installed\x64-windows\debug\bin"
$dst = ".\test\JammaLib_Tests\bin\x64\Debug"
Copy-Item "$src\gtest.dll"      "$dst\gtest.dll"      -Force
Copy-Item "$src\gtest_main.dll" "$dst\gtest_main.dll" -Force
```

After copying, `gtest.dll` should be ~1.8 MB (the Release version is ~448 KB).

Rebuilding both Debug/Release configurations via solution build will also refresh them:

```powershell
$msbuild = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"

$repoRoot = (Get-Location).Path
while (-not (Test-Path (Join-Path $repoRoot "Jamma.sln"))) {
    $parent = Split-Path $repoRoot -Parent
    if ($parent -eq $repoRoot) {
        throw "Could not find Jamma.sln. Start in this repository or set `$repoRoot explicitly."
    }
    $repoRoot = $parent
}

$sln = Join-Path $repoRoot "Jamma.sln"
& $msbuild $sln /m /t:Build /p:Configuration=Debug /p:Platform=x64 /p:VcpkgEnableManifest=true
& $msbuild $sln /m /t:Build /p:Configuration=Release /p:Platform=x64 /p:VcpkgEnableManifest=true
```
