# NINJAM Tempo Push and Join UX Plan

## Goal

Joining NINJAM preserves two deliberately different behaviors.

1. When remote BPM, BPI, grain, and master interval geometry are effectively
   identical to local timing, retain the existing seamless phase-discipline
   path. Small audio-boundary deltas move the Timer, every audio cursor, MIDI
   event phase, and MIDI automation correction together. Never add periodic hard
   resets to this path.
2. When remote geometry is materially different, follow remote timing and restore
   each incompatible local loop from its own scene-relative anchor at remote
   interval boundaries. Audible jumps are acceptable; loss of intentional
   relative alignment between local loops is not.

An empty-session tempo push remains asynchronous and fallible. Local playback,
recording, and overdubbing continue while pending. Successful chat-message send
is not acknowledgement; only a fresh matching server timing observation is.

## Discovery Findings

Small-model read-only discovery completed on 2026-08-01. No `matt-it` skill or
agent exists in the workspace, user skill directories, or available registry,
so its phase-specific rules could not be loaded. Implementation used
`GPT-5.6 Terra (copilot)` followed by local review and repairs.

### Request lifecycle

- `Scene::ConnectNinjam` captures local timing before network connection.
- With push-local enabled, `NinjamTimingCoordinator` queues the request and emits
  it on the first usable remote join/wrap.
- `NinjamConnection::RequestServerTempo` sends admin (`/bpm`, `/bpi`) and vote
  (`!vote bpm`, `!vote bpi`) messages. Its return value proves delivery to the
  client send path only.
- Acknowledgement requires exact BPI and BPM within `0.01`, a successful send,
  and an accepted observation strictly newer than that send.
- Old server timing is retained as context but cannot create an apply popup while
  a request is queued or awaiting an observation. Expiry creates one proposal
  from the latest server timing and honestly represents the result as unknown.

### Timing and alignment

- One fixed-size latest-wins command is consumed at the top of the audio block.
  Timer and all local takes therefore move at the same boundary.
- Equal geometry keeps the existing signed common-delta path.
- Material replacement is classified on the audio thread from immutable station
  snapshots. No mutable loop data crosses to the job thread for classification.
- Active incompatible mode persists after replacement. Every remote wrap emits a
  unique command, including zero-delta wraps, so restoration is not skipped.

For remote grain $G$, interval $M$, and loop length $L_i$, continuous recurrence
requires both:

$$
L_i \bmod G = 0
$$

$$
M \bmod L_i = 0
$$

For shared monotonic scene coordinate $S$, body/event phase $q_i$, and durable
anchor $a_i$:

$$
a_i = (S - q_i) \bmod L_i
$$

A remote correction $d$ restores:

$$
q_i' = (S + d - a_i) \bmod L_i
$$

The $d$ term is required for both the initial remote phase transition and later
nonzero wrap corrections. Audio uses the loop body index, excluding fade-buffer
storage. MIDI applies the same target phase and updates automation correction by
the inverse cursor translation exactly once.

## User-Facing Contract

1. **Connecting locally**: transport and local operations continue unchanged.
2. **Request queued**: local tempo will be requested at the first usable boundary.
3. **Awaiting server observation**: current server timing is not offered as an
   accepted result. Local operations continue.
4. **Server confirmed**: a fresh matching observation acknowledges and, when
   geometry changed, emits exactly one replacement command.
5. **Expired/unknown**: the dialog is titled `Current server tempo` and offers
   `Follow server` or `Stay local`.
6. **Disconnected**: request/proposal state clears and pending audio commands are
   invalidated.

The current implementation exposes queued/awaiting/acknowledged/expired states in
`[NINJAM][TempoJoin]` logs. It deliberately avoids a modal pending popup so users
can continue adding loops. A dedicated non-modal status indicator remains a
future presentation refinement, not a correctness blocker.

## Follow Policies

- `SeamlessDiscipline`: equal geometry; preserve the current bounded common-delta
  behavior and small-correction safety limit.
- `ContinuousRemote`: material replacement where every playable audio and MIDI
  loop is grain-clean and divides the remote interval.
- `BoundaryRestore`: at least one playable loop is incompatible; restore each
  durable anchor at every remote boundary.
- `StayLocal`: emit no replacement or discipline movement.

## Implemented Changes

- Added pure positive-modulo, anchor capture, delta-aware phase restoration,
  residual, and compatibility helpers.
- Added fixed-size follow policy and scene coordinate fields to the timing command
  mailbox.
- Added a monotonic scene sample coordinate that is independent of Timer geometry
  replacement.
- Added durable audio body and MIDI anchors captured before material replacement.
- Added audio-thread compatibility classification and persistent boundary-restore
  mode.
- Added exact-once generation gates for Timer and takes; equal/stale generations
  move nothing, while invalidation resets the gate.
- Added successful-send freshness to tempo acknowledgement and retry re-anchoring
  after failed sends.
- Added suppression of stale server proposals while a local request is plausible;
  expiry creates one latest-server fallback proposal.
