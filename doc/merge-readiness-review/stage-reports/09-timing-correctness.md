# Stage 09 — Timing and remote-join correctness

## Assignment

- Stage: 09 — Timing and remote-join correctness.
- Primary ownership: behavioural invariants across local playback, empty and populated joins, accepted tempo changes, late observations, reconnect, departure, and `ContinuousSync`, `BlockSync`, and `NoSync`; authority and coordinate conversion; preservation of unequal loop lengths and intentional offsets.
- Explicit exclusions: arithmetic edge mechanics, overflow, sign and rounding proofs belong to Stage 11; generic state-transition completeness and recovery belong to Stage 10; cross-thread publication proof belongs to Stage 7; callback cost belongs to Stage 8; source cleanup and naming are not authorized in Phase 2.
- Required handoff: transition narrative plus expected and observed invariants, concrete candidate findings, and prerequisite regression evidence.
- Status: read-only investigation. Only this report was created. No source, test, build, project, or other review artifact was edited.

## Coverage

### Inputs consumed

- `AGENTS.md`.
- `merge-readiness-plan.md`, `phase-2-runtime-safety-and-correctness.md`, `decisions.md`, `00-scope-and-inventory.md`, `phase-packets/phase-1.md`, `findings.md`, `verification-matrix.md`, `phase-1-baseline-and-structure.md`, and `cleanup-backlog.md`.
- `doc/loop-alignment-and-ninjam-sync.md` and `doc/realtime-audio.md`.
- The Phase 1 human decisions, especially the protected timing distinctions, the requirement for one or two high-quality prerequisite tests before major refactors, the single-authority concern behind F-005/F-006/F-009, and the instruction to keep Scene/Station/LoopTake slim.

### Implementation traced

- Remote observation, conversion, tracking, coordinator policy, command materialization and publication: `JammaLib/src/ninjam/NinjamTiming.h`, `NinjamTimingTracker.{h,cpp}`, `NinjamTimingCoordinator.{h,cpp}`, `NinjamAudioTimingCommand.h`, and `JammaLib/src/engine/Scene.cpp`.
- Audio-boundary application and common-map lifecycle: `JammaLib/src/audio/AudioHost.{h,cpp}`, `JammaLib/src/utils/Timer.{h,cpp}`, and `JammaLib/src/ninjam/NinjamLoopAlignment.h`.
- Per-station/per-take/per-loop application, anchors, MIDI cursor and automation correction: `JammaLib/src/engine/Station.{h,cpp}`, `LoopTake.{h,cpp}`, and the relevant `Loop` APIs.
- Departure/reconnect entry points and connection lifetime context: `JammaLib/src/engine/Scene.cpp` and `JammaLib/src/ninjam/NinjamSession.{h,cpp}`.
- Existing regression evidence: `test/JammaLib_Tests/src/ninjam/NinjamAudioTimingCommand_Tests.cpp`, `NinjamTiming_Tests.cpp`, `NinjamTimingTracker_Tests.cpp`, `NinjamTimingCoordinator_Tests.cpp`, `NinjamTimingIntegration_Tests.cpp`, and `test/JammaLib_Tests/src/engine/LoopTakeTiming_Tests.cpp`.
- History for the affected flow, including `d6fdb88` (unified audio-boundary command), `6abf7c7` (join lifecycle/generations), `b90f398` (reconnect invalidation), `a3f72b4` (three follow modes), `b41b5a8` (boundary projection/map), `1d665d2`/`0d90bac`/`bef7943`/`e72f3b0` (source-coordinate map and final alignment changes).

### Commands and queries used

- `Get-Content -Raw` for all governing artifacts and timing/real-time design documents.
- Numbered `Get-Content` slices for the implementation and test paths above.
- `rg -n` for timing commands, maps, anchors, follow policies, invalidation, generations, joins, reconnect/departure, tests, and logging/hot-path handoffs.
- `git log --oneline -- <timing paths>` and focused `git blame -L` for command, coordinator, Scene, AudioHost, Timer, and LoopTake history.
- `git status --short` to distinguish review work from source state.
- No build, native test, or runtime/manual scenario was executed in this read-only stage.

