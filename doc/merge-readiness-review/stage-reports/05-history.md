# Stage 05 — Git-history archaeology

## Assignment

- Stage: Phase 1, Stage 5 — Git-history archaeology.
- Primary ownership: branch intent; reversions; superseded experiments; fixup chains; feature merges; and files or approaches that changed direction.
- Explicit exclusions: independently declaring current code dead, unused, incorrect, unsafe, or poorly placed. Current reachability belongs to Stage 6; architecture, conventions, naming, runtime safety, real-time cost, correctness, test quality, and observability policy remain with their owning stages.
- Required inputs read in full before investigation: `AGENTS.md`; `doc/merge-readiness-review/merge-readiness-plan.md`; `doc/merge-readiness-review/phase-1-baseline-and-structure.md`; `doc/merge-readiness-review/00-scope-and-inventory.md`; `doc/merge-readiness-review/stage-reports/01-diff-inventory.md`; `doc/loop-alignment-and-ninjam-sync.md`; `doc/realtime-audio.md`; and `doc/build.md`.
- Comparison/range: current code and diff use `master...HEAD`; archaeology uses all 123 commits in `master..HEAD`, including both parents of the three merge commits.
- Output/single-writer boundary: this report is the only file written by Stage 5. Production code, tests, project/build files, canonical artifacts, the inventory, and other stage reports were not edited.

## Coverage

### History actually reviewed

- Reviewed the complete 123-commit topology and first-parent chronology from merge base `4941b780f7ff5a46f742167d79338e3ab592a565` through `HEAD` `e72f3b0f489cfa12ea697e966d3c0f619e31d1f6`.
- Inspected both-parent relationships and first-parent payloads for all three merges:
  - `04fa9e86f19e86489c1007e112d8a491ce4b05ad` has parents `44be0c87e37a3fe6ee5297d69ee14cf99376af45` and `f8a1cbbdc92b1d16c5c6f0634cc49a38f349827a`; its first-parent delta is 22 files, 1,269 insertions, and 16 deletions for the original transport-sync feature.
  - `24a43a05a53ecfb433452d5e4e517f47e43278aa` has parents `e3568a30b79d1d2efa3b3c6d686aeab5f24dcce9` and `5318b5491a8d9c47aea7449bc96e6005b100b650`; its first-parent delta is 56 files, 2,213 insertions, and 187 deletions for HUD/visual/resource work.
  - `880112d438e21983688395f60f970427d7f1d0b9` has parents `b9619b9b84cc1f46aeea134707cd83b3a8f36d83` and `70ba98c38cc0b4b2c5338efc3f5c2447abc0f85d`; its first-parent delta is 25 files, 2,079 insertions, and 347 deletions for VST3 parity plus resource/tooling work.
- Reviewed every branch addition/deletion/rename event to identify short-lived files and explicit direction changes. There is no commit with an explicit `Revert` subject. Supersession occurs through replacement/refactor commits and follow-up deletions instead.
- Traced the timing implementation through the original `ExternalTransport` branch, the coordinator/tracker refactor, the unified audio-boundary command, the three follow policies, local-loop alignment recovery, phase-map rebasing, source-coordinate mapping, and the final alignment fix.
- Inspected the HUD and VST3 second-parent lineages far enough to identify intentional feature payloads and cleaned-up experiments. The current layout or correctness of those features was not re-reviewed.
- Inspected current surrounding code only where needed to establish whether a historical construct still has a textual/current representation. Exact current references are included below; a reference search is evidence for a Stage 6 handoff, not a Stage 5 reachability verdict.

### Commands and queries used

- `git log --graph --decorate --date=short --format=... master..HEAD`, reverse full-parent chronology, and `git log --first-parent --reverse`.
- `git log --merges --format=... master..HEAD`; `git show -s --format=...` for exact merge parents.
- `git diff --shortstat <merge>^1 <merge>` and `git show --stat --summary --find-renames <commit>` for feature payloads and selected fixup chains.
- `git log --diff-filter=A|D --name-status master..HEAD` and `git log --find-renames --summary master..HEAD` for transient files, replacements, and renames.
- `git log -p`, `git show -p`, `git blame -L`, and `git log --follow` for the timing map, alignment diagnostics, `RemotePhaseDiscipline_Tests.cpp`, project membership, HUD resources, and VST3 cleanup.
- `rg -n` over current source/tests/docs for old timing types, phase/source-map fields, diagnostic receipts/logging, project/resource membership, and historical names.
- `git status --short` before report creation confirmed the only visible worktree entry was the shared untracked `doc/merge-readiness-review/` artifact tree.

