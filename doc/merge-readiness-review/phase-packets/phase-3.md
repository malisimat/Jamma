# Phase 3 packet — Maintainability, tests, documentation, and product contracts

## Inputs received

- Governing policy/design: `AGENTS.md`, the merge-readiness plan and all phase files, `doc/loop-alignment-and-ninjam-sync.md`, `doc/realtime-audio.md`, `doc/ninjam.md`, and `doc/build.md`.
- Human constraints and accepted prior gates: `../decisions.md`, `../00-scope-and-inventory.md`, `phase-1.md`, and `phase-2.md`.
- Canonical artifacts: `../findings.md`, `../verification-matrix.md`, and `../cleanup-backlog.md`.
- Six complete immutable investigator reports: `../stage-reports/13-simplification.md` through `18-input-robustness.md`; Stages 13–15 and 16–18 ran as two bounded, non-colliding waves, and every stage report was committed separately.
- Phase 3 kickoff: branch `bugfix/align-remote-join`, review `HEAD` `70e48a5403dd409aca73e7efc0490464c8636a00`, production tip `e72f3b0f489cfa12ea697e966d3c0f619e31d1f6`, merge base/master `4941b780f7ff5a46f742167d79338e3ab592a565`, clean worktree before review edits.

Phase 2 reconciliation recorded one controlling correction before dispatch: original F-024's ordered/delta alternative is rejected. Only a latest complete desired remote transport state is in bounds. No source, test, project, build, upstream NJClient, cleanup backlog, or human-decision content was changed in Phase 3.

## Coverage and exclusions

| Stage | Actual coverage | Deliberate exclusions / handoff |
| --- | --- | --- |
| 13 Simplification | Live timing abstractions and code-size/responsibility in coordinator/Scene/AudioHost/Station/LoopTake/Loop/Timer; diagnostics and map ownership; ranked deletion/risk/dependencies | Consumed Stage 6 dead code; no HUD/VST/window/tooling/unrelated work; no generic mailbox/traversal abstraction |
| 14 Tests | All changed timing-focused native suites, project membership, real LoopTake/Timer/MIDI seams, and production observability needed to judge contracts | No production fix; retained optional features and unrelated MIDI excluded; no build/run |
| 15 Docs/comments | Timing design/integration/TDD docs, changed source comments/strings, and timing test descriptions | Runtime log cost belongs to F-034; production naming to F-010–F-017; no implementation |
| 16 Compatibility | Well-formed `.jam` NINJAM identity, transport offset, session starts, producer/consumer defaults/fallbacks | Malformed/hostile inputs to Stage 18; retained VST/window/persistence breadth excluded |
| 17 Observability | Timing logging config, coordinator diagnostics, applied receipt, Scene/Station hierarchy dumps, identifiers/levels/volume | Consumed Stage 8 cost; no general logger redesign or unrelated logs |
| 18 Robustness | Remote timing/config/network/path/credential boundaries, seed policy, timing-relevant MIDI bounds, serialization/resource limits | Not a broad security certification; generic JSON/upstream limits recorded as residuals, not widened cleanup |

The human-directed Scene/Station/LoopTake slimming was covered for timing authority and diagnostics. HUD, VST3 parity, window/tooling, non-timing resources, unrelated MIDI, and upstream NJClient remain intentionally outside cleanup review while staying in the merge. No Phase 3 build, native test, runtime session, profiler, sanitizer, fuzz run, or hostile-file execution occurred; Phase 2's 821/822 native baseline remains the latest executed evidence and does not close the new findings.

## Integrated system model

The simplest safe architecture retains four distinct owners. The NINJAM integration owner publishes physical availability/session epoch and one complete desired remote transport value: follow policy, validated device-rate remote geometry, and timestamped remote master phase. Scene presents prompts and forwards values, but does not construct timing events. AudioHost compares desired with last applied state at the block boundary: epoch change clears map/anchors/gates, geometry change replaces Timer timing, unchanged geometry derives phase correction, and `NoSync` clears authority without cursor movement. Timer retains local master geometry and the separate monotonic scene coordinate.

