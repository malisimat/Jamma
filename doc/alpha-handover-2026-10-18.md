# Jamma alpha handover — 18 October 2026

## Repository and local integration

- Repository: `C:\Users\matto\Source\Repos\Jamma`.
- Current feature branch: `codex/trigger-names`. Inspect `git log` and `git status` before resuming. This handover and the final corrections are committed locally.
- Integration branch: `bugfix/alpha-cleanup`, at `704e6adc` (station-label merge). Trigger-name commits have **not** been merged into it yet.
- Station feature: `f57851cf`, merged by `704e6adc`.
- Trigger feature history: `afb0957d` (hermetic native Alt tests), `9abbf432` (initial rename), `fe34c142` (in-place editor and pencil art), `d396e27b` (accepted layout and pedal hit priority), followed by the final label-click/cancel-colour correction. The initial modal design is superseded.
- Nothing pushed, released, tagged, or merged into master. Preserve all unrelated work and untracked files.

User asked to finish the final two corrections, commit, write this handover, and stop. Do not interpret this handover as a request to continue work in the outgoing session.

## Read before implementation

Read repo `AGENTS.md`, `doc/glossary.md`, `doc/realtime-audio.md`, `doc/build.md`, and all five plans in `C:\Users\matto\OneDrive\Desktop\`:

1. `Jamma-Alpha-Implementation-Plan.md`
2. `Jamma-Station-Labels-Plan.md`
3. `Jamma-Trigger-Names-Plan.md`
4. `Jamma-MIDI-VST-Rack-Plan.md`
5. `Jamma-Trigger-Learn-Plan.md`

The user's UI decisions below override the older modal trigger-rename contract in those plans. Keep source editing serial, use independent feature reviewers, and use a final independent Sol reviewer for the eventual integrated result. Keep changes focused, reuse existing ownership/code, and keep tests few.

## Implemented and accepted decisions

### Station labels

200px fader-style label above the fader; 36px row. Transparent editor preserves background. Ctrl reveals a 28px pencil at the right; label-background click also opens editing on release without Ctrl. Movement over 4px cancels that click. Enter/outside click commits; Escape/cancel X discards. The cancel X now uses the same 28px rectangle and palette as the pencil, consistently with trigger rename. Remote names are read-only. Bin stays above label; VST remains below fader. SHFT/DIV is pinned 14px diagonally toward screen centre by cursor quadrant.

Reuse `GuiStationLabel`, acknowledged RigCoordinator metadata promotion, immutable Station names, `GuiTextBox::BeginEditing` and UTF-8 boundary editing, and `ContextualKeyboardInput` exact admitted-release receipts. Rename preserves membership, selection, histories and undo. Pending VST jobs use receiver/plugin identity, not the mutable name. An unnecessary ordinary-job lifecycle try-lock was removed in the station feature to fix intermittent Busy; explicit save/shutdown/input/ack guards remain.

### Trigger labels

User explicitly rejected a separate rename dialog. Rename is **in place**, like Station rename: select the raw name, Enter/outside click commits; Escape or cancel X discards. Preserve Trigger ID, mappings, routes, histories and normal rig acknowledgement. Validation/rejection retains the draft; acceptance never autosaves. Pencil stays visible without Ctrl and is replaced by the cancel X in exactly the same slot during editing.

User confirmed functionality and the revised layout work beautifully. Final two corrections were requested afterward: clicking the trigger label must open rename and front trigger panels must prevent clicks leaking to Station controls behind; cancel X must use the pencil palette, distinct from the red delete X. These final corrections still need the user's quick visual check after rebuilding.

Current card geometry: 136×132, rail width 156. Socket centres are at Y=12 and Y=120; top-right delete is 24×24. Name row is Y=76, with a centred single-line display label and 28px pencil/cancel slot at X=104. Pedals are 46×62 at X=20/70, Y=8, centred as a pair. Pedal hits take priority where the lower socket hit radius overlaps their edge; the actual socket centre remains draggable.

Pencil art has default/hover/down variants. Cancel art uses the same rounded geometry/palette (orange/white/darker orange), because the existing close art has baked-in red and the shared tint shader multiplies the background too. The red delete textures are retained. Art is registered in `Jamma/resources/ResourceList.txt` and copied by the app build.

The abandoned dialog's invisible text was caused by detached children initialized before shared ownership: missing parent links made clipping use incorrect global coordinates. The in-place collaborator is attached through the actual card tree after construction. Regression assertions cover ancestry and translated editor/label positions; do not revive the detached modal.

## Useful owners and regression seams

- `JammaLib/src/gui/GuiTriggerRename.h`: UI-owned raw draft, transparent editor, cancellation, pending/rejection state, row geometry.
- `JammaLib/src/gui/GuiHud.cpp/.h`: trigger cards, pointer routing, label-click gesture, stable ID/revision validation, cable hit priority and accepted presentation. The front input strip/trigger rail capture pointer input; the passive bottom status bar retains its existing pass-through semantics.
- `JammaLib/src/engine/Scene.cpp`: UI focus/input orchestration and serialized rig edits. Normal HUD rig submissions depend on Scene serialization; the text-accept callback uses the public locking entry because Enter/blur/outside acceptance can run outside ordinary HUD dispatch.
- `GuiStationLabel.h`, `GuiTextBox.*`, `ContextualKeyboardInput.h`: shared editing/input contracts; do not replace the exact admitted-release ledger with ordinary matching of every key-up (inverse bindings can attack on key-up).
- Existing fixtures: `GuiComponents_Tests.cpp`, `GuiLayout_Tests.cpp`, `CableInteraction_Tests.cpp`, `SceneRoutingIntegration_Tests.cpp`, `RigSnapshot_Tests.cpp`.

## Verification record and limits

- Station's earlier focused native run: 85/85. Debug library/app/tests builds passed; station-era Release library build passed with zero warnings/errors.
- Full station-era Debug run: 1,824/1,826 passed; 17 disabled. The two failures were native background Alt tests reading physical OS key state despite synthetic messages. Fixed through the existing test-owned `_IsNativeKeyDown` seam in `SceneRoutingIntegration_Tests.cpp`; focused reruns passed. Do not claim a later full-suite pass.
- In-place trigger implementation: Debug library/app/tests builds passed with zero warnings/errors; 308/308 GUI/cable/rig/Scene checks passed. User visually accepted functionality.
- Accepted geometry pass: Debug builds passed with zero warnings/errors; 20/20 targeted editor/layout/cable checks passed. One test sequencing error was corrected: queued pedal edges legitimately block routing edits until consumed, so socket-drag admission is checked on an idle trigger.
- Final label-click/panel-capture and shared station/trigger cancel-X correction: Debug library, app and native-test builds passed with **zero warnings/errors**. Final selected native run passed **331/331** across GUI, HUD/cables, rig snapshots, Scene routing and Scene input fixtures. Runtime cancel-texture hashes match the source assets; type-2 32-bit RGBA bottom-first encoding was checked. Logs: `alpha-final-lib.log`, `alpha-final-app.log`, `alpha-final-test-build.log`, `alpha-final-tests.log` and `alpha-final-audit.log`.
- Independent source reviews cleared station, in-place rename, and geometry after findings were fixed. The final threading script reported four new `Scene` UI `scoped_lock` sites because it scans whole-file diffs. Manual source inspection places these in `_OnPointerAction`/`_OnPointerMove`, preserving ordinary HUD rig-edit serialization. No callback bodies were modified by the final corrections. This is a qualified manual review, **not** an unqualified static-audit pass.
- User interaction acceptance is evidence for the tested naming/UI flows, not for unperformed hardware/plugin or persistence scenarios. Final Release app/test builds, combined full regression, integrated performance acceptance and final Sol review remain pending.

Before **every** build/native test, read local `.vscode/tasks.json`; it is authoritative. Use incremental Build of affected projects, the documented PATH/Path process preflight if needed, and absolute `SolutionDir` with exactly one trailing backslash for direct vcxproj builds. Do not edit tasks, Clean/Rebuild by habit, guess tool paths, or run parallel MSBuild into shared outputs. The applicable installed MSBuild is recorded in those tasks, not this document.

Correct app: `C:\Users\matto\Source\Repos\Jamma\Jamma\bin\x64\Debug\Jamma.exe`. Desktop launcher previously opened installed Release. Preserve unsaved sessions; verify executable path. Computer-use helper discovery/approval did not provide reliable access, so do not claim automated visual acceptance.

Preserve untracked `alpha-*.log` verification records. The user explicitly requested unused image/resource cleanup: this session's two icon staging directories and unused `rename_pencil*_previous.tga` comparison images were removed, including copied Debug runtime backups. All six pencil/cancel state textures are referenced and registered; the red delete textures remain in use. Do not accidentally commit verification logs. `alpha-progress.html` is committed and records milestones.

## Next sequence and alpha blockers

1. Confirm the user's quick visual acceptance of the last two trigger corrections; inspect latest code/status. Merge the reviewed trigger feature locally into `bugfix/alpha-cleanup` when its gates are satisfied. Avoid cherry-picking work already in the branch.
2. Implement **Station VST rack/channel cables** from that integration tip, following the companion plan completely. It is unimplemented: stable identity slots, live-only per-channel Broadcast/Explicit routing, explicit missing-target silence, sustain/note release ownership, additive instrument summation and policy-aware latency, legacy serial compatibility, bounded import/recovery, persistence and latest below-fader/collapsible-channel layout are all required. MIDI FX transformation/chaining is excluded. Use fake processors that replace output to prove instrument summation; preserve Take/Loop serial chains and recorded MIDI output routes.
3. Implement **trigger learn and explicit rig overwrite**, then integrate. It is unimplemented: atomic multi-row pedal draft, four-row UI addition cap without truncating imported rows, passive MIDI/serial observation with freshness tokens, reserved/modal keyboard policy, current canonical inverse-edge normalization, busy/stale/conflict retry and existing accepted rig transition. Extend `RigDefaultSave`; do not autosave accepted edits or change defaults on ordinary Save.
4. Complete paired JAM+rig save/reload, naming during capture/pending plugin work, real supported VST2/VST3/hardware checks, integrated held note/sustain/route-change performance session, final Debug full suite, appropriate Release builds, threading audit plus manual callback inspection, and final independent Sol review. Fix findings before claiming readiness.

Alpha is **not release-ready**: rack and learn/save features and these integration/acceptance gates remain. Do not push, release/tag or merge master.

## Suggested prompt for the fresh session

> Read `C:\Users\matto\Source\Repos\Jamma\doc\alpha-handover-2026-10-18.md`, repo AGENTS.md, and `C:\Users\matto\OneDrive\Desktop\Jamma-Alpha-Implementation-Plan.md` with its four companion plans. Continue the remaining alpha tasks autonomously, following their sequence and the latest user decisions recorded in the handover. Station labels are already merged; trigger names are implemented on `codex/trigger-names`, with in-place editing and user-approved layout superseding the old modal specification. Inspect current status and complete the final naming acceptance/integration before implementing Station VST rack/channel cables, then trigger learn/explicit rig overwrite. I authorize feature branches/worktrees, local commits and validated merges into `bugfix/alpha-cleanup`. Keep shared-file editing serial; use independent feature reviewers and a final independent Sol reviewer. Keep changes focused, reuse existing code and owners, keep tests few, preserve unrelated work and verification logs, and clean up unused generated images/resources before committing. Read local `.vscode/tasks.json` before every build/native-test run. Complete the prescribed build, threading and acceptance checks; fix findings before proceeding. Ask only about genuinely unresolved decisions or blockers the approved plans/source cannot settle. Do not push, release/tag or merge master. Keep me informed and report actual verification results and remaining alpha blockers.
