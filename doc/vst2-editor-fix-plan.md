# VST2 Editor Fix Plan

## Summary

The repaint bug is almost certainly an editor-host compatibility gap, not a plain `WM_PAINT` mistake. The Steinberg minihost is only the lower-bound baseline. More fully-featured VST2 hosts, especially JUCE-based ones, add a tighter editor idle loop, richer `audioMaster*` support, plugin-driven resize handling, focus routing, and selective keyboard compatibility workarounds. Jamma already has the right basic shape, but it is still missing several pieces that real-world plugins expect.

The strongest signal from the host survey is this:

- `MiniVST` is too minimal to be a compatibility target. It barely implements `audioMasterVersion` and does not represent what broad plugin support needs.
- `Element`, `DirectPipe`, and `Pedalboard2` all lean on JUCE's VST2 host implementation. That stack is much closer to the practical compatibility target for editor behavior.

That shifts the plan from "copy minihost" to "match minihost baseline first, then close the specific JUCE-style compatibility gaps that real plugins use".  Note we explicitly do NOT support JUCE or have any dependency on it in Jamma, we are just using those implementations to help guide any potential lacking features or details.

In Jamma, that work is not abstract. The concrete owning path is:

1. `vst::VstEditorWindowManager::HandleVstEditorOpen()`
2. `vst::VstEditorWindowManager::OpenVstEditorForPlugin()`
3. `graphics::VstEditorWindow::Create()`
4. `vst::IVstPlugin::OpenEditor()`
5. `vst::Vst2Plugin::OpenEditor()`
6. `graphics::VstEditorWindow::WindowProcedure()` and `graphics::VstEditorWindow::CallWndRetProc()`
7. `vst::Vst2Plugin::IdleEditor()` and `vst::Vst2Plugin::HostCallback()`

That is the codepath this plan is targeting.

## Reference Host Findings

### MiniVST: useful lower bound, not enough for compatibility

`MiniVST` confirms the core Steinberg lifecycle shape:

1. Instantiate plugin.
2. `effOpen`.
3. `effSetSampleRate`.
4. `effSetBlockSize`.
5. `effMainsChanged(1)`.
6. `effEditOpen(parent)`.
7. `effEditGetRect`.

It is still far too thin to model robust host behavior:

- Host callback only meaningfully answers `audioMasterVersion`.
- No meaningful editor resize compatibility.
- No serious host capability reporting.
- No real plugin-to-host editor contract beyond the bare minimum.

Takeaway: keep minihost as the baseline lifecycle reference, but do not stop there.

### JUCE hosts: the practical compatibility target

`Element`, `DirectPipe`, and `Pedalboard2` are all effectively wrappers around JUCE's VST2 hosting layer, so the important behavior is the shared JUCE implementation rather than the app-specific glue.

That common host behavior includes:

- Regular editor idle pumping on the UI thread at roughly `18-20 ms` cadence.
- Full editor lifecycle calls such as `effEditOpen`, `effEditGetRect`, `effEditIdle`, and `effEditClose`.
- Support for plugin-driven resize via `audioMasterSizeWindow`.
- Host callback responses for common plugin queries such as `audioMasterCanDo`, `audioMasterGetTime`, `audioMasterGetSampleRate`, `audioMasterGetBlockSize`, vendor/product identity, and automation notifications.
- Selective focus and keyboard workarounds when the plugin asks for keys or fails to route focus cleanly.
- Editor hosting on the message thread, with plugin UI lifetime tied explicitly to the host window lifetime.

Takeaway: this is the compatibility bar Jamma should target for VST2 editor hosting.

## Required VST2 Interface Inventory

This is the concrete interface surface the implementation should explicitly account for.

### Host -> plugin calls

Required lifecycle and editor calls:

- `effOpen`
- `effClose`
- `effSetSampleRate`
- `effSetBlockSize`
- `effMainsChanged`
- `effEditOpen`
- `effEditGetRect`
- `effEditIdle`
- `effEditClose`

Calls to evaluate and likely support for editor compatibility:

- `effEditTop`
- `effEditSleep`
- `effKeysRequired`

Calls already relevant elsewhere in the host and worth keeping correct while touching this area:

