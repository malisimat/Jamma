# Jamma Performance Console Plan

## Outcome

Build one C++ FTXUI companion, `JammaConsole.exe`. Jamma opens it automatically in a dedicated Windows Terminal window when `wt.exe` is available. If Windows Terminal is absent or cannot launch, Jamma opens the same companion automatically in the built-in Console Host. Commands, transcript, prompt, status, keyboard editing, and basic colour work in both. Console Host may offer less capable font, emoji, transparency, and mouse presentation. Recommend Windows Terminal without making installation a prerequisite.

The first release has one screen: fixed header, bounded scrolling transcript, single-line prompt, and one-line status. Future Jamma-owned panels may be added later without building a panel framework now. There is no PowerShell, arbitrary shell execution, assistant integration, command history, suggestions, or multiline editor.

```text
+------------------------------------------------------------------+
| JAMMA                                      connected · 122 BPM   |
|                                                                  |
| Jamma events, stdout, stderr, NINJAM chat, help, results        |
|                                                                  |
| > /connect 2                                                     |
| last: connected · loops 18 · memory 612 MB                       |
+------------------------------------------------------------------+
```

### Current behavior to preserve

`Jamma/src/Main.cpp` currently calls `AllocConsole`, starts `io::ConsoleTui` from `JammaLib/src/io`, redirects `std::cout`/`std::cerr`, and dispatches prompt input. Existing slash commands are `/`, `/?`, `/help`, `/c <number>`, `/connect <number>`, and disconnect aliases `/d`, `/q`, `/quit`, `/exit`, `/disconnect`. Ordinary input is NINJAM chat. `/connect` takes a **server-list number**, not a hostname. Preserve the asynchronous server-directory help behavior and Scene replacement safety. Do not advertise `/mute` or `/status` unless later work adds and tests them.

The repo has a vcpkg manifest and Visual Studio projects but no tracked installer project. A source checkout must work without an installer or a Windows Terminal profile.

## Implementation rules

- Inspect touched code first. Select the simplest design that meets the behavior with the smallest change radius. Make surgical edits; follow existing naming, ownership, project, and test conventions. Do not add a general event bus, command framework, theme engine, installer, or telemetry subsystem for this screen.
- Keep audio, loop, and NINJAM behavior in JammaLib and app orchestration in Jamma. Keep FTXUI and rendering in JammaConsole. Put shared protocol types in a neutral location only if both processes actually need them.
- Follow `doc/glossary.md` and `doc/realtime-audio.md`. Callback-owned paths must stay bounded, allocation-free, exception-free, lock-free, and free of formatting, logging, console writes, and pipe I/O.
- Do not promise a status value without an authoritative, safely published source. Start with connection state and last event. Add loop count, memory, BPM, phase, or mute count only after verifying ownership and snapshot access.
- Keep TUI model mutation and rendering on one owner thread. Pipe workers post bounded updates to it. For each new shared variable record owner, readers, writers, synchronization, and teardown. Prefer existing immutable snapshot patterns across thread boundaries.
- Bound queues, messages, input, and transcript retention. Coalesce replaceable status. Keep event order; when events must be dropped, count them and emit a visible loss notice once space returns. A full queue must never block audio or UI-critical work.
- Never introduce anonymous namespaces.

## User experience

### Launch and lifecycle

- Open on app startup by default. Add a persisted disable-startup option and an app action to reopen. Reopening must not create duplicate active companions.
- Use supported `wt -w <unique-name> --size <columns>,<rows>` syntax with a correctly quoted absolute companion path. Name the window uniquely per Jamma process so `wt` does not add a tab to an existing window. The named-pipe handshake, not the `wt.exe` handle, determines readiness.
- If `wt` is absent or launch/handshake fails, create a new Console Host window for the companion and report the fallback once. Jamma remains usable without either host's optional visual features.
- Closing or crashing the companion disconnects only its IPC session. Jamma and audio continue. Reopen creates a fresh authenticated session. On Jamma shutdown, request companion exit and tear down workers with bounded waits.
- Let the terminal host own resize, focus, minimization, placement, and font. Do not locate or manipulate the terminal `HWND`.

### Appearance

- Use semantic colours and the host's font. Emoji must not be essential to interpreting state. Remain legible in narrow widths and Console Host.
- A Windows Terminal JSON fragment with `opacity` and `useAcrylic` is optional. Install it only through a supported real packaging or first-run path; do not edit user `settings.json`. Default launch must work without the fragment. Acrylic may be unavailable on Windows 10 because of OS, power, remote desktop, or graphics conditions.
- Document the terminal's native transparency gesture where supported. Do not add an `/opacity` command or window-handle hacks.

