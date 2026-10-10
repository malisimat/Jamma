# Trigger and Station state remediation plan

Status: implementation complete; independent adversarial reviews, integrated native
tests and affected application builds passed. Final evidence and limits follow.

This plan covers Trigger ownership of recording actions, LoopTake capture lifecycles,
Station visual aggregation, and interruption by confirmed routing/source loss.
The baseline is `22e8980e`, following `c91401e3` and `206f71d3`.
Keep each independent fix in a separate commit. Update this document with actual
commits, tests, review outcomes, and evidence limits as work completes.

## Required behaviour

1. A Trigger owns at most one **live** Record/Overdub/Punch capture at a time.
   The user explicitly clarified that a new capture **must be allowed while a prior
   take is ending**. Prior audio tails may overlap the new capture and each must
   reach Playing independently. Different Triggers may also record concurrently.
2. New capture cannot bypass an outstanding structural result, but must not wait
   for a completed action's audio tail. Freeze each take's capture identity and
   completion work, so a later Trigger action cannot overwrite an earlier tail.
   MIDI-only takes finish immediately; audio and mixed takes keep their full
   delayed-input capture and crossfade work.
3. Station display is derived from its takes, rather than the last arbitrary action:

   | Takes present | Display |
   | --- | --- |
   | Any active Record, Overdub, or Punch | Most recently changed mode among those active takes |
   | No active mode, but any recording tail | Record End |
   | Neither active capture nor recording tail | Default |

   Record, Overdub, and Punch have equal priority. Ending/ditching one take must
   reveal the next-most-recent still-active mode, not conceal it. No-op/rejected
   actions must not change mode recency. Completed playable content still falls
   back to Default, as explicitly requested; retain numeric enum values for compatibility.
4. A confirmed loss/disconnection that invalidates an owned capture cancels that
   active take, restores an overdub source where appropriate, and clears stale
   control-down state. Preserve completed history and other Triggers' takes.
   Rejected edits, persistence failure, and transient inventory errors must not ditch.
5. No new data races, unsafe borrowed-pointer lifetimes, callback locks/waits,
   callback allocation/deallocation, or unbounded retry/scanning work.

## Research evidence and boundaries

Two independent Luna researchers traced the production trigger path and Station
membership/publication paths. Findings are source-derived; regression tests must
reproduce them before calling remediation complete.

- Production trigger input runs through `Trigger::OnTick`,
  `_ProcessQueuedInputActions`, `_ApplyInputEdge`, and `StateMachine`
  (`JammaLib/src/engine/Trigger.cpp`, approximately lines 419–427 and 529–625).
  `_pendingCompletion` pauses edge consumption. Structural results are matched by
  sequence/completion, and rejected start/end/ditch outcomes settle or restore state
  (approximately lines 1151–1242). Preserve these existing protections.
- Structural actions execute on the job owner under `Scene::_sceneMutex`
  (`JammaLib/src/engine/Scene.cpp`, `OnJobTick` and `_PumpTriggerStructuralActions`,
  approximately lines 1780–1789 and 1926–1936). The older direct Trigger action
  methods were not found in current production input call sites. Harden bypasses
  only if actual callers or tests demonstrate a supported unsafe entry point.
- A successful record/overdub end sets Trigger to Default and clears its active
  history index before its audio take finishes the tail (`Trigger.cpp`,
  `_ApplyStructuralResult`, approximately lines 1179–1193). Another activation
  then creates a take (`Station.cpp`, REC_START, approximately lines 1227–1255).
  This overlap is expressly permitted by the user's subsequent clarification;
  prove that only one live capture exists and all prior tails finish independently.
- Station action branches write one visual enum imperatively (`Station.cpp`,
  approximately lines 1254, 1340, 1366, 1438, 1460, 1480, 1508). They can mask
  another Trigger's active take. The current tail helper only fixes tail priority.
- Callback `Station::OnTick` writes the same visual enum and traverses mutable
  `_loopTakes` through `_HasRecordingTail` (`Station.cpp`, approximately lines
  1591–1618). `Scene::InitAudio` wires this tick into the audio callback. Membership
  is modified by AddTake/commit on job/UI owners (approximately lines 1672 and
  2350 onward). Atomic take states do not make mutable-vector traversal safe.
- Tail jobs are not dropped by the general job-list deduplication: they are
  dispatched synchronously from Scene::CommitChanges (approximately lines
  2388–2400). However `_endRecordingCompleted` and `_loopsNeedUpdating` are plain
  flags written by callback EndMultiWrite and consumed/reset by UI CommitChanges
  (`LoopTake.cpp`, approximately lines 874–901 and 2347–2368; `LoopTake.h`,
  approximately lines 460–461). This is an evidenced publication race that can
  lose independent completion/update work. Audit neighbouring counters too.
