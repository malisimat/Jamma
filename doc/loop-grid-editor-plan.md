# Loop grid editor: implementation and orchestration plan

## Outcome and scope

Open one selected, completed audio `Loop` or `midi::MidiLoop` in a responsive 3D grid editor. Keep the loop near its current scene position and orientation while its ring unwraps into a flat surface. Move the perspective camera into an almost top-down view, retaining visible 3D depth. Leave surrounding objects in place, dim them, and disable their picking. Reverse the transition on exit. Audio is view-only; MIDI supports mouse editing, preview, one commit per gesture, undo/redo, and persistence.

Edit one loop, even when its `LoopTake` contains several. Do not change transport, recording, overdub, audio content, automation, remote-follow policy, or existing quantisation-control gestures. Keep edit operations separate from mouse input so another interface can use them later. Avoid a general editor framework or a second note data model.

## Instructions for the implementing orchestration session

1. Work on `feature/loop-grid-editor`. Record baseline commit and `git status`; preserve unrelated changes. Read `AGENTS.md`, this plan, `doc/glossary.md`, `doc/realtime-audio.md`, `doc/loop-alignment-and-ninjam-sync.md`, and `doc/build.md`. Before **every** build or native test run, read `.vscode/tasks.json` and use its applicable command/tool path. If it has no applicable command, report that limit instead of guessing. Build only affected projects, incrementally.
2. Run one sustained session through every phase and the final review. The main agent owns architecture, cross-subsystem changes, integration, review decisions, and commits. Spawn subagents for bounded code research: MIDI publication/lifetime, scene input/camera, graphics/shaders, and test/build conventions. Request file-and-line evidence and have research agents avoid edits. Reconcile findings centrally before choosing an implementation. If implementation is delegated, assign disjoint files and precise contracts; review the integration centrally.
3. Keep a durable progress log in the session or a temporary file: baseline, current phase, decisions and evidence, changed files, build/unit-test results, review findings/fixes, phase commit hashes, and next action. After interruption, compaction, or agent failure, re-read it and `git status`; resume without repeating completed work. Diagnose routine failures, fix them, rerun focused unit tests, and review again. Raise only genuine external blockers.
4. Each phase includes implementation, focused native **unit** tests, and code review. Review correctness against this plan and the original spec, ownership boundaries, UX, lifecycle, and thread safety. Fix findings and repeat the gate until none remain. Commit that phase to the feature branch only then, staging only its files. Record the hash. Do not use Clean/Rebuild, a solution build, or broad suites without a concrete reason. UI automation, hardware, and end-to-end testing are not phase gates.
5. Prefer the simplest solution that gives the best local editing experience and visual result. Make surgical changes, match conventions, keep engine behavior in JammaLib and Scene glue thin. Avoid anonymous namespaces, per-frame CPU remeshing, scene-wide transform changes, and new synchronization schemes where published immutable snapshots fit.
6. After phase 5, perform one final cross-phase review. Fix and commit remaining findings, confirm unit-test evidence and feature worktree status, then create a self-contained HTML summary in a newly created temporary directory. State precisely what works, mouse behavior, phase commits, exact build/unit-test commands and results, review conclusions, and any remaining concerns or work. Open the HTML in a browser and report its path. Strive to finish the whole feature in this major session; report honestly if a real blocker remains.

## Product and interaction contract

### Enter, view, and exit

- Offer a discoverable action on a selected loop and a visible close affordance. Escape also exits. Disable entry for recording, incomplete, or zero-length loops with a brief explanation.
- Track a stable loop and take identity with weak ownership or equivalent validation. If either is removed or replaced, cancel any gesture and exit safely. At most one editor exists. Restore the exact entry camera pose/view, selection depth, opacity, picking, and pointer behavior, even after interrupted transitions.
- Keep the loop approximately in its current screen location and local orientation while unwrapping. Allow a small placement/scale adjustment for legibility. Move the camera to read the plane from above, retaining perspective and surface thickness. Do not move other loops, takes, or stations; dim them in place and keep them from intercepting input.
- Start gestures only after the camera and morph settle. On resize, recompute projection and pointer ray to the editor plane; do not change time/pitch coordinates. Initially frame existing MIDI notes with padding and a useful empty-range fallback. Provide mouse wheel pitch scrolling or zoom so all 128 rows remain reachable, with readable labels and hit targets.

### MIDI mouse behavior

