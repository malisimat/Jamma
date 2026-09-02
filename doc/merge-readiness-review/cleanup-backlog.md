# Cleanup backlog

> **Status: approved for execution.** The human approved this complete Phase 4 cleanup-batch gate at commit `2e770b743d9f2466b2edafff5c92faf139d93108`, including B001–B017, their dependency order, prerequisites, owners, prohibited collateral, rollback points, verification requirements, independent reviews, protected timing invariants, and recorded no-batch residual risks. This authorizes the one remaining execution pass to run B001–B017 sequentially. Any scope expansion, failed prerequisite that cannot be corrected within its batch, or irreconcilable batch review returns to the human gate.

## Controlling invariants and execution contract

- Cleanup scope is remote timing/sync only. Do not edit retained HUD, VST implementation, window/tooling, unrelated MIDI/resources, upstream NJClient, or F-049/F-050 `.jam` password/work-directory behavior. F-020 is an explicit no-change resource residual; F-045 is an accepted zero-default downgrade residual.
- Preserve the glossary. Common mapped elapsed time is never a shared cursor. Every `M`, `2M`, `3M`, or non-divisor audio/MIDI entity retains its own anchor, length, wrapped phase, MIDI event cursor, and automation origin. `NoSync`/epoch invalidation moves no cursor.
- No callback allocation, lock, wait, blocking I/O, formatting, final destruction, or unbounded traversal. Add no timing/logger/mailbox class, generic hierarchy walker, anonymous namespace, compatibility shim for rejected command/sentinel behavior, remote BPI fallback, or persisted live epoch/authority/map/anchor.
- A single implementation owner takes one batch at a time. The default is for its one or two named tests to exist in a prerequisite commit, be registered in `JammaLib_Tests.vcxproj`, build incrementally, and pass before source movement. When a truthful named test necessarily exposes behavior that does not exist until the already-approved batch implementation, characterization-first sequencing is allowed: commit and build the test, record its exact expected pre-fix failure, then implement only inside that batch's existing boundary. The test and all focused verification must be green before independent approval or any dependent batch begins. Test commits remain part of the batch and precede production commits; this ordering exception never expands scope, ownership, collateral, invariants, rollback, or final evidence.
- Before every build/test, reread `.vscode/tasks.json` and `doc/build.md`. Use the exact current-machine executable/arguments through `.github/skills/builder/invoke-msbuild.ps1` as designed in `stage-reports/21-merge-hygiene.md`; never invoke MSBuild directly. Record command, filter, commit, timestamp, exit code, pass/fail/skip count in `verification-matrix.md` and `batch-reviews/B###.md`.
- Each batch ends with a focused incremental `JammaLib_Tests` build/run, static audit, canonical artifact update, and independent batch review. A failure reopens that batch and all dependents. Rollback means reverting only that batch's commits to the named pre-batch commit; never use a destructive worktree reset.

## Dependency summary

| Batch | Objective | Findings | Depends on | Required passing prerequisite(s) |
| --- | --- | --- | --- | --- |
| B001 | Guard-scoped remote stereo consumption | F-033, F-041 | none | P9 |
| B002 | Reject invalid remote tempo before conversion/egress | F-032 | none | P12 |
| B003 | Bound local seed-policy conversion | F-048 | none | P13 |
| B004 | Publish coherent remote/local observations and correct numeric domains | F-021, F-023, F-026, F-029, F-030, F-031, F-039, F-040 | B002 | P5, P6 |
| B005 | Model physical lifecycle, loss, retry, and invalid/deadline recovery | F-027, F-028, F-041 | B004 | P7, P8 |
| B006+B007 | Atomically apply one complete desired state, reset every epoch/generation gate, and move production to the NINJAM integration owner | F-009, F-024, F-025, F-038, F-039, F-047 (epoch/version only) | B004, B005 | P1, P2, P3 |
| B008 | Move the empty-scene reset edge off the callback | F-022 | B005, B006+B007 | P8 (already passing from B005) |
| B009 | Neutralize engine timing APIs and consolidate the common map | F-005, F-006, F-018, F-038 | B004, B006+B007, B008 | P3, P4 |
| B010 | Consolidate only equivalent direct LoopTake cursor shifts | F-035 | B009 | two named existing LoopTake tests |
| B011 | Preserve signed local offsets for long loops | F-044, F-045 | B010 | P11 |
| B012 | Settle timing value/header/mailbox ownership | F-007, F-008, F-012 | B004, B006+B007, B009 | one named existing local-offset test |
| B013 | Centralize proposal identity and require full remote BPI | F-036, F-046 | B006+B007, B012 | two named coordinator/local-inference tests |
| B014 | Replace callback hierarchy logs with bounded correlated diagnostics | F-019, F-034, F-047 (diagnostics only) | B006+B007 | P10 |
| B015 | Apply bounded timing vocabulary and casing changes | F-010, F-011, F-013, F-014, F-015, F-016, F-017 | B009, B012–B014 | two named focused tests |
| B016 | Make timing documentation truthful | F-042, F-043, F-045 | B011, B013–B015 | static statement/source audit |
| B017 | Delete the obsolete uncompiled timing test | F-037 | B006+B007, B009 | two registered replacement tests |

