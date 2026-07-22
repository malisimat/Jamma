# NINJAM Timing Refactor Adversarial Review and Completion Plan

## 1. Verdict

The ownership refactor is directionally correct, but the implementation is not yet safe to treat as complete or reliable for live remote tempo synchronisation.

The main architectural gains are real:

- source-rate and device-rate timing values are separated;
- connected timing policy has moved into `NinjamTimingCoordinator`;
- `NinjamTimingTracker` replaces the old public transport state model;
- the audio callback publishes live timing without allocating or locking;
- Timer and take corrections are no longer applied directly by the job thread.

However, several correctness defects remain in the command handoffs and coordinator policy. Two defects can directly break the headline feature: Timer and takes can consume one correction in different audio blocks, and normal mid-interval join corrections are rejected by a safety limit intended for steady-state drift. The current completion report and tests overstate the degree of concurrency, sample-rate, and long-running simulation coverage.

Do not use successful compilation or the current full-suite pass as evidence that remote tempo synchronisation is correct. The existing tests mostly exercise components sequentially and do not reproduce the critical publication windows.

## 2. Confirmed Findings

### 2.1 Critical: Timer and takes do not share one command-consumption boundary

`LoopTake::EndMultiPlay()` consumes take corrections during station traversal in `AudioHost::_OnAudio()`. `Timer::ConsumePendingCommand()` runs later through the tick callback in `Scene::OnTick()`.

A job-thread publication can therefore occur:

- after one take consumes but before another take consumes;
- after all takes consume but before Timer consumes;
- immediately before Timer consumes while every take has already missed the command.

The result is a one-block Timer/take phase split, or even different local takes correcting in different blocks. Generation tags prevent some stale commands, but they do not make independent consumers observe one publication at the same callback boundary.

This violates the plan requirement that Timer and every active local take consume the same signed correction exactly once at one defined audio boundary.

### 2.2 Critical: valid mid-interval join corrections are rejected

`NinjamTimingTracker` correctly freezes a signed circular join delta, but `NinjamTimingCoordinator` applies the same `DefaultBufferSizeSamps * 2` safety limit to both steady-state drift and deliberate join alignment.

At 48 kHz, an eight-beat 120 BPM interval is 192,000 samples. A halfway join can legitimately require approximately 96,000 samples of correction, far above the generic two-buffer limit. The coordinator rejects it and emits no Timer or take command.

The safety policy must distinguish:

- tempo replacement: coherent absolute phase replacement;
- join alignment: one deliberate circular correction up to half an interval;
- steady discipline: small bounded correction with a strict anomaly limit.

### 2.3 High: LoopTake correction fields are not consumed coherently

`LoopTake::EndMultiPlay()` exchanges `_pendingTimingCorrectionSamps` and then separately loads `_timingCorrectionGeneration`. Invalidation and generation changes are separate producer stores.

More seriously, correction application is gated by the raw exchanged `correction`, while generation validity only affects `validCorrection`. A concurrent invalidation can produce `validCorrection == 0` while the stale raw correction is still applied to audio loops and MIDI state.

The delta, generation, reason, and publication sequence must form one coherent command. Invalidated or mismatched commands must never move audio, MIDI visual position, or MIDI anchor state.

### 2.4 High: Timer invalidation is not applied by the production lifecycle

`Timer::CommandType::Invalidate` exists and is tested in isolation, but `Scene::_ApplyNinjamTimingUpdate()` invalidates only take corrections. `Scene::DisconnectNinjam()` also clears takes without publishing Timer invalidation.

A connected Timer correction already published but not consumed can therefore execute after disconnect and move local-only playback. Timer generation state also survives until another command happens to replace it.

Disconnect and every timing-generation change must publish one coherent invalidation to the shared audio-boundary command path before later local or connected commands can be consumed.

### 2.5 High: zero-delta timing replacement can carry a stale hard-coded generation

When a coordinator update contains `ClockSettings` but no nonzero `PhaseCorrection`, `Scene` assigns Timer command generation `1`. Timer ignores commands older than its current audio generation.