AudioHost alone owns the common remote-to-local source ruler and computes one mapped source coordinate per boundary/block. Station performs explicit neutral fan-out. Each LoopTake/Loop retains only entity-specific anchors, lengths, audio phase, MIDI event cursor, and automation origin and wraps the common coordinate/correction independently. This preserves unequal lengths and intentional offsets; mapped elapsed time never becomes a shared cursor.

Diagnostics are mirrors, not authority. Compact coordinator counters plus an AudioHost applied receipt become one configuration-gated, off-thread, transition/rate-bounded trace correlated by session epoch and desired/applied version. Scene/Station hierarchy formatting and the dead LoopTake receipt disappear only after the bounded replacement is verified. Disabled callbacks do zero diagnostic work.

Persistent `.jam` state may retain connection identity and local construction/offset settings, but not live remote geometry, phase, follow policy, map, anchors, generation, or epoch. A persisted/default connection must enter the same fresh lifecycle as interactive connect. Complete server BPI is the proposed compatibility boundary. Portable exports must not carry a password or filesystem authority over NJClient work directories.

### Ranked simplification map

| Rank | Finding | Estimated net deletion | Coupling reduction | Behavioural risk / prerequisite |
| ---: | --- | ---: | --- | --- |
| 1 | F-034 diagnostics relocation/deletion | 80–150 production lines; 160–230 removed from Scene/Station before smaller sink | high | low playback/medium diagnostic; P10 first |
| 2 | F-037 obsolete uncompiled test deletion | 156 test lines | medium | low; confirm current suites registered |
| 3 | F-024/F-009 complete desired state | 70–120 production lines | very high | high; P1/P2 plus F-021/F-023/F-025–F-031 prerequisites |
| 4 | F-006 single common-map owner | 45–75 production lines plus five take fields | very high | medium-high; P3/P4 and epoch reset first |
| 5 | F-005 neutral engine boundary | 25–40 production lines | high | medium; after complete state/P3 |
| 6 | F-035 direct cursor-shift primitive | 20–30 production lines | medium | low; characterize both callers and leave queued path unchanged |
| 7 | F-021 live-timing adapter removal | 15–35 net production lines | high | high observation risk; P5/P6 first |
| 8 | F-036 proposal identity | 6–12 production lines | medium | low; coordinate remote-grid naming |

The estimates overlap and are not additive. No generic seqlock/mailbox abstraction, generic hierarchy traversal, new timing/logger class, removal of the NINJAM integration owner, or collapsed timing vocabulary is proposed.

## Canonical findings added/changed

Phase 3 added 16 canonical findings: 11 must-fix-before-merge items and 5 follow-ups; it added no new merge blocker. Existing merge blockers and must-fix findings remain controlling prerequisites.

| Canonical | Candidate(s) | Severity | Integrated disposition |
| --- | --- | --- | --- |
| F-035 | S13-05 | follow-up | One private direct cursor-shift primitive; preserve caller gates/queued ordering |
| F-036 | S13-07 | follow-up | One proposal identity comparison in existing NINJAM owner |
| F-037 | S14-01 | follow-up | Delete 156-line uncompiled obsolete test; retain current-owner coverage |
| F-038 | S14-02, S15-05 | must fix | Replace false model integration evidence with production-faithful P1–P4 seam |
| F-039 | S14-03, S15-05 | must fix | Rewrite rejected command and zero-sentinel expectations |
| F-040 | S14-04 | must fix | Deterministically prove concurrent mailbox overlap/non-torn values |
| F-041 | S14-05 | must fix | Controllable loss/retry/deadline/buffer-borrow test seams in existing owners |
| F-042 | S15-01 | must fix | Correct integration-guide ownership, interval, MIDI anchor, and `Stay local` contract |
| F-043 | S15-03 | must fix | Convert stale MIDI TDD plan to current implementation/residual record |
| F-044 | S16-02 | must fix or explicit break | Support signed-offset artifacts correctly for unequal lengths or declare unsupported |
| F-045 | S16-03 | follow-up decision | Accept/document or guard older-binary loss of local offset |
| F-046 | S16-04 | follow-up | Require full authoritative remote BPI; remove unreachable fallback |
| F-047 | S17-02 | must fix | Shared epoch/version and bounded rejection-reason diagnostics |
| F-048 | S18-02 | must fix | Bound/check seed-policy conversion before Windows 32-bit cast |
| F-049 | S18-03 | must fix | Redact password from portable export |
| F-050 | S18-04 | must fix | Portable `.jam` cannot select/create arbitrary NJClient work directory |