### Transcript, prompt, and status

- Show one ordered stream of existing non-real-time application events, stdout/stderr, NINJAM chat, help, and command results. Audit producer threads before replacing stream buffers; do not redirect callback logging into a locking or allocating path. Avoid recursive logging of broker errors.
- Store logical entries with stable IDs separately from wrapped rows. Initially cap at **64 MiB and 100,000 entries, whichever is reached first**. Render visible rows plus a small margin. Preserve a logical viewport anchor across resize and new output. Define selection/anchor behavior when an entry is evicted.
- Follow the newest entry until the user scrolls, clicks older content, or selects. `End` or a follow action resumes. Wheel and Page Up/Down scroll the transcript.
- Prompt supports UTF-8 insertion, left/right, Home/End, Backspace/Delete, word movement/deletion, paste, mouse caret placement where supported, selection, and `Ctrl+C` copy. Bound input length; reject malformed or oversize input clearly. Jamma validates/executes commands, while the companion submits text and displays results.
- Update status at no more than 1 Hz without busy polling. Keep connection state and last significant event at narrow widths, then include only safely available fields. Status is presentation, never transport authority.

### Mouse selection and clipboard

The companion enables terminal mouse reporting and owns ordinary left-drag selection; the terminal host consequently does not own ordinary drag selection in that mode. Use FTXUI's supported mouse/selection APIs where sufficient and add only a small local model for logical transcript mapping and edge scrolling. Do not fork FTXUI.

- Press/drag/release updates selection continuously; dragging above or below the viewport extends it with bounded automatic scrolling. Selection expands and contracts.
- Prompt click without drag places the caret; drag selects prompt text. Starting one region's selection clears the other.
- `Ctrl+C` copies selected plain Unicode text to the Windows clipboard and must not terminate the companion. Copy logical newlines, never visual wrap breaks. Preserve grapheme and full-width character boundaries in caret, wrap, selection, and copy mapping.
- If mouse reporting is unavailable, retain keyboard input and host-native selection as reduced behavior; click-to-place is unavailable and the limitation is clear.

## Process and IPC contract

```text
Jamma.exe: app/Scene owner + non-real-time console broker
    || versioned local named pipe
JammaConsole.exe: pipe client -> single-owner model -> FTXUI
    -> Windows Terminal or Console Host
```

- Use one local duplex named pipe, one active client, explicit protocol version, and a per-launch unpredictable session identifier. Restrict access to the current user. In Phase 0, choose and document a rendezvous that works when Windows Terminal launches the child from an existing terminal process; do not assume handle inheritance from Jamma. Validate the identifier and pipe client identity before accepting commands. Define the threat model honestly: a command-line identifier or same-user-readable storage cannot authenticate against another process already running as that same user.
- Frame explicit UTF-8 messages with a bounded length prefix. JSON is acceptable outside the audio path. Specify maximum frame/input/event sizes, invalid version or frame behavior, disconnect behavior, and monotonically increasing command request IDs.
- Minimum messages: `hello`, `event`, `status_snapshot`, `command_request`, `command_result`, and `shutdown`. Add heartbeat only if pipe disconnect handling leaves a concrete liveness gap.
- On connect/reconnect, send current status then live events. A new companion need not replay a previous process's transcript; document this. Execute an accepted command ID at most once per session and reject requests from stale sessions.
- Serialize command execution onto the app/Scene owner path. Preserve the current Scene replacement lifetime boundary. Never hold a pipe/console lock while calling Scene or emitting output.
- Separate event capture, queueing, pipe I/O, and rendering. All serialization and writes run outside callbacks. If a producer is callback-owned, leave it on an already safe diagnostic path or omit it from console capture.
- Shutdown order: stop accepting commands; detach sources; request companion exit if connected; cancel/close pipe operations; join workers with bounded behavior; restore any intercepted streams; destroy Scene under its existing ownership rules.

## One-session orchestration instructions

Execute the phases **in order within one long-running session** and strive to complete the feature. Start by recording branch, status, plan revision, and repo instructions. Work on the feature branch; never commit unrelated user changes. Read `doc/glossary.md`, `doc/realtime-audio.md`, `doc/build.md`, and local `.vscode/tasks.json`; reread tasks before **every** build or native-test run as repo policy requires. Build affected projects incrementally. Run **unit tests only**; do not substitute GUI/manual/system/integration tests. Report host behavior that unit tests cannot prove.

