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
| B006 | Apply one complete desired state and reset every epoch/generation gate | F-024, F-025, F-038, F-039, F-047 (epoch/version only) | B004, B005 | P1, P3 |
| B007 | Move complete-state production to the NINJAM integration owner | F-009, F-038 | B006 | P2 |
| B008 | Move the empty-scene reset edge off the callback | F-022 | B005–B007 | P8 (already passing from B005) |
| B009 | Neutralize engine timing APIs and consolidate the common map | F-005, F-006, F-018, F-038 | B004, B006–B008 | P3, P4 |
| B010 | Consolidate only equivalent direct LoopTake cursor shifts | F-035 | B009 | two named existing LoopTake tests |
| B011 | Preserve signed local offsets for long loops | F-044, F-045 | B010 | P11 |
| B012 | Settle timing value/header/mailbox ownership | F-007, F-008, F-012 | B004, B007, B009 | one named existing local-offset test |
| B013 | Centralize proposal identity and require full remote BPI | F-036, F-046 | B007, B012 | two named coordinator/local-inference tests |
| B014 | Replace callback hierarchy logs with bounded correlated diagnostics | F-019, F-034, F-047 (diagnostics only) | B006, B007 | P10 |
| B015 | Apply bounded timing vocabulary and casing changes | F-010, F-011, F-013, F-014, F-015, F-016, F-017 | B009, B012–B014 | two named focused tests |
| B016 | Make timing documentation truthful | F-042, F-043, F-045 | B011, B013–B015 | static statement/source audit |
| B017 | Delete the obsolete uncompiled timing test | F-037 | B006, B009 | two registered replacement tests |

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

### B005 — Session lifecycle and recovery

- **Owner / objective:** existing session/integration/coordinator owners publish availability and a fresh session epoch; loss/retry/invalid/deadline transitions produce one idempotent `NoSync` and recover.
- **Owned files:** `JammaLib/src/ninjam/NinjamSession.h/.cpp`, `NinjamConnection.h/.cpp`, `NinjamController.h/.cpp`, `NinjamTimingCoordinator.h/.cpp`, `NinjamNetworkService.h/.cpp` if already the integration owner; Scene call sites only for start/loss forwarding; focused tests/project entries.
- **Prohibited collateral:** persisted live epoch; ordered timing queue; map/engine API rewrite; raw buffer seam; broad Scene cleanup.
- **Prerequisites:** P7 `NinjamSessionTiming.PhysicalLossRetryCreatesOneNoSyncAndFreshEpoch` and P8 `NinjamTimingCoordinator.NoObservationAndInvalidTimingRecoverIdempotently` pass before production edits.
- **Rollback:** revert availability/epoch publication and lifecycle hooks to the pre-B005 commit; no half-migrated persisted/interactive start paths.
- **Verification:** P7/P8; shipped/default and saved `.jam` start; physical loss/retry/manual disconnect; exactly one `NoSync`; fresh epoch; no stale prompt/request/remote station. Review: `B005.md`.

### B006 — Complete desired state and epoch/generation gates

- **Owner / objective:** AudioHost compares one latest complete desired remote transport state with last applied state; epoch change clears map/anchors/all gates, geometry change replaces Timer, same geometry disciplines phase, `NoSync` moves no cursor.
- **Owned files:** Jamma-owned desired-state value/mailbox; `JammaLib/src/audio/AudioHost.h/.cpp`; `engine/Station.h/.cpp`, `LoopTake.h/.cpp` only for epoch/gate application; production-boundary and command/timing tests/project entries; minimal epoch/version fields of F-047.
- **Prohibited collateral:** ordered/delta commands, standalone invalidation, dual old/new authority, map consolidation, naming/docs/diagnostics formatting, cursor reduction modulo master length.
- **Prerequisites:** P1 `NinjamTimingProductionBoundary.CompleteDesiredStateSupersedesFormerCommandSequences` and P3 `NinjamTimingProductionBoundary.ReconnectPreservesM2M3MEntityOffsetsAcrossEpochOne` pass before production edits.
- **Rollback:** revert the complete desired type, AudioHost consumer, and gate reset together to the pre-B006 commit; no compatibility shim retains rejected command semantics.
- **Verification:** P1/P3; empty/populated join; Continuous/Block/NoSync; same-state idempotence; stale/equal generations; reconnect generation 1; `M`/`2M`/`3M` audio/MIDI/automation offsets. Remove Stage 21's trailing EOF blank only in the owned `NinjamTimingIntegration_Tests.cpp` prerequisite edit. Review: `B006.md`.

### B007 — NINJAM integration producer boundary

- **Owner / objective:** existing NINJAM integration owner is the sole complete-state producer; Scene only presents prompts and forwards values.
- **Owned files:** `JammaLib/src/ninjam/NinjamTimingCoordinator.h/.cpp` and existing integration/network-service owner; desired-state value; `JammaLib/src/engine/Scene.h/.cpp`; focused production-boundary tests.
- **Prohibited collateral:** second producer, generic dispatcher/class, UI redesign, map/engine changes, persisted authority.
- **Prerequisite:** P2 `NinjamTimingProductionBoundary.OverlappingIntentsPublishOneCoherentDesiredVersion` passes before producer movement.
- **Rollback:** revert producer ownership and Scene reduction together; there must be exactly one producer before and after rollback.
- **Verification:** P2; rapid job/UI intents; one desired/applied version; prompt behavior; Scene audit shows no transport-state construction. Review: `B007.md`.

### B008 — Empty-scene ownership edge

- **Owner / objective:** Scene/job owner handles empty/non-empty cleanup; callback publishes/consumes only bounded immutable state.
- **Owned files:** `JammaLib/src/engine/Scene.h/.cpp`, `audio/AudioHost.h/.cpp`, existing station snapshot/edge value; focused Scene/timing tests.
- **Prohibited collateral:** raw `_stations` callback traversal, callback Quantiser mutation/destruction, generic Scene slimming, diagnostic cleanup.
- **Prerequisite:** B005's passing P8 is re-run at B008 head before source edits.
- **Rollback:** revert empty-edge publication and job handler together.
- **Verification:** connected-empty preserves timing; disconnected final-take removal clears once; retry/reconnect; callback audit has no raw container, lock, allocation, or destruction. Review: `B008.md`.

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
