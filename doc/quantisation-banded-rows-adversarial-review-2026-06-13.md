# Quantisation Banded Rows Adversarial Review

Date: 2026-06-13

## Scope

Reviewed the current implementation for these three requirements:

1. Outward-facing banded rows must match the current per-LoopTake quantisation fraction.
2. Fraction drag must be Ctrl-only and must start from a dedicated overlay handle/button, not by dragging directly on a LoopTake.
3. When Ctrl is pressed with no active selection and no hovered object, the overlay must show only one global handle/button.

## Verdict

No correctness bugs were found in the implementation for the three requested behaviors.

## Evidence

### 1. Banded row count follows the current LoopTake fraction

- `LoopTake::QuantisationVisual()` now includes `MidiQuantisation().Fraction` in the visual payload.
- `QuantisationModel::SetLoopTakeVisuals()` now derives `gateCount` from `midi::MidiQuantisation::Divisor(visual.Fraction) / 2u`.
- This produces the expected visible counts:
  - `1/32 -> 16`
  - `1/16 -> 8`
  - `1/8 -> 4`
  - `1/4 -> 2`
  - `1/2 -> 1`
  - `1 -> 1` after clamp
- Live update path is present:
  - fraction drag calls `LoopTake::_ApplyMidiQuantisationGesture()`
  - that immediately calls `SetMidiQuantisation(updated)`
  - station overlay visuals are rebuilt on every `Station::Draw3d()` via `RefreshQuantisationOverlayFromClock()`
  - `RefreshQuantisationOverlayFromClock()` regenerates visuals from current `LoopTake::QuantisationVisualsFor(GetLoopTakes())`

Conclusion: the row count is now driven by the live per-take fraction rather than by scene seed timing.

### 2. Fraction drag is Ctrl-only and must start from the overlay

- `HasMidiQuantisationGestureModifiers()` still accepts Ctrl-only.
- Direct Ctrl-drag start from `LoopTake::OnAction(TouchAction)` was removed.
- Fraction drag now starts through `Scene::OnAction(TouchAction)` only when the Ctrl overlay hit-test returns button `1`, which then routes into `Scene::_BeginFractionDrag()`.
- `Scene::_BeginFractionDrag()` calls `take->BeginMidiQuantisationGesture(action)`, preserving the Ctrl-only modifier requirement.

Conclusion: direct dragging on a LoopTake no longer starts the fraction gesture; the dedicated overlay button is now required.

### 3. Global overlay falls back to one button when nothing is selected or hovered

- `Scene::_CtrlHandleButtonCount()` returns `1` only when both:
  - `_HasQuantisationSelection()` is false
  - `_HasQuantisationHover()` is false
- Otherwise it returns `2`.
- `Scene::_RefreshCtrlHandleOverlay()` is called on:
  - Ctrl key press/release
  - hover updates in `SetHover3d()`
  - selection updates in `_UpdateSelection()`
- `CtrlHandleOverlay` now respects `_visibleButtonCount` in layout, hit-testing, and quad generation.

Conclusion: the overlay context now matches the requirement and updates when Ctrl, hover, or selection state changes.

## Validation Performed

- Built `test/JammaLib_Tests` successfully.
- Ran `JammaLib_Tests.exe --gtest_filter=QuantisationModel.GateGeometryBuildsHalfFrameInstanceMesh` successfully.

## Residual Risk

No failing behavior was found in code review, but the new overlay-context behavior does not yet have a focused automated test. The implementation is compile-validated and the existing quantisation model regression still passes, but the single-button-vs-two-button Ctrl overlay behavior is currently relying on code inspection and manual runtime verification.