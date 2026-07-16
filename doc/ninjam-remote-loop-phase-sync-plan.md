# NINJAM Remote Loop Phase Synchronisation Plan

## 1. Goal

Keep every local loop take phase-locked to the authoritative NINJAM interval while connected, without changing free-running/local timing while disconnected.

The connected-mode invariants are:

1. NINJAM interval zero is the authoritative master zero used by the metronome and local loops.
2. A take keeps its musical offset from that zero. A take recorded on beat 2 continues to start on beat 2 (in the case the take has same length as ninjam interval).
3. Every take receives the same signed phase correction at a remote wrap. The correction is reduced modulo that take's own length.
4. Corrections happen at most once per valid remote interval wrap, after any join alignment, and apply the complete measured error rather than a fixed-size step.
5. Steady-state corrections are small. A large or implausible measurement is treated as a generation/reset anomaly, not applied as an audible jump.
6. The audio thread owns live play-cursor mutation. Other threads may publish a pending correction but must not race its load/advance/store sequence.

For a signed correction `d`, a short take of length `L` and a long take of length `2L` must move as follows:

```text
short: (0     + d) mod L
long:  (L     + d) mod 2L
```

This shared translation preserves their relative musical phase. Independently deriving and chasing a target for each take does not provide that guarantee.

## 2. Current Path

The relevant connected path is:

```text
NJClient::GetPosition
  -> NinjamConnection::_UpdateSnapshot
  -> Scene::OnJobTick
  -> NinjamNetworkService::_FeedExternalTransport
  -> ExternalTransport::IngestSnapshot
  -> TimingQuantiser::DisciplineRemotePhase
  -> LoopTake::RebaseMasterAnchor / RepositionFromAnchor
  -> Loop::SetPlayIndex
```

The NINJAM metronome has a better-timed reference path. `AudioHost` reads `NinjamController::GetLiveTiming()` in the audio callback and `NinjamMetronomeTiming` derives beat and interval-zero onsets directly from that live NINJAM phase. Use the audible metronome as the phase oracle during manual testing.

The disconnected path does not call `_FeedExternalTransport` corrections. Preserve `Station`, `LoopTake::Play`, `Loop::EndMultiPlay`, `Timer`, and local quantisation semantics when no connected correction is pending.

## 3. Findings on the Current Branch

### 3.1 Confirmed: join alignment is calculated but not applied

`ExternalTransport::BeginJoinAlignment` calculates:

```text
AlignmentDeltaSamps = remotePhase - localPhase
```

No production code consumes `AlignmentDeltaSamps`. At the first remote wrap, `NinjamNetworkService` calls `RebaseMasterAnchor`, which deliberately preserves every current cursor. This discards the phase translation needed to move the old local master origin onto NINJAM zero. It can preserve a pre-existing local-versus-remote phase error indefinitely.

The first-wrap operation must apply one shared signed join delta to every active local take, modulo each take length. Once applied, connected tracking starts from an aligned baseline.

### 3.2 Confirmed: correction is per-take target chasing, not a shared translation

Each take derives a target from its private `_masterAnchorSample`, current remote wrap count, remote interval length, and its visual loop length. It then moves by at most `NinjamPhaseCorrectionStepSamps` (64 samples).

This has three problems:

- Two takes with different or stale anchors can receive different signed corrections at the same wrap.
- A 64-sample cap does not apply the exact measured drift. An error larger than 64 samples is carried into later intervals, producing repeated movement and possible oscillation.
- The target calculation is more stateful than the actual requirement. Drift is one master phase error, so it should be measured once and translated equally onto all takes.

Keep anchor helpers only if another feature needs them. They should not be the source of steady-state NINJAM correction.

### 3.3 Confirmed: play-cursor mutation has a lost-update race

`NinjamNetworkService` runs on the job thread and eventually calls `Loop::SetPlayIndex`. The audio thread concurrently advances the same atomic in `Loop::EndMultiPlay` with a load, arithmetic, and store sequence.

The atomic removes undefined data races but does not make the compound update atomic. This interleaving can lose either the normal block advance or the correction:

```text
audio: load old index
job:   store corrected index
audio: store old index + block size
```

Publish a pending signed correction from the job/transport side and consume it once inside `LoopTake::EndMultiPlay` on the audio thread. Do not write a playing loop cursor directly from the job thread.

