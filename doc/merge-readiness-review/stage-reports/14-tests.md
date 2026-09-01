# Stage 14 — Unit-test quality

## Assignment

- **Primary ownership:** whether the new and changed native tests in the remote timing/NINJAM sync scope state meaningful deterministic contracts, exercise realistic boundaries, make strong assertions, diagnose failures usefully, omit required regression contracts, or duplicate lower-value simulations.
- **Explicit exclusions:** production correctness except where a test cannot observe its claimed contract; retained HUD, VST3 parity, window/tooling, unrelated MIDI/resources, and non-timing product work. This report does not propose collapsing local/remote timing, Timer absolute/device/scene coordinates, master/per-loop phase, source anchors, mapped elapsed time, follow policy, or any other protected glossary distinction.
- **Required inputs consumed:** `AGENTS.md`; `merge-readiness-plan.md`; `phase-3-maintainability-and-contracts.md`; `decisions.md`; `00-scope-and-inventory.md`; Phase 1 and Phase 2 packets; `findings.md`; `verification-matrix.md`; `cleanup-backlog.md`; Stage 7–12 reports; `doc/loop-alignment-and-ninjam-sync.md`; `doc/realtime-audio.md`; and `doc/build.md`.
- **Status:** read-only investigation at Phase 3 kickoff production tip `e72f3b0`. Only this report was added. No source, test, project, canonical artifact, build, native-test run, or cleanup implementation was performed.

## Coverage

### Commands and queries used

- `git status --short`, `git diff --name-status master...HEAD -- test/JammaLib_Tests`, and focused `git diff --stat`/`--unified=0` queries established the changed-test inventory.
- `rg --files`, `rg -n "TEST|EXPECT|ASSERT"`, and `rg -c "^TEST\\("` enumerated timing tests, assertions, and suite sizes.
- Numbered `Get-Content` reads covered every changed NINJAM timing test, `LoopTakeTiming_Tests.cpp`, the changed Timer/quantisation/automation/remote-grid/MIDI-clock seams, and the production surfaces needed to assess observability.
- `rg -n "RemotePhaseCorrectionDelta|DisciplineRemotePhase|ApplyRemotePhaseCorrection" JammaLib/src test/JammaLib_Tests/src` and project-membership queries checked whether tests exercise current code and are compiled.
- Focused `git log --oneline --follow`, `git log -S`/`-G`, `git show`, and `git blame` queries traced test intent and later refactors. Relevant commits include `aa76ab0`, `9c7ca51`, `f36bfe7`, `d0d208e`, `d6fdb88`, `b9619b9`, `b41b5a8`, `4c1c0e1`, `6abf7c7`, `e0f669c`, `0d90bac`, `bef7943`, and `e2dc3f8`.
- No build or test command was run. Phase 2 already recorded an incremental native baseline of 821/822 passed with one hardware skip, and explicitly states that the accepted blockers are not covered.

### Actual coverage

Deep review covered all 12 timing-focused new test files plus the changed Timer, quantisation, automation-anchor, remote-grid, and MIDI-clock tests:

- `test/JammaLib_Tests/src/ninjam/{NinjamTiming,NinjamTimingTracker,NinjamTimingCoordinator,NinjamAudioTimingCommand,NinjamTimingObservationMailbox,NinjamTimingIntegration,NinjamMetronomeTiming,ExportLaneTiming}_Tests.cpp`;
- `test/JammaLib_Tests/src/engine/LoopTakeTiming_Tests.cpp`, `Timer_Tests.cpp`, and the timing-specific additions in `Quantisation_Tests.cpp`;
- `test/JammaLib_Tests/src/timing/RemotePhaseDiscipline_Tests.cpp`;
- timing-specific additions in `MidiAutomationLaneResolution_Tests.cpp`, `MidiQuantisation_Tests.cpp`, and `MidiTimestampMapper_Tests.cpp`;
- `test/JammaLib_Tests/src/audio/NinjamMetronome_Tests.cpp` and native-project membership;
- observable production seams in `AudioHost.cpp`, `LoopTake.cpp`, `Timer`, coordinator/tracker, timing value helpers, connection/session state, and command/observation publication.