- Overlapping overdub sessions currently share the Trigger's `_overdubMixer` and
  prepared writer. `PreparedTriggerBounceWriter::Write` advances that mixer on
  every write (`Trigger.cpp`, lines 25-50); each active/tailing take calls its writer
  independently (`LoopTake.cpp`, lines 566-586). `StartOverdub` clears both delayed
  action arrays and resets the shared mixer (`Trigger.cpp`, lines 1561-1570), while
  `StartRecording` clears mixer actions (lines 1507-1513). Losing an old delayed
  punch-out can leave `_isPunchInActive` true and prevent tail completion
  (`LoopTake.cpp`, lines 887-895). This is a concrete overlap defect, not merely a
  hypothetical reason to prohibit new recording.
- MIDI loss can remove the release needed to clear `_isLastActivateDownRaw`.
  Repeat NoteOn is then suppressed after reconnect (`Trigger.cpp`, IgnoreRepeats/
  Debounce, approximately lines 1251–1305). Device refresh closes endpoints and
  dispatch without generating trigger releases (`midi/MidiRouter.cpp`, InitMidi/
  `_CloseMidi`, approximately lines 433–435 and 600–623; `Scene.cpp`,
  `_RefreshMidiIfNeeded`, approximately lines 1861–1908).
- User routing edits are boundary-checked and currently rejected while busy
  (`audio/AudioHost.cpp`, PublishRigTriggerTransitionAtAudioBoundary, approximately
  lines 227–258; Trigger CanEditRouting/CanApplyCaptureRouting, lines 482–526).
  Capture-route checks currently omit the previous ending take. Never equate
  submitting a candidate with confirming a disconnect.
- Ordinary device disappearance preserves configured channels/names
  (`io/RigFile.cpp`, RigFileRouting::Resolve, approximately lines 580–703).
  Runtime refresh therefore does not normally swap receivers/routes. Its defect
  is loss of input delivery without lifecycle recovery. Test any exceptional
  normalization-caused route change rather than assuming all unplug events swap routes.

## Implementation sequence and commit boundaries

### A. Aggregate Station visuals without callback-owned mutable membership

Replace imperative action/tick visual assignments with one consistent derivation
using published immutable membership and coherent per-take mode/recency information.
Prefer a read-derived `GetVisualState` or a single-owner publication; do not leave
audio and job threads overwriting one enum with independently computed results.

Use the established published snapshot pattern. Membership must include newly
created takes and exclude removed takes at the correct publication boundary, including
before UI commit. No callback access to `_loopTakes`, `_backLoopTakes`, maps, or
other mutable job/UI containers. Avoid adding callback hierarchy traversal merely
to keep a cached display value alive; compute on the UI/render side when possible.

If a transition serial is needed, make state and serial one coherent observation
(for example, one packed lock-free atomic or existing immutable publication), with
a Station-owned ordering source and explicit lifetime. Do not introduce hidden
global mutable sequence state. Increment on meaningful accepted mode transitions,
not every read/action/tick. Define MIDI-only punch and delayed audio punch visuals
explicitly so their logical mode is represented even when audio capture is delayed.
Logical active Punch belongs to the live session; an old tail with delayed audio
punch work remains a tail and must not win active-mode priority. Distinguish these
without treating `IsArmed()` as a live-session predicate: it deliberately includes
tail states and `_isPunchInActive` (`LoopTake.cpp`, lines 864-873).

Audit every `GetVisualState` caller before making it read-derived: the getter is
also used during engine action handling, not exclusively rendering. Use an existing
safe membership snapshot at those callers. If it holds weak references, prove
job-owned strong retention while callback borrowers exist; weak-pointer locking
does not prove that final release/destruction stays off the callback. Packed atomic
presentation values must be demonstrably lock-free on supported Windows targets.

Tests: two distinct triggers record/overdub/punch in both orders; equal modes;
end/ditch the newest and an older take; tails with other active modes; no-op and
absent-target actions; unpublished/new and removed membership; tail completion;
MIDI-only punch; Default with and without completed content. Include controlled
cross-thread publication tests and immutable membership retirement checks.

### B. Prove one live capture and safe independent overlapping tails

First fix the evidenced callback-to-UI completion/update flag handoff with bounded,
lock-free consume-once publication (for example atomic flags consumed by exchange).
Atomic exchange alone fixes the lost-update race, but does not establish exactly
one completion: EndMultiWrite can publish true again while the completion job is
pending. Either tolerate and prove idempotent repeated completion or latch a
per-session completion once, including generation validation when jobs execute.
Release/acquire publication must cover the payload read by the consumer; merely
making the flag atomic cannot make concurrent counter resets/reads safe.
Also close the parent dirty-signal lost wakeup: `GuiElement::CommitChanges`
(`JammaLib/src/base/GuiElement.cpp`, lines 284-302) checks atomic `_changesMade`
and unconditionally clears it after `_CommitChanges`. A callback publication during
that call can be erased, leaving the final completion unconsumed if audio stops.
Consume dirty state before processing, while preserving existing derived-class
semantics (audit their use of `_changesMade` before selecting a fix). Add a controlled
publication-during-commit test with no later callback tick. Keep this a focused
handoff fix, not a broad unrelated GUI flag conversion.
Audit `_endRecordSampCount`, `_endRecordSamps`, recorded count, and punch/ditch/reset
access for cross-thread conflicts; keep sample counting on its existing owner and
publish only what other owners need. Completion is per take, and a stale completion
must not finish a different capture or survive reuse/reset. Mark ditched/removed
take status inactive at the appropriate boundary so old snapshots cannot report a
live take forever. Test callback completion concurrent with UI consumption and
prove no lost wakeup, duplicate destructive job, or callback allocation.
This is evidenced: `LoopTake::Ditch` (lines 1998-2034) clears counters/content but
does not publish `STATE_INACTIVE`; coordinate invalidation and callback quiescence
before clearing callback-visible resources. Inactive publication alone does not
make an already executing callback safe.

