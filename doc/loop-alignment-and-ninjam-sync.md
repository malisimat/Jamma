# Loop alignment and NINJAM sync

This is the design reference for preserving local loop phase while following a
NINJAM transport. Timing values are sample positions unless stated otherwise.
The audio callback owns live Timer and cursor mutation; the NINJAM integration
and job side owns remote authority and policy. Engine behavior remains in
JammaLib under `Station -> LoopTake -> Loop`.

## Coordinate domains

Keep these domains explicit; equal numeric values do not make them
interchangeable:

- **Device audio sample** is the monotonic sample counter at an audio boundary.
  A remote phase is meaningful only together with its
  `RemotePhaseDeviceSample` observation anchor.
- **Local master absolute sample** is the Timer's loop-count plus master-phase
  coordinate at that same observation. It is not a later job-thread Timer read.
- **Scene coordinate** is `Timer::SceneSamplePos`, a monotonic callback-owned
  ruler that survives remote Timer geometry replacement.
- **Local source coordinate** is the common mapped progress on the ruler used by
  locally recorded loop buffers.
- **Per-entity source coordinate** is each audio loop body cursor or MIDI event
  cursor wrapped by that entity's own length.
- **Automation global sample origin** is the frozen
  `MidiLoop::AutomationGlobalSampleOrigin()`. It is distinct from the MIDI event
  cursor and from remote-grid origin.

Remote master geometry and local loop geometry are likewise distinct. An
accepted remote replacement changes Timer's master interval/BPM/BPI, while each
local audio and MIDI entity retains its own buffer length, anchor, and phase.

## The common map is not a common cursor

Correcting only `utils::Timer` is insufficient because local loops may have
lengths `M`, `2M`, `3M`, or non-divisors and may begin at intentionally different
phases. Assigning one wrapped master cursor to all entities would destroy those
relationships.

`ninjam::SyncPhaseMap` stores only the remote-to-local mapping geometry:

- local source-master length, `M_l`;
- remote master length, `M_r`;
- scene origin, `S_0`; and
- monotonic local source coordinate at that origin, `E_0`.

It does **not** store a loop phase. Audio-loop and MIDI-cursor anchors remain on
their owning `Loop`/`LoopTake`. At mapped source coordinate `E_0`, entity `i`
with phase `q_i` and length `L_i` captures

```text
a_i = (E_0 - q_i) mod L_i
```

and later restores

```text
q_i(E) = (E - a_i) mod L_i
```

One common signed correction therefore changes every entity by the same source
amount while each entity applies its own modulo. Relative offsets and long-loop
turn information survive.

Anchor capture is requested when AudioHost establishes a map that was absent.
Existing anchors are retained on later discipline/rebase operations; rebase
updates only the map's common origin and never recaptures them. A playable entity
that has no anchor may capture its current phase lazily at restore. Epoch or
`NoSync` invalidation clears anchors without moving a cursor, allowing the next
independent follow session to capture a new origin.

The map contract is implemented in
[`NinjamLoopAlignment.h`](../JammaLib/src/ninjam/NinjamLoopAlignment.h), with
entity capture/restore in
[`LoopTake.cpp`](../JammaLib/src/engine/LoopTake.cpp) and fan-out through
[`Station.cpp`](../JammaLib/src/engine/Station.cpp).

## Mapping remote elapsed time

For scene elapsed time `S - S_0`, mapped local-source progress is

```text
E = E_0 + round((S - S_0) * M_l / M_r)
```

The implementation evaluates whole remote intervals plus a rounded remainder
using integer arithmetic. It is not a floating-point phase accumulator. Exact
remote interval boundaries map to exact local-master boundaries, and every
entity receives the same monotonic mapped source coordinate before wrapping by
its own length.

This mapping preserves phase relationships, not sample-perfect time stretching.
Until rate adjustment exists, integer conversion may repeat or skip source
audio/MIDI samples. The effect should be small for `ContinuousSync` and can be
conspicuous for `BlockSync`.

