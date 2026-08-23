# NINJAM Transport, Loop Alignment, and VST Timeline Continuity

## Status and goal

This is the implementation plan for the timing failure exposed when Jamma
pushes a local tempo to an empty NINJAM server and then accepts that timing.
The change is complete only when all of these are true:

- local audio and MIDI loops of different lengths retain every intentional
  relative phase;
- at zero scene transport offset, the local master loop reaches sample zero at
  every NINJAM interval wrap, including when the accepted interval length is
  different from the pre-join local master length;
- the scene transport-offset control moves local loop zero by its one intended
  amount and is not lost or applied twice during a timing replacement;
- VST2 and VST3 plugins receive a continuous, truthful host sample timeline and
  only fields whose validity is actually advertised;
- all live mutations remain coherent at one audio-block boundary, with no new
  allocation, lock, exception, or logging on the callback.

Read `doc/loop-alignment-and-ninjam-sync.md` and `doc/realtime-audio.md` before
editing. Update the former if implementation changes any documented equation.

## Confirmed defects and risks

### Replaceable transport time is being sent to plugins

`Timer::ApplyCommand(ReplaceTiming)` replaces interval geometry and resets the
timer loop count. `Timer::AbsoluteSamplePos()` can consequently fall from a
multi-minute local epoch to the current remote phase. `Station::_RunVstBlock`
publishes that value as VST2 `VstTimeInfo::samplePos` and VST3
`ProcessContext::projectTimeSamples`. This is a real plugin-facing locate even
though Jamma only intended to establish a new musical phase relationship.

`Timer::SceneSamplePos()` is a `uint64_t` callback-owned sample ruler which is
not reset by timing replacement. It is the appropriate Jamma project/continuous
sample timeline. Do not replace the rewind with an arbitrary forward jump.

### Remote phase must be mapped into the local source-master ruler

The accepted timer uses the new NINJAM interval length `M_r`, while existing
loop buffers remain in the pre-acceptance local source ruler `M_l`. The current
replacement resolver compares the remote phase directly with the old local
phase. That is only correct when `M_l == M_r`.

Define the source-master phase which is guaranteed to finish exactly at the
next remote wrap using the same integer mapping as `RestoreSyncPhaseMap`:

```text
mapElapsed(e, M_l, M_r) =
    floor(e / M_r) * M_l
    + round((e mod M_r) * M_l / M_r)

sourcePhaseAtRemotePhase(R, M_l, M_r) =
    (M_l - mapElapsed(M_r - R, M_l, M_r)) mod M_l
```

Use the complement form above, rather than an independently rounded
`R * M_l / M_r`, so the exact integer algorithm guarantees:

```text
(sourcePhaseAtRemotePhase(R) + mapElapsed(M_r - R)) mod M_l == 0
```

At replacement, `Timer::SampOffset()` becomes `R` in the new remote ruler, but
the common correction sent to all local takes is the signed circular delta from
the old local source-master phase to `sourcePhaseAtRemotePhase(R)`.

Ongoing join/discipline deltas are also expressed in the remote ruler. Do not
apply their raw sample count to source loops when the rulers differ. Restore the
current map first, calculate the source-master target from the timer phase
before and after the remote correction, apply that signed circular source delta
once to every local take, and then rebase the map at the same scene coordinate.
This avoids a one-sample rounding disagreement and makes the next-wrap
invariant the authority.

The local source-master length must remain stable for the lifetime of a follow
map. A later replacement while already following must not overwrite `M_l` with
the previous remote timer length. Explicitly define the lifecycle:

- capture `M_l` from the local timer only when establishing a new independent
  follow map;
- retain it across replacement, join, and discipline rebases in that follow
  session;
- clear it with map/anchor invalidation;
- before supporting reconnect after invalidation, verify where the durable
  local source-master geometry comes from. Do not silently treat the last
  remote interval as a loop-buffer ruler. If no durable owner currently exists,
  add one beside the map state and update it only on genuine local timing
  seeding/tempo changes.

### The UI transport offset is loop-local, not part of Timer phase

Let `O` be the normalized UI offset converted to samples in the current local
master ruler. The correct relations are:

```text
Timer remote phase:       P = R                  (mod M_r)
zero-offset source phase: B = sourcePhaseAtRemotePhase(R, M_l, M_r)
local master-loop phase:  Q_master = B + O       (mod M_l)
other loop phase:         Q_i = base_i + O       (mod L_i)
```

Do not use `P = R - O`. Timer/NINJAM phase is independent of this local UI
control. With `O == 0`, the local master reaches zero at the remote wrap. With a
non-zero offset, the loop phase at that wrap is exactly `O` modulo its own
length; equivalently its zero point moves by the documented cyclic amount.

There is an ordering hazard in the current callback. A replacement starts the
sync map, then recalculates the sample target for the UI offset using the new
master length. Applying the offset after capturing map anchors allows the
same-block restore to undo that offset, while `_appliedLocalTransportOffsetSamps`
already says it was applied. Resolve both mailboxes and the new offset target at
the same boundary, then order mutations as follows:

1. restore the old map at the block-start scene coordinate when one exists;
2. apply the timer command and derive the one common source-ruler correction;
3. apply that correction once to every local take;
4. apply the change from the old absolute UI-offset target to the new target
   once to every local take;
5. capture/rebase the new map anchors only after both shifts;
6. perform normal playback and end-of-block advancement.

An equivalent implementation may rebase anchors after a later offset update,
but no restore may use anchors captured before the final same-boundary shifts.
Only the audio thread may mutate live loop cursors and applied-offset state.

## Coordinate and ownership contract

Keep these coordinates distinct:

| Coordinate | Meaning | Owner and behavior |
| --- | --- | --- |
| `S` | scene/project sample coordinate | `Timer::SceneSamplePos`; `uint64_t`, callback-owned, monotonic for the timer lifetime |
| `R` | accepted NINJAM interval phase | `[0, M_r)`, projected to the current callback boundary, wraps remotely |
| `P` | Jamma timer phase | `Timer::SampOffset`, uses `M_r` while following, equals `R` after accepted correction |
| `B` | zero-offset local source-master phase | mapped from `R` into `M_l` by the exact complement equation |
| `Q_i` | audio body or MIDI event-cursor phase | shifted by common source elapsed/correction, then wrapped by its own `L_i` |
| `O` | scene transport offset | an absolute local take shift derived once from the normalized UI value |

Audio uses `Loop::BodyPlayIndex`, never fade-prefixed storage `PlayIndex`. MIDI
uses `LoopTake`'s event/visual cursor. Every cursor translation must continue to
apply the inverse change to `_midiAnchorCorrection`, keeping MIDI automation
phase equal to event phase.

All timer, take, offset, and map mutation happens on the audio thread. Cross-
thread publishers continue to use the fixed-size latest-wins mailboxes. Do not
add a mutex, container rebuild, dynamic allocation, or plugin call to command
resolution.

## VST host policy

### Sample timeline

Add an explicitly named, pure/noexcept host-time resolver and publish the
block-start `S` to `HostTimeState`. Use a 64-bit integer internally and convert
to the SDK type only at the adapter boundary. Do not narrow the host sample
position through the existing `uint32_t blockStartSample`; that counter wraps
after roughly 24.9 hours at 48 kHz. MIDI block timestamps may retain their
existing type in this change, but any narrowing must be explicit and must not
feed VST project time.

For VST3, publish `S` as both `projectTimeSamples` and
`continousTimeSamples`, advertise `kContTimeValid`, and construct `state` from
the SDK enum constants. The current literal `state = isPlaying ? 1u : 0u` is
incorrect because VST3 `kPlaying` is `1 << 1`. When tempo and time signature
are populated, advertise `kTempoValid` and `kTimeSigValid`.

For VST2, publish `S` as `samplePos` and preserve `kVstTransportPlaying`.
NINJAM replacement is not a sample-timeline locate, so do not set
`kVstTransportChanged` for it. A future real user transport locate must be
modelled explicitly and signal the SDK-defined discontinuity for one block.

### Musical position honesty

The current VST2 adapter derives `ppqPos` directly from `samplePos * tempo` and
marks it valid. Across a tempo replacement this does not describe a tempo map
or a remote-aligned musical origin. VST3 currently writes musical parameters
without their validity flags and does not publish project musical position.

For this fix, choose one coherent, tested policy:

1. Preferred bounded scope: keep tempo/time signature valid, stop advertising
   VST2 PPQ as valid, and do not advertise VST3 project/bar musical position.
   Fields without a validity bit are ignored. This is truthful and prevents a
   tempo-synced sequencer from acting on a fabricated position.
2. If plugin song-position alignment is implemented now, add one shared
   audio-thread-owned musical timeline, anchored at `(S, R, BPM, BPI)`. It must
   produce a non-wrapping PPQ whose phase modulo `BPI` matches the remote
   interval, choose a bounded forward interval epoch on first acceptance, and
   explicitly signal any later discontinuity. Both VST2 and VST3 must receive
   the same PPQ/bar semantics and matching validity flags.

Do not leave VST2 PPQ marked valid with the old direct formula after switching
sample time to `S`. Do not reset PPQ to zero at every NINJAM wrap. Full musical
timeline support may be a follow-up only if policy 1 is implemented in this
change and covered by adapter tests.

## Implementation sequence

### 1. Add failing characterization tests

Before changing production code, add tests that demonstrate:

- replacement preserves `SceneSamplePos` while `AbsoluteSamplePos` may reset;
- with `M_l != M_r` and a non-zero observed `R`, the mapped local source master
  reaches exactly zero after the remaining remote samples;
- a raw remote delta is not used as a source-loop delta when lengths differ;
- changing master length while `O != 0` cannot be undone by same-block restore;
- VST host sample time does not decrease or truncate at values above `2^32`.

