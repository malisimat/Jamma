# VST3 Feature and Reliability Parity Plan

## Goal

Bring `Vst3Plugin` to the same practical reliability bar as `Vst2Plugin` for:

1. project state save/restore;
2. construction, editor, and destruction lifecycle safety;
3. live MIDI controller delivery through VST3 parameter mappings; and
4. editor automation reaching both the processor and Jamma's recorder.

Implement the steps in order. Each step should build independently. Keep all
audio-thread paths allocation-free, exception-free, and lock-free.

## Sources checked

The plan was verified against the current Jamma implementation and the local
VST3 SDK 3.8.x checkout at `C:\Users\matto\Source\Repos\vst3sdk`.

Authoritative SDK references:

- `pluginterfaces/base/ibstream.h`: `IBStream` contract and seek constants.
- `pluginterfaces/vst/ivstcomponent.h`: component `getState`/`setState`.
- `pluginterfaces/vst/ivsteditcontroller.h`: controller
  `setComponentState`, `getState`, `setState`, `IMidiMapping`, and the
  component-handler contract.
- `pluginterfaces/vst/ivstmidicontrollers.h`: controller numbers, including
  `kAfterTouch == 128`, `kPitchBend == 129`, and `kCountCtrlNumber == 130`.
- `public.sdk/source/vst/vstpresetfile.cpp`: canonical component/controller
  state save and restore ordering.
- `public.sdk/samples/vst/again/source/againcontroller.*`: small controller
  state and MIDI-mapping example.

Do not copy the SDK preset-file container. JAM already stores an opaque Base64
blob and only needs a compact, self-describing Jamma container.

## Step 1 - Add VST3 project-state persistence

### Files

- `JammaLib/src/vst/Vst3Plugin.h`
- `JammaLib/src/vst/Vst3Plugin.cpp`
- `JammaLib/src/io/JamFile.h`

### Public overrides

Add these near `IsLoaded()` and `Name()` in `Vst3Plugin.h`:

```cpp
std::vector<std::uint8_t> GetState() const override;
void SetState(const std::vector<std::uint8_t>& blob) override;
```

Both methods are non-RT. Keep the existing `IVstPlugin` call sites unchanged:
`Loop`, `LoopTake`, and `Station` already save and restore state generically.

### Memory stream

Add a private translation-unit helper implementing `Steinberg::IBStream` over
owned or borrowed byte storage. It must:

- implement `read`, `write`, `seek`, and `tell`;
- match the SDK's exact `write(void* buffer, ...)` signature; its logically
  read-only input is non-const in `IBStream`;
- support `kIBSeekSet`, `kIBSeekCur`, and `kIBSeekEnd`;
- reject negative positions and checked-add overflow;
- report short reads/writes through `numBytesRead`/`numBytesWritten`;
- return `kInvalidArgument` for invalid pointers/counts and `kResultOk` only
  when the requested operation is valid; and
- implement `FUnknown` using the same `FUNKNOWN_CTOR`, `DECLARE_FUNKNOWN_METHODS`,
  and `IMPLEMENT_FUNKNOWN_METHODS` pattern as the existing fixed VST3 helpers.

Using the SDK's `MemoryStream` would add another SDK implementation dependency
to `JammaLib`; a small local adapter is preferable here.

### Blob format

Continue the VST2 outer framing so `JamFile` remains plugin-type agnostic:

```text
offset  size  field
0       1     format version = 1
1       1     state type = 2 (VST3 component/controller state)
2       4     little-endian payload byte count
6       4     little-endian component-state byte count
10      4     little-endian controller-state byte count
14      N     component-state bytes
14+N    M     controller-state bytes
```

The outer payload size is `8 + N + M`. Use explicit little-endian byte helpers;
do not `reinterpret_cast` packed structs. Before allocating or slicing, check
all additions against `size_t` overflow and require the declared total to equal
the blob size exactly. Also reject either sub-blob above `INT32_MAX`, because
`IBStream` byte counts are signed `int32`. Reject unknown versions/types,
truncated headers, oversized lengths, and trailing bytes without touching the
plugin.

