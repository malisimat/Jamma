# Station State Rings Plan

## Objective

Replace the small green/red/orange/purple `Trigger` status square with two large, highly legible, three-dimensional state rings built into the top and bottom caps of `graphics::StationModel`.

The rings are the only visible station-state indicator. They communicate state through both a unique colour and a unique occluding silhouette, so the result remains readable without relying on colour alone.

## Current Behaviour And Root Cause

The observed square is not part of `StationModel`:

- `engine::Scene::FromFile` creates each `Trigger` at `Size = { 24, 24 }`, `Position = { 6, 6 }`, with `Texture = "green"` and `TextureRecording = "red"`.
- `engine::Trigger::Draw` draws that texture in 2D.
- On the second activate press, `engine::Trigger::EndRecording` sets `_state = TRIGSTATE_DEFAULT` before it dispatches `TRIGGER_REC_END` to the station. Default is drawn by `GuiElement::Draw`, which uses the green texture.
- Therefore this square returns to green immediately. It is independent of the station's 3D visual state and independent of the loop take's recording-tail completion.

`StationModel` is a separate 3D mesh. `engine::Station::Draw3d` supplies its `StationVisualState`; `graphics::StationModel::Draw3d` maps that state to a colour; `resources/shaders/station.frag` applies the colour to the cap surface. The existing deck is small in local coordinates and is mainly a cap on a very tall station body, so the current cap-only colour is not a strong status cue.

## Desired Look

Build two matching lathed collars which arc around the station globe/caps:

- A **top crown ring** directly below and around the upper cap. It is the primary status signal.
- A **bottom crown ring** directly above and around the lower cap. It mirrors the top ring so the state remains visible when the station is viewed from below or tilted.
- Both collars extend farther outward than the current bevel and are substantially thicker than the existing thin luminous rim.
- The bright material is interrupted by a dark, graphite-grey occluding layer. The cut-outs are physical raised/overlapping geometry, not an alpha mask. This keeps their silhouette readable under every lighting condition.
- Each station state selects its own cut-out profile. A glance should identify the state from the ring pattern even in monochrome.
- The body and side-wall VU treatment stay visually subordinate. The cap rings own state communication.

The aesthetic target is a piece of illuminated industrial instrumentation: broad lathed metal collars, luminous state material behind deliberate dark shutter forms, crisp 32-sided faceting, and no decorative noise that prevents state recognition.

## Scope

### Remove

1. Stop creating the station trigger's visible 2D image in `engine::Scene::FromFile`.
2. Remove the trigger from the HUD trigger collection used for 2D drawing, while preserving its receiver, bindings, input routing, and state-machine behaviour.
3. Remove the old cap-wide `StationStateColor` fill. State colour must be restricted to the new ring material.

Do not remove trigger input handling, trigger history, trigger-to-station action dispatch, or any audio/MIDI behaviour. This is a visual ownership change only.

### Add

1. Ring geometry and profile tables in `graphics::StationModel`.
2. Dedicated ring shader treatment in `station_ring.vert` / `station_ring.frag`; do not add ring branches to the body shader pair.
3. An explicit station visual state for the end-recording tail. It remains active until the relevant `LoopTake` exits `STATE_PLAYINGRECORDING` / `STATE_OVERDUBBINGRECORDING`.
4. Focused geometry and state-transition tests.

## Mesh Contract

### Constants

Keep all dimensions as named `constexpr` values near the existing `DeckRadius`, `BevelWidth`, and `SideHeight` constants in `StationModel.cpp`.

The station body and the state rings are separate rendering materials. The existing `station.vert` / `station.frag` pair stays responsible for the complex full-height cylinder: radial expansion, side-wall VU gradient, deck, bevel, hover, and selection. The rings use their own shader pair so their normals, emission, bevel lighting, and patterned occluders cannot accidentally inherit the body's height profile or side-wall gradient.

Use these starting proportions in existing model-local units:

| Constant | Initial value | Meaning |
| --- | ---: | --- |
| `StateRingSides` | `64` | Enough angular resolution for rounded panel ends and detailed patterns. Keep the body at its existing resolution. |
| `StateRingInnerRadius` | `9.3f` | Slightly under the existing deck radius so the collar joins cleanly. |
| `StateRingOuterRadius` | `15.8f` | Broad, clearly visible outer edge. |
| `StateRingHeight` | `16.0f` | Starting total height for the top collar; the bottom collar uses its own profile. |
| `StateRingGlowInset` | `0.35f` | Inset of bright material behind the occluder. |
| `StateRingOccluderLift` | `0.55f` | Radial and normal offset preventing z-fighting. |
| `StateRingTopY` | `-2.0f` | Top collar starts slightly below the cap plane. |
| `StateRingBottomY` | `-(2 * BevelHeight + SideHeight) + 2.0f` | Bottom collar starts slightly above the lower cap plane. |
| `StateRingPanelSegments` | `4` | Angular subdivisions used to round each occluder panel's leading and trailing edge. |

The exact values are deliberately initial art-direction values. Preserve their names and adjust only those constants during visual iteration.

### Profile Data

Define a tiny 2D profile type in `StationModel.h` or at file scope in `StationModel.cpp`:

```cpp
struct RingProfilePoint
{
    float Radius;
    float Y;
};
```

The bright rings are produced by revolving `(radius, y)` profiles around the Y axis. Do not force the top and bottom to share one silhouette. They share the state pattern vocabulary and angular phase, but each has its own profile so the station can read as designed from either end.

Use this top profile first:

```cpp
// From cap inward/outward silhouette, in local station coordinates.
constexpr RingProfilePoint StateRingProfile[] = {
    {  9.30f,  0.00f }, // hidden overlap under cap
    { 10.80f, -1.25f }, // shallow inner chamfer
    { 15.80f, -3.50f }, // wide, proud outer shoulder
    { 15.80f, -9.50f }, // straight illuminated band
    { 14.90f,-13.50f }, // lower chamfer
    { 11.20f,-16.00f }, // tucked return into side wall
};
```

Use this different bottom profile first. It is wider at the lip, flatter across the luminous band, and tucks into the lower cylinder with a longer return:

```cpp
constexpr RingProfilePoint StateRingBottomProfile[] = {
   {  9.30f,  0.00f }, // hidden overlap under lower cap
   { 11.60f,  1.10f }, // broad lower-cap flare
   { 16.60f,  3.20f }, // wider lower shoulder than the top
   { 16.60f,  8.20f }, // flatter luminous band
   { 15.30f, 12.40f }, // softened return chamfer
   { 12.00f, 17.50f }, // long tuck into the cylinder
   { 10.20f, 19.00f }, // small underside lip
};
```

The implementation may normalize these local Y values around `StateRingTopY` and `StateRingBottomY`, but must preserve the asymmetry. The top should feel like a crown or brow; the bottom should feel like a heavier keel or stabilizer.

Implement one general helper:

```cpp
static std::tuple<std::vector<float>, std::vector<float>>
BuildLathedProfileGeometry(unsigned int numSides,
    std::span<const RingProfilePoint> profile,
    float yOffset,
    bool invertY,
    float partKind);
```

For every adjacent pair of profile points and every angular side:

1. Generate the four revolution points at `angle0` and `angle1`.
2. Emit an outward-facing quad with `PushQuad`.
3. Set `uv.x` to angular fraction `$i / N$` so the fragment shader can place additional fine markings later.
4. Set `uv.y` to the supplied ring part kind.

Do not put profile selection, state selection, or geometry rebuilding on the render/audio path. Build the distinct bright top and bottom rings once in the `StationModel` constructor.

### Dark Occluding Layer

Generate separate top and bottom occluder meshes for every state. Each uses the corresponding top or bottom base profile, but adds state-specific contour panels over the bright band. The top and bottom pattern must match as a readable motif while following their different curvature and vertical proportions.

Each occluder panel is a quad strip whose inner/outer radii and top/bottom Y coordinates come from a per-state table. The panels must be physically offset from the bright collar by `StateRingOccluderLift`; never depend on depth coincidence.

