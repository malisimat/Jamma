# Device and session control implementation notes

The Audio, MIDI and File tabs use the existing panel, accepted rig coordinator,
MIDI job owner, AudioHost callback gate, and session exporter. Audio precedes
MIDI; endpoint selection and refresh controls live on MIDI. Channel counts share a
three-second steady-clock deadline. Audio transitions have a further 500 ms
cooldown measured after completion. Draft revisions coalesce; capture and rig
transitions defer audio work, and a failed attempt requires an edit or Retry.

## Ownership and teardown

| Introduced state | Owner / readers / writers | Synchronization and teardown |
| --- | --- | --- |
| Audio draft, schedule, inventory cache, accepted buffer-size history, widgets, save/load folders and pending persistence values | UI only | Commands freeze a complete request; Scene replacement destroys these values after job/callback teardown. |
| Scene identity allocator | Scene construction | Class-owned atomic counter; identity is immutable for each Scene. |
| Operation identity and writer exclusion | UI | Increasing operation ID, paired with Scene identity. UI closes trigger ingress, waits by returning to the control loop for job acknowledgement, then try-locks the job lifecycle boundary. |
| MIDI request/completion | UI publishes request, job exchanges request and publishes result, UI exchanges result | Immutable atomic shared-pointer publications carrying Scene/operation IDs and complete desired inputs; outgoing job is joined before Scene destruction. |
| Control admission / session replacement flags | UI writes, job reads | Acquire/release atomics; job skips refresh during control admission and skips all mutation during replacement/shutdown. |
| Scene lifecycle mutex | Job tick and UI lifecycle owner | Off callback only; UI uses try-lock for admission, and never waits for job acknowledgement while holding it. Shutdown acquires it without other locks, publishes quit, releases before joining. |
| Audio active stream metadata | AudioHost lifecycle writes, UI/job reads | Existing complete atomic immutable publication; publish only after start succeeds. |
| Audio backend callback context | Lifecycle publishes host/geometry; backend reads and counts readers | Sequentially consistent host/count handoff. Confirm backend stop before replacing geometry; drain readers before destroying host. Uncertain teardown quarantines backend/context, including a failed candidate that returned no AudioDevice. |
| Audio recovery failure latch | Lifecycle writes; job admission recovery reads | Atomic; pause/resume/uncertain teardown failures cannot be undone by an automatic admission reopen. Explicit safe recovery clears it. |
| MIDI callback admission | Lifecycle closes/reopens; callback enters/leaves | One lock-free atomic word orders closure against reader entry. Drain before resetting timestamp mapper, queues, anchors or callback state. Confirm backend close before clearing captures/reclaiming endpoint. |
| MIDI endpoint mapper / slot / watermark | Callback owns mapper and watermark; job owns slot/topology | Worker is joined before topology changes. Explicit suspension also drains backend callbacks before mapper/queue reset; resume waits for a fresh audio anchor and discards paused ingress. |
| Pending live-note reset | Station callback normally owns; quiescent UI reset stages it | Callback, job and live dispatcher excluded for reset. Release actual delivered live notes after BeginMidiBlock; at most 64 releases and 2048 slot inspections per block. Preserve loop playback ownership and pinned source-loss release records. |
| Hardware metadata merge | Accepted rig coordinator, serialized UI/job lifecycle owners | Rebuild from immutable descriptors, never copy callback-swapped route vectors. Merge only hardware fields into latest accepted rig; retry persistence separately from audio reopening. |
| Export snapshot / attempt files | Operation-local control owner | Quiescent owned copies; restore audio and release other writers before serialization/disk. No borrowed Scene data in disk work. Cleanup only files exclusively created by this attempt; manifest commit uses same-directory no-overwrite rename. |

## Lifecycle decisions

- ASIO discovery must not construct a second RtAudio ASIO instance while a stream
  is open: that backend can unload the active driver. The UI uses cached inventory;
  explicit switching/retry probes after confirmed close and revalidates exact
  device identity, rate and counts before open. Only buffer negotiation is allowed.
- Preserve same-rate transport and material. Rate changes reject any takes,
  including empty ones, update every existing plugin while quiescent, clear empty
  sample-unit quantisation/phase state, reanchor MIDI, and invalidate NINJAM timing
  through its existing unavailable-edge/epoch owner before resuming.
- Route policy is the documented fallback: disconnect unavailable hardware-output
  routes only after successful start, and warn on reduction. Rollback does not
  change routes. Take/Loop destinations are internal station buses and are not
  filtered by physical output count. Export preserves explicit empty route arrays.
- Save preflight resolves local folder/version naming before audio pauses.
  Admission rechecks capture with trigger ingress, job, live MIDI and audio writers
  excluded. Numbered manifests have attempt-specific WAV/MIDI names and a checked,
  exclusive, no-overwrite commit; collision is retryable. File success and audio
  restoration success are separate results.
- Load picks before suspension, constructs a candidate with its job excluded,
  retains the outgoing Scene on failure, and uses exact hardware startup after
  replacement. Failed startup retains loaded content with stopped/retry status.
- Fatal unconfirmed backend teardown deliberately retains resources that may
  still receive callbacks. It reports that application restart is required;
  replacement cannot safely proceed without a backend lifetime barrier.

## Adversarial review corrections

Independent subsystem reviews and lead integration review corrected candidate
construction racing its job, startup fallback during Load, pre-pause mutable
export reads, ambiguous pause failures, old asset overwrites, stale rig persistence
retry, route-bus misclassification, stale MIDI timestamps, callback reset admission
races, queued/live held notes surviving pauses, partial MIDI failure status,
callback-context reclamation after failed opens, and stale admission recovery.
Full-suite integration checks also corrected duplicate popup/HUD button dispatch,
pointer-move reentrancy during protected capture, and empty-take loss in both
manifest parsing and reconstruction. Existing missing/corrupt recorded-asset
skip behavior is retained. Plain command buttons explicitly opt into forwarding;
existing subclasses keep their own dispatch.

## Validation

On 2026-10-12, incremental Debug x64 builds of JammaLib, JammaLib_Tests and
Jamma succeeded using the local task configuration. The full native suite passed
all 1,426 tests across 173 suites; `git diff --check` passed. The focused rerun
of previously failing integration cases passed all 14 tests before the full run.

Focused native coverage includes latest-value scheduling/debounce/cooldown,
exact ASIO validation, stopped/nested quiescence, callback draining, rig hardware
merge, MIDI duplicate identity/source loss/admission, live-note reset, button
modifiers and integer overflow/fraction rejection, numbered saves/reloading assets,
empty takes/internal bus routes, eligibility rejection, commit collision cleanup,
separate restoration failure, and NINJAM format epoch invalidation.

The repository hot-path audit scans whole changed callback-containing files and
therefore flags off-callback Scene locks. Each flagged addition is in job/UI
lifecycle, admission, validation or persistence work. Manual callback-body review
checks the changed AudioHost backend wrapper and Station live-note-release path,
as well as the existing checklist; no lifecycle mutex, wait, filesystem/device
I/O, formatting or allocation is introduced in callback-owned bodies.

Physical ASIO switching/negotiation/stop failures, real MIDI hotplug, third-party
plugin restart, a connected NINJAM format transition, large-session pause duration,
and disk-full/access-denied behavior require hardware/manual validation. Native
checks cannot establish correctness of a third-party driver's or plugin's internal
threads; these are not claimed as manually verified.
