# Jamma agent policy

This file is the canonical repo policy for agent tools that support root-level guidance, including Claude Code, Codex CLI, GitHub CLI, and VS Code / GitHub Copilot Chat.

## Project

Jamma is a Windows multichannel loop-sampling app for recording and live performance.

- App shell: Jamma
- Core engine: JammaLib
- Native tests: test/JammaLib_Tests

Treat this as a real-time audio codebase: prefer predictable, low-latency-safe behavior over clever abstractions.

## Architecture

- Keep engine, loop, and audio behavior in JammaLib; keep Jamma as app entry and wiring.
- Core hierarchy: Station -> LoopTake -> Loop.
- Audio flow:
  - ADC capture -> ChannelMixer -> latency-compensated writes into Station / LoopTake / Loop
  - DAC playback <- Station / LoopTake / Loop mix/read <- ChannelMixer
- Keep glue code thin and explicit. Avoid cross-subsystem coupling.
- Treat `doc/glossary.md` as authoritative for core ownership and ubiquitous language; preserve its boundaries and exact term meanings.

## Build

- Use incremental Build by default. Avoid Clean and Rebuild unless necessary.
- Build only the affected project unless target selection is genuinely unclear.
- For direct .vcxproj builds, pass an absolute SolutionDir with exactly one trailing backslash.
- Before every build or native-test run, read the local `.vscode/tasks.json` when it exists. It is the authoritative machine-specific source for the installed build tools, their locations, and explicit build commands. Use the applicable task command (or its tool path and arguments) rather than assuming an MSBuild location, Visual Studio edition, shell environment, or PATH configuration.
- Never launch MSBuild directly from an inherited environment. Run the task's executable and arguments through `.github/skills/builder/invoke-msbuild.ps1` (or the equivalent environment-normalizing repository build wrapper). It creates a child process with exactly one canonical `Path` variable, preventing the Windows `Path`/`PATH` collision before MSBuild starts.
- `.vscode/tasks.json` is intentionally local-only: its commands may vary by machine and may use any valid tool location. Do not edit it unless the user explicitly asks. If it is absent or lacks an applicable command, report that limitation instead of guessing an MSBuild path.
- Detailed setup, build, test, and task guidance live in doc/build.md.
- .vscode/tasks.json is local-only and ignored by git; users can start from doc/vscode-tasks.example.json.

## Tests

- For behavior changes in JammaLib, add or update tests when practical.
- Run the relevant native test target after engine changes.
- Detailed test commands and troubleshooting live in doc/build.md.

## Investigation Guidance

- For architecture and flow questions, prefer semantic/codegraph tools over blunt text search.
- When using agent subtools, cite the specific files and lines involved.
- For text search, use rg (ripgrep), not grep or Select-String in PowerShell.
- For local loop phase, NINJAM timing, tempo join behavior, and remote-follow
  changes, read doc/loop-alignment-and-ninjam-sync.md before editing.

## Coding Guidance

Prefer modern C++ and functional style where practical.

- Keep callback and hot-path code allocation-free, exception-free, and lock-free.
- Prefer value semantics, explicit inputs/outputs, isolated side effects, and RAII-friendly standard library types when performance allows.
- Avoid hidden global mutable state, raw owning pointers, and exception-driven control flow in real-time paths.
- For cross-thread state and container access, prefer the existing published immutable snapshot pattern; do not introduce a different synchronization scheme when that pattern fits.
- Detailed real-time rules and hot-path review guidance live in doc/realtime-audio.md.

## Change Expectations

1. Respect subsystem ownership and existing naming conventions for methods, classes, and variables.
2. Keep diffs focused and minimal.
3. Add or update tests for behavior changes when feasible.
4. Prefer readability and maintainability over clever template or macro tricks.
5. Briefly document non-obvious invariants.
6. Keep hot-path optimizations explicit and maintainable.
7. Never introduce anonymous namespaces for functions or variables; attach to a class and use static methods or constexpr variables instead.
