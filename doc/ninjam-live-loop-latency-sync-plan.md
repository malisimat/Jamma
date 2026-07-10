# NINJAM Export Latency and Interval-Phase Alignment

Status: **implementation-ready, blocked on an upstream-supported NINJAM timing contract and MIDI baseline reconciliation.**

This document replaces the earlier plan that proposed editing `NinjamLib`. Jamma does
not own that library. Do not patch its source, add a private API to its headers, or
rebuild and stage local `njclient.lib` artifacts. Upstream may change independently;
this design consumes only a released, supported NINJAM API.

The scope is outbound NINJAM local-channel audio: the DAC mix (`outBuf`) and raw ADC
input (`inBuf`) passed by `AudioHost` to
`NinjamConnection::ProcessExportBlock`. It neither changes local monitoring/recording
nor replaces Jamma's receive-side `ExternalTransport` discipline.

## 1. Definition of done

For every source sample accepted by `NJClient::AudioProc` while compensation is
enabled, each transmitted DAC and ADC lane must describe the exact NINJAM musical
instant to which its encoder sample is assigned.

Let $L$ be the active NINJAM interval length, $P$ the phase assigned by NINJAM to
callback sample zero, and $i$ the callback offset. A correctly selected source sample
has content phase:

$$C_i \equiv P + i \pmod L$$

This is the only acceptance invariant. The two streams also align with one another,
but the following relative relation alone is insufficient because both lanes can be
wrong by the same interval offset:

$$K_{dac} - K_{adc} \equiv L_{out} + L_{in} \pmod L$$

Compensation is complete only when the unit and physical-loopback tests in this plan
prove the absolute invariant across normal interval wraps and accepted BPM/BPI changes.

## 2. Confirmed current behavior and ownership boundary

### Export call chain

The audio callback performs this path:

```text
AudioHost::_OnAudio
  -> NinjamController::ProcessExportBlock
  -> NinjamSession::ProcessExportBlock
  -> NinjamConnection::ProcessExportBlock
  -> NJClient::AudioProc
```

`NinjamConnection::ProcessExportBlock` currently clears preallocated scratch lanes,
packs the current callback's interleaved physical DAC and ADC samples, then calls
`NJClient::AudioProc` once. It has no compensation and no independent encoder clock.

`AudioHost::_OnAudio` already resolves device-reported input/output latency in other
audio paths with the intended precedence: `AudioStreamParams` first, then
`UserConfig.Audio` only when a device reports zero. The export path must use exactly
that convention; it must not invent a third latency source.

### Why the present public NINJAM API is insufficient

The current upstream `NJClient::GetPosition(int*, int*)` reads the current
`m_interval_pos` and `m_interval_length`. `NJClient::AudioProc` itself owns and
increments that state. At an exhausted boundary it applies pending BPM/BPI data, calls
`on_new_interval`, resets the position to zero, and only then consumes sample zero.

Consequently, immediately before `AudioProc`, `GetPosition` can report the exhausted
old interval. Jamma cannot learn the new length before selecting the source samples.
That makes `GetPosition` unsuitable for a proof of the invariant at a tempo/BPI
transition. A stale remote snapshot or a local counter has the same defect.

**Do not use any of these as a substitute for encoder phase:**

- `GetPosition` at an exhausted boundary;
- `NinjamRemoteSnapshot` or `ExternalTransportState`;
- `AudioHost::_audioSampleCounter`;
- local loop/take positions, master-loop position, MIDI anchors, or metronome state.

`ExternalTransport` is deliberately receive-side. It publishes immutable,
wrap-gated snapshots to re-anchor Jamma loops to remote timing. It is valuable for a
new joiner with no pre-existing master loop, but it is neither current per-sample
encoder phase nor an audio-callback authority.

## 3. External dependency gate: supported NINJAM timing contract

No Jamma compensation code may be started until upstream releases, or the project
adopts through its normal dependency process, an audio-thread contract equivalent to
the following. The exact upstream name and shape may differ; its semantic contract may
not.

```cpp
struct NJClientAudioBlockTiming
{
    int IntervalPositionSamps;
    int IntervalLengthSamps;
};

// Audio-thread only. Call immediately before AudioProc with the same sample rate.
// Returns the phase and interval length assigned to AudioProc input sample zero.
// It resolves a pending/exhausted interval transition exactly once.
bool GetNextAudioBlockTiming(int sampleRate, NJClientAudioBlockTiming* timing);
```

Required upstream guarantees:

1. Given a successful result `(P, L)`, immediately followed by one
   `AudioProc(..., B, sampleRate)`, input sample $i$ is encoded at
   $(P + i) \bmod L$. This includes a callback that begins at, or crosses, an
   interval boundary.
2. `P` and `L` describe the active BPM/BPI after all pending timing changes relevant
   to sample zero. `0 <= P < L` and `L > 0`.
3. Calling the timing API prepares a pending transition at most once. The subsequent
   `AudioProc` must not prepare it again.
4. The call is valid only on the same audio thread and is adjacent to `AudioProc`.
   No job/UI-thread use or cross-thread observation is permitted.
5. It introduces no Jamma-owned mutex, allocation, callback, logging, or second
   transition. Any pre-existing internal synchronization remains upstream's concern
   and must not occur more often than in the unmodified `AudioProc` path.
6. It has a stable versioned header/library contract. Jamma consumes the released
   upstream artifact through the existing dependency update process; it does not
   maintain a patch or fork.

### Gate acceptance

Before Phase 1, record the upstream release/version and verify a small integration
probe against its shipped header and binary:

- normal block: timing `(P, L)` advances by `B` after `AudioProc`;
- block crossing a wrap: every segment has the predicted modulo phase;
- pending BPM/BPI change: the first sample after the boundary reports `(0, L_new)`;
- repeated timing queries before `AudioProc` are either prohibited or explicitly
  specified by upstream. Jamma calls it exactly once regardless.

If this contract is unavailable, the correct state is **uncompensated export**, not an
approximation. Do not proceed with `GetPosition`, `ExternalTransport`, or a guessed
local lane position.

## 4. Timing model and latency convention

The Jamma transport prerequisite is that, before physical device latency, the local
content written to `outBuf[i]` is already the musical phase $(P+i) \bmod L$. The
loopback integration test must validate this prerequisite for local loops and the
remote-disciplined metronome; export compensation cannot repair a broken local
transport mapping.

For the current callback, the physical content phase of an undelayed source sample is:

$$
\begin{aligned}
C_{dac}(i) &\equiv P + i + L_{out} \pmod L \\
C_{adc}(i) &\equiv P + i - L_{in} \pmod L
\end{aligned}
$$

Where:

- $L_{out}$ is output-device latency in samples;
- $L_{in}$ is input-device latency in samples;
- each value comes from `AudioStreamParams` unless that device value is zero, then
  from `UserConfig.Audio.LatencyOut` or `LatencyIn` respectively.

Select causal history from each physical stream using the smallest non-negative delay:

$$
\begin{aligned}
K_{dac} &\equiv L_{out} \pmod L \\
K_{adc} &\equiv -L_{in} \pmod L
\end{aligned}
$$

The ADC expression deliberately represents an apparent advance as a causal delay of
$L - (L_{in} \bmod L)$. It may require nearly one interval of history. All arithmetic
that subtracts latency or computes a modulus must widen to signed 64-bit before the
subtraction; never underflow an unsigned value and then attempt to correct it.

## 5. Jamma-only design

### Data and ownership

Keep all export timing and delay state in `NinjamConnection`, the owner of lane packing
and the single `AudioProc` call. `AudioHost` remains a generic mixer and supplies the
resolved fixed input/output latency through the existing audio-format relay:

```text
AudioHost -> NinjamController -> NinjamSession -> NinjamConnection::SetAudioFormat
```

Extend that format payload with `inputLatencySamps` and `outputLatencySamps`. Format
publication and buffer allocation occur before the stream starts or while it is
stopped. They must never be triggered by lane refresh or by `ProcessExportBlock`.

Store one delay line per **physical** DAC and ADC channel, not per NINJAM lane. Lane
packing changes on the job thread, while physical channel counts are fixed for an
active audio format. This preserves the existing atomic `NinjamLanePacking` snapshot
and avoids tying callback storage lifetime to a published lane-layout change.

Add a small pure `ExportLaneTiming` value helper. It accepts the upstream timing result,
resolved latencies, and generation state; it validates values and returns:

- `Valid`, `NewGeneration`, and `HistoryReady` flags;
- `IntervalLengthSamps` and `EncoderPhaseSamps`;
- `DacDelaySamps` and `AdcDelaySamps`.

It owns no audio buffers, does not call `NJClient`, and has no thread state.

### Fixed capacity and generation state

Define `constants::MaxNinjamIntervalSamps` from an explicit supported operating domain:

$$
\text{ceil}(\text{MaxSampleRate} \times 60 \times \text{MaxBPI} / \text{MinBPM})
$$

