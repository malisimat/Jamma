# Trigger-centric dynamic routing implementation plan

## Purpose

Finish the editable-routing work so Trigger is the sole owner of trigger state
and take history, while Station remains the owner of `Station -> LoopTake ->
Loop` audio state. A Station must not retain, traverse, tick, or query the
Triggers that happen to target it.

The implementation must support, in one running Scene and without restarting
audio, this exact sequence:

1. Trigger T targets Station A with ADC input `{0}`; record take A1.
2. Edit T to ADC inputs `{0, 1}`; record take A2.
3. Edit T to ADC input `{0}` and target Station B; record take B1.
4. Ditch three times; remove B1, A2, and A1 in that order.

Each action must use the receiver and globally unique take IDs captured when
that history entry was created. No old input event may execute against a new
route. No callback-owned function may allocate, lock, wait, log, or destroy a
last owner.

This plan supersedes the Station-trigger-membership portions of
`doc/plan.md`. Update that document when this work lands so it no longer
describes Station membership as the publication mechanism.

## Baseline and corrected review conclusions

The current branch is `feature/editable-hud-connections` at `311a83d`. There
are 14 staged files containing the first dynamic-route/history implementation.
That index builds and the current native suite passes (901 passed, one
hardware-dependent test skipped), but it is not the finished architecture.

Keep these useful parts of the staged work:

- reuse a Trigger instance for capture-route and station-target changes;
- retain `{sourceTakeId, targetTakeId, receiver}` in each Trigger history
  entry;
- send end, punch, overdub, and ditch actions to the recorded receiver;
- prepare vectors and mixer behaviour off-thread and exchange only at an audio
  boundary;
- distinguish replacement/removal audio-boundary transition from an idle capture-route edit.

Correct or replace these parts:

- remove `Station::TriggerMembership` entirely;
- do not mutate every retained Trigger when applying an unrelated revision;
- replace positional Trigger reuse with stable Trigger identity;
- make all mutable Trigger state audio-thread-owned;
- replace the current atomic gate flag with a real producer barrier;
- cover HUD, keyboard, MIDI, and serial with the same revisioned ingress path;
- distinguish an active start result from merely having older history;
- pop history only after a defined successful/already-absent ditch result;
- replace the mock-only routing-history test with real publication and Station
  integration coverage, including the final input removal `{0,1} -> {0}`.

The earlier review concern that every completed overdub necessarily stops
bouncing after a station move was too broad: route edits are rejected while
the Trigger is actively overdubbing. Nevertheless, Station-side discovery of
bounce pairs through Trigger membership is reverse ownership and must be
removed. Preserve any recording-tail behaviour with explicit tests.

## Architectural invariants

1. **Forward ownership only**
   - RigSnapshot owns the immutable routing graph and dispatch tables.
   - Trigger owns activation state, debounce state, delayed actions, current
     capture route, and LIFO take history.
   - Station owns its ordered LoopTakes and resolves take IDs locally.
   - LoopTake owns its active source/target processing relationship.

2. **Trigger state has one writer**
   - Only the audio thread may mutate Trigger bindings/held state, state
     machine fields, history, delayed actions, receiver, capture vectors, or
     overdub behaviour.
   - UI/job/input threads enqueue fixed-size input values and read published
     atomics only.

3. **No reverse Trigger collection on Station**
   - Station never stores a Trigger vector.
   - Station never ticks or dispatches input to Triggers.
   - Station never asks a Trigger for history or capture configuration.

4. **Revision coherence**
   - Every producer submits against the revision whose immutable dispatch
     entry or HUD widget it used.
   - Closing ingress is a producer barrier, not merely a flag store.
   - A stale revision is rejected before enqueue and checked again by the
     audio consumer as defense in depth.

5. **History stability**
   - A history entry is immutable after creation and contains stable take IDs
     plus a strong receiver reference.
   - Route edits change only the current route. They never rewrite history.
   - Ditch is LIFO and targets the receiver captured by that entry.

6. **Real-time safety**
   - Audio consumes fixed-capacity rings and prebuilt snapshots only.
   - Input producers and audio use bounded SPSC queues and atomics; no new
     mutex is introduced anywhere in Trigger ingress.
   - Snapshot and staged object destruction remains off the callback through
     retained ownership and explicit retirement.

## Target runtime model