Deliberate exclusions were the retained HUD/graphics tests, VST3 tests, window persistence, unrelated MIDI queue/router/velocity changes, generic audio-buffer tests, and project/build quality beyond proving whether an in-scope test is compiled. Hardware, network, ASIO, sanitizer, profiler, and manual-session evidence were not executed. Export-lane tests were assessed only as the conditional evidence for F-017; the disabled feature itself was not judged.

## System understanding

The useful test pyramid already has strong lower layers. Pure timing helpers cover circular deltas, sample-rate conversion, source-map arithmetic, metronome boundaries, and validity. Coordinator tests cover prompt/request/follow-policy happy paths with an injected clock for deadline boundaries. Real `LoopTake` tests construct playable audio loops and MIDI cursors, then assert independent lengths/origins, exact wrap, automation counter-translation, restore-before-rebase, local offsets, and long deterministic advancement (`LoopTakeTiming_Tests.cpp:250`–`:475`). Timer tests distinguish the monotonic scene coordinate from resettable musical geometry (`Timer_Tests.cpp:88`–`:104`). These are meaningful contracts and should be retained as the stable prerequisite layer.

The upper layer is not production-faithful. `TransportHarness` describes itself as exactly mirroring AudioHost (`NinjamTimingIntegration_Tests.cpp:12`–`:24`, `:124`–`:126`), but it applies invalidation to every model take (`:142`–`:157`), resets the model generation (`:194`–`:200`), and accepts equal generations (`:202`–`:207`). Production AudioHost deliberately skips the entire station fan-out when `disablesSync` is true (`JammaLib/src/audio/AudioHost.cpp:174`–`:187`, `:298`–`:320`), while real `LoopTake` rejects equal generations (`JammaLib/src/engine/LoopTake.cpp:524`–`:534`). The model also stores `Length` but never wraps or otherwise uses it (`NinjamTimingIntegration_Tests.cpp:42`–`:48`, `:178`–`:180`). Consequently its reconnect and relative-offset assertions do not observe the current production path.

That matters because the approved cleanup direction is no longer the current delta-command language. The human decision requires one latest **complete desired remote transport state** containing session epoch, policy, full geometry, and timestamped phase. Existing command tests instead declare that two current `ReplaceTiming` values coalesce (`NinjamAudioTimingCommand_Tests.cpp:55`–`:68`) and separately prove only that an invalidation value can be read (`:71`–`:81`). They never test `Invalidate -> Replace`, `Replace -> Discipline`, a complete state superseding another state, or the last-applied session epoch.

### Test-to-contract map