Preserve immediate start after a successful end, even while the earlier take is
ending. Do not add a tail-busy start guard, deferred/backdated capture, or a new
Trigger Record End state. Keep completion on each LoopTake, not a shared Trigger
cursor or current-history index. New starts must not clear an earlier take's needed
delayed punch/mixer actions, alter its capture/bounce writer, shorten its audio tail,
or route its completion to the new active take.

Trace the existing shared overdub mixer/writer and delayed-action arrays carefully.
If a new action can affect an old ending take, freeze the needed capture/bounce
inputs per take or use prepared per-capture handles. Prepare allocations on the
job side and hand off through existing bounded snapshots/commands. Do not introduce
callback-owned variable-sized history, allocation, final-owner release, or extra
raw-pointer status polling merely to gate starts that should remain allowed.

Separate B into reviewable commits: B1 completion/counter handoff; B2 per-session
overdub writer/envelope and target-bound delayed actions; B3 any remaining evidenced
exclusivity/lifetime hole. B2 is required by the shared-writer evidence above.
Allocate/prepare each session's envelope/writer on the structural job owner; retain
it through that take's tail and retire after the reader acknowledgement off callback.
Delayed commands must retain session identity and independent mixer targets, survive
new starts, and be cancelled only for their own ditched/removed session. Ditch rollback
must restore that session's actions without overwriting another session's tail work.
Define envelope advancement once per capture block, including multiple channels,
rather than once per destination write. Freeze channel/source configuration too;
changing a Trigger's next capture configuration must not reinterpret old tail data.
Use existing fixed capacities with explicit safe overflow handling: never silently
drop the punch-out required for completion or overwrite a still-draining session.
Capacity rejection must preserve the current capture and all existing tails.

Preserve pending structural serialization and sequence/token matching so duplicate
or rapid edges cannot initiate two live captures. Prove borrowed target lifetimes
across ditch, removal, reset, completion, and history retirement; a target once in
a Station snapshot is not automatically safe after retirement.

Tests: rapid start/end/start with audio/mixed tails still present; count exactly one
live capture while one or more prior tails exist; advance old tails through callback
capture and structural completion jobs while the new Trigger stays Recording;
old tails must reach Playing with unchanged length/audio. Cover MIDI-only repeat
capture, rapid overdub/end/start, delayed punch/mixer work, another Trigger sharing
the Station, old tail-target removal/reset, routing changes, and bounded history/
command/result rejection. Fix only evidenced holes, each in a separate commit.
Verify actual samples and fade progression for overlapping stereo overdubs; checking
only final enum states cannot catch shared-envelope corruption. Drive production
edge/structural-result paths, not only direct helper methods, for exclusivity proof.

### C. Recover confirmed source loss and disconnect safely

Detect confirmed endpoint/capture-source removal from a successful availability
refresh or accepted routing change. Treat failed inventory refresh as unknown,
not a disconnect. Identify the affected Trigger and its owned active/tailing target;
do not cancel unrelated or completed takes. Freeze capture identity throughout a
live capture, including selected audio channels, MIDI sources, and receiver.

Send a bounded, coalescing cancellation/lifecycle request to the audio-owned Trigger
state machine. It must serialize with pending starts/ends/punch/ditch and use the
existing job structural-command/result discipline for take destruction and source
unmuting. Reset missing-release binding/raw/down state on the audio owner. Associate
requests with identity/revision/generation so reconnect or a new capture cannot be
cancelled by an old loss request. Clear/drain stale queued edges only under the
existing producer/reader barrier or revision discipline. A confirmed loss during
a pending start must either prevent that start or cancel its newly owned take.

Preserve rejected user-edit and failed-persistence recordings. For explicit confirmed
station-target disconnect/replacement, either keep the existing busy rejection (no
disconnect was confirmed) or implement acceptance/cancellation with a documented
transaction boundary. A successful disconnect must never leave an orphan capture.
Do not erase historical takes when cancelling current capture. If audio is stopped,
use only a proven stopped-reader reset path; do not mutate audio-owned fields from
the job owner merely because a timeout elapsed.

Tests: activation-device loss after NoteOn; selected capture-device loss during
Record/Overdub/Punch/tail; multiple sources and unrelated Triggers; pending start/end;
repeat loss coalescing; reconnect and stale request/edge rejection; transient inventory
error; rejected edit and failed persistence; confirmed receiver disconnection;
cancelled overdub source unmuting; safe lifetime before/after acknowledgements.

