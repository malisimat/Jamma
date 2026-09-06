# NINJAM integration guide

Jamma uses vendored NINJAM client files for collaborative jamming. The app shell
wires the feature, while transport, loop, MIDI, and audio behavior remains in
JammaLib. The local ownership hierarchy remains `Station -> LoopTake -> Loop`.

## Interval and transport model

A NINJAM interval contains BPI beats at the session BPM. Its duration is

```text
interval seconds = 60 * BPI / BPM
interval samples = round(device sample rate * 60 * BPI / BPM)
```

The server supplies the authoritative remote BPM, full BPI, interval position,
and sample rate. Full remote BPI is mandatory once remote authority is
available; Jamma does not synthesize a missing remote BPI. Disconnected local
seed timing may still infer local BPI from the local master length and local
grain through the Quantiser's pure local calculations.

Remote master geometry and local loop geometry are different things. Accepting
remote timing replaces the Timer's master geometry, but does not resize local
audio or MIDI loops. Every local entity keeps its own length, anchor, and wrapped
cursor. See [Loop alignment and NINJAM sync](loop-alignment-and-ninjam-sync.md)
for the coordinate and mapping rules.

## Timing ownership and desired-state flow

The NINJAM integration path owns physical availability, remote observations,
session epoch, tempo-request and prompt state, follow-policy selection, and the
complete desired remote transport state:

1. `NinjamConnection` brackets `AudioProc`, accepts a job-side NJClient timing
   tuple only when that audio sequence is stable, and pairs it with the completed
   device-audio sample. AudioHost then adds the callback-time local Timer tuple
   and publishes the coherent observation; the callback performs no NJClient
   timing getter call.
2. On the job side, `NinjamNetworkService` serializes access to
   `NinjamTimingCoordinator`. The Coordinator validates the observation,
   retains full authoritative remote BPI, resolves request/prompt state, and
   produces a complete `NinjamDesiredTransportState`.
3. `Scene::_ApplyNinjamTimingUpdate` forwards that value and any remote MIDI
   grid publication. Scene does not construct transport authority.
4. The integration owner is the sole writer of
   `NinjamDesiredTransportStateMailbox`; the AudioHost callback is its sole
   reader. Each active value carries session epoch, monotonically newer version,
   command generation, follow policy, complete device-rate remote geometry, and
   remote phase at a device-audio observation sample. A `NoSync` value carries
   epoch/version but no remote authority.
5. At the top of each audio block, `AudioHost` reads the latest complete value.
   It ignores stale/equal values and applies only the latest newer value. An
   epoch change or `NoSync` clears authority, map, anchors, and generation gates;
   a geometry change replaces Timer timing; unchanged geometry derives a phase
   correction from the timestamped remote phase. Timer and all local entities
   are updated coherently before normal block advancement.

The value and mailbox contract is declared in
[`NinjamAudioTimingCommand.h`](../JammaLib/src/ninjam/NinjamAudioTimingCommand.h),
produced by
[`NinjamTimingCoordinator.cpp`](../JammaLib/src/ninjam/NinjamTimingCoordinator.cpp),
forwarded by [`Scene.cpp`](../JammaLib/src/engine/Scene.cpp), and consumed by
[`AudioHost.cpp`](../JammaLib/src/audio/AudioHost.cpp). The production-boundary
tests in
[`NinjamTimingIntegration_Tests.cpp`](../test/JammaLib_Tests/src/ninjam/NinjamTimingIntegration_Tests.cpp)
cover latest complete-state substitution, epoch reset, `NoSync`, replacement,
discipline, and per-entity phase preservation.

### Tempo join and follow contract

When joining with valid local timing, Jamma can first request the local BPM/BPI
from the server. The request is asynchronous: local playback, recording, and
overdubbing continue while it is queued or awaiting a server observation.
Sending the chat/control messages is not acknowledgement.

A later server observation acknowledges the request only when its BPI matches
and its BPM is within the inclusive +/-1.0 BPM tolerance. Jamma then follows the
actual observed server interval and phase, including server rounding. If no
near-local observation arrives before the deadline, the latest server timing is
presented as `Current server tempo` with `Follow server` and `Stay local`.

- `Follow server` publishes one complete remote replacement.
- `Stay local` publishes an explicit complete `NoSync` desired state. AudioHost
  clears remote authority and its map/anchor/generation gates without moving a
  local audio cursor, MIDI event cursor, or automation phase. Local transport
  then free-runs.

Physical loss, disconnect, observation timeout, and reconnect use the same
explicit lifecycle. A recovered physical session receives a fresh session
epoch; stale desired values, prompts, maps, and generation gates cannot cross
into the new authority. The lifecycle is exercised by
[`NinjamSessionTiming_Tests.cpp`](../test/JammaLib_Tests/src/ninjam/NinjamSessionTiming_Tests.cpp)
and the Coordinator tests in
[`NinjamTimingCoordinator_Tests.cpp`](../test/JammaLib_Tests/src/ninjam/NinjamTimingCoordinator_Tests.cpp).