### Stable Trigger identity

Add a persisted stable ID to `io::RigFile::Trigger` in
`JammaLib/src/io/RigFile.h`, serialized by `RigFile::ToJsonStream` and parsed
optionally for backward compatibility.

Recommended representation:

```cpp
std::string Id; // GUID generated off the audio thread
```

Rules:

- IDs are non-empty and unique in a normalized candidate.
- Existing rigs missing IDs receive GUIDs during initial normalization using
  `utils::GetGuid()`; combine this with any existing station-target migration
  into one candidate/save operation.
- New HUD Triggers receive an ID when the candidate is created.
- Copy, cable edit, reorder, and deletion preserve IDs.
- `RigCoordinator::_BuildSnapshot` matches accepted and candidate Triggers by
  ID, never by vector position, name, or equivalent configuration.
- Duplicate IDs fail validation. A changed activation definition for the same
  ID creates a replacement instance and requires full replacement
  audio-boundary transition. Capture/receiver-only changes retain the instance.
- Candidate order remains display order only.

Add focused parse/serialize/migration/duplicate/reorder tests before relying
on IDs in publication logic.

### RigSnapshot

Keep `RigSnapshot::Triggers` as the complete ordered runtime Trigger list and
`RigInputDispatch` as the only input-routing authority. Remove:

- `StationTriggerMembership`;
- `RigSnapshot::StationMemberships`;
- all construction/publication of Station trigger vectors.

Each `RigSnapshotTrigger` should contain:

- stable Trigger ID and rig/display index;
- `shared_ptr<Trigger> Instance`;
- resolved station index for display/diagnostics;
- staged receiver/capture values used only for a retained Trigger whose route
  changes;
- no mutable Station membership.

Represent affected work by stable ID or by a pre-resolved pair of accepted and
candidate indices. Do not assume the same numerical index after reorder or
deletion. Prefer explicit records such as:

```cpp
struct TriggerRouteUpdate {
    std::shared_ptr<Trigger> Instance;
    std::size_t CandidateIndex;
};

struct TriggerReplacementCheck {
    std::shared_ptr<Trigger> AcceptedInstance;
};
```

This makes audio-boundary transition and application independent of vector position.

### Trigger history and active action

Rename `TriggerTake` to `TriggerTakeHistoryEntry` if doing so keeps the diff
clear. Its essential value is:

```cpp
SourceType SourceType;
std::string SourceTakeId;
std::string TargetTakeId;
std::shared_ptr<base::ActionReceiver> Receiver;
```

Keep strings because Station and LoopTake IDs are already GUID-based
(`Station::AddTake` and `LoopTake::AddLoop`). Do not use a current Station
index as historical identity.

Add explicit audio-owned active-action state, such as an optional history
index or active target record. `EndRecording` and `EndOverdub` must act only on
the start they successfully initiated; they must not fall back to an older
history entry when the current receiver is absent or rejects the start.

Define ditch outcomes explicitly. Recommended policy:

- `Removed`: pop history;
- `AlreadyAbsent`: pop history, allowing safe pruning after external removal;
- `Failed` or receiver unavailable: retain history and report failure.

Do not unconditionally pop after dispatch. Apply source unmute and target
removal through the same recorded receiver and make the result testable.

### Station/LoopTake overdub processing

Remove Trigger history discovery from `Station::OnBounce`. The simplest
ownership-correct shape is:

- when Station handles `TRIGGER_OVERDUB_START`, it already knows the source
  LoopTake, creates the target LoopTake, and receives the active Trigger/mix
  processor;
- store the active source relationship with the target LoopTake (a weak source
  LoopTake plus a weak Trigger or a narrow non-owning bounce-writer interface);
- `Station::OnBounce` iterates its own LoopTakes and asks each active target to
  process its own source relationship;
- clear the active relationship when the target no longer needs bounce/tail
  processing;
- Trigger retains the authoritative undo/ditch history; the LoopTake linkage
  is only current audio-processing state, not history.

Prefer a narrow `BounceWriter` interface over making LoopTake depend on all of
Trigger, if that interface stays surgical. It needs only the existing
`Trigger::WriteBlock` behaviour. Do not move transport or take state into
Trigger merely to remove membership.

This keeps `Station -> LoopTake -> Loop` responsible for audio processing and
allows Station to remain ignorant of which Trigger created the take.