### Deliberate exclusions

- No build or test run; this is a read-only intent/history stage.
- No compiler/reference proof of deadness and no judgment that a current path is safe, correct, or reachable.
- No independent architectural, naming, style, performance, or test-quality findings.
- No attempt to infer whether independently merged HUD, VST3, window-persistence, or tooling scope should ship; Stage 1 already owns those human scope decisions.

## System understanding

The branch is a sequence of increasingly explicit timing models rather than one implementation written once. The first transport-sync lineage (`1cba2d6` through second parent `f8a1cbb`, merged by `04fa9e8`) introduced `timing::ExternalTransport`, extended `TimingQuantiser`, and attached audio/MIDI anchors. The first-parent series `9c7ca51`–`42def9f` substantially rewrote that behavior, then `d0d208e` split canonical remote timing and the observation mailbox out of `NinjamNetworkService`. Commit `f36bfe7` completed the architectural replacement: it deleted `ExternalTransport` and its tests, reduced the old quantiser role, and introduced `NinjamTimingCoordinator` plus `NinjamTimingTracker`. Commit `d6fdb88` then established the single coherent `NinjamAudioTimingCommand` consumed at the audio boundary, including generation gating and invalidation; `b9619b9` added deliberately bounded job-thread telemetry and an integration harness.

Local-loop alignment was layered on later. `b41b5a8` first tried to make local loops follow an accepted remote position; `4c1c0e1` added normalized local transport offsets; `f74d4ec` repaired MIDI alignment; and `dcfaf1d`/`6abf7c7`/`b90f398` added recovery, UX, and extensive diagnostic snapshots. `a3f72b4` separated `ContinuousSync`, `BlockSync`, and `NoSync`. `e0f669c` made intentional relative loop offsets an explicit invariant, `1d665d2` introduced the remote-to-local `SyncPhaseMap`, `0d90bac` corrected map rebasing across accepted joins, `bef7943` changed the map origin from a wrapped phase to a monotonic source coordinate, and `e72f3b0` made phase derivation use that source coordinate as the single mapping implementation. This history supports the protected glossary: follow policy, master phase, mapped elapsed time, source coordinate, scene coordinate, and per-loop cursor were deliberately separated through bug-driven revisions, not accidentally duplicated.

The separate feature merges also contain real cleanup chains. In the HUD lineage, `034c3ca` temporarily added three `*_over_out`/red-over textures and `234d5d8` removed them; `2cb3fa9` intentionally removed old trigger rendering because HUD became its owner. In the VST3 lineage, `62fc990` introduced `Vst3ControllerEditQueue` and its tests, while immediate adversarial follow-up `e5081de` deleted that experiment before merge. `8714183` briefly introduced `NinjamMusicalTransport.h`; `e2dc3f8` replaced it in the next commit with `utils::MusicalTransport` and renamed `timing/TimingQuantiser` to `engine/Quantiser`. The repeated `Remove dev artefacts` commits deleted working plans and investigation HTML after implementation. Searches found no surviving reference to deleted `ExternalTransport`, old `timing/TimingQuantiser`, or `NinjamMusicalTransport` names in current source/docs/tests.

## Candidate findings

### S05-01 — Wrapped phase member survived the source-coordinate map replacement