## Proposed batches

### B001 — Guard-scoped remote stereo consumption

- **Owner / objective:** one NINJAM lifetime owner; keep connection scratch buffers valid through synchronous callback consumption.
- **Owned files:** `JammaLib/src/ninjam/NinjamSession.h/.cpp`, `NinjamController.h/.cpp`, `NinjamConnection.h/.cpp` only if the existing scoped-use API requires it; `JammaLib/src/audio/AudioHost.cpp`; new/updated NINJAM native test and test project/filter entries.
- **Prohibited collateral:** NJClient edits; authority/epoch/map changes; raw-buffer public test API; callback allocation/lock/wait/copy with dynamic storage.
- **Prerequisite:** add P9 `NinjamSessionAudio.StopCannotInvalidateBorrowDuringSynchronousConsume`; it must pass alone before production edits.
- **Rollback:** pre-B001 gate commit; revert the scoped API chain and P9 together if no safe synchronous ownership is achieved.
- **Verification:** P9; focused start/stop/reconnect stress; incremental tests; callback/lifetime audit; later ASan/page heap/Application Verifier evidence. Independent review writes `batch-reviews/B001.md`.
- **Execution result (2026-08-31):** prerequisite `9b46803ea4bf37f0bb7b3488128da02f1ef4546b`; scoped-ingestion implementation `996bda2a49de9857f31eb9d9b0f3478f3e28f6da`; initial independent rejection `b9aed357f8c9cdd2b8149da5dfa9e50efdb8a259`; corrected P9 overlap coverage `73af93c2229f0073c59a1d2bfa2068daa7e6e74d`; guard-protocol correction `5fd1b8062a87e444ccbde35dbccf87bf7f7b2ab8`; superseding independent approval `620447c27999f28e24d71ad183320c4b925319ea`. Wrapped incremental Debug x64 JammaLib and JammaLib_Tests builds passed. Corrected P9 passed alone and across 640 repeated entry-overlap epochs; both focused `StationRemote` ingestion tests passed 2/2. Static scope, C++ lifetime ordering, and callback-safety audits passed. No live server was available for genuine reconnect stress, and ASan/page heap/Application Verifier remain deferred to Stage 21. Status: implemented, verified, and independently approved.

### B002 — Reject invalid remote tempo before conversion and egress

- **Owner / objective:** existing NINJAM timing/network owner; reject non-finite or implausible BPM/BPI before arithmetic, formatting, or send.
- **Owned files:** `JammaLib/src/ninjam/NinjamTiming.h/.cpp` if split, `NinjamConnection.cpp`; focused NINJAM tests and project entries only if a new file is necessary.
- **Prohibited collateral:** new validation class/state machine; upstream NJClient; connection lifecycle, persistence, or UI behavior.
- **Prerequisite:** P12 `NinjamTimingInput.RejectsNonFiniteTempoWithoutEgress` passes before production edits.
- **Rollback:** revert the two Jamma-owned validation boundaries and P12; valid endpoint behavior must return together.
- **Verification:** P12 plus existing `NinjamTiming.SharedValidityAcceptsPlausibleTiming`; audit that rejection sends nothing and changes no authority. Review: `B002.md`.
- **Execution result (2026-08-31):** initial prerequisite `c9bec2fa0616de5fedda801959fc878a7e1d7370`; implementation `e0d60f2d0141b42a7e3ade5cb161ff264b1200a6`; independent rejection `bd21e1def077d358f9dd05fe2cd55400b03057d6`; corrected P12 boundary coverage `93594c5afcb9b3deefa9c0acc0b5de9a2e161924`; superseding approval `a7bfff8968312d628eaa7a82427c7b0bf5384f60`. Wrapped incremental Debug x64 native-test build passed and the corrected five-test filter passed 5/5. P12 now exercises helper-zero, endpoint, saturation, formatting, and the existing disconnected request boundary; static audit proves invalid input returns before arithmetic/conversion and before formatting/string/lock/send/logging. Dynamic send counting was not added because it would require a prohibited seam. Status: implemented, verified, and independently approved.

### B003 — Bound local seed-policy conversion

