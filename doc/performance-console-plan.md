# Jamma Performance Console Plan

## Decision

Build a separate `JammaConsole.exe` companion process in C++ using FTXUI, and launch it automatically in a dedicated Windows Terminal window. Communicate with `Jamma.exe` over a versioned local named-pipe protocol.

This gives Jamma a Codex-like terminal interface while leaving font shaping, emoji, GPU text rendering, window resizing, scroll presentation, and native Windows composition to Windows Terminal. Windows 10 remains a first-class target. When Windows Terminal is missing, launch the same companion in the classic Console Host with reduced visual features.

The first release has one window and one layout:

```text
+------------------------------------------------------------------+
| JAMMA                                      connected · 122 BPM   |
|                                                                  |
| scrolling transcript: Jamma events, stdout, stderr, chat, help  |
|                                                                  |
|                                                                  |
| > /connect server.example.net                                   |
| last: station 4 muted · loops 18 · memory 612 MB · assistant ●  |
+------------------------------------------------------------------+
```

The architecture must allow Jamma-owned tabs or panels later, but the first release does not implement them. It does not expose PowerShell or arbitrary shell commands.

## Recommendation percentages

The percentages express relative fit for the stated requirements, not mathematical probabilities.

| Approach | Fit | Recommendation |
| --- | ---: | --- |
| C++ FTXUI companion in Windows Terminal | **68%** | Choose this. It provides the requested terminal quality with the least new platform and build complexity. |
| Rust Ratatui companion in Windows Terminal | **22%** | Strong fallback if the C++ selection prototype fails. It closely resembles the Codex CLI stack, but adds a Rust toolchain and a second implementation ecosystem. |
| Native Win32/DirectWrite terminal-style window | **10%** | Reserve for a future product direction that requires exact window ownership or rich visual controls beyond terminal cells. It adds substantial rendering, input, selection, clipboard, accessibility, and resize work. |

### Weighted comparison

| Requirement | Weight | C++/FTXUI + Windows Terminal | Rust/Ratatui + Windows Terminal | Native DirectWrite window |
| --- | ---: | ---: | ---: | ---: |
| Modern font, emoji, GPU rendering | 20 | Excellent | Excellent | Potentially excellent, but Jamma must build it |
| Dynamic drag selection and prompt editing | 20 | Good after a focused selection layer | Good after a focused selection layer | Excellent with substantially more UI work |
| Windows 10 experience | 15 | Excellent when Windows Terminal is installed; functional fallback | Same host behavior | Good, fully bundled |
| Implementation effort | 15 | Best | Moderate | Poor |
| C++/Visual Studio integration | 10 | Excellent | Weak | Excellent |
| Resize and scroll behavior | 10 | Good; terminal and FTXUI handle geometry | Good; terminal and Ratatui handle geometry | Jamma owns all behavior |
| Future internal tabs/panels | 5 | Good | Excellent | Excellent |
| Dependency size and maintenance | 5 | Good; compact, active, no runtime dependency | Moderate | Poor total ownership cost |

## Why this is a terminal, not a shell

- Windows Terminal is the host window and renderer. It provides the font, emoji, GPU rendering, window chrome, resizing, opacity, and terminal protocol support.
- `JammaConsole.exe` is the terminal application. It draws the banner, transcript, prompt, status line, selection, and future panels.
- PowerShell and Command Prompt are shells. They are not part of this design because the console accepts only Jamma commands.
- ConPTY is primarily useful when a host needs to run another console application. Jamma is the terminal application, so it does not need to embed PowerShell or build its own ConPTY host.

## Target experience

### Window and launch

