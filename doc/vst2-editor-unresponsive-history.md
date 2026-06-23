# VST2 Editor Unresponsive History (Battery 4)

## Purpose

This document records all significant changes attempted to fix the VST2 editor freeze/unresponsive behavior (especially Battery 4), with enough detail to rollback or bisect quickly.

## Current Symptom Snapshot

- App shutdown is now stable/clean (no teardown crash/hang in the latest runs).
- Editor still intermittently appears stuck or slow to repaint/respond.
- Plugin emits high-frequency host callbacks (`audioMasterGetTime`, `audioMasterGetCurrentProcessLevel`) while editor is open.

## Change Timeline

### 1. Baseline VST2 host/editor compatibility expansion

Files touched:
- `JammaLib/src/graphics/VstEditorWindow.h`
- `JammaLib/src/graphics/VstEditorWindow.cpp`
- `JammaLib/src/vst/Vst2Plugin.h`
- `JammaLib/src/vst/Vst2Plugin.cpp`
- `JammaLib/src/vst/IVstPlugin.h`

Key changes:
- Added custom editor window messages for VST2 host interactions:
  - `MessageVst2SizeWindow`
  - `MessageVst2Idle`
- Added child window tracking (`_pluginChildWnd`) and host-child topology (`_editorHostWnd` as plugin parent).
- Increased idle cadence from coarse interval to `20 ms` timer in editor window.
- Expanded fast-idle trigger messages in `WH_CALLWNDPROCRET` hook.
- Added focus routing in window proc (`WM_SETFOCUS`, `WM_MOUSEACTIVATE`, `WM_ACTIVATE`) to push focus into plugin child.
- Added VST2 lifecycle hooks in plugin interface:
  - `OnEditorActivated()` -> `effEditTop`
  - `OnEditorDeactivated()` -> `effEditSleep`
- Added/expanded host callback handling:
  - `audioMasterUpdateDisplay`
  - `audioMasterSizeWindow`
  - `audioMasterIdle`
  - `audioMasterAutomate`
  - `audioMasterBeginEdit`
  - `audioMasterEndEdit`
- Added broad trace logging (`[VST2 TRACE]`, `[VST EDITOR TRACE]`) for callback/message/lifecycle visibility.

### 2. Shutdown stability work (deterministic unload)

Files touched:
- `JammaLib/src/engine/Loop.h/.cpp`
- `JammaLib/src/engine/LoopTake.h/.cpp`
- `JammaLib/src/engine/Station.h/.cpp`
- `JammaLib/src/engine/Scene.h/.cpp`
- `Jamma/src/Main.cpp`

Key changes:
- Added `ForceUnloadAllVstPlugins()` across loop/take/station/scene ownership chain.
- Drained active/back VST chains into UI-thread destroy queue deterministically.
- Invoked force-unload on quit path before close-audio / teardown.
- Added shutdown trace markers and destroy-queue drain counts.

Outcome:
- Clean app quit achieved in latest user report.

### 3. Bootstrap regression and fix

Issue introduced:
- Host callback guard returned `0` too early when `self` was null/unloaded.
- During `VSTPluginMain`, plugin host query failed (`audioMasterVersion`), causing load failure (`null or wrong magic`).

Fix:
- Restored bootstrap-safe responses when `effect->user` is not wired yet:
  - `audioMasterVersion`
  - vendor/product/version strings

Outcome:
- Plugin load path restored.

### 4. Process-level response refinement

Files touched:
- `JammaLib/src/vst/Vst2Plugin.h/.cpp`

Key changes:
- Added thread role tracking:
  - `_audioThreadId`
  - `_uiThreadId`
- Updated `audioMasterGetCurrentProcessLevel` response:
  - `Realtime` on known audio thread
  - `User` on known UI thread
  - `Prefetch` otherwise

Reason:
- Avoid always reporting realtime while activated, which can make plugin UI logic misclassify context.

### 5. Editor-open callback race fix + sync resize path

Files touched:
- `JammaLib/src/vst/Vst2Plugin.h/.cpp`
- `JammaLib/src/graphics/VstEditorWindow.cpp`

Key changes:
- Added opening-transition state:
  - `_isEditorOpening`
- In `OpenEditor(...)`, publish parent/opening state before `effEditOpen`, then mark open.
- Host callback now treats editor as active when `isOpen || isOpening`.
- `audioMasterUpdateDisplay` behavior made permissive during transitions.
- Added sync size-window request path:
  - `MessageVst2SizeWindowSync` (`WM_APP + 0x122`)
  - `SendMessageTimeout(..., 100ms)` for truthful resize ack
  - fallback async post path remains.

Reason:
- Prevent loss of plugin callbacks during `effEditOpen` bootstrap and make size negotiation less ambiguous.

### 6. Logging flood reduction (current)

Files touched:
- `JammaLib/src/vst/Vst2Plugin.cpp`

Key changes:
- Added host-callback trace filter to suppress spam by default for:
  - `audioMasterGetTime`
  - `audioMasterGetCurrentProcessLevel`
- Retains trace output for less frequent/editor-significant callback opcodes.

Reason:
- Reduce console flood that can mask actionable events and potentially affect responsiveness.

## What Has Been Verified Repeatedly

- Incremental and full solution builds have been passing after each patch set.
- Focused native tests have remained green:
  - `test/JammaLib_Tests/bin/x64/Debug/JammaLib_Tests.exe --gtest_filter=*Vst2Plugin*`

## Known Remaining Risks

- `effKeysRequired` probing and conditional key-forward compatibility path has not yet been added.
- Parent/child painting assumptions may still be plugin-specific and need targeted validation per plugin.
- Existing tests do not yet exercise full editor-host window behavior with a fake plugin/editor seam.

## Suggested Rollback/Bisect Strategy

If needed, bisect in this order (most likely editor-responsiveness impact first):

1. Keep shutdown stability changes; do **not** revert deterministic unload first.
2. Toggle trace throttling only (safe/no behavior impact expected).
3. Toggle editor-open transition behavior (`_isEditorOpening` + `editorActive` logic).
4. Toggle sync size-window path (`MessageVst2SizeWindowSync` + `SendMessageTimeout`).
5. Toggle process-level thread-role reporting.
6. Toggle focus/idle expansion changes in editor window proc/hook.

This ordering minimizes reintroducing known shutdown regressions while isolating editor behavior changes.
