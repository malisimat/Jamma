# Trigger and Station state remediation plan

Status: research and independent Sol adversarial plan review complete; implementation and validation pending.

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