Document all three limits beside the constant and choose them before implementation.
Each physical delay line has capacity:

$$\text{MaxNinjamIntervalSamps} + \text{constants::MaxBlockSize}$$

The added block covers `AudioBuffer`'s post-write cursor convention. If the upstream
timing reports an interval longer than this capacity, reject that generation, emit a
non-realtime diagnostic through an atomic counter/flag, and send silence. Never rely
on `AudioBuffer::Delay` clamping an oversized delay because that would silently violate
the phase invariant.

A new generation begins when any of these occurs:

- a valid upstream interval length changes;
- the NINJAM connection is recreated or disconnected;
- the audio format, physical channel count, sample rate, or resolved latency changes;
- an invalid or unsupported upstream timing result is observed after a valid one.

For a new generation, reset all callback-owned delay-line cursors/history and mark both
stream classes unprimed. Send zeroes for an affected lane until it contains sufficient
history for its selected delay. A normal interval wrap with unchanged length is **not**
a new generation: upstream phase wraps naturally and the selected delays remain valid.

`AudioBuffer` presently has no reset operation. Before integration, add a minimal
audio-thread-safe history reset that resets its write/play cursors and recorded-sample
count without resizing or clearing its backing storage, or introduce a dedicated
fixed-capacity delay-line type with the same property. Explicit priming prevents a
new generation from reading stale storage, so an interval-sized callback clear is both
unnecessary and prohibited. Do not use `SetSize`, `std::vector::clear`, allocation, or
an $O(L)$ buffer fill to reset a generation in the callback.

## 6. Callback algorithm

`NinjamConnection::ProcessExportBlock` is the only callback path that changes. Its
order is contractual:

1. Load one `NinjamLanePacking` snapshot. Validate scratch capacity, physical delay
   line counts, callback frame count, and the preallocated output pointers. On a local
   resource failure, return silently without allocating or publishing.
2. Clear the existing NINJAM input scratch lanes for `numFrames`.
3. Write the available interleaved physical DAC and ADC channels into their dedicated
   delay lines using `AudioWriteRequest` with the physical-channel stride, then call
   `EndWrite(numFrames, true)` on every written line. A missing physical input/output
   stream stays silent; do not advance a nonexistent source.
4. Call the supported upstream timing API exactly once, immediately before selecting
   samples and immediately before the sole `AudioProc` call. Validate its full result.
   If it is invalid, unsupported, or the required history is unprimed, retain zeroed
   affected input lanes and still make the one normal `AudioProc` call.
5. Pass the validated timing result to `ExportLaneTiming`. Handle generation changes,
   calculate both delays, and decide whether each stream class is primed.
6. For each ready physical line, call `Delay(K + numFrames)`. `EndWrite` leaves the
   write index just after this callback; therefore `Delay(K)` would select a block too
   late. Use `IsContiguous`/`BlockRead` for the fast path. For a wrapped read, either
   pack the two segments directly or copy the wrapped data into a dedicated,
   preallocated per-physical-channel read scratch; never reuse `_inScratch` while it
   is accumulating mapped lane contributions.
7. Pack selected physical DAC pairs and ADC pairs into `_inScratch` with the current
   direct/modulo mapping. Preserve existing additive behavior when multiple physical
   pairs map to a lane. Do not let a source that is unprimed contribute stale data.
8. Set `_inPtrs` and invoke `NJClient::AudioProc` exactly once with the same
   `numFrames` and sample rate used by the timing call. Never split, probe, or call
   `AudioProc` a second time to infer timing.

The callback must not allocate, resize vectors, take a Jamma lock, publish layout,
log, touch `ExternalTransport`, or read generic NINJAM timing getters. Its only new
NINJAM operation is the supported adjacent timing call.

## 7. MIDI and receive-transport reconciliation

The 2026-07-08 MIDI handoff is related only through the common remote-wrap event. It
does not supply outbound encoder phase and must not be read by export compensation.

The handoff reports a completed design in which `MidiLoop::_loopPhaseAnchor` is atomic,
automation dispatch reads it live, and `LoopTake::RepositionFromAnchor` moves it with
the note reposition delta. The current checkout must be reconciled before relying on
that report: it presently shows a non-atomic `_loopPhaseAnchor` and a cached
`AutomationDispatch::loopPhaseAnchor`, with a separate live anchor correction.

Before this export work lands, resolve that discrepancy in its own focused change:

1. Decide whether the shipped model is the handoff's live atomic anchor or the current
   frozen-anchor plus `MidiAnchorCorrection` model. Do not merge both corrections;
   that would double-shift automation.
