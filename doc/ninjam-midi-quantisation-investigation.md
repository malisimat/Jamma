# NINJAM MIDI Quantisation TDD Plan

## Goal and acceptance criteria

Fix the case where committed local MIDI takes sound as though they have different
quantisation-grid origins after the client accepts NINJAM timing.

The required behaviour is:

- NINJAM follow does not change any local audio or MIDI loop length.
- The accepted remote interval, authoritative remote BPI, and observed remote
  phase define one grid in remote/device time.
- MIDI snap boundaries and overlay bars are derived from that one grid. A
  boundary is evaluated directly, not by accumulating a rounded sample step:

  ```text
  beat(k) = origin + round(k * remoteInterval / remoteBpi)
  snap(k) = origin + round(k * remoteInterval /
             (remoteBpi * MidiQuantisation::Divisor(fraction)))
  ```

- The common remote/device-time boundaries are mapped into each take's local
  loop coordinate using the same transport/sync-map relationship as playback.
  Different loop lengths and recording starts may therefore have different
  loop-local boundary positions, but notes from all zero-user-offset takes must
  reach the same absolute NINJAM boundaries.
- Stored user offsets remain independently composable at global, station, and
  take scope. With all three at zero, no Ctrl-drag is needed to align takes.
- Quantisation remains enabled and bars remain visible when the new remote grid
  does not divide an unchanged local loop length.
- In remote/device time the grid follows the accepted NINJAM tempo. For a
  slightly longer remote interval its temporal gaps are correspondingly wider.

Do not use equality of loop-local cursors or of the effective phase placed in a
resolved settings copy as the acceptance criterion. The observable invariant is
equality with the shared absolute remote-grid boundaries.

## Verified implementation state

### Raw user phase and resolved phase are different concepts

`Station::HandleTriggerAction` captures the recording transport coordinate from
`Timer::AbsoluteSamplePos() + TransportOffsetSamps()` and passes it to the new
take (`JammaLib/src/engine/Station.cpp`, around lines 1020-1042).
`LoopTake::Record` stores it as `_midiTransportStartSamps`
(`JammaLib/src/engine/LoopTake.cpp`, around lines 1341-1365).

`LoopTake::MidiQuantisation().PhaseOffsetSamps` is the stored take/user offset
edited by the LoopTake Ctrl-drag handle. `LoopTake::ResolvedMidiQuantisation()`
copies those stored settings, then overwrites `PhaseOffsetSamps` in the returned
copy with:

```text
natural recording-start translation
+ stored take/user offset
+ inherited global and station offsets
```

See `JammaLib/src/engine/LoopTake.cpp`, around lines 2461-2489 and 2536-2548.
The natural term is currently `-(transportStart % step)`.

Consequently, two takes can both have a stored `PhaseOffsetSamps` of zero while
the settings copies returned by `ResolvedMidiQuantisation()` contain different
effective `PhaseOffsetSamps` values. The extra term comes from
`_midiTransportStartSamps`, not `MidiLoop::_loopPhaseAnchor`. It is necessary
when expressing one absolute grid in different loop-local coordinate systems;
it is not by itself evidence of the bug. The existing
`LoopTakeMidiQuantisation.DifferentTakeStartsQuantiseToSharedTransportGrid` test
in `test/JammaLib_Tests/src/midi/MidiLoop_Tests.cpp` demonstrates the valid,
exactly-dividing case.

`MidiLoop::_loopPhaseAnchor` is separate. `MidiLoop::EndRecord` freezes it from
`startGlobalSample` (`JammaLib/src/midi/MidiLoop.cpp`, around lines 253-258),
and the router/automation paths use it to convert global samples into automation
loop fractions. It is not read by `ResolvedMidiQuantisation()`,
`MidiQuantisation::QuantiseSampleOffset`, or MIDI event playback. Event playback
is driven by `LoopTake::_midiVisualPlayIndex`, passed to `MidiLoop::ReadBlock`,
and quantised event positions come from the resolved settings copy. Do not use
`_loopPhaseAnchor` as the shared MIDI quantisation-grid origin.

### A shared cursor map exists, but a shared MIDI-grid descriptor does not

`AudioHost` owns one `ninjam::SyncPhaseMap` and fans it out through `Station` to
each `LoopTake` (`JammaLib/src/audio/AudioHost.cpp`, around lines 307-364;
`JammaLib/src/engine/Station.cpp`, around lines 764-794). Each take captures its
own anchor and restores its audio and MIDI cursor from the common mapped source
coordinate (`JammaLib/src/engine/LoopTake.cpp`, around lines 602-678). Preserve
this distinction: the shared transport/cursor map is already present.

