# Loop Alignment and NINJAM Sync

This is the design reference for preserving local loop phase while following a
NINJAM transport. Treat positions as sample positions. The audio callback owns
live Timer and loop-cursor mutation; cross-thread timing messages use the
existing fixed-size, latest-wins mailbox.

## The sync-map idea

Following a remote transport changes the master interval, and sometimes the
master phase. Correcting only `utils::Timer` is not enough: each local loop may
have a different length and may intentionally start at a different relative
position. Giving every loop the same raw cursor would destroy those musical
relationships.

The sync map is the bridge between two master rulers. At a sync boundary it
remembers:

- the local master interval, `M_l`;
- the remote master interval, `M_r`;
- the monotonic scene coordinate at the boundary, `S_0`; and
- each local loop's phase at that coordinate.

Later, remote elapsed time is converted into equivalent local-master progress.
Every local audio or MIDI entity receives that same mapped elapsed time, then
wraps it in its own coordinate system. This is why different loop lengths and
intentional offsets remain stable while the common transport follows remote
time.

This is a common master timeline, not a shared loop cursor. The master phase
describes where the transport is inside its interval; a loop phase describes
where one particular loop is inside its own buffer.

## Local transport and loop phase

`utils::Timer` owns the local master transport:

$$
A = C \times M + p
$$

where $M$ is the master/interval length in samples, $C$ is `LoopCount`, $p$ is
`SampOffset`, and $A$ is `AbsoluteSamplePos`. `Timer::SceneSamplePos` ($S$) is
a monotonic scene coordinate that does not reset when NINJAM replaces Timer
geometry. Use $S$ as the durable time ruler for sync-map anchors.

Each playable entity has its own wrapped phase $q_i$ and length $L_i$:

- Audio uses `Loop::BodyPlayIndex`, not fade-prefixed storage `PlayIndex`.
- MIDI uses `LoopTake`'s MIDI event cursor, `_midiVisualPlayIndex`.
  `MidiLoop::LoopPhaseAnchor` is an automation-recording anchor, not the MIDI
  event cursor.

At a common audio boundary, capture a scene-relative anchor:

$$
a_i = (S_0-q_i) \bmod L_i
$$

At a later scene coordinate, restore that entity with:

$$
q_i(S) = (S-a_i) \bmod L_i
$$

If a common correction $d$ is applied, then:

$$
q_i' = (S+d-a_i) \bmod L_i = (q_i+d) \bmod L_i
$$

The final equality is the important invariant: one global correction changes
every phase by the same scene-time amount, so relative offsets are preserved.
For example, a master at $0.5M$ and a $2M$ loop at $0.75(2M)$ can be moved by
$d=-0.5M$; the master reaches zero while the long loop reaches $0.5(2M)$.

Do not recapture anchors at every remote wrap. Capture them once at the first
material follow transition for a connection, retain them through that
connection's restores, and invalidate them before the next independent follow
session. Invalidating an anchor does not move a cursor or discard phase; it
only permits the next session to capture the current phase as its new origin.

## Mapping remote time to local time

The sync map converts elapsed remote time into local-master progress:

$$
E_l = \operatorname{round}\left((S-S_0)\frac{M_l}{M_r}\right)
$$

The implementation calculates this as whole remote intervals plus a rounded
remainder using integer sample arithmetic. It is not a floating-point phase
accumulator. Each entity is then restored as:

$$
q_i(S) = (q_{i0} + E_l) \bmod L_i
$$

where $q_{i0}$ is its captured phase at `S_0`. The same $E_l$ is used for every
entity, but each entity applies its own modulo $L_i$. Thus a 2-master-length
loop and a shorter loop can have different raw indices while remaining tied to
the same transport moment.

Until rate adjustment exists, integer remapping may repeat or skip source
audio/MIDI samples. These effects are deliberately small for `ContinuousSync`
and can be conspicuous for `BlockSync`; the map preserves phase relationships,
not sample-perfect time stretching.

## Map lifecycle and AudioHost ordering

For an accepted sync policy, `AudioHost` consumes one timing command at the top
of an audio block. It applies the command coherently to the Timer and every
local station, before normal station advancement. A replacement records the
old local master length and the new remote master length; all accepted sync
commands then establish or rebase the map at the command's scene coordinate.

The validity guard around `syncPhaseMap` requires both master lengths to be
nonzero. When valid, `AudioHost` calls `RestoreSyncPhaseMap` for each local
station. That fans into every local take, which restores each audio loop and
MIDI cursor from its own anchor and the common mapped elapsed time.

