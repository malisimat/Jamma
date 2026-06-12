# Jamma Copilot Instructions

## Project

Jamma is a Windows multichannel loop-sampling app for recording and live performance.

- App shell: `Jamma`
- Core engine: `JammaLib`
- Native tests: `test/JammaLib_Tests`

Treat this as a real-time audio codebase: prefer predictable, low-latency-safe behavior over clever abstractions.

## Architecture

- Keep engine, loop, and audio behavior in `JammaLib`; keep `Jamma` as app entry and wiring.
- Core hierarchy: `Station -> LoopTake -> Loop`.
- Audio flow:
	- ADC capture -> `ChannelMixer` -> latency-compensated writes into `Station` / `LoopTake` / `Loop`
	- DAC playback <- `Station` / `LoopTake` / `Loop` mix/read <- `ChannelMixer`
- Keep glue code thin and explicit. Avoid cross-subsystem coupling.

## Build

Environment:

- Windows
- Visual Studio 2022 with C++ desktop workload
- Windows SDK 10.0
- Toolset `v145`, standard `stdcpplatest`, platform `x64`

Fresh setup:

Assume vcpkg is cloned externally, that VCPKG environment variable is set to vcpkg repo root, and that vcpkg is bootstrapped. Then install dependencies:

```powershell
vcpkg install
```

Rules:

1. Use incremental `Build` by default. Avoid `Clean` and `Rebuild` unless necessary.
2. Build only affected projects:
	- `Jamma/src` changes -> `Jamma/Jamma.vcxproj`
	- `JammaLib/src` or `JammaLib/include` changes -> `JammaLib/JammaLib.vcxproj`, then dependents as needed
	- `test/JammaLib_Tests/src` changes -> `test/JammaLib_Tests/JammaLib_Tests.vcxproj`
3. Use solution builds only when project targeting is unclear.
4. If you hit `C1041` PDB contention, apply `/FS` and a project-specific `ProgramDataBaseFileName` in the affected project.

MSBuild:

```powershell
$msbuild = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
```

Preferred targeted builds:

```powershell
$msbuild = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"

# Resolve repo root by walking upward until Jamma.sln is found.
$repoRoot = (Get-Location).Path
while (-not (Test-Path (Join-Path $repoRoot "Jamma.sln"))) {
	$parent = Split-Path $repoRoot -Parent
	if ($parent -eq $repoRoot) {
		throw "Could not find Jamma.sln. Start in this repository or set `$repoRoot explicitly."
	}
	$repoRoot = $parent
}

# Always pass absolute paths. For direct .vcxproj builds, pass SolutionDir explicitly.
$sln = Join-Path $repoRoot "Jamma.sln"
$jammaLibProj = Join-Path $repoRoot "JammaLib\JammaLib.vcxproj"
$jammaProj = Join-Path $repoRoot "Jamma\Jamma.vcxproj"
$testsProj = Join-Path $repoRoot "test\JammaLib_Tests\JammaLib_Tests.vcxproj"
$solutionDirArg = "/p:SolutionDir=$($repoRoot.TrimEnd('\\'))\"

& $msbuild $jammaLibProj /m /t:Build /p:Configuration=Debug /p:Platform=x64 $solutionDirArg
& $msbuild $jammaProj /m /t:Build /p:Configuration=Debug /p:Platform=x64 $solutionDirArg
& $msbuild $testsProj /m /t:Build /p:Configuration=Debug /p:Platform=x64 $solutionDirArg

# Optional: solution build if project targeting is unclear.
# & $msbuild $sln /m /t:Build /p:Configuration=Debug /p:Platform=x64 /p:VcpkgEnableManifest=true
```

**Important:** For direct `.vcxproj` builds, derive repo root from `Jamma.sln` and pass `/p:SolutionDir=<repo-root>\` with exactly one trailing backslash. Do not use `$(pwd)` or a doubled trailing slash; mismatched `SolutionDir` text can invalidate `.tlog` state and trigger full test recompiles.

### Running tests:

Use solution builds sparingly:

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
$solutionDirArg = "/p:SolutionDir=$($repoRoot.TrimEnd('\\'))\"

& $msbuild $testsProj /m /t:Build /p:Configuration=Debug /p:Platform=x64 $solutionDirArg
& $testsExe
```

## Tests

- For behavior changes in `JammaLib`, add or update tests when practical.
- Build and run tests with:

```powershell
& $msbuild test\JammaLib_Tests\JammaLib_Tests.vcxproj /m /t:Build /p:Configuration=Debug /p:Platform=x64
& .\test\JammaLib_Tests\bin\x64\Debug\JammaLib_Tests.exe
```

- Run a specific test with:

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

Troubleshooting:

- If Google Test headers or libraries are missing, verify `vcpkg integrate install`, `vcpkg install`, and that `vcpkg_installed/` contains `gtest`.
- If the test exe exits with code `1` and no output, stale Release gtest DLLs may be in the Debug output folder. Copy the Debug DLLs back in:

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

## Coding Guidance

Prefer modern C++ and functional style where practical.

Real-time audio rules for callback and hot paths:

- No heap allocation.
- No exceptions.
- No blocking I/O or unpredictable locks.
- No heavy logging or system calls.
- Prefer the fastest safe read/write path.
- Pre-allocate and reuse buffers and resources.

Hotpath review rule after edits in audio-thread code:

- Manually inspect these callback-owned functions before finishing a change: `Scene::OnTick`, `Scene::AudioCallback`, `Scene::_OnAudio`, `Loop::WriteBlock`, `LoopTake::Zero`, `LoopTake::WriteBlock`, `LoopTake::EndMultiPlay`, `LoopTake::EndMultiWrite`, `LoopTake::_InputChannel`, `Station::Zero`, `Station::WriteBlock`, `Station::EndMultiPlay`, `Station::OnBlockWriteChannel`, `Station::EndMultiWrite`, `Station::OnBounce`, `Station::_InputChannel`, `Trigger::OnTick`, `NinjamConnection::ProcessAudioBlock`, and `NinjamConnection::ConsumeStereoPair`.
- Reject any addition of blocking or lock-based primitives inside those bodies, including `std::mutex`, `std::scoped_lock`, `std::lock_guard`, `std::unique_lock`, `std::condition_variable`, `EnterCriticalSection`, `WaitForSingleObject`, `SleepConditionVariableCS`, and `SleepConditionVariableSRW`.

General guidance:

- Prefer value semantics, pure transformations, and explicit inputs/outputs.
- Isolate side effects such as I/O, audio device access, rendering, filesystem work, and threading.
- Use `std::vector`, `std::array`, `std::string`, RAII, and smart pointers when performance allows.
- Prefer `std::optional`, `std::variant`, and strong enums over sentinel values.
- Use algorithms and ranges only when they improve clarity without hurting hot paths.
- Use `const` aggressively; pass large objects by `const&`.
- Use `constexpr` and `noexcept` when correct.

Avoid:

- Raw owning pointers or manual `new` and `delete` unless measurably better in hot paths.
- Hidden global mutable state.
- Monolithic functions mixing state changes, I/O, and control flow.
- Exception-driven control flow in real-time paths.

## Change Expectations

1. Respect subsystem ownership and existing naming.
2. Keep diffs focused and minimal.
3. Add or update tests for behavior changes when feasible.
4. Prefer readability and maintainability over template or macro cleverness.
5. Briefly document non-obvious invariants.
6. Keep hot-path optimizations explicit and maintainable.
