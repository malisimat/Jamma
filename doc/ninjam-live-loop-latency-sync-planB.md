# NINJAM Live/Loop Audio Latency Alignment — Plan

Status: **finalised plan, not yet implemented.** Implement in a later session per the
checklist in §9.

## 1. Problem

`NinjamConnection::ProcessExportBlock` (called once per audio callback from
[AudioHost.cpp](../JammaLib/src/audio/AudioHost.cpp#L196-L206)) packs **two** kinds of
audio into separate NINJAM send lanes, using the *same* callback's data, with no
relative delay between them ([NinjamConnection.cpp](../JammaLib/src/ninjam/NinjamConnection.cpp#L427-L520)):

- **DAC-lane** ("loop" audio): read straight from `outBuf`, i.e. the station/loop mix
  that was *just written* to the DAC this callback.
- **ADC-lane** ("live" audio): read straight from the raw `inBuf`, i.e. the mic/line
  input *just captured* by hardware this callback.

Both are fed into `NJClient::AudioProc` in lockstep, so from a remote listener's
perspective they are currently sent as if they happened at the same instant. They don't,
and the fix must also keep both lanes sample-accurately phase-locked to NINJAM's own
interval clock (not just consistent with each other), otherwise the loop content — which
is currently confirmed to sound in sync with the remote metronome — could end up
audibly offset from the beat grid.

## 2. Model

Define `CONTENT_TRUE_TIME(x)` = the real-world/grid instant that buffer sample `x`
musically represents. Let `n` be a running sample-tick counter (increments by
`numFrames` every callback). Using `AudioStreamParams.InputLatency`/`OutputLatency`
(see [AudioDevice.h](../JammaLib/src/audio/AudioDevice.h#L17-L23)):

- `outBuf[n]` physically reaches the speaker at real time `n + outLatency` (definition
  of output latency), so `CONTENT_TRUE_TIME(outBuf[n]) = n + outLatency`.
- `inBuf[n]` was captured by the mic at real time `n - inLatency` (definition of input
  latency); assuming near-zero performer reaction lag (an unavoidable modelling
  assumption for any latency-alignment scheme, not specific to this design), that's
  whatever grid content was audible at that instant, so
  `CONTENT_TRUE_TIME(inBuf[n]) = n - inLatency`.

`NJClient`'s own encoder tracks its **own live position** within the current interval —
`NinjamConnection` already queries this today via `_client->GetPosition(&pos, &length)`
in `_UpdateSnapshot` ([NinjamConnection.cpp](../JammaLib/src/ninjam/NinjamConnection.cpp#L700-L713)).
Per `njclient.h`'s documented threading contract, `AudioProc`/`GetPosition` need no
external locking — safe to call every block, including from the audio thread. `pos` is
the ground truth for "where NJClient itself currently is" in its own interval — the
correct target for both lanes' phase, rather than an assumption about what "sounds
right" today.

## 3. The math — dual delay via interval wraparound

We want content fed to `AudioProc` at tick `n`, for **either** lane, to land at the same
phase NJClient itself is currently at:

```
CONTENT_TRUE_TIME(content fed at tick n) ≡ pos   (mod L)
```

**DAC-lane**, feeding `outBuf[n - K_dac]`:
```
(n - K_dac) + outLatency ≡ pos (mod L)  ⇒  K_dac ≡ n + outLatency - pos   (mod L)
```

**ADC-lane**, feeding `inBuf[n - K_adc]`:
```
(n - K_adc) - inLatency ≡ pos (mod L)  ⇒  K_adc ≡ n - inLatency - pos   (mod L)
```

Both `K_dac` and `K_adc` are taken as the smallest **non-negative** residue mod `L`
(`utils::ModNeg`, already in [MathUtils.h](../JammaLib/src/utils/MathUtils.h#L12-L13),
does exactly this). Their difference is fixed regardless of `pos`:

```
K_dac - K_adc ≡ outLatency + inLatency   (mod L)
```

— the same relative correction needed to keep the two lanes mutually consistent, now
anchored to NJClient's own observed position instead of an assumption about the
DAC-lane being untouchable.

**Why no time travel is needed:** naively, aligning the ADC-lane to a target phase
*ahead* of its raw content would require reading mic samples that haven't been captured
yet — impossible. But the interval clock is periodic with period `L` (typically several
seconds — far larger than any hardware latency). Advancing by `Δ` samples (impossible)
is equivalent, modulo `L`, to **delaying by `L - Δ`** samples (always possible, purely
historical data). Delaying by a whole extra loop of the interval lands on the exact same
intra-interval phase, which is all that matters — neither NJClient's encoder nor the
remote decoder cares which absolute interval cycle a sample is nominally attached to,
only where in the beat-grid it falls. This is the "mimicked time travel."

**Self-stability:** `n` and `pos` both advance by `numFrames` every block, so
`K_dac`/`K_adc` computed fresh each block are provably constant from block to block
(the same values every time) as long as the underlying tempo/interval-length hasn't
changed. If NJClient's own timing rebases (tempo/BPI change, reconnect), the very next
block's computation adopts the new relationship automatically — no explicit
change-detection or recalibration step is needed; recompute unconditionally every
block (cheap: one `GetPosition()` call plus integer arithmetic, no allocation, no
locking).

This supersedes any approach that assumes the DAC-lane's current calibration must stay
fixed — both lanes are now derived symmetrically from the same observed ground truth,
so whichever lane needs the larger wraparound delay gets it, without any lane being
treated as sacrosanct.

## 4. Design — where the delay lines live

The delay lines live inside `NinjamConnection`, not `AudioHost`:

- `AudioHost`'s remit is the RtAudio callback, generic ADC/DAC ring-buffer plumbing
  shared by all stations (`ChannelMixer`), and station audio fan-in/fan-out. It hands
  `NinjamController` two plain interleaved buffers (`outBuf`, `inBuf`) per callback and
  has no notion of NINJAM lanes, `NJClient`, or interval position.
- `NinjamConnection` already owns all NINJAM-protocol-specific knowledge — lane packing
  (`DacPairs`/`AdcPairs`/`Modulo`), scratch buffers sized to lane counts, and the
  `AudioProc`/`GetPosition` calls. The phase-alignment problem only exists because of
  how `NJClient` bundles these lanes together, so it belongs here, colocated with the
  packing code it cooperates with (per [AGENTS.md](../AGENTS.md): "keep glue code thin
  and explicit, avoid cross-subsystem coupling").

Implementation:

- Two new sets of per-channel `audio::AudioBuffer` delay lines inside
  `NinjamConnection`: one for DAC-lane channels (`packing.DacPairs*2`), one for
  ADC-lane channels (`packing.AdcPairs*2`) — reusing `audio::AudioBuffer`, the same
  ring-buffer/delay-line primitive `ChannelMixer` already uses for its ADC monitor
  delay ([ChannelMixer.cpp](../JammaLib/src/audio/ChannelMixer.cpp#L23-L36)).
- A running tick counter `_lanePos` (member on `NinjamConnection`), incremented by
  `numFrames` every `ProcessExportBlock` call, reset to `0` whenever `SetAudioFormat`
  (re)initialises the connection (matches the buffers' own write cursors, which also
  restart at that point).
- Each `ProcessExportBlock` call:
  1. Write `interleavedDacOutput` into the DAC-lane delay buffers and
     `interleavedAdcInput` into the ADC-lane delay buffers (same
     `OnBlockWrite`/`EndWrite` pattern `ChannelMixer` already uses).
  2. `_client->GetPosition(&pos, &length)`.
  3. If `length > 0`: compute `K_dac`, `K_adc` from §3 via `utils::ModNeg`; call
     `buf.Delay(K)` on each respective buffer; read back via
     `BlockRead`/`IsContiguous`, handling wraparound with a split read exactly as
     `ChannelMixer::WriteToSink` already does.
  4. Pack the delayed reads into `_inScratch` for both the DAC-pair and ADC-pair
     loops (replacing today's direct `interleavedDacOutput[...]`/
     `interleavedAdcInput[...]` reads in both loops).
  5. If `length == 0` (timing not yet established, e.g. just connected): pass through
     undelayed (`K_dac = K_adc = 0`) until valid timing is available, rather than using
     a bogus `L`.
- Thread `inLatencySamps` and `outLatencySamps` (kept **separate**, not pre-combined —
  each is used in a different formula) through the existing `SetAudioFormat(...)` relay
  chain (`AudioHost` → `NinjamController` → `NinjamSession` → `NinjamConnection`, all
  four already share this signature). `AudioHost::Init` resolves both using the
  existing `AudioStreamParams`-first / `UserConfig`-fallback convention already used
  for `inLatency` there and for `outLatency` in
  [Station.cpp](../JammaLib/src/engine/Station.cpp#L958-L966).

## 5. Buffer sizing and tempo changes

- `AudioBuffer::Delay(K)` silently **clamps** `K` to the buffer size if exceeded
  ([AudioBuffer.cpp](../JammaLib/src/audio/AudioBuffer.cpp#L143-L154)) — an undersized
  buffer produces a silently-wrong delay, no error. Buffers must therefore be sized
  `>= L` for any `L` expected to occur.
- `L` (interval length) depends on session BPM/BPI and can change at runtime (tempo
  votes via `TimingQuantiser::ApplyAcceptedRemoteTempo`). Since buffers must be
  pre-allocated (no allocation in the audio hot path, per
  [AGENTS.md](../AGENTS.md)), size them once in `SetAudioFormat` to a new generous fixed
  cap — e.g. `constants::MaxNinjamIntervalSamps`, sized comfortably above any realistic
  BPM/BPI/sample-rate combination (tens of seconds at 96 kHz; a few MB per channel,
  same order of magnitude as `constants::MaxLoopBufferSize`).
- If a session's actual `L` ever exceeds that cap (a pathological BPI/BPM/sample-rate
  combination), the delay silently clamps to the cap — the same degraded-but-non-crashing
  behaviour `AudioBuffer::Delay` already has elsewhere. Log a warning when
  `L > MaxNinjamIntervalSamps` is observed.
- No explicit tempo-change handling is needed beyond this: `K_dac`/`K_adc` are
  recomputed every block directly from the freshly-read `pos`/`L` (§3), so a tempo
  change is picked up on the very next block automatically.

## 6. Relationship to existing loop-position mechanisms

- `Station.cpp`'s `LoopPlayPos(errorSamps, loopLength, outLatency)`
  ([Station.cpp](../JammaLib/src/engine/Station.cpp#L1051)) pre-advances a loop's own
  playback cursor by `outLatency` samples so **local** monitoring/output lands on-beat
  after hardware output buffering. This governs what the local performer hears from
  their own speakers; this plan's delay lines operate purely on a copy of the audio
  taken for NINJAM transmission, downstream of that cursor logic, and never modify it.
  **Still required, unchanged.**
- `timing::ExternalTransport`/`TimingQuantiser` (`DisciplineRemotePhase`,
  `GlobalPhaseOffsetSamps`) discipline the **local** loop-playback clock's phase to the
  remote session's interval position, for local playback/visual coherence
  (receive-side). This plan is entirely send-side (what leaves the local machine via
  NINJAM) and doesn't touch or duplicate that mechanism. **Still required, unchanged.**
- Nothing existing becomes redundant — this plan adds a new, self-contained correction
  layer purely inside `NinjamConnection`'s send path.

## 7. VST latency — plumb the read-path now, defer compensation

Only **loop-driven** VST latency (audio-loop insert FX, and MIDI-loop → VSTi
rendering) should ever be compensated for, by adjusting playback position — that
position-adjustment work is out of scope for this session. **Live** VST paths (a user
playing a VSTi live, or live-inserted FX, if/when supported) must instead be kept at
minimal latency at the source — compensating live latency is impossible without adding
monitoring lag, so the correct fix there is to avoid/bypass plugin lookahead/buffering
for live paths entirely, never to "compensate" it. This is a constraint for future
work, not actionable now.

For this session: plumb in the ability to read a plugin's reported latency and wire it
up for observability; do not change any playback-position or delay math yet.

- Add `virtual unsigned int GetLatencySamples() const noexcept { return 0; }` to
  `IVstPlugin`; implement in `Vst2Plugin` as `_effect ? _effect->initialDelay : 0`
  (real-time safe, single field read). `Vst3Plugin` keeps the default (0) until it has
  its own accessor — no VST3 latency API work in this session.
  Update `Vst2Plugin.cpp`/`Vst3Plugin.cpp`/`IVstPlugin.h`.
- Add an aggregate query on `vst::VstChain` (e.g. `GetLatencySamples()` = sum, since a
  chain of inserts each add their own delay) so callers don't need to walk the chain
  themselves.
- `Loop` and `LoopTake` each already own a `VstChain` (`_vstChain`, published via the
  existing atomic-swap pattern — see [Loop.h](../JammaLib/src/engine/Loop.h#L301) /
  [LoopTake.h](../JammaLib/src/engine/LoopTake.h#L367)). Add a read-only accessor on
  each (e.g. `CurrentVstLatencySamps()`) that loads the published chain and queries it —
  no new cross-thread state needed; it's a pure derived read of state that's already
  published.
- **Do not** feed this value into `UserConfig::LoopPlayPos`/`OverdubPlayPos`, `Loop`'s
  ongoing `_playIndex` advance, any MIDI-loop cursor, or `NinjamConnection`'s
  `K_dac`/`K_adc` math yet. Leave `// TODO(latency):` comments referencing this doc at:
  - [Station.cpp](../JammaLib/src/engine/Station.cpp#L1051) — the `LoopPlayPos(...,
    outLatency)` call site: future work folds `+ take->CurrentVstLatencySamps()` (or
    the loop's own) into the `outLatency` passed in here.
  - [Loop.cpp](../JammaLib/src/engine/Loop.cpp#L444) — the `_playIndex` advance /
    initial-seed path: future work generalises the same style of index
    pre-compensation to include VST latency.
  - The MIDI-loop playback cursor (`LoopTake`'s MIDI play/seek path) — same
    compensation, MIDI-side.
  - [NinjamConnection.cpp](../JammaLib/src/ninjam/NinjamConnection.cpp) `ProcessExportBlock`
    — future work may need a *per-station* (not just global) adjustment when a
    ninjam-bound station's loop content passes through a mix of VST and non-VST
    sources; a harder, separate problem, not to be solved by simply folding into the
    global `outLatency` term.
- Rationale for plumbing now: once `GetLatencySamples()` exists and is observable (e.g.
  logged when a VST is loaded), a future session can decide the right compensation
  strategy with real numbers in hand, without having to first invent the read-path.

## 8. Verification

- **Impulse-alignment test**: feed `NinjamConnection::ProcessExportBlock` a known
  impulse on the DAC side and a matching impulse on the ADC side, with a mocked/stubbed
  `GetPosition()` returning controlled `(pos, length)`; assert the two land at the
  computed relative offset (`K_dac - K_adc ≡ outLatency + inLatency (mod L)`).
- **Phase-lock stability test**: drive a sequence of blocks with `GetPosition()`
  advancing in lockstep with `numFrames`; assert `K_dac`/`K_adc` are stable
  (unchanging) block to block.
- **Wraparound test**: construct a scenario where a naive "advance" would be required
  (target phase ahead of raw content); assert the computed `K` correctly wraps to
  `L - Δ` rather than going negative or wrong.
- **Practical audible check**: physical DAC→ADC loopback, connected to a real/test
  NINJAM server; confirm the two lanes line up sample-for-sample and that the result
  still sounds on-beat against the remote metronome. This depends on the
  near-zero-reaction-lag modelling assumption (§2) and on `NJClient` behaving as
  `GetPosition()` reports, so it's worth confirming in practice even though the math is
  now exact rather than heuristic.
- Rebuild `JammaLib` + `JammaLib_Tests` per [doc/build.md](build.md) after the change.

## 9. Step-by-step implementation checklist

1. Add `constants::MaxNinjamIntervalSamps` to [Constants.h](../JammaLib/include/Constants.h),
   sized generously (document the derivation — comfortably above any realistic
   BPM/BPI/sample-rate combination).
2. **`NinjamConnection.h`/`.cpp`**: add per-channel `audio::AudioBuffer` delay lines for
   *both* DAC-lane and ADC-lane channels, sized to the new constant, (re)allocated in
   `_RefreshLanePacking`/`SetAudioFormat`. Add the `_lanePos` running tick counter,
   reset in `SetAudioFormat`, incremented by `numFrames` in `ProcessExportBlock`.
3. Thread `inLatencySamps` and `outLatencySamps` (separate values) through
   `SetAudioFormat(...)` in `NinjamConnection`, `NinjamSession`, and `NinjamController`
   (pure relay, mirrors existing `numInputChannels`/`numOutputChannels` plumbing).
4. **`NinjamConnection::ProcessExportBlock`**: write DAC/ADC content into their new
   delay lines; call `_client->GetPosition(&pos, &length)`; if `length > 0` compute
   `K_dac`/`K_adc` via `utils::ModNeg` (§3); `Delay(...)` + wraparound-safe read back
   (mirroring `ChannelMixer::WriteToSink`) for both the DAC-pair and ADC-pair packing
   loops. If `length == 0`, pass through undelayed.
5. **`AudioHost::Init`**: resolve `inLatency`/`outLatency` (existing fallback
   convention) and pass both into `_ninjamController->SetAudioFormat(...)`.
6. Add the three native tests from §8. Build via `Build Tests (Debug x64)` task, run
   `JammaLib_Tests.exe`.
7. **VST latency plumbing** (§7): add `GetLatencySamples()` to `IVstPlugin`/
   `Vst2Plugin` (`Vst3Plugin` keeps the `0` default); add aggregate accessor on
   `VstChain`; add read-only `CurrentVstLatencySamps()` accessors on `Loop` and
   `LoopTake`. No behavioural change — pure new read-path. Leave the `TODO(latency)`
   markers listed in §7.
8. **Manual verification**: physical DAC→ADC loopback test against a real/test NINJAM
   server per §8; confirm local monitoring, local loop-to-metronome sync, and
   recording/overdub alignment are all subjectively unchanged (§6 — untouched by
   construction), and that the loop lane still sounds on-beat to a remote listener.
9. **Docs**: once implemented, add a short cross-reference from
   [doc/ninjam.md](ninjam.md) to this plan doc under its "Timing and Sync" section.
