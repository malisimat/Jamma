# MIDI Loop Visualization Agent Handoff

Date: 2026-05-22

This handoff captures the current state of the MIDI loop 3D visualization implementation and the next steps for another agent to continue independently. The implementation is intentionally staged: the pure MIDI interpretation, generic rendering support, and `MidiModel` shell are in place and tested; scene ownership and live update integration remain next.

## Current State

Implemented so far:

1. Pure MIDI note-span extraction.
2. Focused tests for note pairing and edge cases.
3. A narrow `MidiLoop` event snapshot/revision API for non-real-time visualization paths.
4. Generic `GuiModel` per-instance attribute buffer support.
5. A compileable `MidiModel` shell that converts note spans into instance attributes.
6. A single MIDI shader resource that handles scene, picker, and highlight modes.
7. MSBuild and filter wiring for the new files.

Not implemented yet:

1. `MidiLoop` ownership or attachment of a `MidiModel` instance.
2. `LoopTake` arrangement/drawing of MIDI rings.
3. Live throttled update scheduling during recording.
4. Final model rebuild at record end.
5. Manual visual verification inside the app.
6. Any persistence, editing, or individual note selection. These remain out of scope.

## Verification Already Run

Commands used:

```powershell
$msbuild = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
& $msbuild Jamma.sln /m /t:JammaLib_Tests /p:Configuration=Debug /p:Platform=x64
& .\test\JammaLib.Tests\bin\x64\Debug\JammaLib.Tests.exe --gtest_filter="MidiNoteSpan.*:MidiLoop.*:MidiEvent.*:MidiQueue.*"
& .\test\JammaLib.Tests\bin\x64\Debug\JammaLib.Tests.exe
```

Results:

- Focused MIDI tests: 29 passed.
- Full native suite after final changes: 304 passed, 1 skipped.
- Skipped test: `MidiDevice.OpensPreferredDeviceWhenAvailable`.
- Build target note: use `JammaLib_Tests` for the solution target. `JammaLib.Tests` fails as an MSBuild target name because the dot is treated as an invalid target character.

## Changed Files

Modified:

- `Jamma/resources/ResourceList.txt`
- `JammaLib/JammaLib.vcxproj`
- `JammaLib/JammaLib.vcxproj.filters`
- `JammaLib/src/engine/MidiLoop.cpp`
- `JammaLib/src/engine/MidiLoop.h`
- `JammaLib/src/gui/GuiModel.cpp`
- `JammaLib/src/gui/GuiModel.h`
- `test/JammaLib.Tests/JammaLib.Tests.vcxproj`
- `test/JammaLib.Tests/JammaLib.Tests.vcxproj.filters`

Added:

- `Jamma/resources/shaders/midi_note.frag`
- `Jamma/resources/shaders/midi_note.vert`
- `JammaLib/src/engine/MidiModel.cpp`
- `JammaLib/src/engine/MidiModel.h`
- `JammaLib/src/engine/MidiNoteSpan.cpp`
- `JammaLib/src/engine/MidiNoteSpan.h`
- `test/JammaLib.Tests/src/engine/MidiNoteSpan_Tests.cpp`
- `doc/midi-loop-visualization-agent-handoff.md`
- `doc/midi-loop-visualization-implementation-summary.html`

## Implemented Details

### Note Span Extraction

Files:

- `JammaLib/src/engine/MidiNoteSpan.h`
- `JammaLib/src/engine/MidiNoteSpan.cpp`
- `test/JammaLib.Tests/src/engine/MidiNoteSpan_Tests.cpp`

`MidiNoteSpan` contains:

- `StartSample`
- `DurationSamples`
- `Channel`
- `Note`
- `Velocity`

`ExtractMidiNoteSpans(const MidiEvent* events, std::size_t eventCount, std::uint32_t loopLengthSamps)` currently:

- Pairs `NoteOn` and `NoteOff` by channel and pitch.
- Treats `NoteOn` with velocity 0 as note-off via `MidiEvent::IsNoteOff()`.
- Ignores events where `sampleOffset >= loopLengthSamps`.
- Ignores note-offs without active note-ons.
- Clamps unmatched note-ons to `loopLengthSamps`.
- Handles overlapping same channel+pitch note-ons by closing the prior span at the new note-on sample, then starting a new span.
- Returns spans in discovery order, then appends any clamped active notes by note-slot order.

Tests cover:

- Basic note pairing.
- Velocity-zero note-on as note-off.
- Clamp at loop end.
- Outside-window filtering.
- Orphan note-off handling.
- Independent channel/pitch matching.
- Overlapping same-note handling.
- Empty input.
- Dense event stability up to `MidiLoop::Capacity()`.

### MidiLoop Snapshot/Revision API

Files:

- `JammaLib/src/engine/MidiLoop.h`
- `JammaLib/src/engine/MidiLoop.cpp`

Added:

- `std::uint64_t Revision() const noexcept`
- `bool TryGetEvent(std::size_t index, MidiEvent& ev) const noexcept`

`_revision` increments on:

- `StartRecord()`
- Successful `RecordEvent()`
- `EndRecord()`
- `Reset()`

Important invariant: `TryGetEvent` copies one event out by index and does not allocate. This is suitable for non-real-time visualization snapshots. The hot MIDI paths still do no geometry, GL work, locks, blocking I/O, or heap allocation.

### GuiModel Instancing Extension

Files:

- `JammaLib/src/gui/GuiModel.h`
- `JammaLib/src/gui/GuiModel.cpp`

Added:

- `GuiModel::InstanceAttribute`
- `SetInstanceAttributes(std::vector<InstanceAttribute> attributes, unsigned int instanceCount)`
- `InstanceCount() const noexcept`
- Optional instance VBO creation in `InitInstanceAttributes()`
- `glVertexAttribDivisor(attribute.AttributeIndex, 1)` for per-instance attributes

Behavior:

- Existing models with no instance attributes continue to draw through the old `numInstances` path.
- Models that call `SetInstanceAttributes` draw exactly `_instanceCount` instances.
- `_ReleaseResources()` now deletes all three existing vertex buffers. Previously it deleted only two despite `_vertexBuffer[3]` existing; this fix is bundled with the instancing work.

Caution for next agent:

- `SetInstanceAttributes({}, 0)` marks `_usesInstanceAttributes = true`, so a model can intentionally draw zero instances. That is how the current `MidiModel` starts empty.
- `InitVertexArray()` now calls `_ReleaseResources()` before recreating VAO/VBOs. This compiled and tests passed, but manual app rendering should still be checked because this is shared OpenGL plumbing.

### MidiModel Shell

Files:

- `JammaLib/src/engine/MidiModel.h`
- `JammaLib/src/engine/MidiModel.cpp`

`MidiModel` currently:

- Derives from `gui::GuiModel`.
- Provides `SetLoopIndexFrac(double)` and rotates around the Y axis like `LoopModel`.
- Builds a reusable segmented arc-prism base mesh with `BuildBaseVerts()` and `BuildBaseUvs()`.
- Converts `std::vector<MidiNoteSpan>` into two instance attributes:
  - Attribute 3: start fraction, duration fraction, pitch offset, velocity.
  - Attribute 4: radius, radial thickness, note height, unused pad.
- Uses `RenderMode` to switch shader behavior:
  - `0`: scene rendering.
  - `1`: picker rendering.
  - `2`: highlight rendering.

Current defaults in `MidiModelParams`:

- Texture: `levels`
- Shader: `midi_note`
- Radius: `1.0f`
- Radial thickness: `0.035f`
- Note height: `0.035f`
- Pitch step: `0.035f`
- Center pitch: `60`

Caution for next agent:

- `MidiModel` is not yet owned, attached, arranged, or drawn by `MidiLoop`/`LoopTake`.
- Picker/highlight shader mode is present, but actual picker ownership semantics still depend on scene integration and `GlobalId()`/selection propagation.
- Pitch offset is clamped to `[-0.9, 0.9]` as an MVP readability guard. Tune after visual inspection.