### Trigger ticking

Tick every Trigger exactly once from the audio-applied RigSnapshot. Do not use
`RigCoordinator::Accepted()` inside the callback because UI promotion lags the
audio acknowledgement.

Recommended placement:

1. `AudioHost::ApplyPendingRigSnapshotAtAudioBoundary()` acquires/adopts the
   current applied snapshot.
2. At the existing end-of-block tick point, AudioHost iterates that applied
   snapshot's Trigger instances and calls `Trigger::OnTick` once.
3. Scene's tick callback continues to tick Stations for station/take visual
   tail work, but `Station::OnTick` no longer traverses Triggers.
4. Publish Trigger audio-boundary transition only after Trigger queues and delayed work have
   been processed for that boundary.

Retain an audio-owned reference to the applied snapshot. Coordinator/AudioHost
retention must guarantee that assigning or dropping the callback-local handle
cannot destroy the last snapshot or Trigger reference on audio.

## Input ingress and producer barrier

### Why the current code is unsafe

`Trigger::OnEvent` currently mutates `DualBinding`, debounce fields, Trigger
state, and sometimes history from UI/job input paths, while `Trigger::OnTick`
runs on audio. HUD alone uses a ring. The ring resembles SPSC, but HUD,
keyboard, MIDI, and serial are distinct possible producers. Finally,
`MidiRouter::GateRigTriggerInput` is only an atomic flag; a producer can pass
its check and enqueue after audio has acknowledged audio-boundary transition.

### Chosen design: reuse the existing SPSC lanes and add only the missing boundary

**Hard constraint: the audio thread never acquires, tries, polls, or otherwise
touches a mutex. Input producers also do not contend on a new Trigger-ingress
mutex.** Normal input delivery is only revision checks plus a bounded SPSC
push.

The current topology has two semantic Trigger producer domains:

- **UI producer:** HUD pedals and keyboard events;
- **job/input producer:** MIDI and serial events after their existing hardware
  ingress is drained by `Scene::OnJobTick`.

Retain and upgrade the existing per-Trigger `_externalControlActionQueue` as
the UI-to-audio edge queue. It already has the correct single UI producer and
single audio consumer topology; do not add a second UI queue. Expand its
fixed-size payload with the rig revision, binding/control identity, edge, and
event timestamp, and add the bounded `Peek`/drain operations needed to merge
it with job input. HUD and keyboard may share it only after confirming that
both are dispatched exclusively by the same UI event thread.

Also retain the existing `MidiInputEndpoint::Ingress` queue for the raw
RtMidi-callback-to-job-thread hop. There is one such SPSC queue per MIDI input,
and it already stamps events with the observed rig revision. Do not introduce
another raw MIDI ingress queue.

That existing MIDI queue cannot also carry the final edge to audio: its sole
consumer is `MidiRouter::PumpMidi` on the Scene job thread, which must continue
to perform non-audio-safe station MIDI recording, automation, routing, and
diagnostics. Giving the queue a second audio-thread consumer would violate its
SPSC contract, while moving the whole pump onto audio would expand the hot
path substantially. After consuming raw MIDI (or serial) input, the job thread
therefore translates matching input to a compact edge and pushes it to the
one missing queue: a fixed-capacity per-Trigger job-to-audio SPSC queue.

The resulting per-Trigger ingress is therefore one reused UI queue plus one
new job queue:

```cpp
struct TriggerInputQueues {
    ExistingUiActionQueue<TriggerInputEdge> Ui; // upgraded in place
    SpscQueue<64, TriggerInputEdge> Job;
};
```

Use the established fixed-array/acquire-release/drop-newest semantics from
`midi::MidiQueue` for the new job queue. If extracting that generic template
to `base::SpscQueue` would make the diff broader than keeping an engine-local
equivalent, keep the change local; do not churn all existing MIDI call sites
merely to rename a proven queue implementation.

Audio drains both queues without locks. Compare their head timestamps and
consume in deterministic event-time order, bounded by the combined capacities
per block. Use a fixed UI-before-job tie break when timestamps are equal.
Never allocate while merging.

Before implementation, verify and document that all HUD/keyboard calls occur
on one UI event thread and all semantic MIDI/serial Trigger dispatch occurs on
the one Scene job thread. Add debug-only producer-thread assertions where the
platform supplies a stable thread ID. If another producer domain is found,
give it another SPSC queue; do not silently turn a queue into MPSC.