Use deliberately asymmetric values (for example `M_l=1000`, `M_r=1200`,
`R=250`) because equal-length tests cannot expose the ruler bug.

### 2. Centralize integer phase mapping

Put the exact elapsed mapping, source phase at remote phase, and signed
source-correction resolution in `ninjam/NinjamLoopAlignment` or another small
timing header already used by production and tests. Use unsigned 64-bit
intermediates, sign-preserving conversion for negative deltas, explicit zero-
length guards, and no floating point. Reuse the helper from
`LoopTake::RestoreSyncPhaseMap`; do not maintain duplicate rounding formulas.

Test zero, wrap-minus-one, half interval, odd lengths/ties, negative discipline,
large-but-valid products, and the algebraic next-wrap invariant.

### 3. Make the audio-boundary transaction coherent

Refactor the top of `AudioHost::_OnAudio` into explicit callback-owned phases
matching the six-step ordering above. Preserve generation gating and the
restore-before-discipline rule. Retain the original local source-master ruler
for the active follow map. Remote stations remain untouched.

Do not apply a delta through both `ApplyTimingCommand` and the queued
`EndMultiPlay` correction path. Receipts should record both the remote timer
delta and the actual source-loop delta, or be renamed/documented so diagnostics
cannot confuse their units.

### 4. Publish continuous VST host time

Extend `HostTimeState` with integer-width and validity fields needed by both
adapters. Source its sample coordinate from the same shared timer at block
start for every station. Keep per-block construction as stack/value data.

Update VST2 and VST3 adapters according to the sample and musical-position
policies above. Factor adapter-context construction into pure helpers where
needed so tests do not require loading a third-party plugin.

### 5. Add physical audio/MIDI regression coverage

Model-only unwrapped `Position` tests are insufficient. Add or extend native
tests using real `LoopTake`/`Loop` state to cover:

- master audio-only, MIDI-only, and combined audio+MIDI takes;
- loop lengths `M_l`, `2*M_l`, a subdivision, and an odd non-divisor;
- distinct intentional initial phases, including phases straddling wrap;
- replacement at several remote phases with `M_l != M_r`;
- the exact next remote wrap and at least two later wraps;
- positive and negative join/discipline corrections followed by map rebases;
- non-zero UI offset before replacement, changed in the same boundary, reset to
  zero, and remote stations proving unchanged;
- MIDI automation phase equal to the MIDI event cursor after every operation;
- stale/equal/zero generations, invalidation, disconnect, and reconnect.

Assert per-loop expected phase from the common mapped elapsed plus each
captured origin, modulo that loop's own length. Do not compare raw cursor
differences between unequal wrapped lengths as the definition of alignment.

### 6. VST context tests

Test pure host-state/adaptor conversion for:

- consecutive blocks and replacement (`S`, `S + blockSize`);
- `S > 2^32` and exact integer-to-SDK conversion in the supported range;
- VST3 `kPlaying`, `kContTimeValid`, `kTempoValid`, and `kTimeSigValid` bits;
- VST2 playing/tempo/time-signature bits and absence of a NINJAM transport-
  changed flag;
- PPQ/project-music validity exactly matching the selected policy.

### 7. Verify incrementally

Build only affected projects first, using an absolute `SolutionDir` with one
trailing backslash for direct `.vcxproj` builds. Run focused timing/loop/VST
tests, then the full native test executable. Manually audit all changed hot
paths against `doc/realtime-audio.md` and inspect the final diff for unrelated
changes.

## Acceptance criteria

- `Timer::SampOffset() == R` immediately after an accepted replacement or
  correction, in the remote ruler.
- At the next remote interval wrap, the zero-offset local master audio body and
  MIDI cursor are exactly zero; each later wrap repeats exactly under the
  integer map.
- With UI offset `O`, the master phase at remote wrap is exactly `O mod M_l`,
  and changing/resetting `O` applies one delta only.
- Every local take receives one identical source-ruler correction before
  wrapping by its own length; all captured intentional offsets survive.
- A replacement during an existing follow session retains the original local
  source-master ruler.
- VST host sample position is non-decreasing across NINJAM replacement and
  advances by the processed block size. It remains correct beyond `2^32`.
- VST validity/transport flags exactly describe populated fields; no fabricated
  PPQ or project-music value is advertised.
- The command boundary remains allocation-free, lock-free, exception-free, and
  free of callback logging.

## Non-goals

- Time-stretching audio or MIDI. The integer map may repeat/skip source samples
  when rulers differ; it must nevertheless preserve common phase exactly.
- Changing transport-offset UI range, sign convention, or persistence.
- Treating a NINJAM follow adjustment as a general DAW locate/cycle feature.
- Publishing full PPQ/bar context unless the shared non-wrapping musical
  timeline in VST policy 2 is implemented and tested for both VST versions.
