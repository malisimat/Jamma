# Native command window handoff — 16 October 2026

Implementation branch: `codex/native-command-window`, based on
`codex/integrate-ready-features` at `5ee471a2`. Reviewed implementation:
`fcf15fbb9003432439c4117be2f33ffe7654f3fd`. The subsequent handoff commit changes
documentation only. No merge or push has been performed.

## Result and review

The [reviewed plan](native-console-window-plan.md) is implemented. The separate
console process, host detection, IPC protocol, FTXUI renderer, bespoke terminal
text models and unused ConsoleTui are removed, along with their project entries,
tests and FTXUI/utf8proc dependencies. Existing parser/NINJAM commands,
`consoleautostart`, capture opt-in, LineRing and the main-window console action
remain. The implementation checkpoint removes a net 3290 lines across 43 files.

One subagent reviewed the original plan; a fresh subagent implemented it. Root
review and a separate implementation reviewer checked cleanup, native window
lifetime, input isolation, output bounds and command ownership. Review repairs
preserve results during capture overflow, retry creation only on explicit user
actions, truncate long UTF-8 results safely, and pair performance-key releases
across console/main focus changes, including changes inside one message batch.

The native transcript supports colored output, bounded scrolling history,
selection and word selection through Windows controls, and copying. The input
supports native editing and bounded single-line paste; the status row shows
connection state and the latest event/result. Closing hides the window and
retains its history/draft. See [operation and shortcuts](performance-console.md).

## Ownership and limits

| State | Owner and lifetime |
| --- | --- |
| Window, child controls, font, Rich Edit DLL | UI thread; partial failure unwinds; explicit destroy before Scene/COM teardown; destructor is idempotent. |
| Command queue | UI thread only, window procedures enqueue and main loop executes against current Scene; 64 commands, 4096 UTF-8 bytes each; close rejects/discards pending commands. |
| Pending transcript output | UI thread only; at most 516 entries, including two bounded capture drains, notices, echo and guaranteed result; at most one command admitted when this queue is empty. |
| Rich Edit transcript | UI thread only; at most 2048 logical entries and 256 Ki UTF-16 units; complete oldest entries are evicted; at most 32 entries appended per iteration. |
| Tracked key-origin/release state | UI thread only; native message observation and at most three sampled global transitions per iteration; no engine API or hook changes. |
| Existing captured output | Existing opted-in non-audio producers publish to unchanged bounded LineRing; UI consumes it; producer joins still precede capture restoration. |

No new cross-thread state, worker, callback operation, mutex or wait is introduced.
No files under `JammaLib/src/engine` or `JammaLib/src/audio` change. The only
remaining library changes remove unused ConsoleTui, retire the obsolete host
preference, and correct a capture comment. Controls are created hidden after
the first main-window Render/Swap; automatic display uses HWND_BOTTOM and
SWP_NOACTIVATE once. Explicit activation uses normal native window behavior.

## Automated validation

- Baseline Debug: **1722/1722 passed** before implementation.
- Final Debug: **1685/1685 passed**, including all ten real native-control tests
  and the final held-key focus-round-trip regression (65.7 seconds).
- Final Release: **1679/1679 passed** (16.4 seconds). Both configurations had
  zero failures or skips; six callback probes are intentionally Debug-only.
- Incremental x64 Debug and Release engine/tests/app builds passed.
- Threading audit, cumulative callback-file review, MSBuild XML parsing and
  whitespace checks passed. Debug and Release executable dependency inspection
  found no terminal-specific runtime dependency.

The Release executable is `Jamma/bin/x64/Release/Jamma.exe` with SHA256
`ADA328027AC09E11D66D7732730C9AF399F2061376098B629FDB840C38BFECDA`.

The smaller suite replaces obsolete pipe, launch, terminal grapheme/selection
and renderer tests with native-window and key-routing coverage. Existing
capture/ring/concurrency, parser, config and main-window toggle tests remain.
The native tests exercise actual hidden Rich Edit/EDIT controls: UTF-16
paragraph accounting, Unicode selections, wrapped viewport anchors after trim,
large scroll positions, retention limits, read-only insertion, tail following,
draft retention, validated paste replacement, colors/status, resize/DPI,
passive display and close/hide/destruction. Clipboard tests use the same paste
application path without replacing the user's system clipboard.

Local logs, XML, dependency dumps and the task-reading validation runner are in
ignored `bin/native-console-validation/`. Builds use the installed MSBuild from
local `.vscode/tasks.json`, incremental Build and an absolute SolutionDir with
one trailing backslash. Reproduce using that local configuration and the
commands in [build.md](build.md); focused native tests use
`--gtest_filter="NativeConsoleWindow.*:ConsoleSession.*:ConsoleCommand.*:InitFile.*:Window.ConsoleToggleShortcutIsConsumedOnce"`.

## Remaining manual acceptance

These interactions have not been certified by this run. Automated control
tests and source ordering do not establish every desktop/hardware interaction.

1. Launch the Release Jamma.exe through Explorer and a Start-menu shortcut.
   Confirm the main window renders first and automatic console display leaves
   its focus unchanged. Also switch to another app during startup and confirm
   passive console display does not reclaim focus. Click the console later and
   confirm ordinary activation.
2. Set `consoleautostart` false, relaunch, then use main-window Ctrl+` to open,
   hide and reopen. Close the console and reopen again; history/draft should
   survive and Jamma should continue normally.
3. Select and double-click words while logs arrive, copy with Ctrl+C, paste
   Unicode text with Ctrl+V and the context menu, and reject multiline paste
   without changing the draft. Check native scrollbar/mouse wheel navigation.
4. While performing/recording, use Insert/Page Up/Page Down in the console,
   including held keys while switching focus both ways. Console navigation
   must not start performance gestures or leave held gestures stuck.
5. Check colors/status and `/help`, then connection/disconnection and chat on
   a session you choose. Load another session and confirm commands use the
   replacement Scene. Close Jamma while the console is visible/hidden and
   check ordinary audio/plugin teardown.
6. Resize the console and move it between monitors with different DPI. Check
   input, selection and transcript layout, including a small/narrow window.

After acceptance, merge into `codex/integrate-ready-features` and rerun its
combined build/regression checks. Keep master promotion separate.

Suggested follow-up prompt:

> Read doc/native-console-window-handoff.md and run the remaining manual
> acceptance checks with me on codex/native-command-window. Record precise
> passes/failures, fix any regressions on this branch, and rerun affected tests.
> Once acceptance passes, merge into codex/integrate-ready-features and verify
> the combined result. Do not push or promote master.
