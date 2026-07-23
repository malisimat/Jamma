# Remote Tempo Acceptance Local-Loop Sync Fix Plan

## Scope And Guarantees

This change fixes the initial transport rebase when an existing local scene accepts a changed NINJAM interval. At one audio-block boundary it must:

1. project the latest observed remote phase to that boundary;
2. replace the `Timer` with the accepted remote interval and projected phase; and
3. translate every existing local audio and MIDI cursor by one identical signed sample delta.

That shared translation preserves the local timeline relationship between audio and MIDI. Existing loops remain exactly aligned to later NINJAM wraps only when their physical lengths are compatible with the accepted interval (equal to it, an exact divisor, or an exact multiple as required by the musical arrangement). Accepting a genuinely different, non-commensurate tempo does not time-stretch recorded content, so permanent wrap alignment is impossible without a separate resampling/time-stretch feature.

## Review Findings

The original plan found two real defects, but coordinator-only modulus correction is not sufficient.

1. `NinjamTimingCoordinator::_AcceptTempoChange` compares the old local `Timer` phase with the new remote phase using the new interval as the circular modulus. The local phase belongs to the old master domain.
2. The pending prompt retains the remote position captured at the generation-change observation. A user may accept seconds later. Comparing that stale remote position with a live local phase, then installing it at a later callback boundary, creates an error equal to the unaccounted elapsed time.
3. `LoopTake::ApplyTimingCommand` requires a playable audio loop before it shifts MIDI state. A MIDI-only take therefore accepts the command generation but remains on the old timeline.
4. The current integration harness keeps take positions unwrapped and uses a zero replacement delta. It cannot prove physical loop wrapping, fade-offset handling, prompt-latency projection, or MIDI-only behavior.
5. While a changed tempo is awaiting a decision, later wrap observations can still produce join/discipline corrections against the old local interval. Local content must not move toward an unaccepted or rejected interval.

The existing audio-boundary fanout order is otherwise correct: `AudioHost::_OnAudio` consumes one coherent command before playback advancement, applies it to the `Timer`, and fans the same command to every non-remote station.

## Timing Contract

Use these values at the callback boundary that consumes `ReplaceTiming`:

- $M$: old local master length from `Timer::SeedSourceLength()` before replacement;
- $p$: old local master phase from `Timer::SampOffset()` before replacement;
- $N$: accepted remote interval length;
- $r_o$: remote interval position captured by the audio callback observation;
- $a_o$: monotonic `_audioSampleCounter` value at the observation block start;
- $a_b$: monotonic `_audioSampleCounter` value at the command-consumption block start.

Project the observation to the consumption boundary:

$$
r_b = (r_o + (a_b - a_o)) \bmod N
$$

Only use the elapsed term when the observation anchor is present and $a_b \ge a_o$. A missing or invalid anchor falls back to the observed phase; it must not underflow.

Map that projected phase into the old master domain and choose the existing `SignedCircularDifference` representative:

$$
d = \operatorname{SignedCircularDifference}(p, r_b \bmod M, M)
$$

If $M=0$, use $d=0$ because there is no established local master timeline to preserve. The helper's existing positive half-interval tie rule remains authoritative.

Apply the results atomically in transport terms, in this order:

1. capture $M$, $p$, and $a_b$ from the old `Timer`;
2. calculate $r_b$ and $d$ without mutating shared state;
3. replace the `Timer` using $N$ and absolute phase $r_b$;
4. fan out the identical $d$ to every local audio and MIDI consumer; and
5. continue normal block playback advancement.

For each local cursor with physical length $L_i$ and pre-command body position $q_i$:

$$
q'_i = (q_i + d) \bmod L_i
$$

The signed representative is not arbitrary. Values differing by $M$ align the master equally, but they select a different repetition inside a loop of length $kM$. Use the shortest representative returned by `SignedCircularDifference` so longer loops receive the nearest bounded translation and retain their intended cycle identity.

## Implementation Steps

### 1. Add One Pure Boundary Resolver

Add a small value result and `noexcept` timing helper beside `SignedCircularDifference` in `JammaLib/src/ninjam/NinjamTiming.h`, following this semantic signature:

```cpp
struct NinjamBoundaryTimingReplacement
{
	unsigned int RemotePhaseSamps = 0u;
	long long LocalDeltaSamps = 0;
};

inline NinjamBoundaryTimingReplacement ResolveBoundaryTimingReplacement(
	unsigned long oldMasterLengthSamps,
	unsigned int oldMasterPhaseSamps,
	unsigned int newIntervalLengthSamps,
	unsigned int observedRemotePhaseSamps,
	std::uint64_t observationAudioSample,
	std::uint64_t boundaryAudioSample) noexcept;
```

The helper returns:

- the projected absolute remote phase in the new interval; and
- the signed local rebase delta in the old master interval.

Keep it value-only, allocation-free, and independent of `Timer`. Return `{ 0, 0 }` for a zero new interval. Otherwise project the remote phase even when the old master length is zero, in which case only `LocalDeltaSamps` is zero. Reuse `SignedCircularDifference`; do not duplicate circular-difference rules in the coordinator, `AudioHost`, or tests. Use 64-bit arithmetic for anchor subtraction and elapsed-time modulo before narrowing the projected phase.

This helper is the single executable definition of the equations above and gives unit/integration tests direct access to the production math.

### 2. Preserve A Fresh Anchored Remote Phase While Prompting

Add `std::uint64_t AudioBlockStartSample = 0u` to `NinjamTiming`. Extend `ToDeviceTiming` to receive it separately from `LocalBlockStartSample`, and add a matching atomic field to `NinjamTimingObservationMailbox::Publish`/`ReadLatest`. Populate it from `AudioHost::_OnAudio`'s existing 64-bit `blockStartSample` when publishing each live timing observation. Update direct `ToDeviceTiming` callers and tests explicitly.

Do not repurpose `LocalBlockStartSample`: that field is a `Timer::AbsoluteSamplePos` anchor used by existing join/discipline comparison, and it can move with transport correction rather than representing elapsed wall-clock samples.

Add `AudioBlockStartSample` to both `NinjamTempoChange` and `NinjamClockSettings` in `JammaLib/src/ninjam/NinjamTimingCoordinator.h`. This makes the complete handoff explicit: `NinjamTiming` -> observation mailbox -> pending `NinjamTempoChange` -> accepted `NinjamClockSettings` -> scene command -> command mailbox -> audio callback.

In `NinjamTimingCoordinator.cpp`:

1. populate the monotonic anchor when `_MakeProposal` creates a tempo proposal;
2. on every later valid observation matching the pending proposal's tempo identity, refresh only its `IntervalPositionSamps` and observation anchor;
3. do not reopen or churn the prompt when only phase/anchor changes;
4. on acceptance, publish the accepted interval, grain, latest observed phase, latest anchor, and current tracker generation; and
5. stop calculating the final replacement delta in `_AcceptTempoChange`, because only the audio thread knows the exact consumption boundary.

Tempo identity remains the existing stable fields: interval length, device/source-rate identity as currently represented, grain, BPI, and BPM tolerance. Position and anchor are observation data, not proposal identity.

If the prompt is accepted without a newer matching observation, use the anchored generation-change observation. Boundary projection still accounts for its age.

### 3. Suppress Corrections For An Unaccepted Tempo

After a generation change proposes a tempo different from the active local timing, do not emit join-alignment or phase-discipline movement while that mismatch is pending or has been rejected. The generation-change invalidation must still be published so older commands cannot execute.

Define the gate from current state, not merely optional presence: while `timing.IntervalLengthSamps != clock.SeedSourceLength()` and the matching proposal is pending or ignored, skip the join/wrap delta calculation. If remote timing later returns to the active seed length, clear the stale pending/ignored mismatch state and allow the existing equal-length join path instead of prompting to replace the clock with its current interval.

Tempo-request retry/acknowledgement bookkeeping may continue, but no phase command may compare the old local `Timer` modulus with an unaccepted remote interval. Resume connected correction behavior only after a replacement is accepted, or after observations again match the active local interval under the existing join rules.

Add this as a narrow coordinator gate; do not add locks or a second command path.

### 4. Carry The Anchor Through The Existing Command Mailbox

