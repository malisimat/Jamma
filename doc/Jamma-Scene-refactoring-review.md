# Scene refactoring review handoff

Updated 2026-10-12. Worktree: C:\Users\matto\Source\Repos\Jamma.worktrees\Jamma-scene-refactoring.
Original plan: [Jamma-Scene-refactoring-options.md](Jamma-Scene-refactoring-options.md), including its implementation-session notes. The implementation, new sources, and both plan/review documents are included in the refactoring commit. Review the complete commit, including added files.

## Implemented

Plan steps 1–8 have source implementations:

1. graphics::Skybox::AnimationTransform: deterministic animation math; Scene retains time, camera, and GL orchestration.
2. ninjam::NinjamTimingDiagnosticPresenter: diagnostic formatting and sequence/overflow bookkeeping; existing job drain and verbose gate retained.
3. gui::NinjamTempoPrompt and ninjam::NinjamTempoProposal: UI owns popup lifetime; coordinator owns identity/resolution. Service validates session/revision and resolves under its existing timing mutex. Phase observations retain identity; replacements/reconnects invalidate it. User dismissal rejects; obsolete cleanup does not. Required-press button gating prevents an old release accepting a replacement popup.
4. gui::SceneSettingsControls: controls, entries, command IDs, and typed decoding. Scene retains subsystem effects. String-edit notifications and accepted-value fallback preserved. User approved finite MIDI-channel numeric clamping before integer conversion; nonfinite values retain the accepted channel.
5. engine::HierarchySelection: selection/mute through a caller-owned locked station view. Scene retains locking, gestures, picking, and rack/editor consequences.
6. midi::MidiRefreshPolicy: polling, inventory comparison, and explicit missing/failed-device retry. Device lifecycle, confirmation, and rig/input publication remain in existing owners.
7. io::IoSessionLoader: reconstruction, matching descriptors, HUD input values, saved master values, and fresh-rig history restoration. Scene retains installation/publication and failure ordering.
8. audio::AudioHost::InitWithBlockAdvancement / AdvanceBlock: captures the exact shared Timer once before startup; advances the existing borrowed station list at the original callback point. Old custom callback API remains; Scene::OnTick is a compatibility adapter. AudioCallback and _OnAudio bodies unchanged.

New sources are registered in JammaLib.vcxproj and its filters. Existing test-helper widget accesses in GuiComponents_Tests.cpp and Trigger_Tests.cpp were mechanically redirected; no test assertions or cases were added or changed. Documentation updates include doc/ninjam.md and doc/realtime-audio.md.

## Authorized HUD ownership fix — source complete

Runtime routing producers now call `GuiHud::PublishRoutingConfig` (`Scene.cpp:1496`, `:1668`, `:1934`) under their existing `_sceneMutex` serialization. They publish one immutable presentation value containing audio input count, MIDI names and the shared immutable RigSnapshot. These calls no longer rebuild widgets or access the popup manager. Initial reconstruction still uses `SetRoutingConfig` before normal UI use (`Scene.cpp:325`).

`GuiHud::ApplyPendingRoutingConfig` (`GuiHud.cpp:732`) atomically takes the latest complete value and reuses `SetRoutingConfig`. `Scene::CommitChanges` consumes it under its existing UI scene lock (`Scene.cpp:2066`) and invalidates hover after rebuilding. `Window::Render` calls this before resource initialization, VU updates and drawing (`Window.cpp:500`). Lock contention leaves the pending value for a later frame. All current production CommitChanges callers are UI-owned: Render, the synchronous VST-insert callback from the key handler, and InitAudio. Main's four InitAudio calls are on startup/the window message loop (`Main.cpp:819`, `:1157`, `:1188`, `:1206`), not the job runner. Scene.h now states that caller contract explicitly.

Routing revision changes remove only the delete-confirmation popup through a small exact-element `GuiPopupManager::Close(element)` overload. This also removes a buried obsolete confirmation without closing the tempo prompt or HUD source-identity popup. Popup ownership and existing pointer clearing on Open are preserved. Delete identity is cleared before cleanup as before.

Shared-state review: `GuiHud::_pendingRoutingPresentation` is owned by the HUD; job/UI producers write only through release-store, serialized by the existing Scene lock; UI reads/clears through acquire-exchange. Later publications replace earlier unconsumed complete values. The immutable value retains rig topology and trigger lifetime until replacement/consumption; widgets only retain the existing weak trigger references after application. No audio callback reads/writes this member. Atomic shared_ptr operations and allocation are confined to job/UI paths. Scene shuts down and joins the job runner before HUD destruction, which releases any pending value on the UI owner.

The audio/input publication handshake, admission revision checks and trigger-input reopening remain in their existing order. HUD presentation may lag until the next successful UI reconciliation; existing revision admission prevents stale displayed trigger input being accepted. There is no new callback lock, wait, allocation, timing authority or second station list.

## Validation and remaining review

Source/diff review and bounded independent static review found no concrete source defect in the extracted classes. Compilation and live behavior remain unverified. No builds, native tests, allocation tests, or live checks were run. git diff --check passed, including after the HUD ownership fix. The threading-review hot-path script was rerun after the HUD fix and exits 1 for the moved pre-existing Scene lock in a settings UI handler; manual review found no added callback lock/wait. Do not describe the script as passing. Direct comparison with HEAD confirms AudioHost::AudioCallback and AudioHost::_OnAudio bodies are unchanged. AdvanceBlock only ticks the pinned clock and traverses the existing borrowed station list; Scene::OnTick forwards to it. Other callback-owned engine bodies in the checklist have no diff. All 12 new source/header paths are present in both JammaLib.vcxproj and its filters.

The audio clock invariant was reviewed: an internally advanced stream pins its Timer while SetTimingClock remains public. Initialization publishes the same Timer after old-stream quiescence and before startup. Replacing it midstream with a different instance would split timing application from advancement; AudioHost.h:132 documents the same-instance-until-reinitialization contract; current production code publishes it only from initialization. Future SetTimingClock callers must honor it.

Source implementation for steps 1–8 and the authorized HUD fix is complete. Compilation/runtime confirmation remains unavailable under the user prohibition; no build or behavioral-test success is claimed.

Continuing constraints: work only in this worktree. Do not add tests or run tests/builds unless explicitly requested. Read AGENTS.md and its referenced glossary, real-time, build, and NINJAM alignment guidance. For any later authorized build/test, first read this worktree's .vscode/tasks.json; it is authoritative and must not be edited. Use the threading-review skill for the HUD cross-thread change. Ask directly before dependent changes if material requirements or risks are unclear.

## Cleanup

The work directory and static HTML discussion book were removed at the user's request; do not recreate them. Previously reported literal cache directory for manual cleanup: C:\Users\matto\Source\Repos\Jamma.worktrees\Jamma-scene-refactoring\%SystemDrive%. Automatic approval review rejected its deletion; it is not source implementation.
