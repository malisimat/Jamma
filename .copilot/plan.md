# Feature: improve-midi-loop-rendering

## Summary
MIDI loops / loop takes are hard to select. Today the only geometry a MIDI loop
contributes to the picker render pass is its 3D note events, so a loop with few (or
zero) notes is effectively un-hoverable. Add a semi-transparent disc at the height of
middle C — rendered **fully opaque in the picker pass** and with **some vertical
thickness** — so every MIDI loop always presents a large, reliable hover/select target
(e.g. to open its VST editor window).

Ref: https://github.com/malisimat/Jamma/issues/103

## Goals & Acceptance Criteria
- A semi-transparent disc is visible for every MIDI loop, centred on the middle-C plane.
- The disc has visible vertical thickness (it is a solid 3D ring/disc, not a flat plane).
- Hovering the disc highlights the loop, and clicking it selects the loop (same behaviour
  as hovering a note today), including for MIDI loops with **no notes**.
- The disc renders **opaque** in the picker pass so picking is reliable.
- No regression to MIDI note rendering, model building, or audio-thread behaviour.

## Scope
### In scope
- MIDI loop visual model (`MidiModel`) instance generation.
- The `midi_note` vertex/fragment shaders (scene + picker + highlight paths).
- `MidiModelParams` tuning fields for the disc.

### Out of scope
- Station rendering, audio (`LoopModel`) rendering, quantisation overlay rendering.
- Selection/hover routing logic (the disc reuses the existing note `ObjectId`, so the
  existing picking pipeline already resolves it).

## Background / Current Architecture (researched)

### Rendering data flow
- `engine::MidiModel` ([JammaLib/src/graphics/MidiModel.cpp](JammaLib/src/graphics/MidiModel.cpp),
  [.h](JammaLib/src/graphics/MidiModel.h)) derives from `gui::GuiModel` and draws MIDI
  notes using **instanced** geometry. A single shared base mesh
  (`BuildBaseVerts`/`BuildBaseUvs`, a 16-segment arc tube) is drawn once per note via
  `glDrawArraysInstanced`.