- **Owner / objective:** existing UserConfig/Quantiser owner; validate or clamp policy and use checked/saturating conversion before Windows 32-bit casts.
- **Owned files:** `JammaLib/src/io/UserConfig.cpp`, `JammaLib/src/engine/Quantiser.h/.cpp`, `test/JammaLib_Tests/src/io/UserConfig_Tests.cpp`, `src/engine/Quantisation_Tests.cpp`.
- **Prohibited collateral:** remote authority changes, new class, `.jam` schema work, broad config hardening.
- **Prerequisite:** P13 `Quantisation.SeedPolicyBoundsConversionBeforeCast` passes before production edits.
- **Rollback:** revert the seed-policy validation/conversion and P13 as one unit.
- **Verification:** P13 plus `Quantisation.TimingFromSeedAndMasterDerivesBpmAndBpi`; incremental tests; finite/nonzero output and no malformed request. Review: `B003.md`.
- **Execution result (2026-08-31):** prerequisite `d1b6a7698a51078d1363032888d6ce3c67b633ca`; implementation `749ff1ca98f86a061d2cc1732eca7291548afcb7`; independent approval `8386ac8c2b873b9af3dfdd8a2934981479a0d279`. Wrapped incremental Debug x64 JammaLib and native-test builds passed. P13 plus derivation passed 2/2, all `Quantisation.*` tests passed 33/33, and related UserConfig timing tests passed 3/3. Static audit found no remaining direct target-maximum floating-to-`unsigned long` cast; the checked helper saturates before widening. No parser, schema, request, or authority collateral entered. Parser/manual extreme-config egress was not run. Status: implemented, verified, and independently approved.

### B004 — Coherent owned observations and numeric domains

- **Owner / objective:** NINJAM job/integration owns remote observation; AudioHost/Timer own coherent device and local-transport anchors; remove live NJClient getter adapters only after all consumers migrate.
- **Owned files:** `JammaLib/src/audio/AudioHost.h/.cpp`; `utils/Timer.h/.cpp`; Jamma-owned timing observation/value/mailbox files; `ninjam/NinjamConnection`, `NinjamSession`, and `NinjamController` adapters; corresponding Timer, timing, observation, and MIDI timestamp tests/project entries.
- **Prohibited collateral:** `AudioProc` changes in NJClient; desired-state/lifecycle/map refactor; dual observation owners; zero sentinel; 32-bit absolute intermediate; early phase wrap; public second apply path.
- **Prerequisites:** P5 `NinjamTimingObservationMailbox.ConcurrentReadProvesOverlapAndCoherence` and P6 `NinjamTiming.PresenceWidthAndDownsampleTailRemainDistinct` use the characterization-first exception below and must both pass before independent approval or dependent work.
**Human-approved B004 sequencing amendment (approved against `HEAD` `0aee948d3af6b4e9bde7103dcfd53d9953f4b5b7`):** B004 may add and register truthful P5/P6 in a test-only commit, build them, and record their expected pre-fix failures before production movement. Those failures are characterization evidence, not passing verification. Production work may then proceed only inside B004's already approved owned files and prohibited-collateral boundary. Both P5 and P6 and all focused B004 verification must pass, and an independent review must approve B004 before B005 or any dependent batch begins. This amendment changes sequencing only; it does not change scope, invariants, ownership, rollback, final evidence, or any no-batch decision.
- **Rollback:** keep the final pre-deletion adapter commit as the rollback point; never leave both live getters and the new owned observation active.
- **Verification:** P5/P6; delayed initial-join equivalence; Timer >`UINT32_MAX`; 96→48 tail; callback audit proves NJClient `AudioProc` is the only upstream callback call; MIDI/VST host-time smoke. Review: `B004.md`.
- **Execution result (2026-09-01):** characterization-only prerequisite `523714cf115f7d880f8c842f3c461f4b99a0b100`; explicit-presence/wide-arithmetic/coherent-local rollback point `18663f31fa5b533233447244ff006953edf85d3a`; job-owned remote publication and live-adapter deletion `88e3ecceef9d02ef858fb46f43b4c9baa6e434f9`; independent approval `74c8e528ac637bfe2738107f90b5cbc06d58d80f`. P5 passed 20/20 repeated overlap runs; P6 passed 1/1 with explicit absent/present-zero/present-nonzero, Timer crossing, and 96→48 tail cases; delayed initial join and the 134-test focused timing/Timer/NINJAM/MIDI/VST-smoke filter passed. Static audit leaves `AudioProc` as the sole NJClient call in callback-reachable `ProcessExportBlock`; timing getters exist only in job-owned `_UpdateSnapshot` behind a stable even AudioProc sequence. The extra full native-suite attempts stalled and were interrupted, so no full-suite pass is claimed here; final full-suite evidence remains Stage 21. Status: implemented, focused-verified, and independently approved.

### B005 — Session lifecycle and recovery

