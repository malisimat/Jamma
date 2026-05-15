## Context

Jamma is an audio application that makes use of plugins (VST's) that both effect audio and show an editor window for the UI. There is an issue with the VST editor window where it is unresponsive and does not paint/resize/close immediately after being displayed.

## Goal

Isolate and fix the VST editor white-window freeze. Run the application (using the harness script), check diagnostics from logs, make changes, build, and iterate. Each iteration should test one hypothesis, make the smallest viable change, run the harness once, and hand off a compact result to the next fresh subagent.

# VST Editor Freeze Agent Handoff Protocol

Use the local branch `bugfix/debug-window-freeze-agentic` as the baseline. Each debugging pass must be run by a brand new subagent with fresh context. The controller agent keeps only the minimal handoff state, chooses the next hypothesis, and launches the next subagent.

## Canonical Tools

- Harness script: `scripts/run-vst-editor-debug.ps1`
- Primary artifacts: `artifacts/vst-debug/last-run-summary.txt`, `artifacts/vst-debug/last-run-stdout.txt`, `artifacts/vst-debug/last-run-stderr.txt`
- Primary diagnostic log: `%APPDATA%\Jamma\vst-diagnostic.log` unless `-LogPath` overrides it
- Main code surface: `Jamma/src/Main.cpp`, `JammaLib/src/engine/Scene.cpp`, `JammaLib/src/engine/Station.cpp`, `JammaLib/src/vst/VstPlugin.cpp`, `JammaLib/src/graphics/VstEditorWindow.cpp`
- Build instructions: `.github\copilot-instructions.md` (pwd is repo root)

## Controller Agent Loop

1. Read the latest handoff block and the latest artifacts.
2. Choose exactly one next hypothesis.
3. Spawn a fresh subagent with only:
	 - the symptom statement
	 - the latest handoff block
	 - artifact paths
	 - the repro inputs to use
	 - the single hypothesis to test
4. Require the subagent to inspect the latest summary and log before editing code.
5. Require the subagent to make minimal changes, build only what it touched, run the harness, and return a new handoff block.
6. Archive the returned handoff block as the new source of truth, then launch the next fresh subagent if the issue is still unresolved.

## Subagent Rules

1. Start by reading `last-run-summary.txt` and the current diagnostic log.
2. Inspect only the code files implicated by the current hypothesis before changing anything.
3. Test one hypothesis per iteration. Do not mix instrumentation, refactors, and speculative fixes in one pass.
4. Keep real-time audio safety intact. Do not add callback logging or blocking behavior.
5. Prefer targeted builds:
	 - Build `JammaLib` first if any JammaLib source changed.
	 - Build `Jamma` after `JammaLib` if app or linked behavior changed.
	 - Build tests only when pure parsing/helper logic changed.
6. Run the harness once after the build succeeds.
7. End by writing a concise handoff block for the next fresh subagent.

## Harness Invocation

Use explicit repro inputs every time. Do not depend on hidden local state.

```powershell
pwsh -File .\scripts\run-vst-editor-debug.ps1 \
	-Configuration Debug \
	-Platform x64 \
	-DefaultsPath "<defaults.json path>" \
	-StationIndex <station index> \
	-PluginIndex <plugin index> \
	-TimeoutSeconds 20
```

Optional:

```powershell
-LogPath "<explicit diagnostic log path>"
-NoAutoOpen
-NoFileLog
```

## Artifact Handling

- Treat `last-run-summary.txt` and the diagnostic log as the source of truth.
- The harness overwrites the latest artifacts on each run. If history matters, archive the previous `last-run-*` files and diagnostic log before launching the next iteration.
- A subagent must quote the decisive log lines in its handoff block rather than forcing the next agent to rediscover them.

## Outcome Classification

Each subagent should classify the run into one of these buckets:

- No diagnostic log created
- Plugin chain never became loaded
- Editor open started but failed early
- Host window created but paint/input stopped
- Plugin child window likely owns the stall
- Candidate fix changed behavior but did not solve it
- Freeze fixed and reproducible across reruns

## Required Handoff Block

Use this exact structure at the end of every iteration.

```markdown
## Iteration N
- Hypothesis: <single thing tested>
- Repro input: <defaults path>, station=<n>, plugin=<n>, timeout=<n>
- Files touched: <file list or none>
- Build/test result: <what was built and whether it passed>
- Harness result: <opened | timed out | failed early>
- Key evidence:
	- <most important log line>
	- <second most important log line>
- Conclusion: <what this iteration proved or ruled out>
- Next best task: <single next task for the next fresh subagent>
- Artifact paths:
	- <summary path>
	- <log path>
```

## Prompt Template For Each Fresh Subagent

```text
You are a fresh debugging subagent working on the VST editor white-window freeze.

Overall plan: doc/debug-window-freeze-plan.md

Read these first:
1. artifacts/vst-debug/last-run-summary.txt
2. The latest diagnostic log at the provided log path
3. doc/debug-window-freeze-plan.md
3. Only the files implicated by the current hypothesis

Current symptom:
<symptom statement>

Current hypothesis:
<single hypothesis>

Repro inputs:
- DefaultsPath: <path>
- StationIndex: <n>
- PluginIndex: <n>
- TimeoutSeconds: <n>

Rules:
- Test exactly one hypothesis.
- Make the smallest viable code change.
- Preserve audio-thread safety.
- Build only the necessary targets.
- Run scripts/run-vst-editor-debug.ps1 once after a successful build.
- End with a markdown handoff block using the required template.

Previous handoff block:
<paste latest block here>
```

## Decision Heuristics For The Controller

- If no log is created, focus on startup config, env propagation, and harness execution before touching VST code.
- If the log stops before `load-vst-success`, focus on load/commit timing.
- If the log reaches `open-editor-begin` but not `open-editor-success`, focus on `VstPlugin::OpenEditor()` and attach preconditions.
- If the log shows `editor-ready` and the host message loop continues but the window is still white, pivot to child HWND ownership, thread affinity, and paint/input routing.
- If a pass only adds more logging, the next pass should use that evidence to test a concrete behavioral fix rather than adding more generic diagnostics.

## Stop Conditions

Stop the iteration loop when one of these is true:

- The freeze is fixed and the result is repeatable.
- The stall point is isolated tightly enough that the remaining work is a clearly bounded code change.
- The controller lacks a stable repro input and must first standardize one.