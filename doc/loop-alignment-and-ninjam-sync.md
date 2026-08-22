# Loop Alignment and NINJAM Sync

This is the working reference for local loop phase preservation and NINJAM
tempo joining. Treat all positions as sample positions. The audio callback owns
live Timer and loop cursor mutation; cross-thread timing messages must use the
existing fixed-size, latest-wins mailbox.

## Local Transport and Loop Recovery

`utils::Timer` owns the local master transport:

$$
A = C \times M + p
$$

where $M$ is master/interval length in samples, $C$ is `LoopCount`, $p$ is
`SampOffset`, and $A$ is `AbsoluteSamplePos`. `Timer::SceneSamplePos` ($S$) is
a monotonic scene sample coordinate which does not reset when NINJAM replaces
Timer geometry. Use $S$ for durable NINJAM recovery anchors.

Each playable entity has its own wrapped phase $q_i$ and length $L_i$:

- Audio: use `Loop::BodyPlayIndex`, not fade-prefixed storage `PlayIndex`.
- MIDI: use `LoopTake`'s MIDI event cursor (`_midiVisualPlayIndex`).
  `MidiLoop::LoopPhaseAnchor` is an automation-recording anchor, not the MIDI
  event cursor.

Capture an entity's scene-relative anchor at a common audio boundary:

$$
a_i = (S-q_i) \bmod L_i
$$

At a later scene coordinate, restore it with:

$$
q_i = (S-a_i) \bmod L_i
$$

If remote alignment supplies a common correction $d$, use:

$$
q_i' = (S+d-a_i) \bmod L_i = (q_i+d) \bmod L_i
$$

The last equality is the key invariant: one global correction preserves every
intentional relative offset. Example: local master phase $0.5M$, a $2M$ loop
at $0.75(2M)$, and $d=-0.5M$ puts the master at zero and the long loop at
$0.5(2M)$.

Do not recapture anchors at each remote wrap. Capture fresh anchors once at the
first material follow transition of a connection, retain them through that
connection's boundary restores, and invalidate them before the next independent
follow session so the next capture reflects current local phase. Invalidating
an anchor does not move a cursor or discard phase.

## Grain and Geometry

Local quantisation grain is $G$. A loop is locally grain-clean when:

$$
L_i \bmod G = 0
$$

For continuous recurrence under remote grain $G_r$ and remote interval $M_r$,
every playable audio and MIDI loop must satisfy:

$$
L_i \bmod G_r = 0 \quad\text{and}\quad M_r \bmod L_i = 0
$$

If either condition fails for any playable entity, do not pretend it is a
continuous grid match. Use durable per-loop boundary restoration so relative
phase remains intentional even if the remote grid cannot contain every loop.

## NINJAM Timing Flow

The audio callback obtains NINJAM timing, converts remote samples to device
sample rate, and publishes an observation. The job/UI side runs
`NinjamTimingCoordinator`, which produces one `NinjamAudioTimingCommand`.
`AudioHost` consumes that command at the top of an audio block, before local
station advancement, and applies it to Timer and every local take in that same
block.

Remote interval conversion is:

$$
M_d = \operatorname{round}\left(M_r\frac{f_d}{f_r}\right)
$$

Remote timing is valid only with plausible BPM/BPI and nonzero interval/sample
rate. Do not use transient NJClient pre-handshake values as authoritative.

There are three follow policies:

- `SeamlessDiscipline`: old and new Timer seed length and grain are identical.
  Keep geometry and apply signed circular phase corrections only.
- `ContinuousRemote`: material replacement, but every playable loop is
  compatible with remote grain and interval. Replace Timer geometry and follow
  remotely without per-boundary hard restores.
- `BoundaryRestore`: material replacement and at least one loop is
  incompatible. Replace Timer geometry, preserve per-loop anchors, and restore
  each local audio/MIDI phase at every remote wrap, including zero-delta wraps.
- `StayLocal`: do not apply a remote replacement or phase movement.

For ordinary phase discipline, compute the signed shortest delta between local
and remote phase. Joins may correct up to half an interval. Ongoing wrap
discipline is bounded to two audio buffers to reject implausible drift. Apply a
valid correction exactly once by generation; stale/equal generations move
nothing. An invalidation resets this generation gate.

For a material replacement, project the observed remote phase to the actual
audio boundary before applying it. Timer receives the remote absolute phase;
local loops receive the one corresponding local delta or the anchor restoration
formula above. MIDI event cursor moves with audio body phase. MIDI automation
correction changes by the inverse cursor translation exactly once, so automation
and MIDI events retain the same effective phase.

## Join Tempo Contract

### Intended behavior

When joining with valid local timing, the normal user path should push local
BPM/BPI automatically. The server request sends both admin (`/bpm`, `/bpi`) and
vote (`!vote bpm`, `!vote bpi`) forms. A successful client send only means the
messages entered the send path; it is not server acknowledgement.

While the request is queued or awaiting a result:

- local recording, overdubbing, and playback continue;
- retain old server timing only as context;
- do not show a dialog proposing that old timing;
- retry at usable remote boundaries and expire after a finite wall-clock
  deadline, not only after a number of long remote intervals.

A server result acknowledges a push only when it is a fresh observation after a
successful send, has the same BPI, and is near requested BPM. The intended
tolerance is within `1.0` BPM so a server-rounded result can succeed. On
acknowledgement, automatically follow the observed server interval/phase; do
not prompt to accept the stale pre-push tempo.

If the request expires or produces a materially different result, show `Current
server tempo` with `Follow server` and `Stay local`. `Follow server` publishes
one material replacement; `Stay local` publishes none. Local tempo remains
active while waiting and after `Stay local`.

### Current known caveat

The historical implementation defaulted `PushLocalTempoOnJoin` to false, and
the `/connect` command used that default. A log containing `request=0
pushLocal=0` means no local push was attempted, so an immediate old-server
dialog is expected behavior from that configuration, not acknowledgement.
`doc/ninjam-tempo-push-followup-plan.md` defines the pending fix: enable the
normal command path, use the 1 BPM acknowledgement rule, and add a wall-clock
deadline.

## Diagnostics and Tests

Use `[NINJAM][TempoJoin]` logs for request states: `queued`,
`awaiting-server-observation`, `acknowledged`, and `expired-unknown`. Use
`[LocalLoopAlignment]` logs for Timer geometry, MIDI/audio cursors, anchors,
and grain remainders. A `maxResidual=0` receipt alone is insufficient if it
only compares a value with the target just assigned; tests must verify emitted
MIDI event phase and automation phase.

Cover at least: pre-send observation rejection; old-server proposal suppression
while pending; rounded near-tempo acknowledgement; timeout fallback; equal
geometry discipline; compatible material replacement; incompatible MIDI loops
over three remote wraps; and disconnect/reconnect anchor recapture.

Relevant implementation: `utils/Timer`, `audio/AudioHost`,
`ninjam/NinjamTimingCoordinator`, `ninjam/NinjamLoopAlignment`, `engine/Loop`,
`engine/LoopTake`, and `engine/Station`. For implementation work, also read
`doc/ninjam-tempo-push-followup-plan.md`.