- **Owner / objective:** existing session/integration/coordinator owners publish availability and a fresh session epoch; loss/retry/invalid/deadline transitions produce one idempotent `NoSync` and recover.
- **Owned files:** `JammaLib/src/ninjam/NinjamSession.h/.cpp`, `NinjamConnection.h/.cpp`, `NinjamController.h/.cpp`, `NinjamTimingCoordinator.h/.cpp`, `NinjamNetworkService.h/.cpp` if already the integration owner; Scene call sites only for start/loss forwarding; focused tests/project entries.
- **Prohibited collateral:** persisted live epoch; ordered timing queue; map/engine API rewrite; raw buffer seam; broad Scene cleanup.
- **Prerequisites:** P7 `NinjamSessionTiming.PhysicalLossRetryCreatesOneNoSyncAndFreshEpoch` and P8 `NinjamTimingCoordinator.NoObservationAndInvalidTimingRecoverIdempotently` pass before production edits.
- **Rollback:** revert availability/epoch publication and lifecycle hooks to the pre-B005 commit; no half-migrated persisted/interactive start paths.
- **Verification:** P7/P8; shipped/default and saved `.jam` start; physical loss/retry/manual disconnect; exactly one `NoSync`; fresh epoch; no stale prompt/request/remote station. Review: `B005.md`.
- **Execution result (2026-09-01):** characterization-only prerequisite `86dbc2c1481960be224866b1713b2e8a69d49c19`; lifecycle/recovery implementation `0fcf7c4`; replacement-session epoch-edge correction `4e99584cb56a6fb588d2a3530fe83889b01ee0ed`; initial independent rejection `9e361482f08f53913e91a60587d15f4626692d4c`; review-gap characterization `ab5ea60`; production-routing/lifecycle-seam correction `789c388a1288ca637a50e3cba8b9e205736f7699`; superseding independent approval `ae5d862e553f787afcc72b5a5cea89fad992b5d3`. The initial pre-fix wrapped build failed only because P7/P8 referenced absent B005 contracts; the correction test commit separately exposed that cached observations could not expire through the production boundary and that P7 bypassed replacement/cleanup operations. Neither red state is called a passing prerequisite. At clean `789c388`, the wrapped Debug x64 build passed with zero warnings/errors; corrected P7/P8 plus the production-boundary deadline regression passed 3/3; the focused B005 filter passed 61/61; config regressions passed 3/3; B004 P5/P6 passed 20/20 and 1/1; and protected timing/NINJAM/MIDI/VST coverage passed 137/137. NetworkService now advances the coordinator deadline on cached-snapshot visits, coordinator freshness is keyed to B004's existing audio-block anchor, and recovery requires a new anchor. P7 now traverses persisted Controller start, forced replacement loss, pending-snapshot clearing, fresh epoch, and remote-station removal through production-used seams. Static audit finds no B006 desired-state/audio gate, B009 map, raw-buffer, callback, persistence, or broad Scene collateral. Live server/cable/manual retry, runtime `.jam` launch, sanitizers, and the full suite remain Stage 21 limitations. Status: corrected, focused-verified, and independently approved after initial rejection.

### B006+B007 — Atomic complete desired state and integration producer boundary

