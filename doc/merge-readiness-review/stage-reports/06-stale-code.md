# Stage 06 — Stale/dead-code sweep

## Assignment

- Stage: Phase 1, Stage 6 — Stale/dead-code sweep.
- Primary ownership: current reachability and redundancy of unused types, compatibility paths, helpers, instrumentation, impossible branches, old names, and assets; distinguish dead, dormant, duplicated, and merely complex code.
- Explicit exclusions: broad simplification of live abstractions (Stage 13); architecture, naming, and style judgments; runtime correctness or performance; and independent history archaeology.
- Required inputs read in full before investigation: `AGENTS.md`; `doc/merge-readiness-review/merge-readiness-plan.md`; `doc/merge-readiness-review/phase-1-baseline-and-structure.md`; `doc/merge-readiness-review/00-scope-and-inventory.md`; `doc/merge-readiness-review/stage-reports/01-diff-inventory.md`; `doc/merge-readiness-review/stage-reports/05-history.md`; `doc/loop-alignment-and-ninjam-sync.md`; `doc/realtime-audio.md`; and `doc/build.md`.
- Comparison: `master...HEAD`. Stage 5's commit-backed residue candidates S05-01 and S05-02 were the starting point; history was queried only to identify the introducing commit for a newly confirmed asset residue.
- Output/single-writer boundary: this report is the only file written by Stage 6. Production code, tests, build/project files, canonical artifacts, the inventory, unrelated worktree changes, and other reports were not edited.

## Coverage

### Classification rules used

- **Dead:** current program behavior cannot reach or consume the item under the repository's code, registry, and project model.
- **Dormant:** a current call path exists but is disabled by configuration or activated only in a specific operational mode.
- **Duplicated:** the information is still produced/stored, but every current consumer derives or reads the same authority elsewhere.
- **Merely complex:** the path has several related fields/helpers, but each has a current consumer or preserves a protected timing distinction; complexity alone is not evidence of staleness.

### Current code and assets actually covered

- Fully traced Stage 5 candidate S05-01 through `ninjam::SyncPhaseMap`, `AudioHost`, station/take fan-out, and the current NINJAM alignment tests.
- Fully traced Stage 5 candidate S05-02 through `LoopTake::AlignmentReceipt`, all eleven receipt atomics, `LastAlignmentReceipt`, `Station` before/after snapshots, `Scene` logging call sites, and the separate `NinjamTimingDiagnostics` surface.
- Searched current `Jamma`, `JammaLib`, tests, and maintained docs for the superseded names identified by Stage 5: `ExternalTransport`, `timing/TimingQuantiser`, `NinjamMusicalTransport`, `Vst3ControllerEditQueue`, and `GuiPopupHost`.
- Reviewed every branch-added resource name against `Jamma/resources/ResourceList.txt`, code string consumers, the resource loader, and the app project's wildcard resource-copy rule. This covered all 19 added TGA files and all four added shaders, not just the dead candidates.
- Used the complete Stage 1 changed-file inventory to search branch-added/current declarations, compatibility/debug markers, helper names, and reference counts across audio, engine, NINJAM, MIDI, GUI/resources, VST, I/O, utilities, app wiring, and native tests. Deep semantic re-review was bounded to concrete low-reference or history-backed candidates; no claim is made that every function body in the 199-file diff received an independent control-flow proof.

### Commands and queries used

- `git diff --name-status|--name-only master...HEAD`; `git diff --unified=0 master...HEAD -- <candidate paths>`.
- `rg -n` repository-wide reference searches for `SourcePhaseAtOrigin`, `SourceCoordinateAtOrigin`, `SourcePhaseAt`, sync-map APIs, receipt types/fields/readers/writers, alignment logging, diagnostics, old names, resource names, project entries, compatibility/debug/TODO markers, and branch-added types/helpers.
- Numbered `Get-Content` inspection of surrounding current code in `NinjamLoopAlignment.h`, `AudioHost.cpp`, `LoopTake.*`, `Station.*`, `Scene.cpp`, `NinjamTimingCoordinator.*`, `NinjamNetworkService.h`, `ResourceLib.cpp`, `GuiHud.cpp`, `ResourceList.txt`, and project files.
- `Get-ChildItem`/`Get-Item` for exact added-asset names and byte sizes.
- `git log --format=... --diff-filter=A -- <dead asset paths>` and `git show --stat 5ae751e -- <HUD resource paths>` only to attach the relevant introducing commit after current deadness was established.

