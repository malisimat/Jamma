# Quantisation + Ctrl Overlay Handover (2026-06-13)

## Status

- Phase implementation is complete through overlay latch behavior, grain-aware quantisation provenance, dual-model render split, shader/resource/project wiring, and targeted tests.
- Solution build (`Debug|x64`) succeeds.
- Focused quantisation/scene-routing test coverage has been updated to remove the legacy `GTEST_SKIP` placeholders and assert against latched ctrl-overlay routing directly.
- Ctrl overlay handle contract is now explicit by select depth:
  - `DEPTH_STATION`: 2 handles (`0=global phase`, `1=fraction`)
  - `DEPTH_LOOPTAKE` / `DEPTH_LOOP`: 3 handles (`0=global phase`, `1=local take phase`, `2=fraction`)
- Station-depth fraction gestures now apply to all LoopTakes in the hovered station.

## Remaining Concerns

1. Full regression confidence still depends on rerunning the focused native test filter after any follow-on interaction changes.
2. Overlay button ordering is now a behavior contract used in tests; any future reordering should update tests and this note together.

## Recommended Follow-Up

1. Keep button-hit coordination anchored through `Scene::CtrlOverlayButtonCenterForTest(...)` so tests remain deterministic under camera/hover changes.
2. Rerun on the next integration pass:
   - `Build Tests (Debug x64)`
   - `JammaLib_Tests.exe --gtest_filter="LoopTakeMidiQuantisation.*:SceneInteractionRouting.*"`
