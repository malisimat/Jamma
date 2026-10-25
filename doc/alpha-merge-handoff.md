# Alpha merge handoff — 2026-10-25

## Source and scope

Destination: `codex/trigger-learn` in `C:/Users/matto/Source/Repos/Jamma`.
Prepared branch: `codex/alpha-readiness` in `Jamma.worktrees/Jamma-alpha-readiness`.
Original shared baseline: `80384c8c`. Candidate previously reviewed and tested:
`25c3637b`, followed by documentation commit `478d58d6`.
Merge `4ff8c9e0` incorporates destination tip `7466e85a` (and skill commit
`4b056696`). It merged without textual conflicts. The destination is now an
ancestor of this branch, allowing a fast-forward if it has not advanced again.

The combined changes provide continuous rotated diagnostics including CRT output,
shared control/routing presentation repairs, UI runtime dialogs and deferred VST
confirmation validation, responsive selection/Inputs/settings layout, save and
window-title corrections, embedded app icons and test-source cleanup. Destination
selection-depth art, all six texture states per depth, layout padding and tracked
validation helpers are preserved.

## Review and fixes

Reviewed the production delta against `7466e85a`, the automatically merged
selection layout against both parents, resource/project wiring, regression and
fixture changes, and prior independent full-candidate review/disposition evidence
under `artifacts/workflow/2026-10-25-alpha-readiness/`. No additional actionable app
code finding remained. This integration review was performed by the coordinating
agent; it does not claim a new independent reviewer.

Ran the threading audit and a copy configured to inspect the actual full
`80384c8c..HEAD` delta. New locks are confined to off-callback notice transfer and
UI rack operations. Runtime dialog state/tokens and layout/title caches remain
UI-owned; workers transfer detached notices, then join before presenter teardown.
CRT reader lifetime is bounded by owner shutdown/join before capture detaches.
Rig save results use the existing atomic immutable completion and Scene identity
check. Audio processing files are unchanged, and Scene callback bodies are
unchanged. No new callback locks, allocation, logging or I/O were introduced.

The main checkout's uncommitted fallback skill and Python helper were incorporated
here. Fixed its exact-title filter so saved sessions (`Jamma - <filename>`) are
accepted, while requiring the `JammaWindow` class and requested PID. Mock checks
cover saved/unsaved titles, wrong class/PID, hidden windows and ambiguity; syntax
parses. No live capture or interaction is claimed for this helper revision.

Corrected stale checklist statements about runtime Load Rig (compatible edits are
supported; device changes require restart), and removed obsolete separate-console
build/runtime requirements. Historical candidate hashes/results are explicitly
distinguished from this merged candidate.

## Verification

Fresh integration evidence is in `artifacts/merge-preparation/` in the prepared
worktree. These local artifacts are not part of the merge.

- Debug x64 incremental solution Build: passed, zero warnings/errors.
- Complete Debug native suite: 1,921 passed; zero failures/disabled/errors.
- Release x64 incremental solution Build: passed, zero warnings/errors.
- Complete Release native suite: 1,915 passed; zero failures/disabled/errors.
  Six existing tests are Debug-only.
- All 18 selection-depth texture states exist, are registered and have 32-bit TGA
  headers. Every deployed Release resource outside the source-art directory
  matches its source hash.
- Fallback syntax/window-discovery mock checks and Git whitespace check passed.

Both builds and native suites used merged app code at `4ff8c9e0`. Subsequent
changes affect only the validation skill/helper and documentation. Release
executable: `Jamma/bin/x64/Release/Jamma.exe`, SHA-256
`10aabecb5fa4f9e053e0bce4a195700f1649979ab22c6fdea780d89d0690a0ee`.
The branch's final handoff commit contains this report; use `git rev-parse
codex/alpha-readiness` to resolve it rather than the earlier merge commit.

No new runtime acceptance was attempted. The main checkout's recovery-5 capture
and manifests identify the old `d75e85b1...` Release binary at `478d58d6`; inspecting
that image confirms a rendered historical frame only. Its inspection manifest
still says visual review pending. Do not reuse it as merged-candidate evidence.

## Destination local files

The main checkout was inspected read-only and left untouched:

| Local item | Disposition |
| --- | --- |
| `doc/surge-scrape-output-plan.md` (untracked) | Initially identical to this branch's tracked plan. This branch now adds current execution status and corrects the console references. |
| `doc/alpha-manual-checklist.md` (untracked) | Older requirements-only checklist. This branch's tracked checklist includes historical actual results and current handoff references. |
| `.agents/skills/jamma-self-validation/SKILL.md` (modified) | Incorporated here. |
| `.agents/skills/jamma-self-validation/scripts/capture-window-fallback.py` (untracked) | Incorporated here with the saved-title discovery fix. |
| `artifacts/computer-use-recovery*` (untracked) | Local historical evidence; retain locally, do not commit or overwrite. |

Before merging in the main session, inspect status again. Back up the four source
files above and their diff to a fresh local evidence directory. Compare any edits
made since this inventory and preserve/reconcile them before proceeding. Only
after the known skill modification is backed up and confirmed incorporated, restore
that tracked file to destination HEAD. Move the three untracked source/doc files
to the backup so they cannot block incoming tracked files. Leave all other local
work and runtime artifacts alone. Then use `git merge --ff-only codex/alpha-readiness`.
If the destination advanced, stop the fast-forward attempt and integrate its new
commits on the prepared branch first; do not reset or force it.

## Work remaining after merge

The plan's implementation and automated candidate work is delivered. Alpha
acceptance is still incomplete. Remaining work is the manual checklist: interaction
and resize/collapse under capture; recording/playback/Overdub/undo and listening;
real VST2/VST3/editor/recovery and two-instrument note/sustain routing/removal;
JAM/rig round trip, defaults, Unicode title and paired restart; unavailable devices,
offline behavior, runtime failure dialogs/logs, latency/NINJAM and shell icons;
and orderly shutdown. The fallback capture helper also needs a fresh live check
against the exact merged binary, including a named session window.

Rebuild from the main checkout using its own `.vscode/tasks.json`, verify resource
deployment, and generate a fresh validation build record before runtime work.
Worktree outputs, test XML, build records and runtime evidence do not travel through
Git. Preserve live defaults/sessions and other Jamma processes. Record actual
results in the checklist and fix concrete failures found during acceptance.
Do not mark manual rows passed merely because native tests pass. No known app
implementation fix is deferred to the destination; newly discovered failures
remain possible. No publishing, tagging or master merge is part of this handoff.
