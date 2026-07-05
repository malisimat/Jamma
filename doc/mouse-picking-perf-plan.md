# Mouse Picking and Render Slowdown — Root Cause Analysis & Fix Plan

## Objective
Preserve hover/click/drag behavior while removing the severe render slowdown that
occurs whenever the mouse is moved — including movement over empty background.
A previous attempt (commit `8e8f8312`, "Improve mouse move performance and fix
render stuttering") did not resolve it and may have made it worse. This document
captures the **verified** root causes found by reading the current code on branch
`bugfix/mouse-render-slow`, and a prioritized, implementation-ready plan.

---

## Verified Architecture (read before changing anything)

- **Single UI thread does both message pump and rendering.**
  `Jamma/src/Main.cpp` (~line 387): `while (PeekMessage(&msg, ... PM_REMOVE)) { Translate/Dispatch }`
  drains **all** pending Windows messages, then calls `window.Render(); window.Swap();`.
  The `WindowProcedure` / `WM_MOUSEMOVE` handler runs on this same thread.
- **VSync is ON.** `JammaLib/src/graphics/Window.cpp:297` `wglSwapIntervalEXT(1)`.
  `Swap()` blocks until the next vertical blank, so the frame budget is ~16.6 ms at 60 Hz.
  Any per-frame CPU/GPU stall that pushes past a vblank halves the effective frame rate.
- **Windows coalesces `WM_MOUSEMOVE`.** The queue holds at most one pending move, so in
  practice the app processes roughly one move per frame — *the per-move cost is effectively
  a per-frame cost while the mouse is in motion.*
- **Job thread** (`Scene::_JobLoop`, `Scene.cpp` ~1727) runs every 20 ms and calls
  `OnJobTick` → `_PumpMidi` / `_PumpSerial`, which pass `_sceneMutex` down into
  `IoInputSubsystem` and hold it across MIDI/serial processing.
- **Audio thread** reads stations through a lock-free published snapshot
  (`AudioHost` `_audioStations` atomic `shared_ptr`). This path is healthy and is **not**
  the bottleneck — do not disturb it.

### Current per-frame work in `Window::Render()` (`Window.cpp:456`)
```
_scene.CommitChanges();                              // acquires _sceneMutex EVERY frame
_scene.InitResources(...);
_pickContext->Bind(); glClear(...);
_scene.Draw3d(*_pickContext, 1, PASS_PICKER);        // FULL scene redraw into picker FBO — UNCONDITIONAL
if (_hover3dDirty && _cachedCursorPosition) {
    objectId = _pickContext->GetPixel(pos);          // glReadPixels — SYNCHRONOUS GPU STALL
    if (objectId != _lastHoverObjectId) _scene.SetHover3d(...);
    _hover3dDirty = false;
}
_scene.ApplyDeferredHoverUpdates();                  // 2D hover path resolve + apply, every frame
... highlight pass, scene pass, 2D draw ...
```

---

## Root Causes (verified, ordered by impact)

### RC1 — Synchronous `glReadPixels` picker readback (PRIMARY differentiator)
`GlDrawContext::GetPixel` (`JammaLib/src/graphics/GlDrawContext.cpp:114`) calls
`glReadPixels(pos.X, pos.Y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &pixels)`.
`glReadPixels` from a framebuffer forces the driver to **flush and finish the GPU pipeline**
and stall the CPU until the pixel is available (commonly several ms, driver dependent).

This is the key reason moving the mouse tanks the frame rate while an idle mouse is smooth:
- Idle: `_hover3dDirty == false` → `GetPixel` is skipped → no stall.
- Moving: `_hover3dDirty == true` every frame → one full GPU sync stall **per frame**, on
  top of a VSync-locked frame budget. One stall that crosses a vblank boundary drops the
  frame rate from 60 → 30 (or worse).

### RC2 — `PASS_PICKER` full-scene redraw runs unconditionally every frame
`Window.cpp` (~line 445): `_scene.Draw3d(*_pickContext, 1, PASS_PICKER)` renders the entire
station/GUI hierarchy into the picker FBO **every frame**, regardless of whether hover can
have changed. This is constant overhead (not strictly mouse-specific) but it is pure waste
on frames where nothing pickable moved and the cursor did not move. It also feeds RC1: the
readback is only meaningful right after this draw.

### RC3 — Duplicated 2D hover work: per-message traversal + per-frame deferred hover (REGRESSION)
Commit `8e8f8312` intentionally **removed** the per-message full-tree traversal from
`Scene::OnAction(TouchMoveAction)` and replaced it with a deferred, once-per-frame hover
resolution (`_hover2dDirty` → `ApplyDeferredHoverUpdates` / `_ResolveHoverPath2d`).

On the current branch that traversal is **back** (re-introduced during later hover reworks,
`Scene.cpp:585-609`):
```cpp
for (auto it = _guiChildren.rbegin(); it != _guiChildren.rend(); ++it)
    (*it)->OnAction((*it)->ParentToLocal(action));   // recursive into GuiElement::OnAction
... _modeRadio->OnAction(...); _globalMidiQuantRadio->OnAction(...);
for (auto& station : _stations)
    station->OnAction(station->ParentToLocal(action));
```
`GuiElement::OnAction(TouchMoveAction)` (`GuiElement.cpp:436`) recurses over **all** children
and calls `ApplyHoverPoint` → `_HitTest`, mutating each element's `_state`.

Consequences:
1. **The GUI/station tree is now hover-resolved twice per frame** — once eagerly per message
   and once in `ApplyDeferredHoverUpdates`. Both are `O(total elements)`.
2. The two mechanisms **fight over `_state`**: the eager traversal sets `STATE_OVER/NORMAL`
   on every element it hit-tests, while the deferred path applies hover only along a single
   resolved path. This is a correctness smell in addition to wasted work.

Note: the per-element work itself is lock-free and allocation-free (good), so RC3 is a CPU/
redundancy problem, not a locking problem — but it compounds the per-frame budget already
strained by RC1.

### RC4 — `_sceneMutex` priority inversion between render and job threads (SECONDARY, real)
- **Render thread** takes `_sceneMutex` **every frame** inside `Scene::CommitChanges`
  (`Scene.cpp:1175`).
- **Job thread** (every 20 ms) takes `_sceneMutex` inside `OnJobTick` for tempo work
  (`Scene.cpp:915`) and, more importantly, holds it across `_PumpMidi` / `_PumpSerial`
  (`Scene.cpp:966,984`), which iterate stations/takes/loops/lanes inside
  `IoInputSubsystem` / `MidiRouter`.

When the job thread holds `_sceneMutex` during a long MIDI/serial pump, the render thread's
per-frame `CommitChanges` **blocks**, stalling rendering. This is independent of the mouse,
but it stacks with RC1–RC3 to produce compound stutter and should be addressed to make the
render loop robust. Keep the audio snapshot path untouched.

### RC5 — Per-frame heap churn in deferred hover (minor)
`_ResolveHoverPath2d` + `_ApplyHoverPath2d` + two `_LockHoverPath` calls
(`Scene.cpp` ~1300-1383) allocate several `std::vector`s and do `weak_ptr`→`shared_ptr`
locks every frame a hover change is pending. Minor next to RC1, but easy to reduce with
reusable member buffers once RC3 removes the duplicate path.

---

## Fix Plan (prioritized, small safe commits)

### Commit A — Kill the readback stall (RC1) + gate the picker (RC2)
Files: `JammaLib/src/graphics/Window.cpp`, `Window.h`,
`JammaLib/src/graphics/GlDrawContext.cpp`, `GlDrawContext.h`

Two-part change; do the gating first (cheap, safe), then async readback.

1. **Gate both the picker draw and the readback to frames that need it.**
   Introduce a single `_needsPick` decision computed at frame start:
   ```
   bool needsPick = _hover3dDirty || _forcePick;   // _forcePick set on resize/context recreate,
                                                    // button up/down, mode/state change
   ```
   Wrap `Draw3d(PASS_PICKER)` **and** `GetPixel` in `if (needsPick)`. On skipped frames,
   reuse `_lastHoverObjectId`. This removes RC2's constant cost and ensures the readback
   only happens when hover could actually change.

2. **Make the readback non-stalling (PBO ping-pong).**
   In `GlDrawContext`, add two `GL_PIXEL_PACK_BUFFER` PBOs. Each pick frame:
   - `glReadPixels(...)` targets the *current* PBO (async — returns immediately; no CPU wait).
   - Map/read the *previous* PBO to get the object id from the frame before.
   Consume the 1-frame-old id for hover. Hover is a visual affordance, so a one-frame delay
   is imperceptible. Keep a `GetPixelSync()` fallback for any case that needs an immediate
   exact id (e.g. click-down precision) — but prefer routing clicks through the same PBO
   result if latency proves acceptable.

   Safety: PBOs are created/destroyed on the render thread only (respect
   `GlDeleteQueue::IsRenderThread`). Recreate them in `ApplyPendingResize` alongside the
   other contexts. No new locks; no audio-thread involvement.

Expected effect: eliminates the per-frame GPU sync stall while moving — the single biggest win.

### Commit B — Remove duplicated 2D hover traversal (RC3)
Files: `JammaLib/src/engine/Scene.cpp`, `Scene.h`

Restore the intent of commit `8e8f8312`: `Scene::OnAction(TouchMoveAction)` should **not**
walk the whole GUI/station tree in the passive case. Keep only:
- popup handling,
- quantisation overlay drag (`_quantisationInteraction.TryHandleTouchMove`),
- background drag,
- active `_touchDownElement` forwarding,
then `_InvalidateHover2d()` and return `NoAction()`.

Delete the trailing `for (_guiChildren...) / _modeRadio / _globalMidiQuantRadio /
for (_stations...)` block (`Scene.cpp:585-609`). All passive hover is then resolved once per
frame by `ApplyDeferredHoverUpdates` → `_ResolveHoverPath2d`, which already uses
`FindTopmostDescendant` (a single top-down descent, not an all-elements sweep).

Validation focus: verify hover highlight, sticky selection, and the behaviors that the later
"sticky selection / better hover" commits (`8be8d8d`, `dcdd9b7`, `e1c6d87`) were tuning for
still work through the deferred path only. If any of those relied on the eager `_state`
writes, port that specific behavior into `_ApplyHoverPath2d` rather than re-adding the sweep.

### Commit C — Shrink `_sceneMutex` hold time / decouple render from job (RC4)
Files: `JammaLib/src/engine/Scene.cpp`, `Scene.h`, `JammaLib/src/io/IoInputSubsystem.*`,
`JammaLib/src/midi/MidiRouter.*`

Goal: the render thread's per-frame `CommitChanges` must never block for long on the job
thread. Options, least invasive first:

1. **Reduce lock scope in the pump path.** In `_PumpMidi` / `_PumpSerial`, only hold
   `_sceneMutex` around the minimal mutation that truly needs it; move MIDI event decode and
   per-lane iteration outside the lock where safe. Verify what state each guarded region
   actually protects.
2. **`try_lock` in `CommitChanges` (render side).** If the job thread holds the lock this
   frame, skip the commit and retry next frame instead of blocking the render. Station
   changes are already frame-coalesced, so a one-frame deferral is harmless.
3. If neither is sufficient, consider a dedicated smaller mutex for the render/commit path
   distinct from the MIDI pump path.

Safety: do **not** touch the audio snapshot (`AudioHost::_audioStations`). Keep changes on the
UI/job side only. Preserve existing lock ordering to avoid deadlock; document any new order.

### Commit D — Reduce per-frame allocations in deferred hover (RC5, optional)
Files: `JammaLib/src/engine/Scene.cpp`, `Scene.h`

After RC3 is removed, make `_hoverPath2d`, and the temporary shared/locked path vectors,
reusable member scratch buffers (`clear()` + reuse capacity) instead of allocating each frame.
Only pursue if profiling still shows churn.

### Commit E — Logging/TUI pressure (separate concern, keep last)
Files: `JammaLib/src/io/ConsoleTui.cpp`, hover/log call sites.

Ensure no `std::cout` / TUI writes happen on the per-frame hover path unless UI logging is
`verbose`. `SetHover3d` already gates its path logging behind `_loggingConfig.Ui == "verbose"`;
keep it that way and audit for any ungated high-frequency logging. Console writes on Windows
are synchronous and can stall the UI thread. Not the primary fix — do not let it delay A–C.

---

## Validation Plan

### Metrics (add temporary counters behind a debug guard)
1. Picker draw passes per second and read-pixel calls per second (expect near 0 when idle,
   ≤ frame rate when moving).
2. Frame time p50/p95 while moving the mouse rapidly over empty space.
3. Frame time p50/p95 while idle.
4. `_sceneMutex` render-side wait time p95 (to confirm RC4 impact).

### Functional checks
1. Hover highlight updates correctly (2D and 3D), including at rest after a move stops.
2. Click selection remains exact (no wrong object due to 1-frame readback latency).
3. Drag gestures (elements and background) remain smooth and correct.
4. Selection / mute / sticky-selection workflows unchanged.
5. Quantisation overlay interactions unchanged.
6. No audio glitches or added latency during rapid mouse movement.

### Scenarios
1. Fast movement over empty scene space (the reported failure case).
2. Fast movement across dense object regions.
3. Rapid click / drag start / end transitions.
4. Resize window, then move mouse (verifies `_forcePick` + PBO recreate).
5. Active MIDI input while moving the mouse (verifies RC4 fix).

---

## Rollout Order
1. **Commit A** — picker gating + PBO async readback (biggest win; RC1+RC2).
2. **Commit B** — remove duplicated per-message hover traversal (RC3).
3. **Commit C** — shrink `_sceneMutex` hold / `try_lock` render commit (RC4).
4. **Commit D** — deferred-hover allocation reuse (RC5, optional).
5. **Commit E** — logging/TUI hardening (separate concern).

Reassess after A+B: those alone are expected to remove most of the slowdown. C is about
robustness/stutter under MIDI load. Measure before doing D/E.

---

## Conciseness and Safety Principles
1. Keep diffs focused to the window/scene hot paths; no broad refactors while diagnosing.
2. Never add locks to, or otherwise perturb, the audio callback or the published station
   snapshot path.
3. Prefer behavior-preserving guards over logic rewrites; keep each change independently
   reversible.
4. Add minimal comments only where invariants are non-obvious (e.g. "hover id is 1 frame old").
5. Add temporary counters/telemetry behind debug guards for verification, then remove.
6. Keep hot-path code allocation-free, exception-free, and lock-free per repo real-time rules.