- **Human-approved merge amendment (approved against `HEAD` `9431c3f859261f6581c4e188988d987a4be3cca4`):** B006 and B007 execute as one atomic implementation/review batch because no complete desired state can be produced inside the former B006 file boundary without either retaining the prohibited delta adapter or moving the former B007 producer boundary. This amendment combines their owned files, prerequisites, rollback, verification, and review; it does not relax either batch's prohibited collateral, protected invariants, downstream dependencies, or no-batch residuals.
- **Owner / objective:** the existing NINJAM integration owner is the sole complete-state producer; Scene only presents prompts and forwards values; AudioHost compares one latest complete desired remote transport state with last applied state. Epoch change clears map/anchors/all gates, geometry change replaces Timer, same geometry disciplines phase, and `NoSync` moves no cursor.
- **Owned files:** Jamma-owned desired-state value/mailbox; `JammaLib/src/ninjam/NinjamTimingCoordinator.h/.cpp` and existing integration/network-service owner; `JammaLib/src/engine/Scene.h/.cpp` only for producer removal and forwarding; `JammaLib/src/audio/AudioHost.h/.cpp`; `engine/Station.h/.cpp`, `LoopTake.h/.cpp` only for epoch/gate application; production-boundary and command/timing tests/project entries; minimal epoch/version fields of F-047.
- **Prohibited collateral:** ordered/delta commands, standalone invalidation, dual old/new authority, generic dispatcher/class, UI redesign, map consolidation or broader engine changes, persisted authority, naming/docs/diagnostics formatting, cursor reduction modulo master length.
- **Prerequisites:** P1 `NinjamTimingProductionBoundary.CompleteDesiredStateSupersedesFormerCommandSequences`, P2 `NinjamTimingProductionBoundary.OverlappingIntentsPublishOneCoherentDesiredVersion`, and P3 `NinjamTimingProductionBoundary.ReconnectPreservesM2M3MEntityOffsetsAcrossEpochOne` use the global passing-first/characterization-first rule and must all be green before independent approval or dependent work.
- **Rollback:** revert the complete desired type/mailbox, integration-owner producer, Scene forwarding reduction, AudioHost consumer, and epoch/gate reset together to the pre-B006+B007 commit. There must be exactly one producer before and after rollback; no compatibility shim may retain rejected command semantics.
- **Verification:** P1/P2/P3; rapid job/UI intents; one desired/applied version; prompt behavior; Scene audit shows no transport-state construction; empty/populated join; Continuous/Block/NoSync; same-state idempotence; stale/equal generations; reconnect generation 1; `M`/`2M`/`3M` audio/MIDI/automation offsets. Remove Stage 21's trailing EOF blank only in the owned `NinjamTimingIntegration_Tests.cpp` prerequisite edit. One independent review writes `batch-reviews/B006-B007.md` and must approve the complete atomic range before B008.
- **Execution result (2026-09-02):** characterization-only prerequisite `46816af3c01242039993ecc5690821fffb5e76f5`; initial atomic implementation `f1e0df15987d039a9a7162dd59c3535f6bb102eb`; audit-correction characterization `34918aa7eca15e164ecf0e5ae841e2b0468b3302`; corrected implementation `da0db2459976f2589f2085f0179acd1262944e69`. At `46816af`, the wrapped Debug x64 native-test build failed as expected at 2026-09-02T07:30:17.9096164-06:00 with 77 diagnostics rooted in the absent complete-desired-state contract, so no test executable ran; this was characterization evidence, not a passing prerequisite. Audit of `f1e0df1` found that the epoch reset also discarded an unrelated queued local correction, exposed the audio-boundary method publicly for tests, modeled P2 below the real NetworkService/Coordinator producer, and did not prove P3 with free-run advancement and a real MIDI loop. The `34918aa` correction tests built successfully, then `ExternalPhaseCorrection.QueuedLocalCorrectionSurvivesNinjamEpochReset` failed exactly as expected at 2026-09-02T09:22:45.6297886-06:00 before the correction. At clean `da0db24`, the wrapped incremental build passed at 2026-09-02T09:32:05.1310879-06:00 with 0 observed warnings and 0 errors; the four correction/P1/P2/P3 tests passed 4/4; the exact focused timing/NINJAM/MIDI/VST filter passed 99/99 at 2026-09-02T09:32:38.4652297-06:00; and protected B004/B005 regressions passed 5/5. The full native suite at 2026-09-02T09:32:57.0839198-06:00 ran 824 tests: 823 passed, the expected hardware-dependent MIDI test skipped, and 0 failed. The existing NINJAM NetworkService/Coordinator is the single serialized complete-state producer; Scene only forwards the desired state and its paired remote-grid value; AudioHost keeps desired-versus-applied version/epoch/generation state and exposes its callback seam only to the private friend test accessor. Epoch/`NoSync` clears NINJAM map, anchors, and generation gates without discarding queued local corrections or moving cursors. P2 now overlaps real producer calls and verifies coherent grid/state/version precedence; P3 advances through `NoSync` free-run with real `M`/`2M`/`3M` audio, MIDI-loop, and automation state, then proves fresh-epoch generation 1 and equal/stale-generation rejection even at newer publication versions. The old pseudo-model and the delta/standalone-invalidation command API are removed; static callback audit found no added allocation, lock, wait, logging/I/O, or unbounded traversal. Initial independent review `f181cbb` rejected only the incorrect recorded filter count and the assigned `NinjamTimingIntegration_Tests.cpp` EOF blank. Whitespace correction `19e9ca8` makes that file end in exactly one LF. Fresh clean-HEAD evidence passed: wrapped build at 2026-09-02T10:09:50.3376202-06:00 with 0 warnings/errors; P1/P2/P3 plus queued-local correction 4/4 at 10:10:21; the exact canonical filter 99/99 across 15 suites at 10:10:31; protected B004/B005 regressions 5/5 at 10:10:43; and the full suite at 10:10:55 with 824 run, 823 passed, one expected hardware MIDI skip, and 0 failures. `git diff --check master...HEAD` now reports only the pre-existing `RemotePhaseDiscipline_Tests.cpp:156` EOF blank assigned to B017; no whole-range-clean claim is made. Superseding independent approval `b3f2bebd06ea9b35daff14a29bcb4e29ef19e298` reran the corrected focused, protected, and full-suite gates and found no remaining bounded-batch blocker. Live server/prompt/loss/reconnect scenarios, race/sanitizer/profiler tooling, Release/final solution builds, and final Stage 21 audits are not claimed. Status: corrected after an evidence-only rejection, focused/full-suite verified, and independently approved.

