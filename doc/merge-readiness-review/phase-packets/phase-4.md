# Phase 4 packet — Reconciliation and proposed cleanup batches

> **Status: cleanup-batch gate approved; final execution pass in progress.** The human approved the complete gate at commit `2e770b743d9f2466b2edafff5c92faf139d93108`. No cleanup, cleanup verification, batch review, post-cleanup Stage 21 result, or `merge-brief.md` existed when execution began.

## Inputs received

- Governing policy/design: `AGENTS.md`, the main merge-readiness plan, all four phase specifications, `doc/loop-alignment-and-ninjam-sync.md`, `doc/realtime-audio.md`, and `doc/build.md`.
- Approved earlier work: `phase-1.md`, `phase-2.md`, `phase-3.md`, the restored append-only `../decisions.md`, `../00-scope-and-inventory.md`, `../findings.md`, and `../verification-matrix.md`.
- Complete bounded Phase 4 reports: `../stage-reports/19-reconciliation.md`, `20-change-impact.md`, and the design-only `21-merge-hygiene.md`.
- Proposed execution contract: `../cleanup-backlog.md` B001–B017.
- Phase 4 kickoff: branch `bugfix/align-remote-join`, kickoff `HEAD` `8cd8725bd4447d4125f671b9524f95e277396939`, production tip still `e72f3b0f489cfa12ea697e966d3c0f619e31d1f6`, merge base/current `master` `4941b780f7ff5a46f742167d79338e3ab592a565`, and a clean worktree before report creation.

Stage agents wrote only their assigned reports. The integrator restored decision provenance, reconciled canonical statuses and verification obligations, then proposed batches. No production, test, project, resource, build-output, or local task file was changed; no build/test/manual scenario was run.

## Coverage and exclusions

Stage 19 reconciled all F-001–F-050 IDs, all phase packets, human dispositions, inventory partitions, protected terms, and accepted residuals. Stage 20 mapped every accepted candidate or explicit non-candidate across APIs, threads, persistence, UI/VST/MIDI/assets, tests, dependencies, rollback, and verification. Stage 21 grounded future commands in the local `.vscode/tasks.json`, repository wrapper, project membership, local/generated artifacts, formatting, and two-range final-diff audit without executing them.

Intentional exclusions remain controlling:

- HUD, VST3 implementation, window/tooling, unrelated MIDI/resources, and upstream NJClient stay in the merge but outside cleanup.
- F-020's unregistered HUD textures remain unchanged under later G3-1 scope.
- F-045 requires no schema guard; missing offset defaults zero and older-binary resave loss is accepted.
- F-049/F-050 are rejected/no-change risks. Generic JSON resource limits and unavailable upstream NJClient bounds remain accepted residual exclusions; this review is not a broad security certification.
- Runtime/manual, profiler, sanitizer, page-heap/Application Verifier, and post-cleanup build/test evidence remain intentionally unexecuted until batch approval.

No unowned investigation gap remains. Missing executable evidence is assigned to a prerequisite, batch verification, Stage 21 closeout, or explicit final residual.

## Integrated system model

The approved target retains four owners. The NINJAM integration/job side owns physical availability, session epoch, validation, request/prompt state, follow-policy selection, and one complete desired remote transport value. Scene presents prompts and forwards values but does not construct timing authority. AudioHost compares desired with applied state at the top of an audio block, owns Timer replacement and the one common remote-to-local mapped-source calculation, then issues neutral reset/correction/restore operations. `Station -> LoopTake -> Loop` retains membership plus every entity-specific anchor, length, audio cursor, MIDI event cursor, and automation origin.

The common calculation never becomes a common cursor. Recovery for loops of `M`, `2M`, `3M`, and non-divisor lengths applies the same mapped elapsed/correction amount against each entity's own anchor and modulo. Rebase changes the common map origin without recapturing entity anchors. Epoch/`NoSync` invalidation clears authority, map, anchors, and generation gates without moving local cursors. Full BPI is mandatory whenever remote authority exists; disconnected local BPI remains pure inference from local master length and grain.

The dependency order follows that model: lifetime and input safety; coherent observation/numerics; lifecycle/recovery; complete desired authority and producer ownership; empty-edge correction; neutral common-map consolidation; then local simplifications, persistence compatibility, ownership/header cleanup, BPI identity, diagnostics, vocabulary, docs, and obsolete-test deletion.

## Canonical findings added/changed

No new F-ID was added and no existing ID was merged or split. All F-001–F-050 remain independently auditable.

- F-035–F-048 statuses now reflect the Phase 3 human decisions.
- F-044 is mandatory behavior-preserving signed-offset recovery for distinct long-loop positions.
- F-045 is decision-resolved/no implementation beyond zero-default proof and documentation.
- F-046 requires full remote BPI while preserving disconnected pure-local inference.
- F-047/F-048 are accepted only as bounded, surgical changes in existing owners.
- F-020 is accepted historically but excluded from this cleanup by G3-1.
- F-049/F-050 are rejected/no-change accepted risks, not remaining must-fix cleanup.
- Stage 21 S21-01 receives no F-ID: the integration-test EOF whitespace belongs to the B006 test-owning work, while deleting F-037's obsolete file consumes the other instance.

