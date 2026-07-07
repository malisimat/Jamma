## Plan: Continuous NINJAM Transport Sync

Implement proper always-synced NINJAM session transport by promoting the NINJAM interval to an authoritative external transport while connected, preserving per-loop and per-looptake relative position state so playback can be re-anchored at interval boundaries without brute-force resets to zero. The recommended approach is to add an explicit transport-sync layer that tracks remote interval phase continuously, stores per-loop and per-take phase anchors relative to an unbounded master-loop timeline, and only commits the master-loop zero-alignment when the remote interval wraps. Free-running local transport remains the fallback when disconnected.

### Goals

- When disconnected from NINJAM, transport remains free-running and behaves exactly as it does now.
- When connected to NINJAM, transport always follows authoritative NINJAM interval phase, not just interval length.
- Joining mid-interval must not force every looptake and loop to jump to zero immediately.
- Master-loop zero should be committed only when the remote interval wraps.
- All looptakes and loops must resume from their mathematically correct relative positions against the master-loop timeline.
- Multi-length loops and looptakes must derive their play position from the shared master timeline rather than from a single raw interval position.

### Non-Goals

- Do not solve acoustic latency compensation yet.
- Do not solve device input/output latency calibration yet.
- Do not conflate remote visual station sync with local authoritative transport sync.

### Core Model

The connected NINJAM interval must become the authoritative external transport while connected. That transport needs more than BPM and BPI. It needs continuous interval phase, wrap detection, and a mapping into local sample time. The engine should maintain an unbounded master-loop timeline and derive local loop or looptake positions from anchors against that timeline.

The key behavioral rule is:

- Disconnected: free-running local transport.
- Connected: externally-driven continuous NINJAM sync.

The key alignment rule is:

- On join, compute pending alignment immediately.
- Do not set local master-loop zero immediately.
- Wait for the next NINJAM interval wrap.
- At that wrap, commit master-loop zero and reposition all local objects from stored relative anchors.

### Transport Math

Authoritative remote snapshot data should include:

- Remote interval length in samples.
- Remote interval position in samples.
- Remote sample rate.
- Wrap count or equivalent unbounded remote interval count.
- Local audio-thread anchor sample where this snapshot became authoritative.

Recommended derived values while connected:

- Authoritative interval start in local sample space.
- Authoritative master-loop count.
- Pending alignment between the current local master-loop position and the remote interval.
- Per-looptake or per-loop relative offsets against the master-loop timeline.

For multi-length loops, the interval only tells the engine where the shared master cycle is. Each looptake or loop then maps that master position into its own cycle length. That mapping must be per object, not inferred from a single representative loop alone.

### Existing Seams In The Codebase

- [JammaLib/src/ninjam/NinjamConnection.cpp](JammaLib/src/ninjam/NinjamConnection.cpp) is the ingress path for remote interval position, interval length, BPM, BPI, and sample rate.
- [JammaLib/src/ninjam/NinjamNetworkService.cpp](JammaLib/src/ninjam/NinjamNetworkService.cpp) is the current bridge from NINJAM snapshots into remote visuals and tempo-sync behavior.
- [JammaLib/src/timing/TimingQuantiser.h](JammaLib/src/timing/TimingQuantiser.h) and [JammaLib/src/timing/TimingQuantiser.cpp](JammaLib/src/timing/TimingQuantiser.cpp) currently handle remote tempo adoption, wrap detection, queued tempo sends, and clock setup.
- [JammaLib/src/utils/Timer.h](JammaLib/src/utils/Timer.h) and [JammaLib/src/utils/Timer.cpp](JammaLib/src/utils/Timer.cpp) provide the current unbounded loop-count plus sample-offset transport primitive.
- [JammaLib/src/engine/Scene.cpp](JammaLib/src/engine/Scene.cpp) coordinates audio-thread ticking and job-thread NINJAM snapshot pumping.
- [JammaLib/src/engine/Station.cpp](JammaLib/src/engine/Station.cpp), [JammaLib/src/engine/LoopTake.cpp](JammaLib/src/engine/LoopTake.cpp), and [JammaLib/src/engine/Loop.cpp](JammaLib/src/engine/Loop.cpp) contain the local playback state that will need coherent externally-driven reposition support.
- [JammaLib/src/engine/LoopRemote.cpp](JammaLib/src/engine/LoopRemote.cpp) and [JammaLib/src/engine/StationRemote.cpp](JammaLib/src/engine/StationRemote.cpp) are useful references for push-style externally-driven position updates.
- [JammaLib/src/midi/MidiLoop.h](JammaLib/src/midi/MidiLoop.h) and [JammaLib/src/midi/MidiLoop.cpp](JammaLib/src/midi/MidiLoop.cpp) already contain an anchor-based phase model that should inform the audio-sync design.