| Test surface | Contract usefully covered now | Boundary/diagnostic quality | Missing, misleading, or duplicate contract |
| --- | --- | --- | --- |
| `NinjamTiming_Tests.cpp:84`–`:120`, `:123`–`:257` | Source-map ratios, signed circular ties, replacement projection, conversion, and ingress validity | Exact integer assertions and good half-interval examples | `:150`–`:160` treats valid zero as an unusable anchor; no 96→48 tail, non-integer ratio, non-finite BPM, or explicit absent-vs-zero value |
| `NinjamTimingTracker_Tests.cpp:16`–`:52` | Generation/wrap/join events, duplicate/backward rejection, explicit disconnect | Small deterministic transition tests | No availability epoch or valid→invalid→valid contract; these require the integration owner rather than more tracker-only simulation |
| `NinjamTimingCoordinator_Tests.cpp:42`–`:240`, `:242`–`:546` | Prompt/accept/reject, follow boundary, request acknowledgement/retry/expiry, generation, join bounds | Injected time at `:517`–`:546` is deterministic; many state assertions are strong | Deadline tests keep supplying valid observations; no periodic no-observation deadline, post-authority invalidity, delayed initial observation, fresh physical epoch, or idempotent repeated loss |
| `NinjamAudioTimingCommand_Tests.cpp:32`–`:94` | Current mailbox empty/once/latest/explicit-zero mechanics | Exact field assertions | Encodes the superseded delta-command contract and never proves the approved complete desired state or cross-session coalescing |
| `NinjamAudioTimingCommand_Tests.cpp:96`–`:240` | Pure anchor helpers and Timer command generation/wrap | Useful narrow assertions | Timer-only invalidation cannot prove station/take invalidation or AudioHost ordering |
| `NinjamTimingObservationMailbox_Tests.cpp:7`–`:67` | Complete single-thread publication and latest replacement | Every field has a sentinel relation | The only concurrent test can execute zero reads and still pass (`:69`–`:119`) |
| `NinjamTimingIntegration_Tests.cpp:240`–`:649` | Coordinator/helper/mailbox composition and telemetry arithmetic | Easy deterministic scenarios | Pseudo-takes duplicate real `LoopTake` coverage while bypassing production invalidation, equality, wrapping, station fan-out, map ownership, and session state; reconnect at generation 6 (`:509`–`:519`) is unlike coordinator reset to 1 |
| `LoopTakeTiming_Tests.cpp:250`–`:475` | Real audio/MIDI correction, independent lengths/origins, both follow policies, source-map restore/rebase, local offset, disconnected free-run | Strong exact cursor assertions; the long simulation reports interval and length (`:452`–`:475`) | Older queued-correction reconnect tests (`:426`–`:449`) do not observe direct AudioHost command invalidation; no real two-session generation-1 production path |
| `Timer_Tests.cpp:69`–`:123` | Consume-once ordering, scene monotonicity, Timer-only invalidation | Exact small-value assertions | No `UINT32_MAX` crossing, widened multiplication, near-max Tick, or coherent Timer tuple |
| `Quantisation_Tests.cpp:17`–`:49`; `MidiQuantisation_Tests.cpp:277`–`:291` | Local grain geometry and direct active/remote grid boundaries | Endpoint assertions preserve glossary distinctions | Not a substitute for end-to-end remote geometry publication or `NoSync` invalidation |
| `MidiAutomationLaneResolution_Tests.cpp:176`–`:267`; `MidiTimestampMapper_Tests.cpp:42`–`:92` | Automation anchor translation and modular device-clock block boundaries | Exact wrap/full-loop assertions | No integrated Timer/device/scene sentinel test or audio/MIDI epoch reset |
| Metronome suites (`NinjamMetronomeTiming_Tests.cpp:5`–`:176`; `NinjamMetronome_Tests.cpp:8`–`:39`) | Beat/accent ordering, generation reset, latency, real output-channel onset | Focused deterministic DSP/timing assertions | Valuable but not prerequisite coverage for authority/map refactors |
| `ExportLaneTiming_Tests.cpp:27`–`:212` | Pure disabled export-lane formula/reset/anomaly behavior | Exact formula sequences | Conditional evidence only; exclude from lean suite unless F-017 path is retained/enabled |
| `RemotePhaseDiscipline_Tests.cpp:11`–`:156` | Obsolete Quantiser phase-discipline API | None in the native executable | Removed from the project in `f36bfe7`; current APIs are absent; duplicates later coordinator/timing contracts |

### Proposed lean minimum regression suite

The suite below deliberately selects **one or two focused prerequisites for each major refactor**, as required by the human gate. Tests should be landed in a separate Stage/Finding commit and normally observed passing before the corresponding production refactor. When the approved behavior does not yet exist, the Phase 4 characterization-first exception permits an exact expected pre-fix failure before bounded implementation, but requires green focused verification before independent approval or dependent work. Existing strong helper tests remain supporting evidence rather than being copied into new simulations.

