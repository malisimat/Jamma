# Jamma alpha implementation plan

Deliver a reviewed alpha candidate with a reproducible Release x64 build and recorded automated and manual results. Execute the steps below in order; verify existing behavior before changing it, and implement only the remaining gaps.

## Execution rules

- Read `AGENTS.md`, [glossary](glossary.md), [real-time guidance](realtime-audio.md) and [build guide](build.md). Engine behavior remains in JammaLib; `Station -> LoopTake -> Loop` owns loop material and per-entity state. Scene coordinates lifecycle/publication; GUI collaborators own presentation and typed intents. Main stays app wiring and Window owns native window presentation.
- Preserve active work. Record the current branch, exact baseline, staged/unstaged changes and intended candidate contents before editing. Establish sole source/build ownership. Reuse a suitable checkout or create a `codex/alpha-readiness` branch/worktree using `.agents/skills/jamma-tree/SKILL.md`; explicitly reconcile any local changes carried into it.
- Work serially in one checkout. Use a `gpt-6.1-sol` implementer with high reasoning effort and a different independent reviewer for each changed step. With explicit model/effort overrides, use `fork_turns="none"` and self-contained briefs. The coordinator dispatches within host capacity; children do not spawn agents. Resolve findings and make focused commits. Already satisfied requirements need evidence, not duplicate changes.
- Before **every** build or native-test run, read the active worktree's `.vscode/tasks.json` and use its applicable installed command. Follow `doc/build.md` troubleshooting; report missing commands rather than guessing. Use incremental affected-project Build, absolute `SolutionDir` with exactly one trailing backslash for direct vcxproj builds, and serial build outputs.
- Callback work stays bounded, allocation-free, exception-free, lock-free and free of waits, I/O, formatting and final-owner/plugin reclamation. Reuse established immutable snapshots/mailboxes, serialized writers and off-callback retirement. Apply `threading-review` to cross-thread changes and manually inspect affected callback bodies. No anonymous namespace helpers.
- Add practical regression coverage to existing fixtures for uncovered behavior. Update the [manual checklist](alpha-manual-checklist.md) with actual results against the identified candidate; retain Unperformed where hardware, plugin, visual or listening checks have not occurred.

## 1. Continuous run.log

**Key files:** `Jamma/src/StartupLog.h`, `ConsoleCapture.h`, `Main.cpp`; `test/JammaLib_Tests/src/io/DiagnosticRouting_Tests.cpp`; [diagnostics](diagnostics.md) and [performance console](performance-console.md).

Extend console capture into a rotated `run.log` for the entire run, retaining early startup capture. Include one run-start timestamp with time-zone offset and app version. Apply verbosity once per producer; console visibility must not control file retention. Cover application-owned stdout/stderr/clog and practical CRT/library diagnostic sources through the existing sink. Callback faults use bounded events/counters drained off callback.

Acceptance and validation:

- Main/worker, early/runtime, warning/error and partial-line diagnostics reach the intended sinks once. Errors remain visible with verbose output disabled.
- Test flush, mixed/concurrent streams, thread exit, startup bursts, oversized output, saturation/truncation reporting, rotation/size bounds, denied/full destinations and shutdown. Sink failures report without recursion.
- Exercise contextual JAM/asset, VST and device failures. Document the actual capture limits and run.log location. Run focused routing tests and affected app/library builds.

## 2. Shared controls and runtime dialogs

**Key files:** `JammaLib/src/gui/GuiTextBox.*`, `GuiHud.*`, `TriggerLearnController.h`, `GuiPopup.*`, `GuiPopupManager.*`, `GuiScrollPanel.*`, `GuiLabel.*`, `GuiRack.*`, `GuiVstRack.*`, `VstRackControls.*`; `Jamma/src/RigDefaultSave.h`; existing `GuiComponents_Tests.cpp`, `GuiLayout_Tests.cpp` and `CableInteraction_Tests.cpp` under `test/JammaLib_Tests/src/gui`.

Complete shared controls without duplicating Station/LoopTake/Loop rack implementations:

