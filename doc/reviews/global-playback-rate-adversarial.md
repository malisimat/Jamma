# Global playback rate: independent implementation review

Date: 2026-11-05. Reviewer: `final_adversarial`, independent source investigator.

Subsequent build, test and runtime results are recorded separately in
[implementation evidence](global-playback-rate-status.md). The review's pending
validation statements describe what was available to the reviewer at that time.

## Scope and verdict

Checkout: `C:/Users/matto/Source/Repos/Jamma.worktrees/Jamma_focused-owners`.
Branch: `codex/global-playback-rate`. Base and HEAD:
`f60c1fdc8f2f251b58f4ab0bf0f7e24cb16d7628`.
Mode: complete working tree, including new untracked production source and tests.
The reviewer changed only this report; implementers made the production repairs.

**Source review approved for the inspected candidate: no confirmed remaining
P1/P2 source finding in the reviewed feature paths. Release validation remains
open.** This replaces the earlier interim report. Its old findings are not
outstanding by default: the repaired implementations were inspected again.
This is not certification that the final build, full suite or manual tests pass.

### Final delta reassessment (2026-11-05)

The later Loop FIR optimization was inspected separately. In
`Loop::ReadBlock` (lines 865–984), the contiguous path runs only for original
material, after capture readiness. It bounds each span by the seam start,
published `BufferBank::Length`, the current bank edge, the loop child wrap and
the remaining 128 taps. `BufferBank::BlockPtr` (lines 197–205) then addresses
an allocated bank, whose storage is appended rather than moved by
`UpdateCapacity` (lines 95–109). Spectral playback of the current generation
uses the scalar `sampleAtBody` path, preserving `Loop::_ReadSpectralSample`'s
transition blend. Provisional capture also stays scalar. SSE2 `Dot` handles
groups of four and a scalar tail within the same bounded span. Coefficient
caching is keyed by rate and phase fraction, so it recomputes on a changing
trajectory. No confirmed P1/P2 defect was found in this delta.

The new bank/wrap and edited spectral tests in `LoopPlaybackRate_Tests.cpp`
(lines 524–567) exercise those paths, but use a zero-fade fixture and do not
cover a nonzero seam transition during FIR filtering. This is a focused test
gap, not an established source defect; the existing scalar seam path remains
in place. The disabled CPU matrix (lines 253–290) now includes near-unity,
3.7x, 4x and a ramp at one and 32 mono voices. The pre-optimization Release
32-voice run reportedly exceeded the 5.333 ms callback deadline; no
post-optimization timing was independently observed in this review. CPU
acceptance remains open.

### Spectral/seam cache reassessment (later final candidate)

The above seam test gap was subsequently closed by
`FilteredSeamMatchesCapturedUnityMaterialAcrossPartitions` in
`LoopPlaybackRate_Tests.cpp` (lines 616–648). The spectral test now compares a
transition against an expected linear ramp and a split callback (lines
575–614). The benchmark matrix includes raw 1.01/3.7/4/5x, a ramp, a
64-frame source with a 16-frame seam, steady spectral and active spectral
fade, each at one and 32 mono sources (lines 254–323). These tests inspect
the material paths; passing CPU cost still needs a measured Release result.

The new `Loop::ReadBlock` cache (lines 930–1002) is a pair of 128-float stack
rings local to one reader call. The first filtered frame fills the complete
footprint; later frames advance its head by wrapped source displacement and
refill at most 128 entered taps. A jump of 128 or more forces a complete
refill. For a short child, revisiting the same wrapped source phase correctly
reuses the same static material. The cache does not retain coefficients,
borrowed snapshot pointers or bank addresses between calls. A zero-seam
original source still uses the direct contiguous-bank path. The cache uses
`sampleAtBody` for seam composition and provisional material, preserving the
readiness and published-extent gates at lines 865–915.

During a current-generation spectral transition, `_ReadSpectralSample` is a
linear blend of previous and current material at a physical block offset
(lines 1852–1870), while `Hanning::Mix` is linear in its two inputs. Filtering
the two endpoint materials and blending their FIR results by
`frame / transitionRemaining` therefore reproduces the per-tap physical
transition for that block, up to floating-point summation order. Endpoints
are fixed for the call because spectral leases and transition progress change
at `EndMultiPlay`. The ring bounds every refill by the logical child length
and 128 taps. No allocation, lock, wait or unbounded traversal was added to
the callback path. No confirmed P1/P2 source defect was found in this later
delta.

