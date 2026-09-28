# Loop grid editor plan

## Scope and interaction

- Enter an editor for one selected audio `Loop` or one selected `midi::MidiLoop`. Keep its identity stable while editing; close the editor if that loop is removed. Show one loop only, even when its `LoopTake` contains several loops.
- Animate the selected loop from its ring position to a centred rectangular grid and move the camera to a top-down pose. Move other loops, takes, and stations away and fade them during the transition. Reverse the same animation on exit. Disable scene picking for displaced objects while the editor is active.
- Put time on X from 0 to the loop length. For MIDI, put pitch on Y with one row per semitone and draw note spans with velocity shading. For audio, draw the min/max waveform envelope as height over the time axis; audio is view only in this first version.
- Show a playhead, loop boundary, pitch labels for MIDI, and grid lines using the loop's current quantisation settings. Keep the visual grid and editing snap based on the same boundary calculation.

## Geometry and rendering

1. Define a shared canonical surface in local coordinates: `u` is loop time in `[0,1]`, `v` is pitch row or waveform amplitude, and `h` is visual height. Use fixed vertex and instance data across both views.
2. Add a render-only morph value in `[0,1]` to the waveform and MIDI note vertex shaders. At 0, project the canonical surface into the existing circular position; at 1, project into the rectangular grid. Interpolate positions in the shader, using a single scene animation clock and the same morph value for every part of the selected loop. Preserve the existing ring appearance at morph 0.
3. Resolve the seam explicitly: maintain separate vertices for `u=0` and `u=1`, identical ring positions at morph 0, opposite grid edges at morph 1. Tessellate long note spans enough that their intermediate vertices follow the ring during the morph. Keep pitch, envelope, and time mapping independent of camera pose so the image does not jump as the camera moves.
4. Feed the selected model's morph, placement, opacity, and picking state through the existing draw path. Do not rebuild waveform samples or MIDI note instances per animation frame; update uniforms and transforms only. Preserve the current waveform texture update path.
5. Add a dedicated camera editor target and transition in `graphics::Camera`; restore the prior view and selection depth on exit. Verify endpoints and intermediate geometry across aspect ratios and window resize.

## MIDI editing

1. Introduce a loop-specific edit controller on the UI/job side. Convert pointer coordinates to canonical `(time, pitch)` only after the camera has reached the stable top-down editor pose; use an unprojected grid plane and explicit bounds checks. Suppress background pan, selection paint, and quantisation overlay gestures while an editor drag owns the pointer.
2. Add a non-real-time edit operation to `midi::MidiLoop` that replaces a targeted note span by stable event identity or by a snapshot revision plus span key. Construct and validate the complete new event list off the audio callback, then publish it through the existing immutable playback-buffer mechanism. Reject stale edits and capacity overflow atomically. Refresh the model from the accepted event snapshot and create one undo record per gesture.
3. Quantisation active: a click toggles the occupied pitch/time slot. A drag paints a consistent add or remove action across slots crossed, visiting each slot once. Derive slot boundaries from the selected take's resolved MIDI quantisation; never use an unrelated scene grid. New notes occupy one slot by default.
4. Quantisation inactive: clicking empty space creates a note at that time and pitch; dragging an existing note near its left edge adjusts onset, near its right edge adjusts end, and from its centre moves the note in time and pitch. Hit regions use a minimum screen pixel width. Preserve a minimum positive duration, clamp pitch to 0–127, and handle the loop seam using the existing wrap-span convention.
5. During drag, show a preview without mutating playback on every pointer move. Commit on release, cancel on Escape or loss of capture. Make undo/redo and save/export see the accepted events.

## Integration order

1. Add pure geometry and pointer-to-grid mapping helpers with tests for endpoints, seam, pitch rows, quantised cells, and resize.
2. Add the selected-loop editor state and enter/exit control in `engine::Scene`, with camera and other-object transitions. Gate scene picking and mouse routing.
3. Implement shader morph and grid rendering in `graphics::LoopModel`, `graphics::MidiModel`, and `Jamma/resources/shaders`. Verify audio and MIDI rings at morph 0 and rectangular views at morph 1.
4. Add non-real-time MIDI event edits, publication, preview, and gesture logic. Keep audio view only.
5. Add native tests for note creation, removal, resize, move, quantised paint, wrap, capacity, stale revision, undo, and playback snapshot integrity. Run the relevant `JammaLib_Tests` target and an incremental affected-project build using the copied `.vscode/tasks.json` commands.

## Invariants and acceptance

- Only one loop is editable. All other objects become visually subordinate and cannot intercept editor input.
- Morph 0 matches current ring visuals; morph 1 is a stable top-down Cartesian grid. No per-frame CPU remeshing, seam jump, or floating-point drift from repeated transforms.
- Audio callback remains allocation-free, exception-free, and lock-free. It sees either the complete old MIDI event snapshot or the complete new one.
- Grid edits use the selected MIDI loop's time base and resolved quantisation, including nonzero phase offset and note spans crossing the seam.
- Exiting the editor restores the previous scene view, visibility, picking, and pointer behavior.

## Relevant existing paths

- Scene routing and camera: `JammaLib/src/engine/Scene.cpp`, `JammaLib/src/graphics/Camera.cpp`.
- Audio model and waveform shader: `JammaLib/src/graphics/LoopModel.cpp`, `Jamma/resources/shaders/waveform.vert`.
- MIDI model and note shader: `JammaLib/src/graphics/MidiModel.cpp`, `Jamma/resources/shaders/midi_note.vert`.
- MIDI event owner and take quantisation: `JammaLib/src/midi/MidiLoop.cpp`, `JammaLib/src/engine/LoopTake.cpp`.
- Core ownership and real-time rules: `doc/glossary.md`, `doc/realtime-audio.md`.
