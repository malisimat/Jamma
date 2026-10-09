# Quantisation enhancement validation

Validated on 2026-10-09 in `feature/quantisation-enhancements`, this worktree.
Implementation followed the committed [contract/gap pass](quantisation-contract-pass.md).
Independent reviews identified and corrected frozen physical/logical interval
confusion, stale NoSync authority, lost saved tap bases, source-history revision
coupling, global enable precedence, oversized manifest interval narrowing,
hidden input routing and pending-grid preview commits.

## Build and native evidence

- Full native run: **1,294 tests from 160 suites passed**, 16.857 seconds.
- Incremental Debug x64 test/library build: **0 warnings, 0 errors**.
- Incremental Debug x64 app build: **0 warnings, 0 errors**.
- Final source whitespace check: clean.

Before each build/native run, local `.vscode/tasks.json` was read. MSBuild came
from its authoritative VS 18 Community command. Direct project builds used
absolute project paths, `/m /t:Build /p:Configuration=Debug /p:Platform=x64`
and the absolute `SolutionDir` with exactly one trailing backslash. The process
Path preflight followed `doc/build.md`; no task file was changed.

The initial library build lacked `vcpkg_installed` in this worktree. The existing
main-checkout dependency directory was copied into the ignored local directory.
Debug runtime DLLs were copied into the native test output after an initial
silent startup failure. Later source/test failures were corrected before the
passing run above; their output is retained as investigation evidence.

Full logs remain in ignored build output:

- [Native results](../test/JammaLib_Tests/bin/x64/Debug/quantisation-evidence/quantisation-all-tests.log)
- [Test/library build](../test/JammaLib_Tests/bin/x64/Debug/quantisation-evidence/quantisation-tests-build.log)
- [App build](../test/JammaLib_Tests/bin/x64/Debug/quantisation-evidence/quantisation-app-build.log)
- [Conservative thread audit](../test/JammaLib_Tests/bin/x64/Debug/quantisation-evidence/quantisation-thread-audit.log)

The [diagnostic trace](quantisation-diagnostic-trace.txt) contains actual native
runtime output for default recording inference, sole MIDI-only and multichannel
taps, additional-take subdivisions, and remote-authority subdivisions. These are
engine fixture executions; they are not a live GUI or external-server trace.

## Acceptance covered by native execution

- Actual tap handler: completed MIDI-only and multichannel audio+MIDI counting,
  sole beat geometry, preserved MIDI source length, frozen additional-take
  construction/cursors, remote descriptor preservation on repeated observations.
- Tracker: zero timestamp, two-second expiry boundary, timeout, smoothing reset,
  first-tap radio preservation, overlay grace/fade timing, and non-increasing
  and invalid inputs. Candidate limits, straight/triplet ties and
  exact requested-count grain rounding have explicit regressions.
- Source-backed resolution round trips, duration/wrap and rational sample
  boundaries, shuffle placement, legacy fraction ordinals and pack/session
  parsing, local stream override isolation, global enable precedence.
- Undo/redo across grid changes and rejection of stale gestures/external source
  writers, including the gap before a changed take grid has been published.
- Native Scene input simulation with E open and Ctrl held, physical Space
  repeat/release, text precedence after focus changes, focus cleanup, explicit
  picked MIDI stream selection and preserved editor gestures.
- Controller selection capture despite later hover/selection changes, smooth
  rapid transitions, overlapping Space/gesture holds, Ctrl-preserving gesture
  cancellation and target deletion cleanup.
- Saved local tap bases and stream overrides, exclusion of live remote base
  geometry, and physical audio tail retained across WAV/manifest reload.
- Existing NINJAM timing, alignment, transport, loop geometry, MIDI, trigger,
  overdub, UI layout and VST suites passed in the same full regression run.

## Thread review

Applied `.agents/skills/threading-review/SKILL.md`. The audit script conservatively
flagged seven added locks in its file-level diff. Manual inspection placed them
in `LoopTake::SetMidiLoopQuantisationOverride`, Scene keyboard/reclock handling,
`_InteractionContext`, `_HandleTapTempo`, `_RouteQuantisationTouch` and
`_RouteQuantisationMove`. They are off-callback owner/routing operations. No new
lock, wait, allocation, formatting, logging or I/O was added to the callback
bodies listed in `doc/realtime-audio.md`. The script result is retained rather
than represented as a clean automated audit.

| New state | Owner/writers | Readers and synchronisation | Teardown |
| --- | --- | --- | --- |
| Take base interval/divisions | Job/UI engine under Scene ownership | One packed atomic value; publication resolves into immutable MIDI snapshots | Remote geometry change, NoSync/reclock, take destruction |
| Accepted remote descriptor and generation/source | Scene-serialised job/UI; source and generation use atomics | Tap policy under Scene mutex; atomic diagnostics | NoSync, local clear/reclock, Scene destruction |
| MIDI stream overrides and inherited/forced settings | Owning take capture mutex, off callback | Audio consumes immutable playback snapshot; owner getters stay off callback | Stream replacement/destruction; optional session restore |
| Source revision | MIDI publication owner under capture mutex | Immutable editor/playback snapshots, full publication guard remains | Stream lifecycle |
| Space/gesture grid reasons | Atomic scalar holds, UI writers and job cleanup | Atomic overlay state on render/UI | Focus/session cleanup and clear |
| Ctrl capture/alpha/feedback and physical keys | UI thread | UI input/render; hierarchy access uses Scene mutex | Gesture/capture/focus/editor/session cleanup |
| Deferred input reset | Job producer | UI atomic exchange in animation advancement | Consumed once before UI traversal |

## Explicitly unverified

Computer Use instructions were read, but this tool session has no callable
`node_repl` runtime. Native input simulation does not establish live window
behaviour. No live visual, audio-device or external NINJAM acceptance is claimed.

Still check panel/handle shader output, captions, clipping, fades, restored racks,
HUD feedback and suppressed hover/click/wheel/focus at representative window
sizes/DPI. Exercise mouse taps, held Space and Ctrl release mid-drag in the real
window, including native capture loss and deleted targets.

Record/play/listen to sole audio, MIDI-only and stereo+MIDI takes; confirm live
propagation, continuity, source/history round trips and additional-take phases.
MIDI keeps its source period if integer audio rounding changes the master length;
the audibility/phase implications of that deliberately unequal period remain a
live check. WAV export retains tails within the existing PCM16 scaling/truncation
precision, not bit-exact floating-point precision.

Exercise a real NINJAM server, NoSync/Stay local, disconnect/reconnect, reclock,
and interactive session loading. Older-binary resaves of new triplet/override/base
fields were not tested. Live application timing receipts, perceptual continuity
and physical DAC-to-ADC loopback were not measured.

User instructions are updated in [overlay controls](overlay-controls.md) and
[loop grid editor](loop-grid-editor.md). The [HTML handoff](quantisation-enhancements-summary.html)
collects the result, concerns, evidence and remaining acceptance checks.