The exact-base working-tree checker exited 0, whitespace exit 0, fingerprint
`90ae4daa5ecdf50100fba2659dfb82535cffa36bb8b77f956ca04d263c4cae07`
before this report edit. `git diff --check` exited 0. Root reported a clean
final Debug build and 41 focused tests passing; this reviewer did not run or
independently certify those binaries. Full final Debug regression and Release
CPU measurements were still running when this assessment was recorded.

The Timer legacy geometry guard (`Timer::LocalMusicalPosition`, lines 229–240)
avoids inferring PPQ from a nonintegral grain while preserving explicit full
BPI. Its explicit-BPI `QuantiseLength` branch rounds boundaries on that same
master ruler. Scene's empty-history tap handling (`Scene::_HandleTapTempo`,
lines 1799–1817) passes no history until the audio writer has published a
segment; future timestamps are deferred when history exists. No confirmed
P1/P2 defect was found in these later edits. The separate MIDI velocity/grid
fixture corrections were inspected as test changes only.

Read AGENTS.md, glossary, real-time guidance, loop-alignment guidance, the global
playback-rate plan and the repository review skill. Acceptance covers one
applied trajectory, distinct physical/source/musical rulers, capture-time
baselines, smooth positive corrections, coherent authority admission,
MIDI/automation, source-preserving persistence, bounded callback work and
measured DSP quality.

## Reassessed findings and repairs

