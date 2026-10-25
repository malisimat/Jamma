---
name: jamma-review
description: Review substantial Jamma changes for correctness, real-time safety, ownership boundaries, conventions, class growth and test quality using explicit Git scope and repeatable static checks. Use for requested quality reviews and before handing over substantial engine or shared-state changes.
---

# Jamma change review

Review the selected checkout without editing source unless fixes were requested. This skill does not authorize commits, PR publication or merging. Resolve every path from that checkout, including in a worktree.

## Establish the contract

Read `AGENTS.md`, `doc/glossary.md`, `doc/realtime-audio.md` and relevant subsystem guidance. They own the rules; this skill owns their review procedure. Turn the user's specific requests into explicit acceptance criteria. For example, "do not grow Scene" is a constraint to assess and report, not permission to invent a universal size cap.

Establish the intended target and candidate. For branch review, resolve the merge base with the intended target and record its full hash; do not assume a branch name. Run:

```powershell
python .agents/skills/jamma-review/scripts/review_changes.py --base <merge-base-sha> --head <candidate-sha>
```

Use `--working-tree` instead of `--head` when reviewing the current files, including staged, unstaged and untracked source. `--base HEAD --working-tree` intentionally reviews only local edits; it is not branch review. `--format json` returns the same evidence for automation. The script writes nothing. Python 3 and Git are required; report missing tooling rather than substituting an empty check.

Exit 0 means the checker completed without detected policy/whitespace violations, not that the change passed review. Exit 1 means those checks need attention; exit 2 means scope/tool/input failure. Review candidates alone do not fail the command. Regression checks for the checker itself run with `python -B -m unittest discover -s .agents/skills/jamma-review/tests -v`.

Read the reported scope, dirty-worktree notice, size measurements and candidates. An empty scope is not review approval. The report fingerprints inspected candidate content; rerun after relevant edits or HEAD drift. Committed review excludes dirty changes and explicitly lists that fact.

## Review the evidence

Use [references/review-checklist.md](references/review-checklist.md). For focused requests, perform the applicable areas and label omitted areas; full reviews cover all of them. Script candidates are prompts for inspection, not confirmed violations. Inspect surrounding code and affected callers, including unchanged helpers. Do not certify callback safety from keyword absence.

The script measures changed C++ files and header/implementation pairs, flags growth in Scene, Station and GuiHud, inventories changed test declarations, checks added anonymous namespaces and diff whitespace, and lists lock/wait, heap, I/O, shared-state and ownership candidates across all changed C++ files. It does not parse C++, prove reachability, measure function complexity, detect races or establish test quality.

Use existing configured compiler/static-analysis tools when relevant and available. Do not introduce a formatter configuration, new warnings baseline or whole-repository cleanup during review. Run affected builds/tests when requested or needed to resolve a finding, following local task/build guidance. For runtime application validation use the checkout's `jamma-self-validation` skill; distinguish that evidence from source review.

## Report

Return the scope (base, candidate/mode, fingerprint and local exclusions), user acceptance criteria, check results, findings and missing evidence. Save a report only when requested or required by the enclosing workflow. Each actionable finding needs severity, file/line evidence, a concrete consequence or violated rule, and the smallest suggested correction. Separate correctness defects, policy violations, design concerns and unverified candidates. Include meaningful test coverage losses, not just totals.

For growth, explain the added responsibility, its proper owner and whether an existing collaborator should absorb it. Report justified growth explicitly; do not force arbitrary extractions or reward moving lines into generic helpers. Recommend only tests that exercise a material behavior or identified risk. Record exact validation commands/results and unavailable or unperformed checks. Reassess changed areas after fixes; previous evidence applies only to its recorded candidate.