### 3.4 Confirmed: MIDI correction can turn a small backward move into a large forward move

`LoopTake::RepositionFromAnchor` computes:

```text
(newMidiPos + midiLoopLength - oldMidiPos) % midiLoopLength
```

That is always a forward modular distance. A small backward correction across or near zero can become almost one complete loop, then be accumulated into `_midiAnchorCorrection`. This explains large MIDI/automation phase jumps even though the audio correction is capped at 64 samples.

Carry the original signed shared delta through the MIDI path. Shift the MIDI play index by that delta modulo its own length, and adjust `_midiAnchorCorrection` by that same signed delta. Do not reconstruct the sign from two modular positions.

### 3.5 Confirmed: current tests prove isolated helpers, not the failing workflow

The current transport and phase tests pass. They cover wrap counting, unsigned target positions, anchor round trips, fixed-step approach, and clock reseeding independently. They do not exercise:

- join delta -> first wrap -> all local takes;
- several different take lengths under one correction event;
- job-thread publication racing an audio block boundary;
- audio and MIDI receiving the same signed correction;
- noisy snapshots, length changes, duplicate/backward positions, and reconnect;
- disconnected playback remaining byte-for-byte equivalent in cursor movement.

### 3.6 High-risk hypotheses to confirm with telemetry

The following could amplify the confirmed bugs and must be measured before selecting thresholds:

- A remote interval-length change can make normalised position decrease and look like a wrap.
- The job-thread snapshot phase and `Timer::SampOffset()` may be separated by variable scheduling delay. If so, their difference is measurement noise rather than drift.
- The current threshold `max(64, intervalLength / 64)` is roughly 1,378 samples for an 88,200-sample interval. It can permit tens of milliseconds of drift before moving the clock.
- A duplicate, stale, or anomalous NINJAM position can create a false correction event.
- Audio channels within a take are expected to share one length, but correction code should still reduce against each `Loop::LoopLength()` rather than a maximum visual length.

## 4. Target Design

### 4.1 One correction event per wrap

Introduce a small value type in the timing layer, for example:

```cpp
struct RemotePhaseCorrection
{
    std::uint64_t Generation = 0;
    unsigned long RemoteWrap = 0;
    unsigned int IntervalLengthSamps = 0;
    unsigned int RemotePositionSamps = 0;
    unsigned int LocalPositionSamps = 0;
    long long DeltaSamps = 0;
    bool IsJoin = false;
};
```

Use this sign convention everywhere:

```text
DeltaSamps = shortest_signed(remotePhase - localMasterPhase, intervalLength)
```

Positive advances a local play cursor; negative moves it backward. Add a pure, unit-tested signed circular-difference helper and stop converting the result to an unsigned target until the final modulo operation.

At a valid wrap:

1. Read one coherent local/remote phase pair.
2. Calculate one signed delta.
3. Use the same delta to discipline the local `Timer` and every active local take.
4. Publish no take correction when the delta is inside the measured jitter dead-band.
5. Publish the full delta when it is valid; do not clamp it to 64 and carry the remainder across wraps.

### 4.2 Join handling

On the first valid timing snapshot, capture local and remote phase before accepting/reseeding the local clock. Preserve this as the pending join correction.

At the first valid remote wrap:

1. Validate that the interval generation and length still match the captured join.
2. Apply the pending shared join delta once to all local takes.
3. Align the local clock to the observed remote phase using the same phase model.
4. Mark the join generation committed only after the correction is queued.
5. Do not call `RebaseMasterAnchor` merely to preserve the unaligned cursor.

If the tempo/length generation changes before commit, discard the pending join and recapture it from the new stable generation.

### 4.3 Audio-thread handoff

Add a pending signed external phase delta to `LoopTake`, using the existing published-state/atomic style. The job thread only queues corrections. The audio thread consumes the accumulated delta once per block with `exchange(0)` in `EndMultiPlay`.

On consumption, in one audio-thread operation:

1. Advance each audio loop normally for the completed block.
2. Shift each loop by the same signed delta modulo that loop's own `LoopLength()`.
3. Shift `_midiVisualPlayIndex` by the same signed delta modulo `_midiVisualLoopLength`.
4. Update `_midiAnchorCorrection` using the same signed delta so MIDI events and automation remain phase-consistent.
5. Ensure all channels in a take observe the event in the same block.

