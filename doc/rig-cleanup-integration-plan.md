# Rig cleanup integration handoff

Prepared 16 October 2026. Planning only: no merge, build, test, converter run or
profile write was performed in this session. Execute this plan in a fresh
session and leave a clean integration branch and runnable build for user testing.

## Scope and inspected state

- Target: `codex/integrate-ready-features` at `f6d4c939`, in
  `C:/Users/matto/Source/Repos/Jamma.worktrees/Jamma-integrate-ready-features`.
- Source: `feature/rig-model-cleanup` at `5d7c2859`, in
  `C:/Users/matto/Source/Repos/Jamma.worktrees/Jamma-rig-model-cleanup`.
  Both worktrees were clean. Common ancestor: `cff00825`.
- Source brings four commits and 37 changed files: canonical rig `2.0`, explicit
  device registry IDs, independent activate/ditch press/release bindings,
  separate audio/MIDI capture selections, validation/normalization diagnostics,
  routing/snapshot/runtime adaptations, timing-field changes and test updates.
  Establish intended semantics from code/history before preserving or changing them.
- Source startup points at `%APPDATA%/Jamma-rig-cleanup/defaults.json`; target
  correctly uses `%APPDATA%/Jamma/defaults.json`. The final app must use **Jamma**,
  with all selected upgraded rigs there and no accidental alternate-profile dependency.
- Converter was deleted by `5d7c2859`. Recover and review
  `tools/rig-migrator/{migrate-rig.ps1,migrate-rig.cmd,run-all.cmd}` from
  `09d20195`; do not revert that commit wholesale or invent a replacement migration.
- A06 in `C:/Users/matto/Desktop/Jamma-alpha-fixes-plan.md` is inspiration only.
  Implement no other items from that plan. Do not push or merge/promote master.
- User has now tested the integrated native console by launching the Debug exe
  through Explorer and confirms it works well and looks good. Preserve that result.

## Ordered execution

1. **Re-establish baseline and intention.** Read `AGENTS.md`, `doc/glossary.md`,
   `doc/realtime-audio.md`, source rig documentation and the four source commits.
   Recheck both statuses/tips and preserve unrelated work. Read local
   `.vscode/tasks.json` before every build/test; follow `doc/build.md` with
   incremental affected-project builds and absolute `SolutionDir`. Record target
   baseline tests (previous result: 1687 Debug / 1681 Release). Explain the intended
   schema, registry identity, binding priority/overlap, missing-device handling,
   capture subscriptions and save behavior in a short review record.

2. **Preview and reconcile against current integration.** Use read-only
   `git merge-tree <base> <target> <source>` (installed Git lacks `--write-tree`).
   At inspected tips, textual conflicts appear in `RigCoordinator.cpp`,
   `RigSnapshot.h`, `Scene.cpp`, `Trigger.cpp`, `IoInputSubsystem.h`,
   `MidiDevice.h` and `MidiRouter.cpp`. Recompute this list; a clean textual merge
   does not establish semantic compatibility. Review all changed files, especially
   Main startup/save/load, AudioHost, Scene lifecycle, published immutable snapshots,
   device refresh and trigger ownership. Preserve the target's newer coordination,
   device/session behavior and console. Keep the canonical Jamma profile path.
   Integrate on a temporary `codex/` preparation branch based on the target so
   conflict resolution and fixes can be tested before updating the target branch.

3. **Prepare missing tests and shake out obsolete code.** Preserve useful tests
   from both branches, then add focused regressions for uncovered behavior:
   strict 2.0 parsing/round trips, malformed/unsupported rigs without silent
   replacement, registry IDs versus hardware names, unavailable/reopened devices,
   first-run input subsets, binding order/overlap/capacity, paired releases,
   capture routing and save/reload after session replacement. Verify
   `preDelay` -> `preDelaySamps` and `debounceSamps` -> `debounceMs` intentionally:
   the historical converter copies the debounce number; confirm its actual old
   units rather than inferring a numerical conversion from the old field name.
   Test converter parity against the real C++ parser, dry-run, repeated runs,
   collisions, unsupported files, interrupted transaction recovery and rollback.
   Remove only proven dead legacy rig structures, parsing paths, references and
   fixtures; update project entries/docs. Keep the migration utility available.
   Review shared JSON changes against JAM/defaults consumers. Keep test fixtures
   worktree-local; never let tests write live Roaming. Apply the threading-review
   skill for cross-thread changes and independently review the final combined diff.

