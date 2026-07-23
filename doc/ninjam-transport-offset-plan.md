# Local Transport Offset Plan

## Goal

Provide one scene-wide, persisted control that adjusts the absolute phase of all
local audio and MIDI playback relative to the active NINJAM transport. The value
is a normalized position in the current master-loop domain:

$$
f \in [0, 1]
$$

where $f = 0$ means no local offset and $f = 1$ means one complete master-loop
offset, which is cursor-equivalent to $0$ after wrapping. For a master length
$M$, changing the control from $f_0$ to $f_1$ applies the shared local cursor
translation:

$$
d = \operatorname{round}((f_1 - f_0)M)
$$

The remote Timer is not moved. Every non-remote local take moves by $d$ in its
own loop modulus, so local relative timing is preserved while local playback is
phase-shifted relative to the remote interval.

## Current State

The control is wired through `Scene`, `Station`, session persistence, and the
take correction queue:

- `Scene` exposes a numeric field and stores `TransportOffsetLoopFrac`.
- `Station::SetTransportOffsetLoopFrac` converts a change in fraction into a
  sample delta using `Timer::SeedSourceLength()`.
- Existing local takes receive a queued correction; remote stations are
  excluded.
- New recording/overdub operations include the offset in their MIDI transport
  start timestamp.
- The session exporter writes the fraction and `JamFile` restores it.

This gives existing audio takes the intended broad behavior, but it does not
yet provide a coherent, complete scene-wide transport operation.

## Problems

### 1. MIDI-only takes do not consume the offset

`LoopTake::EndMultiPlay` only applies a queued transport correction when a take
has a playable audio loop. A MIDI-only take retains the pending correction and
does not shift its MIDI visual cursor or anchor correction. It therefore drifts
relative to shifted local audio.

### 2. The operation is not atomic at the audio boundary

`Scene` iterates stations on the UI thread and each station queues corrections
individually. The audio callback consumes them later, after normal playback
advancement. A callback can observe a partial scene update, and a manual offset
can race with a simultaneous NINJAM replacement or phase-discipline command.

The existing NINJAM command path already applies one timing operation to the
Timer and all local takes before any playback advancement. The transport offset
must use an equivalent audio-boundary path.

### 3. The value range is ambiguous

The UI, `Scene`, `Station`, and `JamFile` currently accept `[-1, 1]`. Because
the control represents a circular master-loop phase, negative values duplicate
the positive range: `-0.25` and `0.75` identify the same wrapped position. This
does not meet the desired normalized `0..1` contract.

### 4. Remote replacement needs an explicit policy

The normalized value is currently converted at the time the field is edited.
A later remote timing replacement may rebase local loops to the new remote
phase without accounting for the user-selected local-versus-remote offset.

The intended policy should be: a nonzero transport offset remains active across
remote timing replacement. The replacement target for local cursors must include
the configured local offset, while the Timer still receives the unmodified
absolute remote phase. Ongoing phase-discipline corrections then preserve that
relative offset because they translate the Timer and all local takes together.

## Design

### Canonical value and persistence

1. Rename or document `TransportOffsetLoopFrac` as a normalized cyclic phase
   fraction, constrained to `[0, 1]`.
2. Change the numeric control to `Min = 0.0`, `Max = 1.0`, retaining an
   appropriately fine step such as `0.005`.
3. Use one shared normalization helper for UI input, Scene state, Station state,
   deserialization, and export validation.
4. Migrate old negative saved values by wrapping them into `[0, 1)`:

   $$
   f_{canonical} = ((f_{legacy} \bmod 1) + 1) \bmod 1
   $$

   Preserve an explicitly entered or persisted `1.0` for display if useful;
   cursor math may treat it as equivalent to `0.0`.

### Audio-boundary command

1. Add a local-only transport-offset payload to the audio timing command
   mechanism, or add a dedicated single-slot mailbox consumed at the same top
   of `AudioHost::_OnAudio` boundary.
