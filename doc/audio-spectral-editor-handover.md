# Audio spectral editing

The spectral editor is a non-destructive view of an audio Loop. Loop retains immutable source material, authored edit state and published playback; spectral owns analysis, editing and reconstruction. Persistence remains deferred.

## Ownership and structure

- `JammaLib/src/spectral`: AudioSpectral source capture, resource admission, FFT/DSP, gain tiles, broadband envelope, edit/playback snapshots, worker, controller, display job and picking. All use namespace `spectral`.
- `JammaLib/src/graphics/AudioSpectralModel.*`: fixed geometry, immutable display metadata, texture upload and last successfully drawn picking surface. Rendering types remain in `graphics`.
- `JammaLib/src/engine/Loop.*`: immutable source/edit/playback publication and audio snapshot borrowing.
- `JammaLib/src/midi/LoopGridEditor.*`: shared editor lifecycle, input, camera framing and UI wiring. Engaged MIDI and spectral editors hide station controls.

## Editing and navigation

Ordinary left/right brushes raise/cut normalized magnitude at 60 dB/s with physical time/octave support. Authored silence is recoverable; originally absent coefficients remain absent. Positive ordinary exposure under an Alt mask captures the composed baseline into owned coefficient tiles, then raises it without reopening unrelated frequencies. Owned coefficients reconstruct after the sample-domain envelope with original phase. Undo/cancel restore immutable before-state.

Alt-left captures an envelope baseline and sweeps a relative time band. Vertical displacement changes the target; horizontal motion sweeps the same target over time. Neutral restores the visited region against the captured baseline. Winning influence and local ramps preserve subdivision and prior-silence invariants.

Shift-right zooms time horizontally and frequency vertically around the click. Shift-left pans both axes at unchanged spans. Plain wheel resizes the brush; Alt-wheel resizes the time band; Shift-wheel zooms frequency and Alt-Shift-wheel zooms time. Ctrl is reserved. Navigation scales/clips the canonical mesh without scheduling analysis or display rebuilding.

## Scheduling and real-time rules

The worker has independent audio and visual lanes. Source preparation, analysis and coalesced playback reconstruction run on the audio lane; authoritative editing and presentation alternate on the visual lane. Authoring has a 60 Hz scheduling interval; audio requests are coalesced at 30 Hz with a final release flush. Queue mutexes, allocation, FFT work and teardown joins remain off audio callbacks.

Frozen input maps stay worker-owned until terminal release/acquire publication. Only queued edits receive terminal cancellation callbacks. Gain copies share COW tiles. The private visual cache retains heights and thin identity metadata, rather than old source/edit reservations. Dirty projection requires matching baseline and destination revisions; unknown/skipped baselines or restored edits take the full path. GPU partial upload requires matching uploaded provenance and a trusted completed job. Failed uploads invalidate picking.

The unchanged reservation covers six gain generations, bounded input/envelope scratch and seven height matrices within the 64 MiB host display allowance. Conservative 30-second admission is 510.378 MiB under the 512 MiB limit. Audio callbacks use existing published immutable snapshots with bounded borrowing; no callback allocations, locks, waits or logging are introduced.

## Validation and remaining limits

Tests cover FFT/WOLA, original phase/context, authored-zero recovery, envelope winners and seams, source/revision rejection, Undo/reopen/cancel, blocked-audio visual publication, incremental/full equality, clipping/picking, navigation and resource admission. Controller fixtures use smaller geometry; a full production topology check remains. Performance diagnostics and development stroke logging have been removed from code. The shader manifest test resolves source resources independently of working directory.

Historical measurements and implementation review are in [Plan A](audio-spectral-plan-a.md). [Plan B](audio-spectral-plan-b.md) retains two future sculpting designs. CPU local projection measured about 1 ms; full rebuilds remain substantially more expensive. Those results are not GPU/frame latency guarantees. Live GPU/input/listening checks remain manual, and strict composed revision publication can defer audible convergence during continuous faster authoring. Recovery boundaries retain FFT window support rather than sample-exact isolation.

Supporting contracts: [source](audio-spectral-source-contract.md), [DSP](audio-spectral-dsp-contract.md), [FFT](audio-spectral-fft-dependency.md), [playback](audio-spectral-playback-contract.md), [surface](audio-spectral-surface-contract.md).