| Area | Current source assessment |
| --- | --- |
| Rate, Timer and local metronome | Scalar descriptors replay one recurrence. Physical Scene samples remain physical; musical progress and local beat onsets consume the source trajectory with the full master BPI. `audio/PlaybackRate.*`, `audio/LocalMetronomeTiming.cpp`, `utils/Timer.cpp`. |
| Capture duration and trigger origin | Final lengths use take-source history. The effective content edge is resolved physically before historical musical-position lookup and master/take ruler conversion. Pending-master geometry has a separately named physical origin. `Station::OnAction`, `LoopTake::BeginCaptureAudioBoundary`. |
| Master activation | Selected-master phase is recaptured at activation. Selected and first-recorded master paths now reserve new history capacity before changing geometry; a denied first-master request stays unconsumed. `AudioHost::ApplyLocalMasterTimingAtAudioBoundary`, `AudioHost::_OnAudio`. |
| History pressure | Admission carries master metadata; per-take histories exclude unrelated master tags. Remote phase/geometry publications reserve a new segment even when slope is unchanged. First-master and selected-master geometry have explicit admission gates. `AudioHost::ApplyDesiredTimingAtAudioBoundary`, `PlaybackRateHistory::CanAppend`, `LoopTake::CanBeginPlaybackRateBlock`. |
| Pressure during detach | Denied NoSync immediately suspends external authority in the receipt while retaining the admitted remote rendering projection until the complete detach can be described. The newest target remains pending. It no longer accepts detachment and then cancels captures for the same denied segment. Reset/restart clear the suspension state. |
| Remote support limits | Every take slope is checked before authority mutation. Unsupported follow remains local with a typed rejection/status. Empty legacy geometry uses slope 1 rather than zero. The documented prepared base domain is 0.25–4 with the bounded 25% correction margin; larger protocol geometries are rejected instead of silently clamped. `Station::CanApplyRemotePlaybackRate`, `doc/global-playback-rate.md`. |
| Local master reference | A complete local reference survives follow, and an empty-before-follow session can establish it from its first explicit recorded geometry. Reset clears the retained pre-follow reference. Following, Aligning and pending admission are distinct receipt states. |
| Shared SHFT and child phase | Shared movement feeds the source trajectory used by audio/MIDI/automation. Cold inherited offsets are adopted once. MIDI keeps unwrapped source progress and applies each child's own modulo. Child-local SHFT remains separate. `LoopTake::BeginPlaybackRateBlock`, `Loop::AdoptInheritedPlaybackOffsetAtAudioBoundary`. |
| Finite SHFT settling | A large signed offset that exhausts double precision can finish its stalled tail only when the remainder fits that frame's correction budget. `Loop::_NextPlaybackOffset`. |
| Queued MIDI endpoints | The source endpoint residual survives integer device-frame mapping; physical compensation stays separate. Missing history or exhaustion of the global four-inverse-mappings budget cancels queued owner events and recovers audible ownership. A pending attack cannot survive without its release. `MidiIndexedOutputSink::TryOnVoiceEvent`, `MidiDelivery::_InvalidateScheduledMidiAtBlockStart`. |
| MIDI source grids | Remote grids convert into the take's recorded source interval or the accepted legacy source interval. Local transport defaults convert the master recording-rate ruler into take units. Export writes the resolved local base interval. `LoopTake::_ResolvedMidiQuantisation`, `SnapshotMidiForExport`. |
| Grid admission and supersession | Scene now reconstructs the job-side grid from the complete accepted audio receipt. BPI, tempo, origin and preserve-master intent share the existing desired-mailbox/receipt sequence with source/remote geometry. An early desired grid and a newer deferred phase-only publication cannot suppress or overwrite an already accepted grid. The cache skips unchanged geometry while advancing its observed revision. Reset invalidates the old applied receipt before the cache can reuse it. `Scene::_ApplyNinjamTimingUpdate`, `Scene::CommitChanges`, `AudioHost::LastAppliedDesiredTiming`. |
| Save during convergence | The target is read inside the paused/locked consistency boundary. Audio indices compensate requested versus applied local SHFT; shared requested offsets are added once for cold load. MIDI source progress retains child-relative phase. No DSP history or smoothing tail is persisted. `IoSessionExporter::ExportToDirectory`, `Loop::ToJamFile`, `LoopTake::SnapshotMidiForExport`. |
| Device restart | Per-take history reset is explicit at the quiescent device boundary, preserving completed playback and retiring active captures separately. The inspected SessionControl operation prepares the stream before the same job owner's MIDI pump; no extra history-reallocation race was demonstrated. |
| FIR and capture frontier | Prepared rows use conservative upper-bin cutoffs, including near unity, instead of mixing an all-pass row into speed-up filtering. Capture history and consumer pins include a 128-frame FIR margin; bounded catch-up includes the filter frontier. `Loop::PlaybackFilter`, `Loop::_FlushFilteredCapture`, `LoopTake::RequiredRateHistoryStart`. |
| Scene snapshot iteration | The station-naming loop retains one Accepted snapshot for both iterator endpoints. `Scene::_RequestStationControl`. |

The final accepted-grid receipt implementation also closes a defect found in
the intermediate staged-grid repair: advancing a grid payload's version on a
phase-only intent could make an admitted V1 grid wait indefinitely for deferred
V2. The final implementation consumes the actual accepted receipt rather than
a pending desired-grid version.

New or extended tests target the material cases: near-unity alias bins,
subframe MIDI endpoints, Missing-history cancellation, five simultaneous
edited-endpoint recovery, mixed recording-rate persistence, history pressure,
detachment and unequal children. Grid receipt/supersession tests were still
being adapted separately. Test declarations do not establish passing results.

## Real-time and ownership assessment

The inspected additions use prepared storage, fixed FIR tables, scalar
trajectories and bounded loops. No concrete new callback heap allocation,
blocking lock, file I/O or logging defect was found in the reviewed paths.
History preparation stays outside the callback. Membership uses the existing
immutable snapshot/borrow pattern.

Bounded work does not establish adequate CPU cost. FIR work scales with audible
channels and repeated readers; inverse MIDI queries replay retained trajectory
segments. The global four-mapping budget bounds that extra work, with explicit
owner release recovery on overflow. Dense edits may therefore shorten affected
MIDI voices. Measure this policy and the maximum claimed load; do not infer
real-time performance from allocation freedom or helper-test success.

Scene's growth is orchestration and receipts; Station's is fanout and
master/capture wiring. Loop owns source filtering/capture resampling. LoopTake's
substantial growth remains source-timing/capture ownership, although the many
related scalars need continued discipline as coherent state. No arbitrary class
size cap or additional production refactor is required by this review.