### Deliberate exclusions

- I did not re-prove mailbox atomics or non-atomic ownership; Stage 7 owns that question. This report assesses whether a coherently published value has correct behavioural semantics.
- I did not assess the cost of per-block map restoration or snapshot traversal; Stage 8 owns it.
- I did not develop the 32-bit `Timer::AbsoluteSamplePos` long-duration overflow concern found during reconnaissance; it is handed to Stage 11.
- I did not assess malformed transition recovery, request timeout completeness, or teardown state exhaustively; Stage 10 owns those topics.
- HUD, VST3 parity, window persistence, general MIDI routing, and tooling remain retained but out of cleanup scope under `decisions.md`.

## System understanding

### End-to-end timing flow

The audio callback samples the remote NINJAM interval, converts source-rate interval length and phase into device samples, and attaches two deliberately distinct anchors: Timer-absolute local position and monotonic device audio position (`JammaLib/src/audio/AudioHost.cpp:437`-`:453`; `JammaLib/src/ninjam/NinjamTiming.h:140`-`:174`). The job-thread coordinator validates session intent, tracks generation/wrap/join state, chooses follow policy, and emits either replacement geometry, a signed join/discipline correction, or invalidation (`NinjamTimingCoordinator.cpp:68`-`:266`, `:316`-`:380`). Scene currently materializes that update into an immutable audio command (`Scene.cpp:403`-`:481`).

At the next callback boundary AudioHost consumes at most one command before playback (`AudioHost.cpp:162`-`:165`). A replacement projects observed remote phase from its device-audio observation anchor to the callback boundary, replaces Timer geometry, converts remote master phase to the retained local-source ruler, and fans one source correction through local stations (`AudioHost.cpp:187`-`:320`). A live map then restores every local take each block from the common mapped source coordinate; every loop and MIDI cursor wraps that coordinate by its own length and anchor (`AudioHost.cpp:456`-`:459`; `LoopTake.cpp:602`-`:678`). `ContinuousSync` and `BlockSync` use the same map/anchor algorithm; only expected correction size/policy differs. `NoSync` clears the map and scene anchors, resets external musical transport, and leaves local playback advancing at device rate (`AudioHost.cpp:174`-`:186`).

This architecture correctly preserves the protected distinctions when commands arrive in the assumed order: remote master phase is not a loop cursor; scene time stays monotonic; the map owns common elapsed source progress; each entity owns its phase/anchor; MIDI automation correction moves opposite the event-cursor translation (`LoopTake.cpp:681`-`:688`). The current failures are not reasons to collapse those concepts. They arise because the command stream does not preserve required transitions and because the join anchor is captured in mismatched observation/live time.

### Transition narrative and invariants