Changed existing findings:

- F-005/F-006/F-009 now contain the neutral engine boundary, one common map/source-coordinate owner, and explicit complete desired-state production shape from S13.
- F-021 includes exact pre-advance owned publication and deletion of the live-timing adapter chain.
- F-024 is retitled/replaced with the only human-approved latest-complete-desired-state disposition; ordered commands, standalone invalidation, and phase-delta publication are out of bounds. S15's command/design documentation corrections attach here.
- F-027 includes the persisted/default `.jam` auto-connect path that currently bypasses timing lifecycle initialization.
- F-032 covers finite/plausible validation at both numeric conversion and outgoing tempo serialization.
- F-034 now contains the S13/S17 code-size, retained signal, identifier, level, and bounded-volume contract; S15 field-name corrections attach to F-013–F-016/F-034.

Every new finding and each existing refinement was committed separately with its verification row. Human decisions remain pending for F-035–F-050 and Phase 3 refinements; no cleanup status was changed.

## Duplicates and contradictions resolved

- S13-02 is not an alternative to F-024: it is the approved replacement disposition. All ordered/delta/standalone-invalidity paths are explicitly rejected.
- S13-03 enriches F-006; copied per-take map geometry is live until the single owner exists. Entity anchors/modulo are retained.
- S13-04 enriches F-005 and depends on F-025 epoch reset; it does not merge follow policies.
- S13-06 enriches F-021; it deletes Jamma adapters only after exact timestamped publication exists and never edits NJClient.
- S13-01/S17-01 are one F-034 diagnostic responsibility/cost finding. F-019 remains only the dead receipt subset; F-047 separately owns missing diagnostic correlation/reasons.
- S14-02/S15-05 form F-038; S14 owns executable fidelity, while S15 contributes false-description evidence. S14-03/S15-05 similarly form F-039.
- S15-02 enriches F-024 rather than creating a documentation duplicate. S15-04 is the statement-level correction handoff for accepted F-013–F-016/F-034.
- S16-01 enriches F-027 rather than adding a second lifecycle owner. S16-02/F-044 is independent because it asks a persisted artifact support question using unequal-length phase proof.
- S18-01 enriches F-032. Invalid/absent remote authority still uses F-027/F-028's single `NoSync`/fresh-epoch recovery; no security-specific state machine is added.
- F-049/F-050 are separate from valid-format compatibility: one is secret disclosure and one is filesystem authority. Neither persists or changes timing coordinates.

No investigator contradiction remains. The only unresolved choices are human product/compatibility/log-policy decisions below.

## Verification additions

`../verification-matrix.md` now contains one row for every F-001–F-050 finding, the Phase 2 scenarios, the P1–P10 lean prerequisite suite, and five Phase 3 manual additions. The central rule is unchanged: before each major refactor, select and pass only the one or two closest prerequisite tests in their own Stage/Finding commit; do not create one umbrella test batch.

Priority order:

1. P1/P2 complete desired-state production boundary and producer coherence.
2. P3/P4 real unequal-length audio/MIDI reconnect and restore-before-rebase.
3. P5/P6 coherent publication plus zero/width/conversion boundaries.
4. P7/P8 physical/persisted start, loss/retry, no-observation, invalid/recovery.
5. P9 scoped remote stereo consumption.
6. P10 zero-disabled/bounded-enabled diagnostics.

Manual additions cover persisted/default start, signed-offset and downgrade behavior, bounded normal/verbose traces, export redaction/workdir containment, and malformed observed/requested/local timing. No obligation is marked executed by Phase 3.

Coverage gaps/residuals:

