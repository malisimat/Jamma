# Quantisation enhancements: implementation plan

## Handoff

- Branch: `feature/quantisation-enhancements`
- Worktree: `C:\Users\matto\source\repos\Jamma.worktrees\Jamma-quantisation-enhancements`
- Code reviewed at `8604af2eed28caa022554a231d6b8bbab5d20593`. Findings below are static observations, not reproduced live failures; recheck them if the code changes.

Implement the required behaviour below in focused phases, reviewing and committing each coherent change. Preserve existing functionality outside these changes. Findings are starting points for investigation, not a mandatory design.

Follow `AGENTS.md`. Read `doc/glossary.md`, `doc/realtime-audio.md`, and `doc/loop-alignment-and-ninjam-sync.md` for timing work; read `doc/overlay-controls.md` and `doc/loop-grid-editor.md` for interaction work. Keep engine behaviour in JammaLib and respect Station -> LoopTake -> Loop ownership. Reuse existing publication and rendering paths; apply `threading-review` to cross-thread changes. Callback work must remain allocation-free, exception-free, lock-free, and free of logging/formatting/I/O.

## Required behaviour

### 1. First recording: taps reinterpret beats and update the live MIDI grid

Count one completed local LoopTake as one musical recording, regardless of audio channel count or accompanying MIDI layers. Exclude remote stations, incomplete recordings, and pre-reclock takes under the existing generation rules. Recognise MIDI-only masters without requiring an audio Loop.

With a sole eligible recording and local timing authority, accepted taps determine beats per master interval and associated grain geometry. Preserve recorded audio, raw MIDI events, and playback continuity. Document integer rounding/ties and any logical-length adjustment: local audio geometry must satisfy `M = GrainSamps * local grain count`, with the physical buffer and any retained tail accounted for. Inspect the existing `Loop::Play` resize path before adapting this behaviour to MIDI; do not arbitrarily trim or stretch the performance.

Space taps must work with the E editor open and Ctrl held. Accepted updates must reach playback, the editor, and visible grids promptly.

Resolution changes must preserve the musical pattern and edit history without deleting or duplicating source notes. Coarser grids necessarily merge visible occupancy; they cannot preserve identical cell indices or every gap. Define how existing note coverage maps to new boundaries and how durations/wrap are handled. With no intervening note edits, returning to the original resolution must recover the original pattern. Use existing source-event attribution where possible; changing resolution alone must not rewrite raw events. Cancel an active note preview safely or keep its geometry stable until release so stale cell coordinates cannot be committed.

### 2. Additional recordings: taps change subdivisions only

With more than one eligible completed local take, taps must leave local master length, grain size, grains per master, loop lengths, and phase relationships unchanged. Normal cursor advancement continues. Retain nearest-permitted-division selection, extending it for straight/triplet choices with explicit candidate limits and tie rules.

Playback, ordinary overlays, editor rendering, and hit testing must agree on effective boundaries. Extend the existing settings/snapshot path as needed to express grid authority/source, interval/origin, and subdivisions. Keep local grain distinct from grid spacing. Define how the tap-selected base grid composes with global/station/take/loop subdivision settings and offsets, applying each once; preserve quantisation enabled/off state.

While following NINJAM, remote BPM/BPI remains authoritative. Local taps may change compatible subdivisions or reject incompatible geometry changes with an explainable outcome. A local helper must not accidentally clear the accepted remote descriptor. Preserve NoSync, reclock, and saved-session behaviour.

### 3. Ctrl edit mode: scope, visibility, transitions

Resolve targets at the active selection depth: selected items first, otherwise the relevant hovered item, otherwise global. Capture targets on Ctrl press and keep them stable for that overlay session and any active drag. Support multiple selected targets and indicate mixed values.

Station, take, and loop edits must affect the stated scope. Current loop-depth edits promote to the owning take; investigate which controls need loop-local settings and implement them without silently changing siblings. Preserve ownership boundaries rather than merely relabelling take-wide changes.

Ctrl should ease the edit overlay in quickly while fading station/take/loop faders, master controls, and racks out. At full opacity only applicable edit controls remain over the scene. Release restores other controls more gradually, without jumps on rapid repress/release. Suggested starting points are 100-150ms entry and 350-500ms release, subject to visual tuning.

Controls suppressed by edit mode must also stop receiving clicks, scrolling, focus, and hover. Restore prior visibility and layout. Preserve E-editor Ctrl gestures, undo/redo, text entry, and existing shortcuts; update modifier state despite editor consumption without stealing contextual actions. Space used for text entry must not accidentally tap tempo.

### 4. Overlay styling and drag feedback

Retain blue/green shift and orange/red division scope colours. Add a flat translucent grey panel with a subtle border, rounded handle outlines, a restrained gradient stronger at the centre, and uppercase captions above each handle: SHFT and DIV. Prefer existing fonts and code/shader geometry for these simple shapes.

