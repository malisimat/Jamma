# Global playback rate implementation evidence

Base: `feature/alpha-tweaks`, `f60c1fdc8f2f251b58f4ab0bf0f7e24cb16d7628`, explicitly selected by the user.
Candidate: `codex/global-playback-rate`, working tree including new source and tests.
Date: 2026-11-05. Contract: `doc/plans/global-playback-rate.md` and repository policy.

## Implementation and review

Implemented the common applied rate trajectory, per-take recording baselines,
bounded smooth phase/authority transitions, source-domain capture and playback,
MIDI/automation timing, physical versus musical clocks, server request fences,
optional JAM metadata, and Timing-panel controls. Source media remains unchanged
on save. There is no offline mixed-audio exporter in this checkout.

Separate implementers covered scalar math, rendering/capture, musical timing and
contract integration. Fresh-context Sol reviewers challenged the complete feature
and subsequent fixes. See [independent review](global-playback-rate-adversarial.md).
The latest source review found no confirmed remaining P1/P2 issue; performance,
native tests and manual runtime evidence are separate gates.

## Build and automated evidence

Logs, XML and JSON build records are in the ignored directory
`Jamma/bin/validation/global-playback-rate/`. Both configurations use the local
task file's MSBuild and incremental solution Build. Every build and native-test
invocation reads `.vscode/tasks.json` first.

- Debug and Release solution builds: succeeded, zero warnings and zero errors.
- Full Debug suite on the final production implementation: 2,289 passed,
  zero failed (249.055 seconds); `debug-all-final.log` and XML.
- Final full Release suite: 2,271 passed, zero failed (36.415 seconds);
  `release-all-final.log` and XML. Debug-only instrumentation accounts for
  additional Debug tests.
- Release orphan-routing regression: 100 consecutive passes;
  `release-orphan-repeat-final.log`.
- Focused final DSP/SHFT/callback checks: 41 passed; `debug-final-dsp.log`.
- After the final test-only fixture corrections, rebuilt Debug and passed all
  188 routing/cable/JAM tests; `debug-final-fixtures.log`. Production code was
  unchanged from the full Debug run.
- The opt-in CPU benchmark is deliberately disabled in ordinary regression
  runs and is executed separately with `--gtest_also_run_disabled_tests`.

Release testing exposed undefined initialization in an existing JAM fixture.
It now initializes its loop and explicitly supplies valid speed, level and mix;
the production parser remains unchanged. Other repaired fixtures exercise the
real UI commit/scroll paths and the current amplitude-to-MIDI-fader mapping.
The orphan reconnect fixture now records the actual lifecycle-admission receipt
and retries a complete gesture only for `EditsDisabled`, within two seconds.
Missing dispatch, other rejections and incorrect accepted routing still fail.
The independent reviewer checked these test-only corrections separately.

The exact-base working-tree review checker and `git diff --check` passed.
Focused tests cover rate/history partitioning, positive correction/settling,
capture filtering/frontiers, mixed baselines, save during SHFT convergence,
queued MIDI endpoint recovery, authority admission/history pressure, and odd BPI.
Debug CRT allocation probes exercise prepared callback rate transitions.

Final application SHA-256 values:

- Debug: `94eac9058ec4f1b1459627a6897b14838c4131b5a8a99e123d2f7e6c4aee7e05`
- Release: `496f5f0a2234361c9526017239473197ffd07c921220cb976ee0fb1d4b2f5481`

The FIR tests require stop-band RMS below `1e-4` at the selected playback/capture
tones, and below `3e-4` across near-unity and upper rate bins through 5x. These
compare rejected tones with the ideal band-limited zero reference. They also
check passband level, transition onset, unity equivalence, repeated readers,
bank boundaries and published spectral material. These are numeric tests, not
listening evidence or a broad offline-reference audio comparison.

## Release renderer measurements

Final opt-in matrix: 256 frames, 48 kHz deadline 5.333 ms, 32 mono sources,
64 measured blocks after eight warm-up blocks. No builds or regression suites
were running during this measurement. Host reports Intel64 family 6 model 94,
eight logical processors. Evidence: `release-cpu-final.log`.

| Material and source rate | Median ms | Maximum ms |
| --- | ---: | ---: |
| Raw, 1.01x | 2.2430 | 4.3279 |
| Raw, 3.7x | 2.3917 | 4.4407 |
| Raw, 4x | 1.0813 | 2.0739 |
| Raw, 5x correction margin | 1.1937 | 1.7179 |
| Raw, repeated 1-to-2 ramp | 2.5133 | 4.4094 |
| 64-sample child, 16-sample seam, 5x | 1.4413 | 1.8194 |
| Spectral, 3.7x | 2.8889 | 4.8341 |
| Active spectral fade, 3.7x | 3.8504 | 4.2782 |

All measured renderer maxima fit that block deadline. This is a finite
measurement, not a worst-case execution-time proof or a guarantee for a complete
32-channel callback with capture, VST and networking. The spectral case leaves
little room for those other costs; practical channel/plugin limits need hardware
testing. A concurrent-regression measurement exceeded the deadline on some
blocks and is retained separately as `release-cpu-concurrent.log`.

The original scalar FIR failed this gate (32 raw voices about 16 ms, spectral
about 17 ms). Fixed contiguous spans, coefficient reuse and stack-local material
caches resolved that measured bottleneck without changing coefficients or range.

## GUI and hardware evidence

An earlier successful Debug candidate was launched with disposable defaults and
verified by executable path and SHA-256. Its scoped Win32 screenshot showed the
purple rate control, 1.000x label and server request control without clipping.
Startup logs confirmed Focusrite USB ASIO at 44100 Hz / 512 frames, 18 inputs and
20 outputs, plus MIDI connections. See `gui-inspection.json`, `gui-started.png`
and `gui-logs/run.log` in the evidence directory.

Computer Use discovery timed out, so mouse/keyboard interaction was not tested.
Only that verified disposable process was terminated; orderly shutdown was not
verified. This startup screenshot predates the final rendering optimization and
is not evidence of the final binary's interactive behavior.

## Manual test handover

Run `Jamma/bin/x64/Release/Jamma.exe` for listening and load testing; also smoke
test `Jamma/bin/x64/Debug/Jamma.exe`. Close any existing Jamma instance first and
use a disposable session. Follow [the scenario checklist](../global-playback-rate.md#manual-checks).
In particular, test mixed recording rates, overdub/punch during ramps, child
SHFT, save/reload during convergence, odd BPI and device restart, then real
NINJAM detach/request/acceptance/reconnect. Listen for clicks, aliasing and stuck
notes and inspect underruns with active VST racks and capture.

Listening, GUI gestures, sustained hardware performance and real server
interaction remain manual checks; automated tests do not establish those results.