### Three-Phase Delivery Plan

## Phase 1: Transport Contract And State Capture

Goal: build the connected-sync contract and state plumbing without changing end-user playback behavior yet.

#### Work Items

1. Define an explicit NINJAM external transport snapshot structure that carries:
   - Remote interval length.
   - Remote interval position.
   - Remote sample rate.
   - Remote wrap tracking or unbounded interval count.
   - Local audio-sample anchor where the snapshot became authoritative.

2. Add explicit connected-sync state to the timing layer for:
   - Connected versus disconnected transport mode.
   - Pending join alignment.
   - Staged master-loop zero commit.
   - Last committed remote wrap.
   - Authoritative interval start in local sample space.

3. Add persistent relative-position metadata for local playback objects.
   Recommendation:
   - Keep the authoritative musical anchor at LoopTake level.
   - Support per-Loop derived positioning when loops inside a take differ in length.

4. Define the join handshake for a mid-cycle connection:
   - Observe current local master-loop play position.
   - Observe current remote interval position.
   - Compute pending alignment.
   - Do not commit the alignment yet.
   - Mark it for commit on the next remote interval wrap.

5. Add debug observability and logging for:
   - Remote interval position.
   - Remote wrap detection.
   - Pending alignment state.
   - Committed alignment state.
   - Derived master-loop count.
   - Derived per-object positions.

6. Review thread ownership and real-time safety for the new state.
   - Audio-thread readers must remain lock-free.
   - Job-thread snapshot ingestion must not introduce hot-path contention.

#### Deliverables

- New transport state structures and invariants are defined.
- Pending-alignment math exists and is observable.
- No behavioral transport changes are live yet.

#### Verification

1. Confirm remote interval position advances monotonically during a connected session.
2. Confirm wraps are detected exactly once per interval.
3. Confirm pending alignment stays stable between snapshots.
4. Confirm staged master-loop zero commit data does not oscillate unexpectedly.

## Phase 2: Authoritative Connected Transport And Re-Anchoring

Goal: make connected playback genuinely follow continuous NINJAM interval phase and restore local playback positions from master-loop-relative anchors.

#### Work Items

1. Extend the current timer or add a thin external-transport layer above it.
   Recommendation:
   - Use a thin external-transport layer if that keeps free-running and externally-driven semantics clean.

2. Replace the current one-shot remote seeding model with continuous remote-phase discipline.
   - Fresh NINJAM interval snapshots must continuously update authoritative external transport state.
   - Unchanged tempo must no longer imply free-running phase.

3. Compute authoritative interval start in local sample space from continuous snapshots.
   - Keep an unbounded master timeline.
   - Avoid destructive transport resets on every snapshot.

4. Implement wrap-gated master-loop zeroing.
   - When the remote interval wraps, commit the staged alignment.
   - Set master-loop play position to zero at that boundary.
   - Reconcile master-loop count.

5. Add coherent reposition APIs for local playback objects.
   - Reposition all affected loops or takes from one authoritative transport calculation.
   - Avoid independent ad hoc free-running corrections.

6. Restore looptake and loop play positions from relative master-loop anchors.
   - Use master-loop count math to derive each object position.
   - Do not infer everything from the raw interval position.

7. Reconcile MIDI quantisation, automation dispatch, and host-time consumers.
   - MIDI transport start samples.
   - Loop phase anchors.
   - Automation phase reads.
   - VST host time.
   - Representative-loop visual logic where needed.

