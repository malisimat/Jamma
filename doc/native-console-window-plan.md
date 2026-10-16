# Jamma native command window implementation plan

This plan targets `codex/native-command-window`, created from `codex/integrate-ready-features` at `5ee471a2`. The focus prototype and its recommendations remain separate. This repository copy incorporates independent review before implementation.

## Motivation and decision

Replace the separately launched `JammaConsole.exe` and its Windows Terminal/classic-console host with a small native command window owned by `Jamma.exe`. Jamma already captures its log stream and executes submitted commands on the app-owner thread. Keeping the view in the same process removes the host-selection, named-pipe, companion-startup, and focus-repair paths while preserving the existing command behavior.

The requested interface is a Jamma command/log view, not a general-purpose shell: a colored, scrollable transcript; ordinary mouse text selection including double-click word selection; Ctrl+C/Ctrl+V; one command input line; a simple status bar; and a layout that follows window resizing. Windows Rich Edit and EDIT controls supply the text editing, selection, and clipboard behavior. Jamma supplies transcript formatting, command rules, layout, and lifecycle. No ConPTY, FTXUI renderer, Windows Terminal, or separately installed application is needed.

## Target behavior

- Keep the existing `consoleautostart` setting. When enabled, create and show the command window only after Jamma has completed its first main-window render and buffer swap. Show it once with the tested passive `SetWindowPos` path (`HWND_BOTTOM`, `SWP_NOACTIVATE`, and `SWP_SHOWWINDOW`). Do not use a timer, activation repair, or persistent no-activate style.
- Create the controls hidden after the first completed main-window frame, including when `consoleautostart` is disabled. The native transcript then owns bounded history while hidden; do not discard drained startup lines or add a second replay model. Drain the startup ring only once the controls exist. An explicit user action uses ordinary show/activation behavior.
- The command window is a normal unowned top-level window on Jamma's UI thread. It has no permanent focus suppression. Clicking it later activates it normally. Closing it hides it; it does not close Jamma or reopen itself. The main window's existing `toggle-console` action continues to open or hide it.
- Route log output to a read-only Rich Edit transcript. Preserve the familiar severity/subsystem/chat colors. Cap retained text explicitly (UTF-16 units and logical entries), including programmatic insertion; a native edit limit alone is insufficient. Append bounded batches. Preserve selection and a text-based viewport anchor, adjusting both for any evicted prefix; follow output only at the bottom with no active selection. Never rebuild the transcript with SetWindowText.
- Use a single-line EDIT control for commands. Keep the current command parser and input limit. Enter submits one command; Ctrl+C and Ctrl+V use normal Windows text-editing behavior. Transcript selection copies selected text. Paste remains bounded and rejects or safely removes line breaks so a multiline paste cannot submit several commands unexpectedly.
- Show a compact status row for connection state and the most recent command/log status. Recalculate child-control layout on resize and DPI changes. Keep all window/control operations on the UI thread.
- Console typing, Insert, Page Up/Down, selection and clipboard keys must not trigger performance actions. Consume tracked global transitions while the console is foreground, while forwarding any release needed for an action previously dispatched to the performance window. Preserve the global shortcut behavior outside the console. Support keyboard focus traversal between transcript and input and retain F1 help where practical.

## Implementation steps

1. **Create the new branch and add a focused window class.** Branch from `codex/integrate-ready-features`. Add `ConsoleWindow.{h,cpp}` to the Jamma app project. Keep its API small: create hidden, append a batch of log entries, update status, submit a command request, show passively, show for an explicit user request, hide, and destroy. Use the existing Win32 message loop; do not add a second UI thread. Window/control/font/DLL lifetimes have one UI-thread RAII owner; partial creation failures unwind safely and report once without stopping audio.

2. **Build the native layout.** Load the Windows `Msftedit.dll` Rich Edit control for a read-only multiline transcript, a standard EDIT control for one-line command input, and a simple native status control. Register the console window as an ordinary top-level window without `WS_EX_NOACTIVATE`. Handle `WM_SIZE`/DPI layout, Enter submission, close-to-hide, and the standard control clipboard shortcuts. Reuse the app's existing `Comctl32` link dependency where applicable.

