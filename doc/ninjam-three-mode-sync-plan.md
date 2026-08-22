# NINJAM Three-Mode Local Sync Plan

## Objective

Replace the historical geometry-based follow policies with three user-facing
transport behaviors while preserving every local audio and MIDI loop's
intentional phase:

- `ContinuousSync`: adopt remote timing when the accepted remote BPM is equal
  or within `1.0` BPM of local timing. Correct every audio block from one shared
  master-relative phase map, producing small cursor adjustments until genuine
  rate adjustment is implemented.
- `BlockSync`: adopt materially different remote timing and use the same shared
  phase map every audio block. Larger repeat/skip corrections are expected and
  explicitly accepted as the current broken-record fallback.
- `NoSync`: retain/free-run local timing and disable any active remote phase
  map. Neither Timer geometry nor local loop cursors follow remote timing.

Tempo-request acknowledgement remains a separate decision: a fresh,
successfully-sent, same-BPI observation matches the request when its BPM is
within the inclusive fixed tolerance `+/- 1.0 BPM`. This models NINJAM's
integer BPM rounding. It is not a percentage tolerance.

## Verified Current Behavior and Defects

1. `NinjamTimingCoordinator` already uses the correct fixed inclusive `1.0f`
   acknowledgement comparison, but boundary tests do not lock the exact
   `+/-1.0` edges.
2. `AudioHost` currently chooses among `SeamlessDiscipline`,
   `ContinuousRemote`, and `BoundaryRestore` from exact loop divisibility. That
   is loop geometry compatibility, not the requested tempo-distance behavior.
3. The uncommitted `BoundaryRestore` implementation already establishes the
   right basic mechanism: capture each audio body cursor and MIDI event cursor,
   then derive both from one scaled local-master/remote-master elapsed time on
   every audio block.
4. A later join/discipline correction is currently overwritten by the original
   per-block map. The map must be rebased after every accepted common phase
   correction so the correction persists.
5. `StayLocal` is not a complete policy: rejecting a proposal emits no command,
   so an already-active per-block map can remain active. `NoSync` must explicitly
   cancel the map, and a no-sync command must never replace Timer geometry.
6. Audio uses `Loop::BodyPlayIndex`; MIDI event playback uses
   `_midiVisualPlayIndex`. Every MIDI cursor translation must apply its inverse
   to `_midiAnchorCorrection`, including per-block repeat/skip movement, so MIDI
   automation remains aligned with emitted MIDI events.

## Design

### 1. Policy ownership and classification

Define exactly this enum in `NinjamAudioTimingCommand.h`:

```cpp
enum class NinjamLocalFollowPolicy : std::uint8_t
{
    ContinuousSync,
    BlockSync,
    NoSync
};
```

Select the policy on the job thread, where both local and accepted remote tempo
metadata are available, rather than inspecting loop lengths in the callback:

```text
no local timing/content              -> ContinuousSync
abs(remote BPM - local BPM) <= 1.0  -> ContinuousSync
otherwise                            -> BlockSync
explicit reject/disconnect           -> NoSync
```

The same named `1.0 BPM` constant may be shared by request acknowledgement and
near-tempo policy classification, but keep the two predicates separate. BPI
equality remains mandatory only for acknowledgement. Once the user explicitly
chooses to follow a different remote BPI/tempo, that is `BlockSync`.

Carry the selected policy in `NinjamClockSettings`, copy it through
`Scene::_ApplyNinjamTimingUpdate`, and publish it in the existing fixed-size
`NinjamAudioTimingCommand` mailbox. Phase-correction commands inherit the
audio-thread-owned active policy; they do not reclassify.

Remove callback classification based on `IsRemoteTimingCompatible`. If the
compatibility helpers become unreferenced, remove their `Station`/`LoopTake`
fan-out and tests only where they are now dead. Keep the pure alignment helpers
that are still used for capture/restore math.

### 2. Shared master-relative phase map

Use the same map for both `ContinuousSync` and `BlockSync`; policy names describe
correction magnitude, not two independent algorithms.

At the callback boundary that accepts a timing replacement:

1. Read the previous local master length `M_local` and common scene coordinate
   `S0` before replacing Timer geometry.
2. Resolve the remote phase at the actual callback boundary and apply the one
   signed local delta to every playable local audio body cursor and MIDI event
   cursor. Do not set any loop to zero.
3. Replace Timer geometry/phase.
4. Capture each loop's post-correction phase as its phase-map origin. Preserve
   its individual length and offset.
5. Store `M_local`, accepted remote master length `M_remote`, and `S0` as
   audio-thread-owned fixed-size state.

For every later audio block at scene coordinate `S`:

```text
elapsed = S - S0
mappedLocalElapsed = round(elapsed * M_local / M_remote)
target_i = (originPhase_i + mappedLocalElapsed) mod loopLength_i
```

Set every audio loop's `BodyPlayIndex` and the MIDI cursor from this target
before audio/MIDI block reads. This produces one common mapped progression for
all lengths, preserving intentional relative alignment. Use integer arithmetic
with an explicit round-to-nearest rule and no floating-point accumulator, so
there is no cumulative drift.

When a `JoinAlignment` or `PhaseDiscipline` command applies a common delta:

1. Apply that delta to Timer and every audio/MIDI cursor once.
2. Rebase the phase-map origin at the current scene coordinate from the newly
   corrected cursor positions while retaining `M_local/M_remote`.