2. Run or restore the handoff's two MIDI phase-anchor tests, plus the existing
   `ExternalTransportReanchor` coverage, against the selected model.
3. Keep the zero-sentinel, VST host-time, end-to-end MIDI test, and
   `_midiVisualPlayIndex` cross-thread concerns as separately tracked follow-ups.

The export change's regression responsibility is narrower: prove that it neither
changes receive-side `ExternalTransport` publication nor interferes with the selected
MIDI/automation wrap-reanchor behavior.

## 8. Test plan

### Deterministic native tests

Add localized tests before callback integration:

1. **Absolute phase, DAC and ADC.** With $L = 17$, non-zero $P$, non-zero latencies,
   and source tags equal to source phase, assert every selected tag equals
   $(P+i) \bmod L$ for both stream classes. Also assert the relative equation, but
   never use it as the primary proof.
2. **Post-write cursor and wrap.** Use real fixed delay storage with two small blocks
   and a ring wrap. After `EndWrite`, assert `Delay(K + numFrames)` selects exactly the
   intended historical tag. This catches the one-block indexing error.
3. **Generation and priming.** Feed a fake supported timing provider that changes from
   an exhausted old interval to `(0, L_new)`. Assert new delays, cleared history, zero
   output until enough samples have been written, and no reset for an ordinary wrap of
   the same interval length.
4. **Packing preservation.** Test direct and modulo DAC/ADC pair mappings after delay
   selection, including an absent source and a lane receiving additive contributions.
5. **Unsupported timing.** Invalid phase/length and `L > MaxNinjamIntervalSamps` must
   produce silence, increment the non-realtime rejection diagnostic, and never call an
   oversized delay.

The helper tests use a value provider, not a mocked `NJClient`. A thin integration test
against the supported upstream API belongs at the dependency gate because it validates
the contract Jamma relies on.

### Build and runtime verification

1. Build the changed `JammaLib` project, then `JammaLib_Tests`, using incremental
   Debug x64 builds and the repository's absolute `SolutionDir` convention.
2. Run the focused new native tests, then the full `JammaLib_Tests.exe` suite. Record
   pre-existing failures separately; no new failures are acceptable.
3. Add temporary, rate-limited, non-realtime diagnostics for timing rejection,
   generation start, selected delays, and history-ready transition. Drain them from a
   job/UI context and remove or gate noisy probes once validated.
4. Run a physical DAC-to-ADC loopback against a test NINJAM server. Tag transients at
   known interval phases and prove that DAC and ADC exports arrive at the same NINJAM
   phase over normal wraps and an accepted BPM/BPI change.
5. Confirm unchanged local monitoring, local record/overdub alignment, remote join
   alignment, receive-side `ExternalTransport`, and the selected MIDI automation
   re-anchor behavior.

## 9. Implementation sequence and release gates

1. **Reconcile baseline.** Confirm the MIDI handoff's actual landed state and record
   the selected single correction model. Do not alter upstream NINJAM.
2. **Adopt upstream dependency.** Obtain the supported block-timing contract and its
   released header/library through the normal dependency update path. Complete the
   boundary/BPM/BPI contract probe.
3. **Land pure timing proof.** Add `ExportLaneTiming` and the absolute-phase,
   generation, unsupported-result, and post-write indexing tests.
4. **Prepare fixed storage.** Define supported interval limits, add/reset fixed delay
   storage, and extend the audio-format relay with resolved latencies. Verify there is
   no allocation in the audio callback.
5. **Integrate selection and packing.** Implement the exact callback order in Section
   6, preserving lane packing and one `AudioProc` call.
6. **Prove system behavior.** Run native tests, physical loopback, normal-wrap, and
   BPM/BPI-change validation. Update `doc/ninjam.md` only after those results pass.

### Release blockers

Do not enable compensated export when any of these remains unresolved:

- no upstream-supported block-start timing contract;
- no proof that `outBuf` already represents the remote-disciplined local grid before
  physical output latency;
- unsupported interval length, invalid timing result, or unprimed delay history;
- unresolved MIDI baseline discrepancy that could mask a receive-transport regression;
- missing physical loopback coverage through a timing change.

## 10. Explicitly out of scope

VST host-time/PPQ re-derivation, VST latency compensation, the MIDI zero-anchor
sentinel, and the `_midiVisualPlayIndex` cross-thread review remain separate work. They
must not be folded into this callback change. Likewise, local loop transport correction
and outbound physical latency correction are complementary, not interchangeable.