### Save algorithm

`GetState()` should:

1. return empty unless the plugin is loaded and has a component;
2. call `IComponent::getState` into a component stream;
3. if a controller exists, call `IEditController::getState` into a separate
   controller stream; treat `kNotImplemented`/failure as no controller blob;
4. return empty if component `getState` fails; and
5. frame both byte vectors using the format above.

Live state capture still needs an owner-level quiescence protocol: current save
callers can invoke `GetState()` while audio processing is active. Do not add a
mutex in the audio path; establish this owner contract before relying on live
saves.

### Restore algorithm and lifecycle

The SDK preset implementation establishes this order:

1. processor/component receives component state;
2. controller receives the same component state via `setComponentState`;
3. controller receives optional controller-only state via `setState`.

Jamma's project-load paths call `SetState` on the newly loaded plugin before the
plugin is published into its live chain. Preserve and document that ownership
invariant: no process callback may reach the object during `SetState`. Do not
try to manufacture quiescence by only clearing `_isActivated`; that does not
wait for a callback already in flight. Implement this sequence:

1. fully validate and split the blob before changing activation state;
2. pass a fresh component stream at position zero to `component->setState`;
3. if a controller exists, rewind/create a second view over the component bytes
   and call `controller->setComponentState`;
4. if controller bytes are non-empty, pass a fresh stream to
   `controller->setState`;
5. rebuild host parameter maps and MIDI mappings because restored programs may
  change parameter or controller assignments.

The SDK permits `IComponent::setState` in initialized, connected, setup,
activated, or processing lifecycle states, but that permission does not make a
concurrent host call safe. Keep quiescence as a caller precondition instead of
adding an audible deactivate/reactivate cycle.

Log restore failures with the failing SDK operation. Do not throw. A controller
state failure should not undo successfully restored processor state.

Update `JamFile::VstEntry::State` to describe a Base64-encoded plugin state blob
from `IVstPlugin::GetState`, with VST2/VST3 self-describing formats. No JSON or
Base64 code changes are needed.

### Focused validation

- Build `JammaLib` Debug x64.
- Add pure helper tests for valid framing, empty controller state, truncation,
  unknown type/version, inconsistent sizes, and trailing data. If helpers stay
  private to `Vst3Plugin.cpp`, extract only the framing/parser into a small
  `Vst3StateBlob.{h,cpp}` pair and add both files to `.vcxproj` and `.filters`.
- Manual: change a VST3 preset, save JAM, reload, and verify processor and UI.

---

## Step 2 - Share GL-context restoration across VST2 and VST3

### Files

- new `JammaLib/src/vst/VstGlContextScope.h`
- `JammaLib/src/vst/Vst2Plugin.h`
- `JammaLib/src/vst/Vst2Plugin.cpp`
- `JammaLib/src/vst/Vst3Plugin.cpp`
- `JammaLib/JammaLib.vcxproj`
- `JammaLib/JammaLib.vcxproj.filters`

Move the existing VST2 nested scope to `vst::VstGlContextScope` in the shared
header. Preserve its behavior exactly: capture `wglGetCurrentContext()` and
`wglGetCurrentDC()` in the constructor; in the destructor call
`wglMakeCurrent(savedDc, savedRc)` only when a context was captured. Remove the
old nested declaration/definitions and use the shared type in VST2.

In VST3, place a scope around every plugin call that may create or switch a UI
graphics context:

- module/factory/component setup in `PreInit`: `InitDll`, `GetPluginFactory`,
  `createInstance`, and `IComponent::initialize` (one outer scope is fine);
- the fallback component/controller `createInstance` and `initialize` path in
  `Load`, because `Load` can run without `PreInit`;
