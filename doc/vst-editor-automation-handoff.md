# VST Editor Automation Overwrite Notes

## Status

This note describes the code as it exists after reverting the experimental changes that were tried in this session.

Those reverted changes are intentionally not part of this handoff because manual testing suggested they were not materially better and the user wants the next pass to start from the original implementation.

## User-reported bug

When a VST editor parameter is dragged with the mouse while automation record is active, the expectation is that recent editor motion should overwrite existing automation in the affected lane.

Observed problem:

- Old automation points can remain interleaved with the newly recorded editor-driven points.
- The issue is intermittent from the user perspective.
- The stale points are visible in the 3D automation visuals.
- The stale automation is also audible after recording finishes.

User-stated desired behavior:

- If it has been less than 800 ms since the last parameter change from the VST editor, wipe any automation that already exists in the affected lane between the current position and the time when automation record mode was previously disabled.
- This should happen while playback is running.
- The overwritten result should persist after automation recording finishes.

## Relevant code paths

### Editor-touch ingestion

`JammaLib/src/midi/MidiRouter.cpp`

- `MidiRouter::_ConsumeEditorAutomation(...)`
- Reads `vst::_lastTouchedParam.Sequence`, `Plugin`, `ParameterIndex`, and `Value`.
- Only records editor-driven automation while `_automationRecordHeld` is true.
- Resolves the target loop with `Station::ResolveEditorAutomationLoop(...)`.
- Resolves or claims a lane with `MidiLoop::ResolveAutomationLaneFor(...)`.
- Reuses one `EditorOverwriteSession` per `(plugin, param)` when possible.
- Stores captured points in the session via `_RecordOverwritePoint(...)`.
- Refreshes per-parameter playback suppression with an 800 ms sample-domain deadline.

### Session application

`JammaLib/src/midi/MidiRouter.cpp`

- `MidiRouter::_ApplyEditorOverwriteSessions(std::uint32_t nowSample)`
- For each active session:
  - expires it when `ExpirySample - nowSample <= 0`
  - drops it if the loop disappeared
  - drops it if lane index is invalid
  - drops it if the lane mapping no longer matches `(plugin, param)`
  - otherwise clears the entire lane point buffer with `ClearAutomationLanePoints(...)`
  - then replays only the points currently stored in the session buffer

This means the original implementation does not model an overwrite window. It only has:

- a lane reference
- a `(plugin, param)` identity
- a fixed set of captured points
- an expiry sample

It does not retain:

- the lane contents from before the editor drag began
- a start sample for the overwrite range
- a wrapped range model for loop boundaries
- an explicit held value extending from the last drag sample to the current playhead

### Lane storage semantics

`JammaLib/src/midi/MidiLoop.cpp`

- `MidiLoop::SetAutomationValueAtFrac(...)`
- Points are kept sorted by fractional position.
- Nearby points within `1 / 2048` of each other are merged in place.
- Capacity is fixed at `AutomationLane::MaxPoints = 256`.
- If full, new points are silently dropped.

- `MidiLoop::ClearAutomationLanePoints(...)`
- Clears all points in the lane but preserves the lane mapping.

- `MidiLoop::GetAutomationValueAtCursor(...)`
- Playback interpolates between adjacent points.
- Before the first point it holds the first value.
- After the last point it holds the last value.

### Playback suppression and dispatch

`JammaLib/src/engine/Station.cpp`

- `Station::RebuildAutomationDispatch()` builds a flat list of active `(plugin, param, loop, lane)` automation routes.
- `Station::_RunAutomationDispatch(std::uint32_t blockStartSample)` skips only those parameters currently suppressed by `MidiRouter::IsParameterSuppressed(...)`.
- Suppression is per `(plugin, param)` and is sample-domain only.

## Current findings

### 1. The original overwrite logic is whole-lane replay, not range overwrite

The active session code clears the whole lane every pump and reconstructs it from `session.Points` only.

That is a very strong behavior, but it still does not encode the user's requested semantics. In particular, it has no notion of:

- where the overwrite started in loop/sample time
- which part of the original lane should survive
- how to behave when the overwrite interval wraps around the loop boundary

So even before diagnosing the intermittent part, the current implementation is not actually representing the desired overwrite rule.

### 2. The EditorOverwriteSessions is complex and not destructive