The explicit remote admission bound and conservative FIR transition band are
documented operating limits. Full-domain remote admission/rejection, transition
quality and worst-case cost need evidence before release.

## Static evidence

Command executed by this reviewer:

```powershell
python .agents/skills/jamma-review/scripts/review_changes.py --base f60c1fdc8f2f251b58f4ab0bf0f7e24cb16d7628 --working-tree --format json
```

Checker exit: 0. Whitespace exit: 0. Candidate fingerprint before replacing this
report:
`a2f7438375a6eb63f87f78abf25169ed4bc19742006abf0ffd4c9608cb938d6e`.

The fingerprint records bytes read, not an atomic filesystem snapshot.
Production source was reported frozen for this final pass; separate test
adaptation remained active. Writing this report itself changes the complete
working-tree fingerprint. Relevant later source edits require reassessment.
Keyword candidates are investigation prompts, not a callback-safety proof.

After the final delta reassessment above, the same exact-base working-tree
checker exited 0 with whitespace exit 0 and fingerprint
`265c8c70e030cb0ce31256bd8141c3b4728da34c8e46b9abafda0120f1e88004`
(60 files). `git diff --check` also exited 0. This report edit changes its own
fingerprint again; the source assessment is tied to the inspected Loop, Timer,
Scene and test content rather than the report's bytes.

## Remaining release-validation gates

No builds, native tests, device operation, listening, GUI validation or callback
measurements were performed by this reviewer. Root reported partial runs,
subsequent repairs and a full-suite interruption. Those are not a completed
final regression result and are not independently certified here.

Before release, retain evidence for:

- Final Debug and Release app/native builds and successful focused plus full
  regression runs after all source/test repairs, including the interrupted
  Scene path. Read local `.vscode/tasks.json` immediately before each invocation.
- Accepted-grid receipt/supersession, pinned-history remote phase/detach, and
  first/selected-master admission regressions, including real takes and active
  capture consumers rather than only an empty host.
- Callback allocation instrumentation and the maximum supported
  take/channel/event load at rate extremes with VST, capture and NINJAM active.
- Numeric spectral, continuity and settling gates plus Release CPU measurements.
  Steady-tone helper checks alone do not establish ramp/filter-transition
  quality or capture-frontier safety.
- Listening and GUI checks for slider reversals, shared/child SHFT, mixed
  baselines, MIDI/pedals/automation, save during convergence, device restart,
  odd BPI and server request acceptance, rejection and supersession.

Session export correctly retains source media. No offline mixed-audio renderer
was found; its duration item is documented as inapplicable in
`doc/global-playback-rate.md`, rather than implemented by resampling saved WAVs.

## Final test-only Release correction review

Root reported a final Debug native run of 2289/2289 passing and an isolated
Release filter matrix whose worst 32-mono-source case took 4.834 ms against
the 5.333 ms block deadline, including active spectral fade. This measures
the isolated renderer matrix; full app callback load and listening remain
separate validation items.

The Release routing correction in
`SceneRoutingIntegration_Tests.cpp` (lines 1495–1548) observes the actual
`Scene::_RequestRigEditLocked` result through the HUD submission callback.
It retries the complete pointer gesture only after the explicit
`EditsDisabled` result, with a two-second deadline. A missing callback,
`TriggerBusy`, `TransitionRejected`, any other failure, or a failed pointer
assertion still fails the test; the final accepted receiver and rig target
must match the replacement. This retry does not mask the routing contract.
The shared `GuiHudTestAccess` class is defined once in a header included by
both test translation units, with inline member definitions and a project
header registration; no duplicate class definition remains in
`CableInteraction_Tests.cpp`. The JAM fixture in `JamFile_Tests.cpp` now
value-initializes its records and explicitly supplies loop level, speed and
wire mix before round-trip. This closes the Release uninitialized-speed
fixture issue without changing production behavior. No P1/P2 test finding.

The exact-base working-tree checker exited 0 with whitespace exit 0,
fingerprint `bfef88531b59f4855d6c1849ca04bb60e03548cf2a8e8db1d79e369b792fa6bc`
before this report edit (63 files). The final Release full suite still needs
one clean run after the test correction; that result was not observed here.