- `effProcessEvents`
- `effGetChunk`
- `effSetChunk`

### Plugin -> host calls

These host callback opcodes are the ones that matter most for editor compatibility and basic plugin expectations.

Must be implemented or preserved accurately:

- `audioMasterVersion`
- `audioMasterGetSampleRate`
- `audioMasterGetBlockSize`
- `audioMasterGetTime`
- `audioMasterGetCurrentProcessLevel`
- `audioMasterGetVendorString`
- `audioMasterGetProductString`
- `audioMasterGetVendorVersion`
- `audioMasterCanDo`

Must be added or upgraded for editor robustness:

- `audioMasterSizeWindow`
- `audioMasterIdle`
- `audioMasterUpdateDisplay`
- `audioMasterAutomate`
- `audioMasterBeginEdit`
- `audioMasterEndEdit`

Should be reviewed while instrumenting to avoid silent plugin breakage:

- `audioMasterProcessEvents`
- `audioMasterWantMidi`
- `audioMasterIOChanged`

The rule here is simple: answer only what Jamma actually supports, but do not silently drop common editor-facing callbacks without at least tracing them during the investigation pass.

## What Jamma Already Gets Right

- `effEditOpen(parentHwnd)` already happens before `effEditGetRect(&rect)` in `Vst2Plugin::OpenEditor()`.
- The editor frame already uses `WS_CLIPCHILDREN | WS_CLIPSIBLINGS`.
- The parent frame already suppresses background erasure and validates paint without drawing over the child.
- `audioMasterGetTime` already returns a non-null `VstTimeInfo*` when host timing state exists.

Those parts should be preserved.

## Known Gaps To Close

- Idle cadence is still too coarse at `50 ms`; the stronger host references sit near `20 ms`.
- Fast idle nudges are too narrow; `WM_MOUSEMOVE` alone is not enough.
- `ResizeEditorHostWindow()` is still a stub.
- `audioMasterSizeWindow` is still not part of the real host contract.
- The host does not explicitly track the plugin child HWND after editor open.
- Focus activation is still weak, which can break repainting, keyboard, or plugin-owned timers.
- Common editor-facing callbacks may still be getting dropped silently.
- Tests still cover the callback surface only lightly and do not exercise the editor host behavior itself.

## Current Jamma Code Path

### Editor open path

The current editor-open flow is:

1. `vst::VstEditorWindowManager::HandleVstEditorOpen()` picks a loaded plugin from the hovered target or station scan.
2. `vst::VstEditorWindowManager::OpenVstEditorForPlugin()` deduplicates already-open editors, restores/focuses an existing window if one exists, or allocates a new `graphics::VstEditorWindow`.
3. `graphics::VstEditorWindow::Create()` registers the Win32 class, creates the top-level frame, stores `_editorWnd` and `_editorHostWnd`, then calls `_plugin->OpenEditor(wnd)`.
4. For VST2 plugins, `vst::Vst2Plugin::OpenEditor()` dispatches `effEditOpen`, then `effEditGetRect`, and writes the result into `_editorSize`.
5. Back in `graphics::VstEditorWindow::Create()`, Jamma resizes the outer frame using `_plugin->GetEditorSize()`, shows the window, starts the idle timer, and installs the `WH_CALLWNDPROCRET` hook.

### Current idle path

Jamma currently drives editor updates through two paths inside `graphics::VstEditorWindow`:

- `WindowProcedure(..., WM_TIMER, ...)` calls `_plugin->IdleEditor()` every `50 ms`.
- `CallWndRetProc()` watches for `WM_MOUSEMOVE` on child windows and immediately calls `_plugin->IdleEditor()` when the root window matches one of `s_activeEditorWindows`.

For VST2 plugins, `vst::Vst2Plugin::IdleEditor()` is a thin wrapper over `effEditIdle`.

### Current close path

There are two close paths today:

- `graphics::VstEditorWindow::Destroy()` removes the window from the hook-tracking list, exchanges `_editorWnd` to null, calls `_plugin->CloseEditor()`, then destroys the HWND.
- `graphics::VstEditorWindow::WindowProcedure(..., WM_CLOSE, ...)` also exchanges `_editorWnd` to null, calls `_plugin->CloseEditor()`, and destroys the HWND.