The restore before discipline is intentional. `EndMultiPlay` advances local
cursors at device rate. A remote discipline command can arrive after that
advancement but before the source-rate map is rebased. Restoring at this exact
boundary removes the one-block device-rate residue; otherwise different takes
could capture slightly different origins and lose their intentional relative
offsets. The new common correction is then applied, and the map is begun again
at that same scene coordinate.

On later blocks, the active map reconstructs local cursors from the current
scene coordinate. On a new join or material replacement, fresh anchors and map
origins are captured. On `NoSync`, invalidation, or disconnect, the map and
scene anchors are cleared so local timing can free-run.

MIDI event-cursor translations subtract the identical translation from the MIDI
automation correction. This keeps effective automation phase equal to MIDI
event phase across wrap, repeat, and skip movement.

## Remote timing and follow policies

Tempo-join request/acknowledgement and `Follow server` / `Stay local` behavior
are NINJAM session concerns documented in [Ninjam Integration Guide](ninjam.md).
This document covers the phase-map mechanics after a follow decision has been
accepted.

Remote intervals are converted to the device sample rate before entering the
map:

$$
M_d = \operatorname{round}\left(M_r\frac{f_d}{f_r}\right)
$$

Remote timing is authoritative only when BPM/BPI, interval, and sample rate
are plausible. Transient pre-handshake values are not valid geometry.

There are three local follow policies:

- `ContinuousSync` follows a remote tempo close to local timing. Corrections
  should normally be small.
- `BlockSync` follows a materially different accepted tempo. It uses the same
  map and anchor model, but the correction can be visibly larger.
- `NoSync` clears the map and scene anchors and leaves Timer geometry and local
  cursors free-running. It is used for `Stay local`, invalidation, and
  disconnect.

The policy changes the expected correction size, not the underlying per-loop
phase algorithm. For ordinary discipline, use the signed shortest delta
between local and remote master phase. Join corrections may be as large as
half an interval; ongoing wrap discipline is bounded to two audio buffers to
reject implausible drift. A valid correction is applied once per generation;
stale or equal generations move nothing. Invalidation resets that generation
gate.

## Local geometry and the active-grid migration

The local grain is an exact audio construction unit, not a remote beat and not
necessarily the current musical grid step.  For a first local recording with
physical length $R$, candidate geometry satisfies $M = G \times BPI$ and
$M \leq R$. The worktree retains the current master/timing fields separately;
persisted `LocalAudioGeometry` storage is still pending. The logical source
interval $M$ may exclude a short recorded tail; the physical buffer remains
$R$ and loop readers always wrap within their own logical/physical bounds.

The active quantisation grid is separately represented as a division count
$D$ of its current interval. Its boundary is evaluated directly as

$$
Q(k) = \operatorname{round}(kM/D)
$$

instead of repeatedly adding a rounded step. Thus $Q(0)=0$ and $Q(D)=M$ even
when interior cells differ by one sample. This is the target representation for
MIDI, visual overlays, and PPQ; those consumers still use integer timing while
their migration is in progress. Audio loop construction uses $G$. Record,
overdub, and punch trigger scheduling is not grid-quantised and retains its
action/latency timing.

The current tap gate is one committed `LoopTake` in the current
start/reclock generation; a take may contain multiple channel loops.
Pre-reclock loops remain playable but do not close that gate. Rebuilding from
the immutable $R$, remote-authority gating, and frozen-grid selection are
remaining migration work.

## Grain and continuous recurrence

A loop is locally grain-clean when:

$$
L_i \bmod G = 0
$$

For continuous recurrence under a remote grid step $G_r$ and interval $M_r$, a
playable audio or MIDI loop also needs:

$$
L_i \bmod G_r = 0 \quad\text{and}\quad M_r \bmod L_i = 0
$$

If either condition fails, the remote grid cannot contain every loop as a
perfect repeating subdivision. Do not represent that as a continuous grid
match. The current sync map preserves relative phase; separating remote
geometry so it never replaces local Timer timing remains migration work.

## Relevant implementation

The main building blocks are `utils/Timer`, `audio/AudioHost`,
`ninjam/NinjamTimingCoordinator`, `ninjam/NinjamLoopAlignment`, `engine/Loop`,
`engine/LoopTake`, and `engine/Station`.
