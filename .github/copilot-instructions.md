# Jamma Copilot Instructions

## Project Purpose

Jamma is a Windows multichannel loop-sampling app for recording and live performance.

- Main app: Jamma
- Core engine: JammaLib
- Native tests: test/JammaLib.Tests

Treat this as a real-time audio codebase: prioritize predictable, low-latency-safe behavior over clever abstractions.

## Architecture

1. Jamma hosts app entry/wiring.
2. JammaLib owns engine, loop, and audio behavior.
3. Core loop hierarchy: Station -> LoopTake -> Loop.
4. Audio flow:
	- ADC capture -> ChannelMixer buffer -> monitor + latency-compensated record/bounce writes into Station/LoopTake/Loop.
	- DAC playback -> Station/LoopTake/Loop mix/read from Loop/AudioBuffer -> ChannelMixer sink -> output.

Guidelines:

- Keep engine/loop logic in JammaLib unless it is app-shell specific.
- Keep glue code thin and explicit.
- Avoid cross-subsystem coupling.

## Project Layout

- Jamma.sln
- Jamma/src/Main.cpp
- JammaLib/src
- test/JammaLib.Tests/src

## Build Instructions

Prereqs:

- Windows
- Visual Studio 2022 (C++ desktop workload)
- Windows SDK 10.0

Config:

- Toolset: v145
- Language standard: stdcpplatest
- Platform: x64

Build rules:

1. Restore dependencies when setup changes (`msbuild /t:Restore` or VS restore).
2. Use incremental `Build` by default; avoid `Clean`/`Rebuild` unless required.
3. Build only affected projects:
	- Jamma/src only -> Jamma/Jamma.vcxproj
	- JammaLib/src or JammaLib/include -> JammaLib/JammaLib.vcxproj, then dependents as needed
	- test/JammaLib.Tests/src only -> test/JammaLib.Tests/JammaLib.Tests.vcxproj
4. Use solution build only when project targeting is unclear.

MSBuild path:

C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe

Preferred targeted builds:

```powershell
$msbuild = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
& $msbuild JammaLib\JammaLib.vcxproj /m /t:Build /p:Configuration=Debug /p:Platform=x64
& $msbuild Jamma\Jamma.vcxproj /m /t:Build /p:Configuration=Debug /p:Platform=x64
& $msbuild test\JammaLib.Tests\JammaLib.Tests.vcxproj /m /t:Build /p:Configuration=Debug /p:Platform=x64
```

Solution build (sparingly):

```powershell
$msbuild = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
& $msbuild Jamma.sln /m /t:Build /p:Configuration=Debug /p:Platform=x64
& $msbuild Jamma.sln /m /t:Build /p:Configuration=Release /p:Platform=x64
```

Testing:

- Build/run JammaLib.Tests.
- For behavior changes in JammaLib, add/update tests when feasible.

## Coding Guidance

Prefer modern C++ and functional style where practical.

### Real-Time Audio Safety

Audio performance is KING.  Nothing can get in the way of the audio callback filling the buffer on time.
In callback/hot paths:

- No heap allocation.
- No exceptions.
- No blocking I/O or unpredictable locks.
- No heavy logging/system calls.
- Prefer fastest safe approach for read/write audio paths.
- Pre-allocate and reuse buffers/resources.

### Core principles:

- Prefer value semantics and immutability for state-transform code.
- Favor pure transformation functions.
- Isolate side effects (I/O, audio device, rendering, filesystem, threading).
- Keep functions small/composable with explicit inputs/outputs.

### Preferred patterns:

- Use std::vector/std::array/std::string, RAII, smart pointers when performance allows.
- Use std::optional/std::variant/strong enums over sentinel values.
- Prefer algorithms/ranges where they improve clarity without hurting performance.
- Use const aggressively; pass large objects by const&.
- Use constexpr/noexcept when correct.

### Avoid:

- Raw owning pointers/manual new/delete unless measurably better in hot paths.
- Hidden global mutable state.
- Monolithic mixed-side-effect functions.
- Exception-driven control flow in real-time paths.

### Change Expectations

1. Respect naming, boundaries, and subsystem ownership.
2. Keep diffs focused/minimal.
3. Add/update tests for behavior changes when practical.
4. Prefer readability/maintainability over template/macro cleverness.
5. Document non-obvious invariants briefly.
6. Keep hot-path optimizations explicit and maintainable.
