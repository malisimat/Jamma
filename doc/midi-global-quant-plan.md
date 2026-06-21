# MIDI Global + Local Quantisation Plan

## Context
This plan implements a 3-state global MIDI quantisation model plus per-LoopTake local state, with clear UI behavior and jam persistence.

Locked decisions from grill:
- Global state is tri-state: `Off`, `Mixed`, `All`.
- Local quant scope is `per LoopTake` (applies to all MIDI loops in that take).
- Persistence stores both:
  - Global enum (source of truth for runtime mode).
  - Per-LoopTake local settings (memory for `Mixed`).
- Any local quant edit forces global state to `Mixed` immediately.
- Global control is always visible near existing scene mode radio controls.

## Behavior Contract

### Global State Semantics
- `Off`:
  - Effective quant is disabled everywhere.
  - Local LoopTake enabled states are not destroyed; they are remembered.
- `Mixed`:
  - Effective quant follows each LoopTake's local `Enabled` state.
  - This is the "normal editable" local mode.
- `All`:
  - Effective quant is forced on for all LoopTakes.
  - Local LoopTake enabled states are preserved for later restore when returning to `Mixed`.

### Transition Rules
- Global radio click:
  - `Off -> Mixed`: restore local enabled states.
  - `Mixed -> All`: force all effective enabled.
  - `All -> Off`: disable effective quant everywhere.
- Local quant edit (toggle/fraction gesture/gui payload) while in any global state:
  - Apply the local edit to the target LoopTake state.
  - Set global state to `Mixed` immediately.

### Effective Enabled Formula
- `global == Off`: `effectiveEnabled = false`
- `global == All`: `effectiveEnabled = true`
- `global == Mixed`: `effectiveEnabled = localEnabled`

Local fields (`Fraction`, `GrainSamps`, `PhaseOffsetSamps`) continue to be owned per LoopTake.

## Data Model Changes

### 1) Add Global Enum to IO Model
File: `JammaLib/src/io/JamFile.h`
- Add enum:
  - `enum class GlobalMidiQuantState : std::uint8_t { Off = 0, Mixed = 1, All = 2 };`
- Add `JamFile` field:
  - `GlobalMidiQuantState GlobalMidiQuantState = GlobalMidiQuantState::Mixed;`

File: `JammaLib/src/io/JamFile.cpp`
- Parse/write top-level key, e.g. `globalmidiquantstate` string (`off|mixed|all`) or int (back-compat tolerant parser).
- Keep defaults backward compatible:
  - Missing key defaults to `Mixed`.

### 2) Persist Local LoopTake Enabled + Fraction (+ existing phase)
Current jam already stores `takephaseoffsetsamps` only.

File: `JammaLib/src/io/JamFile.h`
- Extend `JamFile::LoopTake` with:
  - `bool MidiQuantEnabled = false;`
  - `int MidiQuantFraction = 0;` (maps to `MidiQuantisationFraction`).

File: `JammaLib/src/io/JamFile.cpp`
- Parse/write keys inside each take object:
  - `midiquantenabled` (bool)
  - `midiquantfraction` (int clamped to valid fraction range)
- Preserve old files:
  - Missing fields -> current defaults (`Enabled=false`, `Fraction=Whole`).

## Engine Runtime Changes

### 3) Scene-Level Global Quant State Ownership
Files:
- `JammaLib/src/engine/Scene.h`
- `JammaLib/src/engine/Scene.cpp`

Add scene field:
- `_globalMidiQuantState` (tri-state enum).

Add helpers:
- `_SetGlobalMidiQuantState(GlobalMidiQuantState state, bool fromLocalEdit = false)`
- `_ApplyGlobalMidiQuantStateToAllLoopTakes()`
- `_ForceGlobalMidiQuantStateMixedOnLocalEdit()`

Load path:
- In `Scene::FromFile(...)`, after stations/takes are built, set scene global state from `jamStruct.GlobalMidiQuantState`, then apply.

### 4) LoopTake Effective Quantisation Resolver
Files:
- `JammaLib/src/engine/LoopTake.h`
- `JammaLib/src/engine/LoopTake.cpp`

Keep local packed settings as-is (this is the memory layer).

Add a lightweight override channel for effective enabled (non-destructive):
- `_globalForceQuantMode` or equivalent tri-state hint propagated from scene/station.
- `ResolvedMidiQuantisation()` computes `Enabled` using global mode + local enabled.

Important:
- Do **not** mutate local packed `Enabled` when global changes.
- Only local user edits mutate packed local settings.

### 5) Propagation Path (Scene -> Station -> LoopTake)
Files:
- `JammaLib/src/engine/Station.h`
- `JammaLib/src/engine/Station.cpp`

Add station method:
- `SetGlobalMidiQuantState(GlobalMidiQuantState state) noexcept;`