- Derive time cells from the selected take's **resolved** MIDI quantisation, its transport start, and the selected loop's own length. Use the same integer boundaries for grid lines, pointer hit testing, and playback preview. Include local phase offsets and the accepted remote descriptor. The current visual overlay lacks that full descriptor (`doc/loop-alignment-and-ninjam-sync.md`); it is not the editing authority. If quantisation is off or a grid cannot be resolved, use free editing and indicate that state.
- With quantisation active, pressing an empty `(pitch, time cell)` starts add paint; pressing an occupied cell starts erase paint. One drag applies the initial action to every crossed adjacent cell and pitch row, including cells skipped between pointer samples, and visits each cell once. Cross the loop seam deterministically. Occupancy means displayed, quantised coverage at the cell midpoint; overlapping notes need deterministic treatment. Add/remove that cell's coverage without unintentionally changing adjacent cells; split/coalesce spans when needed. New notes use one-cell duration, a conventional velocity, and the selected/default MIDI channel. Preserve unaffected notes' channel/velocity and all non-note events. Reject an unrepresentable or overflowing gesture atomically with feedback.
- With quantisation off, click empty space to create a note at that sample and pitch with a modest default duration. Drag near the left or right edge to trim that edge, or drag the body to move in time and pitch. Give edge hit zones a minimum screen-pixel width. Keep positive duration, clamp pitch to 0–127, and use precise sample coordinates. Specify deterministic hit precedence for overlaps and retain selected note/channel identity through a drag. Respect the existing `MidiNote::ExtractSpans` seam convention.
- Pointer ownership lasts from press to release/cancel and suppresses scene pan, background selection, picking, and quantisation overlay gestures. Show hover and drag preview without publishing on every move; commit once on release. Escape, lost capture, loop invalidation, grid change, or interrupted transition cancels without a source edit or undo entry. Show commit/rejection feedback. Existing undo/redo reverses/reapplies one complete gesture; save/export sees accepted raw events.
- Quantisation stays non-destructive: disabling it exposes the edited raw source. Define and unit-test translation from displayed quantised cells to source events. If that translation is ambiguous for a particular gesture, reject it atomically instead of changing unrelated notes.

### Visual design and performance

- Use a coherent restrained palette, crisp major/minor grid lines, a low-contrast surface with depth/bevel, readable pitch/time labels, velocity-sensitive note color/height, and a distinct hover/preview. Keep selected content legible over the dimmed scene. Audio's min/max envelope retains existing waveform texture sampling and reads as an elevated ribbon or relief above the time grid.
- Give the playhead a shader-driven luminous sweep: narrow bright core, soft falloff, subtle pulse/trail. Derive time from the selected loop's **own** playback phase, not master phase or scene sample count. It must remain readable during ring, morph, and grid states without masking content. Bound transparency/overdraw and use uniforms rather than CPU geometry updates per tick. Handle pause, wrap, zero length, and GL context restoration.
- At morph 0 preserve the existing ring appearance and automation visuals; at 1 show a rectangular time-by-pitch MIDI grid or time-by-amplitude audio surface. Separate `u=0` and `u=1` vertices: coincident on the ring, opposite grid edges. Fixed tessellation lets long note spans follow the ring during morph. Keep loop coordinates independent of camera pose and world orientation stable. Cover wide/narrow aspect ratios and resize with pure geometry/camera unit tests where practical.

## Technical design and safety

### Geometry, camera, and input

Use a small pure mapping contract for loop-local `u` (sample/time in `[0, loopLength)`) and vertical position (MIDI pitch or audio amplitude), shared by shader inputs, grid drawing, and hit testing. Waveform and MIDI need not use identical vertex formats: waveform already samples a 1D texture; MIDI uses note instances. Preserve these paths, adding only required attributes/uniforms. Compute ring and grid positions from unchanged local inputs and interpolate one morph value. Reuse existing scene/model placement and opacity where possible. `graphics::Camera` already has `TopDown`, remembered poses, and transitions; do not overwrite the user's remembered TopDown pose. Save the actual entry pose and selection depth for exit.

### MIDI source and publication

`MidiLoop` currently has mutable `_events`/`_eventCount` read by callback playback, plus a raw pointer to retained **quantised** buffers. Mutating `_events` and merely republishing quantised events would race with playback. Retaining every edit buffer forever would also grow memory. Before live editing, establish a coherent immutable playback snapshot covering raw/quantised events, count, and revision, with bounded callback-safe lifetime/reclamation. Prepare complete buffers off the callback and publish atomically at a safe boundary. Keep recording/MIDI ingress ownership distinct: edit only completed, non-recording loops and serialize edits with take finalisation/merge/restore and quantisation updates on their established owner thread. Never make the callback allocate, copy, lock, wait, destroy shared ownership, or read a half-updated count/source. Document owner, readers, writers, publication, retirement, and teardown for every new cross-thread field.

Represent each edit as a source-revision-checked transform of raw events. Identify a target note robustly within that revision, including duplicate pitch/channel/time events; preserve note-on/off pairing, canonical same-sample ordering, velocities, channels, non-note events, automation, and loop length. Validate timestamps, positive durations, capacity, state, and revision before publication. Stale/invalid/overflowing gestures reject as a whole. Refresh the model from the accepted snapshot. Undo/redo uses the same safe publication path and rejects an invalidated revision. Review held-note behavior explicitly: removing, moving, or trimming a sounding note must not leave a stuck note or create an unmatched off at wrap.

