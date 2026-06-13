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

- Use incremental `Build` by default. Avoid `Clean` and `Rebuild` unless necessary.
- Build only the affected project unless target selection is genuinely unclear.
- For direct `.vcxproj` builds, pass an absolute `SolutionDir` with exactly one trailing backslash.
- Detailed setup, build/test commands, troubleshooting, and VS Code task guidance live in [../doc/build.md](../doc/build.md).
- `.vscode/tasks.json` is local-only and ignored by git; users can start from [../doc/vscode-tasks.example.json](../doc/vscode-tasks.example.json).

## Tests

- For behavior changes in `JammaLib`, add or update tests when practical.
- Run the relevant native test target after engine changes.
- Detailed test commands, filtered test runs, and troubleshooting live in [../doc/build.md](../doc/build.md).

## Coding Guidance

Prefer modern C++ and functional style where practical.

- Keep callback and hot-path code allocation-free, exception-free, and lock-free.
- Prefer value semantics, explicit inputs/outputs, isolated side effects, and RAII-friendly standard library types when performance allows.
- Avoid hidden global mutable state, raw owning pointers, and exception-driven control flow in real-time paths.
- Detailed real-time rules, hot-path review checklist, and banned blocking primitives live in [../doc/realtime-audio.md](../doc/realtime-audio.md).
- grep is not available (powershell-only) but ripgrep `rg` is.

## Change Expectations

1. Respect subsystem ownership and existing naming conventions for methods, classes and variables.
2. Keep diffs focused and minimal/surgical.
3. Add or update tests for behavior changes when feasible.
4. Prefer readability and maintainability over template or macro cleverness.
5. Briefly document non-obvious invariants.
6. Keep hot-path optimizations explicit and maintainable.
7. Never introduce anonymous namespaces for functions or variables, always attach to a class and use static methods or constexpr variables instead.

