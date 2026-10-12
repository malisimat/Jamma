# Playback investigation and pre-merge handoff

Updated: 2026-10-12

Branch: `feature/midi-loop-seam-merge`

## Current outcome and review boundary

**The reported first-master MIDI double kick now appears resolved.** After the latest
implementation and software validation, the user auditioned it and reported:
“seems like it works!” This is successful listening evidence for the reported
setup, not an instrumented post-fix timing measurement or validation of every route.

The branch contains earlier committed seam and playback-fidelity work plus the
latest first-master/grid/overlap fixes in the index. At this handoff:

- Earlier merged baseline: `cff0082` (quantisation enhancements).
- `8c76364`: unmatched MIDI note continuation across seams.
- `fb8cbbc`: seam/fidelity investigation documentation.
- `9ff58a2`: playback-fidelity F1–F6 fixes and recorded-path latency compensation.
- HEAD `eb34ab1`: earlier open-investigation documentation.
- Latest engine and MIDI test changes are staged but **not committed**; this
  handoff document has unstaged changes. No staging or commit was done for this
  documentation request.

Review the committed branch changes and the complete current checkout, not HEAD
alone. Use `git diff cff0082 HEAD`, `git diff --cached`, and `git diff`; verify
that the eventual PR base is appropriate. `git diff HEAD` includes both staged
and unstaged follow-up work. An independent review of the latest changes and
removal of temporary diagnostics remain before merge.

The linked [original seam plan](midi-loop-seam-note-merge-plan.md) and
[fidelity investigation](midi-playback-fidelity-investigation.md) preserve earlier
research and verification. Their historical exclusions of finite-span overlap
work, and statements that hardware audition was not performed, describe those
earlier stages. This handoff records the expanded final scope and current result.

## 1. What we sought to achieve

The initial symptom was a MIDI-only first/master loop recorded with a MIDI trigger,
quantisation enabled, producing an obvious double kick at **every** loop seam.
Disabling quantisation brought the two attacks closer together but did not remove
the double. The user also described an apparent large timing error, around 300 ms,
and asked for logs to distinguish duration, origin, grid, and latency problems.
The issue was reported even on the first recording after launch; later takes
seemed better, but were not established as correct by that observation.

The agreed musical and timing contract is:

1. The logical master period is exactly `grainSamples * grainCount`, in integer
   samples. Preserve this invariant when inferring timing from the trigger interval.
2. MIDI event times are not proportionally stretched to fit the adjusted period.
   The grid evenly divides the adjusted master period using rational geometry;
   rounding is needed only when resolving an individual sample index.
3. Local master sample zero is grid beat zero. First-master capture uses the
   actual trigger origin and duration; an existing configured grain does not
   establish that a master has already been recorded.
4. Merge overlapping same-channel/pitch note spans from **adjacent source repeats**
   on an unwrapped timeline. Handle quantised onsets both before the seam, both
   after it, or on opposite sides. Preserve non-overlapping and merely touching
   spans, and legitimate retriggers belonging to the same source repeat.
5. A note already held at record start and still held at record stop must not be
   split into a repeated artificial head attack and a tail attack at each seam.
   Keep the first cold-start articulation and preserve the relevant velocities.
6. Maintain bounded callback work and existing subsystem/thread ownership. Keep
   diagnostic additions contained and removable.

The broader branch also addresses shared-grid/master-promotion phase, reliable
MIDI delivery and voice state, relocation, and reported plugin-path latency.
Physical driver latency and instrument envelope calibration require separate
measurement; software sample-position agreement alone cannot establish them.

## 2. Evidence and diagnosed cause

### User capture before the latest fix

The supplied log used Kontakt/Drums, CASIO USB-MIDI for notes, and Focusrite USB
MIDI for the trigger. Trigger channel 2, note 1, NoteOn started/stopped recording;
NoteOff was the subsequent trigger release. Actual ASIO stream: 44100 Hz,
64-sample blocks, two buffers, eight inputs/outputs, input and output latency
177 samples each (about 4.01 ms). Missing Xkey/serial endpoints were not the
recording trigger in this reproduction.

The decisive timing fields were:

| Field | Logged value | Meaning |
| --- | --- | --- |
| Start / stop MIDI sample | 215304 / 302475 | Same timestamp ruler for both trigger edges |
| Physical trigger interval | 87171 samples, 1976.67 ms | Actual first-master capture interval |
| Logical master | 87168 samples, 1976.60 ms | Exact 21792 samples × 4 grains |
| Physical minus logical | 3 samples, 0.068 ms | Tiny grain-product adjustment in this capture |
| Callback-counted duration | 84608 samples | 2563 samples (58.1 ms) shorter than trigger interval |
| Stop age / initial play offset | 1333 / 1336 samples | Stop processing age plus period adjustment |
| Scene sample / master phase at stop | 303808 / 1336 | Logged phase after seeding |
| Seeded before recording ended | 0 | This was actually the first master |
| MIDI grid | 87168 / 16, step 5448, remainder 0 | Quarter-of-grain subdivision of four grains |
| MIDI settings | enabled 1, grain 0, fraction 2, phase 0 | Enabled base grid despite zero construction grain |
| Raw source events | 30; none outside logical loop | Two other MIDI streams were empty |