Add one `std::uint64_t PhaseObservationSample` field to `NinjamAudioTimingCommand` and one matching atomic slot in `NinjamAudioTimingCommandMailbox::Publish`/`Consume`. This field carries `NinjamClockSettings::AudioBlockStartSample`, not `LocalBlockStartSample`.

Update `Scene::_ApplyNinjamTimingUpdate` to copy the anchor from `NinjamClockSettings` for `ReplaceTiming`. Leave join, discipline, and invalidation command semantics unchanged.

This preserves the existing ownership model:

- audio thread writes `NinjamTimingObservationMailbox`;
- scene/job/UI work under `_sceneMutex` updates coordinator state and publishes the command mailbox;
- audio thread is the sole consumer and sole mutator of callback-owned transport state; and
- mailbox sequence acquire/release ordering keeps the command coherent.

No new shared mutable object, mutex, wait, allocation, or logging belongs in this path.

### 5. Resolve And Apply Replacement At The Audio Boundary

In `AudioHost::_OnAudio`, special-case only `ReplaceTiming` before constructing the `Timer::Command`:

1. use the `blockStartSample` already captured at the top of this callback, before mailbox consumption, as the immutable monotonic boundary anchor;
2. read the old seed length and old phase before calling `Timer::ApplyCommand`;
3. call the pure boundary resolver with the command's observed phase/anchor and accepted interval;
4. set the `Timer` replacement phase to the resolver's projected remote phase;
5. retain the accepted seed length, grain, quantisation, and generation unchanged; and
6. fan the resolver's local delta to all non-remote stations instead of trusting a coordinator-time delta.

For join-alignment and phase-discipline commands, keep using the command's existing `PhaseDeltaSamps`. For invalidation, keep the zero-movement generation reset. Do not change station snapshot acquisition or playback ordering.

### 6. Make Audio And MIDI Consumers Independent

Refactor `LoopTake::ApplyTimingCommand` without changing its public API:

1. preserve invalidation, zero/stale generation filtering, and the audio-thread-only `_audioTimingGeneration` update;
2. obtain the immutable audio state snapshot if present, but do not require it for MIDI processing;
3. shift each live audio loop whose `LoopLength()` is nonzero via `Loop::ShiftPlayIndex`;
4. independently load `_midiVisualLoopLength`; when nonzero, wrap `_midiVisualPlayIndex` by that length and add the same signed delta to `_midiAnchorCorrection`;
5. increment `_consumedTimingCorrectionCount` once if at least one audio or MIDI consumer moved; and
6. leave an empty take unchanged apart from accepting the command generation.

Reuse `Loop::ShiftPlayIndex` so the `MaxLoopFadeSamps` storage offset remains encapsulated. Keep all loads/stores atomic and relaxed where the current single-audio-writer/cross-thread-observer design already permits it. Do not introduce a lock, container copy, allocation, exception, or log statement.

Do not broaden this change to the queue-based transport/global-offset path in `EndMultiPlay` unless a separate failing MIDI-only test proves that path needs the same semantic change. NINJAM replacement uses the direct unified boundary path.

## Tests

### 1. Pure Timing-Math Tests

Add tests beside the existing `NinjamTiming` circular-difference tests:

- project an old observation across several complete remote intervals and a partial interval;
- verify anchor zero and boundary-before-observation fallbacks do not underflow;
- verify $M=0$ returns zero local delta while retaining the projected remote phase;
- use non-commensurate old/new lengths to prove the local delta uses $M$, not $N$; and
- cover positive, negative, and exact half-old-interval deltas, explicitly preserving `SignedCircularDifference`'s positive-half tie rule.

For every nonzero old length, assert both the exact shortest delta and:

$$
(p+d) \bmod M = r_b \bmod M
$$

### 2. Coordinator Prompt-Freshness Tests

In `NinjamTimingCoordinator_Tests.cpp`:

- create a changed-tempo prompt with a nonzero position and anchor;
- submit a later same-tempo observation with a different phase/anchor without generating another prompt;
- accept and assert `ClockSettings` carries the refreshed phase/anchor and the original proposal's accepted tempo fields;
- assert pending and rejected changed tempos emit no join/discipline correction on later wraps; and
- retain generation, invalidation, request acknowledgement, retry, and disconnect assertions.

