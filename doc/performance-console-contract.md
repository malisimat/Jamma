# Performance console contract (version 1)

This document fixes the Phase 0 process boundary. `console/Protocol.h` is the
wire-format authority. Jamma owns commands and Scene access; JammaConsole owns
terminal input, transcript, selection, and rendering. Neither process sends
pipe messages from an audio callback.

## Existing paths mapped for migration

| Concern | Current source | Required preservation |
| --- | --- | --- |
| Startup and console | `Jamma/src/Main.cpp:242-251,505-550` | `SetupConsole` allocates Console Host, then `ConsoleTui` starts before app startup logs. |
| Commands and chat | `Jamma/src/Main.cpp:69-153,538-550` | `/`, `/?`, `/help` asynchronously refresh/list servers; `/c` and `/connect` use 1-based reachable-server numbers; disconnect aliases are `/d`, `/q`, `/quit`, `/exit`, `/disconnect`; ordinary input goes to `Scene::SendNinjamChat`. |
| Directory callback | `JammaLib/src/ninjam/NinjamSession.cpp:590-620` | Refresh may print from a detached worker; event capture must not depend on the UI thread. |
| Incoming chat and logging | `JammaLib/src/ninjam/NinjamConnection.cpp:1146-1192`; `JammaLib/src/io/ConsoleTui.cpp:148-160,267-299` | Chat is job-side output; the present narrow-stream buffer formats and writes synchronously on producers. |
| Scene replacement | `Jamma/src/Main.cpp:1056-1122` | The old console submit holds `sceneRawMutex`; replacement clears the raw view, shuts down outgoing Scene, rebinds Window and then publishes the new Scene under that mutex. |
| Shutdown | `Jamma/src/Main.cpp:1134-1165`; `JammaLib/src/engine/Scene.cpp:1985-2020` | Scene stops audio before editor/plugin release and joins its job thread; app stops TUI before process exit. |
| Status source | `JammaLib/src/ninjam/NinjamConnection.h:69-73,86-87`; `JammaLib/src/ninjam/NinjamSession.h:99-103`; `JammaLib/src/engine/Scene.cpp:1471-1504` | Physical connection is NINJAM-owned; do not read private connection data from the broker or infer BPM/phase from unrelated snapshots. |
| Callback audit | `JammaLib/src/audio/AudioHost.cpp:496-505,725-729`; `JammaLib/src/engine/Scene.cpp:1447-1469`; `JammaLib/src/audio/AudioHost.cpp:129-131` | Audio invokes Scene tick; existing RtAudio error output can originate at a callback boundary. No new capture lock/pipe write on it. |

## Rendezvous and trust

Before each launch Jamma generates a fresh unpredictable pipe name and session
token. It passes both as quoted UTF-16 arguments to JammaConsole. This works
when Windows Terminal asks an already-running terminal process to start the
child: no inherited handle or `wt.exe` process identity is needed. Jamma creates
one local duplex named-pipe instance with a DACL for the current user/logon SID,
`PIPE_REJECT_REMOTE_CLIENTS`, and `FILE_FLAG_FIRST_PIPE_INSTANCE`. The broker
checks the client's Windows identity and its first `hello` token before any
command is accepted. It accepts only one client per launch generation. A new
launch gets a new name and token, and old generations cannot submit commands.

This protects against other users, remote clients, accidental attachment, and
stale companions. It cannot protect against another process running as the same
user: that process may read command-line arguments or access same-user storage.
The token is a session discriminator, not a same-user security boundary.

## Frames

One message is a 32-bit little-endian length followed by a 10-byte body header
and UTF-8 payload. Length counts the body only. Header bytes are protocol
version (currently 1), message type, and a 64-bit little-endian request ID.
Maximum body size is 65,536 bytes. Event and result payloads are at most
16,384 bytes; commands and status are at most 4,096 bytes, `hello` at most
128 bytes, and `shutdown` has no payload. Invalid UTF-8, overlong encodings, surrogates,
and out-of-range code points are rejected. A malformed length, type, version,
or payload ends that pipe session; Jamma remains running. A partial frame at
EOF is invalid. The receiver must cap the length before allocating its body.