No build or native-test run was performed. Investigation is read-only, builds would create output/intermediate state, and the inherited shell has no directly discoverable `cl`, `clang-cl`, or `clang++`. Findings therefore use current reference, initialization, registry, and project evidence and explicitly request compile/test verification for any later approved deletion.

## System understanding

The current timing implementation has one active source-coordinate authority. `SyncPhaseMap::SourceCoordinateAtOrigin` is advanced by `SourceCoordinateAt(...)`; the wrapped source phase is derived on demand by `SourcePhaseAt(...)`; and `AudioHost` passes the coordinate origin, not the stored wrapped phase, into station/take begin and rebase calls. The map, source coordinate, wrapped master phase, monotonic scene coordinate, and per-loop cursors remain distinct live concepts. Only the extra stored `SourcePhaseAtOrigin` value is duplicated.

The alignment diagnostics are not one uniformly stale subsystem. `Scene::_LogAppliedNinjamLoopAlignment` has a live job-thread call at `JammaLib/src/engine/Scene.cpp:1395`, and it calls `Station::LogLocalLoopAlignment` for local stations at `JammaLib/src/engine/Scene.cpp:566`–`570`. Before-command snapshots are captured at `JammaLib/src/engine/Scene.cpp:469`–`480`. `Station::_LogLocalLoopAlignment` and `_ninjamBeforePositions` are therefore **dormant**, not dead: both execute when audio logging is `verbose` (`JammaLib/src/engine/Station.cpp:2327`–`2344`) and the vector supplies before/after comparisons at `JammaLib/src/engine/Station.cpp:2411`–`2425` and `2452`–`2464`. The coordinator diagnostics are also live: `Scene` reads tempo-request counters at `JammaLib/src/engine/Scene.cpp:507`–`514`, and native tests inspect additional counters. Their overlap is diagnostic subject matter, not current implementation duplication proved by this stage.

Inside that live verbose logger, however, the `AlignmentReceipt` subpath is dead. All receipt atomics retain their zero initializers, no current writer/store/fetch operation exists, and the only reader returns `nullopt` when the sequence is zero. Consequently the receipt-printing branch can never execute. This is separate from deciding whether the surrounding snapshot logger should be a supported observability feature.

The resource system loads texture/shader names enumerated in `ResourceList.txt`: `ResourceLib::LoadResource` constructs a texture path from the supplied registered name at `JammaLib/src/resources/ResourceLib.cpp:28`–`58`. The app project copies the whole resource tree through `Jamma/Jamma.vcxproj:200`–`202`, so unregistered files still inflate the runtime payload. The live HUD uses the neutral `trigger_back` and the activate/ditch variants at `JammaLib/src/gui/GuiHud.cpp:672`–`710`; ten added green/red back variants have neither a registry entry nor any current textual consumer.

The protected timing structures that remain multi-field or multi-layered were classified as merely complex where their values have current callers. In particular, `SourcePhaseAt(...)`, `SourceCoordinateAt(...)`, per-take source-coordinate state, scene anchors, mapped elapsed time, and the three follow policies are live and must not be collapsed as part of stale-code cleanup.

## Candidate findings

### S06-01 — Remove the duplicated stored `SourcePhaseAtOrigin` member, not the derived phase operation

- Stage / reviewer: Stage 6 — Stale/dead-code sweep; confirms Stage 5 candidate S05-01.
- Scope reviewed / exclusions: current references, initialization, containing-object use, and tests around `SyncPhaseMap`; no timing-correctness redesign or simplification of protected concepts.
- Severity: follow-up.
- Evidence: `SourcePhaseAtOrigin` is declared at `JammaLib/src/ninjam/NinjamLoopAlignment.h:54` and assigned only by `Rebase` at `JammaLib/src/ninjam/NinjamLoopAlignment.h:84`–`90`. Repository-wide current reference search finds no read of that member. `SourcePhaseAt(...)` instead derives its result from `SourceCoordinateAt(...)` at `JammaLib/src/ninjam/NinjamLoopAlignment.h:65`–`81`. The sole containing production object is the default-initialized `AudioHost::_syncPhaseMap` at `JammaLib/src/audio/AudioHost.h:144`; current fan-out passes `SourceCoordinateAtOrigin` at `JammaLib/src/audio/AudioHost.cpp:355`–`364`. This is the exact residue of the Stage 5 history chain `1d665d2` -> `0d90bac` -> `bef7943` -> `e72f3b0`.
- Why it matters: storing a wrapped origin beside the authoritative monotonic coordinate suggests two authorities and increases the chance that future code accidentally revives the superseded phase-origin model.
- Recommended disposition: remove the `SourcePhaseAtOrigin` member, its stale explanatory comparison, and the assignment in `Rebase`; retain `SourcePhaseAt(...)`, `SourceCoordinateAtOrigin`, `SourceCoordinateAt(...)`, and all scene/per-entity anchors.
- Protected timing concepts affected: sync phase map, mapped elapsed time, source/scene anchor, master phase, and per-loop phase. The deletion removes only duplicated storage and must not merge these concepts.
- Verification: compile `JammaLib` and native tests after an approved cleanup; run the `NinjamLoopAlignment`, sync-map rebase, unequal-loop-length, intentional-offset, reconnect/rebase, and `NoSync` invalidation coverage.
- Human decision: pending.