For VST2 plugins, `vst::Vst2Plugin::CloseEditor()` currently only dispatches `effEditClose` and clears `_editorSize`.

### Current resize path

`graphics::VstEditorWindow::ResizeEditorHostWindow()` is still a stub. `WM_SIZE` reaches it, but no frame or child-host sizing work is done there yet.

The local reference shape for resize logic already exists on the VST3 side in `HostPlugFrame::resizeView()` inside `vst::Vst3Plugin.cpp`. That method computes the outer frame size with `AdjustWindowRectEx`, resizes the frame with `SetWindowPos`, optionally resizes the child host window, and finally calls `view->onSize(newSize)`.

### Current VST2 host callback path

All plugin-to-host VST2 callbacks currently funnel through `vst::Vst2Plugin::HostCallback()`.

Jamma already handles:

- `audioMasterVersion`
- `audioMasterGetSampleRate`
- `audioMasterGetBlockSize`
- `audioMasterGetTime`
- `audioMasterGetCurrentProcessLevel`
- `audioMasterGetVendorString`
- `audioMasterGetProductString`
- `audioMasterGetVendorVersion`
- `audioMasterCanDo`
- `audioMasterAutomate`
- `audioMasterIdle`

`audioMasterAutomate` already feeds Jamma's editor-automation path through `vst::PublishLastTouchedParameter()`, which is later consumed in `midi::MidiRouter::_ConsumeEditorAutomation()`.

What is notably missing from this callback path for editor hosting is `audioMasterSizeWindow`, plus any explicit handling for `audioMasterUpdateDisplay`, `audioMasterBeginEdit`, and `audioMasterEndEdit`.

## Implementation Plan

### Step 1: Instrument the current editor-host path before changing behavior

Add temporary trace points at the concrete owning methods:

- `vst::VstEditorWindowManager::OpenVstEditorForPlugin()`
- `graphics::VstEditorWindow::Create()`
- `graphics::VstEditorWindow::WindowProcedure()`
- `graphics::VstEditorWindow::CallWndRetProc()`
- `vst::Vst2Plugin::OpenEditor()`
- `vst::Vst2Plugin::CloseEditor()`
- `vst::Vst2Plugin::IdleEditor()`
- `vst::Vst2Plugin::HostCallback()`

Trace at least these events:

- `effEditOpen`
- `effEditGetRect`
- `effEditIdle`
- `effEditClose`
- `audioMasterSizeWindow`
- `audioMasterIdle`
- `audioMasterUpdateDisplay`
- `audioMasterAutomate`
- `audioMasterBeginEdit`
- `audioMasterEndEdit`
- `audioMasterProcessEvents`
- `WM_TIMER`
- `WM_MOUSEMOVE`
- mouse button down and up messages
- mouse wheel messages
- `WM_SETFOCUS`
- `WM_MOUSEACTIVATE`
- `WM_ACTIVATE`
- `WM_SIZE`

Goal: get a concrete Battery trace showing whether the repaint failure is caused by missing idle cadence, unsupported resize requests, lost focus, or an unhandled host callback.

### Step 2: Align the baseline lifecycle with Steinberg's minihost

Keep the existing correct order:

1. `effEditOpen(parentHwnd)`
2. `effEditGetRect(&rect)`

Then tighten the baseline behavior:

- In `graphics::VstEditorWindow::Create()`, change `SetTimer(wnd, 1, 50, nullptr)` to a `20 ms` cadence.
- Keep all editor idle dispatch on the UI thread.
- Preserve the existing `WM_CLOSE` and `Destroy()` ordering where `vst::Vst2Plugin::CloseEditor()` runs before `DestroyWindow()`.
- Keep `vst::Vst2Plugin::OpenEditor()` as the owner of the initial `effEditGetRect` query and `_editorSize` update.

This is the minimum bar.

### Step 3: Broaden idle compatibility beyond the fixed timer

Keep the periodic `20 ms` timer in `graphics::VstEditorWindow::WindowProcedure()`, but broaden the immediate idle path in `graphics::VstEditorWindow::CallWndRetProc()`.

