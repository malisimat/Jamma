# Quantisation enhancements: implementation plan

## Handoff

- Branch: `feature/quantisation-enhancements`
- Worktree: `C:\Users\matto\source\repos\Jamma.worktrees\Jamma-quantisation-enhancements`
- Investigated base: `4c3f2047fc0cb568be2444fdea2642a070c0a157` (master).
- This session creates the worktree and this plan only. Implement in a fresh session.
- The jamma-tree skill copied local `.vscode/` and `.agents/` contents without overwriting existing files.
- This plan lives under `docs/` as requested; existing authoritative project references remain under `doc/`.

Read `AGENTS.md`, `doc/glossary.md`, `doc/realtime-audio.md`, `doc/loop-alignment-and-ninjam-sync.md`, `doc/overlay-controls.md`, and `doc/loop-grid-editor.md` before implementation. Timing/loop behavior belongs in JammaLib. Keep callback work allocation-free, exception-free, lock-free, and free of logging/formatting/I/O. Use existing coherent mailbox and immutable snapshot patterns; apply the threading-review skill to cross-thread changes. Never add anonymous namespaces.

## Findings and probable causes

These are static code findings, not claims of a reproduced live bug. File/line references refer to the investigated base.

| Finding | Evidence | Implication |
| --- | --- | --- |
| The open MIDI editor consumes Space before Scene sees it. | `JammaLib/src/engine/Scene.cpp:1313`; `JammaLib/src/midi/LoopGridEditor.cpp:1312`, `:104` | The later Space tap handler at `Scene.cpp:1361` is unreachable while the editor consumes ordinary keys. This directly fits the reported edit-mode sequence. |
| Master discovery assumes an audio Loop pointer. | `JammaLib/src/engine/Quantiser.cpp:281`, `:300`, `:345`; `JammaLib/src/engine/LoopTake.cpp:1209` | Visual take length can come from MIDI, but master selection uses the audio-loop list. MIDI-only takes can fail discovery or fall into the frozen-geometry path without a usable grain. |
| Multiple-loop taps update a value with no external grid consumers. | `JammaLib/src/engine/Quantiser.cpp:347`, `:365`, `:619` | Storing active divisions alone does not update MIDI playback, the editor, or visual grid snapshots. A repository search found no external callers of ActiveGrid. |
| MIDI grids use published per-loop settings. | `JammaLib/src/midi/LoopGridEditor.cpp:1113`; `JammaLib/src/graphics/MidiModel.cpp:289`; `JammaLib/src/engine/Quantiser.cpp:202` | Fix propagation through the actual playback/render/editor settings path; avoid a second UI-only grid calculation. |
| Ctrl currently holds the grid and immediately shows handles. | `JammaLib/src/engine/Quantiser.cpp:853`, `:861`, `:969` | Grid visibility and edit-control visibility must become independent state. Handles currently fade over 0.5s; quant grids fade over 2s (`:426`). |
| Overlay target context is captured on Ctrl press. | `JammaLib/src/engine/Quantiser.cpp:947`; `doc/overlay-controls.md` | Preserve selection-over-hover priority and stable targets through a gesture. Loop depth currently promotes to the parent take; true loop-local targeting needs investigation rather than a cosmetic relabel. |
| Handles are bare coloured GL quads. | `JammaLib/src/graphics/CtrlHandleOverlay.cpp:141` | Add the requested panel, gradients, captions, and rounded borders in the rendering path. |
| Fractions are powers of two only. | `JammaLib/src/midi/MidiQuantisation.h:16`, `:64`, `:78` | Add explicit triplet fractions/divisors; inspect enum persistence, packing, drag order, radio counts, and all boundary consumers. |
| CLICK is the metronome toggle. | `JammaLib/src/engine/Scene.cpp:193` | Do not silently turn the metronome into tap tempo. Add an actual Tap tempo button and give the metronome a descriptive label. |
| Logs exist but omit the decision chain. | `JammaLib/src/engine/Quantiser.cpp:181`, `:758` | Report inputs, policy, geometry decision, publication, and outcome together, including the currently silent multiple-loop branch. |

Existing tests cover inference helpers and tracker arithmetic, not the complete tap workflow: `test/JammaLib_Tests/src/engine/Quantisation_Tests.cpp:238`, `:410`, `:571`. Fraction and boundary tests are in `test/JammaLib_Tests/src/midi/MidiQuantisation_Tests.cpp:105`, `:274`. Retain and extend these; add integration coverage for the missing paths.