- Filled glyph-height text selection remains readable with correct caret, clipping and UTF-8 boundaries. Station/Trigger rename preserves Enter/outside acceptance, Escape cancellation and exact admitted physical-key releases through modal dismissal/focus loss.
- Both Activate and Ditch pedals expose Learn controls and Alt-held overlays; Alt-click works. Label/pedal/cable hit priority prevents input leaking to Station controls.
- Learn dialogs have a configurable header, clear learning/active-row appearance, compact binding details, remove action and full-height Learn action. Reveal a newly armed offscreen row once, preserving ordinary scroll. Preserve atomic draft acceptance/rollback, stale identity rejection, passive MIDI/serial observation and imported bindings beyond the four-row interactive addition limit.
- The Station MIDI 4x4 grid starts collapsed with the reusable directional expander and a dark translucent flat label. Use the shared configurable label treatment consistently. Allowed/broadcast and explicitly cabled channels both display active, including a cabled channel outside the input filter; visual changes do not rewrite routing.
- Cable drags work from every channel cell and onto visible scrolled slot controls, with correct clipping, collapsed endpoints, cancellation and stale-revision rejection. Slot retry/replace/removal wording stays consistent across Station, LoopTake and Loop.

After step 1 establishes diagnostics, convert normal runtime notices, confirmations and save/plugin/editor failures to existing Jamma popups. Workers transfer owned values to the window/UI owner. Revalidate target identity/revision after delayed confirmation; retain default-cancel behavior and modal key cleanup. Keep a reliable native/stderr/log fallback when startup failure, fatal shutdown or teardown leaves no usable renderer. Preserve native file/directory picker behavior.

Validate small/normal windows, translated/clipped controls, active capture, unavailable devices/plugins, late/stale targets, focus loss, cancellation and renderer absence. Run focused GUI/rig/interaction tests and app build. Commit control repairs and dialog conversion separately.

## 3. Matching top-panel width and responsive Inputs layout

**Key files:** `JammaLib/src/gui/GuiMainPanel.{h,cpp}`, `GuiHud.{h,cpp}`, `GuiPanel.h`; thin layout-context forwarding in `JammaLib/src/engine/Scene.cpp`; `test/JammaLib_Tests/src/gui/GuiLayout_Tests.cpp` and `CableInteraction_Tests.cpp`.

The top GUI panel is `Scene::_selectionPanel` (selection/performance groups); settings is `Scene::_mainPanel`; Inputs is `GuiHud::_topStrip`.

- Give the expanded top panel the same outer width as settings through one shared width policy. Replace the selection panel's fixed 404px choice in `GuiMainPanel::_Layout`; reuse `_SettingsPanelWidth` measurement. Equality holds regardless of settings visibility/page. Preserve viewport clamping, content wrapping/scrolling and independent collapsers.
- Anchor Inputs to the window's top-right margin. Its left edge follows the top panel's presented visible right edge, including its collapsed handle, plus standard padding (currently 16px). Fill the remaining span rather than capping at the old preferred width. Recompute on viewport changes.
- Enforce an initial Inputs minimum of 320px (`GuiHud::_TopStripMinWidth`), retaining the 80px source-card minimum in `SourceCardWidth` and horizontal scrolling. If the side span is smaller, put Inputs below the top panel with standard vertical padding; return alongside when it fits. For windows narrower than the minimum plus margins, clamp the stacked strip to available nonnegative width and retain accessible scrolling/clipping.
- Keep the right anchor at the window edge. Where the Trigger rail intersects Inputs, move its upper boundary below the strip and preserve rail access/scrolling. Coordinate settings height and occupied bounds without recursive layout feedback or overlapping hit regions.
- Use the existing eased collapse progress and `GuiStyle::PanelTransitionSeconds` (currently 0.22s, approximately the requested 0.3s). The top panel currently slides vertically, so its raw right edge cannot drive smooth horizontal resizing. Add a horizontally retracting presented/clipped extent from expanded right edge to collapsed handle edge, using the same progress. Keep expanded content width equal to settings; derive visible clipping, occupied bounds and input geometry from the presented extent. Coordinate any retained vertical slide and stacked fallback so nothing overlaps during motion.
- Inputs reaches its final width with the top-panel transition. Reverse/repeat toggles and resize mid-animation retarget from current presentation without snapping or restarting indefinitely. Side/stack threshold decisions remain stable. Preserve source identity, scroll, meters, hover/capture and cable/socket geometry; draw, clipping and hit testing use the same presentation.