8. Handle multi-length loop behavior explicitly.
   - Each loop maps authoritative master position into its own loop length.
   - Each looptake must remain musically coherent even when its loops do not share the same length.

#### Deliverables

- Connected transport is continuously disciplined by NINJAM interval phase.
- Master-loop zero is committed only at remote wrap.
- Local playback objects restore from stored relative anchors instead of zero-based jumps.

#### Verification

1. Join mid-interval and confirm the next remote wrap commits master-loop zero exactly on remote beat 1.
2. Confirm every looptake resumes at its mathematically expected relative position instead of jumping to zero.
3. Confirm each loop inside multi-length scenarios lands at the correct derived play position.
4. Confirm MIDI quantisation, automation playback, and VST host time remain phase-consistent before and after sync commit.

## Phase 3: Lifecycle Integration, Edge Cases, And Regression Coverage

Goal: stabilize the always-synced connected behavior across joins, reconnects, tempo changes, loop edits, and teardown.

#### Work Items

1. Enforce the transport policy:
   - Disconnected means free-running.
   - Connected means always synced.

2. Remove or narrow existing behavior that ignores fresh interval positions once tempo is unchanged.

3. Handle session transitions cleanly:
   - Join mid-cycle.
   - Disconnect mid-cycle.
   - Reconnect.
   - Remote BPM or BPI change.
   - Remote sample-rate change.
   - Loop activation while connected.
   - Overdub while connected.
   - Master-loop replacement while connected.

4. Preserve relative positions across structural edits whenever the contract allows.

5. Add regression-focused tests or harness coverage for:
   - Multi-length loops.
   - Multi-length looptakes.
   - Join-then-wrap alignment.
   - Connected/disconnected transport transitions.
   - Tempo-definition changes.

6. Tighten diagnostics and document the final sync model.

#### Deliverables

- Always-synced connected transport survives lifecycle transitions.
- Free-running disconnected transport remains unchanged.
- Regression coverage exists for the transport-anchor math and connected sync behavior.

#### Verification

1. Disconnect and confirm local transport returns cleanly to current free-running behavior.
2. Reconnect and confirm continuous sync reactivates without stale anchors.
3. Change remote tempo or BPI and confirm the engine stays continuously synced rather than reverting to one-shot seed behavior.
4. Activate or overdub loops while connected and confirm relative-position math remains correct.
5. Run the relevant native test target and add focused regression coverage where practical.

### Implementation Decisions

- Use authoritative interval phase, not just BPM/BPI-derived interval length.
- Do not repeatedly hard-reset all loop state on each snapshot.
- Preserve an unbounded master timeline and derive local object positions from anchors.
- Commit local master-loop zero only on authoritative remote wrap.
- Keep remote visual station sync separate from local authoritative transport sync.

### Open Design Choices To Settle Early

1. Whether the relative-position anchor should live primarily on LoopTake, each Loop, or both.
   Recommendation: keep the musical anchor at LoopTake level and derive per-Loop positioning where lengths diverge.

2. Whether the current Timer should be extended directly or wrapped by a thin external-transport layer.
   Recommendation: prefer a thin layer if it avoids semantic confusion between free-running and externally-driven operation.

3. How aggressive connected phase correction should be between wraps.
   Recommendation: continuously update authoritative phase state every snapshot, but defer destructive or user-visible play-position commits to remote wrap boundaries unless a tempo-definition change requires a stronger migration.

### Suggested Session Handoff Order

1. Session 1 should implement Phase 1 only and finish with strong diagnostics and no risky behavioral rollout.
2. Session 2 should implement Phase 2 only and focus on authoritative connected transport plus coherent re-anchoring.
3. Session 3 should implement Phase 3 only and harden lifecycle behavior, structural edits, and regressions.

### Ready-To-Use Handoff Summary

The next implementation session should start in the NINJAM timing and transport layer, not in the audio export wrapper. The job is to promote NINJAM interval phase to authoritative connected transport, keep disconnected transport free-running, stage mid-cycle alignment until the next remote wrap, then restore every looptake and loop from stored master-loop-relative anchors so multi-length playback resumes from the correct position without big zero-based jumps.