## Required behavior and implementation work

### 1. First loop: taps change the master beat interpretation and live MIDI grid

Treat one completed local LoopTake as one musical recording, regardless of its audio channel count or accompanying MIDI layers. Exclude remote stations, incomplete recordings, and pre-reclock takes according to the existing generation rules.

Make master discovery represent the owning take and its audio/MIDI geometry explicitly. Do not require an audio Loop to recognise a MIDI-only master. For a sole recording, accepted taps determine beats per master interval and the associated local grain/beat geometry. Preserve recorded material and playback continuity. Inspect the existing sole-loop path that calls `Loop::Play` (`Quantiser.cpp:373`) before adapting it to MIDI.

Document the chosen integer rounding and tie rules. Preserve the physical recording and original source events; keep `M = GrainSamps * local grain count` for local audio construction and account explicitly for a retained physical tail. Do not arbitrarily trim or stretch the recorded performance to make the UI look right.

Allow Space taps while the E MIDI editor is open and while Ctrl edit controls are held. Rebuild playback/editor/grid snapshots on accepted timing updates so visible step count changes immediately.

Pattern preservation is an explicit acceptance criterion: changing step count must keep the existing filled/unfilled musical pattern and its relative positions around the interval, without clearing notes, duplicating notes, or resetting edit history. Use stable source-note identities and master-relative positions to remap occupancy to the new boundaries. Different resolutions cannot retain identical cell indices; define and test how covered cells merge/split and how note durations survive. Returning to the original resolution must recover the original pattern. Changing resolution alone must not destructively rewrite raw MIDI events. If a note-edit gesture is active, cancel its preview safely or freeze its geometry until release; never commit stale cell coordinates.

### 2. Additional loops: taps change subdivisions only

Once other completed local takes exist, freeze local master length, grain size, and grains per master interval. A tap changes musical grid subdivision, not audio construction geometry or loop lengths/cursors. Retain the existing nearest-permitted-division intent, but extend it deliberately for the straight/triplet family and make its tie/limit rules explicit.

Publish one coherent effective grid description through the existing quantisation settings/snapshot path to playback, ordinary overlays, and the MIDI editor. Carry the source (default/tap/remote), interval/origin, effective divisions, and subdivision information needed by those consumers. Do not substitute a finer grid step for GrainSamps. Ensure per-station/take/loop offsets and subdivision settings compose once, with the same boundary calculation used for hit testing and playback.

Keep remote authoritative BPM/BPI and local grain distinct. Define local-tap behavior while following NINJAM explicitly: retain remote authority and alter permissible local subdivisions, or reject incompatible local geometry changes with a clear reason. Do not let a local propagation helper accidentally clear the accepted remote descriptor (`SetMidiGrain` currently clears it). Regression-test NoSync, reclock, and saved-session load.

### 3. Ctrl edit mode: target scope, visibility, and transitions

Use the active selection depth to resolve targets: selected items win; otherwise use the relevant hovered item; otherwise global. Hover must not influence selected edits. Capture scope when Ctrl is pressed and keep it stable throughout the gesture, matching the current documented interaction. Represent multiple selected targets explicitly and show mixed values where necessary.

Investigate the current loop-to-take promotion. The request includes station/take/loop edits: implement actual loop-local semantics where supported, or extend the owning engine settings so loop depth does not silently change siblings. Preserve the Station -> LoopTake -> Loop ownership boundaries.

Holding Ctrl should quickly ease in the edit overlay and ease out station/take/loop faders, master controls, and racks. At full edit-mode opacity only the applicable edit controls remain visible over the scene. On release reverse the transition more gradually. Proposed initial timings: 100-150ms entry, 350-500ms release, with a smooth easing curve and continuity on rapid repress/release.

Apply visibility to both rendering and interaction: faded/hidden racks and faders must stop taking clicks, scrolling, focus, and hover. Preserve layout and prior visibility when restoring them. Inspect `Scene.cpp:2668`, `GuiRack.cpp:284`, and entity Draw3d paths; existing master-control visibility alone is insufficient.

Keep E-editor Ctrl note movement, pan/zoom, undo/redo, text entry, and existing Ctrl shortcuts usable. Route modifiers centrally so key state updates are not swallowed by the editor, while contextual gestures retain their intended precedence.