On accepted remote timing, `Scene::_ApplyNinjamTimingUpdate` publishes interval,
BPI, phase, and generation to the audio timing command, but the MIDI path only
calls `Quantiser::SetMidiGrain` (`JammaLib/src/engine/Scene.cpp`, around lines
403-444). `SetMidiGrain` updates `GrainSamps` independently on every take and
publishes neither the accepted grid origin nor its phase
(`JammaLib/src/engine/Quantiser.cpp`, around lines 203-230).

There are partial abstractions to reuse or replace deliberately:

- `QuantisationGrid` already evaluates `round(k * interval / divisions)`
  directly (`JammaLib/src/engine/Quantiser.h`, around lines 72-92).
- `RemoteTransportGeometry` already declares interval, BPI, phase, BPM, and
  generation, but is currently unused (same file, around lines 94-101).
- `Quantiser::ActiveGrid()` currently reports only a division count with source
  `Default`; it carries no interval origin (`JammaLib/src/engine/Quantiser.cpp`,
  around lines 591-604).

### Current event snapping can produce different effective grids

`MidiQuantisation::StepSamps` reduces the grid to the constant integer
`GrainSamps / divisor`. `MidiQuantisation::QuantiseSampleOffset` then snaps and
wraps that sequence independently modulo each loop length
(`JammaLib/src/midi/MidiQuantisation.h`, around lines 199-205, and
`JammaLib/src/midi/MidiQuantisation.cpp`, around lines 78-100).
`MidiLoop::PublishQuantisedEvents` rebuilds the immutable playback snapshot from
that constant step and the take's resolved phase
(`JammaLib/src/midi/MidiLoop.cpp`, around lines 479-512).

For the reproduced geometry:

| value | samples |
| --- | ---: |
| local grain | 19,632 |
| short loop | 78,528 = 4 local grains |
| long loop | 157,056 = 8 local grains |
| accepted remote interval | 78,985 |
| accepted remote grain currently forwarded | 19,746 |

the unchanged loop lengths are not multiples of the forwarded grain:

| loop | `loopLength % 19,746` |
| --- | ---: |
| 78,528 | 19,290 |
| 157,056 | 18,834 |

For remote BPI 4, the direct interval boundaries are `0, 19746, 39493,
59239, 78985`; repeated addition of the rounded grain yields `0, 19746, 39492,
59238, 78984`. Independent modulo wrapping then introduces a different
recurrence discontinuity for each loop length. A non-zero accepted remote phase
cannot be represented at all because that phase is not supplied to the MIDI
quantisation path. Thus the implementation can rebuild two zero-user-offset
takes onto different effective absolute grids after sync.

A concrete counterexample follows the production arithmetic. Consider the same
musical occurrence at the second local-master boundary:

- In the 78,528-sample take, an event at loop-local zero recurs when common
  local-source progress reaches 78,528.
- In the 157,056-sample take, the corresponding recorded event at loop-local
  78,528 is rebuilt by the current constant-step quantiser at
  `4 * 19,746 = 78,984`.
- `MapRemoteElapsedToLocal(elapsed, 78,528, 78,985)` reaches those two source
  coordinates at remote/device elapsed samples 78,985 and 79,444 respectively.
  The emitted timestamps can therefore be 459 device samples apart even though
  both stored take/user offsets are zero.

Distinct recording starts change the loop-local offsets and the natural terms
in the resolved settings copies, but the intended absolute occurrence still
snaps to 78,984 in the long take while the short loop physically recurs at
78,528. Recording-start compensation therefore does not cancel this
non-dividing recurrence mismatch. This is a constructive code-level proof that
the failure can occur; the TDD regression below must reproduce it through real
`LoopTake`/`MidiLoop` emission so the fix is gated by executable evidence.

The test oracle must express boundaries in remote/device time and pass them
through the production sync mapping before comparing loop-local events. Do not
blindly use remote-device sample distances as local-buffer distances: the
active sync map converts remote elapsed time to local-source progress.

### The missing overlay bars are a separate confirmed defect

`LoopTake::QuantisationVisual()` sets `LoopGrains` to zero unless
`loopLength % grain == 0` (`JammaLib/src/engine/LoopTake.cpp`, around lines
1211-1235). `QuantisationModel` then suppresses the zero-frame visual, and both
graphics models assume uniformly spaced divisions (`JammaLib/src/graphics/
QuantisationModel.cpp`, around lines 236-309, and `JammaLib/src/graphics/
QuantisationDivisionModel.cpp`, around lines 161-204).