The table data format must be simple enough to edit without understanding OpenGL:

```cpp
struct RingOccluderSegment
{
    unsigned int FirstSide;  // inclusive, within [0, StateRingSides)
    unsigned int SideCount;
    float InnerRadius;
    float OuterRadius;
    float YMin;
    float YMax;
};
```

Use `FirstSide` / `SideCount`, not floating angle literals. With 64 sides, a segment of 4 sides is a 22.5-degree arc. A low-context implementer can count sides without converting radians.

Do not build an occluder as one flat quad for a broad angular span. For every panel, subdivide the leading edge, body, and trailing edge into at least `StateRingPanelSegments` angular strips. Interpolate the panel's inner/outer radii and Y bounds across those strips so its ends are chamfered or tapered. This gives the dark layer a rounded, machined appearance instead of a cardboard cut-out.

Each detailed panel should support these independent features:

1. A 1-2 side chamfer at the leading edge.
2. A central run of 2-8 sides with stable width.
3. A mirrored trailing chamfer.
4. Optional upper-only or lower-only coverage for split patterns.
5. Optional inner-radius retreat, allowing a bright inner crescent to remain visible.

Use small gaps of at least one angular side between unrelated dark panels. Add a second, narrower inset dark strip to selected patterns rather than making every panel wider; the bright/dark/bright rhythm is what makes the ring legible at a distance.

Generate each state pattern as separate static geometry at construction time. Do not dynamically mutate a VBO per frame. Draw exactly one top occluder mesh and one bottom occluder mesh selected by `StationVisualState`.

## State Pattern Table

Use the bright collar colour and silhouette below. Top and bottom rings use the same state vocabulary and angular rhythm, but the geometry is adapted to each ring's different profile. Do not simply scale or vertically mirror the top mesh.

| State | Colour | Occluding silhouette | Required segment recipe |
| --- | --- | --- | --- |
| `DEFAULT` | neutral cyan `#57C8E3` | Calibrated toothed bezel | 8 repeated motifs at 45-degree intervals. Each motif is a 1-side lead chamfer, 2-side full-height notch, 1-side trailing chamfer, plus a narrow inner-radius notch on its centre side. |
| `RECORDING` | red `#F03338` | Urgent interlock | 16 narrow slits at 22.5-degree intervals, each with chamfered ends. Every fourth slit gets a parallel inset slit one side away, making four double-cut cardinal locks. |
| `ENDRECORDING` | amber `#F5D137` | Closing iris | 4 broad tapered wedges at the cardinal axes. Each wedge uses 2-side chamfers, a 4-side body, and a 2-side inner retreat; add a short upper-only notch on each wedge shoulder to suggest blades closing. |
| `PLAYING` | electric blue `#3DADFA` | Directional chevrons | 6 arrow motifs. Each has a 1-side lead, 3-side rising diagonal body, 1-side point, and a separate 1-side lower counter-notch. The angular phase advances clockwise from top to bottom. |
| `OVERDUBBING` | orange `#F28A28` | Interleaved weave | 8 alternating upper/lower ribbons. Each ribbon has chamfered ends, a 2-side bright gap at its centre, and a narrow inset companion strip on the opposite half. The alternating halves should read as a woven braid. |
| `PUNCHIN` | violet `#B455ED` | Four locked gates | 4 full-height gates, each 3-side wide with rounded 1-side shoulders, plus two 1-side upper ticks and two 1-side lower ticks around each gate. The repeated gate/tick/tick/open rhythm must survive grayscale viewing. |

All occluder geometry uses a fixed charcoal material, approximately `vec3(0.10f, 0.12f, 0.14f)`, with rough diffuse shading and no emissive contribution. It must be visibly dark even when selected or hovered. Do not tint it with state colour.

## Shader Contract

### Separate Ring Shaders

Add `Jamma/resources/shaders/station_ring.vert` and `Jamma/resources/shaders/station_ring.frag`. Register them as a separate shader pair in `StationModel` rather than adding ring conditionals to `station.vert` / `station.frag`.

