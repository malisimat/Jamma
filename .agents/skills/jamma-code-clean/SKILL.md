---
name: jamma-code-clean
description: Audit Jamma code growth, large owners and test quality against a previous snapshot; produce evidence-backed cleanup proposals without changing production code. Use for the weekly audit or an on-demand structural cleanup review.
---

# Jamma code-clean audit

Audit the explicitly selected, verified Jamma checkout. This skill is read-only for Jamma source and tests. A proposal is a later surge handoff, not permission to implement it. The weekly automation uses this repo-local skill, not the older global `code-clean` skill.

Read `AGENTS.md`, `doc/glossary.md`, `doc/realtime-audio.md`, relevant subsystem guidance and [references/review-criteria.md](references/review-criteria.md). The glossary decides ownership; the reference gives the strict audit questions. Verify `Jamma.sln`, the Git root, branch, full HEAD and dirty status. Never assume `master` is current. If the checkout is a branch/worktree the user did not intend, report that scope; do not silently switch branches.

Run from the selected checkout, passing its verified root explicitly (replace the sample path for another worktree):

```powershell
python .agents/skills/jamma-code-clean/scripts/structure_audit.py --repo C:\Users\matto\Source\Repos\Jamma --previous <previous-snapshot.json> --snapshot <new-snapshot.json>
python .agents/skills/jamma-code-clean/scripts/test_inventory.py --repo C:\Users\matto\Source\Repos\Jamma
```

`--previous` is optional on the first run. Snapshot paths must be outside the repository. The structure script counts physical lines in tracked first-party C/C++ source and headers (`Jamma/src`, `Jamma/include`, `Jamma/Resource.h`, `JammaLib/src`, `JammaLib/include`, and native tests), including blanks/comments. It excludes vendored `JammaLib/lib`, generated/dependency/build output and untracked files. The report lists the current top 20 in descending order with prior count and delta, branch/commit, large production files, growth candidates, and Git change/static candidates. Treat all thresholds and matches as prompts for code inspection, never automatic findings. When the previous snapshot is from another branch, show the numeric comparison with a cross-branch caveat and use a deliberate merge-base analysis before attributing growth to recent work. When no previous snapshot exists, state that week-over-week deltas are unavailable; never fabricate them. Save the new snapshot to automation memory only after the scan succeeds, preserving the previous snapshot until comparison completes.

The test inventory lists active tracked native test declarations, approximate assertion counts, duplicate-body candidates and presentation assertion candidates, including assertions inside useful interaction tests. Verify compile inclusion in `test/JammaLib_Tests/JammaLib_Tests.vcxproj` before calling anything redundant or weak. A zero direct-assertion count may hide helper assertions. Inspect candidate source and behavior; don't infer test quality from counts.

For new committed code, also use the repo's existing `.agents/skills/jamma-review/scripts/review_changes.py` with the previous snapshot commit as `--base` and current HEAD as `--head` when the former is an ancestor. If branches diverged, resolve the merge base deliberately and label the narrower scope. For relevant dirty source/test files, run its `--base HEAD --working-tree` mode separately. This adds repeatable header-pair growth, anonymous-namespace, whitespace and callback-risk candidates. Its successful exit is not a quality approval; inspect the cited code and callers.

Look past file size: trace duties, callers, state, lock/thread ownership, dependencies and repeated policy. For a genuine god class, name a concrete responsibility to move, its existing or proposed owner and narrow interface, the calls/state that disappear from the large owner, required ordering, and behavior tests that protect the seam. Try to delete branches, duplicate state or pass-through layers rather than just moving lines. Compare recent Git changes for new conditionals, modes, helpers, cross-owner logic, header/interface growth and tests that mirror implementation. A file crossing 1,000 lines or growing sharply deserves inspection, not mandatory splitting. Do not recommend extraction that makes callback work unbounded, allocates, waits, locks, or violates Station -> LoopTake -> Loop and Scene/AudioHost ownership.

Explicitly consider removal of cosmetic pixel position, dimension, symmetry, padding and font assertions that merely restate implementation. Preserve evidence-backed clipping, reachability, hit-testing, text bounds and interaction checks. For each proposed test removal/merge, state current behavior, retained protection and later verification. Cosmetic-only cuts may need visual review instead of a replacement automated geometry test.

Compare proposed findings with the previous audit memory and source snapshot, suppressing unchanged findings even if unrelated commits landed. Do not list, read, triage, consolidate or deduplicate Synced ideas/proposals. If genuinely new evidence warrants a handoff, write exactly one dated `YYYY-MM-DD-jamma-code-clean-<topic>.md` to the verified Synced destination without reading its contents or overwriting a file; if unavailable, keep the draft outside Jamma and report the blocker. Include scope, top-20 table, prior comparison, ranked evidence, boundary-respecting removals/moves, retained tests, risks and explicit exclusions. If no new actionable evidence exists, the scheduled run stays quiet. Never edit Jamma production or tests during the audit.