Removing only the divisibility guard is insufficient. The visual data and both
renderers need boundary positions, or equivalent shared-grid metadata, so they
can show direct fractional interval boundaries mapped into each displayed loop
window.

### Corrections to the previous proposal

- Do not require the effective `PhaseOffsetSamps` in copies returned by
  `ResolvedMidiQuantisation()` to be zero or equal across takes. Require the
  stored `MidiQuantisation().PhaseOffsetSamps` user offsets to remain zero and
  emitted notes to share absolute grid boundaries.
- Do not add the proposed first-replacement `oldAbsolute + stationDelta` fix.
  A wrapped source-coordinate origin is paired with anchors captured in the
  same coordinate system, so whole-interval translation cancels on restore,
  including for a loop twice the master length. Existing unequal-length sync-map
  tests should remain green.
- Do not describe the cursor sync map as missing. The missing state is a shared,
  phased MIDI quantisation-grid description and its mapping into loop-local
  coordinates.
- Do not assume `NinjamTimingCoordinator::_MakeProposal` preserves the server's
  BPI. It currently calls `UserConfig::DeduceLoopTiming` and uses the derived
  BPI/grain (`JammaLib/src/ninjam/NinjamTimingCoordinator.cpp`, around lines
  285-315), even though `NinjamTiming::Bpi` is authoritative. Cover this with a
  test and keep legacy local seed deduction separate from the remote grid.

## TDD execution plan

### 1. Establish deterministic test seams and a baseline

Run the existing focused suites before editing:

```powershell
test\JammaLib_Tests\bin\x64\Debug\JammaLib_Tests.exe `
  --gtest_filter="LoopTakeMidiQuantisation.*:MidiLoopQuantisation.*:QuantisationGrid.*:TransportPhaseOffset.*:NinjamTimingCoordinator.*:NinjamTimingIntegration.*"
