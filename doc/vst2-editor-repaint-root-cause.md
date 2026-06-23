# VST2 Editor Repaint — Root Cause Analysis

## Purpose

This document explains, in reproducible detail, **why a hosted VST2 plugin editor
(notably Native Instruments Battery 4) would paint exactly once and then never
repaint again** when opened inside Jamma, and what the underlying cause turned
out to be. It is written so the failure can be deliberately reproduced and so the
fix can be understood and defended against regressions.

It deliberately covers only the **repaint** failure. It does not discuss any
input/event-routing behaviour.

## Symptom

When a VST2 plugin with an editor (`effFlagsHasEditor`) was loaded and its editor
window was opened:

- The editor drew its initial frame correctly (background, controls, current
  values all visible).
- After that first frame it became **static**: animated meters, value readouts,
  and any control whose appearance should change over time were frozen.
- The plugin was clearly still alive — it continued to emit a high rate of host
  callbacks (`audioMasterGetTime`, `audioMasterGetCurrentProcessLevel`) — but its
  GUI never issued another paint.

Reference behaviour: the Steinberg VST2 SDK **minihost** sample hosts the same
plugin and repaints continuously. The goal was to match minihost.

## Reproduction Setup

- Windows, x64, Debug.
- Jamma built with `JAMMA_VST2_ENABLED`.
- A VST2 plugin whose editor performs continuous/animated drawing and which binds
  its GUI/idle dispatch to the thread that instantiated the effect. Battery 4 is
  the canonical reproducer; many NI plugins behave the same way.
- Load the plugin into a station/loop chain, then open its editor.

To observe the freeze clearly, pick a plugin view with a constantly moving
element (a level meter, a clock, a spectrum). It will move for one frame's worth
of state and then stop.

## Root Cause (primary): thread affinity between `effOpen` and `effEditOpen`

The decisive cause was that **the plugin instance was created and opened on a
different thread from the one that drove its editor**.

In the original code:

1. `Vst2Plugin::Load(...)` ran on Jamma's **job thread**
   (`_jobRunner` → `OnJobTick` → receiver `OnAction` → `plugin->Load`). That is
   where the plugin entry point `VSTPluginMain()` was called, where the returned
   `AEffect` was obtained, and where `effOpen` / `effMainsChanged` were
   dispatched. In other words, the effect was **instantiated and opened on the
   job thread**.

2. `VstEditorWindow` / `Vst2Plugin::OpenEditor(...)` ran on Jamma's **UI thread**
   (the thread that owns the Win32 message pump and the OpenGL render context).
   That is where `effEditOpen`, `effEditGetRect`, and the periodic `effEditIdle`
   were dispatched.

This split violates an assumption that many VST2 plugins make. The Steinberg
minihost calls `VSTPluginMain()`, `effOpen`, **and** `effEditOpen` all on the
**same** (main) thread. NI-style plugins bind their GUI message/idle machinery to
the thread that *instantiated* the effect. When `effOpen` happens on thread A but
`effEditOpen` / `effEditIdle` arrive on thread B, the plugin's internal GUI
dispatch is wired to thread A while paint/idle requests come from thread B. The
result is exactly the observed symptom: the editor builds and paints once, then
its idle/repaint pump is effectively orphaned and never fires again.

### Why it looked like an idle/timer problem first

Because the editor was driven by an `effEditIdle` timer, it was tempting to blame
idle cadence. It was not the cadence. Even at a 20 ms (~50 Hz) idle cadence the
editor stayed frozen, because the idle calls were crossing a thread boundary the
plugin did not expect, so the plugin's own repaint logic never engaged.

## Contributing Causes (secondary)

Before the thread-affinity cause was identified and fixed, three other issues
were found and corrected. Each one independently degraded editor repaint and had
to be right for the editor to behave, but none of them was the primary cause.

### C1 — Plugin parented into an intermediate child window

The plugin editor was originally parented into an intermediate `STATIC` child
window, and `_editorParentHwnd` pointed at that `STATIC`. Host-callback posts
(`audioMasterSizeWindow`, `audioMasterIdle`, `audioMasterUpdateDisplay`) were
`PostMessage`d to that `STATIC`, whose default window procedure silently dropped
them. The plugin's requests to be idled/resized/redrawn therefore went nowhere.

Fix: parent the plugin **directly** into the editor frame `HWND`
(`OpenEditor(wnd)`), so host-callback posts land on
`VstEditorWindow::WindowProcedure` and are actually handled.

### C2 — `audioMasterGetCurrentProcessLevel` always reported realtime

