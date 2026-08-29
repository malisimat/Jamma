# Phase 1 — Baseline, Intent, and Structural Review

## Goal and human gate

Establish what changed, why it changed, and whether it is located and shaped consistently with Jamma architecture. Human gate: approve the inventory, protected-concepts glossary, and proposed structural findings before they can enter Phase 4 batching.

## Outputs

- Populate `00-scope-and-inventory.md` with a subsystem/file heat map, added-line concentration, public API changes, hot-path entry points, generated/binary assets, and commit-to-feature map.
- Add structural, naming, historical, and stale-code findings to `findings.md`.
- Add agreed terms to `decisions.md`; the glossary must distinguish sync modes, sync phase/source coordinates, local timing, remote timing, remote join, and loop alignment.

## Independent review stages

1. **Diff and ownership inventory** (`stage-reports/01-diff-inventory.md`). **Primary ownership:** objective branch scope, diff/commit statistics, subsystem partition, high-churn files, public API surface, and suspected accidental scope growth. **Excludes:** judging layout or code quality. **Required handoff:** a stable file/subsystem inventory used by stages 2–6 and later phases.
2. **Logical layout** (`stage-reports/02-logical-layout.md`). **Primary ownership:** architectural placement, dependency direction, layer leakage, and duplication of subsystem ownership, particularly across `audio`, `engine`, `ninjam`, `midi`, and `utils`. **Excludes:** naming/style and proving runtime correctness. **Required handoff:** boundary concerns with the current owner and proposed owner identified.
3. **Conventions** (`stage-reports/03-conventions.md`). **Primary ownership:** objective `AGENTS.md` and neighboring-style violations: anonymous namespaces, header/implementation placement, naming-form consistency (not domain meaning), RAII/value semantics, and hidden mutable globals. **Excludes:** architecture, vocabulary semantics, and dead-code reachability. **Required handoff:** rule citation plus current-line evidence for every candidate.
4. **Naming and domain vocabulary** (`stage-reports/04-vocabulary.md`). **Primary ownership:** whether names communicate coordinate system, authority, lifetime, thread ownership, and the approved timing glossary. **Excludes:** general formatting/style and proposals to merge protected concepts. **Required handoff:** a rename table of old name, proposed name, semantic defect, and protected distinction retained.
5. **Git-history archaeology** (`stage-reports/05-history.md`). **Primary ownership:** branch intent, reversions, superseded experiments, fixup chains, and files that changed direction. **Excludes:** independently declaring current code dead or incorrect. **Required handoff:** commit-backed residue candidates for stage 6 and intent notes for all later stages.
6. **Stale/dead-code sweep** (`stage-reports/06-stale-code.md`). **Primary ownership:** current reachability and redundancy of unused types, compatibility paths, helpers, instrumentation, impossible branches, old names, and assets. Consume stage 5's historical candidates instead of repeating archaeology. **Excludes:** broad simplification of live abstractions (stage 13). **Required handoff:** reference/compiler evidence and a clear distinction between dead, dormant, duplicated, and merely complex code.

## Reviewer prompts

- Is each new timing concept owned by one layer and translated deliberately at boundaries?
- Are public interfaces smaller than they need to be, or exposing implementation details?
- Did the GUI/VST/resource work enter this branch for an intentional, documented reason?
- Can a deletion or move retain all the protected timing concepts while reducing code?

## Human review packet

The integrator writes `phase-packets/phase-1.md` and presents: coverage against the inventory, top added-code areas, proposed boundaries, glossary, historical residues, naming changes ranked by value/risk, cross-stage handoffs, and a list of deletions/moves that might later be batched. Human reviewers approve, reject, or defer the baseline and proposed findings in `decisions.md`; approval does not yet authorize source edits.
