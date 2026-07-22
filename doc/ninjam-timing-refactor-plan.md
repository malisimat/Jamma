# NINJAM Timing and Tempo Synchronisation Refactor Plan

## 1. Goal

Unify connected NINJAM timing behind one clear subsystem while preserving local-only quantisation and the existing real-time audio constraints.

The target design has three simple concepts:

- `NinjamTiming`: an immutable value describing the latest valid remote interval in local device samples.
- `NinjamTimingTracker`: the narrow state machine currently represented by `ExternalTransport`. It validates generations, wraps, and join alignment.
- `NinjamTimingCoordinator`: the only job-thread owner of connected tempo policy, remote timing state, local tempo requests, and phase-correction decisions.

`NinjamNetworkService` remains responsible for connection, chat, remote users, and remote audio routing. `TimingQuantiser` remains responsible for local musical timing: tap tempo, seed/master deduction, quantisation grain, overlay state, and local MIDI grain. Neither class should continue to contain half of the connected NINJAM timing protocol.

The refactor must keep these behaviours:

1. Local quantisation works without a NINJAM session.
2. A connected session can adopt remote tempo automatically or after user confirmation.
3. A local tempo can be proposed to the server and acknowledged without a false remote-change prompt.
4. Mid-interval joins align once at a validated wrap.
5. Steady connected playback remains phase-locked without cursor races, stale corrections, or audible jitter.
6. The metronome receives current block-level remote phase and does not use a snapshot that is stale until the next interval wrap.
7. Export-lane latency compensation remains independent. Only pure sample conversion helpers may be shared.

## 2. Current Problems

### 2.1 Ownership is split across the wrong classes

`NinjamNetworkService::_FeedExternalTransport()` currently performs sample-rate conversion, constructs the transport observation, tracks generation changes, stages join alignment, applies clock discipline, and fans corrections out to local takes. The same service also owns remote tempo prompts and local server-request acknowledgement.

`TimingQuantiser` contains local quantisation and NINJAM-specific state together:

- `_remoteMasterLoopSamps`
- `_remoteSampleRate`
- `_lastRemoteIntervalPos`
- remote proposal, acceptance, acknowledgement, queue, send, and phase-discipline methods

This makes `NinjamNetworkService` the hidden coordinator while `TimingQuantiser` acts as a second connected-timing authority.

### 2.2 There are two timing fact paths

`NinjamRemoteSnapshot` is produced on the job path and contains users plus timing. `NinjamLiveTiming` is read in the audio callback for the metronome. They duplicate position, interval length, sample rate, BPM, BPI, and validity with different naming and conversion rules.

`ExternalTransportState` is a third representation. It stages current phase but publishes only at accepted wraps, so it cannot replace the live metronome input as currently designed.

### 2.3 Timer correction ownership is unsafe

`Timer::Tick()` runs on the audio thread and advances `_sampOffset` with a load/arithmetic/store sequence. `TimingQuantiser::ApplyRemotePhaseCorrection()` currently calls `Timer::SetMasterLoopIndexFrac()` from the job thread. Atomic fields prevent undefined behaviour, but they do not make the two compound operations atomic. A job-thread store can be overwritten by the audio thread's pending store.

The same lost-update class was already avoided for loop cursors by publishing corrections to `LoopTake` and consuming them in `LoopTake::EndMultiPlay()`. Timer correction must follow the same ownership rule.

### 2.4 The current public transport model carries derived clutter

`ExternalTransportState` contains remote facts, derived local interval start, master-loop shape, master play position, wrap count, and pending join internals. Most callers need either the latest timing facts or a validated event, not the entire state machine.

### 2.5 Correction names describe origin poorly

`QueueTransportPhaseCorrection()` and `QueueExternalPhaseCorrection()` both translate playback cursors. One is used when accepting a new tempo and one is generation-scoped connected discipline. Their distinction should be represented by a correction value containing reason and generation, not by two accumulating atomics with similar names.

## 3. Target Architecture