### B008 — Empty-scene ownership edge

- **Owner / objective:** Scene/job owner handles empty/non-empty cleanup; callback publishes/consumes only bounded immutable state.
- **Owned files:** `JammaLib/src/engine/Scene.h/.cpp`, `audio/AudioHost.h/.cpp`, existing station snapshot/edge value; focused Scene/timing tests.
- **Prohibited collateral:** raw `_stations` callback traversal, callback Quantiser mutation/destruction, generic Scene slimming, diagnostic cleanup.
- **Prerequisite:** B005's passing P8 is re-run at B008 head before source edits.
- **Rollback:** revert empty-edge publication and job handler together.
- **Verification:** connected-empty preserves timing; disconnected final-take removal clears once; retry/reconnect; callback audit has no raw container, lock, allocation, or destruction. Review: `B008.md`.
- **Execution result (2026-09-02):** characterization-only prerequisite `b379f26787218dd5de8620c88cc233e27254f4ef`; initial implementation `bcfa055a68c6bcc2d3c1d366fa05f937e0ce25ca`; independent rejection `50410a9`; correction `6e908ca0e67908652f42d6bda33dda6fa0b5929a`; superseding independent approval `e6cdd6ffcc28b8926c07808cc2dbf60e122d3050`. Before either B008 commit, P8 passed 1/1 with exit 0 at clean `cbc0297`, 2026-09-02T10:25:28.2514045-06:00. The pre-fix wrapped incremental Debug x64 native-test build then passed, followed at 10:34:38 by the two amended/new `SceneReset` assertions failing 0/2 as expected: callback-side final-take removal still reset immediately, and connected-empty job handling cleared accepted timing. This was characterization evidence, not a passing prerequisite. At clean `bcfa055`, the wrapped build and focused contract were green, but review B008-R1 found that the remote-snapshot branch recomputed local content through `Station::NumTakes()`, bypassing the published take snapshot and contradicting the recorded owner. Correction `6e908ca` reuses the one `OnJobTick` `localTiming`/`hasLocalContent` pair, derived with `GetLoopTakeSnapshot()`, for both `ObserveTiming` and `TickTiming`; `rg` finds no `NumTakes(` in `Scene.cpp`. The correction changes only `Scene.h/.cpp` and adds no expanded Controller Pump test seam. After required task/build rereads, the wrapped build passed at 10:56:38.3747648; P8 passed 1/1 at 10:56:51.4268663; the two edge tests passed 40/40 over 20 repetitions at 10:57:03.5396633; `SceneReset.*` passed 4/4 at 10:57:14.7455266; `Trigger.*:SceneReset.*` passed 21/21 at 10:57:31.3619536; and the exact canonical filter passed 103/103 across 16 suites at 10:57:46.3140621. The superseding reviewer independently repeated the clean wrapped build at `6f1e7a0`, P8, 40/40 edge repetitions, 4/4 SceneReset, 21/21 Trigger plus SceneReset, and the exact 103/103 canonical filter, all green, then found no remaining bounded blocker. `Scene::OnTick` no longer counts takes or clears timing; connected-empty timing is preserved and each disconnected-empty clear edge is consumed once on the job owner. The pre-existing callback-reachable deferred-trigger snapshot allocation/vector mutation remains outside B008, so no whole-callback allocation-free claim is made. Live connect/loss/retry races and race/sanitizer/profiler tooling remain Stage 21 limitations. Status: corrected, focused-verified, and independently approved after initial rejection.

### B009 — Neutral engine boundary and one common map

- **Owner / objective:** AudioHost interprets follow policy and owns one common mapped-source calculation; Station/LoopTake receive neutral operations while retaining entity anchors/lengths/modulo; delete only unread `SourcePhaseAtOrigin`.
- **Owned files:** `JammaLib/src/audio/AudioHost.h/.cpp`; `engine/Station.h/.cpp`, `LoopTake.h/.cpp`, `Loop.h/.cpp` only if neutral signature requires it; `ninjam/NinjamLoopAlignment.h/.cpp`; production-boundary and LoopTake timing tests.
- **Prohibited collateral:** deleting per-entity anchors, one shared cursor, master-length modulo, anchor recapture on rebase, policy collapse, naming/docs/diagnostics.
- **Prerequisites:** P3 and P4 `NinjamTimingProductionBoundary.RestoreBeforeRebasePreservesEntityAnchors` pass before production edits.
- **Rollback:** revert the whole map-interface migration to pre-B009; mixed common-map ownership is not an allowed intermediate endpoint.
- **Verification:** P3/P4; `M`/`2M`/`3M` and non-divisor audio/MIDI lengths; intentional offsets; delayed restore-before-rebase; wraps; reconnect/`NoSync`; one calculation per block and callback benchmark. Review: `B009.md`.

