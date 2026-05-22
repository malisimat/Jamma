# PRD: MIDI Loop 3D Visualization

## Summary

Add a display-only 3D visualization for recorded MIDI notes stored in `MidiLoop` instances.

Each `MidiLoop` will own a `MidiModel` responsible for rendering that loop's notes as 3D arc segments arranged in the same loop-centric visual language as the existing audio `LoopModel`. `LoopTake` will arrange `MidiModel` instances as additional concentric rings alongside the existing audio loop rings.

This is an MVP visualization feature only. It does not include MIDI editing, note interaction, or persistence.

## Problem

Recorded MIDI currently exists in the engine as in-memory `MidiLoop` event data, but there is no scene-level representation of that data. Users cannot see what has been recorded, inspect timing at a glance, or visually correlate MIDI note content with the existing 3D loop presentation.

## Goals

- Display recorded MIDI note content in the 3D scene.
- Reuse as much of the existing loop rendering path as practical.
- Keep all MIDI recording and playback hot paths real-time safe.
- Make the visualization feel native to the current loop-ring scene.
- Support live visual updates while recording, using throttled non-real-time geometry rebuilds.
- Include MIDI notes in the picker pass, so hovering / clicking notes selects the Loop / LoopTake / Station.

## Non-Goals

- MIDI note editing.
- Individual note selection, or manipulation.
- Save/load persistence for MIDI loop data or MIDI visualization state.
- A separate dedicated MIDI editor or alternate view.
- A full custom rendering system outside the existing model/render pipeline.

## Product Decisions

### Placement and ownership

- MIDI notes are shown inside the existing loop-oriented 3D scene, not in a separate view.
- Each `MidiLoop` owns a `MidiModel` that renders only that `MidiLoop`.
- `LoopTake` is responsible for arranging `MidiModel` instances as additional concentric rings.
- The MIDI visualization should feel like a sibling of `LoopModel`, not a detached inspector.

### Visual metaphor

- A note is rendered as a 3D arc segment spanning the note's held duration.
- The arc should read like a partial section of a ring with visible thickness.
- All notes use the same thickness in MVP.
- Pitch is represented by vertical offset.
- Velocity affects color only.

### Time and playback alignment

- Arc position around the ring represents time within the loop.
- `MidiModel` rotates in sync with loop playback using the same time orientation convention as `LoopModel`.
- Notes with no matching `NoteOff` before loop end are clamped to the loop end.

### Interaction scope

- MVP is display-only.
- `MidiModel` participates in all render passes (scene, picker, highlight).

### Persistence scope

- MVP is in-memory/session-only.
- Persistence is explicitly out of scope and should be considered follow-up work.

## User Experience Requirements

### Core experience

- When a user records MIDI into a `MidiLoop`, note content becomes visible as 3D arcs in the loop scene.
- The MIDI arcs should visually align with the take's existing concentric loop layout.
- The user should be able to understand note timing, note duration, pitch contour, and relative velocity at a glance.

### Live recording behavior

- While recording, the MIDI visualization updates live.
- Live updates must be throttled and performed outside the real-time MIDI ingest path.
- The geometry must always be finalized when recording ends.

### Readability

- Rings must remain legible when a take contains multiple loops and multiple MIDI loops.
- Velocity coloring should use a fixed continuous gradient from low velocity to high velocity (can use texture like 'Levels.tga' if simpler).
- MIDI arcs should remain visually subordinate to scene readability rather than becoming dense debug geometry.

## Functional Requirements

### Data interpretation

- `MidiModel` must derive note spans from `NoteOn` and `NoteOff` event pairs.
- The model build step must interpret raw `MidiEvent` storage into visual note instances.
- Unmatched note starts at the loop end must be clamped to the loop boundary.
- Events outside the playable loop window must not produce visible note geometry.

### Scene integration

- `LoopTake` must own and arrange MIDI loops shown as visual rings in the same centered concentric layout style used for audio loops.
- MIDI rings must share the same loop-progress rotation convention as audio rings.

### Rendering model

- `MidiModel` must be built on top of the existing `GuiModel`-style rendering path.
- The implementation must include a small generic instancing upgrade so models can provide per-instance data buffers.
- `MidiModel` should use instanced geometry rather than expanding fully unique CPU-side mesh data per note.

### Geometry requirements

- The per-note visual primitive should be a static base mesh or similarly reusable primitive transformed per note instance.
- Per-note instance data must support at least:
  - start angle or equivalent time-start parameter
  - angular span or equivalent duration parameter
  - vertical pitch offset
  - velocity-derived color parameter
- Vertex shader logic should participate meaningfully in shaping the final note geometry.
- Height and positioning should be influenced in the vertex shader, not only by CPU-authored baked geometry.

## Technical Direction

### Reuse strategy

- Reuse the current `GuiModel` / shader / `Draw3d` path wherever possible.
- Reuse the current loop-ring scene conventions for placement and motion.
- Prefer a generic rendering-path improvement over `MidiModel`-specific rendering hacks.

### Recommended rendering approach

Recommended MVP approach:

- Add a small generic extension to `GuiModel` for instance-data buffers and instanced attributes.
- Create a reusable base arc mesh or arc-prism mesh for one note body.
- Use instanced draws where each note instance provides span, pitch offset, and color-driving data.
- Use the vertex shader to place and deform the base arc into final world shape.
- Keep fragment shader work lightweight, primarily for shading and velocity-gradient color output.

Acceptable implementation exploration:

- Stretching a reusable arc mesh azimuthally in the vertex shader.
- Using shader-driven trimming or masking if it materially simplifies geometry generation.
- Geometry shader usage, if supported by existing openGL and common hardware (avoid anytthing that is potentially unsupported on a typical consumer laptop).

Not preferred for MVP:

- Rebuilding a unique dense triangle mesh for every note on every update.
- Forcing the feature through uniform-only instancing without per-instance data support.
- Building a fully separate renderer outside the current scene/model stack.

## Real-Time and Performance Constraints

- No heap allocation in MIDI hot paths.
- No blocking or locks in MIDI ingest or playback hot paths.
- No geometry rebuilds in the real-time callback path.
- MIDI recording should continue using fixed-capacity in-memory event storage.
- Geometry generation and note-pair interpretation must happen on a safe non-real-time path.
- Live updates must be throttled to bounded rebuild checkpoints.

## Architecture Notes

### Proposed ownership

- `MidiLoop`
  - continues to own MIDI event storage and recording/playback state
  - owns a `MidiModel` used only to visualize that `MidiLoop`
- `MidiModel`
  - transforms raw MIDI events into displayable note instances
  - owns render geometry resources and note-instance data
- `LoopTake`
  - owns the set of `MidiLoop` instances
  - arranges corresponding `MidiModel` rings concentrically in the scene

### Deliberate scope boundary

This feature should not force a false ownership link between audio `Loop` and MIDI `MidiLoop`. Audio loops and MIDI loops may coexist in the same `LoopTake` visual stack, but the MIDI visualization remains driven by `MidiLoop` data and `LoopTake` arrangement.

## Acceptance Criteria

### Product acceptance

- Recording a MIDI loop causes visible MIDI note arcs to appear in the 3D scene.
- Note arcs represent note duration, not just note-on hits.
- Pitch is visible as vertical displacement.
- Velocity is visible as color variation across a continuous gradient.
- MIDI rings rotate in sync with loop playback.
- Multiple MIDI loops in a take appear as additional concentric rings.
- The feature is display-only and does not expose note interaction.

### Technical acceptance

- No new allocations or blocking are introduced in MIDI recording or playback hot paths.
- Geometry rebuilds occur only on non-real-time paths.
- Live updates are throttled during recording and finalized at record end.
- `MidiModel` uses the generic instanced rendering path rather than one-off custom draw code.
- Scene rendering remains correct including picker/highlight integration for MIDI arcs.
- Code duplication minimalised, for example common code in LoopModel and MidiModel to be extracted.

### Edge-case acceptance

- Notes without a matching `NoteOff` before loop end render as arcs clamped to the loop boundary.
- Loops with no note content produce no MIDI note geometry.
- Dense note content remains stable and legible enough for MVP without crashing or stalling rendering.

## Suggested Implementation Slices

### Slice 1: Generic instanced model support

- Extend `GuiModel` to support per-instance attribute buffers.
- Add the minimum shader/uniform plumbing needed for instance-driven model deformation.
- Keep the extension generic so it can support future scene features.

### Slice 2: Note extraction and visual instance model

- Add a non-real-time note pairing/build step from `MidiEvent` sequences.
- Define the per-note instance payload for start, duration, pitch, and velocity.
- Add tests for note pairing, clamp-at-loop-end behavior, and skipped out-of-window events.

### Slice 3: `MidiModel`

- Implement `MidiModel` as a sibling concept to `LoopModel`.
- Add shaders and base geometry for note-arc rendering.
- Support velocity-driven gradient color and pitch-driven vertical placement.

### Slice 4: `MidiLoop` and `LoopTake` scene integration

- Add `MidiModel` ownership to `MidiLoop`.
- Arrange `MidiModel` instances in `LoopTake` as concentric rings.
- Synchronize playback rotation with the existing loop progress convention.

### Slice 5: Live update pipeline

- Add throttled non-real-time rebuild scheduling during recording.
- Ensure final geometry rebuild at record end.
- Verify no hot-path regressions.

## Testing Guidance

Prefer a TDD style implementation, where failing tests are written FIRST, and then implementation follows such that test passes.  Refactoring to clean up can be done alongside or after.

Planning should include test coverage for:

- note-on/note-off pairing into duration spans
- unmatched note clamping at loop end
- live update throttling behavior
- MIDI rings rotating with the same phase convention as audio loop rings
- stability under dense note counts
- no regression to MIDI real-time safety guarantees

Rubber duck review after each vertical slice / task is completed.  Code committed to git after passing tests.

## Follow-Up Work

Not part of this PRD, but expected future candidates:

- persistence and load/save of MIDI loop content
- picker/highlight integration for MIDI visuals
- individual note hover or inspection UX
- MIDI overdub and punch-specific visualization behavior
- channel-aware styling or alternate color modes
