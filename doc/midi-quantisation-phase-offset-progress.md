# Handoff: Ctrl Phase-Edit Overlay Handles

## Recent Work (session 2026-06-12)

- **Ctrl-only trigger**: removed the Shift requirement from both `LoopTake::HasMidiQuantisationGestureModifiers` and `Scene::_IsMidiPhaseDragModifier`. Holding Ctrl alone now activates the quantisation fraction-drag gesture and the phase-offset drag in Scene.
- **QuantisationModel visual polish**: added a fourth mesh part (`HandlePart = 3.0`) as four quads that protrude beyond the outermost gate ring; added an alternating `BandValue` per-instance stream (attribute index 5) so odd/even subdivision bands shade differently; added a `Hue` uniform and HSV colour palette in `quantisation.frag`; added diffuse + specular lighting using the computed surface normal; added `ResourceList.txt` entry for `Hue`.
- **Column origin fixed**: the ColumnInstance transform angle was previously hardcoded to `0.0` — it is now set to `phaseOffset` so the origin marker follows the take's resolved phase.
- **Tests updated**: all `CtrlShift` test helpers and test names renamed to `Ctrl`; geometry regression test updated for the new 28-quad mesh size.
- **Validation**: build clean (0 warnings), 560 tests run, 559 passed, 1 skipped (MidiDevice availability).

---

## Bug: Ctrl Handles Are Not Near the Mouse Cursor

### Diagnosis

The handles added in the last session are **3D geometry** appended to the existing `QuantisationModel` gate mesh. They appear as small protrusions at the outer radius of the radial gate ring — i.e. somewhere in the 3D scene centered on the hovered station, **not** near the mouse pointer.

What was described in the requirements — "a few rounded-rect buttons coloured differently (global drag, LoopTake fraction, future ops)" — is a **2D screen-space popup panel** that follows the cursor. That feature does not exist yet.

---

## Proposed Solution

Add a `CtrlHandleOverlay` class — a lightweight 2D overlay drawn on the `Scene::Draw` pass alongside `_label`, `_selector`, and `_modeRadio`. It should:

1. **Track hover position**: Scene already stores `_hoverPath3d` and calls `SetHover3d` on every cursor move. Each `Station` already computes `_modelScreenPos` via `GlDrawContext::ProjectScreen` during `Draw3d`. Use the hovered station/take screen position as an anchor; fall back to raw mouse pixel position if nothing is hovered.

2. **Show/hide with Ctrl**: Scene already calls `_SetQuantisationOverlayHeld(true/false)` on key-char 17 (VK_CONTROL). Feed that same held state as `SetVisible` on the overlay. Use `Quantisation::OverlayAlpha` for smooth fade, exactly like the existing `QuantisationModel`.

3. **Render 2–3 rounded-rect buttons**: Use `NinePatchImage` (already in the codebase) for each button. Suggested set:
   - **Phase** — horizontal drag → calls `_BeginMidiPhaseDrag` / `_UpdateMidiPhaseDrag`
   - **Fraction** — vertical drag → calls `LoopTake::BeginMidiQuantisationGesture`
   - *(reserved slot for future ops)*

4. **Dispatch on touch-down inside a button rect**: The overlay lives in `Scene::Draw` 2D space. On `Scene::OnAction(TouchAction)`, check whether the incoming position hits a visible button rect before the existing drag-start logic runs.

5. **Screen-edge clamping**: clamp the panel position so it stays within scene bounds when the cursor is near an edge.

---

## Files to Change

| File | Change |
|---|---|
| `JammaLib/src/graphics/CtrlHandleOverlay.h/.cpp` | New class. Owns 2–3 `NinePatchImage` buttons, tracks anchor pos and alpha. |
| `JammaLib/src/engine/Scene.h` | Add `_ctrlHandleOverlay` member, add `_UpdateCtrlHandleOverlayPosition` helper. |
| `JammaLib/src/engine/Scene.cpp` | Construct overlay in ctor; call `_ctrlHandleOverlay.Draw(ctx)` in `Scene::Draw`; update position in `SetHover3d`; wire Ctrl key and touch dispatch. |
| `Jamma/resources/ResourceList.txt` | Any new textures used by the button NinePatches. |