| Type | Direction | Request ID | Payload |
| --- | --- | --- | --- |
| `hello` | companion to Jamma | 0 | session token |
| `event` | Jamma to companion | 0 | one log/chat/help line |
| `status_snapshot` | Jamma to companion | 0 | presentation state; connection and last event initially |
| `command_request` | companion to Jamma | positive, increasing | entered command or chat |
| `command_result` | Jamma to companion | matching request | outcome text |
| `shutdown` | Jamma to companion | 0 | empty |

The broker executes an accepted request ID at most once in a session and
rejects duplicate, decreasing, or stale-session IDs. Command execution is
serialized on the app/Scene owner path, with Scene replacement excluded; pipe
workers never hold an output lock while calling Scene. On a fresh connection,
Jamma sends current status before live events. A replacement companion starts
with an empty transcript; there is no cross-process transcript replay.

## State ownership and failure behavior

| State | Owner | Readers / writers | Boundary and teardown |
| --- | --- | --- | --- |
| Scene pointer | Jamma UI path | UI/replacement writes; command dispatch reads | Existing `sceneRawMutex` currently excludes replacement; commands must move to serialized app path. |
| Pipe generation and token | Jamma broker | broker writes; authenticated client supplies token | Replace per launch, close server on shutdown. |
| Outbound events | non-real-time broker | safe producers enqueue; pipe worker drains | Bounded queue; overflow counted and reported after space returns; no producer waits for pipe. |
| Status | Jamma broker | app publishes; pipe worker reads latest | Replaceable snapshot, at most 1 Hz presentation updates. |
| Transcript, prompt, selection | JammaConsole owner loop | pipe worker posts bounded updates; UI loop mutates | Never mutate render model on pipe thread; discard at process exit. |

Transcript entry IDs increase with display order and are never reused. Retained
entry text is immutable. Evicting either endpoint clears its active selection.
Rows must be wrapped at Unicode grapheme boundaries, while cell widths use the
pinned FTXUI renderer's `string_width` policy; FTXUI 6.1.9 displays some joined
emoji across more cells than a typical terminal grapheme width.

`ConsoleTui` currently intercepts `std::cout`/`std::cerr` synchronously and
formats under a mutex on the producer thread (`JammaLib/src/io/ConsoleTui.cpp`).
Audio and vendor error callbacks can also write to those streams. Replacing
those buffers with a broker lock or pipe write would violate callback rules.
The capture migration must isolate callback-owned output or leave it on an
existing safe diagnostic path. C stdio and wide C++ streams are separate from
the existing narrow-stream capture and need an explicit non-real-time source
policy before claiming full stdout/stderr coverage.

## Terminal APIs

Use `wt -w <per-process-unique-name> --size <columns>,<rows> <absolute path>`.
Readiness is the pipe handshake. If launch or handshake fails, start the same
executable with `CREATE_NEW_CONSOLE`; do not manipulate a terminal HWND. FTXUI
6.1.9 supports mouse reporting and component mouse events, so a fork is not
needed. FTXUI's rendered selection cannot preserve logical newlines through
wrapping, so the companion needs a small logical selection model. `Ctrl+C`
must be handled by the component so it copies selection and does not terminate
the process. Clipboard data is `CF_UNICODETEXT` with movable global memory;
ownership transfers to Windows after successful `SetClipboardData`.

## Phase 0 change and verification record

Changed files: `vcpkg.json` pins the manifest baseline and FTXUI/utf8proc;
`JammaConsole` is a direct-build FTXUI API skeleton; `console` contains the
pure frame, prompt, and logical selection interfaces; the existing native
test project compiles their unit tests. No app or engine callback code changed.