### Asynchronous producer acknowledgement

Use a stable atomic `RigTriggerInputGate` containing:

- current open revision;
- requested closed revision;
- UI producer acknowledged revision;
- job producer acknowledged revision;
- permanent shutdown-closed state.

Closing is asynchronous and never blocks a producer or audio:

1. The UI routing-edit handler finishes its current event dispatch, stores
   `requestedClosedRevision = N`, and acknowledges the UI domain for N. Since
   it is the sole UI producer, every earlier UI push is now visible in its
   SPSC queue.
2. At the top of `Scene::OnJobTick`, before MIDI/serial Trigger dispatch, the
   job producer observes the request. After any already-entered job dispatch
   has completed, it stops accepting revision N and publishes its
   acknowledgement for N.
3. `_AdvanceRigPublication` polls the two acknowledgement atomics on the job
   thread. It publishes the audio-boundary transition request only when both equal N.
4. Until then audio continues normally; it never waits or reads producer
   acknowledgement state.
5. Once the request is published, no producer can add another revision-N
   edge. Audio drains both queues and evaluates audio-boundary transition normally.

On rejection or persistence failure, reopen accepted revision N using one
coherent gate publication. On success, publish candidate input dispatch and
new HUD widgets first, then open revision N+1 last. On shutdown, request
permanent closure and receive both producer acknowledgements off audio before
stopping and releasing readers.

Keep the revision in every queued edge. Old HUD widgets or retained input
snapshots are harmless because their revision no longer equals the open
revision, and audio discards mismatched entries as defense in depth.

### Fixed-size input values

All mutable binding/held/debounce state moves to the audio-owned Trigger. Do
not enqueue strings, vectors, shared pointers, or `base::Action` objects.

Build immutable dispatch routes off-thread which translate raw input into a
compact value such as:

```cpp
struct TriggerInputEdge {
    std::uint64_t RigRevision;
    std::uint16_t BindingIndex; // or DirectHudBinding
    TriggerControl Control;     // Activate or Ditch
    TriggerEdge Edge;           // Down or Up
    std::int64_t EventTimeUsec;
};
```

Pre-resolve device names to immutable route/device tokens in
`RigInputDispatch`. Preserve per-binding held/repeat semantics on the audio
thread by retaining a binding index in the edge. The audio consumer owns all
binding state and performs debounce/state-machine transitions.

Drain a bounded number no greater than the combined queue capacities each
block. Document and test overflow policy and per-domain atomic diagnostic
counters. Dropping newest is
consistent with the current bounded queues, but make release-loss behaviour
observable; if tests show a held control can stick, publish a compact latest
control-state fallback rather than allocating or blocking.

Existing callers that need immediate `ActionResult` should receive an
"accepted for audio processing" result, not pretend the state transition has
already happened. If Scene reset/quantisation logic requires actual outcomes,
publish monotonic activation/ditch outcome counters from audio and consume
them on the job thread.

## Revision publication protocol

Use one protocol for every edit:

1. **Build:** coordinator normalizes/validates the candidate and builds all
   snapshots, dispatch routes, replacement Triggers, vectors, and mixer
   behaviour off audio.
2. **Close ingress:** the UI requests closure for `acceptedRevision`,
   acknowledges its own producer domain, and leaves the candidate waiting for
   producer acknowledgements.
3. **Acknowledge producers:** the job thread stops old-revision MIDI/serial
   dispatch and acknowledges. Only after both domain acknowledgements match
   does Scene publish candidate/accepted handles and the audio-boundary transition request to
   AudioHost.
4. **Drain and decide:** on a later audio boundary, tick the applied Triggers,
   drain old-revision edges, and check only the affected accepted instances:
   - retained route change: idle, no held edge, no delayed action;
   - replacement/removal: the stronger predicate, including no history.
5. **Reject:** clear the request, retain accepted state, and reopen the old
   revision. Do not save or publish candidate state.
6. **Persist:** after successful audio acknowledgement, persist the complete
   candidate. On failure, publish/carry out cancellation and reopen only after
   audio has acknowledged it is still using the accepted revision.