```text
NJClient
  |
  +-- audio callback observation ---------------------------+
  |                                                         |
	|   NinjamTimingObservationMailbox::Publish()             |
	|     -> same-block NinjamTiming value                     |
  |     -> NinjamMetronomeTiming::Compute()                 |
  |                                                         |
  +-- job snapshot -----------------------------------------+
        NinjamNetworkService::UpdateRemoteStations()
				mailbox::ReadLatest()
        NinjamTimingCoordinator::ObserveRemoteTiming()
          -> NinjamTimingTracker
             -> generation change / accepted wrap / join
          -> tempo proposal and local-request acknowledgement
          -> pending audio-owned Timer correction
          -> generation-tagged LoopTake correction

Audio callback boundary
  -> Timer consumes pending clock command, then ticks
  -> LoopTake::EndMultiPlay consumes matching cursor command
```

The coordinator is the connected timing authority, but it does not render audio, mutate loop cursors directly, own UI widgets, or perform network I/O itself.

### 3.1 `NinjamTiming`

Add `JammaLib/src/ninjam/NinjamTiming.h` with one canonical read model. Use the project's existing field style consistently rather than retaining the lowercase `NinjamLiveTiming` names.

```cpp
struct NinjamTiming
{
	bool IsConnected = false;
	bool IsValid = false;
	unsigned int IntervalLengthSamps = 0u;   // device-rate samples
	unsigned int IntervalPositionSamps = 0u; // device-rate samples
	unsigned int DeviceSampleRate = 0u;
	unsigned int SourceSampleRate = 0u;
	float Bpm = 0.0f;
	unsigned int Bpi = 0u;
	std::uint64_t Generation = 0u;
	unsigned long RemoteWrapCount = 0ul;
	std::uint64_t ObservationSequence = 0u;
	std::uint64_t LocalBlockStartSample = 0u;
};
```

Required invariants:

- Interval length and position are always converted to the current audio device rate before publication.
- Raw source-rate values stay at the connection boundary and are not compared with `Timer` or loop positions.
- `Generation` changes when interval length or source/device sample-rate domain changes, and on reconnect.
- `ObservationSequence` changes for each accepted fresh observation; it is not a wrap counter.
- Invalid/disconnected publication clears interval fields so stale timing cannot drive audio.

Do not include user/channel collections, prompt state, loop pointers, or export delay state.

### 3.2 `NinjamTimingTracker`

Rename `ExternalTransport` to `NinjamTimingTracker` after the coordinator is proven. During migration, it may retain its filename temporarily to keep diffs reviewable.

The tracker owns only:

- device-rate position and interval validation;
- duplicate and backward-jump rejection;
- generation changes;
- accepted wrap count;
- pending mid-cycle join measurement and wrap commit;
- pure signed circular difference and sample-rate conversion;
- diagnostics counters for observations and wrap decisions.

Use small input/output values:

```cpp
struct NinjamTimingObservation
{
	unsigned int IntervalLengthSamps = 0u;
	unsigned int IntervalPositionSamps = 0u;
	std::uint64_t LocalSample = 0u;
};

enum class NinjamTimingEventType
{
	GenerationChanged,
	Wrap,
	Join
};

struct NinjamTimingEvent
{
	NinjamTimingEventType Type;
	std::uint64_t Generation = 0u;
	unsigned long RemoteWrapCount = 0ul;
	unsigned int IntervalLengthSamps = 0u;
	unsigned int RemotePositionSamps = 0u;
	long long PhaseDeltaSamps = 0;
};
```

Return explicit events from observation ingestion. Do not require callers to compare `Generation()`, inspect diagnostics, and infer what happened.

Remove these fields unless a concrete consumer is found during migration:

- `MasterLoopLengthSamps`
- `MasterLoopCount`
- `MasterLoopPlayPositionSamps`
- `AuthoritativeIntervalStartSamps`
- published copies of pending join internals

Join internals should remain private tracker state. Tests should assert emitted `Join` events rather than inspect private bookkeeping after publication.

### 3.3 `NinjamTimingCoordinator`

Add `JammaLib/src/ninjam/NinjamTimingCoordinator.h/.cpp`. It is constructed with or passed explicit collaborators rather than owning `Scene` or stations permanently.

It owns all connected timing runtime state currently spread across `NinjamNetworkService` and `TimingQuantiser`:

- `NinjamTimingTracker`;
- connect/disconnect generation;
- latest canonical `NinjamTiming` publication;
- join options;
- pending, ignored, and locally requested tempo state;
- join-push acknowledgement window;
- remote tempo proposal and acceptance policy;
- local tempo request queue and wrap-boundary send decision;
- dead-band, safety limit, and diagnostics counters;
- generation-scoped correction event production.

