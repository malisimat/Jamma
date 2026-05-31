# Feature: midi-overdubs

## Summary

Ref https://github.com/malisimat/Jamma/issues/93. MIDI overdub should behave like audio overdub: create a new loop take from an existing MIDI loop take, copy the source note material, erase only the punched-in source region, record new live MIDI only inside the punch window, and keep the source take muted while punch-in is active. The new MIDI loop length is still determined by the existing trigger flow, so overdubs can be shorter, longer, or multiple passes of the source loop.

The important musical edge cases are note boundaries. Source notes held at punch-in must be cut at punch-in. Source notes held through punch-out must restart at punch-out. Live notes already held when punch-in starts should be captured as if their NoteOn happened exactly at punch-in. Live notes still held when punch-out happens should be cut with a NoteOff at punch-out so no stuck notes escape.

## Acceptance Criteria

- New unit tests cover the core MIDI edit helper, `MidiLoop` integration, and `LoopTake`/trigger integration.
- A MIDI overdub loop contains copied source notes outside punch windows and new live notes only inside punch windows.
- Source MIDI events inside punch windows are erased only for those windows; material before and after the punch survives.
- Source notes crossing punch-in are shortened to end at punch-in; source notes crossing punch-out resume with a new NoteOn at punch-out and keep their remaining duration.
- Live NoteOn events that occur before punch-in but are still held when punch-in starts become NoteOns at punch-in.
- Live notes still held when punch-out fires get synthetic NoteOffs at punch-out.
- Source take mute/unmute behavior remains driven by existing trigger actions.
- Audio-thread code has no new locks, allocations, blocking calls, file I/O, logging spam, or shared ownership churn in hot paths.
- Existing MIDI quantisation and visual model behavior still works after overdub finalization.

## Current Code Landmarks

