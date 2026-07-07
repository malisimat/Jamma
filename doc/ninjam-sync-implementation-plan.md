## Plan: Continuous NINJAM Transport Sync

Implement proper always-synced NINJAM session transport by promoting the NINJAM interval to an authoritative external transport while connected, preserving per-looptake relative position state so playback can be re-anchored at interval boundaries without brute-force resets to zero. The recommended approach is to add an explicit `ExternalTransport` layer above `Timer` that tracks remote interval phase continuously, owns the connected-sync runtime state, projects authoritative phase continuously while connected, and only commits destructive master-loop zero-alignment when the remote interval wraps. Free-running local transport remains the fallback when disconnected.

### Goals

- When disconnected from NINJAM, transport remains free-running and behaves exactly as it does now.
- When connected to NINJAM, transport always follows authoritative NINJAM interval phase, not just interval length.
- Joining mid-interval must not force every looptake and loop to jump to zero immediately.
- Master-loop zero should be committed only when the remote interval wraps.
- All looptakes and loops must resume from their mathematically correct relative positions against the master-loop timeline.
- Different looptakes may have different lengths, but loops within a single looptake must share one musical length and one authoritative take anchor.

### Non-Goals

- Do not solve acoustic latency compensation yet.
- Do not solve device input/output latency calibration yet.
- Do not conflate remote visual station sync with local authoritative transport sync.

### Core Model

The connected NINJAM interval must become the authoritative external transport while connected. That transport needs more than BPM and BPI. It needs continuous interval phase, wrap detection, and a mapping into local sample time. The engine should maintain an unbounded master-loop timeline and derive local looptake positions from anchors against that timeline.

`ExternalTransport` should be a thin adaptor over `Timer`, not a replacement for it. `Timer` remains the low-level free-running primitive. `ExternalTransport` owns the connected-sync runtime state and projects between authoritative remote phase and the underlying timer state.

Internally, `ExternalTransport` should prefer a double-buffered transport-state POCO model rather than mutating one live state object in place. One buffer is the currently published transport state used by readers. The other buffer is the staging state that receives fresh snapshot-derived updates during the current remote interval. On authoritative remote wrap, `ExternalTransport` flips the active buffer index from `0 -> 1` or `1 -> 0`, publishing the staged state atomically. Immediately after the flip, the newly inactive buffer should be refreshed from the new front buffer so the staging side starts from the latest committed state before the next set of snapshot-derived edits.

That design keeps publication explicit at remote wrap boundaries, avoids partially-applied live mutations, and still allows the staging buffer to track continuous remote phase between wraps.

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

Recommended `ExternalTransportState` POCO contents:

- Connected/disconnected mode.
- Local sample anchor for the authoritative remote snapshot.
- Authoritative remote interval length and position.
- Observed remote wrap count or equivalent unbounded interval count.
- Authoritative master-loop length, count, and play position.
- Pending join alignment and staged master-loop zero commit metadata.
- Last committed remote wrap identifier.
- Any derived local interval-start sample or equivalent projection helper data.

Recommended derived values while connected:

- Authoritative interval start in local sample space.
- Authoritative master-loop count.
- Pending alignment between the current local master-loop position and the remote interval.
- Per-looptake relative offsets against the master-loop timeline.

For different looptakes with different lengths, the interval only tells the engine where the shared master cycle is. Each looptake then maps that master position into its own cycle length. Loops inside a single take should not diverge in length; that is an invariant to enforce rather than a behavior to support.

Not all connected-sync runtime state is derivable from persisted musical state alone. Local musical state such as master-loop count, master-loop length, master-loop play position, BPM, and BPI describes the current transport shape, but live connected behavior still requires ephemeral remote bookkeeping such as the latest authoritative remote interval position, the local sample anchor where that snapshot became authoritative, pending join alignment, and last committed remote wrap.

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
   - Enough data to project to and from local free-running `Timer` state based on master-loop length, count, and play position.

2. Define the `ExternalTransportState` POCO and double-buffer publication contract.
   Recommendation:
   - Keep exactly two state buffers owned by `ExternalTransport`.
   - Treat one as published front state and one as mutable back state.
   - Update only the back state during the interval.
   - Publish by flipping the active index only when the authoritative remote interval wraps.
   - After publishing, clone the new front state into the new back state so the staging side remains current.

3. Add explicit connected-sync state to the timing layer for:
   - Connected versus disconnected transport mode.
   - Pending join alignment.
   - Staged master-loop zero commit.
   - Last committed remote wrap.
   - Authoritative interval start in local sample space.

4. Add persistent relative-position metadata for local playback objects.
   Recommendation:
   - Keep the authoritative musical anchor at LoopTake level.
   - Treat loops inside a take as one-length mirrors of the take anchor, not as independently anchored timelines.
   - Add a debug-visible invariant that loops inside a take must share one length.

5. Define the join handshake for a mid-cycle connection:
   - Observe current local master-loop play position.
   - Observe current remote interval position.
   - Compute pending alignment.
   - Do not commit the alignment yet.
   - Mark it for commit on the next remote interval wrap.

6. Add debug observability and logging for:
   - Remote interval position.
   - Remote wrap detection.
   - Front/back buffer flip events.
   - Published active-buffer index.
   - Pending alignment state.
   - Committed alignment state.
   - Derived master-loop count.
   - Derived per-object positions.

7. Review thread ownership and real-time safety for the new state.
   - Audio-thread readers must remain lock-free.
   - Job-thread snapshot ingestion must not introduce hot-path contention.
   - Publishing should be an atomic active-index or equivalent pointer flip, not a lock-heavy state copy on the reader path.

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