Use `MidiQuantisation::BoundarySampleAt`, resolved take settings, and `MidiQuantisationTransportStartSamps`, not `StepSamps` or a separate float grid. Cover non-dividing loop lengths, phase offsets, remote origin, unequal master/loop lengths, and a grid change while the editor is open. Cancel an active gesture when its grid changes; keep display and hit mapping coherent.

### Review and verification

Only native unit tests are required. Place pure mapping/transform tests beside existing JammaLib tests; use bounded fake sinks for playback and held-note checks. Unit tests must assert behavior and boundaries rather than mirror implementation. After shared-state or hot-path edits, run `.agents/skills/threading-review/audio-hotpath-audit.ps1` and manually inspect the callback-owned functions in `doc/realtime-audio.md`; the script alone is not proof. Review source/playback lifetimes, callback bounds, model publication, GL thread ownership, transition cancellation, and raw-source save behavior.

## Phases and commit gates

### Phase 1 — Contracts and pure helpers

Implement compact coordinate/grid helpers for sample-to-`u`, pitch rows, integer boundaries, traversed cells, seam, hit zones, and note-operation validation. Establish exact edit owner/thread and playback snapshot lifetime from code research; document invariants next to relevant declarations or here. Add focused unit tests for endpoints, zero length, nonzero phase, non-dividing length, remote origin, skipped/crossed cells, duplicates, wrap, and resize math. Review against `MidiQuantisation` and loop-local phase ownership; fix, incrementally build, run focused unit tests, and commit.

### Phase 2 — Editor state, camera, and routing

Add single-loop editor state in `engine::Scene`, entry/exit camera handling, entry/close UI, lifetime validation, pointer ownership, and reversible visual/picking overrides. Keep other transforms fixed. Unit-test state transitions, cancellation, removal, input routing, camera restoration, and interrupted transition through testable state seams. Review all exit paths and existing selection/quantisation gestures; fix, build, run focused unit tests, and commit.

### Phase 3 — Unwrap and visual finish

Extend `graphics::LoopModel`, `graphics::MidiModel`, and shaders with fixed-geometry ring-to-grid morph, grid/depth styling, loop-phase playhead glow, and preview drawing. Preserve the ring endpoint, waveform texture, and automation rendering. Unit-test morph endpoints, seam, long-note tessellation inputs, phase wrap, and camera/viewport math. Review GL lifecycle, picking geometry, overdraw, transitions, and exact ring endpoint; fix, build, run focused unit tests, and commit.

### Phase 4 — Safe MIDI edit publication and undo

Implement off-callback revision-checked edit preparation, immutable raw/quantised playback publication and retirement, model refresh, save/export visibility, and one undo record per gesture through existing history. Audio remains view-only. Unit-test atomic acceptance/rejection, capacity, stale revision, same-sample order, duplicates/channels, non-note preservation, raw/quantised divergence, undo/redo, save snapshot, old/new callback snapshot consistency, and held notes. Run threading audit and manual hot-path review; fix, build, run focused unit tests, and commit.

### Phase 5 — Mouse editing and integration

Connect gesture state/preview to edit operations. Implement quantised add/erase paint over all traversed cells/pitches and free create/move/edge trim, seam, cancellation, feedback, and grid-change invalidation. Do not publish during pointer moves. Unit-test skipped/revisited cells, filled/empty start, overlaps, wrap, clamping, lost capture, grid change, and one undo step. Review against the product contract, thread ownership, routing, stale state, and phase 4 publication; fix, build, run focused unit tests, and commit.

### Final cross-phase gate

Inspect the accumulated feature commits against this plan and original spec. Check enter/edit/undo/save/exit, selected-loop phase versus master phase, audio/MIDI views, callback safety, scene restoration, and evidence for every acceptance claim. Run only focused unit tests needed to close remaining risk. Fix, review, and commit any findings. Confirm no uncommitted feature changes. Generate and open the HTML summary, then report the final commit and honest limits.

## Relevant existing code

- Scene/camera: `JammaLib/src/engine/Scene.cpp`, `JammaLib/src/engine/Scene.h`, `JammaLib/src/graphics/Camera.cpp`, `JammaLib/src/graphics/Camera.h`.
- Waveform: `JammaLib/src/graphics/LoopModel.cpp`, `Jamma/resources/shaders/waveform.vert`.
- MIDI rendering: `JammaLib/src/graphics/MidiModel.cpp`, `JammaLib/src/graphics/MidiModel.h`, `Jamma/resources/shaders/midi_note.vert`.
- MIDI source/spans/grid: `JammaLib/src/midi/MidiLoop.cpp`, `JammaLib/src/midi/MidiLoop.h`, `JammaLib/src/midi/MidiNote.h`, `JammaLib/src/midi/MidiQuantisation.h`.
- Take/undo: `JammaLib/src/engine/LoopTake.cpp`, `JammaLib/src/engine/LoopTake.h`, `JammaLib/src/actions/ActionUndoHistory.cpp`, `JammaLib/src/base/ActionUndo.h`.
- Ownership/timing/real-time guidance: `doc/glossary.md`, `doc/loop-alignment-and-ninjam-sync.md`, `doc/realtime-audio.md`.