In general, whilst editor automation is flowing in in automation recording mode,  we want to simply delete any existing points that fall between now and when we started recording (or at least since the automation recording last began).  The whole EditorOverwriteSessions is unecessarily complex, and seems to be attempting non-destructive when there is no need to be.  Easier to just remove points based on time filter, and keep updating.
If we really shouldn't remove such points in the thread responsible for updating the lane control points, then we can store refs/ids/ranges of those to be deleted, and remove them soon after on appropriate thread.

### 3. Session expiry happens before replay on the expiry sample

`_ApplyEditorOverwriteSessions(...)` immediately deactivates a session when:

- `static_cast<std::int32_t>(session.ExpirySample - nowSample) <= 0`

That means the session does not perform one last replay on the exact expiry sample. Final persisted state depends on the last pump that happened strictly before expiry.

Whether this matters in practice depends on pump cadence versus editor-touch cadence, but it is one place where persistence can become timing-sensitive.

### 4. Editor-touch capture only keeps the latest touch observed per pump

`_ConsumeEditorAutomation(...)` consumes `vst::_lastTouchedParam` by comparing one global sequence number against `_lastEditorAutomationSeq`.

This means:

- multiple editor changes between two `PumpMidi(...)` calls are collapsed to the latest one seen at pump time
- there is no queue of editor-origin changes

That is accepted for now - changes to this are considered out of scope, but it is a lossy sampler, not a complete event stream.

### 5. Session point capacity is small and overflow is silent

The session capture buffer size is `MaxEditorOverwritePoints = 256`.

Once full, `_RecordOverwritePoint(...)` stops adding points.

The underlying lane storage also has a 256-point cap.

This may or may not be relevant to the specific bug, but long or dense editor drags can be truncated without any diagnostic.  It should be increased to 2048.

### 5. Point merging can hide rapid local motion

Both `_RecordOverwritePoint(...)` and `MidiLoop::SetAutomationValueAtFrac(...)` merge nearby fractional positions using the same epsilon of `1 / 2048`.

That means two quick edits landing near the same normalized position replace each other instead of coexisting.

This could make some drags appear to overwrite cleanly while others look uneven, depending on loop length and how densely `PumpMidi(...)` samples the drag.

### 6. Existing tests cover suppression, not overwrite-session semantics

There are tests for:

- suppression table behavior in `test/JammaLib_Tests/src/midi/VstEditorAutomationSuppression_Tests.cpp`
- lane mapping behavior in `test/JammaLib_Tests/src/midi/MidiAutomationLaneResolution_Tests.cpp`
- playback suppression behavior in `test/JammaLib_Tests/src/engine/StationMidiInstrument_Tests.cpp`

There is currently no unit test in the reverted codebase that directly exercises:

- `EditorOverwriteSession`
- `_ConsumeEditorAutomation(...)`
- `_ApplyEditorOverwriteSessions(...)`
- loop-wrap behavior during editor overwrite
- persistence of the final overwritten lane after expiry

## Notes on how the original code works

### What the original implementation is good at

- It keeps the audio-thread suppression path bounded and sample-domain based.
- It avoids heap allocation in the hot paths it touches.
- It preserves lane mapping while clearing only points.
- It uses the existing lane interpolation path for playback instead of adding a separate editor-automation playback mechanism.

### What is non-ideal in the original implementation

- The overwrite session is defined as a bag of points plus an expiry, not as a time/range operation.
- Session replay is destructive to the whole lane on every pump.
- Final persisted state is tied to pump timing near expiry.
- Editor input sampling is lossy because only the latest touch since the last pump is observed.
- Silent fixed-capacity truncation exists both in the session buffer and the lane buffer.
- There is no direct test coverage around the exact feature that is reported broken.

## Files worth reading first

- `JammaLib/src/midi/MidiRouter.cpp`
- `JammaLib/src/midi/MidiRouter.h`
- `JammaLib/src/midi/MidiLoop.cpp`
- `JammaLib/src/engine/Station.cpp`
- `test/JammaLib_Tests/src/midi/VstEditorAutomationSuppression_Tests.cpp`
- `test/JammaLib_Tests/src/midi/MidiAutomationLaneResolution_Tests.cpp`
- `test/JammaLib_Tests/src/engine/StationMidiInstrument_Tests.cpp`

## Validation history from this session

- A more invasive experimental change was attempted and then reverted.
- Manual testing reportedly showed behavior that seemed unchanged.
- The repository has been left on the original implementation, with this note added only.