# Proposal: one MIDI loop per input device

**Status:** design note only. The selection and hover commit `db7f610` does not change MIDI loop ownership, playback routing, or the `.jam` format.

## Current model

A `LoopTake` creates one `midi::MidiLoop` for each configured **input device × MIDI channel** pair. A station listening to channels 1, 2, and 3 on one device therefore records three loops. The MIDI channel is also stored in each `MidiEvent::status` byte; the input device is stored beside each loop in `LoopTake::_midiLoopDevices`. Each loop occupies an output index used by station VST routing, and each is saved as a separate MIDI stream and native sidecar.

The current selection UI draws one shared MIDI ring for the take and treats its channel models as one selection target. This is a presentation rule; the underlying loops and their routes remain separate.

## Proposed identity

Within a take, create **one `MidiLoop` per input device**. Put events from every enabled channel for that device into that loop, retaining each event's original status/channel byte. Two input devices would still produce two loops, even when they listen to the same channels. The device, rather than the channel, distinguishes loops.

Each device loop would have one VST output route shared by all its channels, as agreed for this proposal. The loop grid editor would need a chosen default channel for new notes; existing notes would retain their own channels. If device loops need separate visual positions, a small radius difference can distinguish them without changing their identity or creating channel-specific picker rings. Each actual device loop should have one selectable ring; its notes should select that same loop.

## Changes required before implementation

- **Recording:** replace the per-channel loop list with per-device loops and an allowed-channel mask or set. Ingress must test both device and channel before appending to the one matching loop. Seeded held notes and end-of-record NoteOffs must cover every enabled channel.
- **Playback and VST routing:** output indices would become per-device rather than per-channel. `Station::ReadMidiBlock` and `LoopTake::ReadMidiBlock` must preserve event channel bytes while using the device loop's shared VST route. Audit held-note flushes and muted/ditched loops for stuck notes.
- **Overdub and punch:** match source and target by device, copy all source channels, and retain their channels when building merged events and live transition notes. Define the behavior when an overdub enables a different channel set.
- **Editor and selection:** choose an explicit default channel for note creation. Ring and note hits must resolve to the same device loop at loop depth; take and station depth continue to resolve their owners.
- **Capacity:** today each channel loop has 4,096 event slots and eight automation lanes. One device loop would share those limits across its channels unless they are raised. This decision is still open; neither silent truncation nor callback allocation is acceptable.
- **Publication and tests:** keep callback reads on the existing immutable MIDI playback snapshot path. Add focused tests for three-channel capture, playback, held-note closure, overdub/punch, VST routing, editor defaults, save/reload, capacity, and multiple devices.

## Existing `.jam` files

Do **not** automatically merge old per-channel streams on load. They can continue to use their saved layout and output indices. This avoids changing established VST routes or losing events and automation when combined limits are exceeded.

A later manual in-place conversion should be done with a purpose-built offline utility using `JamFile` and `NativeMidiSidecar` readers/writers, not a text edit of binary sidecars:

1. Back up the `.jam` manifest and every referenced MIDI sidecar.
2. For each take, group streams by input device. Confirm compatible loop length, cursor, timing origin, quantisation, and automation origin before merging.
3. Merge raw events from those streams in timestamp order, preserving their status/channel bytes and the required same-sample ordering. Reject a group that exceeds the chosen device-loop event or automation capacity.
4. Merge automation lanes without changing their plugin bindings. Rewrite the manifest's `midiStreams` entries and sidecar paths to one stream per device, using a declared default channel for editor note creation.
5. Remap station MIDI VST output indices and automation binding stream indices for that take **and every later take in the station**, because output indices are flattened across takes.
6. Validate the rewritten manifest and sidecars through the normal loaders, compare event/automation counts and VST routes, then save atomically only after all checks pass.

The native sidecar format and station route references make a hand-written binary edit unsafe. The conversion utility and its validation tests belong to a separate, explicitly approved migration task.

## Relevant code

- Creation, capture, playback, overdub, export, and restore: `JammaLib/src/engine/LoopTake.cpp`.
- Loop events, capacity, held notes, and callback snapshot: `JammaLib/src/midi/MidiLoop.h` and `MidiLoop.cpp`.
- Per-output VST routes: `JammaLib/src/engine/Station.cpp` and `JammaLib/src/midi/MidiVstOutputSink.cpp`.
- Manifest and native sidecars: `JammaLib/src/io/JamFile.cpp`, `IoSessionExporter.cpp`, and `NativeMidiSidecar.cpp`.
- MIDI editor default channel and scene picking: `JammaLib/src/engine/Scene.cpp`.