Prefer a pure helper for signed modulo and a small audio-thread-owned `Loop::ShiftPlayIndex(long long)` method. It must not allocate, lock, log, throw, or traverse mutable job-thread containers.

Use an event generation or sequence number if a plain accumulated atomic delta cannot prove exactly-once delivery across reconnect/reset. A disconnect must invalidate any queued connected-mode correction before it can be consumed in local mode.

### 4.4 Snapshot coherence and anomaly policy

First try the existing job snapshot and local clock pair, but measure their skew. Log the local absolute sample before and after snapshot handling so the uncertainty is visible.

If live captures show phase noise larger than one audio block or corrections that alternate sign without real drift, publish a lock-free timing observation from the existing audio-thread `GetLiveTiming()` call. Pair it with the audio callback's local block-start sample. The job thread can consume that immutable observation to detect wraps and calculate correction. Do not add logging, allocation, a mutex, or an NJClient call to a new audio hot path.

Wrap validity rules:

- Position decreasing only counts as a wrap when length and generation are unchanged and the previous position was in a plausible end window while the new position is in a plausible start window.
- A length/sample-rate/BPM/BPI change starts a new generation and primes the detector; it is not itself a wrap.
- Duplicate snapshots do nothing.
- A backward jump away from the boundary is an anomaly: count/log it and re-prime without moving loops.
- More than one inferred wrap between observations is an anomaly unless supported by elapsed local samples.

Large-delta policy after join:

- Determine the normal dead-band from captured data, not `intervalLength / 64`.
- Start with a conservative candidate of the measured p99 phase noise plus a small margin, bounded to at most one audio block.
- Apply the complete valid delta above the dead-band.
- If `abs(delta)` exceeds a separate safety limit (initial candidate: two audio blocks), log and re-prime instead of jumping. Revisit this limit using real-session traces.

## 5. Diagnostics Phase

Implement diagnostics behind one runtime flag, off by default. Logging occurs on the job thread only. Use a structured single-line format suitable for loading into a spreadsheet.

Log once per received timing snapshot:

```text
generation, snapshotSequence, intervalLength, remotePosition,
localAbsoluteSample, localPhaseBefore, localPhaseAfter,
previousRemotePosition, wrapCandidate, wrapAccepted, rejectionReason
```

Log once per correction decision:

```text
generation, remoteWrap, isJoin, remotePhase, localPhase,
signedDelta, deadBand, safetyLimit, queued, reason
```

Log per local take only when a correction is queued and when it is consumed:

```text
takeId, eventSequence, takeLength, audioPositionBefore,
signedDelta, audioPositionAfter, midiPositionBefore, midiPositionAfter
```

Do not log per audio block. For consumption telemetry, publish a compact fixed-size diagnostic record or atomic counters from the audio thread and print it later on the job thread.

Capture at least these sessions before choosing final thresholds:

1. Connect at matching BPM/BPI near interval start.
2. Connect at matching BPM/BPI halfway through an interval.
3. Two local takes of lengths `L` and `2L`, with obvious downbeat sounds.
4. A take deliberately recorded one beat after interval zero.
5. Ten or more minutes connected to expose slow drift.
6. Remote BPM/BPI change while connected.
7. Disconnect, continue local playback, then reconnect.

Evidence of the current failure would be any of:

- committed join with nonzero delta but no equivalent queued take correction;
- different signed deltas for different takes at one wrap;
- repeated 64-sample corrections in the same direction;
- large MIDI anchor correction from a small audio delta;
- accepted wraps away from the interval boundary;
- queued correction not matching the value consumed on the audio thread.

## 6. Implementation Steps

### Step 1: Add failing pure timing tests

Extend `ExternalTransport_Tests.cpp` or add a focused remote phase-correction test file.

Cover:

- signed circular delta on both sides of zero;
- equal half-interval tie behavior, explicitly choosing one sign;
- join example `local=700`, `remote=300`, `length=1000` produces `-400`;
- length change primes a generation without emitting a wrap;
- end-to-start movement emits exactly one wrap;
- arbitrary backward movement is rejected;
- duplicate snapshots do not emit duplicate correction events.

These tests must fail against the current unsigned-target/fall-on-any-decrease behavior.

### Step 2: Add coupled multi-length correction tests

Use the existing `TestLoopTake`/`AddLoop` test fixtures. Add a narrow test-only way to create playing loops at known positions if the public state machine makes setup impractical; do not broaden production APIs solely for tests.

