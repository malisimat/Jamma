# Jamma Copilot Instructions

## Project Purpose

Jamma is a Windows multichannel loop-sampling application for recording and live performance.

- Main app project: Jamma
- Core engine project: JammaLib
- Native test project: test/JammaLib.Tests

Treat this as a performance-sensitive, real-time audio codebase. Prefer predictable behavior and low-latency-safe choices over clever abstractions.

## Architecture

High-level structure:

1. Jamma hosts app entry and top-level wiring.
2. JammaLib contains engine, loop, and audio behavior.
3. Core loop hierarchy is Station -> LoopTake -> Loop.
4. Audio flow differs by direction: ADC capture -> ChannelMixer buffer -> monitor pass + latency-compensated record/bounce writes into Station/LoopTake/Loop; DAC playback -> Station/LoopTake/Loop mix/read from Loop/AudioBuffer -> ChannelMixer sink -> output.

When making changes:

- Keep engine and loop logic in JammaLib unless it is truly app-bootstrap/UI-shell specific.
- Keep platform/runtime glue code thin and explicit.
- Avoid introducing hard dependencies between unrelated subsystems.

## Project Layout

- Jamma.sln: solution entry point
- Jamma/src/Main.cpp: app entry point
- JammaLib/src: core implementation
- test/JammaLib.Tests/src: GoogleTest native tests

## Build Instructions

Prerequisites:

- Windows
- Visual Studio 2022 with C++ desktop workload
- Windows SDK 10.0

Projects are configured with:

- Platform toolset: v145
- Language standard: stdcpplatest
- Target platform: x64

Build approach (project and dependencies):

1. Ensure dependencies are present before building:
	- Run restore once per machine/setup change (Visual Studio restore or `msbuild /t:Restore`).
2. Default to incremental builds:
	- Use `Build` targets only. Do not use `Clean`/`Rebuild` unless explicitly needed.
	- Build only the project(s) affected by your changes.
3. Preferred project build order when building individually:
	- JammaLib
	- Jamma
	- JammaLib.Tests
4. If building via solution, project dependencies are handled automatically (Jamma depends on JammaLib), but this is slower than targeted project builds.

Incremental build selection rules:

- If only `Jamma/src` changed: build `Jamma/Jamma.vcxproj`.
- If `JammaLib/src` or `JammaLib/include` changed: build `JammaLib/JammaLib.vcxproj`, then build dependents (`Jamma/Jamma.vcxproj` and/or `test/JammaLib.Tests/JammaLib.Tests.vcxproj`) as needed.
- If only tests changed under `test/JammaLib.Tests/src`: build `test/JammaLib.Tests/JammaLib.Tests.vcxproj`.
- Only build `Jamma.sln` when changes span multiple projects and a targeted choice is unclear.

Build in Visual Studio:

1. Open Jamma.sln in Visual Studio 2022.
2. Select Debug or Release and x64.
3. Build solution.
4. Run startup project Jamma.

Build from PowerShell or Developer Command Prompt (from repo root):

Use this MSBuild executable path:
C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe

Avoid building JammaLib unless changes have been made to files in that project. Prefer building individual projects when working on specific areas to reduce build time.

Incremental targeted builds (preferred):

```powershell
$msbuild = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"

# Build only what changed. These are Build (incremental) invocations.
& $msbuild JammaLib\JammaLib.vcxproj /m /t:Build /p:Configuration=Debug /p:Platform=x64
& $msbuild Jamma\Jamma.vcxproj /m /t:Build /p:Configuration=Debug /p:Platform=x64
& $msbuild test\JammaLib.Tests\JammaLib.Tests.vcxproj /m /t:Build /p:Configuration=Debug /p:Platform=x64
```

Solution build (use sparingly):

```powershell
$msbuild = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"

& $msbuild Jamma.sln /m /t:Build /p:Configuration=Debug /p:Platform=x64
& $msbuild Jamma.sln /m /t:Build /p:Configuration=Release /p:Platform=x64
```

Run tests:

Build and run JammaLib.Tests (Test Explorer or the produced test executable).

When changing behavior in JammaLib, add or update tests in test/JammaLib.Tests/src when feasible.

## Coding Guidance (Modern C++ + Functional Style)

Default to modern C++ practices and a functional style where practical.

## Core Principles

- Prefer value semantics and immutability for domain/state-transform code.
- Favor pure functions for transformations and calculations.
- Isolate side effects (I/O, audio device interaction, rendering calls, filesystem, threading).
- Keep functions small, composable, and explicit about inputs/outputs.
- Model invalid states out of existence when possible.

## Preferred Modern C++ Patterns

- Use std::vector, std::array, std::string, smart pointers, and RAII, provided this does not significantly impact performance.
- Use std::optional, std::variant, and strongly typed enums instead of sentinel values.
- Prefer algorithms/ranges style over manual index-heavy loops when readability improves, provided this does not significantly impact performance.
- Use const aggressively and pass lightweight values by value, larger objects by const&.
- Use constexpr and noexcept when meaningfully correct.

## Avoid

- Raw owning pointers and manual `new`/`delete`,
unless significantly more performant.
- Hidden global mutable state.
- Large monolithic functions with mixed side effects and transformation logic.
- Exception-driven control flow in hot/real-time paths.

## Real-Time Audio Safety

In audio callback/real-time paths:

- Do not allocate memory (use of heap forbidden).
- Do not throw exceptions.
- Do not perform blocking I/O or locks that can block unpredictably.
- Do not log excessively or call heavy system APIs.
- In general, select the most blindingly fast approach when dealing with reading/writing audio (e.g. `pointers` vs `smart_pointers`, `pre-allocated arrays` vs `vectors`), since this is the most critical path for performance and stability.
- Pre-allocate and reuse buffers/resources.

## Change Expectations for Copilot

When generating or modifying code:

1. Respect existing naming, folder boundaries, and subsystem ownership.
2. Keep diffs focused and minimal.
3. Add/update tests for behavior changes when practical.
4. Prefer readability and maintainability over macro-heavy or template-heavy cleverness.
5. Document non-obvious invariants with concise comments.
6. When optimizing, prefer micro-optimizations in hot paths while keeping the overall structure clear and maintainable.