- `IEditController::createView` and `IPlugView::attached` in `OpenEditor`; and
- `IPlugView::removed` in `CloseEditor`.

Keep `setFrame(nullptr)` before `removed()`. If `attached()` fails, call
`setFrame(nullptr)` before releasing `plugView`; do not leave the frame attached
to a failed view.

### Focused validation

- Build `JammaLib` Debug x64 immediately after the shared-scope refactor.
- Open/close a GL-using VST2 plugin to prove behavior did not regress.
- Open/close a GL-using VST3 plugin repeatedly, then verify Jamma rendering and
  GL picking still work.

---

## Step 3 - Implement complete VST3 MIDI controller mapping

### Files

- `JammaLib/src/vst/Vst3Plugin.cpp`
- `JammaLib/src/vst/Vst3MidiMapping.{h,cpp}`

`IMidiMapping` is declared by the already included `ivsteditcontroller.h`;
`ivstmidilearn.h` contains the separate `IMidiLearn` interface and is not needed.
Query `IMidiMapping` from the controller after it is created and initialized.
VST3 SDK 3.8 also contains MIDI 2.0 interfaces, but Jamma's ingress is MIDI 1.0
bytes; implement `IMidiMapping` first and leave MIDI 2.0 out of this parity
change.

### Fixed mapping cache

Precompute mappings off the audio thread. Do not use an `unordered_map` in the
MIDI hot path. Add a fixed table for input event bus 0:

```cpp
static constexpr std::size_t MidiChannelCount = 16;
static constexpr std::size_t MidiControllerCount =
    static_cast<std::size_t>(Steinberg::Vst::kCountCtrlNumber); // 130

struct MidiParameterAssignment
{
    Steinberg::Vst::ParamID ParamId = Steinberg::Vst::kNoParamId;
};

std::array<std::array<MidiParameterAssignment, MidiControllerCount>,
    MidiChannelCount> midiParameterAssignments;
```

Initialize every entry to `kNoParamId`. For each channel 0..15 and controller
0..`kCountCtrlNumber - 1`, call:

```cpp
getMidiControllerAssignment(0, channel, controllerNumber, paramId)
```

This includes all CCs 0..127 plus channel pressure (`kAfterTouch`) and pitch
bend (`kPitchBend`). Program change is 130 and is intentionally outside
`kCountCtrlNumber`; keep it on the legacy event path. Rebuild the table after
controller creation and after state restore. First check that input event bus 0
exists. Treat the table as an immutable RT snapshot once published.

When `restartComponent(kMidiCCAssignmentChanged)` is received, set an atomic
rebuild-request flag only. Override `Vst3Plugin::IdleEditor()` to run a concrete
non-RT `PollPendingControllerChanges()` method from the existing editor timer.
It must run on the UI owner thread; do not call it from save operations. Build a complete
replacement table off-thread and publish it through the repository's existing
fixed snapshot/mailbox pattern; never mutate the table currently read by the
audio thread. If no suitable fixed mailbox exists in the VST subsystem, add a
small two-slot SPSC mailbox whose producer can write only a slot acknowledged
free by the consumer, and let `BeginMidiBlock` adopt a completed snapshot. Do
not use an atomic `shared_ptr`, because releasing the old snapshot can delete on
the audio thread.

### Delivery

Refactor MIDI classification so `Vst3Plugin::SendMidiEvent` can either add a
parameter point or fall back to `FixedEventList`. Reuse the exact block-window
classification currently inside `FixedEventList::AddMidiEvent`; expose a small
helper/result carrying `sampleOffset` so event and parameter paths cannot drift.

For mapped messages, add a point to `inputParameterChanges` using the event's
sample offset within the current block:

- CC: `data2 / 127.0`;
- channel pressure: `data1 / 127.0`;
- pitch bend: combine `data1 | (data2 << 7)` and divide by `16383.0` to obtain
  the VST3 normalized parameter value in `[0, 1]` (center is approximately
  `8192 / 16383`); and