Cover one event applied to takes of `L`, `2L`, and a non-multiple length:

```text
before: short=0, long=L
delta:  +2
after:  short=2, long=L+2
```

Also cover negative correction across zero, all channels in one take receiving the event, MIDI receiving the identical signed delta, and two queued events being consumed exactly once.

### Step 3: Pin disconnected behavior before changing ownership

Add regression tests showing that with no pending external correction:

- `EndMultiPlay(N)` advances audio and MIDI exactly as before;
- recording, overdub, punch, fade-tail, and local quantisation setup are unchanged;
- disconnect/reset clears or invalidates a correction queued but not yet consumed;
- no NINJAM timing state is persisted in jam files.

### Step 4: Harden transport generations and wrap detection

Modify `ExternalTransport` so generation changes reset `_hasLastPos` and establish a baseline. Return or publish an explicit wrap/correction decision rather than making callers infer an event by reading `RemoteWrapCount` before and after atomic publication.

Keep publication immutable. Remove fields that remain dead after the new path is complete, especially misleading master-position fields or pending alignment data that is no longer consumed.

### Step 5: Return one signed clock correction

Refactor `TimingQuantiser::RemotePhaseCorrectionOffset`/`DisciplineRemotePhase` around the signed correction value. The operation that moves the `Timer` should return the exact signed delta it applied, or consume a precomputed `RemotePhaseCorrection`, so loop and clock correction cannot disagree.

Replace the interval-scaled threshold only after diagnostics establish a defensible noise floor. Add tests around the selected dead-band and safety policy.

### Step 6: Add the real-time-safe take handoff

Add pending correction publication to `LoopTake`. Consume it in `EndMultiPlay`, and add the minimal audio-thread-owned loop shift primitive. Apply one signed value to every audio loop and to MIDI.

Remove direct job-thread calls to `Loop::SetPlayIndex` from connected synchronisation. Delete `ApproachTakePosition` and `NinjamPhaseCorrectionStepSamps` if no caller remains.

Run the threading hot-path audit immediately after this step and manually inspect `Loop::EndMultiPlay` and `LoopTake::EndMultiPlay` for allocation, locks, logging, or new unbounded work.

### Step 7: Apply join and steady-state events

Change `NinjamNetworkService::_FeedExternalTransport` to:

1. validate/prime the timing generation;
2. capture the pre-reseed join delta;
3. on the first valid wrap, queue that join delta once;
4. on later wraps, calculate one shared drift delta;
5. discipline the clock and queue the identical value to every local take;
6. skip remote stations;
7. invalidate pending events on disconnect or generation reset.

Do not call `RebaseMasterAnchor` as a substitute for join correction.

### Step 8: Fix MIDI signed correction

Remove the unsigned forward-distance reconstruction. The MIDI cursor and `_midiAnchorCorrection` must consume the signed event directly. Add tests for `old=5000`, `delta=-4900`, `length=30000` and small corrections crossing zero in both directions.

### Step 9: Add diagnostics and counters

Add the runtime flag and structured job-thread output described above. Include counters for accepted wraps, rejected wrap candidates, join corrections, steady corrections, safety-limit rejections, queued events, and consumed events.

The normal steady-state expectation is one accepted wrap per NINJAM interval, equal queued/consumed event counts, and corrections clustered near zero without alternating large signs.

### Step 10: Remove obsolete anchor sync code after proof

Once coupled tests and live validation pass, remove NINJAM-only anchor chasing that has no remaining caller. Keep generally useful pure helpers only when tests document a current use. Avoid carrying two correction mechanisms that can fight each other later.

## 7. Automated Validation

After each implementation step, build the smallest affected targets incrementally:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' `
  JammaLib\JammaLib.vcxproj /m /t:Build /p:Configuration=Debug /p:Platform=x64 `
  "/p:SolutionDir=$((Get-Location).Path)\"

& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' `
  test\JammaLib_Tests\JammaLib_Tests.vcxproj /m /t:Build `
  /p:Configuration=Debug /p:Platform=x64 `
  "/p:SolutionDir=$((Get-Location).Path)\"