### 4. Overlay styling and drag feedback

Retain the blue/green shift and orange/red division scope colours. Add a flat translucent grey panel with a subtle border. Each coloured handle gets a clean rounded outline, a restrained gradient with higher opacity at its centre, and an uppercase caption above it: SHFT and DIV. Use existing fonts/rendering resources; prefer code/shader geometry over new bitmap assets for these simple shapes.

On press or drag, show only the active handle and its caption; hide all other edit-overlay controls. Keep the target and pointer capture stable even if Ctrl is released mid-drag. At gesture end restore the appropriate controls according to Ctrl state, and fade the grid normally.

Print the live value in the bottom status bar from initial press through movement: scope plus signed offset in ms (and samples where useful), or exact subdivision fraction/division count. For multiple targets, show mixed/range information instead of inventing a single value. Reuse the HUD status area (`JammaLib/src/gui/GuiHud.cpp:628`, `:891`) with explicit temporary feedback ownership, so routing/editor messages recover after the gesture. Handle Escape, lost capture/focus, target deletion, and cancellation without stuck hidden controls.

### 5. Main panel: triplets and useful labels

Add Tap tempo (Space) beside subdivision radio choices. Route mouse taps and keyboard taps through the same timestamped engine action. Label the metronome separately, e.g. Metronome.

Keep existing straight fractions and add triplet choices beside tap tempo, initially 1/3, 1/6, and 1/12 of the grain/beat reference; display the reference clearly. Calculate boundaries directly using rounded interval/division arithmetic, including non-integral sample spacing. Preserve existing persisted enum values and packed fields; use an explicit display/drag ordering rather than assuming ordinal order equals resolution.

A triplet lattice permits a shuffle pattern by placing notes at the first and third triplet positions (2:1 spacing). Uniform triplet quantisation does not itself create alternating swing. Provide the requested triplet choices and verify that shuffle placement is possible; do not advertise an automatic swing amount unless an alternating-boundary mode is actually implemented.

### 6. Space and gesture grids: independent state

Move hold-to-show quantisation grids from Ctrl to Space. On a physical Space keydown, hold the grids visible and register one tap; ignore auto-repeat. On keyup, release the hold and begin the normal grid fade. Mouse taps pulse the grids. Ctrl alone must not hold or pulse them.

Use a 3s inter-press timeout (the current tracker uses 2.5s). An isolated press after a longer gap starts a fresh sequence without applying a tempo change. Measure press-to-press times, not release times. Holding Space generates no additional taps, and release is not another tap. Cover the exact timeout boundary and zero/non-increasing timestamp cases.

While a handle is pressed/dragged, hold the grid visible; releasing it starts the same fade as Space. Compose Space-held and gesture-held reasons so releasing one cannot hide a grid still required by the other. Keep grid alpha separate from Ctrl panel alpha. Reset held state on focus loss and editor/session teardown.

### 7. Explainable diagnostics

Log at INFO for recording/master establishment, each tap input/result, accepted grid changes, and rejected/no-op outcomes. Include:

- Recording/take identity, completed-local-take count, reclock generation, authority, and source (default, tap, remote).
- Physical original length and logical master length, both samples and ms; sample rate.
- Grain samples/ms, grains per master, inferred musical BPI, effective BPM.
- Configured defaults/policy, raw and smoothed tap gaps in samples/ms, requested BPI/divisions, selected candidate, rounding/tie rule, exact subdivision fraction, effective division count.
- Before/after state, reason (first tap, timeout reset, MIDI master discovered, sole-loop inference, frozen geometry, no master, invalid geometry, unchanged), and publication/applied identity when crossing the audio boundary.

Distinguish local grain count from musical grid divisions and remote BPI; do not print all three as an ambiguous seeds value. Example: a 2000ms master tapped at 500ms requests 4 beats; logs should say why 4 was chosen and what fraction makes the displayed grid 4, 8, 12, or 16 cells. Include enough context to diagnose first-loop inference from defaults without taps.

Keep formatting and output on the job/UI side. If callback-owned application needs confirmation, publish a bounded receipt and format it off callback. Rate-limit drag traces and emit a final summary; avoid per-frame/cell/event spam.

## Execution order and subagents