| Refactor group | Passing prerequisites before source movement | Essential assertions |
| --- | --- | --- |
| Complete desired state, one integration owner, neutral engine policy (F-005/F-009/F-024 direction) | **P1:** production-boundary `Invalidate -> Replace -> Discipline` coalescing test using complete desired states. **P2:** overlapping job/UI intents resolve through one producer while the audio reader sees only a complete state. | Final Timer geometry/policy/epoch/phase and map are one authority; no lost invalidation; no mixed fields; one state applies once; no callback lock/allocation |
| Common map consolidation and reconnect generation (F-006/F-018/F-025) | **P3:** two real local takes (audio + MIDI), unequal lengths and intentional offsets, session 1 generation >1, production invalidation, session 2 epoch with generation 1. **P4:** restore-before-rebase at a delayed boundary. | Every gate resets once; stale session-1 state is rejected; session-2 state applies once; common mapped elapsed wraps independently; offsets and MIDI automation phase remain invariant |
| Remote observation/local Timer publication (F-021/F-023/F-026/F-029/F-030/F-031) | **P5:** synchronized alternating-sentinel publication test for complete remote and local transport values. **P6:** one table-driven numeric test combining zero-origin translation, `UINT32_MAX` Timer crossing, and 96→48 final-source-sample conversion as distinct subcases. | At least one concurrent overlap is proven; no mixed tuples; zero is valid and absent is explicit; delayed initial join is translation-invariant; Timer absolute remains monotonic; converted phase stays `< length` and does not wrap early |
| Empty/reset, loss/retry, invalid/absent timing (F-022/F-027/F-028) | **P7:** production-faithful availability fixture drives connected → retrying → connected and connected → failed. **P8:** no-observation job ticks cross the request deadline, plus valid → invalid → repeated invalid → valid. | Exactly one loss invalidation; connected-empty preserves accepted timing; disconnected final-take removal clears once; prompt/request/stations/authority clear; local state free-runs; recovery requires fresh epoch/valid observation |
| Scoped remote buffer lifetime (F-033) | **P9:** controlled Stop/disconnect attempt after connection-use acquisition but before synchronous stereo consumption. | Stop cannot retire storage until consumption returns; no raw borrow escapes; fixed/preallocated buffers only; repeat under ASan/page heap/Application Verifier when available |
| Logging/dead receipts/Scene-Station slimming (F-019/F-034) | **P10:** disabled diagnostics exercise the same join path with zero captured work; enabled bounded capture emits one off-thread record with epoch/generation/station/take identifiers. | No callback formatter/I/O/allocation; dead receipt absence does not remove applied-state diagnosis; volume is bounded |
| Mechanical naming/moves and local-offset owner (F-007/F-008/F-010–F-017/F-032) | Select the single closest existing passing helper test plus, only where behavior changes, one focused new boundary test (non-finite BPM for F-032). | Retired-name/include audit plus unchanged semantic values; no duplicated umbrella simulation |

The irreducible pre-refactor core is P1–P8. P9 is required before the lifetime fix, and P10 before deleting/moving diagnostics. Metronome, export, UI, VST, window, and unrelated MIDI tests are not part of this lean timing regression set.

## Candidate findings

### S14-01 — Delete the uncompiled obsolete remote-phase test suite

- **Stage / reviewer:** Stage 14 — unit-test quality.
- **Scope reviewed / exclusions:** test reachability and duplication only; no production deletion is proposed.
- **Severity:** follow-up.
- **Evidence:** `test/JammaLib_Tests/src/timing/RemotePhaseDiscipline_Tests.cpp:11`–`:156` contains 16 tests calling `Quantiser::RemotePhaseCorrectionDelta`, `DisciplineRemotePhase`, and `ApplyRemotePhaseCorrection`, but those symbols have no current production definition or call site. The current native project source list around `test/JammaLib_Tests/JammaLib_Tests.vcxproj:204`–`:216` omits the file. Commit `aa76ab0` added the suite; `f36bfe7` explicitly removed it from the project while moving coverage to coordinator/tracker tests; `e2dc3f8` later mechanically rewrote the already-uncompiled file. Its pure circular-delta and discipline cases are now covered through current `NinjamTiming`/coordinator/Timer tests.
- **Why it matters:** 156 lines look like passing regression evidence but are neither compiled nor compatible with the current owner model. They inflate apparent coverage and preserve the superseded Quantiser-owned remote-discipline vocabulary.
- **Recommended disposition:** remove this test file. Do not re-register it or recreate the removed Quantiser APIs. Retain current-owner coverage in `NinjamTiming_Tests`, coordinator tests, and the real `LoopTake` boundary.
- **Protected timing concepts affected:** none; deletion removes a superseded owner without merging phase, coordinate, or policy concepts.
- **Verification:** native-project membership/search audit; incremental native build; confirm current circular-delta, coordinator, Timer, and LoopTake tests remain registered and passing.
- **Human decision:** pending.