| Transition | Expected invariant | Observed implementation/evidence | Assessment |
| --- | --- | --- | --- |
| Local playback / `NoSync` | No remote authority moves Timer or loop cursors; local transport offsets remain independently usable. | `NoSync` clears map/anchors (`AudioHost.cpp:174`-`:186`); local offset is consumed separately and fanned out (`:332`-`:364`). `LoopTakeTiming_Tests.cpp:405`-`:436` covers offset independence and disconnected advance. | Correct in isolation. |
| Connect/reconnect boundary | Before accepting a new session, stale map/anchors and every consumer's prior-session generation gate are invalidated exactly once. A later session may start its own generation sequence without being rejected. | Scene publishes `Invalidate` (`Scene.cpp:254`-`:272`); coordinator resets its counter to zero (`NinjamTimingCoordinator.cpp:10`-`:29`). AudioHost clears map/anchors but skips take command application for an invalidation (`AudioHost.cpp:174`-`:186`, `:298`-`:320`), so `LoopTake::_audioTimingGeneration` remains from the prior session (`LoopTake.cpp:518`-`:533`, `:590`-`:600`). | Violated; S09-02. |
| Empty remote join | Remote geometry may seed Timer without inventing a local loop cursor; later local content begins from remote-disciplined transport. | With no local content, generation-change proposal is auto-accepted (`NinjamTimingCoordinator.cpp:136`-`:169`); replacement can set Timer while old local source length is zero, leaving the source map inactive (`AudioHost.cpp:224`-`:247`, `:303`-`:319`). `NinjamTimingCoordinator_Tests.cpp:87`-`:97` covers auto-accept; integration covers replacement fan-out but not a production empty-join-to-first-record scenario. | Plausible and consistent; manual/runtime coverage gap remains. |
| Populated same-tempo join | Capture local and remote phase at the same observation instant, then apply one join correction at a remote wrap without changing relative offsets. | Tracker freezes a join delta (`NinjamTimingTracker.cpp:88`-`:98`) and emits it at wrap (`:74`-`:85`). Coordinator passes the live Timer offset with the stale remote observation rather than projecting local phase to the observation anchor (`NinjamTimingCoordinator.cpp:136`-`:146`), then explicitly bypasses its late-observation projection for join events (`:219`-`:240`). | Violated under delayed initial observation; S09-03. |
| Accepted different-tempo join/replacement | Replacement geometry and the correction/map derived from the old local source ruler must be applied before any dependent discipline command. Unequal lengths and intentional offsets receive common mapped elapsed time, not a common cursor. | Replacement mapping follows that invariant when consumed (`AudioHost.cpp:224`-`:320`); unequal length/offset tests exist (`LoopTakeTiming_Tests.cpp:283`-`:404`; `NinjamTimingIntegration_Tests.cpp:374`-`:422`). However the mailbox can replace the required replacement with a later dependent correction before the callback (`NinjamAudioTimingCommand.h:53`-`:119`). | Violated by command coalescing; S09-01. |
| `ContinuousSync` | Small accepted corrections use the active common map, preserve entity offsets, and rebase once at the boundary. | Policy selection is within one BPM (`NinjamTimingCoordinator.cpp:372`-`:380`); AudioHost restores before correction and rebases afterward (`AudioHost.cpp:201`-`:215`, `:270`-`:319`). Tests cover classification and map preservation. | Correct if prerequisite command history was consumed. |
| `BlockSync` | Material tempo change uses the same anchor/map mechanics but permits a larger replacement correction; no entity is forced to master phase. | Same AudioHost/map path is used, and `LoopTakeTiming_Tests.cpp:289`-`:404` covers unequal lengths/origins. | Correct if replacement is consumed; S09-01 can prevent it. |
| `Stay local` | Rejecting a proposal invalidates pending remote authority, clears map/anchors, and makes playback free-running without moving phase. | Coordinator emits `NoSync` invalidation (`NinjamTimingCoordinator.cpp:335`-`:354`); Scene materializes it (`Scene.cpp:460`-`:481`); AudioHost clears map/anchors and does not apply movement (`AudioHost.cpp:174`-`:186`, `:298`-`:320`). | Correct in isolation; same non-substitutable coalescing risk if immediately followed by a new authority command. |
| Disconnect/departure | A pending correction cannot move playback after departure; map/anchors and generation gates are invalidated before another independent session. | Scene publishes invalidation before network disconnect (`Scene.cpp:290`-`:310`). Timer invalidation would reset its gate (`Timer.cpp:205`-`:216`), but it can be overwritten and LoopTake gate is not reset through AudioHost. | Violated for subsequent reconnect and coalesced departure; S09-01/S09-02. |
| Late replacement observation | Project remote phase from device observation sample to the consuming callback, then map into the local source ruler. | `ResolveBoundaryTimingReplacement` receives `PhaseObservationSample` and callback `blockStartSample` (`AudioHost.cpp:224`-`:244`; `NinjamTiming.h:100`-`:123`). Tests cover delayed replacement (`NinjamTimingIntegration_Tests.cpp:279`-`:324`). | Correct for ordinary, non-wrapped anchors; arithmetic/long-duration boundaries handed to Stage 11. |
| Late wrap/discipline observation | Compare remote and local phase at the observation instant, independent of job delay. | Coordinator projects live local offset back through `LocalBlockStartSample` (`NinjamTimingCoordinator.cpp:219`-`:240`). `NinjamTimingIntegration_Tests.cpp:553`-`:605` covers delayed wrap processing. | Correct for discipline. The test does not delay the initial observation used to freeze a join. |
| Stale/equal command | A generation already applied moves neither Timer nor takes twice. | Timer and LoopTake independently reject zero/stale generations (`Timer.cpp:205`-`:237`; `LoopTake.cpp:518`-`:535`); focused tests cover this. | Correct within a session; conflicts with coordinator resetting generations across sessions. |