3. **Connect existing log capture directly to the view.** Keep `ConsoleCapture` and the bounded `LineRing`: capture can begin before the UI exists and remains safe for log producers on other threads. In the owner loop, drain captured lines into a bounded UI batch and append them to the Rich Edit control. Preserve a visible dropped-log notice when the ring overflows. Remove the reconnect replay deque and pipe-event forwarding; they exist to feed a separate companion. Reuse only the severity-to-color mapping from `JammaConsole/src/LogColors.*` if it can be separated cleanly from FTXUI types.

4. **Keep command execution on Jamma's owner thread.** Retain a bounded owner-thread command queue and the existing `HandleSlashCommand` path in `Jamma/src/Main.cpp`. The input control enqueues a command; the main loop executes at most one between message dispatch and rendering, using the current Scene after any replacement and waiting until prior presentation output drains. Capture bounded pre-command and synchronous output, then queue the echo/result directly so capture overflow cannot discard command results. This pending presentation batch holds at most 516 entries; append at most 32 per frame. The native transcript retains at most 2048 entries and 256 Ki UTF-16 units, trimming complete entries. Remove IPC promises/futures and mutexes when all queue users share the UI thread. Validate the 4096-byte UTF-8 limit at submission (not just EDIT's UTF-16 length), reject embedded NUL/control/line breaks, handle conversion failure, and preserve the draft when full or invalid. Bound clipboard reads before allocating and reject multiline paste. Never execute commands or capture a Scene pointer from a window procedure.

5. **Move optional startup after the first real frame.** Remove broker autostart before `Window` construction. Create hidden controls only after the first completed `window.Render()` and `window.Swap()`, then show passively once if configured. This is an explicit lifecycle boundary, not a delay. Consume any explicit toggle without a later automatic reshow; failed startup creation must not retry every frame. Keep passive startup and explicit activation separate so later clicks are never suppressed. A console close must never post WM_QUIT. Initialize MSG and destroy the console before COM teardown on every exit path.

6. **Simplify status and shutdown.** Feed connection and last-event status directly from the existing app state instead of publishing status snapshots to the companion. Drain logs and commands on the owner loop as today. On shutdown, stop accepting commands, destroy the native console controls/window on the UI thread, then continue the existing scene/audio teardown. Remove duplicate broker/command cleanup paths once the lifecycle has one owner.

7. **Remove the companion and terminal-host machinery.** Apply the cleanup below after references and packaging scripts have been checked. Keep the existing `toggle-console` input/action interface and `consoleautostart` configuration key unless implementation discovers a compatibility issue.

8. **Validate the launch and interaction contract.** On the implementation branch, verify actual Start-menu and Explorer launches: the main Jamma window appears first and stays foreground on automatic console display; later clicking the console activates it normally. Also check disabled autostart, explicit toggle/reopen, close-to-hide, log selection during live output, double-click word selection, Ctrl+C/Ctrl+V with bounded single-line paste, colors, status updates, resize/DPI, command results, log overflow, and clean shutdown. Run the same checks with Windows Terminal installed and absent to confirm it has no effect.

9. **Review and commit in checkpoints.** Commit this reviewed plan, then implementation/cleanup, then any review repairs and validation evidence on the new branch. Compile the real ConsoleWindow implementation into native tests and exercise hidden controls without audio: Unicode selection and viewport preservation during append and prefix trim, tail-follow, bounds, clipboard/input rejection, hide/reopen, resize, passive display, and teardown. Retain parser, ring/concurrent capture and toggle tests; replace obsolete transport/render tests rather than preserving dead code for them. Build affected Debug and Release app/test projects using local tasks, run the native suite and threading audit, inspect executable imports for removed terminal DLLs, and record any environmental or manual-only acceptance limits honestly.

## Cleanup inventory