```

Run focused tests during iteration, then the full native suite:

```powershell
& .\test\JammaLib_Tests\bin\x64\Debug\JammaLib_Tests.exe `
  --gtest_filter='ExternalTransport*:RemotePhaseCorrection.*:DisciplineRemotePhase.*:*RemoteLoopPhase*:*ExternalPhase*'

& .\test\JammaLib_Tests\bin\x64\Debug\JammaLib_Tests.exe
```

Run the real-time audit:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .github\skills\threading-review\audio-hotpath-audit.ps1
```

Add a deterministic simulation test that runs thousands of intervals with:

- local clock rate errors on both sides of remote rate;
- varied job snapshot cadence and bounded jitter;
- takes of `L`, `2L`, `L/2`, and a non-divisor length;
- a fixed one-beat take offset;
- wraparound of every take and the remote interval;
- one tempo generation change and one reconnect.

At every correction event assert:

```text
appliedDelta(take i) == sharedDelta modulo takeLength(i)
relativeOffset(i, j) is unchanged by correction
beat-offset take remains one beat from remote zero
abs(steadyStatePhaseError) <= selected dead-band
no correction outside accepted wraps
no correction while disconnected
```

## 8. Manual Live Validation

Keep a temporary runtime escape hatch selecting old versus new connected correction. It must default to the new path only after the following checks pass; disconnected behavior must not depend on the flag.

### Session A: basic phase and join

1. Start locally with the metronome and one obvious kick loop at local beat 1.
2. Connect halfway through a remote interval at matching BPM/BPI.
3. Confirm no movement before the accepted remote wrap.
4. At that wrap, confirm one join correction and no repeated catch-up steps.
5. Confirm the kick and accented NINJAM interval click coincide thereafter.

### Session B: preserved beat offset

1. Record a loop whose obvious transient starts on remote beat 2.
2. Run for at least 20 intervals.
3. Confirm it remains on beat 2 after every correction and after crossing its own loop zero.

### Session C: different lengths

1. Use audible takes of `L`, `2L`, and `L/2`.
2. Put a distinct marker at zero and halfway through the `2L` take.
3. Confirm that when the `L` take is corrected to `+d`, the `2L` take is corrected from either `0` to `+d` or `L` to `L+d`, never to the wrong half.
4. Run for at least 10 minutes and inspect the correction log for equal deltas and stable signs.

### Session D: tempo and connection lifecycle

1. Change remote BPM/BPI.
2. Confirm the old generation emits no correction after the change.
3. Confirm the new generation primes, aligns once, then settles.
4. Disconnect and verify local playback and local metronome behavior are unchanged.
5. Reconnect at a different interval phase and confirm exactly one new join alignment.

### Acceptance criteria

- No local correction occurs except at an accepted NINJAM wrap.
- Join produces at most one deliberate shared translation.
- Every active local take consumes the same signed event exactly once.
- No steady-state event exceeds the safety limit.
- No repeated fixed-size catch-up staircase appears.
- Audio, MIDI, automation, and visuals remain on the same phase.
- The accented NINJAM click and intended local beat stay audibly coincident for at least 10 minutes.
- `L`, `2L`, and shorter takes retain their musical relationship throughout.
- Disconnected recording, playback, overdub, punch, local tempo, and local metronome tests remain unchanged.

## 9. Expected Files

Keep the implementation focused around:

- `JammaLib/src/timing/ExternalTransport.h/.cpp`
- `JammaLib/src/timing/TimingQuantiser.h/.cpp`
- `JammaLib/src/ninjam/NinjamNetworkService.h/.cpp`
- `JammaLib/src/engine/LoopTake.h/.cpp`
- `JammaLib/src/engine/Loop.h/.cpp`
- `test/JammaLib_Tests/src/timing/ExternalTransport_Tests.cpp`
- `test/JammaLib_Tests/src/timing/RemotePhaseDiscipline_Tests.cpp`
- one focused engine test file for audio/MIDI correction handoff

Touch `AudioHost`/`NinjamConnection` only if diagnostics prove the job-thread timing pair is too incoherent. In that case, add only a lock-free immutable live timing observation; do not move transport policy or loop ownership into the app/audio wiring layer.

## 10. Baseline

On this branch before implementation, the focused command below passes 31 tests:

```text
ExternalTransport*:RemotePhaseCorrection.*:DisciplineRemotePhase.*
```

That green baseline must remain green until individual obsolete tests are deliberately replaced by stronger signed/coupled behavior tests. Record any unrelated full-suite failures separately; do not broaden this change to fix them.