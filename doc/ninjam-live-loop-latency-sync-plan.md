# NINJAM Live/Loop Audio Latency and Interval-Phase Alignment

Status: **revised implementation plan.** This document supersedes the previous dual-delay proposal. It is deliberately scoped to NINJAM export timing; VST latency observability is useful but must be a separate change after this work is verified.

## 1. Objective and non-negotiable invariant

`NinjamConnection::ProcessExportBlock` currently packs the DAC mix (`outBuf`) and raw ADC input (`inBuf`) into NINJAM local channels in the same audio callback ([NinjamConnection.cpp](../JammaLib/src/ninjam/NinjamConnection.cpp)). The streams have different physical capture/emission times, so packing their current samples together is not sufficient.

The completed implementation must satisfy both invariants for every transmitted sample while NINJAM timing is valid:

1. The DAC and ADC lane samples represent the same musical/grid instant.
2. That instant equals the NINJAM encoder phase assigned to that sample.

For interval length $L$, encoder phase for output sample $i$ of a callback $P_i$, and source content phase $C_i$, the invariant is:

$$C_i \equiv P_i \pmod L$$

The relative relation is a consequence, not the primary oracle:

$$K_{dac} - K_{adc} \equiv L_{out} + L_{in} \pmod L$$

Passing only the relative relation is insufficient: two streams can be mutually aligned and still be displaced from the NINJAM interval grid.

## 2. Verified NINJAM behavior and required upstream seam

The exact NINJAM source was inspected at `C:\Users\matto\Source\Repos\NinjamLib\ninjam\ninjam\njclient.cpp`.

- `NJClient::AudioProc` owns `m_interval_pos` on the audio thread. It encodes the input stream in order and increments that position after processing each contiguous interval segment.
- It splits a callback at an interval boundary, so sample $i$ has phase $(P_0 + i) \bmod L$.
- `GetPosition` is an unlocked read of `m_interval_pos` and `m_interval_length`. It is therefore appropriate only as an immediate audio-thread observation adjacent to `AudioProc`, not as a generic cross-thread timing API.
- On the callback that begins at an interval boundary, `GetPosition` currently exposes the exhausted old interval (`pos == length`). `AudioProc` then applies a pending BPM/BPI update, calls `on_new_interval`, and resets its position to zero before consuming sample zero. A caller cannot derive the new interval length from the current public API before choosing source samples for that callback.

The final point makes the present public API inadequate for sample-accurate behavior during tempo/BPI changes. Do not paper over it with a stale job-thread snapshot or an unanchored local counter.

### Required NINJAM-library addition

Add an audio-thread-only preflight method to the vendored NINJAM source:

```cpp
// Call only from the audio thread, immediately before AudioProc with this sample rate.
// Applies an exhausted-interval transition exactly as AudioProc would, then returns the
// phase and length assigned to input sample zero of the upcoming AudioProc call.
void GetAudioBlockStartPosition(int sampleRate, int* position, int* length);
```

Implement it by extracting `AudioProc`'s `m_interval_pos >= m_interval_length` / negative-position transition, including the `m_misc_cs`-guarded pending BPM/BPI update and `on_new_interval`, into one private helper called by both methods. `AudioProc` must not transition again after preflight has prepared the interval.

The method has one narrow contract:

> Immediately after `GetAudioBlockStartPosition(rate, &P, &L)` and immediately before `AudioProc(..., B, rate)`, source sample $i$ passed to `AudioProc` is encoded at phase $(P+i) \bmod L$, including callbacks that cross an interval boundary.

Update the vendored `njclient.h`, rebuild the matching Debug and Release `njclient.lib`, and keep the header and binaries under `lib/njclient` in lockstep. The Jamma change must not begin until this dependency is available. This is the smallest honest seam: it avoids reimplementing or guessing NINJAM's boundary logic in Jamma.

## 3. Direct encoder-phase delay math

The preflight result is the direct target: for callback sample offset $i$, NINJAM assigns encoder phase $(P+i) \bmod L$. This plan relies on the existing receive-side transport discipline: before device latency, the loop content written to `outBuf[i]` is already the local musical/grid phase $(P+i) \bmod L$. If that prerequisite is false, no NINJAM-send delay can establish absolute phase; fix the local transport mapping first. `ExternalTransport` remains receive-side and must not be sampled from this callback.

Using the existing device-report convention, resolve `inLatencySamps` and `outLatencySamps` from `AudioStreamParams` first and `UserConfig` only when the device reports zero. The true content phase of an undelayed source sample at callback offset $i$ is:

$$
\begin{aligned}
C_{dac}(i) &\equiv P + i + L_{out} \pmod L \\
C_{adc}(i) &\equiv P + i - L_{in} \pmod L
\end{aligned}
$$

For a source sample delayed by $K$ frames and fed to NINJAM at offset $i$, require `C(i - K) == P+i`. The per-generation constant delays are:

$$
\begin{aligned}
K_{dac} &\equiv L_{out} \pmod L \\
K_{adc} &\equiv -L_{in} \pmod L
\end{aligned}
$$

Use the smallest non-negative residues. In particular, an ADC advance that would be impossible in real time is represented by an ordinary causal delay of $L-L_{in}$ frames. The resulting streams are both phase-correct and preserve:

$$K_{dac} - K_{adc} \equiv L_{out} + L_{in} \pmod L$$

### Generation rules

A generation is valid only after `GetAudioBlockStartPosition` returns `L > 0` and the delay history needed for the selected $K$ values has been written.

- Begin a new generation when the preflight interval length changes, when the connection is recreated, or when the audio format changes. Never reset an independent `_lanePos` and pretend it is an NINJAM clock.
- On a new generation, clear audio-owned delay cursors and mark the lane history invalid. Until enough history exists for the largest required read, transmit silence on the affected lane(s), not pass-through or stale ring-buffer content. This can last at most one supported NINJAM interval.
- A normal interval wrap with the same length is not a generation change. The preflight phase wraps naturally and the delay remains constant.

The application already treats local loop timing as remote-disciplined. The integration test must confirm that the `outBuf` prerequisite matches the established audible loop/metronome alignment; the export path must not use a stale UI/transport snapshot to manufacture that result.

## 4. Ownership, realtime constraints, and placement

Keep the export delay state inside `NinjamConnection`. It owns NINJAM lane packing and the `AudioProc` call; `AudioHost` remains a generic RtAudio/station mixer.

`AudioHost` resolves `inLatencySamps` and `outLatencySamps`, then threads them through the existing `SetAudioFormat` relay (`AudioHost -> NinjamController -> NinjamSession -> NinjamConnection`). No extra local clock is needed in the export API: preflight is the authoritative encoder clock.

The only mutable delay cursors and audio sample storage are owned by the audio callback. No callback code may allocate, lock, log, resize a vector, publish a channel layout, or touch `ExternalTransport`.

### Fixed storage

Reuse `audio::AudioBuffer`, but allocate one buffer per **physical** DAC and ADC channel, not per current NINJAM lane. Lane packing can change on the job thread; physical device channel counts are fixed for the active audio format. This avoids a lifetime race between `_RefreshLanePacking` and `ProcessExportBlock`.

- Allocate the DAC and ADC delay-buffer vectors in `SetAudioFormat` before the audio stream starts or while it is stopped. This is the existing audio-format publication boundary; do not resize in `_RefreshLanePacking`.
- Keep `_RefreshLanePacking` limited to publishing `NinjamLanePacking` and calling the existing NINJAM local-channel configuration. `ProcessExportBlock` takes one atomic packing snapshot for its complete callback.
- Capacity must be `MaxNinjamIntervalSamps + constants::MaxBlockSize`. The extra block is required because `AudioBuffer::Delay` is invoked after `EndWrite`.
- Add a derived, documented `constants::MaxNinjamIntervalSamps` with an explicit supported-domain calculation: maximum supported sample rate times the maximum supported BPI divided by the minimum supported BPM, converted from minutes to seconds. Choose the actual supported limits before coding; do not use "generous" as a specification.
- If preflight reports `L > MaxNinjamIntervalSamps`, disable compensated export for that generation and publish a non-realtime diagnostic through an atomic flag/counter. Do not call `AudioBuffer::Delay` with an oversized value: it clamps and would silently send phase-wrong content.

## 5. Per-callback procedure

`ProcessExportBlock` remains the callback-owned path. Its order is important.