## Two separate publication contracts

Two timing controls reach AudioHost through deliberately separate mailboxes:

1. **Remote desired transport.** `NinjamDesiredTransportStateMailbox` is written
   by the NINJAM integration owner and read by the audio callback. Each active
   publication is complete and latest-substitutable: session epoch, desired
   version, command generation, policy, full device-rate remote geometry, and
   timestamped remote phase travel together. A `NoSync` publication carries no
   remote authority. Multiple job publications before one callback boundary may
   coalesce; AudioHost applies only the latest newer complete state.
2. **Local transport offset.** AudioHost's private
   `LocalTransportOffsetLoopFracMailbox` carries the latest absolute local
   offset target, including an explicit zero. It remains active while
   disconnected or under `NoSync` and is not ordered by a NINJAM epoch or remote
   command generation.

Do not merge these contracts or turn either into a delta queue. Their ownership,
authority, and invalidation semantics differ. The declarations are in
[`NinjamAudioTimingCommand.h`](../JammaLib/src/ninjam/NinjamAudioTimingCommand.h)
and [`AudioHost.h`](../JammaLib/src/audio/AudioHost.h).

## Audio-boundary application order

At the top of an audio block, `AudioHost::ApplyDesiredTimingAtAudioBoundary`
performs the following coherent operation:

1. Read the latest complete desired value and reject stale/equal epoch, version,
   or command-generation work.
2. On epoch change or `NoSync`, reset remote authority, common map, entity
   anchors, Timer and per-entity generation gates. This invalidation moves no
   audio cursor, MIDI event cursor, or automation phase.
3. For active remote authority, project the timestamped remote phase to the
   current device boundary. If remote geometry changed, replace Timer geometry;
   otherwise derive one bounded remote-master phase correction.
4. If a map already exists, restore the mapped local source before applying the
   correction. This removes device-rate advancement residue before rebase.
5. Fan one policy-neutral local-source correction through each published local
   Station/LoopTake snapshot. Every entity shifts independently modulo its own
   length.
6. Rebase the common map at the same scene coordinate. Capture per-entity
   anchors only when the map was previously absent.
7. Apply the separate local-offset target and then, if requested, capture map
   anchors after that offset has reached every local take.

Normal station advancement follows this boundary work. `AudioHost.cpp` is the
sole owner of Timer replacement and common-map calculation; Station and
LoopTake do not interpret NINJAM policy. Their callback traversal reads the
already-published immutable station/take snapshots rather than mutating job/UI
containers.

The ordering and unequal-length invariants are covered by
[`NinjamTimingIntegration_Tests.cpp`](../test/JammaLib_Tests/src/ninjam/NinjamTimingIntegration_Tests.cpp)
and
[`LoopTakeTiming_Tests.cpp`](../test/JammaLib_Tests/src/engine/LoopTakeTiming_Tests.cpp),
including restore-before-rebase, `M`/`2M`/`3M`, independent origins, MIDI/audio
coupling, stale values, reconnect, and `NoSync` cursor preservation.

## Authority, versions, and generations

These counters have different scopes:

- **Session epoch** identifies one physical remote-authority lifetime. Physical
  loss, explicit reconnect, and recovered availability prevent old session
  state from crossing into the new epoch.
- **Desired version** orders complete publications within and across that
  lifecycle. It is the primary latest-newer mailbox/application identity.
- **Command generation** identifies accepted replacement/join/discipline work.
  Timer and each LoopTake retain audio-thread consume-once gates, which reset on
  epoch/`NoSync` invalidation.
- **Map state** has no independent authority generation. Its geometry and origin
  are audio-thread-owned and rebased coherently with the accepted command.
- **Entity/local generations** such as queued correction gates and local
  reclock/tap generations remain local engine concerns. They are not remote
  session epoch, desired version, or remote BPI authority.