Separate loss of a Trigger's activation endpoint from loss of its capture route.
A trigger can record audio while a MIDI device supplies only its activation edge;
loss of that endpoint still needs recovery from the missing release. Conversely,
an unrelated endpoint refresh must not cancel valid captures merely because the
router closes/reopens all ports. Base cancellation on old/new validated configured
identity and actual successful connection results, not device indices alone or
inventory membership alone. Match tails to their frozen session sources independently
from the newest capture. Preserve an unaffected old tail on loss of only the new
session's source, and cancel affected tails if their delayed capture cannot finish.

Cancellation must remain pending when structural command/result queues are full;
coalescing must not turn queue rejection into acknowledged cancellation. Define
precedence against already-queued start/end/ditch and ensure the matching result
settles the Trigger exactly once. Prove stopped-audio device-loss handling too:
waiting for a callback acknowledgement after the device stops is not recovery.
Use the established reader-quiescence barrier before any direct stopped-path reset.

### D. Close demonstrated transition holes, without speculative redesign

Add lifecycle/invariant tests for the existing bounded structural queues, stale
results, rollback, ditch dispositions, punch failure/absent target, external removal,
history capacity, and reset/rig replacement barriers. Fix any failures in their own
focused commits. Do not add a second LoopTake state machine, generic synchronization
framework, arbitrary watchdog timeout, or broad timing/quantisation changes.

## Thread ownership and real-time review

For every changed/new shared value, record owner, readers, writers, synchronization,
and retirement. Trigger runtime state and edge handling remain audio-owned;
structural take creation/destruction, routing persistence, and variable-sized
history remain job/UI-owned. LoopTake owns its capture state and phase. Station
owns membership and presentation ordering; Scene remains wiring/orchestration.

Read `doc/realtime-audio.md` and the `threading-review` skill. Run its audit script
after edits, and inspect complete callback-owned bodies manually. The script scans
working-tree additions only, so also inspect each implementation commit's diff
and transitive callback callees. Reject new mutexes, blocking waits, allocations,
final-owner destruction, formatting/logging, unbounded retries, and traversal of
mutable job/UI containers. Document any existing concern separately and remediate
it when necessary for these lifecycle guarantees.

## Review and validation workflow

1. A fresh Sol reviewer adversarially reviews this plan before implementation,
   correcting unsafe assumptions, missing transaction/lifetime proofs, and scope.
2. A separate fresh Sol implementation coordinator implements the reviewed plan.
   It delegates separate fixes to implementation agents and uses independent
   reviewers as work progresses. Research agents use Luna; implementation/review
   agents use Sol. Bound concurrent workers to available slots and separate files
   or serialize edits where fixes share Trigger/Station/LoopTake.
3. Commit this plan separately, then each independent fix with its regression tests.
   Demonstrate a representative failing test before each fix where practical.
4. Before every build/test run, read `.vscode/tasks.json`. Use its absolute MSBuild
   command, incremental Build, affected projects, and one trailing SolutionDir slash.
   Follow `doc/build.md` for duplicate PATH environment/runtime DLL issues.
5. Run relevant native Trigger/Station/LoopTake/MIDI/audio/rig lifecycle tests, then
   broaden only when remaining concerns warrant it. No live hardware claim without
   an actual hardware run. Update this document with test counts, commands, commits,
   reviewer findings/resolutions, and remaining limits.
6. Completion requires implemented/tested priority and exclusivity, bounded confirmed
   cancellation, no unsafe thread access introduced, no callback lock/allocation
   regression, independent review closure, and a clean committed working tree.

## Implementation record

Sol adversarial plan review strengthened the plan with the evidenced shared-writer /
dropped-punch-out defect, separate B commit boundaries, once-per-block multichannel
envelope progression, completion idempotence/generation proof, presentation-caller
and snapshot lifetime auditing, queue-full cancellation persistence, and endpoint
identity/stopped-reader recovery requirements. Plan approved for implementation
subject to these requirements; this approval makes no claim that code is fixed.

Pending: implementation commits, regression results, independent code reviews, and
final thread/hot-path proof. Implementation must record the selected session resource
handoff, completion idempotence mechanism, and confirmed-disconnect transaction
boundary here; do not leave these decisions implicit in the final audit.

### Completed foundations

- `2863bd29`: reviewed plan committed before implementation.
- `5b83f928`: generation-bound, consume-once completion/update publication; dirty
  publication is consumed before commit and commit context preserves staged buffer
  selectors. UI synchronous completion/update jobs execute under the existing
  Scene mutex, serializing validation and mutation with structural cancellation.
  Cancelled takes reject further capture and playback from retained snapshots;
  MIDI playback flushes held notes once instead of replaying new notes. Destructive
  At this historical checkpoint, Station lifetime retirement remained pending;
  the subsequent committed retirement fix below closes that gap.