2. The command contains only the shared signed `PhaseDeltaSamps`; it does not
   modify the Timer.
3. On consumption, fan the delta to every non-remote station before station
   playback advances.
4. Define deterministic coexistence with a NINJAM timing command in the same
   callback. Prefer a composed command/payload that can carry both operations;
   do not allow one single-slot mailbox publication to silently overwrite the
   other.
5. Preserve no-allocation, lock-free callback behavior. The UI thread computes
   and publishes the delta; the callback only consumes and applies it.

### Take-level application

1. Refactor the correction application so audio and MIDI have independent
   eligibility checks.
2. Shift every playable audio loop by the shared delta.
3. When a MIDI visual loop exists, always shift `_midiVisualPlayIndex` by the
   same delta in its own loop modulus and apply the same signed change to
   `_midiAnchorCorrection`, even if no playable audio loop exists.
4. Leave an empty take unchanged.
5. Count a correction as consumed when at least one audio or MIDI consumer
   moved.

### NINJAM replacement integration

1. Thread the scene's normalized transport offset into the code that authors a
   `ReplaceTiming` command, rather than changing `AudioHost` ordering.
2. When accepting a remote timing replacement, retain the existing old-master
   local rebase calculation for the base remote phase.
3. Augment the local-take correction by the configured transport offset in the
   chosen master-loop domain. The Timer still receives the raw remote absolute
   phase.
4. Make this calculation explicit in a helper with named inputs: old physical
   master length, old local phase, remote absolute phase, and normalized local
   offset. This avoids mixing the old local-loop modulus with the new remote
   interval modulus.
5. Preserve the existing join-alignment and steady-state phase-discipline
   behavior. Those commands should not reapply the configured offset; they move
   Timer and local takes together, thereby retaining it.

## Implementation Sequence

1. Add a focused normalization helper and update the Scene control, Scene
   setter, Station setter, and `JamFile` load/export paths to use `[0, 1]`.
2. Introduce the coherent local transport-offset command and publish it from
   `Scene::_SetTransportOffsetLoopFrac` instead of queuing each station directly
   on the UI thread.
3. Update `AudioHost` to consume the offset at the top of the callback and fan
   it out to non-remote stations in the same callback block.
4. Fix `LoopTake` so MIDI-only takes consume the command correctly.
5. Integrate the persistent offset into accepted remote timing replacements,
   maintaining the established old-master rebase rules.
6. Retain or remove the older per-take queued correction path only after all
   remaining callers have a clear purpose. Do not leave two active paths that
   can apply the same user offset twice.

## Test Plan

### Unit tests

Add tests covering:

- `0.0`, `0.25`, and `1.0` normalization and conversion to sample deltas.
- Legacy negative session values wrapping into the canonical range.
- A local audio take changing from `0.0` to `0.25`, then returning to `0.0`,
  with exact wrapped cursor positions.
- Audio and MIDI positions moving by the same signed delta for loops of
  different lengths.
- A MIDI-only take moving both visual position and MIDI anchor correction.
- An empty take remaining unchanged.

### Audio-boundary integration tests

Add a harness test showing that all local takes consume one manual offset in one
callback before normal advancement, while remote stations and the Timer remain
unchanged. Also publish a manual offset and a NINJAM command in the same block
to prove the chosen composition/order contract.

### Remote replacement regression

Seed local loops with a nonzero configured offset, accept a non-commensurate
remote interval at a nonzero phase, and assert:

$$
q_i' = (q_i + d_{replace} + d_{offset}) \bmod L_i
$$

for every local audio and MIDI loop. Verify the Timer lands at the raw remote
absolute phase and that later phase-discipline corrections retain the selected
local-versus-remote offset.

## Validation

Run the focused `TransportPhaseOffset`, `ExternalPhaseCorrection`, NINJAM timing
coordinator, and timing integration tests. Then build and run the complete
`JammaLib_Tests.exe` suite because this code participates in shared callback-time
transport behavior.