- Stage / reviewer: Stage 5 — Git-history archaeology.
- Scope reviewed / exclusions: historical evolution and current textual representation of `SyncPhaseMap`; no independent declaration that the member is dead or that removing it is behaviorally safe.
- Severity: follow-up.
- Evidence: `1d665d25ba7eeb4e08aee962d8b21e8ef38467f6` introduced `SyncPhaseMap` with `SourcePhaseAtOrigin` as the active origin. `0d90bac8f5a248770b32f488aed6359125ce3c56` still passed that wrapped phase into station begin/rebase calls. `bef794355ec953799611380f40ed48518ac43928` deliberately changed those consumers to `SourceCoordinateAtOrigin` and added the monotonic coordinate while retaining `SourcePhaseAtOrigin`; `e72f3b0f489cfa12ea697e966d3c0f619e31d1f6` then documented `SourceCoordinateAt` as the single mapping implementation. Current code still declares `SourcePhaseAtOrigin` at `JammaLib/src/ninjam/NinjamLoopAlignment.h:54` and assigns it in `Rebase` at `JammaLib/src/ninjam/NinjamLoopAlignment.h:88`–`89`. Current phase derivation reads the coordinate at `JammaLib/src/ninjam/NinjamLoopAlignment.h:65`–`81`; current fan-out passes `SourceCoordinateAtOrigin` at `JammaLib/src/audio/AudioHost.cpp:313`–`316` and `356`–`364`. A repository-wide current reference query found no read of the `SourcePhaseAtOrigin` data member outside its declaration/comment/assignment.
- Why it matters: this is a precise residue candidate from the final phase-to-source-coordinate direction change. Retaining two origin representations can obscure which coordinate is authoritative, but Stage 5 does not treat the reference query alone as a reachability proof.
- Recommended disposition: remove only if Stage 6 confirms the member has no compiler, aggregate-initialization, reflection, serialization, or other current consumer; otherwise retain with a documented consumer. Keep `SourcePhaseAt(...)` as the derived wrapped view and preserve the monotonic source-coordinate model.
- Protected timing concepts affected: sync phase map, mapped elapsed time, source/scene anchor, master phase, and per-loop phase. Any cleanup must remove only duplicated stored representation, not collapse these concepts.
- Verification: Stage 6 compiler/reference evidence; relevant `NinjamLoopAlignment` and integration tests; confirm different loop lengths, intentional offsets, reconnect/rebase, and `NoSync` invalidation remain unchanged.
- Human decision: pending.

### S05-02 — Debugging-only alignment instrumentation remains without a recorded retention decision

- Stage / reviewer: Stage 5 — Git-history archaeology.
- Scope reviewed / exclusions: historical purpose and surviving diagnostic surface only; no judgment that it is unreachable, unsafe, too costly, or inappropriate observability.
- Severity: follow-up.
- Evidence: commit `dcfaf1d8003e4ec11d57452849adb82760e523a6` is explicitly titled `Added logs and description of loop alignment calculation, for debugging` and added 263 lines, including timing receipts and `Scene`/`Station` snapshots. `6abf7c702b8470622a58a6508ba1a9e413d2c17e` extended the alignment receipts, and `b90f3983230d90afc7991a81481c4758a5102566` is explicitly titled `Further debugging to ensure we push local tempo on joining remote empty session`. `e0f669c58b67b683d57e0a1c68aa3e22a108a560` later expanded before/after comparison while hardening the actual alignment invariant. The current branch retains the receipt type/API at `JammaLib/src/engine/LoopTake.h:93`–`109` and `252`, ten atomic receipt fields at `JammaLib/src/engine/LoopTake.h:420`–`430`, the receipt reader at `JammaLib/src/engine/LoopTake.cpp:691`–`714`, the scene logger at `JammaLib/src/engine/Scene.cpp:517`–`571` called at `JammaLib/src/engine/Scene.cpp:1395`, and verbose-gated station snapshots plus before-state storage at `JammaLib/src/engine/Station.cpp:2327` onward and `JammaLib/src/engine/Station.h:398`. This surface is historically distinct from the explicitly production-oriented, job-thread `NinjamTimingDiagnostics` telemetry added by `b9619b9b84cc1f46aeea134707cd83b3a8f36d83`, currently represented at `JammaLib/src/ninjam/NinjamTimingCoordinator.h:117` and `171`.
- Why it matters: the history labels this large cross-layer receipt/snapshot surface as debugging work, but later commits evolved it instead of either deleting it or recording it as a supported diagnostic contract. That ambiguity should be resolved deliberately rather than treating all timing telemetry alike.
- Recommended disposition: Stage 6 should classify which receipt/logging/state pieces are currently reached and whether any are duplicated by `NinjamTimingDiagnostics`; Stage 17 should decide the supported observability contract; Stage 8 should assess any callback-path cost. Retain with rationale and bounded contract where useful, otherwise simplify/remove the debugging-only residue.
- Protected timing concepts affected: local timing, remote timing, loop alignment, master phase, per-loop phase, sync phase map, and source/scene anchors. Diagnostic cleanup must not remove the state or ordering that performs alignment.
- Verification: reference/call-path evidence, verbose/non-verbose runtime behavior, real-time-path inspection, and focused remote-join/local-loop scenarios if later cleanup is approved.
- Human decision: pending.