### MIDI Shader Resource

Files:

- `Jamma/resources/shaders/midi_note.vert`
- `Jamma/resources/shaders/midi_note.frag`
- `Jamma/resources/ResourceList.txt`

ResourceList entry:

```text
2 midi_note MVP ObjectId Highlight LoopHover RenderMode
```

The shader pair:

- Uses per-instance attributes at locations 3 and 4.
- Places arc geometry around the Y axis in the vertex shader.
- Uses pitch offset and note height in the vertex shader.
- Uses a procedural velocity gradient in the fragment shader.
- Handles picker and highlight in the same shader via `RenderMode`.

## Issues and Difficulties Encountered

1. MSBuild project target naming
   - `JammaLib.Tests` cannot be passed directly as `/t:JammaLib.Tests`; MSBuild rejects the dot.
   - Use `/t:JammaLib_Tests` instead, or build the whole solution.

2. Worktree-specific `SolutionDir` problem
   - Repository memory notes that trailing `SolutionDir` can break vcpkg manifest args in this worktree.
   - Solution-level builds without explicit trailing `SolutionDir` worked cleanly.

3. `MidiLoop` private storage
   - `MidiLoop` had no way for a visualization model to inspect recorded events.
   - Added narrow `TryGetEvent` and `Revision` instead of exposing mutable storage or adding allocation-heavy snapshots inside the loop.

4. Shared OpenGL plumbing risk
   - `GuiModel` is a shared path for existing 3D models.
   - The changes compile and tests pass, but they need manual app rendering verification because the native tests do not deeply exercise live OpenGL drawing.

5. Existing cleanup mismatch
   - `GuiModel` had three vertex buffers but `_ReleaseResources()` deleted only two.
   - This was corrected while adding instance buffers.

## Next Implementation Slice

The next agent should start with `MidiLoop`/`LoopTake` integration, not more shader work.  Use TDD approach.

### Step 1: Add `MidiModel` ownership/attachment to `MidiLoop`

Recommended approach:

1. Forward declare `MidiModel` in `MidiLoop.h`.
2. Add `std::shared_ptr<MidiModel> _model;` to `MidiLoop`.
3. Add methods such as:

```cpp
void AttachModel(std::shared_ptr<MidiModel> model) noexcept;
std::shared_ptr<MidiModel> Model() const noexcept;
void UpdateModelFromEvents(bool force = false);
```

4. Keep `UpdateModelFromEvents` explicitly non-real-time. It may allocate a local `std::vector<MidiEvent>` or `std::vector<MidiNoteSpan>` because it must not be called from MIDI ingest/playback callbacks.
5. Use `Revision()` to avoid rebuilding when no new events have arrived.

Alternative:

- Keep `MidiLoop` event-only and let `LoopTake` own a parallel vector of `MidiModel`s. This conflicts with the PRD ownership language, so prefer `MidiLoop` ownership unless construction becomes awkward.

### Step 2: Create models when MIDI loops are created

File: `JammaLib/src/engine/LoopTake.cpp`

Current MIDI creation happens in `LoopTake::Record(...)`:

```cpp
for (auto midiChan : midiChannels)
{
    auto midiLoop = std::make_shared<MidiLoop>();
    midiLoop->StartRecord();
    _midiLoops.push_back(midiLoop);
    _midiLoopChannels.push_back(midiChan);
}
```

Add `MidiModelParams`, create `std::make_shared<MidiModel>(params)`, attach it to the `MidiLoop`, and add the model to `_children` so inherited drawing reaches it.

Use the existing loop model params style in `Loop::Loop(...)` as reference.

### Step 3: Arrange MIDI rings in `LoopTake::_ArrangeChildren()`

Current `_ArrangeChildren()` only counts `_backLoops`. Change it to count audio loops plus MIDI loops with models.

Recommended MVP ordering:

1. Audio loops first, preserving existing scales/order.
2. MIDI loops after audio loops as additional outer rings.

Use the existing scale formula, but compute it from combined ring count:

```cpp
auto numVisualRings = static_cast<unsigned int>(_backLoops.size() + _midiLoops.size());
auto dScale = 0.1;
auto dTotalScale = 0.4 / static_cast<double>(numVisualRings);
```

For each MIDI model:

- `SetModelPosition({ 0.0f, 0.0f, 0.0f })`
- `SetModelScale(1.0 + (ringIndex * dScale) - (dTotalScale * 0.5))`
- Keep GUI rack/mixer layout untouched.

### Step 4: Update MIDI models outside hot paths

Preferred minimal path:

- Reuse `LoopTake::_loopsNeedUpdating` and `JOB_UPDATELOOPS`.
- Extend `_UpdateLoops()` to call a non-real-time MIDI model update method for each `_midiLoops` entry.

During recording:

- `LoopTake::EndMultiWrite()` already sets `_loopsNeedUpdating = true` when armed.
- `RecordMidiEvent()` should continue only stamping and recording events. Do not update models there.

At record end:

- In `LoopTake::Play(...)`, after `midiLoop->EndRecord(...)`, force `midiLoop->UpdateModelFromEvents(true)` so unmatched notes clamp and appear immediately.

Throttle:

- Add a lightweight revision/sample-count throttle in `MidiLoop` or `LoopTake`.
- A simple first pass can rebuild only when revision changes and recorded sample count has advanced by about `sampleRate / 30`, if a sample rate source is easily available.
- If no sample-rate source is obvious, rebuild on `JOB_UPDATELOOPS` while recording for the next slice, then refine throttle before final acceptance.

### Step 5: Rotation sync

Reuse the audio convention from `Loop::Draw3d(...)`:

```cpp
frac = loopLength == 0 ? 0.0 : 1.0 - clamp((index % loopLength) / loopLength)
```

The hard part is finding the right `index` for MIDI rings from `LoopTake`:

- During recording, `_recordedSampCount` is available and suitable.
- During playback, there may not be a direct `LoopTake` playback index. Consider deriving from the first audio loop if present, or adding a small helper on `Loop` to expose its display fraction.
- Keep any helper non-mutating and avoid touching audio callback behavior.

### Step 6: Build and test after each integration sub-step

Use:

```powershell
$msbuild = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
& $msbuild Jamma.sln /m /t:JammaLib_Tests /p:Configuration=Debug /p:Platform=x64
& .\test\JammaLib.Tests\bin\x64\Debug\JammaLib.Tests.exe --gtest_filter="MidiNoteSpan.*:MidiLoop.*:LoopModel.*"
& .\test\JammaLib.Tests\bin\x64\Debug\JammaLib.Tests.exe
```

Request a Rubber duck review after significant step completion, and tests pass.

### Step 7: Manual app verification

After scene integration builds:

1. Build/run `Jamma` Debug x64.
2. Record a MIDI loop with one MIDI channel.
3. Confirm arcs appear while recording or immediately at record end.
4. Record short and long notes; verify duration arcs, not just hits.
5. Leave a note unmatched before record end; verify clamped-to-end arc.
6. Record multiple MIDI loops/channels; verify additional concentric rings.
7. Confirm rings rotate in sync with audio loop rings.
8. Hover/click MIDI arcs; verify picker selects owning loop/take/station level, not individual notes.
9. Verify no severe visual overlap with audio rings.

## Recommended Guardrails

- Do not allocate, lock, log, perform GL work, or rebuild geometry in `RecordMidiEvent`, `MidiLoop::RecordEvent`, or `MidiLoop::ReadBlock`.
- Keep MIDI visualization display-only.
- Do not add persistence in this slice.
- Do not implement individual note selection.
- Keep changes focused; the current implementation is deliberately not refactoring `LoopModel` yet.
- If `GuiModel` instancing shows visual regressions, inspect `_ReleaseResources()` and VAO recreation first.

## Suggested Commit Message

```text
Start MIDI loop visualization foundation

- add MIDI note span extraction and tests
- add non-real-time MidiLoop event snapshot/revision API
- add generic GuiModel instance attribute support
- add MidiModel shell and midi_note shader resource
```