### S14-02 — Replace the pseudo-integration harness with a production-faithful audio-boundary contract

- **Stage / reviewer:** Stage 14 — unit-test quality.
- **Scope reviewed / exclusions:** whether tests observe AudioHost/LoopTake behavior; the production bug remains F-025 and the complete-state implementation remains the approved F-024 direction.
- **Severity:** must fix before merge.
- **Evidence:** the harness claims exact AudioHost fan-out at `test/JammaLib_Tests/src/ninjam/NinjamTimingIntegration_Tests.cpp:12`–`:24` and `:124`–`:126`, but it always forwards invalidation and resets model gates at `:142`–`:157`, `:194`–`:200`. Production skips take fan-out for invalidation at `JammaLib/src/audio/AudioHost.cpp:174`–`:187`, `:298`–`:320`. The model accepts equal generations (`NinjamTimingIntegration_Tests.cpp:202`–`:207`) while real `LoopTake` rejects them (`LoopTake.cpp:524`–`:534`), never uses `ModelTake::Length` for wrapping (`NinjamTimingIntegration_Tests.cpp:42`–`:48`, `:178`–`:180`), and reconnects at generation 6 after generation 5 (`:509`–`:519`) instead of exercising a fresh session starting at 1. Commits `b9619b9`, `b41b5a8`, `4c1c0e1`, and `6abf7c7` expanded this model, while real LoopTake coverage arrived in `e0f669c`/`0d90bac`/`bef7943`.
- **Why it matters:** the suite passes precisely because its model performs the missing production behavior. It therefore cannot be a prerequisite for F-005/F-006/F-009/F-025 and duplicates lower-value unwrapped position assertions already covered more realistically by `LoopTakeTiming_Tests.cpp:250`–`:475`.
- **Recommended disposition:** create a narrow production-owned seam in an existing owner (not a new class) that applies one consumed/desired timing state to Timer, AudioHost map, Station, and real LoopTake objects. Move the two-session reconnect and complete-state boundary tests to that seam. Retain only coordinator/telemetry simulations that exercise distinct off-thread policy contracts, and remove duplicated `ModelTake` phase tests.
- **Protected timing concepts affected:** session epoch, follow policy, Timer geometry, common sync map, per-entity anchors/lengths, audio phase, MIDI event cursor, automation origin, and `NoSync` invalidation must remain independently asserted.
- **Verification:** P1–P4 above; assert unequal lengths/offsets, generation-1 reconnect, equal/stale rejection, audio/MIDI/map/Timer coherence, and exactly-once invalidation through the same branch production uses.
- **Human decision:** pending.

### S14-03 — Rewrite tests that preserve rejected command and zero-anchor semantics

