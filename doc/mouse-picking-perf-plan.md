# Mouse Picking and Move-Path Performance Plan

## Objective
Preserve hover/click/drag behavior while removing extreme slowdown from fast mouse movement, especially over empty scene regions.

## Core Rule (must-have)
Do not run picker pass plus read-pixel continuously.
Only run picking once per rendered frame when there is unconsumed mouse input that could change hover.

Implementation intent:
1. Mouse move updates pending cursor state.
2. Frame start latches latest pending cursor state (position plus modifiers plus buttons).
3. If no newly latched state and no forced-pick condition, skip picker pass and skip read-pixel.
4. If newly latched state exists, run picker once for that frame using the latest latched cursor state, then mark it consumed.

This guarantees:
1. Latest cursor position right before frame is used.
2. At most one pick/read-pixel per frame.
3. No repeated pick/read-pixel work on idle frames.

## Targeted Fixes

### 1) Frame-coalesced cursor snapshot in Window
Files:
- JammaLib/src/graphics/Window.h
- JammaLib/src/graphics/Window.cpp

Plan:
1. Add pending cursor snapshot state and a consumed flag/version.
2. On WM_MOUSEMOVE path, update only pending state and set pending-dirty.
3. At render start, atomically/lazily capture pending state into frame-local cursor.
4. Drive pick scheduling from this captured state.

Safety notes:
1. Keep all state mutation on the window thread where possible.
2. Avoid introducing locks in render hot path.
3. Prefer simple monotonic generation counters if needed.

### 2) Conditional picker pass and conditional read-pixel
Files:
- JammaLib/src/graphics/Window.cpp

Plan:
1. Wrap PASS_PICKER draw in a shouldPickThisFrame check.
2. Wrap GetPixel call in same check.
3. Keep existing object-id change handling (SetHover3d only when id changes).

Forced-pick conditions (still pick even without new move):
1. First frame after resize/context recreate.
2. Mouse button down/up transition.
3. Any mode/state change that can alter pickability without cursor move.

Safety notes:
1. Leave scene and highlight passes unchanged.
2. Preserve exact click and drag start behavior.

### 3) Passive move fast-path in Scene
Files:
- JammaLib/src/engine/Scene.cpp

Plan:
1. In OnAction(TouchMoveAction), early-return in passive cases:
   - no popup,
   - no active touch-down element,
   - no background drag,
   - no active overlay drag gesture.
2. Still update cursor position and invalidate deferred hover state.
3. Skip deep per-element move traversal in this passive path.

Safety notes:
1. Do not break component-specific drag/edit interactions.
2. Ensure behavior is unchanged when buttons are down.

### 4) Optional pick-rate cap for passive hover
Files:
- JammaLib/src/graphics/Window.cpp

Plan:
1. Add optional passive hover pick interval (for example 16-33 ms).
2. Bypass cap for click down/up and drag start.

Safety notes:
1. Keep this behind a config constant/flag for easy rollback.
2. Validate no perceived hover lag regressions.

### 5) Optional async readback (phase 2)
Files:
- JammaLib/src/graphics/GlDrawContext.h
- JammaLib/src/graphics/GlDrawContext.cpp
- JammaLib/src/graphics/Window.cpp

Plan:
1. Move pick readback to PBO ping-pong so GPU/CPU sync stalls are reduced.
2. Consume prior-frame result for hover updates.
3. Keep a forced synchronous path for immediate click precision if required.

Safety notes:
1. Treat as phase 2 only after simpler gating/coalescing fixes.
2. Keep implementation narrow and isolated.

## Secondary Candidate (separate but useful)
Console/TUI logging pressure can amplify stutter when logs are noisy:
- JammaLib/src/io/ConsoleTui.cpp

Plan:
1. Keep high-frequency UI logs gated hard behind verbose checks.
2. Avoid logging inside tight mouse-move and hover loops.
3. Optional: queue TUI writes to a single output thread.

Note:
This is not the primary fix for empty-space mouse movement slowdown.

## Validation Plan

### Metrics to capture
1. Picker pass executions per second.
2. Read-pixel calls per second.
3. Frame time p50/p95 while moving mouse rapidly over empty space.
4. Frame time p50/p95 while idle.

### Functional checks
1. Hover highlight still updates correctly.
2. Click selection remains exact.
3. Drag gestures remain smooth and correct.
4. Selection/mute workflows unchanged.
5. Quantisation overlay interactions unchanged.

### Test scenarios
1. Fast mouse movement over empty scene space.
2. Fast movement across dense object regions.
3. Rapid click/drag start/end transitions.
4. Resize window then move mouse.

## Rollout Order (small safe commits)
1. Commit A: Frame-coalesced cursor snapshot and conditional picker/read-pixel.
2. Commit B: Scene passive move fast-path.
3. Commit C: Optional passive pick-rate cap (if still needed).
4. Commit D: Optional async readback (only if required).
5. Commit E: Logging/TUI improvements (separate concern).

## Conciseness and Safety Principles
1. Keep diffs focused to window/scene hot paths first.
2. Avoid broad refactors while diagnosing perf.
3. Add minimal comments only where invariants are non-obvious.
4. Prefer behavior-preserving guards over logic rewrites.
5. Add temporary counters/telemetry behind debug guards for verification.
6. Keep each change independently reversible.