- `fbdf6c2d`: Station display derives from immutable membership and packed per-take
  logical mode/serial. Active modes have equal recency priority, then tails, then
  Default. The callback no longer scans mutable Station membership or publishes
  an independently computed display enum.

Independent Sol implementation review found and corrected inactive punch arming,
stale punch state resurrection, stale update jobs, cancelled audio/MIDI replay,
and the Scene completion/cancellation validation race. The combined incremental
Debug build passed 56 relevant tests (6 completion handoff, 17 Station display,
7 LoopTake staging, 11 Station staging, 15 Station MIDI instrument). A second
incremental build with capture-session work in progress passed 279 Trigger,
Station, LoopTake and MIDI regression tests. The callback admission foundation
has separately passed 3 tests, including controlled threaded admission/closure,
release publication and rejected-callback silence. At this historical checkpoint, these later fixes were still
uncommitted and undergoing independent review; passing tests do not establish
final lifecycle closure.

Presentation writers are structural job/UI owners serialized by Scene's existing
mutex. Rendering readers acquire the packed lock-free scalar and immutable weak
membership snapshot. Completion counters/flags cross callback-to-UI ownership
through lock-free atomics; emitted jobs carry the observed capture generation.
Resource reset/reuse requires stopped readers, and cancelled resources remain
stable through the subsequently implemented Station/session retirement integration.

No live hardware run or thread-sanitizer result is claimed. Historical required
work: independent review/commits of per-session envelopes and callback admission;
non-destructive Station removal; confirmed loss, stale-release/revision handling,
interruption/queue/retirement regressions; final integrated thread and hot-path
review, broad native tests, and a clean committed tree.

### Subsequent reviewed fixes

- `2b54b0bd`: independent per-capture prepared mixer/writer, frozen routes and
  strong target/source pins. Delayed commands retain their session token and
  mixer, new starts preserve older tails, and envelope advancement happens once
  per capture block across stereo destinations. Punch admission reserves its
  matching release's bounded queue capacity. Failed/removed sessions reclaim
  their slots only after audio acknowledgement and Station snapshot retirement.
  Root adversarial review found and fixed a dangling-else delayed-action error,
  a reusable-slot exhaustion defect and rollback backup double-counting. The
  rollback regression failed before its fix and passed afterwards; 293 focused
  native tests passed after these corrections.
- `e9dbd917`: callback admission gate proves engine-reader quiescence before an
  off-callback lifecycle pump. One bounded lock-free CAS admits the callback;
  its stack lease releases the active bit. Closing and observing a cleared
  active bit transfers control; rejected late callbacks only silence output.
  The gate does not prove driver/host destruction or permit changed stream
  geometry without the existing backend lifecycle barrier.
- `2a946990`: future-revision queue and fallback edges remain parked until that
  rig reaches the audio owner, instead of losing releases during a mid-block
  MIDI refresh. The existing saturation fixture now uses stale revision1 and
  accepted revision2, matching production's monotonic published revisions.
- `f4d7cf75`: Station ditch/reset cancels and removes takes while keeping audio
  buffers, writers and immutable snapshots intact. Removed GUI parent/receiver
  links are detached off callback to break strong ownership cycles; processing
  objects remain alive until acknowledged off-callback retirement. Tests cover
  unchanged borrowed buffers, retained take/rack/loop/model lifetimes, other
  active takes and zero-length ends. The independent Sol source-loss worker and
  root adversary reviewed these changes; 88 focused native tests passed.

The service rejected fresh reviewer spawns and completed-reviewer follow-ups with
`agent thread limit reached`, despite available active capacity. B1/A received a
fresh independent Sol code review; later foundations received independent root
adversarial review, and B3 also received an independent Sol peer review. This
limitation is recorded rather than claiming an extra fresh reviewer ran.

`5c5b87de` also retires discarded zero-length Record/Overdub sessions from both
job and audio histories after dropping their token-bound delayed borrowers.
Tests perform 96 discarded captures of each kind while an older tail survives,
then admit a valid capture and verify weak target retirement. The root adversary
reviewed acknowledgement ordering and the focused zero-length/rollback tests
passed (2/2).

A broader 404-test pass subsequently exposed five regressions in treating the
take's initial `INACTIVE` state as cancellation. Newly constructed or restored
takes can already contain playable audio/MIDI loops before a take capture
lifecycle begins. The follow-up uses a separate atomic cancellation flag,
initially false, published by cancellation and cleared only for valid
Record/Overdub initialization with the existing stopped-reader reuse contract.
Cancellation-only snapshot/job suppression and source validity queries use that
flag; actual live/tail priority still uses recording states. Regression coverage
includes restored sample output, MIDI note/held-note flush, phase corrections,
deferred restored quantisation and overdubbing from a restored source.

Further root adversarial review found a real rapid-END lifecycle gap: prepared
audio loops already existed in the structural back collection, but Play inspected
the not-yet-published front collection. END before a GUI commit therefore skipped
the audio tail and left the child's state Recording with logical length zero.
A subsequent Overdub could also drop unpublished source audio slots. Two new
regressions reproduced both failures before the fix. Structural Play/EndRecording
now initialize/finalize the authoritative prepared loops, and structural Overdub
uses the prepared source slot count. Callback readers continue using their
immutable published snapshot; no callback back-container access is added.
This correction is committed separately as `40d92bb8`.