This capture does **not** show a hundreds-of-ms master-period discrepancy. It
supports using the trigger timestamps rather than the delayed callback count
for first MIDI-only master duration. It does not rule out every origin or latency
problem in other capture paths.

### Why the seam doubled

The raw kick (channel 1 / stored channel 0, pitch 36) contained:

| Event | Sample | Velocity | Interpretation |
| --- | ---: | ---: | --- |
| NoteOn | 0 | 95 | Imported held note: physical attack preceded first trigger |
| NoteOff | 5107 | 64 | Actual release of that initial held note |
| NoteOn / NoteOff | 17931 / 26897 | 102 / 64 | Interior hit |
| NoteOn / NoteOff | 51237 / 59978 | 97 / 64 | Interior hit |
| NoteOn / NoteOff | 73104 / 79693 | 98 / 64 | Interior hit |
| NoteOn | 84246 | 88 | Tail hit still held at second trigger |
| NoteOff | 87167 | 0 | Synthetic completion release at L−1 |

The hihat (pitch 44) likewise had a seeded head at zero, a tail attack at 83630,
and an artificial release at 87167. Physical kick/hihat releases appeared after
record-end in the log. Thus, the tail really was held over the recording end.

The artificial L−1 release made the tail look like an ordinary closed note.
Existing unmatched-note continuation could not join it to the next repeat's
seeded head. It played two attacks every seam.

Without quantisation, the tail-to-next-head distance was
`87168 - 84246 = 2922` samples, about **66.26 ms**. With this grid, the tail
snaps earlier to 81720 and the head remains at zero: the gap is 5448 samples,
about **123.54 ms**. That explains why disabling quantisation brought the double
closer. There is no evidence that sample-zero head was moved to the next beat.

The log also exposed a separate activation defect: a base grid with positive
interval/divisions but construction grain zero was considered inactive by the
playback snapshot. Enabled settings and grid geometry were present, while the
immediate logged playback view was raw. Later grain propagation could activate
quantisation; that explanation is inferred from source, not a captured later
snapshot. The final fix recognises the effective grid directly.

## 3. Changes made

### A. Earlier committed seam implementation (8c76364)

In [MidiLoop](../JammaLib/src/midi/MidiLoop.cpp), source endpoints are paired FIFO
by timing class, channel, and pitch before quantisation/sorting. Immutable raw and
prepared snapshot metadata identifies genuinely unmatched attacks. Callback-owned
fixed bitsets retain accepted sounding continuations across both seam paths;
subsequent same-slot attacks are suppressed until a played release or full flush.
Cold-start heads still play. Revision, mute/cancel, and explicit flush release
voices. Metadata follows completion, overdub, replacement, edits, and restore.
Source MIDI representation and export format remain separate from playback state.

### B. Committed playback fidelity fixes (9ff58a2)

