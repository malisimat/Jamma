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

Before building, read the local `.vscode\tasks.json` and use the applicable task's explicit MSBuild executable and arguments. The file is intentionally machine-specific and authoritative; do not guess a Visual Studio installation or discover a different MSBuild from `PATH`.

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

$solutionDirArg = "/p:SolutionDir=$($repoRoot.TrimEnd('\'))\"

& $msbuild (Join-Path $repoRoot "JammaLib\JammaLib.vcxproj") /m /t:Build /p:Configuration=Debug /p:Platform=x64 $solutionDirArg
& $msbuild (Join-Path $repoRoot "Jamma\Jamma.vcxproj") /m /t:Build /p:Configuration=Debug /p:Platform=x64 $solutionDirArg
& $msbuild (Join-Path $repoRoot "test\JammaLib_Tests\JammaLib_Tests.vcxproj") /m /t:Build /p:Configuration=Debug /p:Platform=x64 $solutionDirArg

# Optional: use the solution only when target selection is unclear.
# & $msbuild (Join-Path $repoRoot "Jamma.sln") /m /t:Build /p:Configuration=Debug /p:Platform=x64 /p:VcpkgEnableManifest=true
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

$solutionDirArg = "/p:SolutionDir=$($repoRoot.TrimEnd('\'))\"

& $msbuild (Join-Path $repoRoot "test\JammaLib_Tests\JammaLib_Tests.vcxproj") /m /t:Build /p:Configuration=Debug /p:Platform=x64 $solutionDirArg
& (Join-Path $repoRoot "test\JammaLib_Tests\bin\x64\Debug\JammaLib_Tests.exe")
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

& (Join-Path $repoRoot "test\JammaLib_Tests\bin\x64\Debug\JammaLib_Tests.exe") --gtest_filter="SuiteName.TestName"
```

## VS Code Tasks

The default F5 configuration, `Launch Jamma (Debug x64)`, runs the incremental
`Build Solution (Debug x64)` task first. This keeps the x64 Debug `Jamma.exe`
current with all source and library changes before debugging.

`.vscode\tasks.json` is ignored by git so each developer can keep local tweaks. To bootstrap a local copy from the tracked starter:

```powershell
New-Item -ItemType Directory -Force .vscode | Out-Null
Copy-Item doc\vscode-tasks.example.json .vscode\tasks.json
```

Starter content lives in [vscode-tasks.example.json](vscode-tasks.example.json). Replace its `C:\path\to\MSBuild.exe` placeholders with the MSBuild executable installed on that machine.

## Troubleshooting

- **Google Test missing headers/libraries**: Verify `vcpkg integrate install`, `vcpkg install`, and that `vcpkg_installed\` contains `gtest`.
- **PowerShell reports duplicate `Path`/`PATH` variables**: This is a Codex Windows tool-shell issue, not an MSBuild project error. The failure occurs before MSBuild starts when PowerShell `Start-Process` copies the inherited environment into a case-insensitive dictionary. The Windows user and machine environments normally contain only one `Path`; do not edit them. A direct foreground `& $msbuild ...` may work, but if the duplicate error occurs, use `System.Diagnostics.Process` and retain the exact executable and arguments from the applicable `.vscode\tasks.json` task:

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
$project = Join-Path $repoRoot "test\JammaLib_Tests\JammaLib_Tests.vcxproj"
$solutionDirArg = "/p:SolutionDir=$($repoRoot.TrimEnd('\'))\"
$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $msbuild
$startInfo.Arguments = '"' + $project + '" /m /t:Build /p:Configuration=Debug /p:Platform=x64 ' + $solutionDirArg
$startInfo.WorkingDirectory = $repoRoot
$startInfo.UseShellExecute = $false
$process = [System.Diagnostics.Process]::Start($startInfo)
$process.WaitForExit()
exit $process.ExitCode
```

  For another task, replace only `$project` and the task-specific arguments. Do not use `Start-Process`, attempt to rename or remove `Path`/`PATH`, or hardcode a different repository path.
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