Source mute ownership now uses a prepared, immutable control handle with a
lock-free atomic claim count. The callback owns transient punch claims and the
replacement target owns its persistent claim on the structural thread. Neither
claim changes performer mute flags or retains the source take. Child loop mixers
bind the handle before publication, preserving full bounce reads while fading
normal output. Mixer mute/level setters publish atomic desired values; only audio
sets/advances the fade target and publishes current level once per block. MIDI
claims use flush-only playback to release held notes once while suppressing new
notes. Tests verify actual stereo samples, independent concurrent claim owners,
idempotent replacement ownership and preserved performer mute.

Confirmed endpoint loss flushes only that device's held live notes. Its aggregate
snapshot is rebuilt from surviving named/anonymous sources; overlapping held
pitches are preserved. A fixed pending release bitmap retains NoteOffs when the
synthetic queue is full, with bounded structural-thread retries and suppression
when a surviving endpoint takes ownership. Reset and allowed-channel filtering
also maintain anonymous state. This mutex is structural-only; the coarse audit
script flags its containing Station file, so callback caller review is required.

### Explicit upstream boundary and hardware evidence

The user explicitly prohibits editing upstream RtAudio source. All remediation
must remain in Jamma wrappers/engine code. Runtime ASIO inventory polling using a
new RtAudio instance is prohibited here: the vendored ASIO implementation shares
global driver state and constructor discovery can remove the active driver.
Heartbeat inactivity can request a recovery handoff, but it never proves reader
quiescence or confirms physical source loss. The admission gate supplies reader
proof; successful actual connection reports or accepted stream replacement supply
source evidence. A driver silently stopping without an exposed confirmed-loss
notification cannot be identified reliably through the available public API.
No claim of universal silent ASIO unplug detection is made. Existing backend
Stop/Pause behaviour is outside this upstream-constrained remediation; confirmed
MIDI loss recovery must not rely on those calls returning.

### Shared state ownership and retirement

Audio ownership below means the normal admitted callback, or the serialized job
owner only after closing admission and observing no admitted reader. It never
means two concurrent owners. Structural operations hold the existing Scene
serialization lock; locks in these operations do not run in callback callees.

