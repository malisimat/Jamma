# NINJAM Local Metronome Plan

## Goal

When connected to a remote NINJAM session, provide an optional local-only metronome so a performer can immediately hear the server beat and interval start. The physical DAC output must land on the remote beat after accounting for the device output latency.

The metronome is an orientation and performance aid. It must not alter NINJAM audio sent to other users, local loop content, transport ownership, or upstream `njclient` code.

## Agreed Behaviour

- Follow the NINJAM server BPM and BPI exactly, not local quantisation or MIDI quantisation.
- Play one pulse per server beat and use a stronger pulse at the NINJAM interval start.
- Mix to every active physical DAC channel so the click is audible from any monitored output pair.
- Keep the click local only. Remote users must never receive it.
- The temporary `CLICK` GUI toggle lives beside the existing MIDI channel override and transport-offset/global-delay controls. It will move into the main GUI panel later.
- The toggle expresses user intent, defaults to **on** on every Jamma load, and is not saved in `session.jam` or `UserConfig`.
- Disconnecting or losing valid NINJAM timing makes the metronome silent but does not change the toggle's visual state. Reconnecting resumes it automatically if the toggle remains on.
- Use a fixed conservative v1 level. Defer level/subdivision controls until the main GUI-panel work.

## Timing Contract

At callback sample $i$, the click content written into the current device buffer reaches the physical DAC after `outLatencySamps`. Therefore its projected NINJAM phase must be:

$$
phase_{remote}(i) = pos_{remote} + scale(i + outLatency_{device})
$$

where `pos_remote` is the latest NINJAM interval position and `scale` converts device samples to remote session samples when the rates differ. A beat occurs when this projected phase crosses a multiple of:

$$
samplesPerBeat_{remote} = \frac{sampleRate_{remote} \times 60}{BPM}
$$

The interval accent occurs when projected phase crosses zero modulo the remote interval length. Use phase-crossing rather than equality so a beat is never lost when it lies between two callbacks or sample-rate conversion produces a fractional result.

`NinjamConnection` already reads `NJClient::GetPosition()` from the audio-side export path. The new timing read must use the same live position source, as late as practical in the callback. Do not drive audible timing from `ExternalTransport`: it is intentionally produced by the job thread and published at interval wraps, which is too coarse for sample-accurate audio scheduling.

## Audio Ordering

The existing DAC path must become:

```text
station and remote mix
  -> ChannelMixer::ToDac
  -> NinjamController::ProcessExportBlock
  -> local NINJAM metronome mix
  -> physical DAC
```

`ProcessExportBlock` must receive the unmodified output mix. Mixing the metronome before that call would send the click to remote users, which is forbidden.

## Sound Design

The click should be transient and warm, not a pitched beep.

- Precompute short mono tables at audio-host initialization: one normal pulse and one interval accent.
- Use a short raised-cosine envelope with a very fast attack and a short decay. Target approximately 6--10 ms for normal beats and 10--14 ms for the accent.
- Build the body from a small inharmonic additive cluster, then apply a short ring-modulated high-frequency component. The inharmonic ratios and short envelope prevent an identifiable sustained pitch; the lower component keeps the transient warm.
- Use only a small fixed number of oscillator terms. Generate both tables once with `std::sin`; the callback reads samples only, with no allocation, locking, I/O, or per-sample trig.
- Normal gain is approximately -18 dBFS; the interval accent is approximately -14 dBFS. Sum normally into each DAC channel and use only a final finite/clamp guard if existing output conventions require it.

## Implementation Steps

### 1. Add a pure timing helper and tests

Create `JammaLib/src/ninjam/NinjamMetronomeTiming.h/.cpp` with a small value-input/value-output API. It should accept live interval position and length, remote BPM/BPI/sample rate, local device sample rate, device output latency, and callback block size. It returns normal and accent onset offsets in the block plus a validity result.

The helper must:

- Reject zero/invalid BPM, BPI, interval length, sample rate, or callback size.
- Perform device-to-remote-rate conversion without cumulative drift.
- Detect ordinary beat crossings, interval-wrap crossings, and coincident beat/accent crossings.
- Treat tempo, BPI, interval-length, or sample-rate changes as a fresh timing generation, with no stale pending onset.

**Verify:** Add native unit tests under `test/JammaLib_Tests/src/ninjam/` for exact beats, blocks straddling a beat, interval wrap, non-equal sample rates, output-latency advance, invalid timing, and a remote tempo/BPI change. Run the focused test target before proceeding.

### 2. Expose live audio-thread timing without upstream changes

