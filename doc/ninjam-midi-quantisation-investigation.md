# NINJAM MIDI quantisation implementation record

**State recorded:** 2026-09-05
**Status:** remote MIDI event snapping is implemented; remote-grid overlay
rendering and the complete live scenario remain incomplete or unverified.

This document replaces the earlier investigation plan. It records the behavior
that exists, the automated evidence that supports it, and the residual work that
must not be reported as complete.

## Problem and settled contract

The original defect appeared when committed local MIDI takes sounded as though
they had different quantisation-grid origins after Jamma accepted NINJAM timing.
The settled contract is:

- NINJAM follow changes Timer's accepted remote master geometry, not any local
  audio or MIDI loop length.
- One remote/device-time descriptor supplies interval length, full authoritative
  remote BPI, and an origin derived from remote phase at its device-audio
  observation sample.
- A boundary is evaluated directly, never by repeatedly adding a rounded step:

  ```text
  divisions = remote BPI * MidiQuantisation::Divisor(fraction)
  boundary(k) = origin + round(k * remote interval / divisions)
  ```

- Recorded events are expressed through each take's transport start and local
  loop coordinate. Different starts and loop lengths can therefore have
  different loop-local event positions while referring to the same absolute
  remote boundary.
- Global, Station, and LoopTake user phase offsets compose once. Zero user
  offsets do not require a Ctrl-drag workaround.
- Remote master geometry, local loop geometry, common mapped source progress,
  per-entity cursors, remote-grid origin, and automation origin remain distinct.

Direct evaluation matters for an interval of 78,985 samples at BPI 4. Its beat
boundaries are `0, 19,746, 39,493, 59,239, 78,985`; repeatedly adding the rounded
19,746-sample step would instead drift to `39,492`, `59,238`, and `78,984`.

## Implemented path

### Authoritative remote descriptor

`NinjamTimingCoordinator` rejects missing BPI and retains the accepted server
BPI rather than deriving it from local grain. A material acceptance publishes
both:

- a complete `NinjamDesiredTransportState` for AudioHost; and
- `NinjamRemoteGridPublication`, containing `RemoteTransportGeometry` and the
  remote/device origin
  `RemotePhaseDeviceSample - RemoteMasterPhaseSamps`.

The relevant types and producer are
[`NinjamTimingCoordinator.h`](../JammaLib/src/ninjam/NinjamTimingCoordinator.h),
[`NinjamTimingCoordinator.cpp`](../JammaLib/src/ninjam/NinjamTimingCoordinator.cpp),
and
[`QuantisationTiming.h`](../JammaLib/src/engine/QuantisationTiming.h).

`Scene::_ApplyNinjamTimingUpdate` forwards the remote grid through
`Quantiser::SetRemoteMidiGrid` to each local LoopTake. It separately forwards
the complete desired transport value to AudioHost. Scene is wiring here, not a
second transport or grid authority. See
[`Scene.cpp`](../JammaLib/src/engine/Scene.cpp) and
[`Quantiser.cpp`](../JammaLib/src/engine/Quantiser.cpp).

### Per-take publication and event snapping

`LoopTake::SetRemoteMidiQuantisationGrid` publishes interval, BPI, and origin as
one seqlock-style value. `ResolvedMidiQuantisation` reads that value coherently
and composes inherited/user offsets. The legacy recording-start rounded-grain
term is not added when a remote grid is active, because the absolute remote-grid
conversion already accounts for transport start.

`MidiLoop::SetQuantisation` rebuilds a quantised playback snapshot away from the
audio callback. `MidiQuantisation::BuildQuantisedPlaybackEvents` finds the
nearest direct remote boundary for each note-on, converts it back into the
take's loop-local coordinate, wraps by that loop's own length, and applies the
composed user offset once. Matched note-offs receive the same event shift and
retain the existing loop-end clamp. Recorded source events are not modified.

The callback reads the already-published immutable snapshot through the
LoopTake-owned MIDI event cursor. Relevant implementation:

- [`LoopTake.cpp`](../JammaLib/src/engine/LoopTake.cpp)
- [`MidiQuantisation.h`](../JammaLib/src/midi/MidiQuantisation.h)
- [`MidiQuantisation.cpp`](../JammaLib/src/midi/MidiQuantisation.cpp)
- [`MidiLoop.cpp`](../JammaLib/src/midi/MidiLoop.cpp)

### Event cursor is not automation origin

`LoopTake::_midiVisualPlayIndex` is the audio-thread MIDI event cursor.
`MidiLoop::AutomationGlobalSampleOrigin()` is frozen at `EndRecord` and maps the
global audio sample counter into an automation fraction. It is not the event
cursor, the remote-grid origin, or a quantisation-grid substitute.

When sync restore or correction moves the event cursor,
`LoopTake::_midiAnchorCorrection` applies the inverse translation to the frozen
automation origin. Existing automation dispatch snapshots therefore remain
coherent without rebuilding in the callback. See
[`MidiLoop.h`](../JammaLib/src/midi/MidiLoop.h),
[`LoopTake.cpp`](../JammaLib/src/engine/LoopTake.cpp), and
[`Station.cpp`](../JammaLib/src/engine/Station.cpp).

## Overlay status