Proposed public surface:

```cpp
class NinjamTimingCoordinator
{
public:
	void Connect(const NinjamTempoJoinOptions& options,
		const NinjamLocalTiming& localTiming);
	void Disconnect();

	NinjamTimingUpdate ObserveRemoteTiming(
		const NinjamRemoteTiming& remote,
		const NinjamLocalTiming& local);

	void ObserveLocalTiming(const NinjamLocalTiming& local);
	std::optional<NinjamTempoRequest> TakeTempoRequestAtWrap();
	std::optional<NinjamTempoChange> PendingTempoChange() const;
	NinjamTimingUpdate ResolveTempoChange(bool accept,
		const NinjamLocalTiming& local);

	std::shared_ptr<const NinjamTiming> PublishedTiming() const noexcept;
	NinjamTimingDiagnostics Diagnostics() const noexcept;
};
```

Keep network and engine effects explicit in returned `NinjamTimingUpdate` values:

```cpp
struct NinjamTimingUpdate
{
	std::optional<NinjamClockSettings> ClockSettings;
	std::optional<NinjamPhaseCorrection> PhaseCorrection;
	std::optional<NinjamTempoRequest> TempoRequest;
	bool InvalidatePendingCorrections = false;
	bool PromptForTempoChange = false;
};
```

This prevents the coordinator from reaching into `Station`, `LoopTake`, `NinjamSession`, or UI objects. `Scene`/`NinjamNetworkService` apply returned effects through narrow adapters.

### 3.4 Keep `TimingQuantiser` local

After migration, `TimingQuantiser` should own only local grid behaviour and generic clock configuration.

Move out or replace:

- `ApplyRemoteTempo()`
- `ProposeRemoteTempoChange()`
- `ApplyAcceptedRemoteTempo()`
- `AcknowledgeLocallyRequestedRemoteTempo()`
- `ForceQueueCurrentTempoAsPending()`
- `ResetPendingTempoSyncState()`
- `DisciplineRemotePhase()`
- `ApplyRemotePhaseCorrection()`
- `QueueLocalTempo()` and `SendQueuedTempo()` NINJAM policy
- `_remoteMasterLoopSamps`, `_remoteSampleRate`, `_lastRemoteIntervalPos`

Retain or generalise:

- `ApplyTiming()` for local clock/grid settings;
- `CurrentTempoTiming()`;
- `IntervalSampsFromTempo()` only if local callers remain, otherwise move it beside NINJAM timing conversion;
- tap tempo, hover master selection, grain propagation, overlay state, and quantisation policy.

Add a small `CurrentLocalTiming(sampleRate)` read model so the coordinator does not inspect private quantiser fields.

### 3.5 Keep connection wrappers thin

`NinjamConnection`, `NinjamSession`, and `NinjamController` remain lifecycle and data forwarding layers.

Replace `NinjamLiveTiming` and the timing subset of `NinjamRemoteSnapshot` with one source-rate boundary type, for example `NinjamRemoteTiming`. Both `GetLiveTiming()` and the job snapshot should use the same field names and validity rules. Do not make the wrappers perform tempo acceptance or phase correction.

`NinjamRemoteSnapshot` may embed `NinjamRemoteTiming Timing;` alongside `Users`. This removes duplicated timing fields without coupling audio code to user snapshots.

### 3.6 Keep metronome planning and export timing separate

`NinjamMetronomeTiming::Compute()` remains a pure block planner. Change its input to accept canonical device-rate interval phase plus only block-specific values:

- frame count;
- output latency in device samples;
- BPM and BPI.

Remove sample-rate conversion from the metronome once `NinjamTiming` guarantees device-rate values.

`NinjamMetronome` remains click-table playback and mixing only.

`ExportLaneTiming` remains independent because it aligns send-lane content to NJClient's encode position. It must not share generation, join, or correction state with receive-side timing. Pure helpers such as rounded sample-rate scaling may be shared if their contracts match exactly.

## 4. Phase Discipline Decision

### 4.1 Publish phase continuously; do not hard-correct continuously