- no production-faithful AudioHost test seam yet; no deterministic connection fault/buffer-borrow seam;
- no Phase 3 build/test/runtime/profiler/sanitizer/fuzz/manual evidence;
- exact fixed diagnostic capacity and whether connection warnings bypass verbose configuration remain human/batch choices;
- distribution/support status of intermediate signed-offset artifacts is unknown;
- generic JSON file/depth/element limits are a real inherited application-I/O residual outside timing cleanup;
- upstream NJClient parser/user/channel/string/work-budget limits cannot be audited from the bundled header/library and must not be “fixed” by editing upstream.

## Gate questions

Record **accept**, **reject with rationale**, or **defer with named owner and accepted risk** in `../decisions.md`. Acceptance authorizes Phase 4 reconciliation/design only, not source/test cleanup.

### Model and scope gates

| Gate | Exact decision required |
| --- | --- |
| G3-1 | Accept/reject/defer Phase 3 coverage/exclusions and continued timing-only cleanup scope, including the protected glossary and retained HUD/VST/window/tooling exclusions. |
| G3-2 | Accept/reject/defer the ranked simplification map, overlap caveat, dependency order, and no-new-class/no-generic-mailbox/no-generic-traversal constraints. |
| G3-3 | Accept/reject/defer P1–P10 and the rule that one or two closest tests pass in separate prerequisite commits before each major refactor. |
| G3-4 | Accept/reject/defer the documentation policy: update docs with their implementation batch; until then explicitly distinguish current defect from approved target. |
| G3-5 | Decide F-044: support intermediate signed-offset `.jam` artifacts with behavior-preserving migration, or explicitly declare them unsupported and accept the unequal-length phase break. |
| G3-6 | Decide F-045: accept/document older-binary loss of `transportoffsetloopfrac`, or require a focused guard. |
| G3-7 | Accept/reject/defer full authoritative BPI as the remote-geometry compatibility boundary (F-046), with no partial-BPI fallback. |
| G3-8 | Accept/reject/defer the bounded observability contract and decide whether connection warnings/errors remain visible when verbose timing diagnostics are disabled. |
| G3-9 | Accept password redaction from portable exports and portable-workdir non-authority (F-049/F-050); separately state whether private app-owned config may retain credentials/custom paths. |
| G3-10 | Accept/reject/defer the explicit residual exclusions: generic JSON resource limits and unverifiable upstream NJClient bounds, with no claim of broad security certification. |

### Canonical finding gates

| Finding(s) | Decision required |
| --- | --- |
| F-005/F-006/F-009 refinements | Accept neutral engine operations, one AudioHost map/source-coordinate owner, and one complete-state integration producer; or reject/defer each with rationale. |
| F-021/F-024 refinements | Accept exact owned observation plus adapter deletion and the replacement complete desired-state contract; no ordered/delta fallback. |
| F-027/F-032/F-034 refinements | Accept persisted-start lifecycle, conversion+egress validation, and bounded off-thread diagnostics; or reject/defer each. |
| F-035/F-036 | Accept/reject/defer the two low-risk production simplifications. |
| F-037 | Accept/reject/defer deleting the obsolete uncompiled test. |
| F-038–F-041 | Accept/reject/defer each test-quality/prerequisite finding. |
| F-042/F-043 | Accept/reject/defer each documentation correction group. |
| F-044–F-046 | Record the explicit compatibility choices from G3-5–G3-7. |
| F-047 | Accept/reject/defer shared epoch/version and bounded rejection-reason diagnostics. |
| F-048 | Accept/reject/defer checked seed-policy bounds/conversion. |
| F-049/F-050 | Record the explicit credential/workdir product choices from G3-9. |

## Human outcome

Accepted in [`../decisions.md`](../decisions.md). The reviewer accepted G3-1–G3-10, all named Phase 3 refinements, and F-035–F-048 with these controlling details: per-entity `M`/`2M`/`3M` alignment must survive recovery; missing downgraded local timing defaults to zero; remote authority always requires full BPI while disconnected local BPI remains pure local inference; and diagnostics/input hardening must remain surgical. F-049/F-050 were rejected as cleanup and remain explicit accepted residual risks. This outcome authorizes Phase 4 reconciliation and batch design only; implementation remains prohibited until the proposed-batch human gate is approved.
