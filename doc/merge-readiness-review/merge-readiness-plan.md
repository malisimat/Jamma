# Bugfix Branch Merge-Readiness Review

> **Status: execution complete; final human merge decision pending.** All four phases, B001-B017 cleanup batches, independent reviews, Stage 21 automated/static verification, canonical reconciliation, and `merge-brief.md` are complete. The final recommendation is **READY WITH ACCEPTED RISKS**; no merge to `master` has been performed.

This review prepares `bugfix/align-remote-join` for a safe merge to `master`. It is a four-phase investigation and cleanup programme, not a request to rewrite the branch. The current comparison is large (the initial estimate for `master...HEAD` was roughly 14,700 insertions across 199 files), so Phase 1 must regenerate those figures rather than treating them as fixed. Independent subagents inspect bounded concerns in parallel and leave durable evidence for synthesis.

The review must preserve the consolidated timing model: the distinct sync modes, the sync map's separation of phase from source/scene coordinates, and the distinction between local and remote timing are intentional. A reviewer may propose clearer names or a smaller implementation, but must not collapse those concepts merely because they are related.

In particular, a common mapped elapsed time is not a shared loop cursor; master phase is not per-loop phase; the monotonic scene coordinate is not wrapped Timer geometry; and a follow policy is not a clock or coordinate system. Any finding that proposes merging these concepts must demonstrate behavioural equivalence for different loop lengths, intentional offsets, reconnects, and `NoSync` invalidation or it is rejected as out of bounds.

## Shared artefacts

At kickoff, the lead creates the following files and directories in this directory. Chat is coordination only; a phase is incomplete until its evidence is in these artefacts.

- `00-scope-and-inventory.md` — generated change inventory, commit ranges, subsystem ownership, hot-path map, and protected timing terminology.
- `stage-reports/NN-short-name.md` — one immutable report per numbered investigation stage. Only that stage's investigator writes it during parallel work.
- `phase-packets/phase-N.md` — the phase integrator's synthesis, contradictions, duplicate candidates, coverage gaps, and proposed gate decisions.
- `batch-reviews/B###.md` — one independent review record for each approved cleanup batch.
- `findings.md` — one finding per heading using the template below; include exact file and line references and a proposed disposition.
- `decisions.md` — human-approved decisions, including explicitly accepted risks and non-changes.
- `cleanup-backlog.md` — deduplicated, ordered implementation work with owner, test/verification, and dependency fields.
- `verification-matrix.md` — each accepted cleanup item mapped to build, native test, runtime/manual scenario, and reviewer sign-off.
- `merge-brief.md` — created only in Phase 4 from the final diff and all approved phase packets.

The lead owns `00-scope-and-inventory.md`. During investigation, stage agents write only their assigned `stage-reports/` file. The phase integrator alone merges reports into `findings.md`, `verification-matrix.md`, and its `phase-packets/` file. Only the lead records human decisions in `decisions.md` and changes backlog status. During approved cleanup, one implementation owner edits source and the corresponding backlog/verification rows; the batch reviewer writes only `batch-reviews/B###.md`. This single-writer rule prevents parallel edits from losing or duplicating evidence.

Finding template:

```md
## F-### — concise title

- Stage / reviewer:
- Scope reviewed / exclusions:
- Severity: merge blocker | must fix before merge | follow-up
- Evidence: `path:line`, commit(s), and a concise observed behaviour.
- Why it matters:
- Recommended disposition: remove | simplify | rename | move | correct | retain with rationale.
- Protected timing concepts affected: none | [list].
- Verification:
- Human decision: pending | accepted | rejected | deferred (link decision).
```

Use `master...HEAD` as the default comparison, and review the actual diff plus relevant surrounding code. Existing unrelated working-tree changes must not be altered or attributed to this branch.

Stage reports use the same fields but assign local candidate IDs such as `S09-01`; the integrator assigns canonical `F-###` IDs only after deduplication. A report must also list commands/queries used, files or subsystems actually covered, deliberate exclusions, uncertainties, and a `no findings` conclusion when applicable. Findings based only on a textual match are incomplete until surrounding code or history supports them.

Every stage report has these sections: `Assignment`, `Coverage`, `System understanding`, `Candidate findings`, `Handoffs`, `Uncertainties`, and `Conclusion`. Every phase packet has: `Inputs received`, `Coverage and exclusions`, `Integrated system model`, `Canonical findings added/changed`, `Duplicates and contradictions resolved`, `Verification additions`, `Gate questions`, and `Human outcome`. This common shape makes synthesis auditable without forcing investigators into a shared writable file.