- **Stage / reviewer:** Stage 14 — unit-test quality.
- **Scope reviewed / exclusions:** truth of expected behavior in tests; production changes remain F-024/F-030 and no implementation is authorized here.
- **Severity:** must fix before merge.
- **Evidence:** `NinjamAudioTimingCommand_Tests.cpp:55`–`:68` names and asserts current latest-delta command coalescing but covers only two self-contained replacement commands; `:71`–`:81` proves invalidation only when it is not overwritten. Human decisions rejected that command-language direction in favor of a latest complete desired remote state. Separately, `NinjamTiming_Tests.cpp:150`–`:160` asserts that observation coordinate zero disables projection, and `:188`–`:198` relies on the same fallback, even though accepted F-030 requires zero to remain valid and absence to be explicit. These expectations originated in `d6fdb88`/`b41b5a8` and now contradict the accepted gate.
- **Why it matters:** keeping these tests unchanged would make the approved correction fail by design, encourage a compatibility shim for behavior explicitly rejected by the human reviewer, and leave the dangerous `Invalidate -> Replace` / `Replace -> Discipline` cases untested.
- **Recommended disposition:** rewrite the command tests around the complete desired-state value and session epoch, including supersession across invalidation/replacement/discipline intent. Replace zero-fallback expectations with a table that distinguishes valid zero, explicit absence, and nonzero anchors and proves translation invariance. Do not delete the useful latest-value or projection contracts; change their value semantics.
- **Protected timing concepts affected:** desired authority state is not a shared cursor; session epoch, policy, remote geometry, timestamped remote phase, device-audio anchor, and Timer-absolute anchor remain distinct fields.
- **Verification:** P1, P2, and P6; include zero/nonzero translated pairs, truly absent anchors, both command orderings, and final applied epoch/geometry/phase assertions.
- **Human decision:** pending.

### S14-04 — Make the mailbox concurrency test prove that overlap occurred

- **Stage / reviewer:** Stage 14 — unit-test quality.
- **Scope reviewed / exclusions:** deterministic test construction; the mailbox memory model remains F-021/F-023 ownership.
- **Severity:** must fix before merge.
- **Evidence:** `NinjamTimingObservationMailbox_Tests.cpp:69`–`:119` starts a writer and reads only while `writerFinished` is false. If the writer completes before the test thread first evaluates the loop, the body executes zero times and every coherence assertion is skipped; there is no start barrier, successful-read counter, or final sentinel assertion. The test was added/refined by `d0d208e`/`f36bfe7`/`b41b5a8`. The command mailbox tests are entirely sequential (`NinjamAudioTimingCommand_Tests.cpp:32`–`:94`), so no other native test guarantees overlapping publication/read.
- **Why it matters:** this is the only claimed concurrent complete-or-deferred proof for the timing publication pattern. It can pass vacuously and is not a reliable prerequisite for moving NJClient observations or publishing a coherent Timer tuple.
- **Recommended disposition:** use a deterministic start barrier/latch, keep the writer alive until the reader has attempted reads, record at least one completed sentinel observation, and assert a nonzero overlap/read count after join. For the selected single-producer contract, use related incompatible sentinels across every field so a torn tuple is diagnosable by generation.
- **Protected timing concepts affected:** remote observation and local Timer transport must use separate complete values; this test must not merge their coordinates.
- **Verification:** P5; run repeatedly and under race tooling where practical, with failure messages including sequence/generation and every mismatched field.
- **Human decision:** pending.

### S14-05 — Add controllable connection-loss and buffer-borrow test seams before lifecycle fixes

- **Stage / reviewer:** Stage 14 — unit-test quality.
- **Scope reviewed / exclusions:** absence and observability of native contracts; connection recovery correctness is F-027/F-028 and lifetime correctness is F-033.
- **Severity:** must fix before merge.
- **Evidence:** there is no focused `NinjamSession` or `NinjamConnection` native test in `test/JammaLib_Tests/src`; the native project list at `JammaLib_Tests.vcxproj:204`–`:216` contains helper/coordinator/integration tests only. Coordinator disconnect coverage calls `Disconnect()` directly (`NinjamTimingCoordinator_Tests.cpp:229`–`:240`), and deadline coverage continues supplying valid observations (`:517`–`:546`). No test drives physical `Connected -> Retrying/Failed -> Connected`, absence after accepted authority, or a stop between acquiring and consuming the connection-owned stereo buffer. Stages 10 and 12 explicitly handed these gaps to Stage 14.
- **Why it matters:** the highest-risk recovery and lifetime refactors otherwise have no deterministic passing prerequisite. A helper-only coordinator test cannot expose the missing Session→Scene availability edge, and a normal start/stop smoke test cannot force the vulnerable borrow interval.
- **Recommended disposition:** add injectable/fake state and clock hooks to an existing connection/session/integration owner, plus a test-only synchronization hook around the scoped synchronous consume boundary. Keep the production callback allocation-free and do not expose a general raw connection/buffer API. Use these seams for P7–P9, not a broad network emulator.
- **Protected timing concepts affected:** physical availability, session epoch, remote timing validity, follow authority, local free-run, and remote audio-buffer lifetime remain separate contracts.
- **Verification:** exactly-once loss invalidation, fresh-epoch recovery, no-observation deadline, repeated-invalid idempotence, and controlled stop-during-consume; then ASan/page heap/Application Verifier and manual reconnect scenarios when available.
- **Human decision:** pending.