The latest `NinjamTiming` should be refreshed from the audio callback's existing `GetLiveTiming()` observation so the metronome and other read-only audio consumers see current phase. This adds no new NJClient call to the hot path: `AudioHost` already performs it when the NINJAM metronome is enabled.

Make observation publication independent of metronome enablement. Connected timing correctness must not change when the click is muted. Capture once per audio block, pair it with the block-start local sample, and convert to device samples with pure integer math. Use that local `NinjamTiming` value directly for the same block's metronome calculation, then publish it to the job thread through a dedicated `NinjamTimingObservationMailbox`.

The mailbox must use fixed storage and a race-free sequence protocol. One acceptable implementation uses an odd/even atomic sequence plus atomic fields: the audio writer marks the sequence odd, stores every field, then publishes the next even sequence with release ordering; the job reader accepts a copy only when matching acquire-loaded even sequences bracket all field loads. Do not use a seqlock around non-atomic fields because that remains a C++ data race. Do not allocate a `shared_ptr` every audio block.

The job thread consumes the most recent coherent observation, passes it to `NinjamTimingTracker`, and updates the coordinator's immutable read publication. Rich user snapshots continue at job cadence. The coordinator's policy and tracker state remain job-thread-owned; the audio callback never calls a policy-mutating coordinator method.

Do not apply the full measured error on every observation. NJClient position sampling, block boundaries, and job scheduling introduce bounded noise; direct chasing can turn that noise into timer and loop jitter.

### 4.2 Keep join and loop translations wrap-gated

Join alignment and local take cursor translation remain one-shot events at an accepted remote wrap. A wrap is the musical boundary where a shared correction is least surprising and where generation validity is known.

Every event uses one signed delta:

```text
delta = shortest_signed(remotePhase - localPhase, intervalLength)
```

The same generation-tagged delta must be consumed exactly once by the audio-owned `Timer` and every active local `LoopTake`. Positive advances local phase; negative moves it backward.

### 4.3 Move Timer mutation to the audio thread

Add an audio-thread-consumed pending clock command. Do not call `Timer::SetMasterLoopIndexFrac()` from the job thread while audio is running.

Prefer one command mailbox with sequence/generation over independent atomic fields:

```cpp
struct TimerCommand
{
	std::uint64_t Sequence = 0u;
	std::uint64_t Generation = 0u;
	unsigned long SeedLengthSamps = 0ul;
	unsigned int QuantiseSamps = 0u;
	long long PhaseDeltaSamps = 0;
	bool ReplaceTiming = false;
};
```

The job thread publishes commands. The audio thread consumes a command at the defined block boundary before advancing the clock for that block. Exact placement must be pinned by a test against `Scene::_OnAudio()`/`Timer::Tick()` ordering so timer and take corrections describe the same completed block.

A new command invalidates older commands by sequence and generation. Disconnect publishes an invalidation/reset command before connected corrections can be consumed in local mode.

### 4.4 Evaluate bounded slew only after telemetry

Hard wrap corrections are predictable but a steady correction of several samples can still click or shift MIDI at a boundary. Add diagnostics first and retain the current full-correction behaviour behind one implementation path.

If live traces show regular nonzero corrections above the measured observation noise, add a bounded audio-thread slew for the `Timer` only:

- consume a generation-tagged target phase error at a validated wrap;
- spread a small correction over the next interval using integer accumulation;
- cap correction rate to a measured inaudible value;
- never change loop length or quantisation grain during a slew;
- ensure loop/audio/MIDI phase follows the same accumulated correction, not a separate target;
- cancel on generation change, disconnect, or tempo acceptance.

Do not implement a PLL, floating-rate clock, or per-block job-thread correction in the first refactor. Those add state and tuning before the actual observation noise is known. The first target is race-free exact correction with continuous measurement.

## 5. Thread Ownership

| State or operation | Writer | Readers | Handoff |
|---|---|---|---|
| Raw NJClient timing observation | Audio callback | Coordinator/job thread, metronome | Atomic-field, sequence-checked mailbox; no mutex or allocation |
| `NinjamTimingTracker` staging | Job thread | Job thread | Single owner |
| Same-block canonical `NinjamTiming` | Audio callback local value | Metronome, observation mailbox | Stack value, then atomic-field publication |
| Coordinator timing publication | Job thread | Render/job consumers | Immutable publication; never used as the metronome's current-block phase |
| Tempo proposals and local request state | Job thread | Job/UI thread | Single owner; copy values to UI |
| Timer command | Job thread publishes | Audio thread consumes | Generation/sequence mailbox |
| Loop correction | Job thread publishes | `LoopTake::EndMultiPlay()` consumes | Existing atomics, consolidated into one typed command |
| Metronome planning state | Audio thread | Audio thread | No cross-thread sharing |
| Export-lane timing state | Audio thread | Audio thread | Independent |