1. **Serial contract and reproduction pass:** primary agent traces Space routing, MIDI-only master establishment, grid ownership, persistence, and loop-local scope. Add failing integration tests for the reported first-loop/editor path and multiple-loop propagation. Settle the pattern-remapping and authority contracts before splitting implementation.
2. **Serial engine foundation:** fix master discovery and sole/multiple-loop policy; implement coherent effective grid publication, live MIDI pattern preservation, triplet representation, and decision diagnostics. Build JammaLib and run relevant native tests. Apply threading-review to changed publication/callback boundaries.
3. **Parallel after contracts stabilise:** one implementation agent handles Ctrl/Space/drag state and visibility; another handles handle rendering and HUD feedback, with disjoint file ownership where possible. The primary handles main-panel tap/subdivision controls and persistence integration. Quantiser.cpp contains both engine and controller code: designate one writer or run these edits serially.
4. **Serial integration:** combine keyboard/editor routing, button actions, transitions, and grid consumers; review scope/authority preservation and cancellation paths.
5. **Serial acceptance:** native tests, incremental app build, interactive visual/behavior verification, and documentation updates. Finish with a sample diagnostic trace explaining default first-loop inference and both tap modes.

Use small Luna subagents for narrowly scoped code research, each returning exact file/line references. Suggested research tasks: (a) timing/geometry/remote authority and tests, (b) keyboard/editor/target/visibility, (c) rendering/status/persistence compatibility. Keep implementation ownership explicit and reserve the primary agent for integration; do not have several agents edit Quantiser.cpp simultaneously.

## Validation and completion checklist

- MIDI-only first recording, audio first recording, and multichannel audio+MIDI take: tap by Space and mouse, including with E editor open. Step count changes live; source material and the musical occupancy pattern survive repeated finer/coarser changes and return to original resolution.
- Record a second take: snapshot master length, grain, grains-per-master, all loop lengths/cursors and offsets; taps change displayed/playback subdivisions while those construction values remain fixed.
- Verify straight/triplet boundaries and shuffle note placement at 44.1/48kHz, fractional steps, interval endpoints, offsets, multiple MIDI channels, and unequal/non-dividing loop lengths.
- Test snapshot/pack/save/load compatibility, note durations/order, editor undo/redo, and cancellation of an edit during a grid change. Extend MidiGridGesture and LoopGridGeometry tests as needed.
- Ctrl: selection beats hover at every depth; hover-only and global fallback; multiple selections; stable drag targets; true loop scope; racks/faders disabled while hidden and restored afterward.
- Space: physical tap vs auto-repeat/hold; no tempo change after >3s; keyup/focus loss clears hold; simultaneous Ctrl, Space, and handle drag states compose correctly.
- Handle press/drag: only active handle visible, grid remains visible, status updates on press and movement, release/cancellation restores controls and fades grids.
- Check overlay at small/large windows and DPI scales: rounded border, legible labels, subtle centre gradient, clean background, no clipping or stale hover/click regions.
- Re-run relevant timing/MIDI/NINJAM/LoopTake tests; manually audit callback changes against doc/realtime-audio.md.
- Before every build/native-test run, read this worktree's .vscode/tasks.json. Use its actual commands and doc/build.md troubleshooting; incremental Build of affected projects only, absolute SolutionDir with exactly one trailing backslash. Report unavailable dependencies/tasks instead of guessing paths.
- Update doc/overlay-controls.md and doc/loop-grid-editor.md to describe the implemented shortcuts, scope, subdivisions, and transitions.
- This planning session has not built the app, run native tests, or verified the live UI. The implementation session must produce that evidence.

## First prompt for the fresh session

> Work in C:\Users\matto\source\repos\Jamma.worktrees\Jamma-quantisation-enhancements on feature/quantisation-enhancements. Read AGENTS.md and docs/quantisation-enhancements-plan.md, then execute the plan through implementation and validation. Use small Luna subagents for code research with exact file/line references, and assign disjoint implementation ownership only after the timing/grid contract is settled. Start by reproducing or adding failing integration coverage for Space being swallowed by the open MIDI editor, MIDI-only master discovery, and multiple-loop taps failing to propagate to grid consumers. Preserve recorded patterns, local grain geometry once additional loops exist, remote timing authority, and real-time safety. Then implement triplet/tap controls, Ctrl/Space/drag visibility and transitions, polished handles, status feedback, and explainable logging in the plan's order. Read local .vscode/tasks.json before every build/test run, use incremental affected-project builds, and finish with tests, interactive evidence, and updated user docs.