The remote event-snapping implementation does not prove the visual overlay.
`LoopTake::QuantisationVisual` no longer hides all non-dividing local lengths,
but `QuantisationLoopTakeVisual` still carries grain/division-style fields and
not the accepted remote interval/BPI/origin descriptor. The graphics models
therefore do not demonstrably render the same direct remote boundaries consumed
by MIDI event snapping. `Quantiser::ActiveGrid()` also reports only a division
count/default source rather than exposing the accepted remote descriptor.

Consequently:

- remote MIDI event snapping is implemented;
- remote-grid overlay rendering for unequal or non-dividing local loops remains
  incomplete or unproven; and
- no full live NINJAM MIDI/overlay acceptance scenario is claimed.

Relevant residual surfaces are
[`QuantisationTiming.h`](../JammaLib/src/engine/QuantisationTiming.h),
[`LoopTake.cpp`](../JammaLib/src/engine/LoopTake.cpp),
[`QuantisationModel.cpp`](../JammaLib/src/graphics/QuantisationModel.cpp), and
[`QuantisationDivisionModel.cpp`](../JammaLib/src/graphics/QuantisationDivisionModel.cpp).

## Evidence ledger

| Contract | Automated/static evidence | Status and limitation |
| --- | --- | --- |
| Direct fractional interval boundaries, including 78,985/BPI 4 | `QuantisationGrid.CalculatesRemoteFractionalCellsFromOneInterval` in [`Quantisation_Tests.cpp`](../test/JammaLib_Tests/src/engine/Quantisation_Tests.cpp) | Passed in the recorded B015 focused/full verification. |
| Remote note-on snaps to a direct boundary with a non-zero origin | `MidiQuantisation.RemoteGridUsesDirectIntervalBoundaries` in [`MidiQuantisation_Tests.cpp`](../test/JammaLib_Tests/src/midi/MidiQuantisation_Tests.cpp) | Implemented and automated; this is not a live-device test. |
| Accepted server BPI is retained; missing BPI creates no authority | `NinjamTimingCoordinator.AcceptedRemoteGridRetainsAuthoritativeBpi` and `MissingBpiCannotCreateOrMutateRemoteAuthority` in [`NinjamTimingCoordinator_Tests.cpp`](../test/JammaLib_Tests/src/ninjam/NinjamTimingCoordinator_Tests.cpp) | Implemented, focused and full-suite verified. |
| Complete desired state, latest-newer application, reconnect, `NoSync`, `M`/`2M`/`3M`, and restore-before-rebase | `NinjamTimingProductionBoundary.*` in [`NinjamTimingIntegration_Tests.cpp`](../test/JammaLib_Tests/src/ninjam/NinjamTimingIntegration_Tests.cpp) | Automated production-boundary evidence; no live server involved. |
| Event cursor and automation origin remain coherent after direct timing movement | `TransportPhaseOffset.DirectTimingCommandKeepsMidiAutomationWithNoteCursor` in [`LoopTakeTiming_Tests.cpp`](../test/JammaLib_Tests/src/engine/LoopTakeTiming_Tests.cpp) | Passed as a B015 prerequisite and in broader regression filters. |
| B015 naming-only reconciliation did not change behavior | Independent B015 verification at `b055a20` ran the expanded timing/MIDI/export/metronome filter 227/227 and the full suite 843 passed, one expected hardware MIDI skip, zero failed | Existing automated evidence; B016 runs no executable tests because it changes statements only. |
| Remote overlay consumes the same descriptor/boundaries as event snapping | Static audit finds the descriptor absent from the overlay payload/renderers | Not complete or proven. No OpenGL/live visual claim. |

The original remote-grid implementation landed in the branch before this record
(historically identified by `e72f3b0`), and its current vocabulary and ownership
were reconciled through independently approved B013-B015. The table cites the
current source and tests rather than directing a future implementation effort.

## Residual and manual-evidence ledger

- Remote MIDI event snapping is implemented. Overlay use of the remote
  descriptor and the complete live NINJAM MIDI scenario remain unverified.
- Integer remote-to-local mapping may repeat or skip source audio/MIDI samples;
  this is not sample-perfect time stretching.
- Send-side export latency compensation remains disabled by default pending a
  physical DAC-to-ADC loopback against a real or test NINJAM server.
- Persisted `transportoffsetloopfrac` defaults to zero when missing. Finite
  signed values are clamped to `[-1,1]`. There is no schema/version guard; an
  older binary may discard the field on resave, and no manual older-binary
  resave test was run.
- The `.jam` format still serializes the NINJAM password and work directory.
  F-049/F-050 remediation was rejected, so disclosure and portable
  work-directory authority remain accepted residual risks.
- Generic JSON size/depth limits and upstream NJClient user/channel/work limits
  were not hardened. This record is not a broad security certification.
- No live prompt/grid/local-inference trace, interactive `.jam` load,
  older-binary resave, bounded verbose diagnostic trace, full live MIDI/overlay
  scenario, or physical loopback evidence is claimed.

Any future overlay work must consume the accepted remote descriptor or
equivalent direct-boundary data, preserve immutable off-callback publication,
leave recorded events and local loop lengths unchanged, and maintain the
callback-safe rules in [Real-time audio guidance](realtime-audio.md).
