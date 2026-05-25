# Feature: midi-triggers

## Summary
Enable a MIDI device to be used to trigger loop recording and ditching. The same or a different MIDI device may be used for triggering and for performance when recording MIDI loops. The MIDI trigger device and mappings will be specified in the rig file. Minimal logging only. This feature must not modify or replace the existing MIDI loop recording code.

## Goals & Acceptance Criteria
- Rig-file key for selecting a MIDI trigger device and mapping exists (e.g., "trigger.type = midi", device identifier, channel, note/CC).
- Pressing the configured MIDI Note or CC triggers loop recording and ditch actions, matching the behavior and debounce semantics of existing serial triggers.
- Existing MIDI loop recording behavior is unchanged and unaffected by this feature.
- Automated unit/integration tests (modeled on serial-trigger tests) pass on Windows.
- Minimal log messages emitted for device connect/disconnect and trigger events.
- No UI changes required for device selection; configuration via rig file only.

## Scope

### In scope
- Rig-file schema addition for specifying MIDI trigger device and mappings.
- MidiInputHandler using RtMidi for device I/O.
- Mapping support for Note-On and CC messages (configurable channel and note/CC number). No velocity sensitivity.
- Reuse/abstract the existing trigger activation path (avoid duplicating serial-trigger logic).
- Debounce implementation consistent with existing trigger types.
- Minimal logging for device lifecycle and triggers.
- Unit and integration tests mirroring serial-trigger tests.

### Out of scope
- Modifying or implementing MIDI loop recording flow (must remain separate).
- Full MIDI device browser or UI for device selection.
- Advanced mapping formats, presets, or velocity-sensitive triggers.
- VST MIDI routing or in-plugin MIDI handling.

## Proposed Approach
- Add a MidiInputHandler component (RtMidi) that enumerates devices and opens the configured device from the rig file.
- Emit normalized TriggerEvent objects (same shape as existing trigger sources) to the existing trigger dispatcher so downstream logic is unchanged.
- Extend rig-file schema with a "trigger" section allowing:
  - type: "midi"
  - device: <device-id-or-name>
  - channel: <1-16> (optional)
  - event: { kind: "note" | "cc", id: <note-or-cc-number> }
- Support both Note-On and CC messages as trigger sources. Ignore velocity for now.
- Reuse debounce and logging facilities used by serial triggers.
- Add tests by adapting serial-trigger tests to drive a virtual RtMidi input (or mock the MidiInputHandler).

## Risks & Open Questions
- RtMidi packaging and vcpkg availability on CI/Windows.
- MIDI device access conflicts when other apps hold the device.
- Thread-safety and low-latency handling; ensure the input thread dispatches into existing trigger-safe paths without allocations on audio threads.
- Rig-file schema migration/backwards compatibility concerns.
- Debounce tuning may require iteration.
- Test harness: need a reliable virtual MIDI device or a mock for CI.
- Risk of code bloat or poor architecture if serial-trigger logic is duplicated; prefer surgical abstractions and reuse.

## TODOs
- [ ] Implement MidiInputHandler (RtMidi wrapper).
- [ ] Add rig-file parsing for MIDI trigger config.
- [ ] Wire trigger events into existing trigger dispatcher.
- [ ] Add unit/integration tests mirroring serial-trigger tests.
- [ ] Update documentation (rig file schema).
- [ ] Verify RtMidi builds on CI and package via vcpkg if necessary.