| State | Writers / owner | Readers | Synchronization | Retirement / reset |
| --- | --- | --- | --- | --- |
| GuiElement `_changesMade` | Callback and structural producers set dirty; commit owner consumes first | Structural commit and staged-buffer selectors | Atomic consume-before-work exchange | Owning element remains pinned by existing hierarchy/snapshots |
| GuiElement `_committingChanges` | Structural commit owner | Structural staged-buffer selectors | Atomic bool preserves staged selection during commit | Cleared after commit; callback never selects mutable back containers |
| Take `_captureGeneration`, `_completionPublishedGeneration`, `_pendingCompletionGeneration` | Structural lifecycle changes generation; audio publishes once for observed generation; commit consumes pending | Audio and structural completion validator | Lock-free generation atomics and tagged JobAction value | Cancellation invalidates generation; valid reuse requires stopped readers |
| Take `_captureCancelled` | Serialized lifecycle owner | Audio suppression, structural update validator and source validity query | Atomic release/acquire | Initial false allows restored contents; cancellation true; valid Record/Overdub clears under reuse contract |
| Take `_loopsNeedUpdating`, recording/tail counters and `_endRecordSamps` | Audio increments/publishes; structural lifecycle initializes and consumes | Audio threshold logic and structural/model readers | Atomic scalar values, consume-first dirty exchange | Each take has independent counters; no shared reset across overlapping captures |
| Take packed `_presentation` | Structural/UI presentation changes under Scene serialization | UI visual reduction | Atomic packed mode/serial | Cancelled/ended take publishes inactive/tail disposition without model writes from audio |
| Station `_nextPresentationSerial` | Structural owner only | Same structural owner | Existing Scene serialization | Belongs to Station lifetime; callback does not access it |
| Take structural `_backLoops` | Existing structural owner | Structural Play/EndRecording/Overdub preparation | Existing Scene serialization | Prepared material is initialized before publication; callback still reads immutable front snapshots |
| Station published membership and `AudioState::OwnedLoopTakes` | Structural snapshot publisher | UI reduction and admitted audio | Existing immutable published snapshots | Strong take pins retired only after completed audio generation, off callback |
| Station `_anonymousLiveHeldMidi`, `_pendingLostLiveMidiNoteOffs` | Structural MIDI producer/retry owner | Same structural producer | Existing held-note mutex, off callback only | Reset clears; channel filtering clears anonymous notes; successful queue publication or surviving ownership clears pending bits |
| Trigger prepared `_captureSessions` slots/count | Job owner allocates, resolves pins and reclaims | Audio borrows stable published session pointers | SPSC structural result publishes immutable payload; slot retirement release/acquire | Audio drops all history/delay borrowers before retirement; final source/target/mixer destruction stays off callback |
| Session frozen receiver, routes, epoch, device name, source/target pins and writer/mixer | Job owner initializes before publication | Audio/history/action processing | Immutable after SPSC publication | Retained until audio token retirement and acknowledged target snapshot retirement |
| Session `RetiredByAudio`, `PublishedToAudio`, `CancellationRequested` | Audio acknowledges retirement/publication; job may retire a never-published failed start and requests cancellation | Counterpart owner | Lock-free atomic flags | Retirement cannot depend on callback-owned arrays read by job; cancellation survives queue saturation |
| Session `SourceMuteClaimHeld` | Audio owner only | Same audio owner | Single-owner invariant | Release once before token retirement or stopped-reader Reset; no structural unsynchronized access |
| Trigger runtime history, delayed mixer/punch arrays, rollback backups and active index | Audio owner only | Same audio owner | Fixed bounded arrays and token identity | Erase only affected token; backup counts cleared before retirement; no allocation or reference-count destruction in callback |
| Trigger structural commands/results, CaptureGeneration/InputGeneration and disposition | Producer copies values into existing SPSC queue | Opposite owner consumes | Release/acquire queue publication | Queue overflow retains cancellation/release work for retry; payload pins originate from job-owned sessions |
| Trigger `_sourceLossScanRequested` | Structural producer, audio retry publisher | Audio owner | Atomic consume-before-scan exchange | Cleared only by audio consumption; bounded scan runs only when announced |
| Trigger `_inputLossGeneration`, `_consumedInputLossGeneration`, `_publishedInputLossGeneration` | Structural producer increments; audio resets bindings and publishes consumption | Input producers, audio and job START validator | Atomic shared generations; consumed value is audio-only | Commands bind originating audio generation; stale physical releases cannot affect a later lifecycle |
| Trigger `_publishedLiveSessionToken`, `_publishedCapturePending` | Audio publishes live token; job publishes pending capture aggregate | Job loss filter and audio routing guards | Lock-free atomic scalar publications | Old-tail-only loss preserves unaffected live token/raw input; no live mode reset for another token |
| Trigger `_activationReleaseRequested`, `_activationReleaseToken` | Structural loss producer | Audio owner | Atomic payload followed by release signal | Consumed against matching token; confirmed missing endpoint cancels its live/pending capture |
| Input queue/fallback `InputGeneration` and revision | Separate UI/job input producers | Audio owner | Existing SPSC queues and atomic fallback seqlock | Future revisions remain parked; past generations/revisions are discarded |
| Trigger confirmed MIDI names/availability and audio channels/device/epoch | Structural owner only | Structural start validation/preparation | Existing owner serialization; no callback reads of strings/vectors | Inventory errors preserve prior knowledge; valid connection delta or accepted stream replacement updates it |
| Bounce writer prepared capture-block offset | Audio owner only | Same writer's block writes | Single owner; begin/end hooks once per capture block | Writer pinned by its session/target; independent old/new envelopes preserve multichannel tails |
| Source control transient and replacement counts | Audio owns transient claims; structural targets own replacement claims | Mixer callback and UI effective mute queries | Lock-free atomic counts | Each owner releases exactly once; control handle has no strong source-take cycle |
| Source control replacement revision/audition flag | Structural performer/replacement owner | Mixer callback and UI | Lock-free atomic scalar publication | Explicit UnMute auditions current replacements; new replacement reasserts mute; transient claims always win |
| Take `_captureSourceMuteControl`, `_replacementSourceMuteControl` | Initial control immutable; replacement handle structural-only | Audio reads immutable control; structural owns replacement handle | Prepared immutable binding plus atomic control payload | Cancel/destructor releases replacement handle off callback without retaining source take |
| Mixer `_replacementAuditionRevision`, `_unmutedFadeTarget` | Structural performer/level setters | Audio fade target calculation and UI | Lock-free atomic desired values | Individual Loop UnMute only auditions current replacement revision; later replacement invalidates it |
| Mixer `_appliedFadeTarget` and fade interpolation state | Audio owner only after construction | Audio owner only | Single owner, no target writes from UI/job | Construction initializes before publication; writer/session pins preserve lifetime |
| Mixer `_publishedFadeLevel` | Audio publishes once per block/Offset | UI and other callback mix stages | Lock-free atomic double | No UI reads of mutable interpolation `Current()` remain in AudioMixer |
| AudioHost `_callbackAdmission` and stack CallbackLease | Callback enters/leaves; serialized lifecycle owner closes/reopens | Callback and lifecycle job | Single bounded admission CAS, release exit, acquire quiescent check | Engine ownership transfers only at closed/no-active state; external target references never block reopen |
| AudioHost `_callbackOutputChannels` | Lifecycle owner after successful backend stop barrier | Rejected callback silence branch | Atomic scalar | Old geometry retained on unchanged stop failure; gate alone is not driver teardown proof |
| AudioHost immutable `_publishedStreamParams` and `_lastInitReplacedStream` | Serialized lifecycle owner | Off-callback metadata clients; outcome read by same lifecycle owner | Atomic immutable metadata; outcome owner-only | Failed stop retains prior identities; successful teardown then failed open confirms actual old-stream loss; callback uses its passed stable params reference |
| Scene heartbeat observation, `_sourceLossRecoveryGateClosed`, `_audioStreamEpoch` | Serialized job/lifecycle owner | Same owner | Existing Scene lock; heartbeat is atomic hint only | Recovery acknowledges Station generations only under proven quiescence, then reopens independently of arbitrary external pins |
| Restored/replaced history pins | Structural restoration/preparation owner before publication | Subsequent audio token history | Explicit strong pins resolved from each retained receiver's immutable Station membership | Receiver identity preserved; reset/removal retirement follows the same acknowledged session path |
| Rig routing update `ReceiverChanged` | Structural candidate preparation derives stable resolved StationIndex difference | Audio boundary readiness/apply checks | Immutable value in prepared RigSnapshot | Retired with rig snapshot; safe future source-route edits remain permitted during frozen tails, destructive receiver changes remain busy-rejected |

