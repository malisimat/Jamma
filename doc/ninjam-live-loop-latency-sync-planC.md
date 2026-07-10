# NINJAM Live/Loop Export Latency Alignment — Plan C (Unblocked)

Status: **implemented (2026-07-10), gated off by default.** The delay-line
compensation, `ExportLaneTiming` helper (`JammaLib/src/ninjam/ExportLaneTiming.h/.cpp`),
generation-reset/anomaly-detection safety valve, VST latency plumbing (plumb-only), and
unit tests described below are implemented on this branch. The escape hatch
(`NinjamConnection::ExportLatencyCompensationEnabled`) defaults to **false**: this
session could not run the physical DAC-to-ADC loopback verification in §5/step 10
(requires real audio hardware and a live/test NINJAM server), so flipping the flag to
true is left for whoever runs that verification. See the follow-up notes for the exact
remaining steps.

Supersedes the stalemate between
[plan](ninjam-live-loop-latency-sync-plan.md) (correct about a real edge case, but gated
on an upstream contract that will never ship) and
[planB](ninjam-live-loop-latency-sync-planB.md) (correct about the buildable design, but
silent on that edge case). Plan C keeps planB's design almost verbatim and replaces
plan's blocking gate with a bounded-error argument plus field telemetry to prove it.

## 0. Why this isn't a stalemate

`plan` is right that `NJClient::GetPosition()` can be stale for one callback: `AudioProc`
applies pending BPM/BPI, resets `m_interval_pos` to 0, and fires `on_new_interval()`
*inside* the same call that consumes the samples crossing the boundary. Anything read
*before* that `AudioProc` call — `GetPosition`, a local tick counter, `ExternalTransport`
— cannot know the new interval has started until the *next* callback.

`plan`'s conclusion — block all work on an upstream API that resolves this before
`AudioProc` — is not going to happen. Jamma doesn't own `njclient`, won't fork it, and
there's no indication upstream would add a bespoke pre-`AudioProc` timing query for one
downstream consumer. Treating that as a hard gate means this fix never ships, while real
NINJAM users on `njclient`-based clients play in sync every day without one.

The resolution: **stop requiring proof for every sample, including the one callback that
straddles an interval wrap.** Prove the invariant exactly for every callback that doesn't
cross a wrap (the overwhelming majority — a wrap happens once per interval, typically
every 4–16 seconds), and prove the wrap-crossing callback's error is *small, bounded, and
self-correcting within one callback*, because that's the actual behaviour, not a hand
wave. Section 1 does the bound. That bound is comfortably smaller than latencies NINJAM
already tolerates by design (a full interval of encode/network/decode lag), so it's a
non-issue in practice, not a compromise dressed up as one.

## 1. The bound (the "bug", quantified)