The host callback answered `audioMasterGetCurrentProcessLevel` with
`kVstProcessLevelRealtime` whenever the plugin was loaded — including when the
query arrived on the UI/editor thread. Plugins use this value to decide whether
they are allowed to do GUI work; a realtime answer on the GUI thread makes them
skip repaint.

Fix: make the answer thread-aware. Stamp `_audioThreadId` at the top of every
`ProcessBlock*` call and return `kVstProcessLevelRealtime` only when
`GetCurrentThreadId() == _audioThreadId`; otherwise return
`kVstProcessLevelUser`.

### C3 — Host-callback logging flood pegged the UI thread

The host callback unconditionally wrote two flushing
`std::cout << ... << std::endl` lines per invocation. Battery 4 spams
`audioMasterGetTime` and `audioMasterGetCurrentProcessLevel` thousands of times
per second from the UI thread. The synchronous, flushing console I/O saturated
the UI thread so thoroughly that the editor had no opportunity to repaint.

Fix: gate tracing behind a hot-opcode filter and drop per-idle/per-timer trace
output. (All of this diagnostic logging has since been removed entirely.)

## Related Consequence: OpenGL context theft (whole-app white screen)

Once `VSTPluginMain()` / `effOpen` were moved onto the UI thread to fix the
primary cause, a second, closely-related repaint failure appeared — important to
record because it is a direct consequence of the fix and is easy to reintroduce.

Jamma's UI thread also owns the OpenGL render context. Some plugins (Battery 4
included) make **their own** GL context current during `VSTPluginMain()` /
`effOpen` and never restore the previous one. Because `Window::Render()` does
**not** re-assert `wglMakeCurrent(_dc, _rc)` every frame, the next render found an
incomplete framebuffer. The visible result was log spam
("Framebuffer is not complete: Unknown error") and the **entire application
painting white** on VST load. The same hazard applies to `effEditOpen`,
`effEditIdle`, and `effEditClose`, all of which run on the UI thread from the
message pump before `Render()`.

Fix: a small RAII guard, `Vst2Plugin::GlContextScope`. Its constructor snapshots
`wglGetCurrentContext()` and `wglGetCurrentDC()`; its destructor restores them via
`wglMakeCurrent(...)`, but only if a context was captured (so the job-thread
fallback path, which has no GL context, is a no-op). The guard wraps every
UI-thread dispatch into plugin code: effect instantiation, `effEditOpen`,
`effEditIdle`, and `effEditClose`. The `wgl*` functions are available through
`windows.h` and `opengl32.lib`, which JammaLib already links.

## The Fix (summary)

- **Instantiate and open the effect on the UI thread.** `VSTPluginMain()`,
  `effOpen`, and the name/channel/buffer setup were moved into
  `Vst2Plugin::_InstantiateEffect()`, which is called from `Vst2Plugin::PreInit()`
  on the UI thread (inside `Scene::CommitChanges()`, driven from `Window.cpp`).
  `Vst2Plugin::Load()` (still on the job thread) now only performs
  `effSetSampleRate` / `effSetBlockSize` / `effSetProgram` / `effMainsChanged`
  and reuses the already-instantiated effect; it only instantiates as a
  job-thread fallback if `PreInit()` was skipped. The instance handed to the
  receiver (`action.PreInitPlugin`) is the same instance later loaded, so the
  UI-thread `effOpen` survives into the editor. `processReplacing` continues to
  run on the audio thread, which is correct.
- **Parent the plugin directly into the editor frame HWND** so host-callback
  posts are delivered (C1).
- **Answer `audioMasterGetCurrentProcessLevel` per-thread** (C2).
- **Restore the GL context around UI-thread plugin dispatch** via
  `GlContextScope` (the white-screen consequence above).

## Verification

- Build: full solution build in Debug x64.
- Focused native tests:
  `test/JammaLib_Tests/bin/x64/Debug/JammaLib_Tests.exe --gtest_filter=*Vst2Plugin*`
- Manual: open the editor of an animated plugin (Battery 4) and confirm it
  continues to repaint (meters/clocks keep moving) for as long as the editor is
  open, matching minihost.

## How to Reproduce the Failure Again (for regression testing)

To re-create the original repaint freeze deliberately:

1. Move `VSTPluginMain()` / `effOpen` back onto the job thread (i.e. instantiate
   in `Load()` instead of in `PreInit()` on the UI thread) while keeping
   `effEditOpen` / `effEditIdle` on the UI thread. The editor will paint once and
   freeze.
2. Or, to re-create the white-screen consequence: keep instantiation on the UI
   thread but remove the `GlContextScope` guards. Loading a plugin that steals
   the GL context will turn the whole app white and emit framebuffer-incomplete
   errors.