The automated hotpath audit reports coarse file-level matches for new locks in
Scene confirmed-connection/recovery/initialization methods and Station scoped
held-note methods. Manual caller review places all of them off callback. The
review must cover the complete remediation range, including transitive mixer,
bounce, token delay, MIDI flush and rejected-callback silence callees; a script
match is not treated as a callback lock, and the script is not reported clean.

### Final completion record

All remediation steps are implemented and committed. Earlier pending statements
above are historical checkpoints, not outstanding work.

- `e83c3d49`: distinguish explicit cancellation from restored INACTIVE playback.
- `09613363`: independently owned source mute claims and callback-owned fades.
  Performer Take UnMute auditions existing replacement-muted originals; individual
  Loop UnMute uses a revision-bound override. A later replacement reasserts mute;
  neither override defeats another capture's transient punch claim. Effective
  mute picking follows the displayed state.
- `40d92bb8`: structural playback/end/overdub use prepared loop material before
  GUI publication. Both rapid-start/end regressions reproduced failure before the
  fix and passed after it; callback membership remains immutable.
- `faa9ff83`: confirmed MIDI connection deltas preserve transient refreshes.
- `5199f1bf`: scoped MIDI release preserves unrelated held notes, retries saturated
  synthetic queues with a fixed bitmap, and drains retired target playback notes.
- `e34dfa48`: integrates confirmed source/activation loss, sticky token cancellation,
  frozen receiver/source sessions, restored history, stale input generation checks,
  bounded stopped-callback recovery and serialized shutdown. Cancellation marked
  while capture is pending remains sticky even if completion wins the next turn;
  already-completed history is excluded when confirmation is first published.
  Old-tail cancellation preserves newer live input and its real release. Safe
  future capture route edits are permitted while frozen tails drain; destructive
  receiver replacement/removal is rejected while owned captures remain pending.

Wrapper TryStop failure preserves the old driver geometry and does not invent
confirmed source loss. Successful teardown followed by failed opening is a
replacement/loss outcome. Recovery uses closed admission plus an actual zero
admitted-reader observation, never a heartbeat or backend state as memory proof.
External resource pins do not prevent callback admission reopening. No upstream
or vendored RtAudio file was modified.

Final incremental Debug x64 validation passed **1366/1366 native tests across
164 suites**, including actual multichannel fade samples, MIDI sink releases,
controlled concurrency, queue saturation, repeated zero-length captures, restored
64-take history, pending cancellation, rapid GUI-delayed publication and safe
routing edits. Jamma and JammaConsole incremental builds also passed. Root
independently verified the complete native and application output.

The threading-review audit was run. Its coarse added-lock scan reports seven
locks in Scene structural connection/recovery/init/shutdown and Station structural
MIDI ledger/retry/retirement methods. These are off-callback false positives, not
a clean script result. Independent manual caller and allocation review covered
`c91401e3`'s parent through the final remediation tree: new preparation/metadata
allocations and locks are off callback; admitted audio adds bounded fixed-array
work and lock-free scalar operations, with no new mutex, wait, allocation or
metadata string/shared-snapshot load in the audio path. Interpolation mutation
has one audio owner and published scalar readers.

Fresh Sol plan and completion/handoff adversaries, separate implementation
subagents, the independent Sol Station-retirement peer review, and root's vigorous
per-fix and integration reviews found and corrected concrete issues recorded
above. Additional fresh final reviewer attempts were rejected by the agent service
thread limit; this is an evidence limit, not a claimed completed review.
No live hardware run or thread-sanitizer run is claimed. Silent ASIO unplug remains
unconfirmable where the public backend API supplies no loss notification; the
engine cannot forcibly finish a hung admitted callback. Existing overall backend
shutdown Stop/Pause blocking behaviour is not changed upstream. These limits do
not weaken the tested ownership, token, snapshot and admission invariants.
