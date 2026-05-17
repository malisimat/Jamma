# Jamma Copilot Instructions

## Project

Jamma is a Windows multichannel loop-sampling app for recording and live performance.

- App shell: `Jamma`
- Core engine: `JammaLib`
- Native tests: `test/JammaLib.Tests`

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

```powershell
vcpkg integrate install
vcpkg install
```

Rules:

1. Use incremental `Build` by default. Avoid `Clean` and `Rebuild` unless necessary.
2. Build only affected projects:
	- `Jamma/src` changes -> `Jamma/Jamma.vcxproj`
	- `JammaLib/src` or `JammaLib/include` changes -> `JammaLib/JammaLib.vcxproj`, then dependents as needed
	- `test/JammaLib.Tests/src` changes -> `test/JammaLib.Tests/JammaLib.Tests.vcxproj`
3. Use solution builds only when project targeting is unclear.
4. If you hit `C1041` PDB contention, apply `/FS` and a project-specific `ProgramDataBaseFileName` in the affected project.

MSBuild:

```powershell
$msbuild = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
```

Preferred targeted builds:

```powershell
& $msbuild JammaLib\JammaLib.vcxproj /m /t:Build /p:Configuration=Debug /p:Platform=x64
& $msbuild Jamma\Jamma.vcxproj /m /t:Build /p:Configuration=Debug /p:Platform=x64
& $msbuild test\JammaLib.Tests\JammaLib.Tests.vcxproj /m /t:Build /p:Configuration=Debug /p:Platform=x64
```

`Directory.Build.props` defaults `$(SolutionDir)` to the repo root when unset, so direct `*.vcxproj` builds should not need a manual `/p:SolutionDir=...` override.

Use solution builds sparingly:

```powershell
& $msbuild Jamma.sln /m /t:Build /p:Configuration=Debug /p:Platform=x64 /p:VcpkgEnableManifest=true
& $msbuild Jamma.sln /m /t:Build /p:Configuration=Release /p:Platform=x64 /p:VcpkgEnableManifest=true
```

## Tests

- For behavior changes in `JammaLib`, add or update tests when practical.
- Build and run tests with:

```powershell
& $msbuild test\JammaLib.Tests\JammaLib.Tests.vcxproj /m /t:Build /p:Configuration=Debug /p:Platform=x64
& .\test\JammaLib.Tests\bin\x64\Debug\JammaLib.Tests.exe
```

- Run a specific test with:

```powershell
& .\test\JammaLib.Tests\bin\x64\Debug\JammaLib.Tests.exe --gtest_filter="SuiteName.TestName"
```

Troubleshooting:

- If Google Test headers or libraries are missing, verify `vcpkg integrate install`, `vcpkg install`, and that `vcpkg_installed/` contains `gtest`.
- If the test exe exits with code `1` and no output, stale Release gtest DLLs may be in the Debug output folder. Copy the Debug DLLs back in:

```powershell
$src = ".\vcpkg_installed\x64-windows\debug\bin"
$dst = ".\test\JammaLib.Tests\bin\x64\Debug"
Copy-Item "$src\gtest.dll" "$dst\gtest.dll" -Force
Copy-Item "$src\gtest_main.dll" "$dst\gtest_main.dll" -Force
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