7. **Publish pending:** publish the complete pending RigSnapshot.
8. **Apply on audio:** at block start, apply only
   `TriggerRouteUpdate` records. New/replacement Triggers were already
   fully configured; unchanged retained Triggers are untouched. Adopt the new
   applied snapshot/revision.
9. **Publish input:** after the audio acknowledgement, swap the complete input
   dispatch snapshot.
10. **Promote and retire:** promote displayed/accepted state after audio and
    input acknowledgements. Retire old snapshots off audio.
11. **Open ingress:** call `Open(candidateRevision)` only after the candidate
    input dispatch is visible. Old-revision producers remain rejected.

Remove or stop using the all-Trigger `AudioHost::_routingEditsEligible` scan.
Eligibility is candidate-specific and belongs in the affected-instance lists.
An unrelated Trigger with history or an active loop must not block another
Trigger's capture edit.

On shutdown: request permanent ingress closure, receive both producer
acknowledgements off audio, publish empty input dispatch, stop/join input
producers, stop audio, then release snapshots/Triggers. Preserve the existing
repeatable `CloseAudio`/`Shutdown` behaviour.

## Shared-state ownership table

| State | Writer | Readers | Synchronization | Teardown |
| --- | --- | --- | --- | --- |
| RigSnapshot/config/dispatch | Coordinator/job | Audio, input, HUD | Immutable `shared_ptr` publication with release/acquire | Retire off audio after both acknowledgements |
| Audio-applied RigSnapshot handle | Audio boundary | Audio callback only | Audio-thread-owned handle; published revision atomic for observers | Clear only after audio stops; retained refs prevent last-release on callback |
| Trigger operational state/history/current route/delays | Audio | Audio; UI reads published summaries only | Single writer; published scalar atomics | Snapshot retirement after producers/audio stop |
| UI Trigger input queue | Sole UI event thread | Audio sole consumer | Fixed-storage SPSC acquire/release atomics | Close/ack UI producer, stop audio, then destroy |
| Job Trigger input queue | Sole Scene job thread | Audio sole consumer | Fixed-storage SPSC acquire/release atomics | Close/ack job producer, stop audio, then destroy |
| RigTriggerInputGate | UI initiates close/open; each producer writes only its own acknowledgement | UI and job producers; Scene publication state machine | Revision/ack atomics; audio has no dependency on this gate | Permanently close and receive both acknowledgements before stopping readers |
| Trigger UI state and outcome counters | Audio | UI/job | Release stores/acquire loads | Trigger lifetime |
| Pending/audio-boundary transition request handles | Scene/job | Audio | Atomic `shared_ptr` and revision atomics | Clear off audio after acknowledgement/shutdown |
| Staged capture vectors/behaviour | Coordinator/job builds; audio exchanges once | Audio during apply | Candidate-owned, unpublished mutable staging with exclusive phase ownership | Old staged values destroyed through retired snapshot off audio |
| Station LoopTake snapshot/state | Existing Station owner/audio boundary | Audio/UI according to existing APIs | Existing published immutable snapshot pattern | Existing Station shutdown order |

Any implementation that adds shared state must extend this table with owner,
readers, writers, primitive, and teardown path before code is accepted.

## Removal map

Remove or rewrite these reverse dependencies:

- `Station.h`: `TriggerMembership`, `PublishTriggerMembership`,
  `TriggerMembershipSnapshot`, and `_triggerMembership`.
- `Station.cpp`: membership construction/reset, `OnTriggerEvent` traversal,
  Trigger traversal in `OnTick`, Trigger history traversal in `OnBounce`, and
  `AcceptsLiveMidiFromDevice`.
- `RigSnapshot.h`: `StationTriggerMembership` and `StationMemberships`.
- `RigCoordinator.cpp`: membership allocation/population/publication.
- `AudioHost.cpp`: Station membership publication.
- `Scene.cpp`: the no-snapshot keyboard fallback through Stations.
- tests and `test/JammaLib_Tests/src/TestRigMembership.h` after all users move
  to direct snapshot/Trigger helpers.

Keep `Station::AcceptsLiveMidiChannel`; channel eligibility is Station state.
Live-MIDI device recipients are already a precomputed RigInputDispatch value.

## One-session implementation phases and commits

Do not return to the user between phases. Continue until the complete suite,
app build, audit, and final review pass, or until a genuine authority/external
blocker is reached.