- `Jamma.exe` launches one dedicated, named Windows Terminal window containing `JammaConsole.exe`; the user never types a launch command.
- The window opens automatically by default. Add a setting to disable startup opening and an app action to reopen it.
- Closing the terminal window disconnects only the companion. Jamma and audio continue running.
- Closing Jamma asks the companion to exit; its terminal window then closes naturally.
- Windows Terminal owns manual resize, minimize, restore, focus, and remembered placement. Do not locate or manipulate its `HWND` with heuristics.
- Start at a practical size with `wt.exe --window <unique-name> --size <columns>,<rows>`. Let the user resize freely afterward.
- Future Jamma tabs and panels live inside the one TUI. Do not create Windows Terminal tabs for Jamma features.

### Windows 10 setup

- Detect `wt.exe` during installation and startup.
- Recommend Windows Terminal strongly on Windows 10 and offer an installer link when absent.
- Install a Jamma Windows Terminal profile through the supported JSON-fragment mechanism instead of editing the user's `settings.json`.
- The profile inherits the user's default terminal font unless the user selects another font.
- If Windows Terminal is unavailable, run `JammaConsole.exe` in classic Console Host. Preserve commands, transcript, prompt, status, keyboard input, and basic colour; clearly omit GPU-specific presentation and acrylic.

### Appearance and opacity

- Use semantic TUI colours mapped onto the terminal palette so the console looks good with user themes.
- Provide a small set of optional Jamma colour schemes without overriding a user's chosen font.
- Configure opacity through the Jamma Windows Terminal profile using official `opacity` and `useAcrylic` settings.
- On Windows 10, prefer acrylic because unblurred profile opacity is a Windows 11 feature.
- Support Windows Terminal's native live opacity gesture, `Ctrl+Shift` plus mouse wheel. Do not add window-handle hacks for a `/opacity` command.
- Treat opacity as progressive enhancement: Windows policy, battery state, Remote Desktop, or GPU capability can disable acrylic.

## Mouse selection decision

Ordinary left-button dragging must dynamically select transcript or prompt text. Dragging beyond the transcript viewport must extend the selection and scroll. `Ctrl+C` must copy the exact selected Unicode text.

Terminal mouse capture is all-or-nothing: when an application asks for mouse events, the terminal stops owning ordinary drag selection. Cursor placement therefore cannot coexist with terminal-owned unmodified selection. The companion will enable mouse capture and own selection as a real application interaction, in the same way a GUI text control owns its selection.

Implement one explicit selection controller on top of FTXUI's mouse and selection support:

- Press in the transcript starts a transcript selection.
- Drag updates the active endpoint on every mouse movement and repaints the selected cells.
- Dragging above or below the transcript starts bounded automatic scrolling and continues extending the selection.
- Press in the prompt starts a prompt selection; a click without a drag moves the prompt caret.
- Transcript and prompt selections are separate. Starting one clears the other.
- `Ctrl+C` copies the active selection as plain UTF-8/Unicode text through the Windows clipboard.
- Scrolling or selecting disables follow-tail. `End` or an explicit follow action returns to the newest line.
- If mouse reporting is unavailable, retain keyboard input and offer terminal-native selection as a degraded fallback; click-to-place is disabled in that mode.

This is the highest-risk interaction in the plan. Prove it on Windows 10 before building the rest of the UI.

## TUI behavior

### Layout

- Fixed banner/header at the top.
- Virtualized, scrollable transcript in the middle.
- Single-line prompt above the status line.
- One-line status bar at the bottom, refreshed once per second.
- No command history, suggestions, multiline editor, dropdowns, or shell mode in the first release.

### Transcript

- Show all application events, stdout, stderr, NINJAM chat, help output, and future assistant responses in one ordered stream.
- Style event categories distinctly while preserving copyable plain text.
- Store logical entries separately from their wrapped screen rows so resizing can reflow the viewport.
- Use a bounded ring buffer. Start with 64 MiB or 100,000 logical lines, whichever comes first, and make the limit configurable later.
- Render only visible rows plus a small margin.
- Maintain follow-tail while the user is at the bottom.
- Pause follow-tail when the user scrolls, clicks older content, or begins a selection.
- Preserve the viewport anchor across resize and new output.
- Never let terminal backpressure block an audio or UI-critical Jamma thread.