After generation 2 or later, a valid remote tempo replacement that happens to need zero phase movement can be marked consumed but silently fail to replace seed length or quantisation.

Every coordinator timing update needs an explicit generation independent of whether a nonzero phase correction is present.

### 2.6 High: local tempo request retry and fallback policy is broken

After a local tempo request is sent, every later wrap satisfies the resend condition, sends the request again, and advances `_joinPushWrap`. The fallback branch requiring more than one wrap beyond `_joinPushWrap` is consequently unreachable.

Acknowledgement is checked only while processing a generation-change event. A matching server state that does not produce the expected interval-length generation change can be missed.

The coordinator needs explicit request states such as `Queued`, `SentAwaitingOutcome`, `Acknowledged`, and `Expired`, with a fixed sent-at wrap/sequence that is not advanced by retries. The network send result must also be reported back to the coordinator instead of assuming delivery.

### 2.7 High: observation time is captured but discarded

The callback publishes `LocalBlockStartSample` with each live remote observation. The coordinator ignores it and instead samples `clock.AbsoluteSamplePos()` when the job thread eventually processes the observation.

Remote phase from callback block $N$ is therefore compared with local phase from a later job tick. Scheduling delay becomes artificial phase error. Under load this can produce unnecessary corrections, alternating correction signs, or safety-limit rejection.

Tracker observations must preserve the callback-local sample anchor. Phase decisions must compare values from the same observation time or project both values to a common later sample using bounded integer arithmetic.

### 2.8 High: timing generation does not include the full sample-rate domain

The tracker changes generation only when the converted device-rate interval length changes. It does not receive source sample rate, device sample rate, connection generation, or observation sequence. The callback currently passes zero for canonical generation and wrap count.

A source-rate or device-rate change that yields the same rounded device interval can incorrectly retain a pending join or correction generation. Reconnect relies on incidental tracker state rather than an explicit connection generation contract.

Generation identity must include connection epoch, source sample rate, device sample rate, and interval domain. Observation sequence should reject stale or reordered values independently of wrap count.

### 2.9 High: live and job-snapshot validity rules disagree

The job snapshot rejects NJClient's known placeholder BPM/BPI values using plausible bounds. `GetLiveTiming()` accepts any positive BPM and BPI. `Scene` prefers the live mailbox value.

The placeholder timing that the snapshot deliberately rejects can therefore drive coordinator proposal and auto-accept behavior through the live path.

Use one shared `IsValidRemoteTiming()` boundary function for live and snapshot production. Invalid publication must retain connection state while clearing all timing fields.

### 2.10 Medium-high: mailbox coherence needs a portable C++ memory-model proof

The observation and Timer mailboxes use odd/even sequence checks around relaxed atomic fields. Atomic fields prevent data races, but the current release/acquire placement is not by itself a convincing portable proof that a matching pair of sequence reads brackets one coherent field set on weak-memory targets.

This is not the most likely failure on the current x64 target, but these handoffs are critical enough to use a protocol whose ordering can be explained mechanically. Prefer a fixed-capacity SPSC command queue or a proven per-slot sequence protocol with bounded audio-reader work. Do not rely on stress tests alone as a memory-order proof.

### 2.11 Medium: tracker wrap acceptance is too permissive for stale observations

Any backward movement from the final quarter into the first quarter is accepted as a wrap. `LocalSample` is unused, and there is no observation-sequence check, elapsed-sample plausibility check, or skipped-interval diagnostic.

A delayed stale observation can become a false wrap. A job stall spanning an interval can lose wraps silently. Tighten acceptance using sequence freshness and expected local elapsed samples, while retaining tolerance for callback-sized sampling uncertainty.

### 2.12 Medium: diagnostics and simulation completion claims are overstated

The current "long-running converted timing simulation":

- supplies already-converted device-rate values instead of exercising 44.1-to-48 kHz conversion;
- does not advance Timer and LoopTake consumers together;
- does not model concurrent publication;
- does not verify relative offsets for $L$, $2L$, $L/2$, and a non-divisor;
- asserts only that emitted corrections are below the same safety threshold that suppresses large corrections;
- resets coordinator diagnostics during reconnect, weakening long-run assertions.