## Candidate findings

### S09-01 — Latest-wins command publication drops non-substitutable transport transitions

- Stage / reviewer: S09 timing and remote-join correctness.
- Scope reviewed / exclusions: semantic ordering from Scene publication through AudioHost consumption; atomic coherence is Stage 7 and queue implementation choice is deferred.
- Severity: merge blocker.
- Evidence: the command mailbox intentionally retains only the latest publication (`JammaLib/src/ninjam/NinjamAudioTimingCommand.h:53`-`:119`; commit `d6fdb88`). Its only coalescing test publishes two complete replacements and asserts that the latter wins (`test/JammaLib_Tests/src/ninjam/NinjamAudioTimingCommand_Tests.cpp:55`-`:68`). The actual command language also contains non-substitutable transitions: connect/disconnect invalidation (`JammaLib/src/engine/Scene.cpp:254`-`:305`) followed by a new-session replacement, and replacement geometry followed by phase discipline (`Scene.cpp:403`-`:481`). AudioHost consumes only one value (`AudioHost.cpp:162`-`:165`). If `Invalidate -> Replace` is published between callbacks, the gate reset/anchor invalidation is lost. If `Replace -> PhaseDiscipline` is published between callbacks, Timer can apply the correction against old geometry and advance its generation gate while the required replacement disappears (`Timer.cpp:205`-`:237`). Coordinator observations can run again before the callback and issue later commands because command generation is job-thread state, not an audio acknowledgement (`NinjamTimingCoordinator.cpp:68`-`:266`).
- Why it matters: "latest" is safe only when the newest value fully subsumes every earlier value. A delta command does not contain replacement geometry or session invalidation, so the mailbox can create mixed Timer/map/take authority and lose an accepted tempo change. This directly undermines F-005, F-006, and F-009.
- Recommended disposition: before boundary refactors, add one production-faithful regression that publishes `Invalidate -> Replace` and `Replace -> PhaseDiscipline` before one audio boundary and proves the final Timer, map, audio loops, MIDI cursor, and generation gates are coherent. Then make every publication self-sufficient/latest-substitutable (a complete accepted timing state plus optional boundary correction) or use a bounded ordered audio command mechanism with explicit overflow/coalescing rules. Prefer completing the command in the NINJAM integration owner per F-009; do not make Scene own another state machine.
- Protected timing concepts affected: remote authority, follow policy, Timer geometry, sync map, source/scene anchors, mapped elapsed time, and per-loop phase remain distinct.
- Verification: prerequisite regression above; unequal local loop lengths and intentional offsets; both `ContinuousSync` and `BlockSync`; invalidation followed by reconnect; command receipt confirms one coherent applied state. Manual trace should show accepted replacement cannot disappear when job cadence exceeds callback cadence.
- Human decision: pending.

### S09-02 — AudioHost invalidation does not reset LoopTake's audio generation gate