Implementation:
- Propagate to all current + back-buffer takes.
- Mark quantisation update pending when effective settings change.
- Keep this off callback-owned hot paths; no new locks in audio callback.

## UI Changes

### 6) Add Always-Visible Global Tri-State Radio
File: `JammaLib/src/engine/Scene.cpp`
- Create a new `GuiRadio` near `_modeRadio` for global quant state.
- 3 toggles: `Off`, `X`, `All`.
- `X` represents 'mixed'.
- Hook action receiver in `Scene::OnAction(GuiAction action)` and map radio index -> global enum.

Visual requirement:
- Stronger on-state visibility than current subtle quant display.
- Ensure active state has distinct texture/tint and hover/down variants.

### 7) Ctrl Overlay + Local Edit Behavior
Files:
- `JammaLib/src/timing/TimingQuantiser.cpp`
- `JammaLib/src/graphics/CtrlHandleOverlay.*` (if visual labels/colors need tweaks)

Current local edits already happen through fraction drag/toggle paths.
Update these paths so that after any local LoopTake quant edit:
- Scene global state is switched to `Mixed`.
- Global radio visual updates immediately.

Implementation option:
- Emit a focused action/event from LoopTake or quant controller up to Scene indicating local quant was changed.
- Scene handles mode switch centrally.

## Persistence Wiring

### 8) Save/Export Writers
Files:
- `JammaLib/src/io/IoSessionExporter.cpp`
- Any future/default jam save builder path (currently only exporter clearly assembles `JamFile`)

Populate new fields:
- `jam.GlobalMidiQuantState`
- For each `jamTake`:
  - `MidiQuantEnabled`
  - `MidiQuantFraction`
  - (existing `TakePhaseOffsetSamps` remains)

Note:
- Current app startup path loads default jam in `Jamma/src/Main.cpp`.
- This plan includes schema compatibility there via `JamFile::FromStream` only.
- If/when app writes back default jam/session jam outside exporter, that writer must also include these new fields.

## Backward Compatibility
- Old jam files missing new keys should load with:
  - global state = `Mixed`
  - local take enabled/fraction defaults
- Unknown global state values clamp/fallback to `Mixed`.
- Fraction values out of range clamp to nearest valid enum index.

## Test Plan

### IO Unit Tests
File: `test/JammaLib_Tests/src/io/JamFile_Tests.cpp`
Add tests for:
- Round-trip of `globalmidiquantstate` values (`off|mixed|all`).
- Round-trip of per-take `midiquantenabled` + `midiquantfraction`.
- Missing keys fallback behavior.
- Out-of-range fraction/global values clamping/fallback.

### Engine Behavior Tests (new or existing engine test files)
- `Off` forces `ResolvedMidiQuantisation().Enabled == false` for all takes.
- `All` forces `ResolvedMidiQuantisation().Enabled == true` for all takes.
- `Mixed` restores local enabled values exactly.
- Local edit in `Off`/`All` flips global state to `Mixed` and applies edited local state.

### Manual QA
1. Start with mixed local pattern across takes.
2. Toggle global `All`, confirm all quant active.
3. Toggle global `Off`, confirm none active.
4. Toggle back to `Mixed`, confirm original local pattern restored.
5. While global `All`, edit one take local quant -> global auto switches to `Mixed`.
6. Save/export jam, reload, confirm global + local states match before save.

## Threading / RT Safety Guardrails
- No mutexes or blocking in callback-owned code paths.
- Global-mode propagation happens on scene/action thread and uses existing publish/update mechanisms.
- Reuse existing atomic/pending-update patterns in `LoopTake` and station commit flow.

## Implementation Sequence (recommended)
1. Add `JamFile` schema fields + parser/writer updates.
2. Extend `IoSessionExporter` to write new fields.
3. Add scene global tri-state state + radio UI + action handling.
4. Add propagation APIs Scene -> Station -> LoopTake.
5. Update LoopTake resolved quant logic to honor global state without mutating local memory.
6. Add local-edit -> global mixed transition hook.
7. Add/adjust tests, then run build + relevant test target.

## Risks and Mitigations
- Risk: accidental destruction of local enabled memory when switching global modes.
  - Mitigation: never overwrite packed local settings from global transitions.
- Risk: UI ambiguity between forced and local states.
  - Mitigation: global radio always visible; ctrl overlay remains local edit affordance.
- Risk: schema drift between exporter and other save paths.
  - Mitigation: centralize jam assembly helper if additional save paths are introduced.

## Done Criteria
- Global radio supports `Off`, `Mixed`, `All` exactly as specified.
- Local edits always force global -> `Mixed`.
- Effective runtime quant behavior matches formulas above.
- Jam round-trip preserves global mode + per-take local quant fields.
- Unit tests cover parse/write and core mode behavior.