`NotifyPhaseCorrectionConsumed()` is not wired to actual consumption, so queued-versus-consumed diagnostics do not currently prove exactly-once behavior.

## 3. Required Target Design Corrections

### 3.1 One audio-boundary transport command fan-out

Replace independent Timer and per-take publication with one scene/audio transport command mailbox owned at the callback boundary.

The job thread publishes one immutable command containing:

```cpp
struct NinjamAudioTimingCommand
{
	std::uint64_t Sequence = 0u;
	std::uint64_t Generation = 0u;
	NinjamTimingCommandType Type = NinjamTimingCommandType::Invalidate;
	unsigned long SeedLengthSamps = 0ul;
	unsigned int QuantiseSamps = 0u;
	utils::Timer::QuantisationType Quantisation = utils::Timer::QUANTISE_OFF;
	unsigned int AbsolutePhaseSamps = 0u;
	long long PhaseDeltaSamps = 0;
	NinjamTimingCorrectionReason Reason = NinjamTimingCorrectionReason::Invalidation;
};
```

At the start of `AudioHost::_OnAudio()`, before station playback advancement, consume at most one latest coherent command. Apply that same local copy to:

1. Timer;
2. every local take that participates in the block;
3. MIDI visual/anchor correction through the same take operation.

Then render/advance the block. No job-thread publication can split consumers because publication after the boundary waits for the next callback in its entirety.

The station snapshot already exists on the callback. Command fan-out must not allocate, lock, or obtain a new station snapshot.

### 3.2 Separate absolute replacement from relative correction

Use explicit semantics:

- `ReplaceTiming`: set seed length, grain, quantisation, and absolute interval phase coherently;
- `JoinAlignment`: apply one signed circular delta, allowing the full documented join range;
- `PhaseDiscipline`: apply a bounded signed delta under the steady-state safety policy;
- `Invalidate`: cancel prior generations without moving phase.

Do not infer generation or command type from whether delta is zero.

### 3.3 Make coordinator effects self-contained

Every `NinjamTimingUpdate` must carry:

- connection/timing generation;
- explicit command type;
- absolute settings or relative delta;
- tempo request identity and state transition;
- invalidation when required.

`Scene` should translate the effect without inventing generation `1`, joining unrelated fields, or applying independent invalidations.

### 3.4 Use one validated observation contract

Add a canonical observation identity containing:

- connection epoch;
- observation sequence;
- source sample rate;
- device sample rate;
- interval length and position in device samples;
- local callback block-start sample;
- BPM and BPI validity.

The tracker owns freshness, generation, and wrap classification. The coordinator owns tempo and correction policy. Neither should resample current Timer state as a substitute for the observation's local anchor.

### 3.5 Model tempo requests as a state machine

Record one request ID and sent-at wrap/observation sequence. Permit at most the configured retry count without moving the original timeout anchor. Process matching timing on every fresh valid observation, not only generation events. Report network send success/failure back into coordinator state.

## 4. Implementation Plan

### Phase 1: Add failing tests for confirmed defects

Before changing production code, add tests that reproduce:

1. publication between two take consumers;
2. publication after take consumption but before Timer consumption;
3. invalidation racing take consumption must apply zero movement;
4. pending Timer correction followed by disconnect must not move local playback;
5. zero-delta replacement at generation greater than one;
6. quarter-, half-, and three-quarter-interval joins at 48 kHz production lengths;
7. local request send once, acknowledgement, mismatch, retry, expiry, and send failure;
8. delayed job consumption using `LocalBlockStartSample` remains phase-invariant;
9. source-rate-only, device-rate-only, and reconnect generation changes;
10. live placeholder timing is rejected exactly like snapshot timing.

These tests should fail against the current implementation for the documented reason. Avoid tests that merely inspect counters without asserting the resulting Timer/take phase.

### Phase 2: Introduce the unified audio-boundary command

Add the fixed-storage command mailbox and move consumption to the beginning of the callback block. Remove correction consumption from `LoopTake::EndMultiPlay()` and remove the separate Timer pending-command mailbox once the unified path is proven.