## MIDI event timing and automation

MIDI event playback and automation use related corrections but remain distinct:

- The audio thread owns the MIDI event cursor,
  `LoopTake::_midiVisualPlayIndex`, and advances or restores it with the audio
  loops. `MidiLoop::ReadBlock` consumes the immutable quantised event snapshot
  at that cursor.
- `MidiLoop::AutomationGlobalSampleOrigin()` is frozen when recording ends and
  maps the global audio sample counter into automation loop phase. It is not the
  MIDI event cursor or the remote grid origin.
- `LoopTake::_midiAnchorCorrection` composes transport cursor translations with
  the frozen automation origin, so moving the event cursor does not require an
  automation dispatch rebuild.

Accepted remote timing also publishes an immutable remote MIDI descriptor:
remote interval, authoritative BPI, and an origin derived from the observed
device sample and remote phase. `MidiQuantisation` evaluates each boundary
directly as `origin + round(k * interval / divisions)` and builds quantised
playback snapshots away from the audio callback. This remote event-snapping path
is implemented and unit-tested. It does not change recorded source events or
local loop lengths.

The current visual overlay remains a residual: its payload/rendering still does
not consume the same remote descriptor, so remote-grid overlay alignment and the
full live MIDI scenario are not verified. The implementation record is
[NINJAM MIDI quantisation investigation](ninjam-midi-quantisation-investigation.md).

## Bounded timing diagnostics

`NinjamTimingCoordinator` is the sole diagnostics owner. When verbose timing
capture is disabled, capture returns before counter/event work. When enabled,
the Coordinator stores reason counts and a fixed 32-event ledger correlated by
session epoch, desired/applied version, and generation. Repeated anomalies emit
their first occurrence and cumulative power-of-two suppression summaries;
overflow summaries follow the same bounded schedule. Desired lag/caught-up
transitions are deduplicated.

`NinjamNetworkService` only provides the two mutex-serialized forwarding
operations needed to configure capture and submit/read AudioHost's existing
applied receipt. It owns no diagnostic state or formatting. `Scene` drains and
presents the records on the job thread only when Event logging is `verbose`;
there is no hierarchy traversal, string formatting, or output in the audio
callback. The fixed-capacity and suppression contracts are covered by
`NinjamTimingDiagnostics.*` in
[`NinjamTimingCoordinator_Tests.cpp`](../test/JammaLib_Tests/src/ninjam/NinjamTimingCoordinator_Tests.cpp).

## Export-lane latency compensation

Receive-side transport following is separate from send-side export-lane
alignment. `NinjamConnection::ProcessExportBlock` has an `ExportLaneTiming`
delay-line compensation path with generation-reset and anomaly detection, but
`NinjamConnection::ExportLatencyCompensationEnabled` remains `false`. It is
disabled by default until a physical DAC-to-ADC loopback against a real or test
NINJAM server validates the design. Integer transport mapping may repeat or skip
source samples; it is phase preservation, not sample-perfect time stretching.

## Persistence and accepted residual risks

`transportoffsetloopfrac` is local state, not remote authority. A missing field
defaults to zero. Finite signed values are retained and clamped to `[-1,1]` so
one common correction can be applied independently modulo `M`, `2M`, `3M`, and
non-divisor entity lengths. There is no schema/version guard. An older binary
may ignore the field and discard it on resave, and no manual older-binary resave
test has been run.

The `.jam` format still serializes the NINJAM password and work directory.
F-049/F-050 remediation was rejected; disclosure of the password and portable
work-directory authority are accepted residual risks. Generic JSON size/depth
limits and unavailable upstream NJClient user/channel/work limits were not
hardened. This timing cleanup is not a broad security certification.

Automated evidence covers the desired-state boundary, lifecycle, remote BPI,
direct MIDI boundaries, signed local offsets, fixed diagnostics, and regression
suites. No live prompt/grid/local-inference trace, interactive `.jam` load,
older-binary resave, bounded verbose trace, full live MIDI/overlay scenario, or
physical loopback evidence is claimed.

## Vendored client files

Files are under `lib/`:

- Header include root: `lib\njclient\njclient.h`
- x64 Debug library: `lib\njclient\x64\Debug\MD\njclient.lib`
- x64 Release library: `lib\njclient\x64\Release\MD\njclient.lib`

A local `Directory.Build.local.props` is no longer needed for NINJAM paths.

When intentionally refreshing the vendored artifacts, build the other NINJAM
repository for x64 Debug and Release with the MD runtime, then copy its header
and matching libraries into these directories. Normal Jamma builds do not need
this refresh step.

If the compiler cannot find `njclient.h` or the linker cannot find
`njclient.lib`, verify those vendored paths. Linking also requires `ogg.lib`,
`vorbis.lib`, `vorbisenc.lib`, and `vorbisfile.lib` from vcpkg plus the Windows
SDK `ws2_32.lib`.