1. Load the already-published lane packing once. Validate pre-allocated scratch capacity and physical delay-buffer counts; return silently if unavailable.
2. Write each available interleaved physical DAC/ADC channel into its corresponding `AudioBuffer` with `AudioWriteRequest{ stride = physicalChannelCount, fadeCurrent = 0, fadeNew = 1 }`, then call `EndWrite(numFrames, true)`.
3. Call `GetAudioBlockStartPosition(sampleRate, &phase, &length)` immediately before packing and immediately before `AudioProc`. It is the sole authoritative encoder timing input. Validate `0 <= phase < length` and `length > 0`.
4. Update or validate the current timing generation and calculate `K_dac` and `K_adc` with a signed 64-bit intermediate before calling `utils::ModNeg`. Do not subtract unsigned values before widening.
5. Because the write cursor now points immediately after this callback's block, request `buffer.Delay(K + numFrames)`, not `buffer.Delay(K)`. `AudioBuffer::Delay(0)` after `EndWrite` points at the next unwritten location; `ChannelMixer::InitPlay` follows the same post-write convention by adding its block size.
6. Read from the resulting per-channel play index and pack into the existing `_inScratch` using the current DAC-pair/ADC-pair mapping. Reuse `AudioBuffer::IsContiguous` / `BlockRead` for a fast contiguous segment and one wrap segment. Preserve the present additive packing behavior in modulo mode.
7. If a delay history is not yet valid, leave the relevant scratch lanes zeroed. Then call `AudioProc` exactly once with the same `numFrames` and sample rate supplied to preflight.

Do not call general NJClient getters, job-thread snapshot code, `std::cout`, a mutex, or any allocation in this sequence. The lifetime guard already used by `NinjamSession` keeps the `NinjamConnection` object alive across the call; this design must not introduce a second locking scheme.

## 6. Small, deterministic tests

Do not try to unit-test the whole network client. Extract a small pure helper, for example `ninjam::ExportLaneTiming`, whose input is:

```cpp
struct ExportLaneTimingInput
{
    unsigned int EncoderPhase = 0;
    unsigned int IntervalLength = 0;
    unsigned int InputLatencySamps = 0;
    unsigned int OutputLatencySamps = 0;
    unsigned int NumFrames = 0;
};
```

It returns validated generation information plus `DacDelaySamps` and `AdcDelaySamps`. The helper owns no buffers and makes no NJClient calls. In tests, a tiny fake phase source returns `(EncoderPhase, IntervalLength)`; it is a value provider, not a mock `NJClient`.

Add these focused native tests:

1. **Absolute phase, both lanes.** Use a short synthetic interval, a non-zero encoder phase, non-zero input/output latency, and tagged source samples whose tag is their source phase. For each delayed selected sample, assert its tag equals `(EncoderPhase + sampleOffset) % IntervalLength` for both DAC and ADC. This is the primary test and proves more than the relative equation.
2. **Post-write cursor and ring wrap.** Simulate two small callback blocks in a real `AudioBuffer`; write then `Delay(K + numFrames)`. Assert the first selected sample is exactly the intended historical tag across the buffer wrap. This prevents the off-by-one-block regression.
3. **Boundary/new-generation behavior.** Feed a fake preflight result that changes from exhausted old timing to phase zero with a new interval length. Assert the helper starts a new generation, recalculates both delays, and marks history invalid until primed.

The first test must also assert the relative identity, but it must not replace the absolute-phase assertions with it. Keep test inputs deliberately small (for example, `L = 17`, blocks of 4 or 5 samples) so failures are readable.

## 7. Integration verification

After unit tests pass:

1. Build `JammaLib` and `JammaLib_Tests`, then run `JammaLib_Tests.exe` as described in [build.md](build.md).
2. Add temporary non-realtime diagnostics for generation starts, preflight phase/length, selected delays, history-ready transition, and unsupported interval rejection. Drain them from the job/UI side; remove or gate noisy diagnostics after validation.
3. Run a physical DAC-to-ADC loopback against a test NINJAM server. Confirm a tagged transient from both exported lanes arrives at the same interval phase, and the loop lane remains on the remote metronome grid across normal wraps and an accepted BPM/BPI change.
4. Confirm local monitoring, local recording/overdub alignment, and receive-side `ExternalTransport` behavior are unchanged. This work exports a copy of audio only; it must not change station cursor or local output behavior.

## 8. Implementation order

1. Add and test the NINJAM-library audio-thread preflight API; rebuild and stage matching headers/libraries in Jamma's vendored dependency.
2. Add the pure `ExportLaneTiming` helper and its three small native tests. Land the absolute-phase proof before integrating buffers.
3. Add the fixed physical-channel delay storage and latency plumbing. Preserve the existing atomic lane-packing snapshot and do not allocate from its refresh path.
4. Integrate the per-callback procedure, including post-write `K + numFrames` indexing, generation priming, and non-realtime diagnostics.
5. Run the native tests and loopback/BPM-change verification. Only then update [ninjam.md](ninjam.md) with a short timing-and-sync cross-reference.

## 9. Explicitly deferred work

VST latency read-path plumbing and playback compensation are not part of this change. They do not establish NINJAM phase correctness and would expand the review surface across plugin, loop, MIDI, and station ownership. Revisit them in a separate plan after this export timing work has objective absolute-phase coverage.