GUI owners compute layout/easing; Scene forwards context only. Extend existing layout tests for equal widths, exact minimum boundary, stacked fallback, collapsed/expanded/intermediate/reversed motion, mid-animation resize and tiny/zero viewports. Check intermediate-frame clipping, source-card bounds, scroll and hit/cable alignment. Build affected targets and manually exercise resizing/collapse during capture.

## 4. Routing, deletion and rack recovery

**Key files:** `JammaLib/src/engine/Station.{h,cpp}`, `JammaLib/src/gui/VstRackControls.*`, `GuiVstRack.*`; `test/JammaLib_Tests/src/engine/StationMidiInstrument_Tests.cpp`, `VstRackLoad_Tests.cpp` and `VstStaging_Tests.cpp`.

Verify `RemoveInactiveRackSlot`, `JOB_UNLOADVST`, route compaction and publication. Deliberate removal clears cables to the deleted identity and clears/rebases indexed routes in one coherent structural update. Imported missing targets stay recoverable and silent. Explicit-empty routes must not become Broadcast.

Preserve these processing contracts:

- Station instruments process silence-fed scratch and add their contribution at their slot; effects process the accumulated mix in slot order, including imported older JAMs. LoopTake/Loop chains remain serial.
- Shared Station channel cables also supply recorded MIDI when no separate recorded-output route is assigned. Explicit recorded-output routes take precedence. Display state and filtering remain distinct.

Test two instruments on channels 4/5, many-to-many routes, channel off/on, held notes/sustain during edits/removal, release retries under backpressure, indexed compaction, stale gestures and pending load/unload. Verify additive summation with an output-replacing fake processor, effect placement, missing/malformed identities and retry/replace/save/reload. Plugin editors/destruction remain on the UI owner with safe retirement. Run focused engine cases, app build and real-plugin checks.

## 5. Persistence, startup defaults and window title

**Key files:** `Jamma/src/Main.cpp`, `RigDefaultSave.h`; file-control integration in `JammaLib/src/engine/Scene.{h,cpp}`; `JammaLib/src/io/JamFile.*`, `IoSessionExporter.*`, `IoSessionLoader.*`; `JammaLib/src/graphics/Window.*`; existing `JamFile_Tests.cpp`, `IoSessionExporter_Tests.cpp`, rig/save and staging fixtures.

Reuse existing save completion and reconstruction. Keep detailed persistence behavior in its owners and title presentation in Window.

- A fresh unsaved title is `Jamma`; successful load/save displays `Jamma - <filename>.jam`, including Unicode filenames. A new session resets it; a newly committed save destination updates it. Cancel/failure retains the last successful JAM path/title.
- A successful JAM write followed by failed startup-default update still identifies the written JAM and reports the separate default failure. Account for numbered exports choosing a new filename.
- Rig Save uses its current path; Save As chooses a rig path; Save rig as default alone changes the startup rig default. Ordinary JAM/rig saves do not change defaults. Explicit Save JAM as default handles named/unnamed sessions and failure without losing the last valid default. JAM load uses the accepted rig; restart can select JAM and rig paths separately.
- Save/reload preserves Trigger identities/names/bindings, routes, plugin slot state/order, audio references, MIDI event order, automation and entity timing. Output remains versioned (current formatVersion 0.3.0) and omits NINJAM credentials/live remote timing authority while retaining supported legacy reads.
- Window-owner completions validate current Scene identity. Old saves/loads cannot rename a replacement session or claim a newer unsaved rig revision. Exercise cancellation, denied writes, default-update failure and missing/corrupt/unsupported files.

Run existing persistence/rig tests and add only uncovered failure/identity cases. Build app and perform paired restart, round-trip and title checks. Separate persistence repairs and title changes into focused commits.

## 6. Windows icon

**Key files:** `Jamma/Jamma.vcxproj`, app icon/resource sources and `JammaLib/src/graphics/Window.*`.

