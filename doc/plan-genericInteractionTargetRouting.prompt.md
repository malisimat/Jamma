## Plan: Generic Interaction Target Routing

Refactor Scene so it owns only generic interaction state: raw 3D hover path, selected/current objects at the active SelectDepth, and the active dragging object set. Command-specific behavior such as MIDI quantisation maps that generic target set to command recipients at dispatch time. This removes `_midiQuantisationGestureTargets` and avoids baking quantisation semantics into Scene's interaction state, while preserving the currently requested Ctrl+Shift-drag behavior.

**Steps**
1. Snapshot current work before changes.
   - Review current modified files: `JammaLib/src/engine/Scene.cpp`, `JammaLib/src/engine/Scene.h`, `JammaLib/src/engine/LoopTake.cpp`, `JammaLib/src/engine/LoopTake.h`, and `test/JammaLib_Tests/src/engine/MidiLoop_Tests.cpp`.
   - Keep any passing tests from the current work as behavioral guardrails, but expect to rename/repoint them to the generic routing model.

2. Replace command-specific Scene state with generic dragging state.
   - Remove `Scene::_midiQuantisationGestureTargets`.
   - Add a generic field such as `std::vector<std::shared_ptr<base::GuiElement>> _dragTargets;` or `_activeInteractionTargets`.
   - The field represents the target objects captured at touch-down at the current `SelectDepth`; it does not know what command is running.
   - Keep `_hoverPath3d` as raw picker path state.

3. Keep and refine generic target expansion.
   - Keep `_CurrentInteractionTargets()` as the core helper.
   - It should return hovered object at current `SelectDepth` plus selected objects at that same depth, deduped and ordered with hovered first.
   - Depth rules:
     - `DEPTH_STATION`: hovered station plus selected stations.
     - `DEPTH_LOOPTAKE`: hovered take plus selected takes across all stations.
     - `DEPTH_LOOP`: hovered loop plus selected loops across all takes/stations.
   - Do not include command-specific mapping in this helper.

4. Add command-recipient mapping helpers as thin adapters.
   - Add a helper for this command only, e.g. `_LoopTakesForInteractionTargets(const std::vector<std::shared_ptr<base::GuiElement>>& targets, base::SelectDepth depth)`.
   - Mapping rules:
     - Station targets -> all LoopTakes in those stations.
     - LoopTake targets -> those LoopTakes.
     - Loop targets -> owning LoopTake for each loop.
   - This helper is command-adjacent but does not store state. It can later be mirrored by helpers for fader, mute, rack, or other operations.

5. Route Ctrl+Shift-drag using generic drag capture.
   - On Ctrl+Shift `TOUCH_DOWN`, after 2D controls decline the touch:
     - Set `_dragTargets = _CurrentInteractionTargets()`.
     - Map `_dragTargets` to LoopTakes via the adapter.
     - Dispatch quantisation gesture start to those LoopTakes.
     - If no command recipient eats the action, clear `_dragTargets` and continue normal selector/scene handling.
   - On `TouchMoveAction`, if `_dragTargets` is non-empty:
     - Map `_dragTargets` using the current depth captured at down time if needed.
     - Dispatch move to the mapped command recipients.
   - On `TOUCH_UP`, if `_dragTargets` is non-empty:
     - Dispatch release/finalize to mapped recipients.
     - Clear `_dragTargets`.
     - Continue selector update as today.

6. Decide whether drag depth must be captured.
   - Recommended: store `base::SelectDepth _dragSelectDepth` alongside `_dragTargets` when touch-down starts.
   - This prevents mode changes during a drag from remapping the target set unexpectedly.
   - Scene still stores generic interaction state, not command-specific state.