4. **Mandatory backup and staged conversion, before live publication.** Close
   every Jamma process before backup/conversion; retain the pre-upgrade runnable
   build and matching profile checkpoint. Inventory both profiles recursively,
   defaults-selected rigs, and any externally referenced rig files. Observed:
   normal profile has two `.rig` files and five JAMs; alternate has two live rigs,
   two archival rig copies in backup subdirectories, and seven JAMs. Reinventory;
   those counts are not a completion checklist. Back up **every rig original**,
   including archival copies and external references, plus complete profiles,
   defaults and relevant external assets before changing any file. Use a unique
   checkpoint outside both profiles, for example
   `C:/Users/matto/AppData/Local/Jamma-rig-integration/<timestamp>/`, with a
   path/size/SHA256 manifest; verify all backup hashes and protect the originals.
   Record which normal/alternate files are authoritative individually.

   Restore the reviewed utility in the preparation branch. Use explicit absolute
   paths on every invocation: `migrate-rig.ps1 -SourceDirectory <backup-profile>
   -DestinationDirectory <separate-staging-directory> -DryRun`, then the same
   command without `-DryRun`. Convert normal and alternate copies separately;
   use `-SourceFile` for external rigs where appropriate. Source/destination must
   be distinct, non-overlapping directories. Never use the historical defaults
   (Jamma -> Jamma-rig-cleanup). Prefer rigs-only mode: `-CopyProfile` supports only
   compatible empty-take sessions/defaults, not general recorded JAM/media migration.
   Inspect every converter warning, including removed overlapping bindings.
   Account for skipped backup files explicitly: preserve archival originals;
   convert a separate copy if an archived rig is selected for active use.

5. **Validate and publish into Roaming/Jamma.** Load every staged active rig with
   the actual merged C++ parser and compare devices, controls, targets, capture,
   timing/settings and paths to its original. Resolve same-name/different-content
   rigs explicitly, preserving both under unique names where needed. Assemble
   a validated canonical-rigs source outside the live profile, then dry-run and
   run the recovered utility with that source and destination
   `C:/Users/matto/AppData/Roaming/Jamma` in rigs-only mode. Preserve destination
   transaction journals/backups and verify output hashes/parser acceptance.
   Reconcile defaults-selected rig paths last. Preserve existing JAMs, recordings,
   media and settings; copy any selected newer alternate-profile work separately
   without assuming converter support or overwriting divergent files. Ensure
   intended rig/default paths no longer depend on Jamma-rig-cleanup. Retain the
   alternate directory and all backups. On validation/publication failure, stop
   cutover, recover the converter transaction and restore affected files from
   verified backups with Jamma closed; never erase new recordings during rollback.
   Legacy builds must use their matching legacy checkpoint, not upgraded rigs.

6. **Merge and verify the delivered result.** Run relevant native suites and
   incremental x64 Debug/Release engine, tests and app builds; inspect diagnostics
   for migration/startup failures without expanding into A05 logging work. Review
   the resolved diff for callback allocations/locks and obsolete rig paths.
   Merge the reviewed preparation branch into `codex/integrate-ready-features`,
   then rerun combined builds/full native suites and verify a clean working tree.
   Smoke-check canonical startup and rig save/reload without a running app racing
   the converter. Leave the final app stopped or ready for the user's hardware
   tests; do not claim hardware acceptance from unit tests. Commit a concise
   handoff with merge/fix SHAs, test totals/logs, executable path/hash, migration
   source/output/backup manifest locations and rollback instructions.

## Completion gate

Complete only when the branch is cleanly merged, reviewed and tested, **all rigs
are backed up and every selected active rig is converted/validated in
Roaming/Jamma**, canonical launch is verified, and the user has a runnable build
with a short checklist for MIDI/serial activate/ditch, capture, device absence,
session load, settings and save/reload. Unsupported migrations or unresolved
files are blockers to completion, not silent omissions. Master remains unchanged.
