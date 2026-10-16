# Diagnostics and startup capture

Jamma captures `std::cout`, `std::cerr`, and `std::clog` on UI and worker threads.
Newlines, explicit flushes, worker exit, and capture shutdown publish pending
text. Each thread has separate stream buffers. Oversize lines split into entries
of at most 4096 bytes at UTF-8 boundaries, retaining the tail. The existing
256-entry console handoff reports dropped entries when producers outrun the UI.
Closing the console hides it; capture and the existing transcript continue.

`startup.log` starts before configuration, device discovery, and Scene creation.
The same capture event reaches the console once and the file once. It includes
ordinary worker output as well as explicit boot messages. Startup ends when the
first rendered Scene has drained its initial job queue, including queued plugin
loads. Initial jobs retain explicit startup origin through completion, so their late
failures still reach the file. A 30-second window after startup enters the UI loop
bounds the general tee if jobs keep arriving; its expiry emits a warning. A blocked UI cannot drain diagnostics
or enforce that deadline until it resumes. Explicit runtime scopes/jobs stay in the console even while the general startup window remains open. Unspecified producers inherit that window; explicit startup producers retain file attribution after it closes. Early orderly returns flush pending startup text before closing the log.

The file rotates through startup.log.1, .2 and .3 and is capped at 1 MiB per launch.
Directory, rotation, open, write, flush and size-limit failures report once through
the console ring and Windows debugger output, bypassing streams to avoid
recursion. A saturated console still reports its lost-entry count when drained;
the startup file tee is independent of console saturation. A completely full
file cannot retain its own size-limit warning.

## Sources and limits

| Source | Route |
| --- | --- |
| VST2/VST3 load errors and VST3 state capture/restore failures | C++ capture; plugin path, operation and available native error |
| JAM/configuration read/parse and WAV material open failures | C++ capture; path and operation; parse diagnostics remain at their existing owners |
| ASIO inventory/probe/open/start/stop/fallback | Existing unconditional diagnostics and startup reports |
| Asynchronous RtAudio errors | One 512-byte bounded mailbox, drained by UI; overflow/truncation counted |
| Audio callback stream status | Atomic bit mask, drained by UI |
| MIDI inventory/connect/disconnect and serial failures | Existing unconditional C++ diagnostics, now including workers |
| NINJAM refresh and caught job/app exceptions | C++ capture, including formerly swallowed refresh exceptions |
| OpenGL callback | C++ capture, replacing the application's raw C stderr callback |
| Console creation failure | Existing unconditional C++ diagnostic, retained in capture |

Capture is an off-callback facility. Audio and MIDI callback entry points use a
trivial thread-local suppression scope before reaching engine/plugin code;
accidental callback stream writes are counted and reported later. This does not
make arbitrary callback stream formatting safe: callback fault producers must
use bounded events or counters. Backend errors use a single-attempt mailbox and
status counters, with no waiting, allocation, formatting, locks or I/O added to
the callbacks.

Raw C stdout/stderr from third-party code, wide streams, OutputDebugString from
third parties, separate processes, and arbitrary SDK-specific logging callbacks
are not redirected. Catchable C++ exceptions are diagnosed at app/job/refresh
boundaries. Any unexpected Scene worker exception reports once, stops job mutation and notifies the UI to run normal Shutdown. The failed worker waits off callback for Shutdown to publish permanent ingress closure, acknowledges it, and joins; final-save failures also remain inside this boundary. No failed structural pump is retried. Access violations, process termination, driver crashes, stack
corruption and arbitrary plugin crashes have no recovery guarantee. No fatal
handler flushes iostream state: its locks/allocators may already be compromised.
Use the Windows debugger/crash reporting for fatal failures; startup.log contains
only output successfully written before the failure.

## Thread ownership

- ConsoleCapture State: non-callback producers and TLS destructors serialize on
  its mutex; the UI restores stream buffers after producer shutdown. Stop flushes
  registered fragments, disables sinks and detaches their references. TLS keeps
  State alive safely after Stop, without retaining sink owners.
- StartupLog: setup/destruction on UI; Append and failure consumption under the
  capture mutex. No callback accesses its handle or file state.
- DiagnosticThread: suppression flag belongs to each callback thread. Suppressed
  counts/status bits are atomic. The backend mailbox transitions idle -> writing
  -> ready -> reading -> idle; release/acquire publishes complete records. Only
  the successful writer/reader accesses its payload. Occupied producers drop and
  increment the atomic loss count; teardown needs no resources.
- Scene job failure: the job owner publishes an atomic failure flag; UI reads it each loop and invokes Shutdown, which publishes the permanent-close token before joining. The worker performs no more pumps/saves after failure and waits only for that token. UI acquires lifecycle ownership after any admitted tick unwinds; the worker never calls Shutdown or joins itself. Teardown joins before the flag owner is destroyed.
- Scene startup completion: UI arms under the existing job mutex after the first
  frame. The job owner consumes it after its queue empties; Scene shutdown joins
  that owner before capture destruction. Queue origin is stamped under the job
  mutex before publication, then remains immutable through consumption; runtime replacement Scenes default
  to runtime origin. The job's TLS scope spans execution, caught failure reporting
  and cleanup. A shared atomic readiness bit hands completion back to UI. Capture
  completion and timeout serialize on the capture mutex. Backend records retain
  origin at production; status masks keep startup/runtime observations separate.
  The audio callback never accesses Scene startup queue state.
- VST picker handler: shell/UI thread only; local ComPtr and Advise references
  retain it through Show, then Unadvise releases the shell's reference. The handler
  retains no dialog pointer. Initial remembered-folder events cannot select a
  bundle; only subsequent navigation does.

VST3 bundle resolution is a non-callback lifecycle operation. A directory resolves
only the Windows x64 layout `<bundle>/Contents/x86_64-win/<bundle-name>`; direct
module files retain their path. Both PreInit and standalone Load use that target.
The outer path remains in the existing saved plugin list. Missing modules and
invalid/wrong-architecture binaries fail recoverably with requested/resolved
paths, stage, and Windows error; other bundle architectures/layouts are unsupported.

The runtime VST picker keeps normal file selection. Double-clicking or opening a
.vst3 directory selects its outer bundle instead of navigating inside it. The
initial remembered folder is ignored by this policy, and cancelling returns no
selection. The hint in the picker describes both choices. This uses the shell's
[folder navigation event](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifiledialogevents-onfolderchanging)
and [Close callback behavior](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifiledialog-close).