No new mutex, allocation, logging, session call, station traversal, or blocking wait may be added to the audio callback. Any audio-thread diagnostics must be fixed-size counters or records drained later by the job thread.

## 6. Migration Steps

Each step must leave the build and focused tests green. Avoid a single rename-and-rewrite patch.

### Step 1: Characterise the current contract

Add or strengthen tests before moving ownership:

- source-to-device sample conversion, including 44.1 kHz to 48 kHz rounding;
- generation prime without a wrap;
- duplicate observation and false backward jump rejection;
- one accepted end-to-start wrap;
- mid-cycle join emits one signed correction at the next accepted wrap;
- reconnect invalidates old generation state;
- matching local server request is acknowledged without a prompt;
- rejected remote tempo remains ignored until the proposal changes;
- local-only quantisation is unchanged while disconnected;
- metronome receives fresh phase between wraps;
- Timer correction cannot be lost when published adjacent to an audio tick.

Create a focused `NinjamTimingCoordinator_Tests.cpp` once the coordinator exists. Do not force all policy cases through `Scene` fixtures.

### Step 2: Introduce canonical timing values and pure math

Add `NinjamRemoteTiming` at the connection boundary and `NinjamTiming` in the device domain.

Centralise these pure functions with focused tests:

- `ScaleSampleRate()`;
- `IntervalSampsFromTempo()`;
- `SignedCircularDifference()`;
- source observation to device timing conversion.

Update `NinjamConnection::GetLiveTiming()`, `NinjamRemoteSnapshot`, `NinjamSession`, and `NinjamController` to use the common source type. Keep compatibility adapters temporarily if needed, then remove them in Step 8.

### Step 3: Add the lock-free live observation handoff

Capture live remote timing once per connected audio block regardless of metronome enabled state. Pair it with the local block-start sample already available in `AudioHost`.

Use the atomic-field odd/even sequence protocol described in Section 4.1, or an existing proven fixed-capacity SPSC primitive with equivalent semantics. Add concurrency-oriented tests proving readers receive either the previous complete observation or the next complete observation, never mixed fields.

Feed the metronome from the canonical device-rate timing. Keep its compute and mix classes otherwise unchanged.

### Step 4: Introduce `NinjamTimingCoordinator` around the existing tracker

Move these fields from `NinjamNetworkService` first:

- `_tempoJoinOptions` if no non-timing caller needs ownership;
- `_joinPushAwaitingOutcome`;
- `_joinPushSentAtAcceptedWrap`;
- `_locallyRequestedTempo`;
- `_pendingRemoteTempoPrompt`;
- `_ignoredRemoteTempoPrompt`;
- `_externalJoinAligned`;
- `_externalGeneration`;
- `_externalTransport`.

Move `_FeedExternalTransport()`, tempo match helpers, proposal resolution, acknowledgement, and correction decision logic into the coordinator. Keep `NinjamNetworkService` methods as thin forwarding adapters until `Scene` is migrated.

At this checkpoint behaviour should be unchanged except that tracker events are explicit.

### Step 5: Move Timer and take correction application to one audio boundary

Add the generation/sequence command mailbox for `Timer`. Consume it on the audio thread at a tested point relative to `Timer::Tick()`.

Replace direct job-thread `SetMasterLoopIndexFrac()` calls for connected timing. Tempo/grid configuration that currently resets several Timer atomics must also be published as one coherent audio command while playback is active.

Replace the two `LoopTake` correction queues with one typed pending correction if feasible without increasing hot-path work. The command must distinguish:

- accepted tempo replacement;
- join alignment;
- steady phase discipline;
- invalidation.

Apply the same signed correction to audio loop indices, MIDI visual index, and MIDI anchor correction. Preserve modulo-by-each-loop-length behaviour and exactly-once generation checks.

Run the real-time audit immediately after this step.