Spawn subagents for **bounded code research and independent read-only review**. Suggested research: command/Scene lifetime, stdout/stderr producer threads, FTXUI mouse/selection hooks, Windows launch/profile semantics, protocol/security, and thread safety. Give each a question, paths, requested file/line evidence, and a short deliverable. The orchestrator owns design and edits, reconciles findings, and prevents simultaneous edits of the same files. Keep implementation serial across dependent phases; parallelize independent research/review. Maintain a compact checkpoint with decisions, phase state, tests, findings, commit hashes, and blockers in session context and, for recovery, a temporary file outside tracked source.

For **every phase**:

1. Research affected paths. Record a small change list, state ownership table, failure behavior, and unit-test list. Choose the smallest convention-matching solution.
2. Implement only that slice, preserving the preceding committed phase. Update this plan before widening scope if new facts change the design.
3. Build affected projects incrementally. Add and run relevant native **unit tests**. Record exact command, result, and test counts. If the machine lacks a prerequisite, report it instead of guessing a build tool path.
4. Obtain independent read-only review against this plan and original behavior, including correctness, thread safety, teardown, callback safety, and diff radius. Fix findings, rerun affected unit tests, and repeat review until no required fixes remain. For cross-thread changes run the `threading-review` audit and manually inspect callback-owned bodies named in `doc/realtime-audio.md`.
5. Inspect diff and status. Commit that phase to the feature branch with a descriptive message **only after** tests and review pass. Record the hash and result. Carry no known required fix to the next phase.

If interrupted or compacted, resume from the latest commit, checkpoint, and git status; inspect uncommitted work before proceeding. Retry failed subagent tasks narrowly or complete research/review directly. Do not loop indefinitely on a blocked dependency or call an unverified phase complete. Finish independent work, record the exact blocker, and report an honest final status. A successful build alone never closes a phase.

## Delivery phases

### Phase 0 — contract and feasibility foundation

**Implementation:** Map existing command/chat, logging, Scene replacement, startup/shutdown, and status sources with file/line evidence. Confirm pinned FTXUI/vcpkg integration, Windows Terminal syntax, Console Host fallback, mouse hooks, and Unicode clipboard against primary sources. Add a compilable companion skeleton and only the pure input/selection/terminal-capability interfaces needed to prove the selected APIs. Define frame schema, limits, error cases, Windows Terminal-compatible rendezvous and threat model, and shared-state ownership. Confirm selection needs no FTXUI fork.

**Unit tests:** Protocol framing/version/size cases and pure prompt/selection coordinates, including expansion/contraction, Unicode/full-width text, edge movement, and copy extraction, using mocked events/clipboard.

**Review:** API compatibility, dependency fit, smallest viable adapter, and model support for required selection behavior. Commit when no required fix remains.

### Phase 1 — companion, broker, and lifecycle

**Implementation:** Add JammaConsole to the solution; pin FTXUI via the manifest/baseline mechanism; implement pipe framing, authentication, one-client lifecycle, app broker, unique-window launch, Console Host fallback, reopen, and bounded shutdown. Keep old console startup behind a temporary migration switch until parity. Keep host launch outside JammaLib.

**Unit tests:** Bad frames/versions/tokens, duplicate clients/requests, disconnect/reconnect generation, launch argument quoting, fallback decision, and shutdown with mocked process/pipe boundaries.

**Review:** Pipe ACL and secret exposure; process/handle lifetime; duplicate/orphan prevention; no callback path into broker; bounded teardown. Fix and commit.

### Phase 2 — commands, events, and status

**Implementation:** Move submit dispatch to app-owned serialized execution, preserving numbered connect, help, disconnect aliases, and chat. Route safe stdout/stderr and events to a bounded ordered stream without recursive capture. Publish only safe status values; coalesce status and expose event-loss count. Add no unrelated commands or engine telemetry.

**Unit tests:** Existing command parsing and fake-Scene dispatch/lifetime, request/result correlation, duplicate rejection, event ordering, overflow notices, status coalescing, and teardown during an in-flight command.

**Review:** Compare every command/chat path with `Main.cpp`; inspect touched producer threads, Scene replacement, lock order, callback safety, and event order. Fix and commit.

### Phase 3 — core screen and keyboard

**Implementation:** Build header, visible-row transcript, logical retention, follow-tail, resize anchoring, bounded prompt editor, 1 Hz status, and narrow/Console Host presentation. Pipe workers post updates to the single TUI owner. Use existing FTXUI components where they meet the contract.