- Per-instance attributes (see [MidiModel.cpp L17-18](JammaLib/src/graphics/MidiModel.cpp#L17-L18)
  and `BuildInstanceData` [L131-L192](JammaLib/src/graphics/MidiModel.cpp#L131-L192)):
  - **location 3** `InstanceTimePitch` = `vec4(startFrac, durationFrac, pitchOffset, velocity)`
  - **location 4** `InstanceShape` = `vec4(baseRadius, radialThickness, noteHeight, 0.0)`
    — the **`w` component is currently unused/zero** and is available as a discriminator.
- `MidiModel::Draw3d` ([L84-L116](JammaLib/src/graphics/MidiModel.cpp#L84-L116)) sets a
  `RenderMode` uniform per pass: `0` = scene, `1` = picker (writes `ObjectId`, opaque),
  `2` = highlight. It then delegates to `GuiModel::Draw3d` which issues the instanced draw.
- The vertex shader [Jamma/resources/shaders/midi_note.vert](Jamma/resources/shaders/midi_note.vert)
  positions each instance: `radius = InstanceShape.x + PositionIN.z * InstanceShape.y`,
  `y = pitchOffset + PositionIN.y * InstanceShape.z`, angle swept from `startFrac` over
  `durationFrac`.
- The fragment shader [Jamma/resources/shaders/midi_note.frag](Jamma/resources/shaders/midi_note.frag):
  - `RenderMode == 1` (picker): outputs `ObjectId` colour with **alpha 1.0** and returns early.
  - `RenderMode == 2` (highlight): outputs `Highlight` and returns early.
  - else (scene): velocity-graded colour at **alpha 0.88**.

### Why MIDI loops are hard to select (root cause)
`BuildInstanceData` returns `InstanceCount = 0` when `spans` is empty
([L141-L150](JammaLib/src/graphics/MidiModel.cpp#L141-L150)). With zero instances the
model draws nothing in any pass, so an empty or sparse MIDI loop contributes no picker
pixels.

### Why the disc "just works" for selection
- Each MIDI loop owns one `MidiModel`, attached in `LoopTake`
  ([LoopTake.cpp L908-L919](JammaLib/src/engine/LoopTake.cpp#L908-L919)) and added to
  `_children`, giving it a `GlobalId`. In the picker pass `MidiModel::Draw3d` writes
  `ObjectId = VecToId(GlobalId)+1` ([L95-L104](JammaLib/src/graphics/MidiModel.cpp#L95-L104)).
- Because the disc is part of the **same `MidiModel`**, it shares that `ObjectId`. Any
  pixel of the disc resolves to the same element the notes do, so the existing
  `Scene::_UpdateSelection` / `_ChildFromPath` hover/select pipeline
  ([Scene.cpp L1930-L2040](JammaLib/src/engine/Scene.cpp#L1930-L2040)) needs **no change**.

### Important constraints discovered
- **Picker pass has no depth test**: `Scene::Draw3d` enables `GL_DEPTH_TEST` only for
  `PASS_SCENE` ([Scene.cpp L332-L335](JammaLib/src/engine/Scene.cpp#L332-L335)). Picking is
  therefore painter-order based. A large filled disc could overwrite the picker pixels of
  other concentric loops drawn earlier. **Mitigation:** keep the disc a *localized annulus*
  around the note radius band rather than a full centre-filled plate, keeping its screen
  footprint close to the existing note geometry.
- **Blending is global**: `glEnable(GL_BLEND)` + `SRC_ALPHA, ONE_MINUS_SRC_ALPHA`
  ([Window.cpp L286-L287](JammaLib/src/graphics/Window.cpp#L286-L287)), so a semi-transparent
  scene-pass disc needs no new GL state.
- **Instance layout is fixed and reusable**: attributes 3 and 4 are already 4-component
  with `glVertexAttribDivisor(..., 1)` ([GuiModel.cpp L302-L308](JammaLib/src/gui/GuiModel.cpp#L302-L308),
  [L386-L392](JammaLib/src/gui/GuiModel.cpp#L386-L392)). Appending one extra instance and
  incrementing the count requires **no VAO/layout change**.
- **No audio-thread impact**: model building runs off the audio thread via
  `QueueModelUpdate` / `ApplyPendingModelUpdate` (atomic pointer swap,
  [MidiModel.cpp L194-L201](JammaLib/src/graphics/MidiModel.cpp#L194-L201)). The disc is
  appended during the same non-RT `BuildInstanceData` call — no new locking or allocation
  on any hot path.

## Proposed Approach
Add the disc as **one extra instance** appended to the existing instanced batch, tagged
via the spare `InstanceShape.w` component ("disc flag"). This keeps a **single draw call**,
reuses the existing VAO and the existing `ObjectId`, and automatically makes empty loops
selectable (instance count becomes ≥ 1).

- **Geometry:** reuse the note base mesh for the disc instance. Choose shape so it forms a
  flat annulus/disc centred on the middle-C plane (`pitchOffset = 0`, since
  `CenterPitch = 60` ⇒ `PitchOffset(60) = 0`):
  - `startFrac = 0`, `durationFrac = 1` ⇒ full 360° sweep.
  - `InstanceShape.x` (radius) and `InstanceShape.y` (radialThickness) chosen to place the
    annulus across/around the note radius band (e.g. radius ≈ `baseRadius`, thickness a
    modest fraction of `baseRadius`). Avoid a centre-filled plate to limit picker overdraw.
  - `InstanceShape.z` (height) = disc vertical thickness (≥ note height, so it reads as a
    solid ring).
  - `InstanceShape.w = 1.0` ⇒ disc flag.
- **Shaders:**
  - `midi_note.vert`: pass `InstanceShape.w` to the fragment shader as a `flat out float IsDisc`.
  - `midi_note.frag`: in scene mode, if `IsDisc > 0.5`, output a neutral semi-transparent
    colour (low alpha, e.g. ~0.18–0.25) instead of the velocity gradient. Picker
    (`RenderMode == 1`) and highlight (`RenderMode == 2`) paths are unchanged — the disc is
    already opaque in the picker because that path writes `ObjectId` with alpha 1.0.
- **Params:** add tuning fields to `MidiModelParams` (disc radius factor, radial thickness
  factor, vertical thickness factor, alpha) with sensible defaults so the disc is
  configurable without code changes.

## Discrete Implementation Steps

1. **Add disc tuning fields to `MidiModelParams`.**
   - File: [JammaLib/src/graphics/MidiModel.h](JammaLib/src/graphics/MidiModel.h#L13-L24).
   - Add `float DiscRadiusFactor`, `float DiscRadialThicknessFactor`,
     `float DiscHeightFactor`, `float DiscAlpha` (alpha can also be a shader constant; keep
     it here if we want runtime control).
   - File: [JammaLib/src/graphics/MidiModel.cpp](JammaLib/src/graphics/MidiModel.cpp#L38-L70)
     — initialise the new fields in both `MidiModelParams` constructors with defaults
     (e.g. radius factor `1.0`, radial thickness factor `~0.12`, height factor `~0.06`,
     alpha `~0.2`). Mirror the existing `if (...empty)` defaulting style.

2. **Append the disc instance in `BuildInstanceData`.**
   - File: [JammaLib/src/graphics/MidiModel.cpp L131-L192](JammaLib/src/graphics/MidiModel.cpp#L131-L192).
   - Restructure so that when `loopLengthSamps > 0` a disc instance is **always** emitted
     (even if `spans` is empty), while the early-return-with-zero-instances case applies
     only when `loopLengthSamps == 0`.
   - Compute `baseRadius` as today (the `70*log(len)-600` clamp). Push the disc instance
     first (or last — within-model order does not affect picking):
     - `timePitchData`: `0.0` (startFrac), `1.0` (durationFrac),
       `PitchOffset(CenterPitch) * baseRadius` (= 0 by default), `0.0` (velocity, unused).
     - `shapeData`: `baseRadius * DiscRadiusFactor`, `baseRadius * DiscRadialThicknessFactor`,
       `baseRadius * DiscHeightFactor`, `1.0` (disc flag).
   - Set existing note instances' `shapeData.w = 0.0` (already the case).
   - Update `InstanceCount` to include the disc.

3. **Vertex shader: forward the disc flag.**
   - File: [Jamma/resources/shaders/midi_note.vert](Jamma/resources/shaders/midi_note.vert).
   - Add `flat out float IsDisc;` and set `IsDisc = InstanceShape.w;`. No change to position
     math (the existing `radius`/`height` formulas already produce the annulus from the disc
     instance's shape values).

4. **Fragment shader: render the disc semi-transparent in the scene pass.**
   - File: [Jamma/resources/shaders/midi_note.frag](Jamma/resources/shaders/midi_note.frag).
   - Add `flat in float IsDisc;` (and a `uniform float DiscAlpha;` if driving alpha from
     params, else a const).
   - Leave `RenderMode == 1` (picker) and `RenderMode == 2` (highlight) paths untouched so
     the disc stays opaque in the picker.
   - In the scene branch, if `IsDisc > 0.5`, output a neutral colour (optionally modulated
     by `Diff` and `LoopHover` for hover feedback) at `DiscAlpha`.

5. **Verify hover/selection feedback for the disc.**
   - Confirm the existing `LoopHover` highlight tint reads acceptably on the disc when
     `_isPicking3d` is set; adjust the disc's hover term in the fragment shader if needed.
   - No code change expected in `Scene`/`LoopTake` — confirm by inspection that hovering the
     disc opens the VST editor path ([Scene.cpp `_TryOpenVstEditorForLoop`](JammaLib/src/engine/Scene.cpp#L300-L306)).

6. **Build and run tests.**
   - Build `JammaLib` then `Jamma` (targeted builds per repo conventions).
   - Build and run `test/JammaLib_Tests`. Add/extend a `MidiModel` test asserting that
     `BuildInstanceData` (via the public `UpdateModel` + `NoteInstanceCount` / `InstanceCount`)
     yields **≥ 1 instance for an empty loop with non-zero length**, and `0` for zero length.

7. **Manual visual verification.**
   - Record an empty/sparse MIDI loop; confirm the disc is visible, semi-transparent, sits
     at the middle-C plane with visible thickness, hovers/highlights, and selects (opens VST
     editor) reliably. Optionally dump the picker buffer (the commented
     `stbi_write_bmp` in [Window.cpp L384-L386](JammaLib/src/graphics/Window.cpp#L384-L386))
     to confirm the disc is opaque in the picker and does not clobber neighbouring loops.

## Risks & Open Questions
- **Picker overdraw (no depth test in picker pass).** A too-large/centre-filled disc could
  overwrite other loops' picker pixels. *Mitigation:* annulus localized to the note band;
  validate via picker-buffer dump. (Primary risk.)
- **Disc geometry shape.** Reusing the 16-segment note mesh yields a 16-gon ring (slightly
  faceted) and degenerate end-cap faces at the 0/2π seam. Acceptable visually; if a smoother
  disc is required later, give the disc its own higher-segment geometry + second draw call.
- **"Disc" vs "annulus" interpretation.** Issue says "disc … with some thickness." We
  interpret "thickness" as vertical thickness and prefer an annulus for picker-safety. Flag
  for owner confirmation; trivially switchable via `DiscRadiusFactor`/`DiscRadialThicknessFactor`.
- **Audio thread.** No new allocation/locking on RT paths (disc built in non-RT
  `BuildInstanceData`; published via existing atomic swap). Re-confirm after edits.
- **Highlight pass.** Disc participates in the highlight pass via the existing
  `RenderMode == 2` path; confirm the selected-loop glow looks correct with the larger disc.

## TODOs
- [ ] Step 1 — `MidiModelParams` disc fields + defaults.
- [ ] Step 2 — append disc instance in `BuildInstanceData` (always-on when length > 0).
- [ ] Step 3 — `midi_note.vert` forward `IsDisc`.
- [ ] Step 4 — `midi_note.frag` semi-transparent disc in scene pass.
- [ ] Step 5 — verify hover/select feedback.
- [ ] Step 6 — build + tests (incl. empty-loop instance-count test).
- [ ] Step 7 — manual visual + picker-buffer verification.