Expand the current fast-idle trigger set from `WM_MOUSEMOVE` to:

- left, right, and middle button down
- left, right, and middle button up
- double-click messages
- mouse wheel messages
- focus gained
- activation gained

Also make `audioMasterIdle` meaningful inside `vst::Vst2Plugin::HostCallback()`. The current code directly dispatches `effEditIdle` from the callback; during implementation, verify that this stays UI-thread-safe and convert it to a host-side prompt if the traces show cross-thread risk.

### Step 4: Implement the real editor resize contract

Add a narrow API from `vst::Vst2Plugin::HostCallback()` into `graphics::VstEditorWindow` for plugin-requested editor resizing.

Implement `audioMasterSizeWindow` so that it:

1. validates the active editor host exists,
2. resizes the host client area to the requested plugin editor size,
3. updates the outer frame size with the correct Win32 non-client math,
4. returns success or failure explicitly.

Replace `ResizeEditorHostWindow()` with real logic that:

- resizes the frame window,
- keeps any tracked plugin child window aligned with the client area,
- behaves correctly on `WM_SIZE`,
- does not assume the plugin child can resize itself without host cooperation.

Use `HostPlugFrame::resizeView()` in `vst::Vst3Plugin.cpp` as the local shape reference for the Win32 sizing math, not as a direct abstraction to reuse.

If instrumentation shows a plugin changes size after open, re-run `effEditGetRect` inside `vst::Vst2Plugin::OpenEditor()` or the new resize callback path and use the latest valid bounds.

### Step 5: Track the plugin child HWND and harden focus routing

After `vst::Vst2Plugin::OpenEditor()` returns inside `graphics::VstEditorWindow::Create()`, discover and store the plugin child HWND. The simplest implementation is to snapshot child windows before and after `_plugin->OpenEditor(wnd)`, or enumerate child windows after open and capture the new editor child.

Use that handle to improve focus behavior:

- on `WM_SETFOCUS`, forward focus to the plugin child,
- on `WM_MOUSEACTIVATE`, ensure the plugin child becomes the active focus target,
- on `WM_ACTIVATE`, restore focus into the plugin child when reactivating the frame.

Do not add blind parent-side message forwarding yet. Focus-first routing is the least risky fix.

### Step 6: Add selective keyboard compatibility only if the traces justify it

Probe `effKeysRequired` during `vst::Vst2Plugin::OpenEditor()` or immediately after open from `graphics::VstEditorWindow::Create()`.

If the plugin requests key handling and the manual trace shows the child still misses keyboard input, add a narrow Windows-only compatibility path for key messages. Keep it limited to keyboard messages such as:

- `WM_KEYDOWN`
- `WM_KEYUP`
- `WM_SYSKEYDOWN`
- `WM_SYSKEYUP`
- `WM_CHAR`

Do not add broad mouse translation or generic input mirroring unless instrumentation proves it is necessary.

### Step 7: Expand host callback coverage where real plugins expect it

Review `vst::Vst2Plugin::HostCallback()` and split the work into three buckets.

Bucket A, preserve and verify existing behavior:

- `audioMasterVersion`
- `audioMasterGetSampleRate`
- `audioMasterGetBlockSize`
- `audioMasterGetTime`
- `audioMasterGetCurrentProcessLevel`
- host identity strings and vendor version
- current `audioMasterCanDo` responses

Bucket B, add editor-facing behavior now:

- `audioMasterSizeWindow`
- `audioMasterIdle`
- `audioMasterUpdateDisplay`
- `audioMasterAutomate`
- `audioMasterBeginEdit`
- `audioMasterEndEdit`

Bucket C, trace first and then decide:

- `audioMasterProcessEvents`
- `audioMasterWantMidi`
- `audioMasterIOChanged`

For `audioMasterCanDo`, keep the answers strict and truthful in `vst::Vst2Plugin::SupportsHostCanDo()`. The point is not to imitate JUCE's entire matrix. The point is to stop lying by omission on common capabilities that Jamma really does support.