On Debug x64, the task-configured absolute MSBuild executable built
`JammaConsole/JammaConsole.vcxproj` and
`test/JammaLib_Tests/JammaLib_Tests.vcxproj` incrementally with the absolute
`SolutionDir` argument. The focused native unit-test invocation was
`JammaLib_Tests.exe --gtest_filter=ConsoleProtocol.*:ConsoleTextCoordinates.*:ConsolePromptEditor.*`:
14 passed, 0 failed. Tests cover framing/UTF-8/version/limits, request-ID
shape, Unicode caret boundaries, selection expansion/contraction and eviction,
logical newline copy, bounded edge direction, input rejection, and a mocked
clipboard success/failure. The repository hot-path audit reported no new
lock/wait or shared-state keywords in the diff.

Independent review found and fixed a caret position inside a newly merged
grapheme, missing semantic frame validation, unstable selection indices, and
a difference between Unicode grapheme widths and pinned FTXUI render widths.

## Phase 1 lifecycle record

The companion is now in the x64 solution and writes beside `Jamma.exe`. The
app broker owns one pipe generation and worker. It mints a new pipe name and
token per Terminal or Console Host attempt, creates a user/System-only local
pipe, verifies the client Windows user and `hello` token, and accepts increasing
request IDs only while the session is open. `JAMMA_CONSOLE_PREVIEW=1` opens the
companion during migration; the original `ConsoleTui` remains authoritative
for commands until Phase 2. The broker has no Scene pointer or callback path.

| State | Owner / synchronization | Teardown |
| --- | --- | --- |
| Broker generation and worker | App UI owns `ConsoleBroker`; worker captures shared `State` | UI signals stop and joins; a timed-out worker remains owned and blocks reopen. |
| `Connected`, `Finished`, fallback notice | Worker writes; app reads atomics | Worker clears connection at exit; UI consumes notice and later joins. |
| Pipe and process handles | Worker alone owns RAII handles | Cancel pending pipe work; request shutdown; wait up to 500 ms for direct child, then terminate it. |
| Companion inbox and screen | Pipe reader posts bounded updates; FTXUI thread owns model | Exit screen, stop reader, and exit companion if OS pipe cancellation remains stuck. |

Windows Terminal can hand child creation to an existing terminal process, so
Jamma cannot reliably terminate a child after a failed Terminal handshake.
That child exits when its named pipe is unavailable. Each fallback attempt uses
another random pipe and token, preventing an old child from joining it. Windows
overlapped cancellation is drained before a stack buffer is released; if the
OS does not complete cancellation within the broker's 2.5-second join budget,
the broker keeps that generation and refuses reopen. At final process exit it
retains the shared state in the detached worker. Terminal host behavior and
pathological OS cancellation remain unverified under the unit-only test scope.

The app and native test Debug x64 projects built incrementally with the local
task's absolute MSBuild and absolute `SolutionDir`. The focused test command
`JammaLib_Tests.exe --gtest_filter=ConsoleProtocol.*:ConsoleTextCoordinates.*:ConsolePromptEditor.*:ConsoleSession.*:ConsoleLaunch.*`
passed 23 of 23 tests. New cases cover one-client tokens, request ordering,
reconnect generations, a buffered request after stop, shutdown pipe/child
ordering with fake boundaries, quoting, fallback, and no duplicate launch.

## Phase 2 command and event boundary

The old console and companion pipe reader now put bounded submissions into
`CommandMailbox` (64 entries, 4 KiB each). The Jamma app thread takes at most
eight per loop, releases the queue lock, then uses the current Scene. JAM
replacement and command execution therefore share the app owner; no raw Scene
pointer crosses to a console thread. Shutdown closes intake before stopping
the broker and Scene. A command already taken by the app finishes its result.
The session gate rejects duplicate IDs, and the broker retains each accepted
request ID until the app result is placed in the outbound queue.