### B010 — Direct LoopTake cursor-shift primitive

- **Owner / objective:** LoopTake owner extracts one private allocation-free helper for the two equivalent direct paths only.
- **Owned files:** `JammaLib/src/engine/LoopTake.h/.cpp`, `test/JammaLib_Tests/src/engine/LoopTakeTiming_Tests.cpp`.
- **Prohibited collateral:** queued correction path, generation/accounting/target-gate changes, public API, automation/event-cursor merge.
- **Prerequisites:** existing `ExternalPhaseCorrection.SharedDeltaPreservesDifferentTakeLengths` and `TransportPhaseOffset.DirectTimingCommandMovesAudioAndMidiOnce` pass at B010 head.
- **Rollback:** revert the private extraction only.
- **Verification:** the two prerequisites plus negative/zero/MIDI-only/audio-only/empty/local-offset cases; queued behavior unchanged. Review: `B010.md`.

### B011 — Signed local-offset compatibility

- **Owner / objective:** JamFile/Scene/local-offset owner retains signed/turn information long enough to apply one common correction independently modulo each entity length; missing field still defaults zero.
- **Owned files:** `JammaLib/src/io/JamFile.h/.cpp`; `engine/Scene.cpp`, `Station.h/.cpp`, `LoopTake.h/.cpp` only for local-offset transport; `JamFile_Tests.cpp` and focused LoopTake tests.
- **Prohibited collateral:** schema/version guard, F-049/F-050, normalization to `[0,1)` before entity correction, remote map/authority change.
- **Prerequisite:** P11 `JamFile.SignedTransportOffsetPreservesM2M3MEntityPhases` passes before production edits, replacing the wrong legacy-normalization expectation.
- **Rollback:** revert signed read/application and P11 together; current writer/current reader round trip remains stable.
- **Verification:** P11; `-1`, `-0.25`, zero, positive/endpoints; `M`/`2M`/`3M`/non-divisor audio/MIDI; missing zero default; join/`NoSync` independence; manual load. Review: `B011.md`.

### B012 — Timing value, mailbox, and header ownership

- **Owner / objective:** existing engine/audio/NINJAM owners move value-only local timing and local-offset publication to their rightful boundary and move non-template runtime bodies behind declarations; add no class.
- **Owned files:** `engine/Quantiser.h/.cpp` plus one small existing timing value header; `audio/AudioHost.h/.cpp`; `ninjam/NinjamAudioTimingCommand.h/.cpp`, `NinjamTiming.h/.cpp`, `NinjamTimingObservationMailbox.h/.cpp`; affected project/filter entries and tests.
- **Prohibited collateral:** behavior change, generic mailbox, new class, writer multiplicity, latest/zero semantic change, UI/VST/resource edit.
- **Prerequisite:** `TransportPhaseOffset.AbsoluteLocalOffsetIsIndependentOfNinjamGeneration` passes before file movement.
- **Rollback:** revert value/mailbox placement separately from runtime-body moves if they become two commits; each intermediate commit must build.
- **Verification:** prerequisite plus mailbox latest/zero tests, disconnected/`NoSync` local offset, include graph, retired include/symbol audit, incremental tests. Review: `B012.md`.

### B013 — Proposal identity and authoritative BPI

- **Owner / objective:** existing NINJAM value/coordinator owns proposal identity; full BPI is mandatory for remote authority; disconnected local inference stays pure and local.
- **Owned files:** `ninjam/NinjamTimingCoordinator.h/.cpp`, associated timing value; `engine/Scene.cpp` prompt comparison; `engine/Quantiser.cpp` only to retain/prove local inference; focused coordinator/quantisation tests.
- **Prohibited collateral:** partial remote BPI fallback, stale server BPI in local inference, UI redesign, persisted live authority, new class.
- **Prerequisites:** `NinjamTimingCoordinator.AcceptedRemoteGridRetainsAuthoritativeBpi` and `Quantisation.CurrentTempoTimingUsesActiveClockWhenMasterCacheIsUnset` pass before edits; add one absent-BPI assertion within the first test if needed.
- **Rollback:** revert comparison/fallback removal together.
- **Verification:** prerequisites; same geometry/new observation keeps prompt; every geometry/policy change replaces it; missing BPI changes no remote authority; local disconnected inference deterministic. Review: `B013.md`.

### B014 — Bounded correlated diagnostics