- clamp all values to `[0, 1]`.

If a mapping exists, do not add a legacy event. If no mapping exists, preserve
the existing `kLegacyMIDICCOutEvent` representation. A full fixed parameter
queue replaces its final point with the newest value; do not allocate or send a
duplicate fallback event.

Note-on, note-off, and poly-pressure remain native VST3 events. Program change
remains a legacy MIDI CC output event.

### Focused validation

- Add tests around a pure mapping/value helper: all 16 channels, CC 0/127,
  pressure 0/127, pitch bend 0/8192/16383, mapped-versus-fallback behavior, and
  sample offsets.
- Build and run `JammaLib_Tests` Debug x64.
- Manual: test mod wheel, expression, sustain, channel pressure, and pitch bend
  with a VST3 instrument on at least two MIDI channels.

---

## Step 4 - Complete editor automation forwarding

### Current defect

`HostComponentHandler::performEdit` calls `OnControllerEdit`, which only writes
Jamma's `_lastTouchedParam` registry. For a separate VST3 controller/processor,
the host is also responsible for delivering that normalized value to the
processor through the next process block's `IParameterChanges`.

Writing directly to `FixedParameterChanges` from the UI callback would race the
audio thread. Do not add a mutex and do not make the fixed process container
cross-thread mutable.

### Bounded UI-to-audio queue

Add a fixed-capacity single-producer/single-consumer queue owned by `Impl`:

```cpp
struct PendingControllerEdit
{
    Steinberg::Vst::ParamID ParamId;
    Steinberg::Vst::ParamValue Value;
};
```

The UI thread is the producer (`performEdit`); the audio thread is the consumer
(immediately before each `processor->process`). Reuse
`midi::MidiQueue<256, ControllerEdit>` for this SPSC handoff. It has 255 usable
slots and drops the newest edit on overflow.

Add one `PrepareProcessBlock()` helper and call it before all three process
entry points (`ProcessBlock`, `ProcessBlockStereo`, `ProcessBlockMulti`). It
drains pending controller edits into `inputParameterChanges` at sample offset
0, then calls `processor->process`. Keep the existing post-process
`inputParameterChanges->BeginBlock()` clear.

`OnControllerEdit` should:

1. clamp the normalized value;
2. enqueue `{paramId, value}` for the processor;
3. resolve the host parameter index; and
4. publish the touch for Jamma's learn/record path.

If the parameter is unknown to Jamma's host-index map, still enqueue it for the
processor but skip publication.

### Gesture semantics

Extend `HostComponentHandler` so `beginEdit` and `endEdit` notify the owner, not
just return success. Track a small fixed set of active parameter gestures on the
UI side. Publishing every `performEdit` remains correct for recording values;
gesture tracking gives tests and future recording logic an unambiguous boundary.
Do not infer gestures from wall-clock time.

Do not add a host-write timestamp guard and do not call
`MidiRouter::RefreshAutomationSuppression` from VST code. Suppression is owned
by `MidiRouter`: it starts when a genuine published editor touch is consumed,
and `Station` consults it before automation playback. A processor input change
queued by `SetParameter` should not trigger `performEdit` in a conforming plugin.
If a specific non-conforming plugin later proves otherwise, capture that plugin
and callback sequence in a regression test before adding a targeted guard.

### Restart handling

Replace the no-op `restartComponent` with atomic request flags for at least:

- `kMidiCCAssignmentChanged`: request MIDI-map rebuild;
- `kParamValuesChanged`: request controller/host parameter refresh; and
- `kParamTitlesChanged`: request parameter-map rebuild.

Perform rebuilds on the non-RT owner thread at an existing safe pump point. Do
not call controller enumeration APIs from the audio thread or synchronously
inside `restartComponent`. Use the `IdleEditor`/explicit non-RT poll and fixed
snapshot publication described in Step 3 rather than leaving an unconsumed flag.