The app, Scene job, legacy console input, and server-directory threads opt in
to `ConsoleTui`'s line capture hook. Audio and vendor callback threads do not.
The hook copies at most 4 KiB into a preallocated 256-line ring using bounded
atomics; the app drains and validates lines into the broker's ordered queue.
Event order is assigned at that app-to-broker ingress. Synchronous output from
a command is drained before its result; a concurrent job line captured after
that drain can follow the result.
Captured loss and invalid UTF-8 are counted and reported when outbound space
returns. The outbound queue holds 512 messages and at most 1 MiB of text;
status has a separate replaceable slot. Results can evict events but never
earlier results. One pipe writer sends frames without a queue or Scene lock.
The companion protects results in its 256-message inbox, coalesces status
separately, and shows event-loss counts after its UI owner drains.

| State | Owner / synchronization | Teardown |
| --- | --- | --- |
| Command submissions | Input/pipe producers enqueue under `CommandMailbox` mutex; app owner takes and executes | `Close` rejects new work and completes queued futures before Scene shutdown. |
| Captured lines | Opted-in producer claims a fixed `LineRing` slot atomically; app owner drains | Scene job and console input threads join; detached directory output is quiesced before stream buffers are restored. |
| Outbound events/results/status | App owner publishes under a short mailbox mutex; broker writer takes | Closed at broker worker exit; late callback references see a closed mailbox. |
| Companion inbox/status | Pipe reader enqueues under inbox mutex; FTXUI owner renders | Stop wakes a reader waiting for UI space; results remain until drained or process exit. |

The first status is sampled from `Scene::NinjamConnected()`, which checks the
physical NINJAM connection through `NinjamSession::IsConnected()`, before the
broker starts. Later status replaces the slot at most once per second. It
contains physical connection and the last published event. It does not infer
tempo, phase, memory, or other unverified fields.

`std::cout` and `std::cerr` on opted-in non-audio threads are mirrored; C
stdio, wide C++ streams, and callback-owned diagnostics are excluded. The
legacy TUI still handles those diagnostics during migration. Phase 5 must
keep the capture path when retiring its visual/input UI, and the final review
must check any required callback diagnostic policy against real-time rules.

Debug x64 `JammaLib`, `Jamma`, `JammaConsole`, and native tests built
incrementally with task-defined MSBuild and absolute `SolutionDir`. The
focused native command
`JammaLib_Tests.exe --gtest_filter=ConsoleSession.*:ConsoleCommand.*:ConsoleLaunch.*:NinjamSessionAudio.ChatReportsNotSentWithoutConnection`
passed 20 of 20 tests after the thread opt-in changes. The tests cover parser aliases, owner
replacement, close with queued and in-flight submissions, result IDs and
overflow, status replacement, capture bounds, final loss notice, and
disconnected chat outcome.

## Phase 3 screen ownership

FTXUI's loop alone owns the retained transcript, prompt, request IDs, paste
state, and rendering. The pipe reader posts bounded inbox updates; a separate
writer consumes a 64-request queue. At most 64 requests can remain outstanding.
The transcript retains up to 64 MiB of UTF-8 payload and 100,000 logical
entries. Entry IDs are never reused. Wrapped row starts are cached for the
current entry and width, and only visible row strings are built for a frame.
Eviction moves an old viewport anchor to the first retained entry; resizing
rewraps at the anchor's byte position. Page Up/Down and arrows pause following;
Ctrl+F resumes the tail. Home/End remain prompt editing keys.

The screen uses FTXUI fullscreen layout. Status arrives from the app at most
once per second; the companion has no polling timer. Narrow status abbreviates
connected/disconnected to C/D while retaining the beginning of the last event.
The prompt is limited to 4 KiB and keeps its caret in the visible cell window.
Windows clipboard Ctrl+V and bracketed terminal paste commit only after input
validation. A bracketed paste containing a control, including CR, LF, or
Escape, is consumed through its closing marker and rejected as a whole.
The companion enables bracketed paste and virtual terminal output mode and
restores console modes at exit. It clears processed input during the FTXUI loop
so Ctrl+C can reach the handled key event rather than a console control
signal. Terminal-specific behavior remains unverified under the unit-only
test scope.