```

If the production remote-boundary-to-loop-coordinate calculation is trapped in
`AudioHost::_OnAudio`, first extract a small allocation-free, `noexcept` value
calculation in `JammaLib` and test that function. Do not build a second model of
the callback. `NinjamTimingIntegration_Tests.cpp` currently uses `ModelTake` and
mirrors an older/simplified fan-out; it cannot prove MIDI event alignment.

Use fixed integer sample coordinates, generations, phases, and block sizes. Do
not use a live NINJAM connection, wall-clock waits, log parsing, screenshots, or
unspecified tolerances.

### 2. Add tests that demonstrate the failure before changing production code

Add or extend focused tests in this order:

1. In `test/JammaLib_Tests/src/midi/MidiLoop_Tests.cpp`, retain the existing
   divisible-grid test and add a diagnostic showing that two takes with distinct
   transport starts and zero stored/inherited offsets acquire different
   effective phase values in their resolved settings copies after the remote
   grain update. Assert explicitly that neither value comes from
   `MidiLoop::LoopPhaseAnchor()`. This diagnostic may pass before the fix; it
   documents why equality of the effective values is not the oracle.
2. In `test/JammaLib_Tests/src/engine/Quantisation_Tests.cpp`, pin direct beat and
   fractional snap boundaries, including `interval=78,985`, `BPI=4`, a non-zero
   origin, exact endpoints, and rounding ties. Reuse `QuantisationGrid::SampleAt`
   rather than introducing another rounding formula.
3. In `test/JammaLib_Tests/src/ninjam/NinjamTimingCoordinator_Tests.cpp`, create
   a case where `UserConfig::DeduceLoopTiming` would choose a different BPI and
   assert that the accepted remote grid retains `NinjamTiming::Bpi`, interval,
   phase/observation anchor, and generation.
4. In `test/JammaLib_Tests/src/midi/MidiLoop_Tests.cpp`, build real `LoopTake` /
   `MidiLoop` playback for lengths 78,528 and 157,056, distinct recording starts,
   zero global/station/take user offsets, remote interval 78,985, BPI 4, and a
   non-zero remote phase. Put note-ons at the same absolute musical beats. After
   applying the same production grid/sync mapping, assert emitted device-time
   note-ons from both takes land on the same direct remote boundaries across a
   wrap and a later discipline/rebase. Avoid note-offs near the loop end because
   the existing implementation deliberately clamps shifted note-offs.
5. Add a negative/control case: a non-zero take user offset moves only that
   take by the requested amount; returning it to zero restores the common grid.
6. Add pure visual-data tests for both non-dividing loop lengths. Assert that
   both return visible boundaries, their absolute boundary identities match,
   and their loop-local positions are the production mapping of the same remote
   grid. Test boundary data or model instance counts, not OpenGL pixels.
7. Preserve the existing `LoopTakeTiming_Tests.cpp` coverage for shared map
   restore/rebase, unequal lengths, independent anchors, and MIDI/audio cursor
   coupling. Add a production-seam regression only if step 4 exposes a genuine
   cursor-map defect; do not encode the disproved absolute-origin assumption.

Run the new tests alone and record that each behavioural regression fails for
its intended assertion. Then run the focused baseline to ensure test setup has
not broken unrelated behaviour. Do not start implementation until the audible
alignment, authoritative-grid, and visual regressions are red.

### 3. Freeze the contract, then dispatch Terra implementation agents

Once the red tests define the shared descriptor and mapping API, dispatch Terra
agents with non-overlapping ownership. Give every agent this plan, the failing
test names/output, `doc/loop-alignment-and-ninjam-sync.md`, and the real-time
rules in `doc/realtime-audio.md`.

- **Terra: remote-grid publication.** Own `NinjamTimingCoordinator`, timing
  command/value types, `Scene`, and `Quantiser`. Preserve the authoritative
  remote interval/BPI/phase/observation anchor/generation as one immutable value.
  Reuse or evolve `RemoteTransportGeometry` and `QuantisationGrid`; keep local
  loop/tap geometry separate. Publish through the existing latest-wins/snapshot
  patterns and do not allocate or lock in the audio callback.
- **Terra: MIDI boundary mapping and playback snapshots.** Own `LoopTake`,
  `MidiQuantisation`, and `MidiLoop`. Replace constant-step, per-loop modulo
  snapping for an active remote grid with direct boundary evaluation followed
  by the common sync-map conversion into the take's coordinate. Compose the
  natural recording-start translation and global/station/take user offsets
  exactly once. Continue publishing immutable quantised event buffers off the
  audio hot path; do not mutate recorded source events or local loop lengths.
- **Terra: overlay data and rendering.** Own `QuantisationLoopTakeVisual`,
  `QuantisationModel`, and `QuantisationDivisionModel`. Remove the exact-
  divisibility eligibility rule and render the same mapped boundary data used by
  MIDI snapping. Avoid assuming equal angular/sample spacing when fractional
  cells differ by one sample.

Integrate the publication contract first. The MIDI and overlay agents may then
work in parallel against that frozen interface. Require each agent to run its
owned failing tests and report focused diffs; the primary agent reviews all
cross-thread ownership and coordinate-domain conversions before combining work.

### 4. Reach green and verify regressions

Run the new failing tests first, then the complete related suites:

```powershell
.github\skills\builder\builder.ps1 -Target JammaLib_Tests -Configuration Debug -Platform x64

test\JammaLib_Tests\bin\x64\Debug\JammaLib_Tests.exe `
  --gtest_filter="LoopTakeMidiQuantisation.*:MidiLoopQuantisation.*:QuantisationGrid.*:QuantisationModel.*:TransportPhaseOffset.*:NinjamTimingCoordinator.*:NinjamAudioTimingCommand.*:NinjamTimingIntegration.*"
```

Then run the full native test executable. Verify exact integer boundaries; only
use a tolerance where a named production conversion has an explicit rounding
bound, and state that bound in the assertion.

The green evidence must cover:

- two unequal-length, differently-started takes with zero stored user offsets;
- accepted remote tempo slightly different from the original local tempo;
- non-zero remote phase/origin;
- initial replacement, at least one wrap, and later phase discipline/rebase;
- authoritative remote BPI and direct interval endpoint arithmetic;
- unchanged local loop lengths and unchanged recorded MIDI source events;
- visible, mutually aligned bars for both non-dividing loops; and
- isolation and reversibility of a manual take offset.

### 5. Cleanup and refactor only after green

After all regressions pass:

- remove obsolete grain-only remote-grid plumbing and duplicate boundary math;
- centralise coordinate-domain naming (`remote/device`, `scene`, `local source`,
  and `loop local`) and document the non-obvious conversion invariant;
- remove or update stale tests that mirror `AudioHost` rather than exercise the
  extracted production calculation;
- keep the callback allocation-free, exception-free, and lock-free;
- retain immutable snapshot publication and existing generation/invalidation
  semantics; and
- rerun the focused and full native suites after each cleanup change.

Finish with a code review specifically checking that no remote-grid change can
alter loop geometry, that user offsets are applied once, and that MIDI playback
and overlay rendering consume the same shared boundary source.