From handle press through drag, show only that handle and caption. Retain target and pointer capture if Ctrl is released mid-drag. At gesture end restore controls according to Ctrl state and release the gesture's grid hold.

Show live feedback in the bottom status area from initial press: scope and signed offset in ms (samples where useful), or exact subdivision fraction/division count. Show mixed/range information for multiple targets. Temporarily own this feedback so normal HUD messages recover afterward. Escape, lost capture/focus, target deletion, and cancellation must not leave stuck controls or stale feedback.

### 5. Main panel: tap control and triplets

Add **Tap tempo (Space)** beside subdivision choices; mouse and keyboard taps must use the same timestamped engine action. Label the existing metronome separately, e.g. **Metronome**.

Retain straight fractions and add **1/3, 1/6, 1/12**. Clearly label their reference: local grain under local timing, remote beat under remote authority; these are not generally equal. Compute boundaries using rounded rational interval/division arithmetic, including non-integral sample spacing. Preserve existing enum values and packed-field meanings, and use display/drag ordering that does not rely on persisted enum order.

Uniform triplets allow a 2:1 shuffle by placing notes at the first and third triplet positions. Verify that placement works; these choices do not constitute automatic alternating swing.

### 6. Space and gesture grids: independent visibility

Move hold-to-show grids from Ctrl to Space. Physical Space keydown holds grids visible and registers one tap; ignore auto-repeat. Keyup releases the hold and starts normal fading without another tap. Mouse taps pulse grids. Ctrl alone must neither hold nor pulse grids.

Use a 3s press-to-press timeout (currently 2.5s). The first press starts a sequence without changing tempo. A gap greater than 3s starts a fresh sequence without changing tempo; exactly 3s remains in the sequence. Holding Space creates no additional taps. Handle non-increasing timestamps and invalid timing inputs safely; sample position zero can be a valid first timestamp.

Handle press/drag also holds grids visible. Compose Space and gesture holds so releasing one cannot hide grids still needed by the other. Keep grid alpha independent of Ctrl panel alpha; clear stale held state on focus loss and editor/session teardown.

### 7. Explainable diagnostics

At INFO, explain recording/master establishment, tap inputs/results, and accepted, rejected, or unchanged grid decisions. Use compact records containing the context relevant to each decision:

- Take identity, eligible local-take count, local reclock generation, authority/source.
- Physical and logical master lengths, sample rate, grain, grains per master, musical BPI/BPM, and effective grid divisions; include samples/ms where useful.
- Default inference policy or raw/smoothed tap gap, requested and selected beats/divisions, subdivision fraction, and rounding/tie decision.
- Before/after state and outcome reason; correlate publication/application identity where needed across the audio boundary.

Keep grain count, grid divisions, and remote BPI distinguishable. For example, a 2000ms master tapped every 500ms requests 4 beats; explain the chosen geometry and fraction producing 4, 8, 12, or 16 cells. Also explain first-recording inference from defaults before taps.

Format/output off callback. Reuse existing diagnostics/receipts where possible; add a bounded receipt only if audio-boundary confirmation is needed. Avoid per-frame/cell/event spam; rate-limit drag traces and summarise the final value.

## Checked findings and investigation entry points

Paths below are relative to `JammaLib/src/` unless stated otherwise. Line numbers locate the reviewed code; function names are the durable reference.

