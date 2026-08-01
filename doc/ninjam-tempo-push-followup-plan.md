# NINJAM Tempo Push and Local Phase Preservation Follow-Up

## Goal

When a user joins a NINJAM server with existing local loops, Jamma should first
attempt to push the local BPM/BPI. It must not offer the server's old timing
while that request is plausibly pending. If the server adopts a near-equivalent
tempo, follow the observed server timing while preserving every local loop's
phase relationship through one common sample offset.

This plan fixes behavior observed in `sync_logs.txt`:

- `/connect` used `PushLocalTempoOnJoin=false`, so the old server timing was
  immediately proposed.
- acknowledgement requires BPM within `0.01`, which is too strict for a
  server that rounds or quantises a successful request;
- scene anchors are only captured when absent, so a later join can reuse an
  obsolete baseline;
- current boundary receipts prove only that a cursor was assigned its target,
  not that emitted MIDI event and automation phase remain correct.

## Required Behavior

1. The normal `/connect` path pushes local timing whenever a valid local timing
   exists. Log `pushLocal=1`, then `queued`, then `awaiting-server-observation`.
2. While a push is queued or awaiting a result, suppress the old remote timing
   dialog. Successful chat delivery is not acknowledgement.
3. A fresh server observation acknowledges the request when BPI is equal and
   server BPM is within `1.0` BPM of requested BPM. It must occur after a
   successful send.
4. On acknowledgement, adopt the actual observed server interval/phase without
   showing a dialog. This accepts the server's quantised result, rather than
   assuming the requested floating BPM was applied exactly.
5. If no near-local observation arrives before a finite, wall-clock deadline,
   present `Current server tempo` using the latest observed server timing.
   Keep boundary retries, but do not make expiry depend only on remote interval
   length.
6. Following remote timing preserves local phase. If the local master is moved
   by correction $d$, every local loop must satisfy:

$$
q_i' = (q_i + d) \bmod L_i
$$

   For boundary restore, capture at the join transition:

$$
a_i = (S-q_i) \bmod L_i
$$

   Then restore at each later remote boundary:

$$
q_i' = (S+d-a_i) \bmod L_i
$$

   Do not recapture anchors on later remote wraps.

## Implementation Steps

### 1. Enable push-local in the user command path

- In `Jamma/src/Main.cpp`, make `/c` / `/connect` call `Scene::ConnectNinjam`
  with `NinjamTempoJoinOptions` whose `PushLocalTempoOnJoin` is true.
- Prefer setting the option's default to true as well, unless another explicit
  call site needs passive-follow behavior. Keep the option for tests and future
  UI configuration.
- Preserve `PromptBeforeApplyingRemoteTempo=true`.

### 2. Make acknowledgement tolerate server rounding

- In `NinjamTimingCoordinator`, replace the `0.01f` request-match tolerance
  with a named constant, `TempoRequestAcknowledgementToleranceBpm = 1.0f`.
- Require exact BPI equality, successful send confirmation, and observation
  ordinal strictly newer than the ordinal recorded at send time.
- Keep proposal equality (`_SameTempo`) strict enough to identify real server
  timing changes; do not accidentally use the acknowledgement tolerance for
  popup de-duplication.
- On a matching generation change, return `_AcceptTempoChange` immediately.
  On a matching wrap without a generation change, mark acknowledged but do not
  replace unchanged device geometry.

### 3. Add a wall-clock request deadline

- Store the first successful-send time in `NinjamTimingCoordinator` using a
  steady-clock timestamp supplied from the non-audio/job side, or inject a
  monotonic elapsed value into `Observe`.
- Use a named default deadline such as 15 seconds. Retry at remote boundaries
  as today, but expire when either the deadline is reached or the configured
  retry budget is exhausted.
- Before expiry, suppress all old-timing proposals. At expiry, create exactly
  one fallback proposal from the latest valid observation and show the existing
  `Follow server` / `Stay local` dialog when local content exists.
- Do not auto-follow on expiry merely because there are zero takes if a valid
  local timing was pushed; preserve the user’s local transport unless product
  behavior explicitly chooses otherwise.

### 4. Establish fresh anchors once per follow session

- Add an audio-thread-safe way to invalidate scene anchors on `Loop` and
  `LoopTake`, including `_hasMidiSceneAnchor`.
- Do not clear anchors during a material replacement or a subsequent remote
  wrap. That would lose the captured phase relationship.
- Invalidate anchors when beginning a new NINJAM follow session, before the
  first material replacement captures fresh anchors. The next capture must use
  the current cursor and current `Timer::SceneSamplePos`:

$$
a_i=(S-q_i)\bmod L_i
$$

- Reset `AudioHost::_activeNinjamFollowPolicy` on timing invalidation and
  disconnect so a later connection cannot inherit `BoundaryRestore` from a
  prior session.
- Ensure MIDI anchor invalidation also happens when a MIDI loop is replaced,
  re-recorded, ditched, or otherwise receives a new loop length.

### 5. Verify actual MIDI event and automation phase

- Keep cursor restoration using `RestoreScenePhaseAfterDelta`.
- Extend per-take alignment receipts with values that can validate the actual
  invariant: captured anchor, restored MIDI cursor, and MIDI event/automation
  phase residual. Do not report a residual computed as `assignedTarget -
  assignedTarget`.
- Ensure the MIDI event read cursor and automation mapping both reflect the
  restored phase. The automation correction must remain the inverse of the
  cursor translation.
- Keep all receipt publication fixed-size, atomic, allocation-free, and free of
  callback logging.

## Tests

Add focused native tests in `test/JammaLib_Tests`:

1. Default `/connect` options queue a valid local tempo request.
2. A materially different old server timing produces no dialog while a request
   is queued or awaiting acknowledgement.
3. A fresh same-BPI observation within `1.0` BPM acknowledges and applies the
   observed server timing; an observation before successful send does not.
4. A same-BPI observation beyond `1.0` BPM does not acknowledge.
5. Deadline expiry produces one fallback proposal from the latest server
   observation; no early popup occurs.
6. For MIDI loops with distinct start phases and lengths $4G$, $8G$, $16G$,
   and $32G$, simulate a material replacement and at least three remote wraps.
   Assert each emitted MIDI event phase and each automation phase equals the
   expected restored phase. Include the concrete example: a master at $0.5M$,
   a $2M$ loop at $0.75(2M)$, and correction $d=-0.5M$ must restore that loop to
   $0.5(2M)$.
7. Disconnect, alter local playback/recording phase, then reconnect and verify
   new anchors are captured from the new current positions rather than reused.
8. Confirm no lock, allocation, wait, stream output, or formatting is added to
   the audio callback.

## Validation

- Build `JammaLib` and `JammaLib_Tests` in Debug x64.
- Run the focused coordinator, audio-command, and MIDI alignment tests, then
  the full native suite.
- Real-server check: join an empty server with local content at a materially
  different tempo. Expected log order is `pushLocal=1`, `queued`,
  `awaiting-server-observation`, then either `acknowledged` with a near-local
  observed tempo or one expired fallback dialog. The old remote timing must not
  be proposed before that outcome.