When adding `audioMasterBeginEdit` and `audioMasterEndEdit`, keep them consistent with the existing automation path that already starts at `audioMasterAutomate` and flows through `vst::PublishLastTouchedParameter()` into `midi::MidiRouter::_ConsumeEditorAutomation()`.

### Step 8: Keep the implementation message-thread-safe and explicit

Use the same thread ownership model throughout this work:

- editor open and close on the UI thread,
- editor idle dispatch on the UI thread,
- host window resize and focus routing on the UI thread,
- no allocations or blocking behavior in the audio callback.

Do not let this turn into a cross-thread state machine. The JUCE hosts are useful references, but Jamma should keep the implementation smaller and more explicit.

### Step 9: Add narrow regression coverage before removing instrumentation

Extend native tests around the host callback and editor-host seam.

At minimum add tests for:

- `audioMasterSizeWindow` success and failure behavior
- `audioMasterCanDo` strings for the capabilities Jamma claims
- automation callback bookkeeping for `audioMasterAutomate`, `audioMasterBeginEdit`, and `audioMasterEndEdit`
- any pure helper extracted for host-window sizing math
- any pure helper extracted for fast-idle scheduling or focus bookkeeping

If needed, add a `VstEditorWindow`-focused test seam with a fake `IVstPlugin` so the editor host path can be exercised without a real plugin DLL.

The current baseline test file is `test/JammaLib_Tests/src/vst/Vst2Plugin_Tests.cpp`. It already covers unloaded safety and `SupportsHostCanDo()`, but it does not cover the editor host path or callback expansion yet.

### Step 10: Run manual validation against real plugins

Validate at least these scenarios after the code change:

- `C:/Users/matto/VST/64bit/Battery 4.dll` repaints while idle
- `C:/Users/matto/VST/64bit/Battery 4.dll` responds during mouse interaction
- `C:/Users/matto/VST/64bit/Battery 4.dll` survives focus changes and alt-tab
- `C:/Users/matto/VST/64bit/Battery 4.dll` closes and reopens cleanly
- `C:/Users/matto/VST/64bit/Serum_x64.dll` remains healthy

If a VST2 effect plugin is available locally, run the same editor checks on one effect as well, because the target here is broad VST2 compatibility, not just one instrument.

Only remove the temporary tracing once the traces show that Battery's repaint path is getting the host behavior it expects.

## Relevant Files

- `JammaLib/src/graphics/VstEditorWindow.cpp`
- `JammaLib/src/graphics/VstEditorWindow.h`
- `JammaLib/src/vst/Vst2Plugin.cpp`
- `JammaLib/src/vst/Vst2Plugin.h`
- `JammaLib/src/vst/Vst.cpp`
- `JammaLib/src/vst/VstEditorWindowManager.cpp`
- `JammaLib/src/midi/MidiRouter.cpp`
- `JammaLib/src/vst/Vst3Plugin.cpp`
- `test/JammaLib_Tests/src/vst/Vst2Plugin_Tests.cpp`

Reference host code worth keeping open during implementation:

- Steinberg minihost: `minihost.cpp`, `minieditor.cpp`
- `MiniVST`: `MiniVST/vst_plugin.cpp`
- JUCE VST2 host layer: `juce_VSTPluginFormat.cpp`
- `Element`: `src/pluginmanager.cpp`, `src/pluginprocessor.cpp`, `src/plugineditor.cpp`
- `DirectPipe`: `host/Source/Audio/VSTChain.cpp`, `PluginLoadHelper.h`, `PluginPreloadCache.cpp`
- `Pedalboard2`: `src/PluginComponent.cpp`, `src/PluginField.cpp`

## Verification

1. Build `JammaLib` and `JammaLib_Tests` incrementally.
2. Run focused VST/editor native tests after adding the seams.
3. Manually validate Battery and Serum editor behavior.
4. Confirm traces explain any remaining repaint failure before removing instrumentation.

## Scope

### Included

- VST2 Win32 editor host robustness.
- Idle, focus, resize, and callback compatibility.
- Narrow regression coverage for the editor-host slice.

### Excluded

- Battery-specific hacks.
- Broad VST3 changes.
- JUCE-style crash scanner processes or plugin sandboxing.
- Unrelated app-window or input rewrites.