### Step 6: Move NINJAM policy out of `TimingQuantiser`

Move proposal, acceptance, acknowledgement, local request, and discipline methods into the coordinator. Replace station mutation inside `ApplyAcceptedRemoteTempo()` with returned clock settings and correction events.

Add a generic `TimingQuantiser::ApplyTiming()`/`CurrentLocalTiming()` boundary as needed. Keep local tap and hover flows using that same local API.

Delete NINJAM state fields from `TimingQuantiser` only after all callers are migrated and focused local quantisation tests pass.

### Step 7: Simplify `Scene` and `NinjamNetworkService`

Change `Scene::OnJobTick()` ordering to one explicit timing call:

1. pump controller and obtain rich snapshot;
2. update remote stations/users;
3. provide current local timing and the latest live observation to the coordinator;
4. apply returned clock/take commands;
5. send any returned server tempo request;
6. expose a pending prompt value to the existing UI flow.

Remove `_SendQueuedTempoAtIntervalWrap()` and `_HandleRemoteTempoSnapshot()` wrappers when they only forward.

Change `_ClearTimingState()` to ask the coordinator whether connected timing is active. It must not inspect an `ExternalTransportMode` type.

`NinjamNetworkService` should finish with connection/chat/controller methods, remote station updates, and one narrow bridge for sending coordinator-produced tempo requests.

### Step 8: Rename and remove compatibility code

Rename `ExternalTransport` to `NinjamTimingTracker` and update project/filter entries. Rename tests accordingly.

Remove:

- `NinjamLiveTiming` compatibility type;
- forwarding methods retained during migration;
- old `ExternalTransportState` fields with no consumer;
- duplicate sample conversion helpers;
- old NINJAM methods and fields in `TimingQuantiser`;
- separate transport/external correction queues if the typed command has replaced both;
- stale comments describing job-thread Timer mutation as safe.

Use the language server rename operation for class and symbol renames so references and tests move together.

### Step 9: Add diagnostics and long-running simulation

Coordinator diagnostics, emitted on the job thread only, should count:

- observations accepted/rejected/duplicated;
- generation changes;
- wrap candidates accepted/rejected;
- join events;
- tempo proposals accepted/rejected/acknowledged;
- phase events queued/consumed/invalidated;
- safety-limit rejections;
- maximum and histogram buckets for observed phase error.

Add a deterministic simulation covering thousands of intervals with 44.1/48 kHz conversion, bounded observation jitter, positive and negative local rate error, a tempo generation change, disconnect/reconnect, and local takes of `L`, `2L`, `L/2`, and a non-divisor length.

Assert that relative take offsets remain invariant, corrections occur only for accepted generations, disconnected mode emits none, and steady error stays within the selected bound.

### Step 10: Live validation and optional slew decision

Capture live traces for:

1. matching tempo join near interval start;
2. matching tempo join halfway through an interval;
3. local tempo push accepted and rejected by the server;
4. remote tempo change accepted and ignored;
5. ten minutes at 44.1 kHz remote / 48 kHz device;
6. several take lengths plus a take intentionally offset by one beat;
7. disconnect, local continuation, and reconnect.

Measure phase-error p50/p95/p99, correction magnitude, sign changes, and queued/consumed counts. Only add bounded slew if wrap corrections remain audibly or measurably disruptive after the race-free refactor.

## 7. Tests by Component

### `NinjamTimingTracker`

- starts disconnected and ignores observations;
- first valid timing primes one generation;
- interval/sample-rate change primes without wrap;
- duplicate does nothing;
- monotonic advance does nothing;
- end-to-start crossing emits one wrap;
- backward jump away from boundary is rejected;
- join delta uses the documented sign and commits once;
- stale pending join is discarded on generation change;
- reconnect cannot reuse generation or wrap state.

### `NinjamTimingCoordinator`

- no local content auto-accepts valid remote tempo;
- local content prompts when configured;
- accept emits one coherent clock settings command;
- reject suppresses the same proposal but not a changed proposal;
- local tempo push waits for an accepted wrap;
- matching remote result acknowledges local request;
- fallback occurs after the existing acknowledgement window;
- generation change invalidates pending corrections;
- join correction and steady correction share one sign convention;
- safety-limit rejection emits no Timer or take command;
- disconnect clears prompt, request, join, and correction state.

