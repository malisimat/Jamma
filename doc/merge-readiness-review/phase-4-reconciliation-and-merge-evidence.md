# Phase 4 — Reconciliation, Cleanup Batches, and Merge Evidence

## Goal and human gate

Turn independent findings into a safe, minimal cleanup sequence and a merge decision. Human gate: approve each cleanup batch and the final merge brief; the branch is not ready merely because reviewers found no more ideas.

## Independent review stages

19. **Cross-review reconciliation** (`stage-reports/19-reconciliation.md`). **Primary ownership:** compare canonical findings and all phase packets for duplicate root causes, contradictory evidence/dispositions, incompatible timing models, missing inventory coverage, and accidental erosion of protected concepts. **Excludes:** independently widening the review with new speculative sweeps. **Required handoff:** canonical-ID merge/split table, contradiction resolutions, coverage exceptions, and unresolved questions for the human gate.
20. **Change impact and regression surface** (`stage-reports/20-change-impact.md`). **Primary ownership:** map each accepted candidate cleanup—not each raw finding—to APIs, threads, persistence, timing transitions, UI/VST/MIDI behavior, assets, tests, and dependencies. **Excludes:** choosing product priority or implementing. **Required handoff:** impact/dependency matrix and recommendations to split or reject over-broad batches.
21. **Build, test, and merge hygiene** (`stage-reports/21-merge-hygiene.md`). **Primary ownership:** build/test command validity, project membership, local-only artifacts, formatting, generated assets, and final diff hygiene. Before every build/test, read `.vscode/tasks.json` and `doc/build.md`; use the repository environment-normalizing wrapper and affected incremental targets. **Excludes:** re-reviewing production semantics. **Required handoff:** exact commands, results/evidence locations, limitations, and final changed-file audit. During initial reconciliation this stage designs the checks; after cleanup it updates the same report with executed results.

## Cleanup execution sequence

1. The phase integrator converts human-accepted findings into small dependency-ordered batches in `cleanup-backlog.md`: safety/correctness first, then deletions and boundary simplification, then naming/docs/tests, then project/resource hygiene. Each batch has one objective, owned files, finding IDs, prerequisites, prohibited collateral changes, rollback point, and verification rows.
2. The human approves the proposed batch boundaries and order. Findings that touch the same file are not automatically combined; combine only when they share one invariant and cannot be verified independently.
3. A single implementation owner takes one approved batch at a time. No two active batches may edit the same files or dependent interfaces. The owner updates the corresponding finding status, backlog row, and verification evidence in the same change.
4. A separate subagent reviews each completed batch against its approved findings, protected concepts, and final diff, recording approval or rework in `batch-reviews/B###.md` without editing implementation. New unrelated concerns become new findings; they are not silently folded into the batch.
5. Build and run only the applicable native target/tests after each engine change. At the end, run the agreed full relevant suite and manual remote-join regression scenarios. A failed check reopens the batch and its dependent batches.

## Single final execution pass after batch approval

The proposed-batch human gate is the last planned pause before final merge evidence. When the human approves the complete `cleanup-backlog.md` batch set:

1. capture the literal approval-gate commit hash in the backlog and Phase 4 packet before any source movement;
2. execute all approved batches sequentially in dependency order, landing each batch's one or two closest prerequisite tests before its production refactor, then focused verification and an independent `batch-reviews/B###.md` review;
3. update findings, backlog status, verification rows, and documentation with each owning batch rather than deferring artifact reconciliation;
4. after every batch is approved, complete the deferred Stage 21 execution section, final incremental builds/full native suite/manual scenarios, and both complete-range and cleanup-only diff audits;
5. synthesize `merge-brief.md` from the final diff and every artifact, then stop at the final human merge decision.

No routine human pause is required between approved batches. Return to a human gate only if scope must expand, a required check cannot be made to pass within the approved boundary, or an independent review requires a materially different invariant or batch shape. This makes cleanup, testing, backlog closure, final verification, and merge evidence one remaining phase without weakening any batch rollback/review requirement.

## Lead synthesis

After all human-reviewed phases and cleanup batches are complete, the lead synthesizer—not an investigator working from chat summaries—reads every shared artefact, the current diff, and relevant source, then produces `merge-brief.md`. It must:

1. Reconcile the final diff against `master`, the commit history, and the approved glossary; confirm that sync modes, sync phase/source-coordinate mapping, and local/remote timing remain distinct and coherent.
2. Deduplicate all findings and report counts by severity, subsystem, disposition, and verification status. Every unresolved item must link to a human decision and be stated as an explicit merge risk.
3. Check that each accepted cleanup has evidence in `verification-matrix.md`, and that no rejected/deferred item was silently implemented.
4. Summarize the final architecture/data-flow in plain language, list intentional scope outside the remote-join bugfix, and identify any proposed follow-up work that must not block merge.
5. Produce the merge recommendation: **ready**, **ready with accepted risks**, or **not ready**. A human makes the final merge decision from this brief; the lead synthesizer does not override the human gate.
6. Include an evidence index linking every phase packet, accepted/deferred decision, batch review, build/test result, and manual scenario, plus a short account of disagreements and how they were resolved.

## Final human merge checklist

- [ ] All four phase packets reviewed and decisions recorded.
- [ ] No unresolved merge blocker or must-fix finding.
- [ ] Final incremental build and agreed native tests pass using the repository build guidance.
- [ ] Remote-join/local-loop manual scenarios pass and evidence is linked.
- [ ] `merge-brief.md` accurately states remaining risks and the human approves the recommendation.