### Phase 0: preserve and baseline the current work

1. Read `.vscode/tasks.json` and `doc/build.md` again.
2. Record `git status --short`, branch, and staged diff summary.
3. Run the incremental native test build and full test suite.
4. Commit the current staged index intact as a recoverable, green groundwork
   checkpoint, e.g. `Preserve trigger take history across route edits`.
   It is not the final architecture, but committing avoids losing reviewed
   work and leaves every later phase bisectable. Do not push yet.
5. If the baseline unexpectedly fails, do not commit. Save a non-destructive
   recovery stash object or patch and diagnose first.

Do not amend or reset existing user commits. Do not squash automatically at
the end; offer an optional manual squash/rebase only after the final branch is
green.

### Phase 1: stable identity and candidate classification

Files likely involved:

- `JammaLib/src/io/RigFile.h/.cpp`
- `JammaLib/src/engine/RigCoordinator.h/.cpp`
- `JammaLib/src/engine/RigSnapshot.h`
- HUD candidate mutation helpers
- Rig serialization/resolver tests

Work:

- add/migrate/serialize stable Trigger IDs;
- match accepted/candidate Triggers by ID;
- replace positional changed-index logic with explicit instance change
  records;
- preserve candidate display order separately;
- prove add/delete/reorder/duplicate/replacement classification with tests.

Commit only after filtered and full relevant tests pass:

`Use stable trigger IDs for runtime routing edits`

### Phase 2: audio-owned Trigger ingress and real gate barrier

Files likely involved:

- `Trigger.h/.cpp`
- `RigSnapshot.h`, `RigCoordinator.cpp`
- `MidiRouter.h/.cpp`
- `Scene.h/.cpp`
- `GuiHud.h/.cpp`
- new small gate type in `engine/` or `midi/` according to existing ownership

Work:

- upgrade the existing `_externalControlActionQueue` in place as the UI edge
  queue, retain each MIDI input's existing raw ingress SPSC, and add only the
  missing per-Trigger job-to-audio SPSC;
- introduce atomic `RigTriggerInputGate` acknowledgements;
- build immutable raw-input-to-edge dispatch routes;
- route HUD/keyboard/MIDI/serial through `TryEnqueue` with revision;
- make Trigger state machine and binding state audio-only;
- add queue overflow/outcome publication as needed;
- integrate close/reopen/close-forever into edit failure, success, and shutdown;
- add deterministic gate-acknowledgement and stale-widget/input tests using
  latches/atomics, never sleeps;
- prove the single-writer assumption for both producer domains at every call
  site.

Audit the audio-facing queue code and demonstrate it contains only bounded
array access and atomics—no mutex, lock, wait, spin, callback, or allocation.

Commit:

`Serialize trigger ingress before audio state transitions`

### Phase 3: tick from snapshot and remove Station Trigger membership

Files likely involved:

- `AudioHost.h/.cpp`
- `Scene.cpp`
- `Station.h/.cpp`
- `RigSnapshot.h`, `RigCoordinator.cpp`
- membership-based tests/helpers

Work:

- keep an audio-applied snapshot handle;
- tick each Trigger exactly once at the callback boundary;
- leave Station tick responsible only for Station/LoopTake work;
- delete Station input/tick/live-MIDI Trigger traversal;
- remove Station membership types and publication;
- remove keyboard fallback and use snapshot dispatch exclusively;
- replace membership tests with direct dispatch/tick assertions.

Commit:

`Drive triggers from the applied rig snapshot`

### Phase 4: make overdub processing take-owned

Files likely involved:

- `Station.h/.cpp`
- `LoopTake.h/.cpp`
- `Loop.h/.cpp` if a narrow writer interface is introduced
- `Trigger.h/.cpp`
- `audio/Overdub_Tests.cpp`, `audio/AudioFlow_Tests.cpp`

Work:

- store active source/bounce linkage on the target LoopTake;
- make Station bounce only its own takes;
- preserve Trigger's existing overdub mixer and delayed punch semantics through
  a narrow writer/link, without Station holding Trigger collections;
- prove overdub, punch, quantised tail, and route-move behaviour;
- remove `Trigger::GetTakes()` if no non-test consumer remains.

Commit:

`Make LoopTake own active overdub processing links`

### Phase 5: harden history and revision application