The coordinator test should no longer claim to validate the final boundary delta; that math now belongs to the pure helper and audio-boundary integration tests.

### 3. Audio-Boundary Replacement Integration Test

Upgrade `NinjamTimingIntegration_Tests.cpp` so its harness mirrors the new boundary resolver and wraps model cursors by their real lengths.

Use:

- an old master loop of length $M$;
- a second loop of length $2M$ with a nonzero start offset;
- a MIDI cursor with its own compatible length;
- a nonzero remote observation phase;
- a deliberately old observation anchor; and
- a command-consumption boundary several blocks later.

Assert:

1. the `Timer` lands at the projected remote phase modulo the accepted interval;
2. every local cursor lands at its old cursor plus the exact same shortest $d$, wrapped by its own length;
3. in a non-tie case, the longer loop did not jump to the alternative representative $d \pm M$;
4. pairwise phase relationships remain invariant modulo `gcd` of each pair's lengths;
5. after advancing by $M$, $2M$, and at least one accepted remote interval, every cursor equals `wrap(rebasedStart + elapsed, ownLength)`; and
6. stale and zero-generation commands remain inert.

Keep the exact half-interval tie in a separate deterministic test. Assert the existing positive-half result rather than making a cycle-identity claim when the two shortest representatives have equal magnitude.

Use a separate non-commensurate old/new interval case to expose wrong-domain arithmetic, but do not assert permanent remote-wrap alignment for that case.

### 4. Real Loop And MIDI-Only Tests

In `FlipBuffer_Tests.cpp`, reuse `TestLoopTake`, `MakePlayingLoop`, `LoopBodyPosition`, and the existing MIDI visual accessors.

- Apply a direct `TempoReplacement` command to a MIDI-only take with nonzero visual position/length. Assert wrapped visual position and signed anchor correction both move, and consumed count increments once.
- Apply the same command to a take containing audio and MIDI. Assert both move by the same signed delta and consumed count still increments once.
- Apply it to an empty take. Assert no position/anchor/count movement, while a later stale generation remains rejected.
- Cover negative and greater-than-length deltas.
- Retain or add a focused assertion that `LoopBodyPosition` is correct after `Loop::ShiftPlayIndex`, proving the fade storage prefix is preserved.

### 5. Validation Order

1. Build `JammaLib` and the test project incrementally in Debug x64.
2. Run focused filters for `NinjamTiming`, `NinjamTimingCoordinator`, `NinjamTimingIntegration`, `NinjamAudioTimingCommandMailbox`, `TimerApplyCommand`, `ExternalPhaseCorrection`, and the new direct `LoopTake` timing tests.
3. Run the full `JammaLib_Tests.exe` suite.
4. Run `.github/skills/threading-review/audio-hotpath-audit.ps1`.
5. Manually inspect `AudioHost::_OnAudio`, `Station::ApplyTimingCommand`, `LoopTake::ApplyTimingCommand`, and `LoopTake::EndMultiPlay` for new locks, waits, allocations, exceptions, or logging.

## Acceptance Criteria

- Acceptance uses a fresh anchored remote observation projected to the exact callback boundary; user prompt delay and mailbox delay do not become phase error.
- The replacement `Timer` phase equals the projected NINJAM interval phase at that boundary.
- The old local master lands at that phase modulo its own physical length using the shortest old-master circular delta.
- Every existing local audio and MIDI cursor receives that identical signed delta and wraps only by its own physical length.
- A loop longer than the master retains the intended cycle identity; an alternative delta differing by one master length is rejected by tests.
- Audio/MIDI relative phase is invariant modulo the greatest common divisor of their lengths immediately after replacement and through later local wraps.
- Equal/divisor/multiple-length local content remains aligned at compatible later NINJAM wrap points. No claim is made that non-commensurate recorded content can remain permanently synced without time stretching.
- MIDI-only takes rebase; truly empty takes do not report a consumed movement.
- No phase discipline is applied while a changed remote tempo is pending or rejected.
- Existing join alignment, steady-state phase-discipline safety limits, generation invalidation, and tempo-request state behavior remain unchanged outside that gate.
- No allocation, lock, blocking operation, exception path, or logging is added to the audio callback or take/station timing fanout.