### Timer and loop handoff

- a command adjacent to `Tick()` is consumed exactly once;
- no normal tick advance is lost;
- Timer and all takes receive the same generation and signed delta;
- `L`, `2L`, and odd lengths preserve relative offset;
- negative correction across zero remains negative semantically;
- MIDI visual and anchor corrections match audio;
- invalidation before consumption prevents stale movement;
- disconnected playback remains byte-for-byte equivalent when no command exists.

### Metronome

- phase advances between remote wraps;
- device-rate input removes duplicate sample conversion;
- output latency projection remains correct;
- timing generation resets click cursors once;
- muting the click does not stop canonical timing publication.

## 8. Validation Commands

Use incremental builds only. The implementation session should read `doc/build.md` and use the repo builder task/script where available.

After each JammaLib step:

```powershell
& .\.github\skills\builder\builder.ps1 -Target JammaLib -Action Build
```

After test changes or engine behaviour changes:

```powershell
& .\.github\skills\builder\builder.ps1 -Target JammaLib_Tests -Action Build -RunTests `
  -TestFilter 'NinjamTiming*:ExternalTransport*:RemotePhaseCorrection.*:DisciplineRemotePhase.*:NinjamMetronomeTiming*:*ExternalPhase*'
```

At each migration checkpoint, run the full native suite:

```powershell
& .\.github\skills\builder\builder.ps1 -Target JammaLib_Tests -Action Build -RunTests
```

After any audio-thread or cross-thread change:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .github\skills\threading-review\audio-hotpath-audit.ps1
```

Manually inspect `AudioHost` callback timing capture, `Timer::Tick()`, `LoopTake::EndMultiPlay()`, and `NinjamConnection::ProcessAudioBlock()` for new allocation, locks, logging, waits, or unbounded work.

## 9. Expected Files

Add:

- `JammaLib/src/ninjam/NinjamTiming.h`
- `JammaLib/src/ninjam/NinjamTimingCoordinator.h/.cpp`
- `JammaLib/src/ninjam/NinjamTimingTracker.h/.cpp` after the final rename
- `test/JammaLib_Tests/src/ninjam/NinjamTimingCoordinator_Tests.cpp`
- `test/JammaLib_Tests/src/ninjam/NinjamTimingPublication_Tests.cpp`

Modify:

- `JammaLib/src/ninjam/NinjamConnection.h/.cpp`
- `JammaLib/src/ninjam/NinjamSession.h/.cpp`
- `JammaLib/src/ninjam/NinjamController.h/.cpp`
- `JammaLib/src/ninjam/NinjamNetworkService.h/.cpp`
- `JammaLib/src/ninjam/NinjamMetronomeTiming.h/.cpp`
- `JammaLib/src/audio/AudioHost.h/.cpp`
- `JammaLib/src/timing/TimingQuantiser.h/.cpp`
- `JammaLib/src/utils/Timer.h/.cpp`
- `JammaLib/src/engine/LoopTake.h/.cpp`
- `JammaLib/src/engine/Scene.h/.cpp`
- relevant `.vcxproj` and `.vcxproj.filters` files
- focused timing, quantisation, metronome, Timer, and `LoopTake` tests

Do not modify `ExportLaneTiming` unless a pure conversion helper is moved without changing its behaviour.

## 10. Completion Criteria

The refactor is complete when:

- `NinjamTimingCoordinator` is the only owner of connected tempo and phase policy;
- `NinjamTimingTracker` is private to the coordinator and exposes explicit events;
- `TimingQuantiser` contains no NINJAM session state or remote policy;
- `NinjamNetworkService` no longer choreographs tracker, quantiser, Timer, and take corrections;
- all remote timing comparisons use device-rate samples;
- current remote phase is available between wraps without per-block allocation or locking;
- Timer and loop cursor mutations occur on the audio thread through generation-safe commands;
- join and steady corrections cannot be lost, duplicated, or consumed after disconnect;
- metronome, local loops, MIDI, automation, and visuals share the same timing generation and sign convention;
- local-only quantisation and disconnected playback remain unchanged;
- export-lane timing remains an independent send-path mechanism;
- focused tests, the full native suite, and the real-time audit pass;
- live validation holds intended beat alignment for at least ten minutes across a sample-rate mismatch.