- Stage / reviewer: S09 timing and remote-join correctness.
- Scope reviewed / exclusions: reconnect/departure behaviour at the audio boundary; generic transition ownership is Stage 10.
- Severity: merge blocker.
- Evidence: each coordinator connect resets `_commandGeneration` to zero (`JammaLib/src/ninjam/NinjamTimingCoordinator.cpp:10`-`:29`, introduced at `6abf7c7`). Scene publishes an invalidation before reconnect (`JammaLib/src/engine/Scene.cpp:254`-`:272`, `b90f398`). AudioHost recognizes invalidation as `disablesSync`, clears anchors/map, then executes the station/take command fan-out only under `if (!disablesSync)` (`JammaLib/src/audio/AudioHost.cpp:174`-`:186`, `:281`-`:320`). `LoopTake::ApplyTimingCommand` contains the intended invalidation reset at `JammaLib/src/engine/LoopTake.cpp:518`-`:531`, but that branch is unreachable through this AudioHost path. `LoopTake::InvalidateSceneAnchors` clears only anchors/map (`:590`-`:600`), leaving `_audioTimingGeneration` unchanged. A prior session at generation N therefore rejects reconnect generations `1..N`.
- Test mismatch: `NinjamTimingIntegration_Tests.cpp:475`-`:520` claims to mirror AudioHost but its harness explicitly applies invalidation to every model take at `:124`-`:164`, unlike production. It also reconnects at generation 6 after generation 5 (`:509`-`:519`), whereas production coordinator restarts at 1. `LoopTakeTiming_Tests.cpp:426`-`:449` exercises the older queued-correction API, not the direct AudioHost command path.
- Why it matters: after any sufficiently active first session, reconnect can update Timer/map while local audio and MIDI takes silently reject corrections, destroying coherent fan-out and local relative alignment. Even if S09-01 preserves the invalidation publication, this independent skip still breaks reconnect.
- Recommended disposition: add the high-quality two-session regression before F-005/F-006/F-009 work: drive session 1 above generation 1, consume invalidation through the production-faithful AudioHost path, reconnect with coordinator-generated generation 1, and assert Timer plus real audio/MIDI takes accept it while unequal lengths/offsets remain invariant. Then make invalidation reset every consumer gate at the same boundary, or replace per-session numeric comparison with an explicit session epoch plus within-session generation. Do not rely on a monotonically higher test-only reconnect generation unless production adopts globally monotonic generation authority.
- Protected timing concepts affected: remote session authority/generation, `NoSync` invalidation, source/scene anchors, Timer geometry, and per-loop phase remain separate.
- Verification: two-session test above; disconnect free-run; stale command from session 1 rejected after session 2 begins; reconnect replacement and first correction each apply exactly once; map/anchors freshly captured without changing intentional offsets.
- Human decision: pending.

### S09-03 — Initial join delta compares a stale remote observation with live local phase

- Stage / reviewer: S09 timing and remote-join correctness.
- Scope reviewed / exclusions: observation-time behavioural invariant; numeric wrap/overflow mechanics belong to Stage 11.
- Severity: must fix before merge.
- Evidence: the first valid generation observation carries the Timer-absolute callback anchor as `NinjamTiming::LocalBlockStartSample` (`JammaLib/src/audio/AudioHost.cpp:441`-`:453`; `JammaLib/src/ninjam/NinjamTiming.h:32`-`:36`). Coordinator copies it into `NinjamTimingObservation::LocalSample` (`NinjamTimingCoordinator.cpp:99`-`:105`) but, when starting join alignment, passes the job-time `clock.SampOffset()` directly to the tracker (`:136`-`:146`). Tracker freezes a delta against `_lastPositionSamps`, which is the earlier remote observation (`NinjamTimingTracker.cpp:88`-`:98`). At the later wrap, coordinator deliberately uses the frozen join delta and bypasses its otherwise-correct projection of local phase back to `observation.LocalSample` (`NinjamTimingCoordinator.cpp:219`-`:240`). Job delay between callback publication and processing is therefore embedded in the join correction.
- Test gap: `NinjamTimingIntegration_Tests.cpp:553`-`:605` delays processing of the later wrap, after the join delta was already frozen; equality can hold without exercising delayed initial join capture. `NinjamTimingCoordinator_Tests.cpp:278`-`:323` also processes the initial generation synchronously. Neither compares prompt vs delayed processing of the first observation.
- Why it matters: populated same-tempo joins are expected to retain local phase until one deliberate remote-boundary alignment. With a delayed job thread, the correction is wrong by approximately the scheduling delay in samples, causing an audible/visible offset and making correctness scheduler-dependent. This is precisely the late-observation invariant the branch intends to remove.
- Recommended disposition: first add a regression with identical callback-time local/remote anchors and two different delays before the initial generation observation is processed; both must emit the same eventual join delta. Then project local phase to the initial observation anchor before `BeginJoinAlignment`, or store both anchors and derive the join correction at the audio boundary. Keep Timer-absolute and device-audio coordinates explicitly distinct per F-015.
- Protected timing concepts affected: remote master phase, local Timer phase, observation anchors, join policy, and scene/source mapping remain separate.
- Verification: delayed initial observation at zero and multiple block delays; positive/negative and half-interval join deltas; subsequent wrap emits once; unequal loop lengths and intentional offsets receive the same corrected mapped elapsed amount.
- Human decision: pending.

