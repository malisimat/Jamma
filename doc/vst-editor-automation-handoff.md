# VST Editor Automation Overwrite Notes

## Status

Resolved. The editor-driven automation overwrite is now implemented as a
bounded sample-domain overwrite window plus phase-anchored frac math. This note
documents the current design and replaces the earlier findings that described
the reverted experimental implementation.

For the broader agent orientation see [automation-agent-brief.md](automation-agent-brief.md).

## Behaviour

When a VST editor parameter is dragged while automation record is held
(Ctrl+Shift+A), each real editor change:

- Resolves the owning recording loop and a lane for `(plugin, param)`.
- Overwrites the half-open sample window `[touchSample, touchSample + 800ms)`
  in that lane with the new value, removing existing points inside the window
  and writing a held point at both the window start and end.
- Refreshes that parameter's playback suppression once, with an 800 ms
  sample-domain deadline.

Idle pump cycles do not write points or extend suppression; they only age out
touch state once no new touch has arrived for longer than the cool-down window.

This satisfies the original request: recent editor motion overwrites existing
automation between the touch position and the end of the cool-down window, the
result persists after recording finishes, and wrapped windows split correctly
across the loop boundary.

## Relevant code paths

### Editor-touch ingestion — `JammaLib/src/midi/MidiRouter.cpp`

- `MidiRouter::_ConsumeEditorAutomation(...)`
  - Reads `vst::_lastTouchedParam.Sequence`, `Plugin`, `ParameterIndex`, `Value`.
  - Always advances the sequence cursor so pre-arm touches are not replayed when
    record mode is later armed.
  - Only folds editor drags into automation while `_automationRecordHeld` is true.
  - Part A (fresh touch): resolves loop + lane, computes the loop-relative sample
    from `LoopPhaseAnchor()`, calls `MidiLoop::OverwriteAutomationWindow(...)`,
    and calls `RefreshAutomationSuppression(...)` once.
  - Part B (idle): expires stale `EditorTouchState` slots only.
- `MidiRouter::_ResetEditorTouchStates()` clears all touch state on arm.

### Lane storage — `JammaLib/src/midi/MidiLoop.cpp`

- `MidiLoop::OverwriteAutomationWindow(lane, startSample, durationSamples, value)`
  - Wraps the window against `LoopLengthSamps()`, compacts surviving points in
    place (two-pointer, no temporary buffer / no allocation), then writes the
    held value at both window ends.
- `MidiLoop::SetAutomationValueAtFrac(...)`
  - Keeps points sorted by frac; merges points within a fixed 10 ms window.
  - Capacity is `AutomationLane::MaxPoints = 8192`, which is intentionally above
    the practical merge ceiling. The merge window is the real density limit for
    distinct points.
- `MidiLoop::EndRecord(loopLengthSamps, startGlobalSample)`
  - Stores `LoopPhaseAnchor()` so dispatch and CC-record fracs stay in phase with
    the visual play index.

### Playback dispatch — `JammaLib/src/engine/Station.cpp`

- `Station::RebuildAutomationDispatch()` bakes a flat list of active
  `(plugin, param, loop, lane, loopPhaseAnchor, loopLengthSamps)` routes.
- `Station::_RunAutomationDispatch(blockStartSample, numSamps)` samples the lane
  at the latest sample in the block (`blockStartSample + numSamps - 1`), skips
  suppressed parameters, and calls `SetParameter` at most once per route per
  block (delta-gated by `AutomationEpsilon`).

### VST2 host callback — `JammaLib/src/vst/Vst2Plugin.cpp`

- `audioMasterAutomate` publishes the touched `(plugin, param, value, sequence)`
  tuple only. It deliberately does not echo the value back through
  `setParameter`: per VST2 semantics the plugin already updated its own
  parameter state (via `setParameterAutomated`) before notifying the host.

## Tests

- `test/JammaLib_Tests/src/midi/MidiAutomationLaneResolution_Tests.cpp`
  (`MidiAutomationPhaseAnchor.*`) — phase-anchor storage, frac math, point
  insert/merge, and bounded/wrapped overwrite windows.
- `test/JammaLib_Tests/src/engine/StationMidiInstrument_Tests.cpp`
  (`StationAutomation.*`) — block-end sampling, suppression gating, and the
  fresh-touch-only hold-window write.
- `test/JammaLib_Tests/src/vst/Vst2Plugin_Tests.cpp`
  (`Vst2PluginHostCallback.AudioMasterAutomatePublishesTouchWithoutEchoingParameter`)
  — confirms the host does not echo `setParameter`.

## Known gaps

- VST3 parameter playback is still a host-side no-op.
- Lane storage is fixed-capacity (matched to the merge epsilon, not dynamic).
- `AutomationEpsilon` (value-delta gate, `1 / 65536`) is intentionally retained;
  it sits below 16-bit parameter resolution and does not distort timing.