`station.vert` / `station.frag` remain the body shaders. They continue to own the cylinder's height-dependent radial profile, side-wall gradient, VU bands, deck, bevel, selection, and hover treatment. The ring shaders receive already-built ring geometry and must not apply the body's full-height radial expansion.

The ring vertex shader must:

1. Transform ring vertices with the normal MVP path.
2. Preserve the authored lathed radius and Y profile exactly.
3. Pass a correct per-face or analytic lathe normal for the ring's curved faces.
4. Pass angular fraction, profile fraction, ring side (top/bottom), and a small pattern coordinate to the fragment shader.
5. Apply a tiny controlled radial lift only to dark occluders when needed for depth separation; do not inflate the entire ring.

The ring fragment shader must:

1. Render bright collar material and charcoal occluders as separate material branches.
2. Use the state colour supplied by `StationStateColor` only for the bright collar.
3. Apply a dedicated bright-metal lighting model: broad diffuse, tight outer-edge specular, and restrained emission.
4. Apply a darker, rougher model to occluders so the cut shapes remain clear under high VU and selection lighting.
5. Use the passed profile/pattern coordinates for small edge highlights and inset strips; do not reconstruct the body's height gradient.

Use separate model instances or draw ranges for bright top, bright bottom, dark top, and dark bottom. All four can share one ring shader program per pass.

### New Part Kinds

The new ring shaders may use a dedicated compact attribute/UV convention rather than extending the body shader's UV part-kind convention. If the existing `GuiModel` attribute layout makes UVs the cheapest route, use:

```cpp
constexpr float UV_STATE_RING_BRIGHT = 4.0f;
constexpr float UV_STATE_RING_DARK   = 5.0f;
```

The bright ring shader branch:

1. Uses `StationStateColor` as its base.
2. Applies stronger directional lighting than the deck.
3. Adds a controlled emissive term of `0.30f + 0.35f * StationLevelOut`.
4. Adds a thin white-hot highlight on the outermost 12 percent of the bright band, driven by the profile's radial coordinate encoded in `uv.x` or a dedicated profile coordinate if needed.
5. Never makes the ring transparent.

The dark occluder branch:

1. Uses the charcoal material above.
2. Applies normal-based diffuse only.
3. Receives hover/selection response as a very small cool-grey lift, at most `0.08f`; it must never wash out the silhouette.

Leave the current side-wall level-gradient VU behaviour intact. Change the current `partKind < 0.5` deck branch back to neutral deck material, otherwise the entire cap competes with the new state ring. Remove the old body-shader rim glow once the dedicated rings provide that highlight; avoid two competing status rims.

### Normal Handling

The current body vertex shader forwards the original normal after applying radial expansion. The new ring vertex shader must either:

- provide correct lathed face normals through the existing geometry/normal upload path, or
- compute a stable radial normal in `station_ring.vert` for `UV_STATE_RING_BRIGHT` and `UV_STATE_RING_DARK`.

Choose one and document it in a short comment. Do not leave the rings lit from a cap normal; the collar needs clear side-facing light and dark planes.

## State Selection And Timing

The ring selection must use `Station::GetVisualState()` only. `Station` remains the source of truth and publishes an atomic visual state to the render thread.

Required lifecycle:

1. `TRIGGER_REC_START` -> `RECORDING` rings.
2. `TRIGGER_REC_END` -> `ENDRECORDING` rings immediately, even though the 2D trigger itself is already back in its default input state.
3. Keep `ENDRECORDING` while any take is `STATE_PLAYINGRECORDING` or `STATE_OVERDUBBINGRECORDING`.
4. Once no take remains in either tail state -> `PLAYING` rings.
5. Preserve existing overdub and punch-in transitions.
6. Ditch/reset -> `DEFAULT` rings.

Do not couple ring timing to `Trigger::GetState()`: the trigger represents input gesture mode, whereas the station represents the audible transport lifecycle.

## File-Level Implementation Steps