| Observation | Evidence | What to check/change |
| --- | --- | --- |
| Editor routing precedes global Space/Ctrl handling; Space currently taps on keyup. | `engine/Scene.cpp:1314`, `:1352`, `:1361`; `midi/LoopGridEditor.cpp:1312`, `OnAction` | Editor consumption blocks the later handler. Track physical key state and preserve contextual/text-input precedence. |
| Tap master discovery and sole-take resizing require an audio Loop. | `engine/Quantiser.cpp:276`, `HandleTapTempo`; `engine/LoopTake.cpp`, `VisualLoopLengthSamps` | Eligible takes are counted, but MIDI-only takes cannot supply `_masterLoop`. Inspect default master establishment as well as taps. |
| Frozen-geometry taps store `_activeGridDivisions` and return. | `engine/Quantiser.cpp:347`, `:365`, `:619` | No external `ActiveGrid` consumers were found; the stored choice does not propagate to the current MIDI/editor/render paths. |
| Local grain propagation clears remote grid state and traverses all stations. | `engine/Quantiser.cpp:202`, `SetMidiGrain`; `:234`, `SetRemoteMidiGrid` | Preserve authority and verify local/remote target scope when adding effective-grid propagation. |
| MIDI views already use published quantisation settings. | `midi/LoopGridEditor.cpp:1111`; `graphics/MidiModel.cpp:289`; `engine/LoopTake.cpp:2819`, `ResolvedMidiQuantisation` | Reuse the existing non-destructive event/snapshot path and source attribution. Ordinary overlays also need matching geometry. |
| Ctrl holds grids and shows handles; depth/hover are captured before selected drag targets. | `engine/Quantiser.cpp:853`, `QuantiserController::OnCtrlModifierChanged`; `:1454`; `doc/overlay-controls.md` | Separate holds/transitions and make target capture match the requirement. Documented loop-to-take promotion is existing behaviour being changed here. |
| Handles are coloured GL quads; current master-control visibility does not suppress the whole scene UI. | `graphics/CtrlHandleOverlay.cpp:141`; `engine/Scene.cpp:2652`; `gui/GuiRack.cpp:284` | Add styling and inspect entity/rack rendering plus input paths for suppression. |
| Fractions and drag order currently follow six power-of-two enum values. | `midi/MidiQuantisation.h:16`, `:45`, `:64`, `:178` | Audit labels, radio counts, drag order, packed settings, and session decoding when adding triplets. |
| CLICK controls the metronome; the HUD has existing bottom-status rendering. | `engine/Scene.cpp:193`; `gui/GuiHud.cpp:628`, `:891` | Add tap separately and reuse status rendering without overwriting unrelated messages permanently. |
| Tap logs exist but do not explain every outcome; timeout is 2.5s. | `engine/Quantiser.cpp:758`, `TapTempoTracker::TapAtSample`; `engine/Quantiser.h:68` | Distinguish first tap, timeout, invalid input, and unchanged/rejected geometry; explain the currently silent frozen-grid branch. |

Relevant tests: `test/JammaLib_Tests/src/engine/Quantisation_Tests.cpp` covers inference/tracker helpers; `src/midi/MidiQuantisation_Tests.cpp` covers fractions/boundaries. Inspect `MidiGridGesture`, `LoopGridGeometry`, `LoopTakeTiming`, and `NinjamTimingIntegration` tests for reusable fixtures. Helper coverage does not establish end-to-end input routing or live propagation.

## Execution and validation

1. **Confirm contracts and gaps.** Trace default/tap master establishment, editor input routing, grid publication, scope, and persistence. Settle tap/subdivision composition, non-destructive pattern mapping, and remote authority before splitting implementation. Add regression tests where practical; use live reproduction for paths the native harness cannot exercise.
2. **Engine foundation.** Implement sole/additional-take policy, MIDI-only masters, effective grid propagation, triplets, pattern preservation, and diagnostics. Review changed callback/publication boundaries, incrementally build JammaLib, and run relevant native tests.
3. **Interaction and presentation.** Implement input/hold states, scope, visibility/transitions, panel controls, handle styling, and HUD feedback. Integrate with editor routing and cancellation. Keep one writer for shared files such as Quantiser.cpp.
4. **Acceptance and handoff.** Run relevant tests and an incremental app build, verify the UI, update `doc/overlay-controls.md` and `doc/loop-grid-editor.md`, and record evidence plus any unverified limitations.

Use small Luna subagents for narrow code research with exact file/line references. Use GPT-6.1-Sol subagent sessions for implementation/review where helpful, assigning disjoint ownership after contracts are settled. Parallel work is optional; avoid concurrent edits to shared files.

Acceptance checks:

- Sole audio, MIDI-only, and multichannel audio+MIDI takes: Space/mouse taps, E editor open, Ctrl held, immediate grid updates, retained source data/history, and resolution round trips.
- Additional take: taps change effective subdivisions while construction geometry and relative phases remain intact. Exercise NINJAM authority, NoSync, reclock, and session load.
- Straight/triplet boundaries: 44.1/48kHz, fractional spacing, endpoints/wrap, offsets, multiple MIDI channels, unequal loop lengths, note durations/order, disabled quantisation, and pack/save/load compatibility. Playback, rendering, and hit testing agree.
- Ctrl scope: selection beats hover at each depth, hover/global fallback, multiple targets, true loop scope, hidden-control input suppression, and restored visibility. Preserve editor gestures and shortcuts.
- Space/drag lifecycle: repeat/hold, timeout boundary, overlapping hold reasons, Ctrl release mid-drag, focus/capture loss, Escape, and target deletion. Verify feedback restoration and transitions at representative window sizes/DPI.
- Include a real diagnostic trace for default inference and both tap modes. Do not substitute invented output for runtime evidence.

Before every build/native-test run, read local `.vscode/tasks.json` and follow `doc/build.md`; use incremental affected-project builds and an absolute SolutionDir with one trailing backslash for direct project builds. Report missing commands/dependencies rather than guessing paths. This plan review did not build, run native tests, or verify the live UI.

Attempt to complete without human intervention or stopping (one long running session), provide a summary html on completion and open in browser (include any concerns, outstanding, next steps).