## Handoffs

- **Stage 13 / Phase 4 batching:** rank S14-01 as a direct 156-line timing-test deletion. Treat S14-02's removal of duplicated `ModelTake` scenarios as conditional on P1–P4 passing through the production seam; do not delete real `LoopTake` coverage.
- **Stage 15:** correct the false “exactly”/“mirrors” test descriptions at `NinjamTimingIntegration_Tests.cpp:12`–`:24`, `:73`–`:75`, and `:124`–`:126`, and the zero-anchor/old-command test names, coordinated with S14-02/S14-03 rather than as comment-only edits.
- **Stage 17:** P10 should validate the retained symptom-to-signal design: epoch/generation, desired/applied authority, station/take identity, and bounded off-thread formatting. Telemetry-only model tests at `NinjamTimingIntegration_Tests.cpp:522`–`:649` should survive only if they observe retained signals unavailable from the production seam.
- **Stage 18:** own hostile/non-finite input breadth. Stage 14's lean suite needs only the accepted F-031 tail-conversion and F-032 non-finite prerequisite cases, not a broad parser/security matrix.
- **Phase 4:** commit each prerequisite test independently before its production finding/refactor commit, with Stage/Finding IDs as the human requested. Do not batch P1–P10 into one umbrella test commit; select the one or two rows attached to the next major refactor.

## Uncertainties

- `AudioHost::_OnAudio` is private and tightly coupled to device buffers. The best production-faithful seam may be a private existing-owner helper plus friend fixture, or a narrower current-owner function extracted during the prerequisite-test commit. This report requires observable production semantics but does not prescribe a new class or public test API.
- Deterministic NJClient fault injection may require a small adapter at the existing `NinjamConnection`/session boundary. Upstream NJClient remains read-only, and no proposed test should modify it or invoke unsupported methods from the callback.
- `NinjamTimingIntegration` telemetry tests may retain distinct value after the phase model is removed. Stage 17 should decide this against the final diagnostic design; S14 does not pre-authorize their deletion.
- Concurrency unit tests cannot prove absence of all memory-model or lifetime failures. The corrected P5/P9 tests are prerequisites; race/sanitizer/page-heap evidence and manual driver/network scenarios remain necessary verification layers.
- No native run was performed in this read-only stage. The Phase 2 821/822 baseline is accepted only as the current execution record, not evidence that any S14 gap is closed.

## Conclusion

The branch has substantial and often strong deterministic lower-level timing coverage, especially for real `LoopTake` audio/MIDI alignment, source-map arithmetic, Timer scene monotonicity, coordinator happy paths, metronome boundaries, and modular MIDI timing. The protected timing glossary is represented well in those focused tests and must remain explicit.

Stage 14 found five candidates: one direct obsolete-test deletion (S14-01) and four must-fix test-contract gaps (S14-02 through S14-05). The current upper-layer “integration” evidence is not production-faithful, two tests preserve semantics already rejected by the human gate, the sole timing-mailbox concurrency test can pass without exercising concurrency, and connection-loss/buffer-lifetime refactors lack controllable native prerequisites. Phase 4 cleanup must therefore start with the selected one or two tests from P1–P10 for each major refactor in their own Stage/Finding commit. They normally pass before production movement; when they truthfully characterize not-yet-implemented approved behavior, record the expected failure before bounded implementation and require them green before review or dependent work. No cleanup is authorized or implemented by this report.