Files likely involved:

- `Trigger.h/.cpp`
- `AudioHost.cpp`
- `RigCoordinator.cpp`
- `Scene.cpp`
- Trigger and RigSnapshot tests

Work:

- introduce explicit active-start state;
- make end actions refer only to a successful current start;
- make ditch pop conditional on the defined result;
- apply capture routing only to retained route-change records;
- remove the all-Trigger edit-eligibility scan;
- verify delayed actions retain recorded receivers and block audio-boundary transition;
- verify unrelated busy/history-bearing Triggers do not block an unrelated
  edit.

Commit:

`Harden trigger history and targeted route application`

### Phase 6: exact integration, cleanup, and documentation

Work:

- add the full real-Station scenario below;
- remove obsolete helpers/APIs/includes/dead equivalence functions;
- update `doc/plan.md` and comments to the final architecture;
- run the full build/test/audit matrix;
- review the complete diff against `doc/glossary.md` and repo conventions.

Commit:

`Cover dynamic trigger routing and ditch history end to end`

## Test matrix

### Exact dynamic-routing integration

Use real `Station`, `RigCoordinator`, AudioHost boundary test access, and input
publication—not direct `ApplyCaptureRouting` with fake receivers:

- A `{0}` record/end -> A1;
- edit to A `{0,1}` through audio-boundary transition/persist/audio/input promotion;
- record/end -> A2 and assert two audio loops/channels as appropriate;
- edit to B `{0}` through the full protocol;
- record/end -> B1;
- assert one retained Trigger identity and three distinct global target IDs;
- ditch -> B1 removed only from B;
- ditch -> A2 removed only from A;
- ditch -> A1 removed only from A;
- history empty; fourth ditch is a no-op;
- no action reaches the current receiver when the recorded receiver differs.

### Identity and edit policy

- reorder does not exchange histories;
- deletion does not shift another Trigger into an old identity;
- activation change creates a replacement and never inherits history;
- deletion/replacement with history is rejected;
- route edit on T1 succeeds while unrelated T2 is playing or has history;
- only route-changed retained instances receive `ApplyCaptureRouting`;
- unchanged active Trigger mixer/config is untouched.

### Input and race tests

- UI and job edges published before their respective acknowledgements drain
  before audio-boundary transition;
- Scene cannot request audio-boundary transition until both producer acknowledgements
  match the requested closed revision;
- either producer starting after its acknowledgement is rejected;
- old revision cannot enqueue after reopen at the new revision;
- stale HUD widget, keyboard dispatch, MIDI route, and serial route cannot act
  on a reused Trigger's new receiver;
- queued old-revision edge is discarded by audio defense check;
- all producers share the same gate API;
- ring-full behaviour and diagnostic count are deterministic;
- shutdown close-forever rejects all later producers.

Use deterministic hooks, barriers, latches, or atomics. Do not use timing sleeps
to try to hit races.

### Trigger state/history tests

- failed/unbound record start creates no active entry and end does not target
  an older entry;
- successful start/end retains receiver and IDs;
- delayed overdub start/end retains its receiver;
- punch delayed actions target the history receiver;
- ditch result `Removed`, `AlreadyAbsent`, and `Failed` have the specified pop
  behaviour;
- reset clears active state, queues, held binding state, and published state;
- debounce/repeat semantics remain identical after producer-side route matching.

### Trigger-centric ownership tests

- one Trigger targeting a Station ticks exactly once;
- several Triggers targeting one Station each tick exactly once;
- an unbound Trigger's documented tick/dispatch behaviour is explicit;
- a Station with no Triggers still ticks its own visual/take state;
- keyboard/MIDI/serial dispatch uses only RigInputDispatch;
- live MIDI device eligibility is correct without querying Station for
  Triggers;
- no `TriggerMembership` symbol or test helper remains.

### Overdub/audio tests

- existing bounce alignment and non-zero-output cases remain green;
- active source/target linkage resides on target LoopTake;
- punch in/out still applies ADC/bounce fades correctly;
- completing an overdub and then moving the Trigger route does not corrupt its
  old Station takes or tail;
- edit remains rejected while overdubbing, punched in, or carrying delayed
  actions, and succeeds once idle.

### Lifecycle/publication tests

