---
name: builder
description: Use for general Jamma build orchestration, solution/project targets, Build/Rebuild/Clean, and optional native test runs.
allowed-tools:
  - powershell
  - view
  - rg
---

# Jamma build guardrails

Start with `doc/build.md`.

- Trigger this skill for: build/rebuild/clean requests, target selection, or "build + run native tests" flows.
- Read local `.vscode/tasks.json` first when present. Its executable and arguments are authoritative for that machine; pass its MSBuild executable to `.github/skills/builder/builder.ps1` with `-MSBuildPath`.
- Build the smallest valid target by default:
  - `JammaLib/src` or `JammaLib/include` -> `JammaLib`
  - `Jamma/src` -> `Jamma`
  - `test/JammaLib_Tests/src` -> `JammaLib_Tests`
  - Use `Jamma.sln` only when full-solution coverage is requested or target mapping is unclear.
- Default to incremental `/t:Build`; use `/t:Rebuild` or `/t:Clean` only on explicit request or stale-artifact failures.
- Script interface:
  - Targets: `JammaLib`, `Jamma`, `JammaLib_Tests`, `Solution`
  - Actions: `Build`, `Rebuild`, `Clean`
  - Test options: `-RunTests`, optional `-TestFilter`
  - Optional executable override: `-MSBuildPath` (use the local task's path when available)
- Keep project paths portable: resolve repo root from current location and use repo-relative project paths. Do not hard-code machine-specific executable paths in tracked scripts; local tasks provide them.
- If engine behavior changed, build tests and run `test/JammaLib_Tests/bin/x64/Debug/JammaLib_Tests.exe`.

Refs: `doc/build.md`, `.github/skills/builder/builder.ps1`, `doc/vscode-tasks.example.json`, `Directory.Build.props`