1. `JammaLib/src/graphics/StationModel.h`
   - Add public testable builders for the lathed bright-ring geometry and state occluder geometry.
   - Add private GPU/model storage for the bright-ring mesh and one occluder mesh per visual state, following existing `GuiModel` resource conventions.
   - Add the `RingProfilePoint` and `RingOccluderSegment` data types where tests can construct them without OpenGL.

2. `JammaLib/src/graphics/StationModel.cpp`
   - Add the shared ring profile and six state occluder segment tables.
   - Implement the lathe helper and per-state occluder builder with deterministic winding.
   - Build the distinct bright top/bottom rings and all twelve occluder meshes (six states x two ends) once during model setup.
   - Draw the base station body with the existing body shader, then bind the ring shader and draw the bright top/bottom rings followed by only the matching top/bottom occluders.
   - Maintain picker behaviour: all ring geometry must report the owning station object ID.
   - Keep the existing picker shader for `PASS_PICKER`; use the dedicated ring shader only for scene/highlight passes. The station model's shader/resource bookkeeping must make this pass split explicit.

3. `Jamma/resources/shaders/station_ring.vert` and `Jamma/resources/shaders/station_ring.frag`
   - Add the dedicated ring vertex/fragment shader pair described above.
   - Keep the body shader pair free of ring-specific profile, pattern, or material branches.
   - Do not make ring animation depend on wall-clock time in this first implementation.
   - Register both files in `Jamma/Jamma.vcxproj` and its `.filters` file so they are copied with the other runtime shader resources.

4. `Jamma/resources/shaders/station.frag`
   - Return the body cap to its neutral material and remove the old state-rim branch if it is no longer needed.
   - Keep all state colours in the existing C++ visual-state colour table, passed by `StationStateColor`; avoid duplicated state-colour tables in GLSL.

5. `JammaLib/src/engine/Station.h` and `JammaLib/src/engine/Station.cpp`
   - Keep/add the `ENDRECORDING` state.
   - Publish `ENDRECORDING` at record end and move to `PLAYING` only after takes leave their engine recording-tail states.
   - Do not add timeouts or sleep-based visual transitions.

6. `JammaLib/src/engine/Scene.cpp`, `JammaLib/src/engine/Trigger.cpp`, and related HUD wiring
   - Remove only the visible 2D trigger square from the HUD composition.
   - Preserve trigger construction, receiver registration, control bindings, and event dispatch.
   - Delete unused `green`, `red`, `blue`, `orange`, and `purple` trigger texture dependencies only after confirming they are not used by another UI feature.

7. `test/JammaLib_Tests/src/graphics/StationModel_Tests.cpp`
   - Add pure geometry tests for both profile closures, UV/vertex count agreement, expected ring triangle counts, non-degenerate radii, and distinct top/bottom silhouettes.
   - Add one test per state/end asserting its detailed occluder geometry is non-empty and not byte-identical to any other state's geometry.

8. `test/JammaLib_Tests/src/engine/StationVisualState_Tests.cpp`
   - Assert record end enters `ENDRECORDING`.
   - Drive the relevant loop-take completion boundary, tick the station, and assert it becomes `PLAYING` only then.

## Acceptance Checks

1. The former upper-left/centre `24x24` trigger image is absent in station mode.
2. A station in each of the six states has a visibly distinct ring pattern in grayscale and in colour.
3. `ENDRECORDING` is visibly amber with the four-wedge closing-iris pattern for the whole latency tail; it does not turn into the playing pattern on trigger release.
4. Top and bottom rings remain recognizable at the normal station camera angle, with a tilted camera, and when the station is selected/hovered.
5. Station selection and picking still resolve to the station, including clicks on a ring.
6. No render-thread dynamic allocation, mesh rebuilding, locks, or audio-thread rendering work is introduced.
7. The focused native tests build and pass, followed by a manual Debug application check at standard and high station VU levels.

## Explicit Non-Goals

- No animated travelling lights, scrolling textures, or time-driven pulses in the first pass.
- No change to recording, loop finalization, latency compensation, trigger binding, MIDI, or audio callback logic beyond publishing the already-real end-recording state.
- No new 2D replacement icon. The cap rings are the complete state affordance.