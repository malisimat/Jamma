# Feature: enhanced station 3D visuals

## Summary
Make stations look great in 3D by giving each station a dedicated procedural 3D presence: a lightweight sculptural "halo deck" that frames the station as a physical performance object instead of an invisible parent container.

## Goals & Acceptance Criteria
when some kind of 3D geometry is visible for the stations and looks fantastic

- Stations render a visible 3D model only while selection depth is `DEPTH_STATION`.
- The model has a dedicated scene shader and supports picker rendering.
- The picker pass selects the owning station from the new geometry.
- Shader resources are registered in `Jamma/resources/ResourceList.txt`.
- A small focused test set covers geometry and pass/visibility behavior where practical.

## Scope

### In scope
station model, dedicated shader, picker pass, visibility dependent on depth mode (Station depth)

### Out of scope
no 2D elements or GUI, nothing outside of visuals or selection

## Proposed Approach
Selected concept: **Station Halo Deck**.

The station gets a low, beveled circular/octagonal command deck drawn behind/beneath the station's existing children. It should feel like stage hardware: a dark graphite platform, raised bus ribs, a thin luminous rim, and subtle hover/selection energy. The expensive-looking part should come from silhouette, normals, and cheap procedural shader math, not heavy meshes, textures, post-processing, or per-frame CPU work.

### Brainstormed alternatives

- **Station Halo Deck**: a beveled round/octagonal platform with ribs and luminous rim. Best balance of strong visual identity, low complexity, and easy integration with the existing model/shader path.
- **Signal Cage**: a 40-triangle holographic bounding cage around each station. Extremely light and useful as an accent idea, but less substantial as the station's primary appearance.
- **Resonant Prism**: a glassy audio-reactive monolith. Visually cool, but direct audio-reactive uniforms would risk pulling the task toward audio-state plumbing and extra coupling.

### Implementation details

- Add a `graphics::StationModel` or similarly named class that follows the existing `gui::GuiModel` pattern.
- Generate static procedural geometry once: beveled deck, raised radial/cross ribs, and outer rim. Keep it roughly in the low hundreds to about 1,200 triangles per station.
- Draw the station model before station children in `Station::Draw3d`, so current racks/loops remain readable on top.
- Toggle station model visibility in `Station::SetSelectDepth` together with the existing station rack visibility: visible only for `base::SelectDepth::DEPTH_STATION`.
- For `PASS_SCENE`, use a new dedicated station shader.
- For `PASS_PICKER`, use the existing picker shader and pack the owning station ID the same way loop picker rendering does.
- For hover/selection/highlight, keep behavior cheap: uniform-driven rim brightness or fallback to existing highlight shader. Do not add bloom, extra render targets, or heavy post passes.
- Avoid audio-thread/state coupling. If a pulse is desired, use render-time math or existing selection/hover state, not live audio analysis.

### Shader direction

- Add `Jamma/resources/shaders/station.vert` and `Jamma/resources/shaders/station.frag`.
- Use existing attributes: position, UV, normal.
- Required uniforms should stay small, for example: `MVP`, `ObjectId`, `Highlight`, `StationHover`, `StationFocus` or equivalent.
- Fragment styling: graphite base, restrained cyan/amber rim, normal-based diffuse, UV/radius-based glow, optional subtle rib accent.
- Update `Jamma/resources/ResourceList.txt` with any new shader entries.

### Testing direction

- Add small native tests for station model geometry generation or pass state if the class exposes testable pure helpers.
- Prefer tests that do not require an OpenGL context.
- Keep tests focused; do not try to screenshot-test OpenGL in the native unit suite.

## Risks & Open Questions
Overall performance, code bloat

- Keep the mesh static and resource-time only; no per-frame geometry rebuilds.
- Keep shader math simple and deterministic.
- Avoid touching 2D GUI behavior beyond the existing station-depth visibility relationship.
- Confirm project/filter files include any new C++ source/header.

## TODOs
- [ ] Add station model class with static procedural halo-deck geometry.
- [ ] Add dedicated station shaders and manifest entries.
- [ ] Wire station model construction, resource init/release, drawing, picker ID, and depth visibility.
- [ ] Add a small number of non-OpenGL tests for geometry/pass behavior where practical.
- [ ] Build affected projects and run the native tests.
- [ ] Rubber duck review the diff for real-time/audio-thread safety and rendering state hygiene.
- [ ] Generate a summary HTML for visual/code review.
