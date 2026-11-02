# Loop alignment and NINJAM sync

This document explains the timing model used when Jamma follows a NINJAM
transport.

## Overview

When sync is active:

1. NINJAM provides a remote interval, tempo, beat count, and current phase.
2. The audio callback applies accepted remote timing and maps elapsed time into
   each recording baseline.
3. Each local audio or MIDI loop follows its baseline using its own length and
   saved phase.
4. A timing update changes phase and tempo; it does not resize or rewrite
   local loops.

This means loops of different lengths, such as `M`, `2M`, and `3M`, remain
related to one another while following the same remote transport.

## Three timing domains

Keep these values separate:

- **Device sample**: the audio callback's monotonic sample counter. Remote
  observations are anchored to this counter.
- **Scene sample**: Jamma's monotonic elapsed-audio counter. It continues through a
  remote timing replacement.
- **Loop sample**: a position inside one audio buffer or MIDI event sequence.
  It wraps at that loop's own length.

A device sample is not a loop position, and a remote phase is not a cursor that
can simply be copied into every local loop.

Scene samples measure continuity, not the musical playhead. Musical transport
position can be relocated; the mapped local source coordinate converts elapsed
scene samples onto the ruler used by local loop buffers. The frozen
`MidiLoop::AutomationGlobalSampleOrigin()` is a separate automation anchor,
distinct from both the MIDI event cursor and the remote-grid origin. See the
[glossary](glossary.md#timing-synchronisation-and-quantisation-language) for the
coordinate and ownership definitions.

## How remote time becomes local time

The sync map stores two interval lengths and an origin:

- `M_local`: the local source-master interval;
- `M_remote`: the remote interval;
- the scene position where the map was established; and
- the local source coordinate at that position.

For scene elapsed time `d`, the mapped local source coordinate is:

```text
local = local_origin + round(d * M_local / M_remote)
```

The implementation performs this with integer arithmetic. It handles complete
remote intervals separately from the remainder, so an exact remote boundary
lands on an exact local boundary. It does not use a floating-point phase
accumulator.

This mapping preserves phase relationships. It is not sample-perfect time
stretching: when the rates differ, a source sample may be repeated or skipped.

## Why each loop keeps its own phase

A sync map describes timing geometry, not a common loop cursor. Takes recorded
under accepted remote authority retain their source-master interval and BPI.
Legacy recordings retain AudioHost's shared source map. Each loop stores an
anchor relating its current phase to the appropriate source coordinate:

```text
anchor = local_source_coordinate - loop_phase (mod loop_length)
loop_phase = local_source_coordinate - anchor (mod loop_length)
```

A correction moves entities recorded under the same baseline by the same source
amount; recordings from other baselines interpret the accepted remote phase in
their own source units. Each loop still wraps by its own length, preserving audio,
MIDI and intentional offsets without forcing lengths to match.

When a map is first established, playable loops capture anchors. Later timing
corrections and map rebases retain those anchors. A loop without an anchor may
capture its current phase when it is first restored.

## What happens at an audio boundary

`AudioHost` owns audio-boundary application of accepted timing. The NINJAM
timing coordinator owns remote authority, validation, and follow decisions.
At the start of an audio block, `AudioHost`:

1. consumes the newest complete remote timing publication;
2. clears remote authority on `NoSync` or a new session epoch;
3. projects the timestamped remote phase to the current device boundary;
4. restores the existing mapped source position before changing the map;
5. applies one source correction to local stations and takes;
6. rebases the map; and
7. applies the separate local transport offset, if one is set.

Normal station advancement happens after this boundary work. `Station` and
`LoopTake` apply the source correction to their own loops; they do not decide
NINJAM policy or replace the timer.

For join observations, the coordinator accepts phase errors up to half the
local master interval. For ongoing wrap observations, it rejects errors larger
than twice `DefaultBufferSizeSamps`. This is an observation acceptance limit,
not a clamp on the eventual source correction or a limit based on the configured
audio buffer size.

## Follow modes and loss of sync

- **ContinuousSync** follows a remote transport that is close to local timing.
- **BlockSync** accepts a materially different remote tempo and maps local
  source progress to the remote interval in the same way.
- **NoSync** returns to local timing.

`NoSync` clears remote authority, the sync map, anchors, and one-shot timing
 gates. It does not move audio cursors, MIDI cursors, or automation phase. The
local transport offset is separate and remains active while disconnected or in
`NoSync`.

A remote update is accepted only when its tempo, full beats-per-interval,
interval length, sample rate, and device observation anchors are consistent.
Incomplete or stale observations do not become timing authority.

## Remote MIDI grid

Remote timing also supplies a musical grid for MIDI quantisation. The grid has
an interval, authoritative beats-per-interval, quantisation fraction, and an
origin on the accepted transport ruler.

A boundary is calculated as:

```text
boundary(k) = origin + round(k * interval / divisions)
divisions = beats_per_interval * fraction_divisor
```

Rounded rational boundaries are calculated directly rather than by repeatedly
adding a rounded step. Playback, the loop-grid editor, and visual overlays use
the same boundary arithmetic. A loop-level quantisation setting overrides the
inherited setting for that stream; the loop's logical length does not change.

The local grain used to construct a loop is different from the remote MIDI
grid. Do not use a remote grid step as a local recording length.

## Local transport offset

The local transport offset is an independent control expressed as a fraction
of the current master interval. It is an absolute target, not a queue of
incremental deltas. Missing saved state means zero; finite values are clamped
to `[-1, 1]`.

Because it is independent of NINJAM authority, the offset can still be applied
while disconnected or after entering `NoSync`.

## Implementation boundaries

The main ownership boundaries are:

- [`NinjamLoopAlignment.h`](../JammaLib/src/ninjam/NinjamLoopAlignment.h):
  integer mapping, modulo arithmetic, and the sync-map data structure;
- [`AudioHost.cpp`](../JammaLib/src/audio/AudioHost.cpp): remote timing
  acceptance at the audio boundary, timer replacement, map correction, and
  rebase;
- `Station` and `LoopTake`: per-entity anchor capture, restore, and phase
  correction;
- `Quantiser` and `MidiQuantisation`: remote grid propagation and rounded
  event boundaries.

The audio callback must remain allocation-free, lock-free, exception-free, and
free of logging or blocking work. Membership and MIDI snapshots are prepared
outside the callback and published for callback-safe reading.

## Useful invariants when changing sync code

- Never assign one wrapped master cursor to every local loop.
- Never confuse device samples with scene samples or loop samples.
- Restore the mapped source before rebasing an existing map.
- Preserve anchors during ordinary corrections; clear them only when remote
  authority is invalidated.
- Keep remote desired transport and local transport offset as separate
  latest-value controls.
- Use one rounded rational boundary calculation for playback, editing, and
  visuals.

The core behavior is covered by the timing tests in
[`NinjamTimingIntegration_Tests.cpp`](../test/JammaLib_Tests/src/ninjam/NinjamTimingIntegration_Tests.cpp)
and
[`LoopTakeTiming_Tests.cpp`](../test/JammaLib_Tests/src/engine/LoopTakeTiming_Tests.cpp).

## Changing server timing while connected

Before any local take exists, accepted BPM/BPI changes replace both remote timing
and the empty session's source ruler. A first recording therefore uses the current
server tempo rather than the tempo observed at join. Recording or retained takes
prevent an empty-session ruler replacement.

**Reclock** now suspends local playback's remote follow without disconnecting.
Remote observations and the remote metronome continue. The next completed local
master establishes local timing; only after its audio activation is acknowledged
does Jamma request that BPM and full BPI using the existing server admin/vote path.
A tap sequence with one eligible completed take since fresh/reclock reinterprets
that master and requests its timing too. Further taps replace pending timing
preparation and server intent. With additional eligible takes, taps continue to
select subdivisions instead of changing the server.

A fresh observation confirming both requested BPM and BPI resumes following.
Jamma aligns local playback with projected server phase; this may audibly jump
playback. The metronome always remains a reference to actual server timing, not
the proposed local timing. Other takes retain their own lengths and relative
offsets. A declined vote, exhausted retry, deadline or invalid/lost remote timing
leaves playback local; a sent request alone never means the server accepted it.
The Timing settings page displays whether playback follows the server, is local,
is waiting for a new master, or is waiting for a server request/observation.

**Sync to server** cancels a pending local tempo request and the previous follow
choice, including Stay local. It waits for a fresh validated observation even if
the server BPM/BPI is unchanged, then follows that timing. It does not request a
server tempo change or erase recorded material/source geometry. Reclock and Sync
to server therefore have distinct purposes: propose a new local master to the
server, or follow the server's current timing.

Follow revision and any explicit local source-master interval are retained in
every complete desired publication. A latest-value mailbox may skip intermediate
NoSync/replacement values without losing the need to reset stale map anchors.

## Recordings after an accepted remote tempo change

Accepting new remote BPM/BPI establishes the reference for future independent
recordings. It resets recorded-master eligibility like Reclock, while retaining
the live remote clock, beat grid and metronome. It does not clear Timer, infer
server tempo from the next take or send another server vote. The first completed
independent take becomes the new recorded-master reference; older takes and their
overdubs do not take that role. Acknowledgement of an explicit local reclock/tap
request retains the master just established locally.

At actual capture admission, each new take freezes the full accepted source
interval/BPI. A take recorded at 128 BPM plays at normal recorded speed while the
remote tempo remains 128 BPM, even if older 113 BPM material remains present.
It still consumes every accepted remote phase/drift correction; normal recorded
speed does not mean free-running playback. Subsequent remote changes discipline
it using the same integer map/anchor behavior as earlier recordings, expressed in
its own source units. This also works across repeated tempo changes and after
all previous takes are ditched, without needing another remote geometry change.

Overdub/punch retains the source take's baseline. NoSync clears active maps and
anchors without erasing source geometry or moving cursors. Optional JAM take
fields `recordingMasterLengthSamps` and `recordingBeatsPerInterval` preserve the
baseline across save/load; missing/invalid pairs use the legacy shared-map path.
Source sample counts follow the existing material loader's unchanged sample
counts when device sample rate changes; active follow maps are reset.

Remote recording lengths use rational boundaries from the full interval/BPI,
including complete intervals with odd BPI. Rounded beat samples remain a local
grain representation and cannot replace authoritative remote interval lengths or
beat counts. Disconnected local-grain inference and quantization-off duration
remain on their existing paths. BlockSync and ContinuousSync retain their existing
map/anchor advancement; this change does not introduce a new stretching algorithm.

Verbose PlaybackTake diagnostics include `recordingMaster` and `recordingBpi`.
Zero means the legacy shared source map applies; a nonzero value identifies the
take's own recording ruler, so the global PlaybackTrace map is not mistakenly
interpreted as the rate for every take.

## Metronome

The metronome defaults off locally. Once the first master establishes Timer's
interval, it accents the master start and divides that interval into local BPI
beats, using rounded rational sample boundaries. Reclock silences the local
click until the next master establishes timing. The Level slider below the
toggle scales monitor output from silence to the original click level.

An actual NINJAM session entry enables the metronome once, remembering the local
toggle; departure restores that choice. Changing the toggle while connected
lasts for that session. Remote observations and tempo retries do not enable it
again. Connected clicks always use actual server geometry and phase, including
Stay local and Reclock; unavailable remote timing remains silent. Clicks are
mixed after NINJAM export and are never sent to the server.