| Existing code or setting | Planned action | Reason |
|---|---|---|
| `Jamma/src/ConsoleBroker.{h,cpp}` | Remove after the native window is wired | Process launch, WT detection/fallback, named-pipe worker, title discovery, reconnect, and focus repair all serve the external companion. |
| `Jamma/src/ConsoleLaunch.h` | Remove | Command-line construction and launch-host selection are no longer needed. |
| `JammaConsole` project and `JammaConsole/src/Main.cpp` | Remove from `Jamma.sln`, the Jamma project reference, and packaging/deployment | There is no separately launched console executable. |
| `JammaConsole/src/LogColors.*` | Reuse only the severity mapping if useful; remove the FTXUI-specific implementation | Preserve familiar colors without keeping the terminal renderer. |
| `console/WindowsPipe.h`, `console/SessionGate.h`, `console/Protocol.h`, `console/OutboundMailbox.h` | Remove once repository-wide reference checks show they have no remaining consumer; move any still-needed generic input limit/UTF-8 validation into the small shared command layer | These implement the companion protocol and pipe transport. |
| `console/ClipboardText.h` | Remove if only the companion uses it | Native controls own clipboard transfer; retain any generally useful conversion helper only if another Jamma path depends on it. |
| `console/Transcript.h`, `SelectionController.h`, `PromptEditor.h`, `PasteInput.h`, `TextCoordinates.h` | Remove after reference checks, retaining only small command validation/history helpers that are still wanted | These implement cell-based FTXUI rendering, caret/selection, and bracketed-paste behavior that native controls replace. |
| FTXUI and `utf8proc` dependencies used only by `JammaConsole` | Remove from manifests/build configuration after checking all solution targets | They are not needed for the native controls. |
| `consoleautostart` in `JammaLib/src/io/InitFile.{h,cpp}` | Keep; retain its current default and serialized key | It remains the user's show-at-startup preference. |
| `consoleforceconhost` in `JammaLib/src/io/InitFile.{h,cpp}` | Remove the field, default, parsing, and serialization | Host selection no longer exists. Older unknown config keys should remain harmless under the existing config parser. |
| `Jamma/src/ConsoleStatus.h` | Remove or reduce after checking references | The old periodic status-snapshot helper was for the companion mailbox; direct UI updates should be event/state driven. |
| `JammaLib/src/io/ConsoleTui.{h,cpp}` | Check references; remove if it is still unused | It is a separate ANSI console UI and does not participate in the native-window design. |
| `Jamma/src/ConsoleCapture.h` and `console/LineRing.h` | Keep | They capture logs from non-UI producers safely and provide a bounded handoff to the UI thread. |
| `console/CommandParsing.h` and `HandleSlashCommand` in `Jamma/src/Main.cpp` | Keep | They define current command semantics and keep scene changes on the app-owner thread. |
| `console/CommandMailbox.h` | Keep but simplify if the promise/future is only used for IPC replies | A bounded queue still cleanly transfers input-control submissions to the owner loop. |
| `Window::ConsumeConsoleToggleRequest()` in `JammaLib/src/graphics/Window.{h,cpp}` | Keep | This is the existing app action boundary; only its consumer changes from broker to native window. |

Before deleting shared helpers, search the whole solution and deployment scripts for references. Do not remove an item merely because its current consumer appears to be the console; keep or relocate any generic behavior that another target uses.

## Stable code landmarks

- `Jamma/src/Main.cpp`: command parsing/handling, `ConsoleCapture` and `LineRing` setup, current broker startup, owner-loop log and command draining, console toggle consumption, and shutdown.
- `Jamma/src/ConsoleBroker.{h,cpp}` and `Jamma/src/ConsoleLaunch.h`: current separate-process and WT/classic-console implementation to retire.
- `console/CommandMailbox.h`, `console/CommandParsing.h`, and `Jamma/src/ConsoleCapture.h`: reusable command/log boundaries.
- `JammaConsole/src/Main.cpp`, `console/Transcript.h`, `console/PromptEditor.h`, and `console/SelectionController.h`: current FTXUI UI and models to consult for behavior, then trim as native controls take over.
- `JammaLib/src/graphics/Window.{h,cpp}`: existing `toggle-console` action and Win32 main-window lifecycle.
- `JammaLib/src/io/InitFile.{h,cpp}`: existing console startup and host-selection preferences.
- `Jamma/Jamma.vcxproj` and `Jamma.sln`: app source/project dependencies to update when removing the companion.

Use these file and API names as orientation rather than line-number instructions; implementation should follow the branch's current structure when the work begins.

## Implementation checkpoint

Implemented and independently reviewed at `fcf15fbb`. Debug and Release app,
engine and native-test builds passed; full suites passed 1685 and 1679 tests
respectively. See [validation evidence and manual acceptance](native-console-window-handoff.md)
before merging into the integration branch.