### S06-02 — `AlignmentReceipt` is never published, making its reader and logger branch dead

- Stage / reviewer: Stage 6 — Stale/dead-code sweep; refines Stage 5 candidate S05-02.
- Scope reviewed / exclusions: receipt reachability versus surrounding snapshot/telemetry reachability; no judgment of callback cost or the long-term observability contract.
- Severity: follow-up.
- Evidence: the receipt type and API are at `JammaLib/src/engine/LoopTake.h:93`–`106` and `252`; all eleven atomic fields are zero-initialized at `JammaLib/src/engine/LoopTake.h:420`–`430`. A current repository-wide search finds those fields only in their declarations and loads at `JammaLib/src/engine/LoopTake.cpp:691`–`714`; there is no writer. `LastAlignmentReceipt` immediately returns `nullopt` when sequence is zero at `JammaLib/src/engine/LoopTake.cpp:695`–`697`. Its only production consumer is the conditional receipt logger at `JammaLib/src/engine/Station.cpp:2378`–`2394`, so that branch is impossible in current code. Commits `dcfaf1d`, `6abf7c7`, `b90f398`, and `e0f669c` establish the debugging origin supplied by Stage 5.
- Why it matters: each `LoopTake` carries eleven atomics plus an API and a verbose logging branch that can never produce a receipt. This dead surface also obscures the useful, reachable before/after snapshot logger and coordinator telemetry.
- Recommended disposition: remove `AlignmentReceipt`, `LastAlignmentReceipt`, the eleven receipt atomics, and the unreachable receipt-printing block unless Stage 17 identifies a concrete supported receipt contract and an owner explicitly restores publication. Retain the currently reachable `_ninjamBeforePositions` before/after snapshots pending Stage 8/17 review.
- Protected timing concepts affected: none of the operational timing state; the receipt mirrors local/remote timing, loop alignment, phase, source/scene anchors, and residuals for diagnostics only. Cleanup must not remove the state or ordering that performs alignment.
- Verification: compile `JammaLib` and native tests after approved deletion; exercise audio logging in normal and `verbose` modes and focused remote-join/local-loop scenarios to confirm only the permanently absent receipt line disappears.
- Human decision: pending.

### S06-03 — Ten unregistered coloured trigger-back textures are dead copied payload

- Stage / reviewer: Stage 6 — Stale/dead-code sweep.
- Scope reviewed / exclusions: current resource registry, string consumers, loader behavior, project-copy behavior, and all branch-added textures/shaders; no visual-design judgment or decision on whether the overall HUD feature belongs in the merge.
- Severity: follow-up; dependent on the Phase 1 decision for S01-01.
- Evidence: the ten files `Jamma/resources/textures/trigger_back_green.tga`, `trigger_back_green_down.tga`, `trigger_back_green_down_out.tga`, `trigger_back_green_over.tga`, `trigger_back_green_over_out.tga`, and the five equivalent `trigger_back_red*` files are each 16,402 bytes (164,020 bytes total). None of their ten resource names occurs in current source, app wiring, or `ResourceList.txt`. The registry contains only neutral `trigger_back` at `Jamma/resources/ResourceList.txt:76`; the HUD assigns that neutral name for normal, over, and down states at `JammaLib/src/gui/GuiHud.cpp:675`–`679`. `ResourceLib` constructs texture paths only for supplied registered names at `JammaLib/src/resources/ResourceLib.cpp:28`–`58`, while `Jamma/Jamma.vcxproj:200`–`202` copies every resource file to output. Commit `5ae751e2c72a32e7cbf2ca37de31a2f033db7e93` added these variants in the HUD lineage.
- Why it matters: the files are never registered or named but are shipped to every output through the wildcard copy, adding opaque binary payload and provenance/review surface without reachable behavior.
- Recommended disposition: if the HUD feature is retained, remove the ten coloured variants. If S01-01 moves the whole HUD lineage out of this merge, resolve these files as part of that move rather than as a separate deletion.
- Protected timing concepts affected: none.
- Verification: after approved cleanup, build the app and confirm the ten files are absent from the output resource tree; launch the HUD and exercise trigger default/hover/down/out rendering to verify the nine registered live trigger textures still load.
- Human decision: pending.