### Prompt

- Accept only registered Jamma commands such as `/connect`, `/mute`, `/status`, and `/help`.
- Support Unicode insertion, left/right, Home/End, Backspace/Delete, word movement/deletion, clipboard paste, mouse caret placement, prompt selection, and `Ctrl+C` copy.
- Keep command parsing and permission checks in `Jamma.exe`; the companion submits command text and displays structured results.

### Status line

Refresh at 1 Hz and show a width-aware subset of:

- connection and session state;
- last significant event;
- loop count;
- memory use;
- BPM/phase or audio health when useful;
- muted-station count;
- future assistant connectivity.

At narrow widths, keep connection state and the last event, then remove lower-priority fields. Do not horizontally scroll the status line.

## Process and data architecture

```text
Jamma.exe
  engine/audio state
        |
        | immutable status snapshots + structured events
        v
  non-real-time console broker
        ||  versioned local named pipe
        vv
JammaConsole.exe
  model -> FTXUI layout -> ANSI/VT output
        |
        v
Windows Terminal / classic Console Host fallback
```

### Responsibilities

`Jamma.exe` owns:

- application and session state;
- command validation and execution;
- status snapshot production;
- log/event fan-out;
- launching and stopping the companion;
- permission policy for future external assistants.

`JammaConsole.exe` owns:

- transcript buffering, wrapping, viewport, and selection;
- prompt editing and command submission;
- banner and status rendering;
- palette mapping;
- reconnect/error presentation;
- future Jamma-owned tabs and panels.

### IPC

- Use a local duplex named pipe with an explicit protocol version and per-launch random token.
- Use length-prefixed messages; JSON is acceptable initially because traffic is low and outside the audio path.
- Define structured message types: `hello`, `event`, `status_snapshot`, `command_request`, `command_result`, `shutdown`, and `heartbeat`.
- Authenticate the companion with the inherited token and restrict the pipe to the current user.
- Keep a bounded non-real-time outbound queue in Jamma. Coalesce status snapshots but do not silently coalesce transcript events.
- If output exceeds safety limits, protect audio first and emit an explicit dropped-event count. Logging from real-time callbacks remains prohibited.
- Make reconnect idempotent so the console can be closed and reopened without restarting Jamma.

## Dependency and project placement

- Add FTXUI through the existing vcpkg manifest and pin the version used by the build.
- Add a small `JammaConsole` application project to the solution.
- Keep TUI presentation and IPC client code in `JammaConsole`.
- Keep reusable command DTOs and protocol definitions in a non-audio part of `JammaLib` only if both processes genuinely need them.
- Keep engine and audio behavior in `JammaLib`; never link terminal rendering into an audio callback path.
- Retire the current `AllocConsole` plus hand-written `ConsoleTui` path after the companion reaches feature parity. Preserve a temporary feature flag during migration.

FTXUI is a reasonable dependency here because it is C++, cross-platform, active, supports Windows, UTF-8, mouse input, layout, input components, and selection, and does not require a separate runtime. Its source footprint is comparable with existing vendored support libraries, although the exact binary delta must be measured in the spike.

## Delivery phases

### Phase 0: feasibility spike and gate

Build a disposable `JammaConsole.exe` that proves the following on the current Windows 10 development machine:

1. Jamma launches a dedicated Windows Terminal window without user command-line work.
2. Font inheritance, emoji, colours, banner, transcript, prompt, and status line render correctly.
3. Resize reflows without corruption or high CPU use.
4. Ordinary left-drag selection works in transcript and prompt.
5. Selection expands and contracts dynamically, auto-scrolls during edge drag, and copies exact Unicode text.
6. A click in the prompt moves the caret.
7. A sustained synthetic log stream remains responsive.
8. Acrylic/opacity works through official Windows Terminal settings on Windows 10.
9. Classic Console Host fallback remains usable.