## Execution model

Phases run in order. Phase 1 establishes the baseline and glossary; Phases 2 and 3 consume that approved baseline; Phase 4 consumes all approved phase packets. Do not start a later phase merely because a concurrency slot is free.

Each numbered stage is one bounded, read-only subagent assignment during investigation. Give the agent this plan, its phase file, `00-scope-and-inventory.md`, earlier approved phase packets, and the exact output filename. The lead retains one execution slot and acts as phase integrator after investigators finish; do not spend a parallel slot on an idle integrator.

The stages are divided by review question, not by exclusive file ownership: thread safety and timing correctness may legitimately inspect the same code, but they must not answer the same question. Each stage's **Primary ownership** is exclusive. Its **Required handoff** tells adjacent stages what to cite rather than re-investigate. If an agent discovers a concern owned elsewhere, it records a short cross-reference under `Handoffs` without developing a second finding. The integrator sends genuine gaps back to the owning stage before closing the phase.

Within a phase, run only stages whose primary ownership is distinct concurrently. After all reports land, the integrator:

1. checks report completeness and coverage against the inventory;
2. resolves duplicate candidates and contradictions with the relevant investigators;
3. assigns canonical finding IDs and updates canonical artefacts;
4. writes the phase packet, including a plain-language account of how the changes work as a whole; and
5. presents the human gate with explicit accept/reject/defer questions.

Every phase ends at that gate. Investigation does not authorize source edits. No cleanup implementation begins until Phase 4 has reconciled findings into a human-approved batch, unless a human explicitly approves an earlier urgent batch and its verification plan.

## Fresh-session kickoff

Before dispatching any stage, the lead must:

1. read `AGENTS.md`, this plan, all four phase files, `doc/loop-alignment-and-ninjam-sync.md`, `doc/realtime-audio.md`, and `doc/build.md`;
2. record branch name, `HEAD`, merge base, `master` tip, worktree status, diff statistics, and any pre-existing changes in `00-scope-and-inventory.md`;
3. create the empty canonical artefacts (except `merge-brief.md`) plus `stage-reports/`, `phase-packets/`, and `batch-reviews/`, without overwriting pre-existing review evidence;
4. publish the protected glossary and scope partition before starting Phase 1 investigators; and
5. add a small task board to `00-scope-and-inventory.md` listing every stage as pending, active, reported, integrated, or gated.

At every agent dispatch, include: stage number and title, primary ownership, explicit exclusions, required inputs, exact report path, read-only status, and the instruction to cite exact current lines and relevant commits. At every return, require a report file even when no problems were found.

## Phase map

1. [Phase 1 — Baseline, intent, and structural review](phase-1-baseline-and-structure.md)
2. [Phase 2 — Runtime safety, real-time performance, and correctness](phase-2-runtime-safety-and-correctness.md)
3. [Phase 3 — Maintainability, tests, documentation, and product contracts](phase-3-maintainability-and-contracts.md)
4. [Phase 4 — Reconciliation, cleanup batches, and merge evidence](phase-4-reconciliation-and-merge-evidence.md)

## Exit criteria

- Every finding is decided, implemented, or explicitly deferred by a human.
- The protected timing terminology has a single approved glossary and no contradictory semantics.
- Merge blockers and must-fix items are resolved and verified.
- The verification matrix contains a successful incremental build, applicable native tests, and focused manual remote-join/local-loop regression scenarios.
- The final human review approves the synthesized merge brief and the remaining risk statement.
- Every stage has a report, every report is represented in its phase packet, and all declared exclusions are either covered by another stage or explicitly accepted.
- Canonical artefacts have no unresolved duplicate IDs, contradictory dispositions, or cleanup work lacking an owner and dependency order.

## Final execution after the Phase 4 batch gate

The approved final execution pass is complete. The literal gate was captured; B001-B017 ran in dependency order and closed with independent approval; Stage 21 completed the available final builds, native tests, and two-range hygiene audits; canonical status was reconciled; and `merge-brief.md` was created. Unavailable live/manual/tooling scenarios are recorded as explicit limitations rather than silent passes. The review has stopped at the required final human merge decision.