The implementation set is represented by 42 accepted finding IDs across B001–B017. Five scope records (F-001–F-004/F-020) and three no-implementation decisions (F-045/F-049/F-050) remain visible outside the cleanup set.

## Duplicates and contradictions resolved

- F-009/F-024 are related but remain separate: complete desired-state semantics land before sole-producer movement. F-038/F-039 provide prerequisite evidence and do not duplicate either production fix.
- F-021 remote observation and F-023 local transport publication remain different owned values; P5/P6 gate their shared batch without merging clock domains.
- F-024 complete state, F-025 consumer gate reset, and F-027 physical lifecycle remain distinct and land in dependency order; there is no ordered-command or standalone-invalidation fallback.
- F-005 neutral policy boundary and F-006 common-map ownership share B009 only after desired-state/epoch semantics settle. F-018 deletes only the unread field; per-entity anchors remain live.
- F-019 dead receipt, F-034 callback logging, and F-047 diagnostic correlation share a bounded diagnostics batch only after epoch/version fields exist; diagnostics remain mirrors, never authority.
- F-044 controls the old normalization contradiction: master-modulo normalization is not equivalent for `2M`/`3M`, so signed/turn information or an equivalent common correction must survive until per-entity application.
- F-046's remote BPI requirement does not prohibit local disconnected inference.
- Later G3-1 controls F-020; later G3-9 controls F-049/F-050. None may enter another batch as collateral.
- The accidental overwrite of Phase 1/2 decisions in `8cd8725` was repaired by restoring their content from `70e48a5` and appending Phase 3 text; canonical decision links resolve again.

## Verification additions

`../verification-matrix.md` now gives concrete planned GoogleTest names for P1–P10 and adds the narrow P11 signed-offset, P12 remote-input, and P13 local-seed prerequisites. Each major refactor names only one or two closest prerequisites in `../cleanup-backlog.md`; there is no umbrella P1–P13 batch.

The exact future command and evidence contract is in Stage 21. Before every run, reread `.vscode/tasks.json` and `doc/build.md`; invoke the task's MSBuild executable/arguments only through `invoke-msbuild.ps1`; use affected incremental targets; and record filters/results in both the matrix and the batch review. The Phase 2 821/822 baseline remains historical and does not satisfy a cleanup gate.

Final closeout after B001–B017 requires Debug and Release incremental solution builds, the full native suite, project/resource/local-artifact/formatting audits, the agreed manual remote-join/local-loop scenarios, and complete `master...HEAD` plus approval-gate-to-`HEAD` diff reconciliation. Any unavailable tooling or manual scenario remains explicit rather than silently passing.

## Gate questions

Approval of this packet must answer all of the following as one proposed-batch gate:

1. Approve B001–B017, their dependency order, exact owned/prohibited surfaces, rollback points, prerequisite tests, verification, and independent batch-review requirement.
2. Approve the protected implementation invariant: common mapped progress is applied independently to `M`/`2M`/`3M`/non-divisor entity anchors and lengths; no shared cursor/master-modulo collapse; `NoSync` moves no cursor.
3. Approve the no-batch dispositions: F-001–F-004/F-020 stay unchanged; F-045 is zero-default/no-schema; F-049/F-050 and generic JSON/upstream limits remain accepted residuals.
4. Approve a single remaining execution pass after this gate: capture the literal gate commit, execute/review B001–B017 sequentially, complete Stage 21 verification, reconcile all artifacts, write `merge-brief.md`, and stop only at the final human merge decision. No routine intermediate human gate is required unless scope changes or a batch cannot pass/review within its approved boundary.

## Human outcome

Approved at gate commit `2e770b743d9f2466b2edafff5c92faf139d93108`. The approval explicitly covers B001–B017, their dependency order, prerequisites, owners, prohibited collateral, rollback points, verification requirements, independent reviews, protected timing invariants, and recorded no-batch residual risks. It authorizes the single final execution pass described in the plan without another routine gate between batches.

Execution must return to the human only for scope expansion, a batch failure that cannot be corrected inside its approved boundary, or an independent review requiring a different invariant or boundary. The final execution pass still stops at the human merge-decision gate after Stage 21 evidence and `merge-brief.md` are complete.

**B004 sequencing addendum (approved against `HEAD` `0aee948d3af6b4e9bde7103dcfd53d9953f4b5b7`):** The human authorizes a characterization-first exception for B004: commit/register P5/P6, record their expected pre-fix failures, implement strictly inside the existing B004 boundary, then require both tests green before advancing. All original protected timing invariants, prohibited collateral, rollback rules, dependencies, and no-batch residual decisions remain unchanged.  Such sequence changes to the work are also permitted elsewhere where it makes sense, and would otherwise block downstream work.