- persistence failure reopens the accepted revision only;
- shutdown during staging and pending leaves no producer able to enqueue;
- old snapshots remain alive through audio and input acknowledgements;
- retired snapshots and old mixer behaviour are destroyed off audio;
- repeated `CloseAudio`/`Shutdown` remains safe.

## Subagent strategy for the implementation session

Use subagents deliberately for context control; the root agent owns the final
architecture, integration, builds, and every commit.

At the start, spawn three **read-only** research agents in parallel:

1. **Trigger/take flow:** revalidate record/overdub/punch/ditch and active-tail
   call graphs; report exact files/lines and hidden invariants.
2. **Publication/input concurrency:** enumerate every producer and prove the
   gate/acknowledgement/SPSC ordering, failure, and teardown paths.
3. **Tests/schema/git:** inventory impacted tests, serializer/migration rules,
   and current index/branch state.

The root agent must personally read the applicable skill, `doc/realtime-audio.md`,
`doc/glossary.md`, and every instruction/reference used for decisions. Do not
delegate interpretation of the threading skill.

After APIs are fixed, parallelize only non-overlapping implementation work (but prefer serial subagent sessions to be safer):

- root: Trigger ingress, RigSnapshot/RigCoordinator, AudioHost/Scene protocol;
- agent A: Station/LoopTake bounce refactor on an agreed narrow interface;
- agent B: integration and audio tests in separate test files;
- agent C: read-only concurrency/hot-path review after each threading phase.

Agents must not commit. They report changed files and exact verification. Root
reviews shared-worktree diffs before staging. Do not let two agents edit
`Trigger.*`, `AudioHost.*`, or `RigCoordinator.*` concurrently. Reassign agents
between phases rather than accumulating unrelated context.

## Build and verification procedure

Before every build or native-test run, reread `.vscode/tasks.json`. Use its
exact MSBuild path and the direct test project command with an absolute
`SolutionDir` ending in exactly one backslash.

After each phase:

1. run the smallest relevant filtered tests;
2. build `JammaLib_Tests` incrementally;
3. run the full native suite before committing;
4. inspect `git diff --check` and the staged diff.

At the end:

1. incrementally build `JammaLib`;
2. incrementally build `Jamma` because Scene/HUD wiring changes;
3. build and run the full native suite;
4. run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .agents/skills/threading-review/audio-hotpath-audit.ps1
```

5. manually inspect every callback-owned function listed in
   `doc/realtime-audio.md`, especially:
   - `AudioHost::_OnAudio` and snapshot application/audio-boundary transition;
   - `Scene::OnTick`;
   - `Trigger::OnTick` and queue drain;
   - `Station::OnBounce`;
   - `LoopTake::WriteBlock` and any new active-bounce method;
   - `Loop::WriteBlock`.
6. search the callback diff for allocation, mutex/lock, waits, I/O, logging,
   throwing operations, unbounded traversal, and last-owner destruction.
7. search for all removed membership symbols and all direct `Trigger::OnEvent`
   callers; only test-private/audio-owned uses may remain.
8. run `git status --short`, `git diff --check`, and review each final commit.

## Definition of done

- The exact A `{0}` -> A `{0,1}` -> B `{0}` -> three ditches scenario passes
  through the real publication protocol.
- Trigger identity and history survive route edits, additions, deletion, and
  reorder without positional guessing.
- Every history operation uses its recorded receiver and global take IDs.
- Station owns takes and is agnostic about which Trigger caused them.
- No Station Trigger membership type, snapshot, traversal, or fallback remains.
- All Trigger mutable operational state has one audio-thread writer.
- HUD, keyboard, MIDI, and serial use the same revisioned producer barrier.
- Closing ingress proves no producer can publish after audio-boundary transition; stale
  revisions are rejected defensively.
- Only changed retained Triggers are mutated at audio publication.
- Unrelated active/history-bearing Triggers do not block unrelated routing.
- Snapshot/object teardown cannot perform last-reference destruction on audio.
- Sufficient unit test coverage of functionality especially problematic edge cases like race conditions (written and verified for each individual phase).
- Full tests and app/library builds pass; hot-path audit and manual review find
  no lock, wait, allocation, logging, or I/O regression.
- Updated docs is they may have changed anywhere due to this update.
- Each implementation phase is preserved as a green, reviewable commit and no
  history rewrite is performed without explicit approval.