7. Keep LoopTake responsible for quantisation semantics.
   - Leave `LoopTake::BeginMidiQuantisationGesture`, `LoopTake::OnAction(TouchMoveAction)`, and `LoopTake::OnAction(TouchAction up)` responsible for fraction deltas, toggles, and `_midiQuantisationGestureActive` cleanup.
   - Scene should only route the gesture lifecycle to command recipients.
   - If the public `BeginMidiQuantisationGesture` name feels too specific but acceptable for LoopTake, keep it there; do not move this detail into Scene.

8. Clean up tests around generic target routing.
   - Keep tests for the externally visible quantisation behavior, but name them around interaction routing rather than Scene owning MIDI state.
   - Add or keep cases for:
     - station depth: hovered station + selected stations affect all their takes.
     - looptake depth: hovered take + selected takes affect those takes.
     - loop depth: hovered loop + selected loops affect their owning takes.
     - 2D controls first: if a station/take GUI child eats the touch, Scene does not start the 3D interaction command.
   - Optional near-term test: changing select depth mid-drag does not remap active targets if `_dragSelectDepth` is captured.

9. Verify with targeted build/tests.
   - Build tests with the direct `.vcxproj` command and exact `SolutionDir` trailing slash.
   - Run `JammaLib_Tests.exe --gtest_filter="LoopTakeMidiQuantisation.*:SceneMidiQuantisation.*"`.
   - Build `Jamma/Jamma.vcxproj` so the runtime binary picks up the changes.

**Relevant files**
- `JammaLib/src/engine/Scene.h` - remove `_midiQuantisationGestureTargets`; add generic `_dragTargets` and likely `_dragSelectDepth`; declare `_CurrentInteractionTargets()` and command-recipient adapter.
- `JammaLib/src/engine/Scene.cpp` - update touch-down/move/up routing to use generic drag capture; keep raw `_hoverPath3d`; remove gesture-specific cached state.
- `JammaLib/src/engine/LoopTake.h` - keep command-specific gesture API on LoopTake, not Scene.
- `JammaLib/src/engine/LoopTake.cpp` - keep quantisation gesture state and movement math here.
- `test/JammaLib_Tests/src/engine/MidiLoop_Tests.cpp` - retain and rename/expand regressions for target expansion by select depth.

**Verification**
1. Build tests:
   - `& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' test\JammaLib_Tests\JammaLib_Tests.vcxproj /m /t:Build /p:Configuration=Debug /p:Platform=x64 "/p:SolutionDir=C:\Users\matto\Source\Repos\Jamma.worktrees\Jamma-midi-quantisation\\" /v:minimal`
2. Run focused tests:
   - `& '.\test\JammaLib_Tests\bin\x64\Debug\JammaLib_Tests.exe' --gtest_filter="LoopTakeMidiQuantisation.*:SceneMidiQuantisation.*"`
3. Build app:
   - `& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' Jamma\Jamma.vcxproj /m /t:Build /p:Configuration=Debug /p:Platform=x64 "/p:SolutionDir=C:\Users\matto\Source\Repos\Jamma.worktrees\Jamma-midi-quantisation\\" /v:minimal`
4. Manual check:
   - In station, looptake, and loop select depth modes, hover + Ctrl+Shift-drag and confirm logs show all expected LoopTakes receiving start/drag/end.
   - Confirm dragging over a 2D UI control still uses the 2D control and does not start the 3D quantisation command.

**Decisions**
- Scene owns generic interaction targets, not command-specific gesture targets.
- Command-specific mapping is allowed as a stateless adapter from generic targets to recipients.
- LoopTake owns MIDI quantisation gesture semantics.
- Capture targets at touch-down so drags remain stable even if hover changes while dragging.

**Further Considerations**
1. Consider extracting generic target expansion into a small `InteractionTargetResolver` later if Scene continues to accumulate multi-select features.
2. Consider introducing typed command actions for future multi-fader/mute edits once there are at least two commands using this path.
3. Decide whether selected parent/child overlap should be flattened by active SelectDepth only, as recommended here, or whether future commands should support cross-depth mixed selections.