## Handoffs

- Stage 6 (primary concrete residue): evaluate S05-01 first. The exact historical transition is `1d665d2` phase-origin map -> `0d90bac` phase-origin begin/rebase -> `bef7943` source-coordinate consumers -> `e72f3b0` single coordinate-derived mapping. Determine current reachability of only the stored `SourcePhaseAtOrigin` member; do not question the live `SourcePhaseAt(...)` derived operation or collapse phase and source coordinates.
- Stage 6 (diagnostic residue): classify the current `AlignmentReceipt`, atomic receipt fields, scene/station loggers, and `_ninjamBeforePositions` as live, dormant, duplicated, or dead. Historical evidence establishes debugging intent, not current reachability.
- Stage 8: if the diagnostic surface is live, inspect its publication/call sites against callback and hot-path rules. Stage 5 makes no cost or safety assertion.
- Stage 17: reconcile the debugging snapshot surface from `dcfaf1d`/`b90f398` with the bounded coordinator telemetry intentionally added by `b9619b9`; decide what operators/users are expected to consume.
- Stage 4: `test/JammaLib_Tests/src/timing/RemotePhaseDiscipline_Tests.cpp:1`–`9` now tests `engine::Quantiser` after `e2dc3f8` moved/renamed the implementation. Its directory/filename reflects the older timing layout, but naming disposition belongs to Stage 4 rather than this stage.
- Later timing/correctness stages: treat `f36bfe7`/`d6fdb88` as the ownership pivot from `ExternalTransport` to coordinator/tracker plus a unified audio command; treat `a3f72b4` as the explicit three-policy pivot; and treat `bef7943`/`e72f3b0` as the authoritative source-coordinate pivot. Earlier phase-only designs are intent history, not a basis for restoring a shared cursor.
- Phase 1 integrator: Stage 1's HUD, VST3, window-persistence, and tooling scope candidates are historically confirmed. The HUD and VST3 changes arrived through independent second-parent feature lineages; this stage found their most obvious short-lived experiments already deleted, but makes no current-reachability or retain/move decision.

## Uncertainties

- Git history records what changed and many commit subjects record why, but it cannot prove that a debugging facility is no longer operationally useful. S05-02 therefore asks for an explicit retention decision rather than declaring the surface stale.
- The current reference query for `SourcePhaseAtOrigin` is strong textual evidence but not compiler/linker or aggregate-initialization proof. Stage 6 owns the final reachability classification for S05-01.
- Merge first-parent deltas describe what entered each then-current parent; they are not additive slices of the final 199-file diff because later commits overlap and revise those files.
- Deleted working plans were inspected through history as intent sources. Their deletion by `Remove dev artefacts`/`Remove plans no longer needed` commits is itself evidence that they are not current contracts; the maintained protected timing document governs current semantics.
- The archaeology did not attempt to validate commit-body claims such as test counts, hot-path audit results, or runtime success. Those are historical intent/evidence notes for later verification, not present-tense verification.

## Conclusion

Stage 5 covered all 123 commits and all merge parents at topology/change-direction level, then traced the main timing, HUD, VST3, resource, tooling, and diagnostic fixup chains far enough to distinguish superseded experiments from the current design. The branch's timing architecture intentionally evolved from `ExternalTransport` and phase-only correction into coordinator/tracker ownership, one audio-boundary command, three follow policies, per-entity anchors, and a source-coordinate sync map. The protected distinctions are history-backed and must not be collapsed.

Two commit-backed residue candidates remain for downstream judgment: the stored wrapped `SourcePhaseAtOrigin` left after the source-coordinate rewrite (S05-01), and the debugging-origin alignment receipt/snapshot surface without an explicit retention decision (S05-02). Both are handed to Stage 6 without a current dead-code verdict; S05-02 also goes to the later real-time and observability owners. No production code, test, build file, canonical artifact, inventory, unrelated working-tree change, or other report was edited.