The [fidelity investigation](midi-playback-fidelity-investigation.md#follow-up-implementation-and-selected-policies)
contains the implementation detail and tests. All six findings were addressed:

| Finding | Implemented change |
| --- | --- |
| F1: master promotion loses phase | Publish local master geometry, phase, and prepared MIDI grids together at a callback boundary; support audio and MIDI-only targets; rebase existing take anchors off callback; reject stale preparation/publication. |
| F2: differing reported plugin latency | Align recorded paths to the longest loaded, non-bypassed path. Include loop/take/station audio chains and destination/downstream station MIDI suffixes. Query VST3 latency and refresh notifications through an off-callback controller pump. |
| F3: adapter overflow loses releases | Report admission acceptance, reserve release capacity, and retain rejected releases in fixed per-destination ledgers for retries. Continuation tracks accepted output. |
| F4: skipped VST3 processing loses block events | Report editor/bypass non-processing, recover uncertain voices, and discard stale MIDI/parameter block points. |
| F5: scaled attacks become velocity-zero releases | Suppress positive source attacks rounded to zero; they establish no held/continuation state, allowing later attacks when level rises. |
| F6: deliberate cursor relocation leaves wrong voices | Release old voices and chase destination notes using prepared metadata. Ordinary seams and small timing corrections retain continuity. |

Recorded compensation uses off-callback audio history allocation and immutable
descriptors, a 64-sample transition, and a fixed 1024-event future-MIDI queue with
256 release slots. Reports beyond the 262144-sample reference limit are rejected
rather than silently clamped. Delay changes recover queued/sounding notes at a
block boundary. Shared hardware output latency is not added again.

Direct live audio and live MIDI remain unpadded by the selected monitoring policy.
Recorded playback can consequently arrive later than direct monitoring. Punch-in
mute cancels host-buffered recorded output; plugin release tails remain plugin
behaviour. Dense VST3 mapped controller curves retain bounded-point/coalescing
behaviour; that is a known fidelity limit.

Rendered regressions also exposed and fixed `Loop::ShiftPlayIndex`: first-play
cursors below the fade prefix now normalise `rawCursor - fadePrefix` modulo body
length, preserving audio/MIDI phase through positive and negative corrections.
Capture timestamps and stored samples retain their original coordinate domains.

### C. Latest first-master timing and grid fixes (current index)

In [Station.cpp](../JammaLib/src/engine/Station.cpp) and
[Station.h](../JammaLib/src/engine/Station.h):

- Decide whether a master exists from `SeedSourceLength()`, rather than
  `IsQuantisable()`. A configured grain alone is not a recorded master.
- For an unseeded MIDI-only master with MIDI start/stop timestamps, use their
  interval as physical duration. Check trigger audio capture ownership explicitly.
  Later takes retain their existing callback/counting and duration-quantisation
  policy.
- Infer the logical master through the existing floor-grain/exact-product policy.
  Set its local MIDI base grid to the seeded interval and corresponding grain
  count, retaining rational subdivision and local origin zero. Remote and
  audio-capture routes are guarded out of this special grid setup.
- Read scene sample once and compute signed stop age, including the case where
  a MIDI edge is ahead of the published audio boundary. Initial phase is
  `(physicalDuration + signedStopAge) mod logicalLength`, normalised positive.
  Use that phase for the master and MIDI initial cursor.

In [Timer.cpp](../JammaLib/src/utils/Timer.cpp) and
[Timer.h](../JammaLib/src/utils/Timer.h), add
`SetMasterLoopSampleOffset` to store the modulo-normalised sample phase directly
in the existing atomic offset, avoiding a fractional round-trip. The master
period remains the exact grain product; events are not time-scaled.

### D. Latest recording seam provenance fix (current index)

In [LoopTake.cpp](../JammaLib/src/engine/LoopTake.cpp) and
[LoopTake.h](../JammaLib/src/engine/LoopTake.h):

- Add capture-owner `_midiRecordSeededHeld` bitsets per stream. Mark only held
  head attacks actually accepted into recording.
- At first-master completion, if a slot was seeded held and remains held, omit
  its artificial L−1 release. The recorded head release can then close the tail
  on the next repeat. Other completion-release policies remain intact.
- Initialise/reset provenance alongside existing held capture state during
  recording, completion, ditch, overdub, and restore lifecycle operations.
  This state is not read by the audio callback or MIDI ingress.

### E. Latest general adjacent-repeat overlap merge (current index)

In [MidiLoop.cpp](../JammaLib/src/midi/MidiLoop.cpp) and
[MidiLoop.h](../JammaLib/src/midi/MidiLoop.h):

- Activate quantisation from enabled effective interval/divisions and event
  count, including grain-zero base grids. Add `PlaybackViewActive` separately
  from `QuantisationActive`, because unquantised playback can also need merging.
- Build `MergePlaybackSeamNotes` at snapshot publication. Pair source endpoints
  FIFO by `PairingSlot`; exact and recorded timing classes pair independently.
  Derive unwrapped effective positions using the same rational grid, transport
  origin, and phase as playback; exact events remain unsnapped.
- Compare same-channel/pitch finite spans from adjacent source repeats, using
  strict interval intersection. Weighted union-find preserves source-cycle
  offsets through connected components and identifies continuously overlapping
  periodic sustains. Same-repeat overlap alone and touching endpoints do not
  establish merge edges.
- Retain the earliest unwrapped attack and its velocity, and the farthest terminal
  release with its original velocity. Equal-time selection favours the tail
  from the previous source repeat. Suppressed prepared endpoints use sample L
  (outside the playback range); raw source arrays remain unchanged.
- Handle wrapped unmatched tails meeting paired heads at the same canonical
  sample only when their unwrapped starts are one source period apart. Preserve
  existing natural-unmatched continuation semantics without inventing a release.
- Carry continuation kinds with prepared events: 0 ordinary, 1 natural unmatched,
  2 merged continuation, 3 bootstrap merged head. A cold-start head can sound once;
  repeated heads are suppressed while their merged tail is sounding.
- Add callback-only `_mergedContinuing` and matching prepared chase state.
  Merged continuation suppresses its duplicate roots/heads, while unrelated
  ordinary retriggers can sound. Played releases, retry recovery, and full flush
  clear both kinds of carry state. Chase uses the same suppression rule.
- Playback readers and chase/model projection select the prepared playback view;
  raw event access/edit/export retains source attribution. Existing immutable
  snapshot publication/lifetime machinery remains in use.

Scratch storage, pairing, sorting, and overlap comparisons occur on the
publication owner, outside the callback. The overlap comparison can be O(N²),
bounded by the fixed 4096-event capacity. Callback changes use fixed bitsets and
published metadata; no new callback allocation, locks, waits, or logging were
introduced. This helper applies to general MidiLoop playback, so its review scope
is broader than the particular first-master reproduction.

### F. Temporary diagnostics and build repairs (current index)

Temporary first-master diagnostics are intentionally isolated:

- Station: `logTimingDiagnostics` gate in `_TrySeedClockFromFirstLoop`,
  extended `[quantisation]` geometry output, and `[midi-timing]` completion block.
- LoopTake: optional `logFirstMasterMidiTiming` argument on `Play` and a gated
  per-stream call after recording completion/publication.
- MidiLoop: `LogTimingDiagnostics` copies at most 32 raw/prepared events and
  metadata to fixed storage, releases the snapshot reader, then writes
  `[midi-grid]` / `[midi-event]` output. Continuation kinds accompany events.

These are owner-side logs, never callback traces. Raw and prepared arrays do not
by themselves prove which attacks were emitted: callback continuation can
suppress a retained bootstrap head. No post-fix emitted-event hardware trace
was supplied.

During diagnostic integration, build errors were repaired by matching Station's
helper declaration/definition (optional sixth argument), and using an indexed
MIDI completion loop so its stream index exists. Final builds passed afterward.
Remove the temporary diagnostic APIs/arguments/blocks before merge; preserve the
permanent phase, grid, capture provenance, and continuation changes.

## 4. Tests and results

Latest changes in
[MidiLoop_Tests.cpp](../test/JammaLib_Tests/src/midi/MidiLoop_Tests.cpp) add:

| Test | Regression covered |
| --- | --- |
| MidiLoopQuantisation.BaseGridActivatesWithoutConstructionGrain | Logged 87168/16 geometry with grain zero; zero remains zero; raw tail remains 84246 while prepared attack is 81720; a non-overlapping finite tail is preserved. |
| MidiLoopSeam.MergesOnlyOverlapsBetweenAdjacentSourceRepeats | Quantised onsets before/at/after seam at phases −50/0/+50; several block partitions; earliest attack and terminal release velocities; same-repeat and touching controls. |
| LoopTakeMidiTiming.FirstMasterMergesHeldKickAcrossRecordingSeam | Logged kick sequence and trigger origin, seeded velocity 95 and tail 88; quantisation off/on, full-period/crossing/64-sample blocks over three repeats. Initial head occurs once; four actual hits recur each period. |
| MidiLoopSeam.OppositeSidesMergeOnlyWhenTheyOverlap | Raw/quantised cross-seam overlap versus a touching non-overlap; preserve initial articulation. |
| MidiLoopSeam.WrappedUnmatchedTailAndZeroHeadEmitOneAttack | Tail snapping to zero with head at zero; one accepted attack per repeat and correct release. |
| MidiLoopSeam.MergedLongNoteDoesNotSuppressSameRepeatRetrigger | A long exact note overlaps its next repeat while a legitimate ordinary same-repeat retrigger survives. |

The existing `LoopTakeMidiTiming.MidiOnlyMasterUsesPressIntervalAndStopPhase`
expectation now includes the physical-minus-logical remainder in initial phase:
`(physicalLength - logicalLength + stopAge) mod logicalLength`. It verifies
master product, note positioning, and cursor consistency.

| Validation | Result and scope |
| --- | --- |
| Original seam stage, 8c76364 | Incremental Debug x64 engine/test build; 382 tests in 52 suites with filter `*Midi*:LoopTakeTiming.*`; hot-path audit and diff check passed. |
| Committed fidelity stage, 9ff58a2 | Incremental Debug x64 engine/native-test and app builds; full native suite **1466 tests in 179 suites passed**; independent Sol review and manual threading review recorded in linked report. |
| Latest engine/test builds | Incremental Debug x64 JammaLib and native-test project builds passed; test-project build reported zero warnings/errors. |
| Latest native test run | **150 tests in 15 suites passed**, filter `MidiLoop*:LoopTakeMidi*:MidiQuantisation*`. Includes the six new tests and existing relevant seam, edit, quantisation, chase/publication, and timing coverage. |
| Latest app build | Incremental Debug x64 Jamma build passed with zero warnings/errors; `Jamma/bin/x64/Debug/Jamma.exe` updated. |
| Latest static/manual checks | `git diff --check HEAD` passed; threading-review hot-path audit passed; manual review found no new callback locks/waits/allocation/logging. |
| Latest hardware listening | User: “seems like it works!” after the latest fixes, resolving the reported double-kick audition. No new post-fix diagnostic log or precise physical timing measurement was provided. |

The full native suite was **not rerun on the latest follow-up state**. The earlier
1466-test result and independent review belong to 9ff58a2, not this current index.
The user did not specify separate post-fix quantisation-off, audio/mixed, or all
plugin/device permutations, so those are not recorded as manual passes.

## 5. Outstanding work before PR merge

### Required completion/review

1. **Review the whole branch plus current index/worktree independently.** Inspect
   overlap source-cycle provenance, FIFO timing-class pairing, connected-component
   endpoints/velocities, continuous sustains, bootstrap-head suppression, chase,
   pending-release recovery, and preservation of legitimate retriggers. Check
   prepared-array sentinel/sort handling and source edit/export/save/load semantics.
   Existing tests are evidence, not a substitute for reviewing the general helper.
2. **Review thread ownership of the new Timer sample setter.** Station's job-side
   completion stores the existing atomic sample offset, which Timer's callback
   also advances. The prior fractional setter used the same scalar pattern;
   atomic access alone does not establish a coherent timing transaction. Check
   this interaction with Tick, geometry seeding, and existing boundary publication.
   No demonstrated race failure is claimed, but this deserves explicit scrutiny.
3. **Remove the temporary diagnostics described in section 3F.** Review the removal
   diff so musical fixes remain intact. If further hardware evidence is desired,
   collect it before removal; the user requested that diagnostic additions be
   surgical and removable.
4. **Validate the final cleaned state.** Read local `.vscode/tasks.json` before
   each build/native run, follow [build guidance](build.md), use incremental
   affected-project builds, and run the relevant tests plus the complete native
   suite. Recheck app build, hot-path audit, and `git diff --check`. Record these
   results separately from the historical results above.
5. **Assess coverage and publication cost.** Useful explicit regressions include
   a configured grain with no seed, negative signed stop age, independent
   pitches/channels in finite-overlap components, and edited/restored merged
   spans with relocation and rejected releases. Existing coverage handles many
   neighbouring cases, but these exact combinations were not all newly tested.
   Assess worst-case owner-side O(N²) preparation at the 4096-event capacity.
6. Review/finalise the staged source changes and this documentation, then prepare
   commits/PR according to the user's workflow. No commit or PR was created by
   this handoff update.

### Remaining limits and optional follow-up investigation

- The current evidence identifies the split held boundary note and grid activation
  defect in the MIDI-only reproduction. It does not quantify the originally
  described ~300 ms impression or establish that every acoustic timing error has
  the same cause.
- First audio/mixed-loop capture counting and trigger-to-callback start acknowledgement
  remain separate, unproven timing suspects. They were not changed speculatively
  from this MIDI-only evidence. Later-take duration policy remains as before;
  a fresh hardware regression check is useful if the PR claims broader capture
  correctness.
- No physical ADC/DAC loopback measurement or specific Kontakt envelope/PDC
  calibration was performed. The 177-sample hardware report is not a measurement
  of total VST/acoustic latency. The committed simulated/rendered latency tests
  establish the engine model, not a physical calibration.
- General overlap merging warrants regression review across edits/restore, remote
  follow/NINJAM, master promotion, and deliberate relocation. Ordinary seams and
  small corrections preserve continuity; intentional seeks/chase and timing
  corrections can affect note envelopes outside a seam by policy.
- Direct monitoring versus recorded-path padding and dense VST3 controller
  coalescing remain the selected/documented policies, not newly resolved hardware
  fidelity questions.

## 6. Suggested fresh-agent starting point

Read this document, repo AGENTS.md, [glossary](glossary.md),
[real-time rules](realtime-audio.md), and
[loop alignment/NINJAM ownership](loop-alignment-and-ninjam-sync.md). Inspect the
committed fidelity report, then review the current diffs in Station, Timer,
LoopTake, MidiLoop and their tests. Start from the successful audition and exact
logged period/grid evidence above. Complete diagnostics cleanup, independent
review, and final validation before describing the branch as ready to merge.

## 7. Adversarial branch review (2026-10-12)

This review covered `cff0082` through HEAD and the complete index/worktree.
Both local `master` and `origin/master` have merge base `cff0082` with this
branch. No fetch was performed, so this establishes the available local PR base,
not the current remote tip. The review covered the changed engine/audio/MIDI/VST
paths, publication and lifetime protocols, capture completion, quantisation,
seam components, chase/recovery, source edit/export/restore separation, and tests.

**The branch is not validated for committing or merging yet.** The user authorised
commits once validated; three concrete review findings remain below. No commit,
index replacement, push, or PR creation was performed during this review.
The original staged work remains staged; review fixes and reproductions are
additional worktree changes. Sections 1–6 above preserve the pre-review handoff.

### Remaining findings

1. **P1 — First-master phase installation races callback advancement.**
   [Station.cpp](../JammaLib/src/engine/Station.cpp) calls
   `SetMasterLoopSampleOffset` during job-owned record completion and subsequently
   rereads `SampOffset` to initialise MIDI playback. [Timer.cpp](../JammaLib/src/utils/Timer.cpp)
   `Tick` separately loads the same offset, computes the next position, and stores
   it. A callback can load the old offset, the job can install the desired phase,
   and the callback can overwrite it with the old-derived next phase. A callback
   between the job's phase store and reread can also change the MIDI initial cursor.
   `SceneSamplePos` advances separately, before Tick stores master phase. These
   atomic fields prevent a C++ data race but do not provide a coherent transport
   transaction. This is a source-proven legal interleaving, not a measured hardware
   failure. First-master geometry, timestamped phase, and take activation need one
   callback-boundary handoff. Adding a callback mutex or merely strengthening
   atomic memory order would not resolve the ownership problem.

2. **P1 — Distinct long same-repeat articulations still share continuation identity.**
   [MidiLoop.cpp](../JammaLib/src/midi/MidiLoop.cpp) stores continuation only by
   channel/pitch. `ShouldSuppressContinuedAttack` suppresses every merged root for
   that slot while any merged root is sounding, including unrelated roots.
   Reproduction: L=1000; exact note on 0 / off 1100 at velocity 95; ordinary note
   on 50 / off 1200 at velocity 88, same channel/pitch. The spans pair independently
   by timing class and are legitimate same-repeat retriggers. Over two repeats,
   zero velocity-88 attacks play instead of two. The retained regression
   `PlaybackReviewProbe.DistinctLongSameRepeatAttacksSurvive` fails on the reviewed
   checkout. The short-retrigger fix described below does not solve this case.
   Prepared group identity must follow sorting, carry state, and chase so one
   component cannot suppress another component's articulation.

3. **P2 — Retried releases discard source release velocity.**
   [MidiLoop.cpp](../JammaLib/src/midi/MidiLoop.cpp) `RetryPendingReleases` reconstructs
   a default-velocity NoteOff from a bitset. A rejected source release with velocity
   23 is retried with velocity zero. The retained regression
   `PlaybackReviewProbe.RejectedReleasePreservesItsVelocity` fails with expected
   23 / actual 0. The Station pending-release ledger has the same reconstruction
   pattern. Instruments that use release velocity receive a different articulation
   precisely on the recovery path. Retain release velocity in fixed callback-owned
   metadata; keep synthetic flush velocity separate from source release velocity.

### Confirmed findings fixed in the worktree

- **P1 — First-master base grid survived master promotion.** A 1000/4 base grid
  kept 250-sample beat spacing after promotion to a 500-sample grain. The new
  regression failed before the fix. Preparation now stages local base-grid removal
  and applies it with the packed grid at the callback boundary; remote authority
  is preserved. `LoopTakeMidiTiming.MasterPromotionReplacesFirstMasterBaseGrid`
  covers this lifecycle.
- **P1 — Cancelled promotion left surviving takes permanently deferring grid updates.**
  Scene cancellation previously restored Quantiser/AudioHost state without clearing
  take/loop preparation. The take's update job then continued returning early.
  Cancellation now discards prepared loop publications under the existing owner
  gates, clears take preparation, and queues a normal update. The new cancellation
  regression verifies that later quantisation updates take effect.
- **P1 — Partial destination acceptance lost rejected delayed releases.** The Station
  sink previously returned OR-of-acceptance for releases. If a delayed destination's
  1024-event queue rejected a release while another destination accepted it, the
  source forgot the release and never retried the still-held binding. Release
  acceptance now requires every destination to retain it; attacks retain the
  existing any-destination acceptance policy. The new fanout regression fills one
  delayed destination, verifies rejection, then verifies release admission on retry.
- **P1 — A continuous sustain absorbed a shorter legitimate same-repeat retrigger.**
  Moving the existing interior retrigger from sample 100 to 50 caused its next-repeat
  span to overlap the long sustain, pulling it into the continuous component and
  erasing it. The new short-retrigger regression failed before the fix. A
  self-overlapping sustain now establishes its own continuity without absorbing
  another span that already overlaps it in the same source repeat. The distinct
  long-root failure above remains separately open.
- **P2 — Avoidable owner publication stall at event capacity.** The unmatched-only
  4096-event capacity regression took approximately 1778 ms in the initial Debug
  full run. Grouping finite and unmatched spans by physical channel/pitch removes
  incompatible/all-unmatched Cartesian comparisons; the same regression took
  approximately 9 ms after the change. These are whole-test timings on this host,
  not a Release benchmark or universal bound. Worst-case dense same-slot finite
  preparation still has quadratic comparison cost and remains owner-side.

### Cleanup and validation

Temporary `LogTimingDiagnostics`, completion logging arguments, and the
`[midi-timing]` completion block were removed. The existing permanent local-master
geometry log was preserved. The press-interval/phase regression now covers both
zero and configured construction grain before a master exists, and signed stop
ages −55 and +3017 samples.

- Initial incremental Debug x64 engine/native-test build: zero warnings/errors.
- Initial complete native run: 1472 tests / 179 suites; 1471 passed. The existing
  `SceneRoutingIntegrationTest.RestoresDistinctHistoriesToTriggersWithSharedStation`
  failed once (a restored history was already ditched) and passed immediately in
  isolation. No unrelated production/test change was made to hide that failure.
- Four correctness fixes plus the existing seam suite: 20 focused tests passed
  before diagnostic cleanup/grouped preparation. The cleaned configured-grain and
  signed-age matrix passed. Final complete-run results are recorded below.
- Final cleaned incremental engine/native-test build: zero warnings/errors.
- Incremental Debug x64 app build: zero warnings/errors.
- The audit script flags the newly added `_midiCaptureMutex` in the owner-only
  cancellation method. Its production caller is Scene's job pump; manual call-path
  review found no new callback lock/wait/allocation/logging. Existing snapshot
  gates and callback leases serialize cancellation against activation. Prepared
  base-grid state is written by the job owner, read/applied by the callback, and
  retained by the existing take lifetime/lease protocol.
- No fresh hardware audition, physical latency trace, remote fetch, or production
  concurrency stress validation was performed. The remaining race and failing
  regressions prevent a clean validation result and therefore prevent the
  requested conditional commit.

### Final rerun of the reviewed checkout

After moving the new owner-only grouping lookup tables from stack arrays to
heap-backed vectors, the complete Debug native suite finished normally:
**1478 tests / 180 suites; 1476 passed; two failed**, both retained adversarial
reproductions listed above. The earlier history-restoration test passed in this
run. The initial grouped-scratch attempt had caused Windows stack-overflow exit
`0xC00000FD` during `MidiLoopSeam.UnmatchedSurvivesSaveLoadAndManyIterations`;
this review-introduced stack regression was repaired before the final rerun.
No grouping scratch allocation occurs on the callback.

The final engine/native-test build and app relink succeeded. `git diff --check HEAD`
passed. The added review fixes, diagnostics cleanup, and regression evidence are
left uncommitted; the initial staged files have not been restaged or replaced.
A clean full-suite result and resolution of the remaining first-master timing,
distinct long-root continuation, and release-velocity findings are required before
calling the branch validated or performing the user's conditional commit.

## 8. Follow-up: hardware evidence and remaining review fixes

The section 7 review fixes were committed as `5ce3812f`. The user then
reported clean kick playback across the first-master seam with note
quantisation enabled and disabled, using Focusrite ASIO at 44100 Hz / 512
samples. The supplied transcript confirms trigger-spanning notes on both
recording edges and a successful ditch/reset:
`MIDI timing reset: reason=empty-local clockLength=110364`. The second
recording establishes fresh geometry (physical 99150 / logical 99148); this
master establishment is independent of the MIDI note-quantisation toggle.
The user also reports confidence that subsequent loops quantise to the master.
These auditions establish the original audible seam fix, not physical latency
accuracy or every master/cancellation lifecycle.

### Resolution of the three retained findings

- Distinct merged continuation components now carry snapshot-local root identity
  through event sorting, held state, and relocation chase. Suppression applies
  only to the sounding component. The former long-note probe is now a regression
  covering 64/1000/2000-sample partitions, source replacement, and relocation.
  This overlong synthetic fixture is not export-equivalent: the existing
  sidecar contract intentionally discards releases outside the playable window.
  Source replacement retains them; export/restore instead creates unmatched
  attacks with the existing single-slot continuation policy.
- Source release velocity is retained in fixed callback-owned arrays in MidiLoop
  and Station. Synthetic flushes use zero unless a source release is already
  pending; crossing the seam cannot overwrite its velocity. Tests cover rejected
  source releases inside a loop and at its last sample, plus plugin rejection.
- First local MIDI-only master geometry is inferred on the job owner and reserved
  without mutating the live Timer. An immutable Station request keeps the take
  and clock alive. Playback waits until its completed take and MIDI streams are
  published. The callback then installs clock geometry and the take cursor at
  the same boundary using `(SceneSamplePos - triggerOrigin) mod logicalLength`.
  Automation receives the same trigger-origin correction. Subsequent owner jobs
  use reserved geometry during this publication gap. Cancellation or an existing
  replacement master prevents stale geometry installation; surviving playback
  can still activate. This change targets the reviewed MIDI-only first-master
  path; mixed/audio recording completion keeps its existing behavior.

### Shared-state ownership audit

| State | Writer / reader | Synchronization and lifetime |
| --- | --- | --- |
| Timer reservation descriptor/version | Job publishes; job reads pending geometry; callback checks scalar version | Immutable owner snapshot; release/acquire published, consumed and cancelled versions; Timer outlives requests |
| Station activation request | Job publishes; callback borrows | PublishedAudioSnapshot lease; strong take/clock ownership; job collects retired requests |
| Activation receipt | Callback writes; job prints once | Receipt fields precede release consumed-version; job acquires before reading |
| Take activation gate/cursor | Job arms; callback applies/unfreezes; restore/record invalidate | Atomic gate; completed-state acquire; immutable take/stream leases |
| Continuation roots / pending velocities | Owner prepares roots; callback owns held/retry arrays | Fixed snapshot storage, co-permuted sort; existing snapshot publication and reclamation; no callback allocation |

The larger playback snapshots are preallocated on the heap during MidiLoop
construction, avoiding Windows stack exhaustion without callback allocation.
The hot-path audit reports no banned lock/wait additions. Manual review confirms
that request consumption uses bounded borrows, scalar atomics and existing
callback setters; allocation, reclamation and the concise `[midi-master]`
activation receipt remain on the job owner.

### Validation

- Final incremental Debug x64 engine/native-test and app builds succeeded with
  zero warnings/errors.
- Complete native suite: **1481 tests / 180 suites, all passed** (58.231 seconds).
  Both former adversarial probes pass. The timing helper covers 12 normal
  activation combinations and 18 cancellation/replacement combinations, with
  signed trigger ages and publication delays up to 200000 samples.
- Hot-path audit passed; manual ownership review found no new callback
  allocation, locking, waiting, reclamation or logging.
- `git diff --check` passed. The hardware executable is
  `Jamma/bin/x64/Debug/Jamma.exe`. Fresh hardware validation of these follow-up
  changes remains to be performed; earlier seam auditions are recorded above.

## 9. Hardware follow-up: activation verified, held-note ditch failure

The supplied follow-up transcript records first-master activation with
origin 212053, scene 297472, logical length 82640 and phase 2779:
`(297472 - 212053) mod 82640 = 2779`. The receipt appears later in
the transcript because the job owner prints it on a subsequent Station commit;
its scene field is the original callback activation coordinate. The second
Serum take has length 330560, exactly four times the first master length, and
the user reports that playback synced well. No
`Master quantisation target set` line accompanies the Shift-click, so promotion
was not established by this transcript. Recording cancellation and fresh
master establishment are present: a recording take is ditched with clockLength=0,
then its replacement completes at 77824 samples / grain 19456.

The user reports a reliably stuck Bass voice when ditching while a key is held.
The native reproduction `OrdinaryDitchReleasesSharedLiveAndPlaybackVoice`
failed in all four completed-playback combinations: compensation delay 0/256
samples, with/without plugin release rejection. Its four recording-only control
combinations passed. The source release path explains the failure: the synthetic
live NoteOff is suppressed while a recorded binding still owns the same pitch;
ordinary ditch then retires that binding with release emission disabled. The
last owner disappears without a plugin NoteOff.

Ordinary ditch now retires recorded ownership through the existing callback
release ledger with release emission enabled. The owner-side loop-held sampling
and broad synthetic playback NoteOff injection were removed. Live flushing
stays separate; recorded release targets its retained destination and respects
remaining audible/live owners. Existing queue and plugin rejection retries are
retained. No shared variables or callback locks/allocations were added.

All 42 Station MIDI tests pass after the fix, including all eight reproduction
combinations and existing source-loss, route-removal and shared-owner cases.
The hot-path audit passes. The complete native run passes **1482 tests / 180
suites** (49.980 seconds); incremental engine/test and app builds pass with
zero warnings/errors, and `git diff --check` passes. Hardware confirmation of
the held-note fix remains outstanding.