### Focused validation

- With a fake owner/handler, assert `performEdit` enqueues one processor change
  and advances `_lastTouchedParam.Sequence` once; `beginEdit`/`endEdit` alone do
  not publish values.
- Build and run `JammaLib_Tests` Debug x64.
- Manual: move a VST3 editor control and confirm immediate audible change,
  automation recording, playback, and no runaway re-recording.

---

## Step 5 - Lifecycle and destruction audit

Re-test the existing `PreInit`/`Load`/`OpenEditor`/`CloseEditor`/`Unload` flow
after Steps 1-4. Preserve these invariants:

- factory, component, controller, views, and connection points are released
  before `FreeLibrary`;
- initialized objects receive `terminate` exactly once;
- `setProcessing(false)` precedes `setActive(false)`;
- connection points disconnect before either endpoint is released;
- `setComponentHandler(nullptr)` happens before controller release;
- `setFrame(nullptr)` and `removed()` happen before view release;
- UI-affine destruction continues through `QueueForUiThreadDestroy`; and
- no process callback can observe freed bus or parameter storage.

The current fallback separate-controller path logs an initialization result but
continues even if initialization fails. Tighten it: on failed controller
`initialize`, call `terminate`, release it, and continue as processor-only (or
fail load if the selected product requires a controller). Track whether the
controller is a separate initialized object so `ResetLoadedObjects` calls its
`terminate()` exactly once. Current `ResetLoadedObjects` never terminates a
controller at all; a separately initialized controller therefore leaks SDK
resources. Also retain the deliberate failed-load distinction: a component
pre-initialized by `PreInit` survives a later `Load` failure, while a component
created by that `Load` must be terminated.

Add lifecycle state booleans rather than guessing from non-null pointers:
`componentInitialized`, `controllerInitialized`, `componentActive`, and
`processorProcessing`. Use them in failure cleanup and normal unload.
Fix the existing activation path at the same time: publish `_isActivated = true`
only after both `setActive(true)` and `setProcessing(true)` succeed. On failure,
reverse whichever transition succeeded and run failed-load cleanup.

### Focused validation

- Build and run the native tests.
- Exercise: `PreInit` failure, component-init failure, controller-init failure,
  editor attach failure, repeated open/close, repeated load/unload, and shutdown
  with an editor open.
- Manual smoke-test at least one single-component and one separate-controller
  VST3 plugin.

---

## Step 6 - Final verification

Use incremental builds; do not clean/rebuild unless stale artifacts force it.

1. Build `JammaLib` Debug x64.
2. Build and run `JammaLib_Tests` Debug x64.
3. Build the full solution Debug x64 after all project-file changes.
4. Run `git diff --check`.
5. Complete the manual matrix below.

| Area | Manual check | Expected result |
| --- | --- | --- |
| State | Save/reload a non-default VST3 preset | Processor sound and editor values restore |
| Editor | Open/close GL-using VST2 and VST3 editors repeatedly | Jamma rendering and picking remain correct |
| MIDI | Send CC, sustain, pressure, and bend on SDK channels 0 and 15 (displayed as MIDI 1 and 16) | Mapped controls respond once, sample timing preserved |
| Automation | Move a VST3 UI parameter while recording | Audio changes and one automation stream is recorded |
| Playback | Play recorded automation without touching UI | No feedback or re-recording |
| Lifecycle | Repeated load/unload and app exit with editor open | No hangs, leaks, callbacks into unloaded DLL, or crashes |

## Suggested commit sequence

1. `Add VST3 state persistence`
2. `Share VST editor GL context guard`
3. `Map VST3 MIDI controllers to parameters`
4. `Forward VST3 editor automation to processor`
5. `Harden VST3 lifecycle cleanup`

Keep tests with the commit that introduces each behavior. This makes every
stage bisectable and gives a fresh implementation session a cheap validation
point after each edit.