- **Owner / objective:** existing NINJAM job owner formats bounded transition/anomaly records correlated by epoch and desired/applied version; remove dead receipt and Scene/Station callback hierarchy logging.
- **Owned files:** existing coordinator diagnostics/desired/applied values; `audio/AudioHost.h/.cpp`; `engine/Scene.h/.cpp`, `Station.h/.cpp`, `LoopTake.h/.cpp` only for diagnostic removal; focused diagnostics tests.
- **Prohibited collateral:** logging class, hierarchy walker, callback formatter/I/O/string/vector/traversal, unbounded record, authority behavior change.
- **Prerequisite:** P10 `NinjamTimingDiagnostics.DisabledIsZeroWorkAndEnabledIsBounded` passes before production edits.
- **Rollback:** revert retained compact signal and deletions together; F-019 dead removal may be its own first commit but cannot claim diagnostic replacement.
- **Verification:** P10; every rejection reason; fixed capacity/overflow; two-session correlation; zero disabled callback work; logging on/off phase equivalence; bounded normal/verbose manual trace. Review: `B014.md`.

### B015 — Bounded vocabulary and casing

- **Owner / objective:** subsystem owners mechanically apply approved authority/coordinate/grid/automation/export/metronome names after interfaces settle.
- **Owned files:** Jamma-owned timing/quantisation/command/observation/export/metronome/MIDI automation declarations and direct consumers/tests/UI strings; no upstream or persistence-key rename.
- **Prohibited collateral:** semantic/ownership behavior, repository-wide rename, NJClient, schema keys, collapsed anchor/origin or clock domains.
- **Prerequisites:** `NinjamTimingCoordinator.AcceptedRemoteGridRetainsAuthoritativeBpi` and `TransportPhaseOffset.DirectTimingCommandKeepsMidiAutomationWithNoteCursor` pass before the rename group.
- **Rollback:** commit simple `Bpi`/`Bpm` casing separately from semantic clock/authority names and conditional export names; revert a group independently if compile/audit fails.
- **Verification:** prerequisites; incremental tests; retired-name/UI-string/schema-key audits; exact protected glossary review. Review: `B015.md`.

### B016 — Documentation truth reconciliation

- **Owner / objective:** documentation owner describes final implemented owners/contracts and explicit residual/manual status without claiming unexecuted evidence.
- **Owned files:** `doc/ninjam.md`, `doc/loop-alignment-and-ninjam-sync.md`, `doc/ninjam-midi-quantisation-investigation.md`, directly affected source/test comments only.
- **Prohibited collateral:** production edits, new promises beyond verified behavior, marking manual cases passed, security-certification claim.
- **Prerequisite:** B011/B013/B015 accepted; no executable prerequisite because this is statement-only.
- **Rollback:** doc statements revert with the behavior batch they describe; standalone status corrections revert as one doc commit.
- **Verification:** statement-to-source/test links; retired command/generation/zero-sentinel wording search; document F-045 zero-default downgrade and F-049/F-050/generic JSON/upstream residuals accurately. Review: `B016.md`.

### B017 — Obsolete uncompiled test deletion

- **Owner / objective:** native-test owner deletes only `test/JammaLib_Tests/src/timing/RemotePhaseDiscipline_Tests.cpp`; never re-register removed Quantiser APIs.
- **Owned files:** that file only; project/filter files are audit-only unless a stale entry is discovered.
- **Prohibited collateral:** pseudo-integration rewrite, production code, other test deletion, resource/project sweep.
- **Prerequisites:** `NinjamLoopAlignment.SourcePhaseReachesZeroAtNextRemoteWrapAcrossUnequalRulers` and `TransportPhaseOffset.SyncPhaseMapPreservesLateAudioAndMidiOrigins` pass before deletion.
- **Rollback:** restore only the deleted file if replacement coverage/project audit fails.
- **Verification:** prerequisites and full focused timing suite; file/retired-symbol/project membership audit; Stage 21 `git diff --check` confirms the deleted EOF issue is gone. Review: `B017.md`.

## No-batch decisions and accepted residuals

| Finding/scope | Disposition at this gate |
| --- | --- |
| F-001–F-004 | Retained in merge; no timing cleanup. Final regression/hygiene only. |
| F-020 | No change under later G3-1 resource exclusion. |
| F-045 | No schema guard; missing field defaults zero and older-binary resave loss is accepted. B011/B016 only preserve/test/document that policy. |
| F-049/F-050 | Rejected cleanup. Current `.jam` password and work-directory writing remain explicit accepted risks. |
| Generic JSON/upstream NJClient limits | Accepted exclusions; no broad security certification. |

## Human batch-approval gate

Approval must explicitly accept B001–B017, their order/dependencies, protected invariants, owned/prohibited files, rollback points, prerequisite tests, verification, and no-batch residuals. After approval, record the literal gate commit hash in this file and `phase-packets/phase-4.md`; that single approval authorizes the final sequential execution pass without another routine gate between batches. Stop and request a new decision only for scope expansion, a failed check that cannot be corrected inside its batch, or an independent review that requires a different invariant/boundary.

**Human outcome:** approved at gate commit `2e770b743d9f2466b2edafff5c92faf139d93108`. The approved single final execution pass is authorized under the contract above.