1. Add a thin `ExternalTransport` layer above `Timer`.
   Recommendation:
   - Keep `Timer` as the low-level primitive.
   - Let `ExternalTransport` own connected-sync runtime state and perform pure projection between authoritative remote phase and timer-facing reads and writes.
   - Implement the connected runtime state as a double-buffered POCO with wrap-gated publication.

2. Replace the current one-shot remote seeding model with continuous remote-phase discipline.
   - Fresh NINJAM interval snapshots must continuously update authoritative external transport state.
   - Unchanged tempo must no longer imply free-running phase.
   - Read-side transport queries should reflect continuous authoritative remote phase while connected.
   - Snapshot ingestion should modify only the staging buffer until the next authoritative remote wrap.

3. Compute authoritative interval start in local sample space from continuous snapshots.
   - Keep an unbounded master timeline.
   - Avoid destructive transport resets on every snapshot.

4. Implement wrap-gated master-loop zeroing.
   - When the remote interval wraps, commit the staged alignment.
   - Set master-loop play position to zero at that boundary.
   - Reconcile master-loop count.
   - Publish the staged `ExternalTransportState` by flipping the active buffer index at that same boundary.
   - Immediately copy the newly published front state back into the new staging buffer before applying later interval updates.

5. Add coherent reposition APIs for local playback objects.
   - Reposition all affected loops or takes from one authoritative transport calculation.
   - Avoid independent ad hoc free-running corrections.
   - Defer destructive re-anchoring work to join commit, wrap commit, or genuine tempo-definition changes.

6. Restore looptake and loop play positions from relative master-loop anchors.
   - Use master-loop count math to derive each take position.
   - Treat per-loop position inside a take as mechanically identical because one take must own one loop length.
   - Do not infer everything from the raw interval position.

7. Reconcile MIDI quantisation, automation dispatch, and host-time consumers.
   - MIDI transport start samples.
   - Loop phase anchors.
   - Automation phase reads.
   - VST host time.
   - Representative-loop visual logic where needed.

8. Enforce the single-length `LoopTake` invariant explicitly.
   - Different takes may still have different lengths.
   - Loops inside a single take must share one length.
   - Add debug assertions or equivalent diagnostics so mismatched loop lengths fail loudly during development.

#### Deliverables

- Connected transport is continuously disciplined by NINJAM interval phase.
- Master-loop zero is committed only at remote wrap.
- Local playback objects restore from stored relative anchors instead of zero-based jumps.

#### Verification

1. Join mid-interval and confirm the next remote wrap commits master-loop zero exactly on remote beat 1.
2. Confirm every looptake resumes at its mathematically expected relative position instead of jumping to zero.
3. Confirm loops inside a take stay length-consistent and land on the same derived take position.
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
   - Do not serialize connected NINJAM runtime state into jam files.
   - Rebuild connected-sync runtime state from fresh snapshots after connect.

5. Add regression-focused tests or harness coverage for:
   - Single-length `LoopTake` invariant enforcement.
   - Different looptake lengths across one station.
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
- Implement connected sync as an `ExternalTransport` adaptor above `Timer`, not by mutating `Timer` into a dual-semantics type.
- Keep connected-sync runtime state inside `ExternalTransport`; do not serialize it into jam files.
- Implement `ExternalTransport` runtime state as a double-buffered transport-state POCO with atomic front/back publication at authoritative remote wraps.
- Do not repeatedly hard-reset all loop state on each snapshot.
- Preserve an unbounded master timeline and derive local object positions from anchors.
- Use continuous authoritative phase projection for connected transport reads, but gate destructive re-anchoring and master-loop zero commits to remote wrap boundaries unless a tempo-definition change requires stronger migration.
- Keep the authoritative musical anchor at `LoopTake` level.
- Enforce the invariant that loops inside one take share one length.
- Keep remote visual station sync separate from local authoritative transport sync.

### Settled Design Choices

1. The relative-position anchor lives primarily on `LoopTake`.
   - Loops inside one take are not separate musical timelines.
   - Mixed loop lengths inside one take are treated as an invariant violation.

2. The current `Timer` is wrapped by a thin `ExternalTransport` layer.
   - `Timer` stays the low-level free-running primitive.
   - `ExternalTransport` owns connected-sync runtime state and projection logic.
   - `ExternalTransport` publishes staged transport-state snapshots by flipping between two internal POCO buffers on authoritative remote wrap.

3. Connected phase correction is continuous for transport reads, but destructive reposition is wrap-gated.
   - Every fresh snapshot updates authoritative connected phase.
   - User-visible re-anchoring waits for remote wrap unless tempo-definition changes require stronger migration.
   - Between wraps, fresh snapshot data updates the staging buffer rather than mutating the published state in place.

4. Connected NINJAM runtime state is runtime-only.
   - Do not persist pending alignment, last remote wrap, authoritative remote anchor sample, or similar connection-specific state into jam files.
   - Rebuild that state from fresh NINJAM snapshots on each connection.

### Suggested Session Handoff Order

1. Session 1 should implement Phase 1 only and finish with strong diagnostics and no risky behavioral rollout.
2. Session 2 should implement Phase 2 only and focus on authoritative connected transport plus coherent re-anchoring.
3. Session 3 should implement Phase 3 only and harden lifecycle behavior, structural edits, and regressions.

### Ready-To-Use Handoff Summary

The next implementation session should start in the NINJAM timing and transport layer, not in the audio export wrapper. The job is to introduce `ExternalTransport` above `Timer`, promote NINJAM interval phase to authoritative connected transport, keep disconnected transport free-running, stage mid-cycle alignment until the next remote wrap, then restore every looptake and loop from stored master-loop-relative anchors without big zero-based jumps. `ExternalTransport` should own runtime-only connected-sync state, expose continuous authoritative phase while connected, and defer destructive re-anchoring to wrap boundaries. `LoopTake` remains the musical anchor, and loops inside one take must share one length.