No other candidate finding is asserted from this bounded sweep. The four added shaders and nine other added TGA textures are registered and have current model/HUD consumers; the legacy VST3 fallback paths have explicit current SDK/runtime roles and were not declared dead merely because they are compatibility code.

## Handoffs

- Phase 1 integrator: deduplicate S06-01 with S05-01. Stage 6 supplies the current dead-reference proof and recommends removal of only the stored member; Stage 5 supplies the direction-change commits.
- Phase 1 integrator: split S05-02 rather than carrying it forward as one ambiguous finding. S06-02 is the dead receipt subset. `_ninjamBeforePositions`, `Station::_LogLocalLoopAlignment`, `Scene::_LogAppliedNinjamLoopAlignment`, and coordinator diagnostics are reachable/dormant and need no stale-code finding.
- Stage 8: inspect callback-side publication/copy cost of any retained diagnostic state. Stage 6 makes no real-time performance assertion.
- Stage 17: decide whether the live verbose before/after snapshots and coordinator counters are supported operational diagnostics. If a receipt contract is desired, it needs an explicit writer, audience, bounds, and verification; that would be new authority, not evidence that the current receipt path is live.
- Phase 1 integrator: S06-03 depends on S01-01. If the HUD lineage is accepted, propose the ten-file deletion as a small later cleanup; if the HUD lineage is moved, do not create a duplicate cleanup item.
- Later verification owner: use the repository build wrapper and applicable local `.vscode/tasks.json` command before any build/test. This investigation performed no build and changed no code.

## Uncertainties

- No compiler/linker dead-code diagnostic was generated because this read-only investigation did not run a build and no compiler executable is directly available in the inherited environment. The private receipt fields and `SourcePhaseAtOrigin` have complete textual-reference/initialization evidence in this repository, but an approved deletion still requires compilation.
- C++ reflection is not used for these private members, and no serialization/aggregate initialization reference was found. An external source tree not represented in this repository could theoretically consume non-private headers, though `SourcePhaseAtOrigin` is only part of a source-tree internal type and Stage 1 found no installed SDK contract for it.
- Resource reachability assumes the repository's current `ResourceList.txt`-driven load path. The project copies the ten dead variants, so an out-of-tree consumer could open them by path; no such runtime contract or dynamic name source is documented or present in current code.
- The broad 199-file sweep used changed-file inventory, marker queries, added-name/reference checks, and surrounding-code review for low-reference candidates. It did not prove every branch-added local variable, template instantiation, platform-specific VST SDK branch, or persistence fallback reachable. VST compatibility paths were retained where current SDK negotiation or persisted-state behavior provides a plausible consumer; deeper compatibility contracts belong to Stage 16.
- Stage 5 already proved the old timing/controller type names absent. Stage 6 reproduced that current absence but did not independently repeat the archaeology behind their removal.
- Whether the live verbose diagnostics are worth their memory, complexity, or runtime cost is deliberately unresolved; live/dormant is a reachability classification, not a retention endorsement.

## Conclusion

Stage 6 completed the required current-reachability follow-up for both Stage 5 residue candidates and a bounded stale sweep across the Stage 1 inventory. S05-01 is confirmed as duplicated stored state: `SourcePhaseAtOrigin` has no reader after the source-coordinate pivot, while the derived phase operation and protected coordinate distinctions remain live. S05-02 splits cleanly: the eleven-field `AlignmentReceipt` path is dead because it has no publisher, but the surrounding verbose before/after snapshots and coordinator telemetry are reachable/dormant rather than dead.

One additional asset residue is confirmed: ten green/red trigger-back variants (164,020 bytes) are unregistered, unnamed, and still copied to output. No old timing/controller names survived, and no live compatibility path or complex timing abstraction was labeled dead without current reachability evidence. Three local candidates, S06-01 through S06-03, are handed to the integrator; no production code, test, build/project file, canonical artifact, unrelated worktree change, or other report was edited.