## Handoffs

- **Stage 7 — thread safety:** decide whether a bounded ordered command mechanism or a complete-state latest mailbox can retain coherent publication without introducing a callback lock. S09-01 is semantic even if the current seqlock is race-free.
- **Stage 8 — hot paths:** any S09-01 fix must remain bounded/allocation-free; assess complete-command copy cost versus a fixed-capacity queue. Do not move retry/blocking behaviour into the callback.
- **Stage 10 — state machine/failure paths:** own the formal session epoch/generation transition table, including disconnect/connect races, invalidation acknowledgement, replacement-before-first-callback, and command overflow policy. S09 supplies the behavioural failure and required invariant.
- **Stage 11 — numerics:** prove Timer-absolute/device counter width and wrap behaviour. `Timer::AbsoluteSamplePos` returns Windows 32-bit `unsigned long` at `JammaLib/src/utils/Timer.h:72`-`:82`, then AudioHost widens the already-truncated result at `AudioHost.cpp:445`-`:452`; assess long-session observation projection separately from S09-03.
- **Stage 13 / Phase 4:** F-005/F-006/F-009 consolidation should use the two prerequisite regression tests from S09-01/S09-02 before moving authority. Scene should forward/present, not acquire more command-history responsibility.
- **Stage 14:** existing tests misdescribe their fidelity at `NinjamTimingIntegration_Tests.cpp:124`-`:164`; update the harness or replace it with production-path coverage so it cannot reset take generations differently from AudioHost.
- **Stage 17:** retained command receipts should expose session/epoch and complete applied authority if that is the selected fix, enabling manual verification without callback logging.

## Uncertainties

- `AudioHost::_OnAudio` is private and the existing integration harness models only a subset of its behaviour. The exact minimal way to exercise the production path may require a narrowly exposed existing-owner helper or test fixture; this report does not prescribe a new production class, respecting the human decision against class proliferation.
- Empty-join-to-first-local-record behaviour is consistent by inspection, but no focused production-level runtime test proves the first later audio/MIDI take captures remote-disciplined geometry without an active old-source map. Keep it in the verification matrix even though it is not an evidence-backed defect.
- A globally monotonic generation can solve cross-session numeric reuse but does not alone solve S09-01's `Replace -> PhaseDiscipline` dependency. Conversely, ordered delivery alone does not solve S09-02 unless invalidation reaches take gates.
- The coordinator can emit updates faster than callbacks in principle; no measured cadence was needed to establish the missing ordering guarantee, but runtime stress should quantify how readily it occurs.

## Conclusion

The source-coordinate map and per-entity anchor algorithm are behaviourally sound under an ordered, fully applied command history and have good helper/take-level evidence for unequal loop lengths, intentional offsets, MIDI/audio coherence, map restore/rebase, and both syncing policies. `NoSync` also has the right intended map/anchor semantics.

Phase 2 cannot accept timing/remote-join correctness yet. Three evidence-backed findings remain: the latest-wins mailbox drops required non-substitutable transitions (S09-01), production invalidation never resets the per-take direct-command generation gate (S09-02), and initial join alignment is scheduler-dependent because it mixes stale remote phase with live local phase (S09-03). S09-01 and S09-02 are merge blockers; S09-03 is a must-fix. The first implementation work should be the two prerequisite regression tests described above, after which the authority/generation fix can be made without collapsing any protected timing concepts.