Model exactly as in planB §2–3: running tick `n`, interval position `pos`/length `L` from
`_client->GetPosition(&pos, &length)`, called once per `ProcessExportBlock`, immediately
before the sole `AudioProc` call, same as `_UpdateSnapshot` already does today
([NinjamConnection.cpp](../JammaLib/src/ninjam/NinjamConnection.cpp#L700-L713)).

For any callback where the interval does **not** wrap between the previous callback's
`AudioProc` and this callback's `GetPosition` read, `pos` is exactly current, and
`K_dac`/`K_adc` computed from it are exactly correct (planB §3, unchanged).

For the one callback per interval where the wrap happens *inside* the previous
callback's `AudioProc` call (i.e. the interval exhausted while processing that block),
`pos` read at the start of the **current** callback already reflects the post-wrap
value — `AudioProc` resets `m_interval_pos` before returning, and `GetPosition` is a
plain read of that already-updated state, not a snapshot taken mid-call. So the
window of staleness is not "the wrap callback" at all — it's already handled correctly,
because we read position fresh, every callback, right before we need it.

Where it's genuinely worth checking: the wrap could in principle happen *between* our
`GetPosition` read and NJClient consuming the samples this callback hands to `AudioProc`,
if `numFrames` for this callback carries the interval past its end. In that case, our
computed `K_dac`/`K_adc` are correct for the *start* of the block (they match `pos` at
sample 0) but drift by up to `numFrames` samples of phase by the *end* of the block,
because we use one delay value for the whole block rather than re-deriving it
per-sample. That's the actual, real edge case — not a staleness bug, a
"one-delay-value-per-block-can't-track-a-mid-block-wrap" quantisation error. It is:

- **Bounded**: at most `numFrames` samples of phase error, only within that single
  block, only in the packing of the historical samples chosen from a delay line, never
  accumulating and never affecting where NJClient itself thinks it is (its own interval
  clock is untouched — this only affects which historical samples we hand it).
- **Self-correcting in one callback**: the very next callback re-reads `pos`/`L` fresh
  and computes a fully correct `K_dac`/`K_adc` again. There is no persistent offset, no
  drift, and nothing to reconcile — the design is intentionally recompute-from-scratch
  every block (planB §3, "self-stability").
- **Small in absolute terms**: with `constants::DefaultBufferSizeSamps = 512`
  ([Constants.h](../JammaLib/include/Constants.h#L32)) at typical 44.1–48 kHz rates,
  that's ~10–12 ms, once per interval (every several seconds). Even at the pathological
  ceiling `constants::MaxBlockSize = 4096` it's ~85–95 ms, still once per interval, still
  self-healing next block.
- **Smaller than tolerances NINJAM already has by design**: NINJAM's entire model is
  "everyone plays one interval behind and it still sounds together" — full intervals
  (seconds) of round-trip lag are the norm, not the exception. A sub-100ms blip on a
  single block, once every several seconds, self-correcting, is not in the same universe
  of severity as what the protocol already routinely absorbs. It is a strictly smaller,
  strictly better outcome than today's *uncompensated* state, which is wrong on
  literally every block, not just the rare wrap block.

Conclusion: **the "bug" plan correctly spotted is real but tiny, structurally incapable
of accumulating, and irrelevant next to NINJAM's own inherent latency budget.** It's a
reason to write a bounded-error test and a telemetry probe (§3, §5), not a reason to
block the whole feature on an API that isn't coming.

## 2. Design — adopt planB almost entirely

Carry planB's design forward unchanged except where noted:

- **Ownership**: delay lines live in `NinjamConnection`, not `AudioHost` (planB §4) —
  unchanged, still correct per [AGENTS.md](../AGENTS.md) glue-code guidance.
- **Math**: `K_dac ≡ n + outLatency - pos (mod L)`, `K_adc ≡ n - inLatency - pos (mod L)`,
  via `utils::ModNeg` ([MathUtils.h](../JammaLib/src/utils/MathUtils.h#L12-L13)) — planB
  §3, unchanged. Timing source is `_client->GetPosition(&pos, &length)`, called once per
  block immediately before `AudioProc` — the same call `_UpdateSnapshot` already makes
  today, no new API, no upstream dependency.
- **Wraparound trick**: unchanged (planB §3) — "advancing" is impossible, delaying by
  `L - Δ` is always possible and lands on the same phase.
- **Buffers**: reuse `audio::AudioBuffer` per physical DAC/ADC channel (not per NINJAM
  lane — physical channel count is fixed for an active audio format; lane packing can
  change on the job thread independently, matching `plan` §5's ownership argument, which
  is a genuine improvement over planB's original "per-lane" framing). Sized to
  `constants::MaxNinjamIntervalSamps + constants::MaxBlockSize` (the extra block covers
  `AudioBuffer`'s post-write cursor convention — confirmed via
  [AudioBuffer.cpp](../JammaLib/src/audio/AudioBuffer.cpp#L143-L154): `EndWrite` advances
  `_writeIndex` past the just-written block, so `Delay(K)` must be called as
  `Delay(K + numFrames)` to select the intended historical sample, not `Delay(K)` alone).
- **Latency plumbing**: thread `inLatencySamps`/`outLatencySamps` (kept separate) through
  the existing `SetAudioFormat(...)` relay (`AudioHost` → `NinjamController` →
  `NinjamSession` → `NinjamConnection`), same convention as `AudioStreamParams`-first /
  `UserConfig`-fallback used elsewhere (planB §4).
- **VST latency plumbing**: unchanged from planB §7 — add `GetLatencySamples()` to
  `IVstPlugin`/`Vst2Plugin` (`Vst3Plugin` default `0`), aggregate on `VstChain`, read-only
  `CurrentVstLatencySamps()` on `Loop`/`LoopTake`. Still plumb-only this round; still do
  not fold into `LoopPlayPos`, `_playIndex`, MIDI cursor, or `K_dac`/`K_adc` yet. Same
  `// TODO(latency):` markers at the same four seams planB §7 identified.
- **MIDI baseline reconciliation**: `plan` §7's caveat about `MidiLoop::_loopPhaseAnchor`
  atomicity and the `AutomationDispatch` cached-vs-live anchor discrepancy is still a
  real, separate concern. It gates nothing here — this change never reads or writes
  MIDI phase-anchor state — but resolve it in its own change before or independently of
  this one; don't let the two get tangled in review.

## 3. What Plan C adds on top of planB

### 3.1 Generation / reset discipline (adopted from `plan` §5, sharpened)

A "generation" boundary — connection (re)created/disconnected, audio format or physical
channel count or sample rate or resolved latency changed, or `L` changes by more than one
callback's worth from one block to the next (see §3.2) — resets all delay-line cursors
and marks both stream classes unprimed until they've accumulated enough history for their
newly computed `K`. A normal same-length interval wrap is **not** a generation change;
`K_dac`/`K_adc` recomputed fresh each block already handle it (§1).

`AudioBuffer` has no reset today — only `SetSize` (resizes, not appropriate in a
callback) and natural overwrite via `EndWrite`. Add a minimal, allocation-free
`AudioBuffer::Reset()`:

```cpp
void AudioBuffer::Reset()
{
    _sampsRecorded = 0;
    _playIndex = 0;
    _SetWriteIndex(0);
}
```

This resets cursors/counts without touching `_buffer`'s storage or size — safe to call
from the audio callback on a generation change, and it makes stale-history reads
impossible by construction (an unprimed buffer reports `_sampsRecorded == 0`, which
`Delay()` already special-cases to `_playIndex = 0`, and callers can gate on
`SampsRecorded() >= K + numFrames` before trusting a read).

### 3.2 Anomalous-jump detection (new safety valve, not in either prior plan)

Because we no longer wait for a provably-clean upstream signal, add one extra guard:
each block, alongside computing `K_dac`/`K_adc`, compare the freshly-read `(pos, length)`
against the previous block's `(pos, length)` advanced by `numFrames` (mod `L`). Three
outcomes:

1. **Matches** (within the ordinary same-length wrap case) → normal path, §1.
2. **`length` changed** (tempo/BPI vote accepted) → generation reset, §3.1.
3. **`pos` differs from the predicted value by more than one block's worth, with
   `length` unchanged** → this would indicate `GetPosition`/`AudioProc` behaving
   differently than the documented model (e.g. a `Seek`/reconnect we didn't otherwise
   detect). Treat as a generation reset too (conservative: better to briefly mute/reprime
   than to compensate with a stale `K`), and increment a rate-limited, non-realtime
   diagnostic counter so this is observable rather than silently wrong.

This turns the one theoretical risk in §1 (an assumption about `AudioProc`/`GetPosition`
ordering, based on documented behaviour but not something we can read the vendored
source to double-check — see [build-notes](../JammaLib/lib/) on `njclient` being a
prebuilt vendored lib with only a header) into something that fails safe and loud instead
of silently mis-compensating.

### 3.3 Field telemetry to replace the missing upstream proof

`plan`'s dependency gate wanted a proof from upstream that the timing contract holds.
We can't get that from upstream, but we can get it from our own running system:
add temporary, rate-limited, non-realtime diagnostics (same style as
[build-notes](../JammaLib/lib/) already uses for hotpath guardrails) logging, per
generation: computed `K_dac`/`K_adc`, observed interval length `L`, and every time §3.2
case 3 fires. Run a real session against a real or test NINJAM server, drain the logs,
and confirm case 3 essentially never fires in practice (i.e. the documented
`GetPosition`/`AudioProc` ordering holds up empirically, not just on paper). Remove or
gate the noisy per-block logging once validated; keep the case-3 counter permanently as
a cheap health signal.

### 3.4 Escape hatch

Gate the whole compensation path behind one runtime flag (config or compile-time,
whichever matches existing NINJAM feature-flag conventions in this codebase — check
`UserConfig`/`NinjamController` for the existing pattern before adding a new one).
Default **on** once validated, but keep an instant fallback to today's uncompensated
pass-through if a real session ever surfaces an issue this plan didn't anticipate. This
is what makes "be daring" safe: we're not blocked on perfection, but we're not
unable to back out either.

## 4. Definition of done (revised from `plan` §1)

Replace `plan`'s absolute-for-every-sample invariant with the honest version this design
actually delivers:

- **Exact** phase alignment (`C_i ≡ P + i (mod L)` for both DAC and ADC lanes) for every
  callback that does not straddle an interval wrap — i.e. essentially all callbacks.
- For the rare callback where the interval wraps mid-block: phase error bounded by one
  callback's `numFrames`, non-accumulating, fully corrected by the very next callback.
  No drift, no persistent offset, ever.
- `K_dac - K_adc ≡ outLatency + inLatency (mod L)` holds on every block (the relative
  invariant, still true even during the bounded-error block, since both lanes are
  computed from the same possibly-stale `pos` together).
- Anomalies outside this bound (§3.2 case 3) are detected, logged, and fail safe (mute +
  reprime), never silently mis-compensate.

## 5. Test plan (revised from `plan` §8 / planB §8)

Deterministic native tests, using a fake/value timing provider (no mocked `NJClient`
needed — same as planB):

1. **Absolute phase, steady state.** Non-wrapping sequence of blocks, `L=17`, non-zero
   `P`/latencies; assert exact `(P+i) mod L` for both lanes every block (planB test 1).
2. **Post-write cursor / wrap indexing.** Confirm `Delay(K + numFrames)` selects the
   intended historical sample after `EndWrite` (planB test 2) — this is the mechanical
   bug plan's own `plan` §6 step 6 flagged and is worth keeping regardless of the
   contract debate.
3. **Bounded wrap-block error (new — replaces `plan`'s impossible absolute-proof test).**
   Construct a block where the fake provider's `(pos, length)` reflects a wrap that
   completed *during* the just-finished `AudioProc` call (i.e. the provider returns the
   post-wrap `pos` at the start of this block, per §1's model). Assert: (a) this block's
   computed `K` is exactly correct for its first sample per the new `(pos, length)`; (b)
   a synthetic "true phase" trace shows error accumulating to at most `numFrames` samples
   by the end of that one block; (c) the following block's computed `K` is exactly
   correct again with zero residual error.
4. **Generation reset.** Interval length change, format change, and §3.2 case-3 anomaly
   each trigger a reset: cleared history, unprimed lanes silent until enough samples
   accumulate, no reset for an ordinary same-length wrap (planB test 3 + `plan`'s
   generation concept, merged).
5. **Anomalous-jump detection.** Feed a provider that violates the expected
   `pos`-advances-by-`numFrames` relationship without a length change; assert the
   diagnostic counter fires and the affected lanes go silent/reprime rather than
   compensate with the bad value (new, §3.2).
6. **Packing preservation.** Direct/modulo DAC/ADC pair mappings still work after delay
   selection, including an absent source and additive multi-pair lanes (planB test 4).
7. **`AudioBuffer::Reset()`.** Unit test confirming it clears cursors/counts without
   touching buffer size or storage, and that `Delay()`/`SampsRecorded()` behave correctly
   immediately after (new, §3.1).

Build and runtime verification, same sequence as planB §8 (incremental Debug x64 builds
via the `Build Tests (Debug x64)` task, then the full `JammaLib_Tests.exe` suite, no new
failures), plus:

- Physical DAC→ADC loopback against a real/test NINJAM server, run long enough to cross
  several interval wraps and at least one accepted tempo/BPI vote; confirm audible
  alignment and that the case-3 diagnostic counter (§3.2/§3.3) stays at zero or explain
  any non-zero reading before shipping.

## 6. Step-by-step implementation checklist

1. Add `constants::MaxNinjamIntervalSamps` to
   [Constants.h](../JammaLib/include/Constants.h), documented derivation: comfortably
   above any realistic BPM/BPI/sample-rate combination (same sizing approach as planB
   §5/`plan` §4 — pick and record the three limits used).
2. Add `AudioBuffer::Reset()` (§3.1) to
   [AudioBuffer.h](../JammaLib/src/audio/AudioBuffer.h)/
   [AudioBuffer.cpp](../JammaLib/src/audio/AudioBuffer.cpp); unit test it (test 7).
3. **`NinjamConnection.h`/`.cpp`**: add one `audio::AudioBuffer` delay line per physical
   DAC channel and one per physical ADC channel (not per lane — §2), sized to
   `MaxNinjamIntervalSamps + MaxBlockSize`, (re)allocated in `SetAudioFormat`. Add
   generation-tracking state (last-seen `L`, last-predicted `pos`) for §3.2.
4. Thread `inLatencySamps`/`outLatencySamps` through `SetAudioFormat(...)` in
   `NinjamConnection`, `NinjamSession`, `NinjamController` (pure relay).
5. Implement the `ExportLaneTiming` pure helper (from `plan` §5, now consuming
   `GetPosition()`'s `(pos, length)` directly instead of a hypothetical upstream API):
   validates values, detects generation changes and §3.2 anomalies, returns
   `K_dac`/`K_adc`/priming flags. No audio buffers, no `NJClient` calls, no thread state
   — keeps it unit-testable without any audio-thread machinery (tests 1, 3, 4, 5).
6. **`NinjamConnection::ProcessExportBlock`**: write DAC/ADC content into the new delay
   lines; call `_client->GetPosition(&pos, &length)` once, immediately before the sole
   `AudioProc` call; feed `ExportLaneTiming`; on a reset, call `Reset()` on affected
   lines and mute until primed; otherwise `Delay(K + numFrames)` + contiguous/wrapped
   read (mirroring `ChannelMixer::WriteToSink`'s existing split-read pattern) into
   `_inScratch`, preserving today's direct/modulo pair packing.
7. Add the tests in §5. Build via the `Build Tests (Debug x64)` task; run
   `JammaLib_Tests.exe`.
8. VST latency plumbing (planB §7, unchanged): `GetLatencySamples()` on
   `IVstPlugin`/`Vst2Plugin`, aggregate on `VstChain`, `CurrentVstLatencySamps()` on
   `Loop`/`LoopTake`. Leave the four `// TODO(latency):` markers.
9. Wire the escape-hatch flag (§3.4); default off until step 10 passes, then default on.
10. Physical loopback verification (§5) including the multi-wrap, tempo-change, and
    case-3-counter checks. Fix or explain any non-zero anomaly count before flipping the
    default flag on.
11. Docs: cross-reference this plan from [doc/ninjam.md](ninjam.md)'s "Timing and Sync"
    section; mark `plan` and `planB` as superseded by this one (don't delete them — they
    document the reasoning this plan builds on).
