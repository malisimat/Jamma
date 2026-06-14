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
- Prefer local VS Code tasks first (`.vscode/tasks.json`), then `.github/skills/builder/builder.ps1` for scripted builds.
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
- Keep paths portable: resolve repo root from current location, use repo-relative project paths, and avoid hard-coded machine-specific absolute paths.
- If engine behavior changed, build tests and run `test/JammaLib_Tests/bin/x64/Debug/JammaLib_Tests.exe`.

Refs: `doc/build.md`, `.github/skills/builder/builder.ps1`, `doc/vscode-tasks.example.json`, `Directory.Build.props`