Do not proceed with full integration until these pass. If FTXUI's selection hooks cannot meet the interaction without invasive patching, repeat this spike in Ratatui. Choose the native DirectWrite route only if both terminal stacks fail the required selection behavior.

### Phase 1: companion and IPC foundation

- Add `JammaConsole` project and FTXUI dependency.
- Implement named-pipe handshake, reconnect, shutdown, and protocol versioning.
- Add a non-real-time console broker in Jamma.
- Launch Windows Terminal with a unique named window and fall back to Console Host.
- Keep the existing console behind a feature flag.

### Phase 2: Codex-like core screen

- Implement banner, virtualized transcript, prompt, and one-second status line.
- Route structured events and current stdout/stderr into the transcript.
- Implement resize, wrapping, follow-tail, wheel/Page Up/Page Down, and bounded retention.
- Add semantic colour themes and distinct styling for future assistant messages.

### Phase 3: complete mouse and clipboard behavior

- Implement transcript and prompt selection models.
- Add continuous drag updates, edge auto-scroll, Unicode extraction, and `Ctrl+C`.
- Add click-to-place in the prompt and prompt selection editing.
- Exercise selection while output is arriving and while the window resizes.

### Phase 4: packaging and polish

- Install the supported Windows Terminal JSON fragment.
- Add startup preference and reopen action.
- Add Win10 detection, recommendation, and fallback messaging.
- Add crash/restart handling and final removal of `AllocConsole` from normal startup.
- Document terminal shortcuts, including live opacity adjustment.

## Acceptance criteria

- No user must open a terminal or type a command to start the console.
- Windows 10 plus Windows Terminal gives the full-quality experience.
- The window uses the terminal's font and supports Unicode and emoji.
- The banner, transcript, prompt, and status line remain correct after arbitrary resizing.
- Status refreshes once per second without busy polling.
- The transcript remains responsive under sustained log output and retains a bounded scrollback.
- Ordinary left-drag selects transcript or prompt text dynamically without a modifier key.
- Selection can auto-scroll, and `Ctrl+C` copies exact text.
- Clicking in the prompt places the caret.
- The console cannot execute arbitrary shell commands.
- Closing or crashing the console does not stop audio or Jamma.
- Console output and IPC never block a real-time audio callback.
- Opacity uses supported Windows Terminal composition settings; unsupported systems degrade cleanly.

## Explicitly deferred

- multiple Jamma console windows;
- internal tabs and panel layouts;
- general PowerShell or shell execution;
- command history, suggestions, and multiline prompts;
- searchable transcript history and persistence across runs;
- dropdowns, modal dialogs, tables, station maps, and 3D control widgets;
- OpenClaw protocol, assistant permissions, and action confirmation policy;
- programmatic `HWND` manipulation of the Windows Terminal window;
- a custom terminal emulator, ConPTY host, font shaper, or GPU text renderer.

## Sources informing the decision

- [Windows console and terminal definitions](https://learn.microsoft.com/en-us/windows/console/definitions)
- [Windows Terminal overview and GPU rendering](https://learn.microsoft.com/en-us/windows/terminal/)
- [Windows Terminal command-line launch options](https://learn.microsoft.com/en-us/windows/terminal/command-line-arguments)
- [Windows Terminal JSON fragment extensions](https://learn.microsoft.com/en-us/windows/terminal/json-fragment-extensions)
- [Windows Terminal profile opacity and acrylic](https://learn.microsoft.com/en-us/windows/terminal/customize-settings/profile-appearance)
- [Windows Terminal mouse interaction](https://learn.microsoft.com/en-us/windows/terminal/tips-and-tricks)
- [FTXUI project and supported features](https://github.com/ArthurSonzogni/FTXUI)
- [FTXUI selection implementation](https://github.com/ArthurSonzogni/FTXUI/blob/main/src/ftxui/component/app.cpp)
- [Codex CLI Ratatui/Crossterm TUI source](https://github.com/openai/codex/blob/main/codex-rs/tui/src/tui.rs)