- Added truthful fallback dialog source and action labels.
- Added job-thread lifecycle logs with join/request IDs and state counters.
- Added atomically bracketed applied-command receipts containing policy, scene
  coordinate, and delta.
- Added atomically bracketed per-take boundary receipts containing generation,
  scene, delta, audio/MIDI entity counts, and maximum modular residual. Formatting
  remains off the callback.

## Thread Ownership

| State | Writer | Reader | Synchronisation | Teardown |
|---|---|---|---|---|
| Request/proposal | Scene job thread | Scene job/UI | Single coordinator owner | Disconnect reset |
| NJClient request | Scene job thread | NJClient/network | Existing connection mutex | Connection teardown |
| Remote timing | Audio callback | Scene job thread | Existing atomic mailbox | Invalid observation/reset |
| Timing command | Scene job thread | Audio callback | Existing atomic latest-wins mailbox | Invalidate command |
| Command receipt | Audio callback | Scene job thread | Odd/even atomic publication guard | Host lifetime |
| Per-take receipt | Audio callback | Scene job thread | Odd/even atomic publication guard | Take lifetime |
| Anchors/cursors | Audio callback | Audio callback | Audio ownership after immutable snapshot publication | Take lifetime |

No new lock, allocation, wait, exception, stream output, or formatted logging was
added to callback-owned code.

## Deterministic Coverage

Automated tests cover:

- queued send, failed-send retry, pre-send observation rejection, fresh matching
  acknowledgement, material matching acknowledgement plus replacement, expiry
  fallback, stale proposal suppression, and disconnect reset;
- equal-generation, stale-generation, and invalidation behavior;
- distinct anchors, negative modular arithmetic, incompatible grain multiples,
  and sample-rate conversion;
- audio and MIDI restoration over three boundaries with positive, negative, and
  zero deltas;
- MIDI automation phase matching the restored event cursor;
- receipt generation/scene/delta, audio/MIDI counts, and zero maximum residual.

Validation on 2026-08-01:

- Debug x64 `JammaLib_Tests` build: passed with zero warnings/errors.
- Full native suite: `790` passed, `1` device-dependent MIDI test skipped.
- Real-time hot-path audit: passed with no banned lock/wait additions.
- Workspace diagnostics and `git diff --check`: clean.

## Rubber-Duck Review

The review found and fixed two defects after the implementation-agent passes:

1. Boundary restoration originally used $(S-a)\bmod L$ and ignored the remote
   correction. It now uses $(S+d-a)\bmod L$ for audio and MIDI.
2. A fresh matching generation change originally marked the request acknowledged
   without applying its timing. It now emits the replacement in the same state
   transition.

Final coordinate checks:

- Audio anchors use `BodyPlayIndex`, not the fade-prefixed storage index.
- All loops use one monotonic `Timer::SceneSamplePos` coordinate.
- Distinct starts remain distinct because each entity retains its own anchor.
- MIDI cursor translation and automation correction have opposite signs, making
  the effective automation phase equal to the restored event phase.
- Compatibility checks every audio loop and the MIDI loop independently, rather
  than relying only on a take's largest visual length.
- Receipts and commands use fixed scalar atomics; job-thread formatting cannot
  block the callback.

## Real-Session Test Gate

This is the remaining step and requires Matt on a real NINJAM server.

Before joining, record:

- one audio-only master-length loop;
- one MIDI-only loop starting at a different beat and with a different length;
- one mixed audio/MIDI take using another grain multiple;
- one optional loop whose length does not divide the prospective remote interval.

Capture complete console output and retain all `[NINJAM][TempoJoin]` and
`[LocalLoopAlignment]` lines through at least three remote wraps for each case:

1. Empty server: push local tempo, record another loop while awaiting the server,
   then observe confirmation.
2. Remote timing differs only by sample rounding.
3. Materially different compatible remote timing.
4. Materially different incompatible remote timing.
5. Expired/unknown request: choose `Stay local`; reconnect and choose
   `Follow server`.

Deterministic log assertions:

- one `connect` line and one monotonic join/request ID per run;
- state order is `queued` -> `awaiting-server-observation` -> `acknowledged`, or
  `expired-unknown`; acknowledgement never precedes a successful send;
- the near-identical case contains no `policy=boundary-restore` lines;
- a material accepted change contains exactly one replacement snapshot;
- incompatible content produces `event=ninjam-boundary-restored` for three
  successive wraps;
- each corresponding `alignment-receipt` has increasing generation/sequence,
  nonzero expected audio/MIDI counts, and `maxResidual=0`;
- the logged intentional `masterAnchor` differences remain stable rather than
  collapsing to one value;
- `Stay local` produces no accepted replacement or boundary-restore receipt.

Stop here until those real-session logs are available. Any nonzero residual,
missing wrap receipt, duplicate generation, or unexpected boundary restore in the
near-identical case is a deterministic failure and should be investigated before
merging.