Apply one command local copy to Timer and all local takes. Add callback-order tests that publish around a controllable boundary and prove all consumers either use the old command state or the new command state for a block, never a mixture.

Run the audio hot-path audit immediately.

### Phase 3: Correct coordinator command and join policy

Make generation explicit on all updates. Split replacement, join, and steady-discipline limits. A join delta should be accepted when it is the tracker's frozen shortest circular delta for the active generation; anomaly rejection belongs in tracker event validation, not a two-buffer magnitude cap.

Keep a conservative small limit for steady discipline until live telemetry supports a different value.

### Phase 4: Correct observation anchoring and generations

Feed connection epoch, sequence, sample rates, and callback anchor into the tracker. Reject stale sequences. Compute phase against the observation-time local anchor or project both phases to a common target sample.

Unify live/snapshot validity through one pure helper and add focused boundary tests.

### Phase 5: Repair tempo request policy

Implement explicit request states and network-send feedback. Test all transitions without Scene fixtures. Preserve ignored remote proposals until the proposal identity changes, and ensure disconnect clears request/prompt state.

### Phase 6: Build a real deterministic integration simulation

Create a simple test harness containing real:

- `NinjamRemoteTiming` to `ToDeviceTiming()` conversion at 44.1-to-48 kHz;
- tracker and coordinator observation;
- unified command publication/consumption;
- Timer advancement;
- model takes with lengths $L$, $2L$, $L/2$, and a non-divisor;
- bounded callback jitter and positive/negative clock error;
- generation change, disconnect, local continuation, and reconnect.

Run a few differnt intervals and assert:

- relative take offsets remain invariant;
- Timer and all takes consume the same command sequence in the same block;
- no command from an old generation moves state;
- disconnected mode emits and consumes no connected correction;
- accepted joins converge at the intended wrap;
- steady phase error stays within a documented bound;
- queued, consumed, invalidated, and rejected diagnostics reconcile exactly.

### Phase 7: Add telemetry for live validation

On the job thread, drain fixed counters/records for:

- observation age in samples;
- accepted/rejected wrap reasons;
- command generation/sequence/type;
- queued and consumed callback block;
- Timer and take phase before/after;
- tempo request transitions;
- phase-error histogram and maximum.

No per-block logging belongs in the callback.

### Phase 8: Final validation and feature support

Required automated gates:

```powershell
& .\.github\skills\builder\builder.ps1 -Target JammaLib_Tests -Action Build -RunTests `
  -TestFilter 'NinjamTiming*:Timer.*:FlipBuffer*:*TimingCorrection*'

& .\.github\skills\builder\builder.ps1 -Target JammaLib_Tests -Action Build -RunTests

powershell -NoProfile -ExecutionPolicy Bypass `
  -File .github\skills\threading-review\audio-hotpath-audit.ps1
```

Also require `git diff --check`, clean diagnostics, and manual inspection of `AudioHost::_OnAudio()`, Timer command application, `LoopTake` correction application, and disconnect teardown.

Required live matrix:

1. matching-tempo join near interval start;
2. matching-tempo join at quarter, half, and three-quarter interval;
3. local tempo push accepted, rejected, and unavailable;
4. remote tempo proposal accepted and ignored;
5. ten minutes at 44.1 kHz remote / 48 kHz device;
6. $L$, $2L$, $L/2$, and non-divisor takes, including an intentional beat offset;
7. disconnect, local continuation, and reconnect;
8. click muted throughout part of the run to prove timing publication independence.

Do not declare completion until automated command sequence reconciliation passes and live traces show no split-block consumption, stale-generation movement, repeated tempo-request loop, or regular safety rejection of valid joins.

## 5. Release Priority

Block live feature confidence on Phases 1 through 6. The same-boundary command defect, join rejection, stale correction race, Timer invalidation gap, and request-state bug are release blockers.

Telemetry and the full live matrix remain necessary before calling the remote tempo sync feature fully supported, but they must follow the correctness fixes rather than substitute for them.