Add a narrow metronome-timing query through `NinjamConnection`, `NinjamSession`, and `NinjamController`. The connection implementation reads the existing `NJClient::GetPosition()` and timing values, returning an invalid result when disconnected or unavailable.

Keep the API value-based and allocation-free. It must not acquire a mutex, alter NINJAM state, or modify `lib/njclient/`.

**Verify:** Unit-test the connection-independent timing helper from step 1. Inspect the new connection query and the call site against `doc/realtime-audio.md`: no locks, allocation, logging, or exceptions in the callback path.

### 3. Add the local metronome mixer

Add a small metronome component owned by `AudioHost`, or a similarly narrow audio-local helper. Generate the two fixed click tables during initialization and mix them only into an existing output buffer.

In `AudioHost::_OnAudio`:

1. Build the station/remote DAC mix with `ChannelMixer::ToDac`.
2. Call `ProcessExportBlock` with that untouched buffer.
3. Query live metronome timing and mix any scheduled table samples into every active DAC channel.

Use the current audio stream's output latency, falling back to `UserConfig::Audio.LatencyOut` when the device does not report one. The metronome must be silent if the toggle is off, no NINJAM connection exists, timing is invalid, there is no output buffer, or there are no output channels.

**Verify:** Add tests for waveform-table length/finite samples and callback-independent mixing with a known onset. Add a test or testable seam proving the export buffer is unchanged by click mixing. Build and run native tests.

### 4. Add the temporary global toggle

In `Scene`, create a `GuiToggle` immediately after the MIDI channel override and transport-offset controls. Give it a distinct action index and label it `CLICK`.

The toggle initializes to on and directly updates an atomic enabled-intent flag owned by `AudioHost`. Do not serialize the flag. Do not clear it from `DisconnectNinjam`, on invalid timing, or when the connection is absent; those states only gate audible output.

**Verify:** Launch Jamma without connecting: the toggle is visibly on but silent. Toggle it off, connect, and confirm it stays silent. Toggle it on while connected and confirm it begins at the next correctly scheduled beat. Disconnect and reconnect: visual selection persists and sound resumes only when the retained selection is on.

### 5. Full regression and real-session verification

Run the native test project using the VS Code `Build Tests (Debug x64)` task, then execute the resulting test binary. Also build the affected JammaLib project with `Build JammaLib (Debug x64)`.

Manual server test:

1. Connect to a server with known fixed BPM/BPI, ideally 120 BPM and BPI 16.
2. With `CLICK` on, confirm normal clicks occur every 500 ms and the stronger click occurs at the interval start.
3. Observe across at least ten interval wraps. The click must not drift or double-fire at callback boundaries.
4. Toggle `CLICK` off and on during the session. It must stop/start locally without affecting remote audio.
5. Ask a remote participant to confirm they never receive the click, or inspect a received/exported remote lane if a second Jamma instance is available.
6. Change server BPM/BPI, wait for the authoritative update, and confirm the click resynchronises without stale clicks from the previous grid.

Physical output-latency loopback test:

1. Route a spare physical DAC output carrying the click to an unused physical ADC input with a cable. Keep monitoring volume safe before connecting hardware.
2. Create a plain local recording station fed from that ADC input. Do not route the recorded channel back to the same DAC pair, avoiding feedback.
3. Connect to a fixed-tempo NINJAM session and record several intervals with `CLICK` on.
4. Compare recorded click onsets with the expected server beat/interval boundaries. The output-latency advance should make each physical onset land on the beat; a stable offset indicates an incorrect device `LatencyOut` estimate, while growing drift indicates a timing-projection defect.
5. Repeat after changing the device buffer size and after a server BPM/BPI change. A correct implementation remains phase-stable in both cases.

## Files Expected to Change

- `JammaLib/src/ninjam/NinjamMetronomeTiming.h/.cpp`
- `JammaLib/src/ninjam/NinjamConnection.h/.cpp`
- `JammaLib/src/ninjam/NinjamSession.h/.cpp`
- `JammaLib/src/ninjam/NinjamController.h/.cpp`
- `JammaLib/src/audio/AudioHost.h/.cpp`
- `JammaLib/src/engine/Scene.h/.cpp`
- `test/JammaLib_Tests/src/ninjam/NinjamMetronomeTiming_Tests.cpp`
- `JammaLib/JammaLib.vcxproj` and `test/JammaLib_Tests/JammaLib_Tests.vcxproj` only as needed to register new source files

## Non-Goals

- No upstream NINJAM library modifications.
- No click transmission to remote users.
- No persistent metronome setting in jam/session or user config files.
- No click-level, routing, sound-selection, or subdivision UI in this first version.
- No use of local quantisation state as a substitute for remote NINJAM timing.