Inspect `C:/Users/matto/AppData/Local/Jamma/Textures/button_activeloops_over.png` and `loopit.png` as visual references; recover their source if absent. Produce three labelled vector-derived tilted-8/border alternatives with alpha-correct previews and ICO sizes 16/24/32/48/64/128/256. Present options, then embed the user's selected set through Windows resources and window class icons. Retain editable source assets.

Check light/dark backgrounds, small-size sharpness and Explorer/taskbar/window linkage. One reviewed asset/wiring commit after selection; other steps can proceed while that choice is pending.

## 7. Test-source cleanup

**Key files:** `test/JammaLib_Tests/src/engine/LoopModel_Tests.cpp`, `test/JammaLib_Tests/src/graphics/LoopModel_Tests.cpp`, `test/JammaLib_Tests/JammaLib_Tests.vcxproj` and `.filters`.

Compare the unbuilt engine LoopModel cases with the compiled graphics cases, including helpers/setup. Account for the expected 13 overlapping cases and seven additional graphics cases, recounting at execution. Transfer unique geometry, UV, envelope, zero-length and cadence assertions, then remove the redundant unbuilt file. Test names or assertion counts alone do not establish equivalence.

Reconcile project/filter membership for `StationVisualState_Tests.cpp`, `NinjamAudioTimingCommand_Tests.cpp` and `NinjamTimingIntegration_Tests.cpp`; check for stale entries and unbuilt unique coverage. Resolve duplicate registrations deliberately rather than compiling every source.

Record semantic disposition and run nonempty `LoopModelMesh.*:LoopModelWaveform.*:VuModelMesh.*` selections plus affected metadata cases. Keep tests bounded and hermetic. Commit cleanup after independent review and before final discovery/regression.

## 8. Combined review and alpha candidate

Freeze the candidate and have a fresh reviewer inspect the complete baseline-to-candidate diff, including admitted local repairs and integration resolutions. Resolve actionable findings, audit cross-thread ownership and manually inspect affected callback bodies. Use the actual full diff for the threading audit.

Build affected Debug library/app/native tests and run the complete native suite. Build Release x64 library/app/native tests and affected JammaConsole; run the applicable Release native gate. Record commands, configuration, exact source state, counts, disabled/failing cases and nonzero focused-filter matches. After fixes, repeat checks for changed, failed or unresolved areas.

Complete the [manual checklist](alpha-manual-checklist.md), including these integration requirements:

- Camera selection, mute groups, grave cable toggle and Space tap tempo preserve the wander deadline, including while wandering/deselecting. Navigation and many-Station Front framing remain usable.
- Help hint and `/`, `/help`, `/?`, F1 are discoverable/equivalent offline; `/ninjam`, `/n` and displayed-number `/c`, `/connect` remain consistent under stale/error conditions.
- Settings fit/scroll; forced-channel single-choice/all-off and PageUp/PageDown bounds/repeat/reset/release behavior remain correct. Gain double-click reaches unity through smoothing/undo and meters remain independent at all hierarchy depths.
- Mixed audio/MIDI take geometry stays within coherent bands through recording/edit/undo/reload, including empty/single/full pitch ranges, unequal lengths, picking and editor transitions. Display scaling does not change sound/events.
- Supported VST2/VST3 startup/runtime loads cover outer bundles/direct modules, state restore/editor/unload and recoverable missing/malformed/Unicode/wrong-architecture paths. Preserve configured plugin paths.
- With known latency-producing plugins, verify capture/playback/automation and applicable NINJAM alignment/recovery against documented policy. MIDI/spectral editors, cables, Record/Overdub and undo coexist without stuck notes, lost material or dropouts.

Use disposable test-owned sessions/files and preserve live defaults/media. Launch the exact prepared Release candidate with matching runtime DLLs, JammaConsole and registered fonts/shaders/textures. Verify startup, unavailable devices, capture/playback, two instruments/effects, note/sustain cleanup, save/reload/restart, offline operation and shutdown. Real-plugin/hardware/listening checks remain pending until performed.

Deliver baseline/final source identity, commits, build/test/review results, executable/resource/log paths, runtime prerequisites, selected icon, known limitations and manual results. Distinguish automated readiness for user testing from alpha acceptance; report any remaining acceptance gates. Stop before publishing, tagging or merging master.