This prevents the next per-block restore from undoing the correction. A zero
delta does not need a rebase unless the command changes timing geometry.

For a loop/MIDI loop created after sync begins, initialize its map origin on its
first callback encounter so its current phase is preserved, rather than leaving
it free-running or snapping it to zero.

### 3. MIDI automation invariant

Create one small `LoopTake` helper for moving the MIDI event cursor that:

- wraps the target by the MIDI loop's own length;
- computes the actual signed cursor translation used;
- stores the cursor; and
- subtracts the same translation from `_midiAnchorCorrection` exactly once.

Use it from ordinary timing corrections and per-block map restoration to avoid
divergent audio/MIDI behavior. The effective automation phase must remain equal
to the MIDI event phase modulo the MIDI loop length across normal movement,
cursor wraps, repeat corrections, and skip corrections.

### 4. NoSync lifecycle

- An explicit `Stay local` decision publishes/carries `NoSync` invalidation so
  `AudioHost` clears the active phase map without moving Timer or loop cursors.
- Disconnect/invalidation also sets the active policy to `NoSync`, clears map
  lengths/origins, and invalidates connection-scoped anchors.
- A `NoSync` replacement-like command must return before applying either Timer
  geometry or station movement. Prefer an explicit invalidation command instead
  of publishing remote geometry with `NoSync`.
- A later accepted follow transition captures fresh phases from the current
  free-running local positions.

### 5. Naming, diagnostics, and documentation

- Rename active-map fields and methods from `BoundaryRestore*`/
  `RemotePhaseMap` to policy-neutral `SyncPhaseMap*` names.
- Update Scene diagnostics to print `continuous-sync`, `block-sync`, and
  `no-sync`. A per-block restore must not log from the callback.
- Update `doc/loop-alignment-and-ninjam-sync.md` to describe the three modes,
  fixed `1 BPM` acknowledgement, per-block integer phase map, rebase behavior,
  and the deliberate repeat/skip limitation before rate adjustment.
- Preserve the latest-wins mailbox and published immutable station/take
  snapshots. Add no allocation, locks, exceptions, formatting, or I/O to the
  callback.

## Implementation Work Packages

### Package A: policy model and coordinator

- Replace the four enum values and defaults with the three values.
- Add policy to `NinjamClockSettings` and propagate it to audio commands.
- Change `_AcceptTempoChange` to receive current local timing and choose
  `ContinuousSync` versus `BlockSync` from absolute BPM difference.
- Keep `_MatchesRequest` at inclusive fixed `<= 1.0 BPM` and exact BPI.
- Make explicit reject/disconnect stop sync through `NoSync` invalidation.
- Update enum-dependent logs and unit tests.

### Package B: callback and loop phase map

- Remove geometry/divisibility policy selection from `AudioHost`.
- Activate the shared map after all accepted replacements for both sync modes.
- Apply common deltas uniformly, then initialize/rebase the map so corrections
  persist.
- Restore audio and MIDI from the same mapped elapsed value before block reads.
- Centralize MIDI cursor/automation translation.
- Reset all map state for `NoSync`/invalidation/reconnect.
- Remove dead compatibility fan-out if no longer referenced.

### Package C: tests and docs

- Update old policy names in all tests and documentation.
- Add exact acknowledgement edge tests: requested `120`, observed `119` and
  `121` accept; values just beyond those edges do not acknowledge.
- Add policy-selection tests: equal, `<1`, exactly `1`, and `>1 BPM`; manual
  materially different follow is `BlockSync`; rejection is `NoSync` and leaves
  Timer/cursors unchanged.
- Replace the current single direct phase-map test with multi-block tests over
  multiple remote wraps for both modes. Use differing, non-divisible audio and
  MIDI loop lengths and nonzero initial offsets.
- Assert every block against the exact master-relative formula, and assert the
  pairwise/relative phase invariant for audio and MIDI.
- Apply a nonzero discipline correction mid-run and prove it remains present on
  subsequent blocks.
- Assert MIDI event cursor and effective automation phase remain equivalent
  across cursor wrap/repeat/skip.
- Test invalidation followed by local free-run and reconnect to prove fresh map
  capture.

## Validation

1. Run `git diff --check`.
2. Incrementally build `JammaLib` and `JammaLib_Tests` Debug x64 with an absolute
   `SolutionDir` ending in exactly one backslash.
3. Run focused tests:

```powershell
JammaLib_Tests.exe --gtest_filter="NinjamTimingCoordinator.*:NinjamAudioTimingCommand.*:NinjamTimingIntegration.*:TransportPhaseOffset.*:NinjamLoopAlignment.*"
```

4. Run the complete native test executable.
5. Manually inspect all modified callback/hot-path functions against
   `doc/realtime-audio.md`.
6. Real-server acceptance check for a decimal local tempo rounded by NINJAM:
   logs must show `acknowledged`, `continuous-sync`, and stable audio/MIDI phase
   against the canonical metronome. A deliberately different accepted tempo
   must show `block-sync` and preserve local relative loop offsets while making
   audible repeat/skip corrections.

## Deferred Work

Actual audio resampling and MIDI event-rate remapping are intentionally out of
scope. The per-block cursor map eliminates accumulated phase drift but can
repeat or skip source material; `ContinuousSync` makes these corrections small,
while `BlockSync` permits them to be conspicuous.