- MIDI loop storage and RT playback live in [JammaLib/src/midi/MidiLoop.h](../JammaLib/src/midi/MidiLoop.h#L50-L129) and [JammaLib/src/midi/MidiLoop.cpp](../JammaLib/src/midi/MidiLoop.cpp#L29-L255). `RecordEvent` is fixed-capacity, `ReadBlock` scans events, and `FlushHeldNotes` prevents stuck notes on loop wrap.
- Note-pair extraction for visualisation lives in [JammaLib/src/midi/MidiNoteSpan.cpp](../JammaLib/src/midi/MidiNoteSpan.cpp#L37-L84). This is useful as a reference, but overdub editing should preserve event playback semantics, not only model spans.
- MIDI recording is owned by `LoopTake` at [JammaLib/src/engine/LoopTake.h](../JammaLib/src/engine/LoopTake.h#L164-L179) and [JammaLib/src/engine/LoopTake.cpp](../JammaLib/src/engine/LoopTake.cpp#L874-L1075). Current recording stamps live events into `_midiLoops`; overdub setup currently creates only audio loops.
- Audio punch/overdub state is implemented in [JammaLib/src/engine/LoopTake.cpp](../JammaLib/src/engine/LoopTake.cpp#L1179-L1295) and per-loop behavior in [JammaLib/src/engine/Loop.cpp](../JammaLib/src/engine/Loop.cpp#L600-L790).
- Trigger actions already mute/unmute the source take around punch-in at [JammaLib/src/engine/Station.cpp](../JammaLib/src/engine/Station.cpp#L674-L783), so MIDI should hook into the same target/source IDs rather than inventing a parallel UX path.
- The audio callback drains live MIDI and recorded loop MIDI in [JammaLib/src/engine/Station.cpp](../JammaLib/src/engine/Station.cpp#L295-L410). MIDI ingress dispatch and recording happen in [JammaLib/src/engine/Scene.cpp](../JammaLib/src/engine/Scene.cpp#L1080-L1126).
- Ditch already flushes held loop notes through the live MIDI queue in [JammaLib/src/engine/Station.cpp](../JammaLib/src/engine/Station.cpp#L1490-L1514). Reuse that stuck-note pattern where needed.
- Existing test anchors: [test/JammaLib_Tests/src/midi/MidiLoop_Tests.cpp](../test/JammaLib_Tests/src/midi/MidiLoop_Tests.cpp#L140-L744), [test/JammaLib_Tests/src/midi/MidiNoteSpan_Tests.cpp](../test/JammaLib_Tests/src/midi/MidiNoteSpan_Tests.cpp#L21-L164), and station MIDI routing tests in [test/JammaLib_Tests/src/engine/StationMidiInstrument_Tests.cpp](../test/JammaLib_Tests/src/engine/StationMidiInstrument_Tests.cpp#L169-L281).

## Proposed Architecture

### 1. Add a Small MIDI Overdub Editing Helper

Add `JammaLib/src/midi/MidiOverdub.h` and `JammaLib/src/midi/MidiOverdub.cpp` beside `MidiLoop` and `MidiNoteSpan`, then register them in [JammaLib/JammaLib.vcxproj](../JammaLib/JammaLib.vcxproj#L346-L447) and [JammaLib/JammaLib.vcxproj.filters](../JammaLib/JammaLib.vcxproj.filters#L299-L631).

The helper should be pure, deterministic, and testable without `LoopTake`. Suggested public model:

```cpp
struct MidiPunchWindow
{
	std::uint32_t StartSample;
	std::uint32_t EndSample;
};

struct MidiOverdubRenderParams
{
	const MidiEvent* SourceEvents;
	std::size_t SourceEventCount;
	std::uint32_t SourceLoopLengthSamps;
	std::uint32_t TargetLoopLengthSamps;
	const MidiPunchWindow* PunchWindows;
	std::size_t PunchWindowCount;
};

std::size_t BuildMidiOverdubBaseEvents(const MidiOverdubRenderParams& params,
									   MidiEvent* outEvents,
									   std::size_t outCapacity) noexcept;
```

Creative bit: treat the source loop as a repeating timeline, then run an event-level interval subtractor over it. Instead of trying to mutate the source events in place, expand source events into note lifetimes across the target loop, subtract punch windows, then emit canonical NoteOn/NoteOff pairs for the remaining segments. That gives clean behavior for:

- target loop shorter than source loop;
- target loop longer than source loop;
- target loop covering multiple source repeats;
- source note crossing punch-in;
- source note crossing punch-out;
- velocity-zero NoteOn used as NoteOff.

Keep this helper non-RT only unless it is fed fixed caller-provided arrays. Avoid `std::vector` in functions that can run from audio or MIDI ingress paths. A two-layer design is okay: one fixed-buffer `noexcept` core, and small non-RT convenience wrappers for tests/model prep.

### 2. Extend `MidiLoop` with Controlled Bulk/Copy Operations

Current `MidiLoop` has fixed storage and public `TryGetEvent` at [JammaLib/src/midi/MidiLoop.h](../JammaLib/src/midi/MidiLoop.h#L93-L99), but no way to install a pre-rendered base event list. Add a small API such as:

```cpp
bool AppendEventForBuild(const MidiEvent& ev) noexcept;
void ReplaceRecordedEvents(const MidiEvent* events, std::size_t count, std::uint32_t loopLengthSamps) noexcept;
void FinalizeOverdubBase(std::uint32_t loopLengthSamps);
```

Pick the narrowest API that fits. The goal is to let `LoopTake` build an overdub loop from source + live punched events without exposing mutable internals or changing playback. Preserve `DefaultCapacity` drop-newest behavior from [JammaLib/src/midi/MidiLoop.cpp](../JammaLib/src/midi/MidiLoop.cpp#L40-L54). If the generated overdub exceeds capacity, drop events deterministically, increment `_dropped`, and keep tests explicit about it.

Do not disturb `ReadBlock` semantics at [JammaLib/src/midi/MidiLoop.cpp](../JammaLib/src/midi/MidiLoop.cpp#L152-L191). Any stuck-note prevention should be represented as generated NoteOff events or existing held-note flushing, not extra runtime branching in playback unless absolutely necessary.

### 3. Track MIDI Overdub Session State in `LoopTake`

Add minimal state to [JammaLib/src/engine/LoopTake.h](../JammaLib/src/engine/LoopTake.h#L237-L257):

- source take ID or source `MidiLoop` snapshots for each MIDI loop;
- punch windows in target-loop sample coordinates;
- active live notes by channel/note/device so starts can be clamped to punch-in;
- current punch-in sample position, or an invalid sentinel when inactive;
- a preallocated scratch buffer for building final MIDI events off the callback path.

Do not store references into another take's mutable vector that can be invalidated. Snapshot source MIDI events by value when overdub starts or when source take is resolved. The source `MidiLoop::TryGetEvent` API at [JammaLib/src/midi/MidiLoop.cpp](../JammaLib/src/midi/MidiLoop.cpp#L81-L88) is the safe read surface.

Design note: a small `MidiOverdubSession` struct owned by `LoopTake` is cleaner than sprinkling booleans and vectors through `LoopTake`. Keep it private to `LoopTake` unless tests need direct helper coverage.

### 4. Resolve Source MIDI Loops During Overdub Start

`Station::OnAction(TriggerAction)` already computes `sourceId` and creates the target take in [JammaLib/src/engine/Station.cpp](../JammaLib/src/engine/Station.cpp#L674-L686). Extend the handoff so `LoopTake::Overdub` can receive MIDI channel/device configuration and source MIDI loop snapshots.

Preferred shape:

1. In `TRIGGER_OVERDUB_START`, resolve `sourceLoopTake = _TryGetTake(sourceId)` before creating the new take.
2. Pass MIDI input channel/device info from `TriggerAction` to `newLoopTake->Overdub(...)`, mirroring `Record(...)` at [JammaLib/src/engine/LoopTake.cpp](../JammaLib/src/engine/LoopTake.cpp#L874-L931). Today `Overdub` only accepts audio channels at [JammaLib/src/engine/LoopTake.h](../JammaLib/src/engine/LoopTake.h#L164-L166).
3. Let `LoopTake::Overdub` allocate target `MidiLoop` objects for the requested MIDI inputs just like `Record` does, but start them in an overdub-capture mode rather than immediately copying events.
4. Associate each target MIDI loop with matching source loop(s) by output slot/channel/device. If a matching source loop is missing, record only live punched notes for that target loop.

This avoids app-shell sprawl: `Station` resolves ownership and IDs; `LoopTake` owns loop-building; `MidiOverdub` owns MIDI event algebra.

### 5. Gate Live MIDI Recording by Punch State

Modify `LoopTake::RecordMidiEvent` at [JammaLib/src/engine/LoopTake.cpp](../JammaLib/src/engine/LoopTake.cpp#L934-L967) so MIDI overdub behaves differently from plain record:

- Plain recording remains unchanged.
- Overdub target takes ignore live MIDI when punch-in is inactive.
- During punch-in, live NoteOns are written with sample offsets clamped to the punch-in start if the key was already held before punch-in.
- During punch-in, live NoteOffs are written at the resolved record sample, capped to the active punch window.
- On punch-out, any still-held live notes for that target loop get synthetic NoteOffs at the punch-out sample.

Important edge: live notes can be pressed before punch-in because [Scene.cpp](../JammaLib/src/engine/Scene.cpp#L1080-L1126) dispatches all note events to armed takes. The overdub session should track held live notes even while not writing them, so it knows which notes need NoteOn at punch-in.

### 6. Capture Punch Windows Precisely

Extend `LoopTake::PunchIn` and `LoopTake::PunchOut` at [JammaLib/src/engine/LoopTake.cpp](../JammaLib/src/engine/LoopTake.cpp#L1248-L1288):

- Compute punch sample in the target loop's record timeline from `_recordedSampCount`.
- On punch-in, open a `MidiPunchWindow` and synthesize NoteOns for live-held notes that should enter the window.
- On punch-out, close the current window, synthesize NoteOffs for live-held notes, and leave source mute/unmute untouched because `Station` already handles it in [Station.cpp](../JammaLib/src/engine/Station.cpp#L753-L778).
- If recording end happens while punch-in is active, close the window before final render; avoid relying on UI state to prevent dangling windows.

The punch window coordinate system should be target-recording samples, not global audio samples. Global samples are only for input latency compensation via `ResolveMidiRecordSample` at [JammaLib/src/engine/LoopTake.cpp](../JammaLib/src/engine/LoopTake.cpp#L1017-L1033).

### 7. Render Final MIDI Overdub at `Play`/End Record Time

Use `LoopTake::Play` at [JammaLib/src/engine/LoopTake.cpp](../JammaLib/src/engine/LoopTake.cpp#L1036-L1075) as the target-loop length boundary, because audio overdub already receives the final quantised length there. After length is known:

1. Build base copied events from source snapshots with `BuildMidiOverdubBaseEvents` using target loop length.
2. Merge in live punched events recorded during the overdub session.
3. Sort by sample offset with a stable tie-break: NoteOff before NoteOn for the same channel/note at the same sample, then original order for everything else. This reduces stuck-note risk at exact punch boundaries.
4. Install the merged events into each target `MidiLoop` and call `EndRecord(loopLength)`.
5. Apply current MIDI quantisation with `SetQuantisation(MidiQuantisation())` and refresh the model just as current recording does at [JammaLib/src/engine/LoopTake.cpp](../JammaLib/src/engine/LoopTake.cpp#L1057-L1064).

Avoid doing source-copy rendering on the audio callback. This can run in the action/job path where `Play` and final model updates already happen.

### 8. Stuck-Note Safety Pass

Explicitly review these after implementation:

- Existing loop wrap flush in [MidiLoop.cpp](../JammaLib/src/midi/MidiLoop.cpp#L152-L191) and [MidiLoop.cpp](../JammaLib/src/midi/MidiLoop.cpp#L228-L251).
- Ditch held-note flush in [Station.cpp](../JammaLib/src/engine/Station.cpp#L1490-L1514).
- New punch-out synthetic NoteOffs.
- Boundary sort order at exact punch-in/punch-out samples.
- Target loop end: unmatched live NoteOns should get NoteOff at `TargetLoopLengthSamps`, and `MidiLoop::ReadBlock` will then also protect wrap playback.

Do not add a mutex to `Scene::_DispatchMidi`/MIDI ingress, `Station::WriteBlock`, `LoopTake::RecordMidiEvent`, `LoopTake::ReadMidiBlock`, or `MidiLoop::ReadBlock`. Prefer atomics, existing queues, and preallocated fixed arrays.

## Test Plan

Add focused tests first; let them define the behavior before wiring the full feature.

### Helper Tests

Create `test/JammaLib_Tests/src/midi/MidiOverdub_Tests.cpp`, register it in [test/JammaLib_Tests/JammaLib_Tests.vcxproj](../test/JammaLib_Tests/JammaLib_Tests.vcxproj#L186-L191) and [test/JammaLib_Tests/JammaLib_Tests.vcxproj.filters](../test/JammaLib_Tests/JammaLib_Tests.vcxproj.filters#L92-L109).

Cases:

- `CopiesSourceOutsideSinglePunchWindow`.
- `CutsSourceNoteAtPunchIn`.
- `RestartsSourceNoteAtPunchOut`.
- `ErasesSourceNoteFullyInsidePunchWindow`.
- `RepeatsSourceAcrossLongerTargetLoop`.
- `ClipsSourceToShorterTargetLoop`.
- `HandlesVelocityZeroNoteOnAsNoteOff`.
- `CanonicalOrderingSendsNoteOffBeforeNoteOnAtBoundary`.
- `DropsDeterministicallyWhenOutputCapacityExceeded`.

### `MidiLoop` Tests

Extend [test/JammaLib_Tests/src/midi/MidiLoop_Tests.cpp](../test/JammaLib_Tests/src/midi/MidiLoop_Tests.cpp#L140-L744):

- Replace/install events preserves playback timing.
- Replace/install events updates dropped count when capacity is exceeded.
- Quantisation still applies after installing an overdub event set.
- Model rebuild uses the installed overdub events.

### `LoopTake` MIDI Overdub Tests

Add near existing `LoopTakeMidi...` tests in [MidiLoop_Tests.cpp](../test/JammaLib_Tests/src/midi/MidiLoop_Tests.cpp#L424-L506), or create `test/JammaLib_Tests/src/engine/LoopTakeMidiOverdub_Tests.cpp` if the file gets too chunky.

Cases:

- `OverdubCreatesMidiLoopsMatchingConfiguredChannelsAndDevices`.
- `RecordMidiEventIgnoredOutsidePunchWindowForOverdub`.
- `PunchInCapturesAlreadyHeldLiveNoteAtPunchStart`.
- `PunchOutClosesHeldLiveNoteAtPunchEnd`.
- `PlayFinalizesCopiedSourceAndLivePunchEvents`.
- `OverdubWithoutSourceMidiStillRecordsLivePunchOnly`.

### Station/Trigger Tests

Add integration tests near [test/JammaLib_Tests/src/engine/Trigger_Tests.cpp](../test/JammaLib_Tests/src/engine/Trigger_Tests.cpp#L867-L1416) or [test/JammaLib_Tests/src/engine/StationMidiInstrument_Tests.cpp](../test/JammaLib_Tests/src/engine/StationMidiInstrument_Tests.cpp#L169-L281):

- Overdub trigger start passes source MIDI take information to the target take.
- Punch-in start mutes source take and opens target MIDI window.
- Punch-in end unmutes source take and closes target MIDI window.
- Recorded overdub playback routes through existing MIDI VST output indices.

### Regression/Hot Path Tests

- Existing `MidiLoop` stuck-note tests at [MidiLoop_Tests.cpp](../test/JammaLib_Tests/src/midi/MidiLoop_Tests.cpp#L277-L381) must still pass.
- Add a focused boundary regression for NoteOff/NoteOn on the same sample at punch-out.
- Build and run only affected native tests first:

```powershell
$msbuild = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
$repoRoot = (Get-Location).Path
while (-not (Test-Path (Join-Path $repoRoot "Jamma.sln"))) {
	$parent = Split-Path $repoRoot -Parent
	if ($parent -eq $repoRoot) { throw "Could not find Jamma.sln." }
	$repoRoot = $parent
}
$testsProj = Join-Path $repoRoot "test\JammaLib_Tests\JammaLib_Tests.vcxproj"
$testsExe = Join-Path $repoRoot "test\JammaLib_Tests\bin\x64\Debug\JammaLib_Tests.exe"
$solutionDirArg = "/p:SolutionDir=$($repoRoot.TrimEnd('\\'))\"
& $msbuild $testsProj /m /t:Build /p:Configuration=Debug /p:Platform=x64 $solutionDirArg
& $testsExe --gtest_filter="MidiOverdub.*:MidiLoop.*:LoopTakeMidiOverdub.*:StationMidiInstrument.*"
```

Then run the full test exe if focused tests pass.

## Implementation Steps

1. Add failing helper tests for source-copy/punch-window event algebra.
2. Implement `MidiOverdub` as a pure helper with no engine dependencies.
3. Add `MidiLoop` install/append API and tests.
4. Add `LoopTake` MIDI overdub session state, but keep plain record paths untouched.
5. Extend `LoopTake::Overdub` to accept MIDI channel/device config and source snapshots.
6. Update `Station::OnAction(TriggerAction)` to pass source MIDI info through existing trigger flow.
7. Gate `LoopTake::RecordMidiEvent` by punch state for overdub target takes.
8. Capture punch windows and live held notes in `PunchIn`/`PunchOut`.
9. Render final target MIDI events in `Play`, then rebuild models and quantisation state.
10. Run focused tests, full tests, and manual hot-path review.
11. Perform a rubber duck review: explain how one note flows through source copy, punch erasure, live capture, final render, playback, ditch, and loop wrap. Fix anything that sounds weird when said out loud.
12. Produce an HTML review summary with design, files changed, tests run, residual risks, and links back to this plan.

## Hot-Path Guardrails

- No `std::mutex`, `std::lock_guard`, `std::unique_lock`, `std::scoped_lock`, `std::condition_variable`, `WaitForSingleObject`, `EnterCriticalSection`, sleeps, logging, file I/O, or heap allocation in audio-thread owned functions.
- Treat these as callback/hot-path inspection targets after edits: `Scene::_DispatchMidi`, `Station::WriteBlock`, `Station::OnBlockWriteChannel`, `Station::EndMultiWrite`, `LoopTake::RecordMidiEvent`, `LoopTake::ReadMidiBlock`, `LoopTake::EndMultiWrite`, `LoopTake::PunchIn`, `LoopTake::PunchOut`, `MidiLoop::RecordEvent`, and `MidiLoop::ReadBlock`.
- If a helper needs allocation or sorting, call it from trigger/action/job finalization, not from `Station::WriteBlock` or MIDI ingress.
- Keep source snapshots immutable once an overdub session starts. Prefer value copies into fixed buffers over shared mutable ownership.
- Keep `Station` changes thin: ID/source resolution only. Keep behavior in `LoopTake` and MIDI helpers.

## Risks & Mitigations

- Risk: copied source notes produce stuck notes at punch boundaries. Mitigation: helper emits canonical NoteOff-before-NoteOn ordering and tests exact-boundary cases.
- Risk: MIDI source copy is done on an audio callback. Mitigation: render after target length is known in `LoopTake::Play`, not during playback.
- Risk: code sprawl across `Station`, `Scene`, and `LoopTake`. Mitigation: `Station` only resolves source; `LoopTake` owns sessions; `MidiOverdub` owns event transformation.
- Risk: mismatched source/target MIDI loop mapping. Mitigation: map by channel + device first, then stable slot fallback only when legacy data lacks device names.
- Risk: target length differs from source length. Mitigation: helper uses target timeline expansion with source modulo mapping.
- Risk: quantisation/model path drifts. Mitigation: re-use `MidiLoop::SetQuantisation` and existing model update path after final event install.

## Handoff Request For Implementation Agent

Use this plan as the contract. Implement the feature end to end, keeping architecture clean and avoiding code sprawl. Favor a small pure MIDI helper plus minimal `LoopTake` integration over spreading MIDI edit logic through `Station` or `Scene`. Preserve real-time audio constraints: no locks, allocations, blocking, logging, or shared ownership churn in callback-owned paths. Add tests first where practical, keep diffs focused, update project files for new C++ sources/tests, then build and run the targeted test command above. After implementation, perform the rubber duck review described in step 11 and create/open the HTML review summary from step 12.

## TODOs

- [x] Expand plan with relevant files and line-number landmarks.
- [ ] Add MIDI overdub helper tests.
- [ ] Implement pure MIDI overdub event helper.
- [ ] Add safe `MidiLoop` bulk install API.
- [ ] Wire `LoopTake` MIDI overdub session state.
- [ ] Pass source MIDI take data through `Station` trigger flow.
- [ ] Add loop take and station integration tests.
- [ ] Verify focused and full tests.
- [ ] Rubber duck review stuck-note and audio-thread safety.
- [ ] Produce and open HTML review summary.