**Unit tests:** Retention/eviction, wrap/reflow, visible-row mapping, follow-tail, narrow status priority, Unicode edit/paste/input limits, and fake-clock refresh with no busy poll.

**Review:** Bound work under sustained synthetic model updates; check grapheme handling, resize, queue/render ownership, and subsystem scope. Fix and commit.

### Phase 4 — selection and clipboard

**Implementation:** Integrate ordinary left drag in transcript/prompt, repaint, bounded edge scrolling, prompt click-to-place, logical-text copy, and reduced behavior without mouse reporting. Ensure `Ctrl+C` cannot accidentally end the companion.

**Unit tests:** Press/move/release, expansion/contraction, region reset, edge scrolling, resize and eviction during selection, new output during selection, full-width/combining/emoji mapping, logical newline copy, clipboard failure, and no-mouse fallback.

**Review:** Check pinned FTXUI event semantics, cursor/selection invariants, clipboard encoding/lifetime, and bounded mouse work. Fix and commit.

### Phase 5 — default launch, migration, distribution, and docs

**Implementation:** Make companion startup normal; add startup preference and app reopen action using existing settings/UI conventions; retire normal `AllocConsole`/`ConsoleTui` startup after parity. Ensure companion and runtime dependencies ship beside Jamma through the actual distribution path. If no installer exists, do not create one solely for a Terminal fragment; document optional profile settings and keep launch functional without them. Document fallback, shortcuts, installation link, and transcript behavior across restart.

**Unit tests:** Preference default/persistence, reopen idempotence, Terminal-absent/failure fallback, companion path resolution, shutdown order, and packaging/config generation functions if present.

**Review:** Check source checkout and shipped layout, automatic startup on both hosts, old-console cleanup, and documentation matching behavior. Fix and commit.

## Final completion gate

After Phase 5, perform one **final independent review of the whole feature** against this plan, original command/chat behavior, `doc/glossary.md`, and `doc/realtime-audio.md`. Inspect cumulative diff and all new cross-thread ownership/teardown; run the threading audit and manually inspect callback-owned bodies. Fix findings, rerun affected **unit tests**, and commit final fixes. Confirm phase commits and clean git status. State which Windows Terminal/Console Host visual or OS interactions remain unverified because the requested testing scope is unit tests only.

Create a self-contained HTML completion summary in the **system temporary directory** and **open it in a browser** for the user. Include delivered behavior, changes and commit hashes by phase, exact build/unit-test evidence, review findings and fixes, fallback behavior, unmet acceptance items or concerns with severity and next action, feature branch, and head commit. Escape HTML content; do not put the report in tracked source. Link it in the final response. Strive to finish every phase in the one session; never claim completion if a required gate failed.

## Acceptance checklist

- Jamma automatically starts a usable companion in Windows Terminal or Console Host without requiring terminal installation or user commands.
- Existing help, numbered server connect, disconnect aliases, and chat retain their meanings and safe Scene lifetime.
- Resize/narrow layouts remain coherent; transcript and prompt memory are bounded.
- Status uses safe sources, updates at 1 Hz or less, and does not busy poll.
- Ordinary left drag selects dynamically; edge scroll, caret click, and exact Unicode copy work when mouse reporting exists. Keyboard editing and host-native selection remain usable otherwise.
- Companion close/crash/reopen never interrupts Jamma or audio. IPC and output never block callbacks.
- There is no arbitrary shell execution. Optional visual styling never gates functional use.
- Every phase has unit-test and review evidence and a feature-branch commit; final review and opened HTML report are complete.

## References

- Repo: `Jamma/src/Main.cpp`, `JammaLib/src/io/ConsoleTui.*`, `doc/glossary.md`, `doc/realtime-audio.md`, `doc/build.md`, `.agents/skills/threading-review/SKILL.md`.
- [Windows Terminal command-line arguments](https://learn.microsoft.com/en-us/windows/terminal/command-line-arguments) for `-w` and `--size`.
- [Windows Terminal JSON fragments](https://learn.microsoft.com/en-us/windows/terminal/json-fragment-extensions), [profile appearance](https://learn.microsoft.com/en-us/windows/terminal/customize-settings/profile-appearance), and [transparency troubleshooting](https://learn.microsoft.com/en-us/windows/terminal/troubleshooting).
- [FTXUI project](https://github.com/ArthurSonzogni/FTXUI) and [selection implementation](https://github.com/ArthurSonzogni/FTXUI/blob/main/src/ftxui/component/app.cpp); verify behavior against the pinned source in Phase 0.