Diagnostics may mirror epoch/version/generation for correlation but never become
a second authority source.

## Follow policies and lifecycle

Remote timing is accepted only when BPM, full BPI, interval, sample rates, and
both observation anchors are plausible. BPI-less remote observations are
unsupported. Disconnected local BPI inference remains a separate Quantiser
calculation.

- `ContinuousSync` follows remote tempo near local timing.
- `BlockSync` follows a materially different accepted tempo using the same map
  and anchor algorithm.
- `NoSync` clears remote authority, map, anchors, and command-generation gates
  without moving any local cursor. `Stay local` explicitly publishes this state;
  invalid timing, observation deadline, physical loss, disconnect, and reconnect
  lifecycle transitions also use it.

Join corrections may legitimately span up to half an interval. Ongoing wrap
discipline is bounded to two audio buffers. A newer complete desired state is
applied once; no ordered sequence of standalone invalidate/replace/delta
commands is required or preserved.

## Remote MIDI grid

An accepted remote replacement publishes `RemoteTransportGeometry` plus a
remote/device origin. The descriptor carries interval length, full authoritative
BPI, phase, BPM, and command generation. `Scene` forwards it through
`Quantiser::SetRemoteMidiGrid` to each local take.

`MidiQuantisation` computes remote boundaries directly:

```text
boundary(k) = origin + round(k * remote interval / divisions)
divisions = remote BPI * MidiQuantisation::Divisor(fraction)
```

It maps each recorded event through the take's transport start, composes user
offsets once, and publishes an immutable quantised event snapshot off the audio
callback. Event playback at the audio-thread MIDI cursor therefore uses the
implemented remote descriptor without changing recorded events or loop lengths.
`AutomationGlobalSampleOrigin()` remains a separate frozen automation anchor;
cursor movement is balanced through the LoopTake automation correction.

The visual overlay is not yet equivalent. `QuantisationLoopTakeVisual` still
contains grain/division-style data rather than the accepted remote descriptor,
and `Quantiser::ActiveGrid()` does not expose that descriptor to the renderers.
The divisibility suppression was relaxed, but correct remote-boundary rendering
for unequal/non-dividing loops and the full live overlay scenario remain
incomplete or unproven. Do not infer overlay correctness from the implemented
MIDI event snapping.

## Local geometry and persistence

Local `GrainSamps` is an exact audio construction unit. It is not the remote grid
step. For local physical recording length `R`, valid local geometry satisfies
`M = GrainSamps * Bpi` and `M <= R`; the physical buffer may retain a short tail.
Record, overdub, and punch scheduling remains action/latency based rather than
remote-grid quantised.

The persisted `transportoffsetloopfrac` is a separate local control. Missing
state defaults to zero; finite signed values are clamped to `[-1,1]`. There is no
schema/version guard, so an older binary may discard the field on resave. No
manual older-binary resave test is claimed.

## Real-time and evidence limits

Callback code must remain allocation-free, exception-free, lock-free, and free
of logging, formatting, blocking I/O, or unbounded traversal. Remote MIDI event
snapshots and Station/LoopTake membership snapshots are prepared/published away
from the callback; callback readers consume immutable state.

Automated coverage verifies the complete desired-state boundary, full remote
BPI, mapping/rebase behavior, event-boundary arithmetic, event snapping, local
signed offsets, and bounded diagnostics. Remote MIDI event snapping is
implemented, but overlay and the full live scenario are unverified. No live
prompt/grid/local-inference trace, interactive `.jam` load, older-binary resave,
bounded verbose diagnostic trace, or physical DAC-to-ADC loopback is claimed.
Export compensation remains disabled by default, and integer mapping may repeat
or skip source samples.

The `.jam` file still serializes the NINJAM password and work directory;
F-049/F-050 remediation was rejected and these remain accepted residual risks.
Generic JSON and upstream NJClient limits were not hardened. This review is not
a broad security certification.
