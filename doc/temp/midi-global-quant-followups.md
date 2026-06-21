# MIDI Global Quant Follow-ups

## Completed in this pass
- Jam schema now persists:
  - `globalmidiquantstate` (`off|mixed|all`)
  - Per-take `midiquantenabled`
  - Per-take `midiquantfraction`
  - Existing `takephaseoffsetsamps` unchanged
- Runtime tri-state implemented across Scene -> Station -> LoopTake.
- Effective enabled logic is now global-aware in `LoopTake::ResolvedMidiQuantisation()` without mutating local stored settings.
- Local quant edits now emit a focused gui action that bubbles to Scene and forces global state to `Mixed` immediately.
- Session export now writes global state and per-take local quant fields.
- Added targeted tests for IO round-trip/defaults/clamping and core engine behavior.

## Remaining verification / polish tasks
- UI polish: global tri-state radio currently uses color-tinted panel toggles; it does not yet render explicit text labels (`Off`, `X`, `All`).
- Add an integration-style Scene test that asserts end-to-end `All/Off -> local edit -> Mixed` transition through actual action routing.
- Manual QA pass in app to confirm visual discoverability and placement of the new radio near mode controls on different window sizes.
- Optional: include `globalmidiquantstate` in default jam JSON string if desired for explicitness (runtime defaults already backward-compatible).

## Notes
- Runtime behavior follows non-destructive local-memory semantics: global mode changes only affect resolved effective enabled state.
- Backward compatibility is preserved: missing keys default to Mixed/global, local enabled false, local fraction whole.
