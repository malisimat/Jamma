# Verification matrix

Phase 1 added obligations only. During Phase 2 integration, the local `.vscode/tasks.json` and `doc/build.md` were read, the authoritative incremental `JammaLib_Tests` target built successfully through `.github/skills/builder/invoke-msbuild.ps1`, and the full native executable passed 821 of 822 tests; `MidiDevice.OpensPreferredDeviceWhenAvailable` was the sole hardware-dependent skip. No interactive Jamma/NINJAM runtime scenario, race sanitizer, profiler, ASan, page heap, or Application Verifier run was executed. The passing suite does not cover the blockers below.

| Finding | Build/static evidence | Native test evidence | Runtime/manual evidence | Later owner/sign-off |
| --- | --- | --- | --- | --- |
| F-001 | Regenerate diff/project/resource inventory if HUD moves | Retained HUD/graphics tests | Trigger/station/HUD rendering | Human scope gate; Stages 12/14/16/20/21 |
| F-002 | Regenerate diff/interface inventory if VST moves | VST3 mapping/state plus affected engine tests | VST2/VST3 load, editor, MIDI, state | Human scope gate; Stages 12/14/16/20/21 |
| F-003 | Affected app build if retained | Persistence tests | Multi-monitor window save/restore | Human scope gate; Stage 16 |
| F-004 | Validate task commands and wrapper if retained | Applicable native target invocation | None | Human scope gate; Stage 21 |
| F-005 | Include/dependency audit; no `engine -> ninjam` policy edge; JammaLib build | Neutral correction for Continuous/Block; explicit epoch reset; `NoSync` moves no cursor | Reconnect and `NoSync` invalidation with unequal audio/MIDI lengths/offsets | Stages 13/20; depends on complete state and F-025 regression |
| F-006 | JammaLib build; one AudioHost map owner; one common calculation per block independent of take count | Unequal audio/MIDI lengths and offsets; rebase without anchor recapture; wrap | Join, reconnect, free-run after `NoSync`; maximum-hierarchy callback benchmark | Stages 13/20; after F-025 epoch regression |
| F-007 | Include-graph and JammaLib build | Coordinator/local timing tests | None | Stages 13/20 |
| F-008 | JammaLib build; include audit | Latest/zero mailbox and local-offset tests | Disconnected and `NoSync` offset control | Stages 7/8/9/20 |
| F-009 | JammaLib build; one explicit integration producer; Scene has no transport-state construction | Overlapping job/UI intent resolves to one complete desired value; coherent reader; final state applies once | Join/leave trace correlates one integration owner with AudioHost desired/applied versions | Stages 13/20; coupled to F-024 prerequisites |
| F-010 | JammaLib/tests compile; identifier audit | Affected timing tests | None | Stage 15/20 |
| F-011 | JammaLib/tests compile; retired-name audit | Export/metronome helper tests | None | Stages 14/15/20 |
| F-012 | Incremental JammaLib link/build | Timing/command/mailbox tests | None | Stages 7/8/20 |
| F-013 | Symbol and UI-string audit | Coordinator/quantisation tests | Remote-tempo prompt wording | Stages 9/15/20 |
| F-014 | JammaLib/tests compile; coordinate-name audit | Command and integration simulations | Trace replace + discipline coordinates | Stages 9/11/15/20 |
| F-015 | JammaLib/tests compile | Distinct Timer/device sentinel tests; MIDI timestamp tests | Late-observation scenario | Stages 9/11/15/20 |
| F-016 | JammaLib/tests compile; usage audit | MIDI automation/timing tests | Automation playback across correction | Stages 9/14/15/20 |
| F-017 | JammaLib/tests compile | Export-lane pure-helper tests | Only if compensation path is retained/enabled | Stages 8/13/15/20 |
| F-018 | Incremental JammaLib/test build | Sync-map, offsets, reconnect, `NoSync` tests | Focused remote join/local loop | Stages 9/20 |
| F-019 | Incremental JammaLib/test build | Existing timing/logging-adjacent tests | Normal and verbose logging; remote join | Stages 8/17/20 |
| F-020 | Cleanup-only diff proves no HUD/resource edit under G3-1 | No cleanup test required | Retained HUD behavior remains a final branch regression sentinel only | Accepted no-change/out-of-scope residual; final diff audit |
| F-021 | Audit `_OnAudio`: NJClient `AudioProc` only; no retired live-timing adapters; no callback lock/allocation | Deterministic overlapping sentinel generations; exact pre-advance observation; delayed projection | Join/reconnect/disconnect/export while network pump runs; race tooling | Stage 13/14 prerequisite before F-009/F-015; Phase 4/20 |
| F-022 | Callback audit excludes `_ClearTimingState`, raw `_stations`, locks and destruction | Empty connected preserves accepted timing; final local take removed while disconnected clears once; race against grid/connect transitions | Remove final take and reconnect while audio runs; RT profiler | Prerequisite empty-transition contracts; Phase 4 + Stages 14/20 |
| F-023 | Versioned local-transport value audit | Alternating length/count/phase sentinels never form mixed tuple | Tempo replacement while job processing is delayed | Prerequisite coherent-snapshot test; coordinate with F-029 |
| F-024 | Desired-state type/call-site audit; no standalone invalidation/delta/ordered queue | Former `Invalidate -> Replace` and `Replace -> Discipline` intents resolve to one complete state; same-state idempotence; epoch/geometry/phase comparisons; Timer/map/audio/MIDI/gates | Stress producer faster than callback; correlate desired/applied complete authority | Highest-priority P1/P2 prerequisite before F-005/F-009; Stages 13–15/20 |
| F-025 | Audit invalidation reaches every generation gate | Two sessions: first generation >1, production invalidate, second starts at 1; stale old command rejected | Disconnect/free-run/reconnect with unequal lengths/offsets | Highest-priority prerequisite regression before authority refactor |
| F-026 | Observation-coordinate audit | Initial join processed at zero vs multiple-block delay emits identical delta | Populated same-tempo join under job-thread delay | Prerequisite delayed-initial-observation test |
| F-027 | Availability/epoch audit; persisted and interactive starts share one integration lifecycle | Loss/retry creates exactly one `NoSync` and fresh epoch; persisted config starts coordinator before first observation | Default `.jam` launch, server/cable loss, retries, manual disconnect, station cleanup | Stages 14/16/20; Phase 4 |
| F-028 | Deadline/state-table audit | No-observation timeout; valid→invalid→valid; malformed zero/rate/BPM/BPI | Loss of timing while socket remains available | Phase 4 + Stages 14/20 |
| F-029 | 64-bit symbol/type audit | Cross `UINT32_MAX`; near-max seed/phase + Tick increment; multiply before widening; distinct Timer/device sentinels | Run/simulate >24 h 51 min at 48 kHz | Coordinate with F-023; Stage 20 |
| F-030 | No zero-as-sentinel audit | Independently test valid zero vs absent vs nonzero for device-audio and Timer-absolute anchors; delayed first-block commands | Join immediately after audio start/reconnect | Stage 14/20 |
| F-031 | Conversion bounds/rounding audit | Exhaust source tails and small ratios; 96→48 and non-integer ratios never wrap early | Remote join near interval tail at differing sample rates | Stage 14/20 |
| F-032 | B002 `e0d60f2d` shares finite/plausible validation at conversion and as the first egress statement; invalid return precedes arithmetic, formatting, strings, lock, four sends, log, and authority mutation | Corrected P12 `93594c5a` directly covers helper-zero, endpoints, saturation, formatting, and disconnected request false-result; focused filter 5/5 | Dynamic send count not instrumented because an approved connection/NJClient seam is absent; malformed manual scenario remains Stage 21 | Initial review rejected policy-only P12 at `bd21e1de`; corrected batch approved at `a7bfff89` |
| F-033 | B001 at `996bda2a` spans guarded acquisition through ingestion; `5fd1b806` fixes the review-found entry race with count-before-load sequential consistency; no callback allocation/lock/wait/I/O/formatting/final destruction | P9 registered/passed pre-movement at `9b46803e`; overlap correction `73af93c2`; final P9 1/1 plus 10/10 repetitions covering 640 overlap epochs; ingestion 2/2 | Live-server reconnect and ASan/page heap/Application Verifier remain explicit Stage 21 limitations | Initial review rejected at `b9aed357`; corrected batch independently approved at `620447c2` |
| F-034 | No callback formatter/I/O/hierarchy traversal; zero disabled work; bounded fixed record | Capacity/overflow, epoch + desired/applied correlation, dead-receipt removal, logging on/off phase equivalence | Normal/verbose join/record/overdub/Stay-local/disconnect/reconnect trace; profiler | Coordinate F-019; Stages 13/17/20 before batch |
| F-035 | Private-helper call-site audit; queued path diff unchanged | Signed/zero shift across unequal audio, MIDI-only/audio-only/empty; automation sign and local-offset accounting | Reconnect/`NoSync` confirms direct helper is not invoked | Stage 13/20; after F-005 boundary shape |
| F-036 | One proposal identity implementation; retired comparison audit | Same geometry/new observation stable; each geometry/policy field change detected | Prompt persists/replaces correctly and clears on reconnect | Stage 13/20; coordinate F-013 |
| F-037 | File/project/retired-symbol audit; incremental tests build | Current timing/coordinator/Timer/LoopTake suites remain registered and passing | None | Stage 14/20; direct 156-line deletion |
| F-038 | Production-seam call audit; model omissions no longer claim end-to-end fidelity | P1–P4: complete state, real unequal audio/MIDI takes, two-session generation-1 reconnect, restore-before-rebase | Production-faithful join/reconnect trace | Stage 14/20; prerequisite before F-005/F-006/F-009/F-025 |
| F-039 | Retired command/sentinel expectation audit | Complete-state supersession; valid zero vs explicit absent vs nonzero in both clock domains; delayed first block | First-block join/reconnect trace | Stage 14/20; coordinate F-024/F-030 |
| F-040 | Deterministic overlap/start-barrier audit | P5 requires nonzero overlapping complete reads with incompatible field sentinels and generation diagnostics | Repeated run plus race tooling where practical | Stage 14/20; prerequisite F-021/F-023 publication changes |
| F-041 | B001 registered an existing-owner scoped-use seam with no raw-buffer test API; P7/P8 lifecycle surfaces remain B005 | P9 passed alone before production movement; corrected overlap coverage passed 640 epochs; P7/P8 pending B005 | Sanitizer/page heap/Application Verifier plus manual reconnect remain Stage 21/B005 evidence | Buffer-borrow portion independently approved at `620447c2`; lifecycle portion remains B005 |
| F-042 | Statement/source and ownership audit after contract fixes | Relevant F-021/F-024/F-025/F-027/F-028 tests linked, not duplicated | `Stay local`, join and reconnect guide trace | Stage 15/20; docs update with implementation batch |
| F-043 | Statement-by-statement implemented/residual audit against `e72f3b0+` | Link remote-grid/BPI/direct-boundary tests; preserve unverified status | Overlay/manual remote-grid cases only when executed | Stage 15/20; docs-only correction after contracts settle |
| F-044 | Migration-policy/schema audit | Signed values/endpoints across `M`, `2M`, non-divisor audio/MIDI lengths; compare deltas and round trip | Load affected session; join/`NoSync` does not reinterpret offset | Human compatibility gate; Stages 16/20 |
| F-045 | Confirm no schema/version guard is added | Missing field defaults to zero; current round-trip endpoints/fractions | Older-binary resave loss is an accepted residual | Decision-resolved/no implementation; document with F-044 |
| F-046 | Producer/consumer contract and retired-fallback audit | Full plausible BPI accepted; BPI absent makes no authority change; supplied BPI retained | Remote prompt/grid trace uses authoritative BPI | Human compatibility gate; Stages 15/16/20 |
| F-047 | Existing-value-field and no-parallel-logger audit | Every rejection reason/counter; epoch + desired/applied version; bounded lag/suppression | Readable geometry/discipline/NoSync/loss/retry trace without hierarchy dumps | Stages 17/20; implement with F-024–F-028/F-032 |
| F-048 | B003 `749ff1ca` routes target maximum through existing `_RoundedToUInt` saturation before Windows-width widening; no remaining direct target cast | P13 + derivation 2/2; `Quantisation.*` 33/33; related UserConfig timing 3/3; finite/nonzero coherent geometry asserted | Parser/manual extreme config and no-malformed-request runtime remain Stage 21 limitations | B003 implemented/verified; independent review pending |
| F-049 | Cleanup-only diff proves no password-serialization change | No cleanup test required | Cleartext portable export remains an accepted residual risk | Rejected/no implementation; final risk statement |
| F-050 | Cleanup-only diff proves no work-directory policy change | No cleanup test required | Portable work-directory authority remains an accepted residual risk | Rejected/no implementation; final risk statement |

Cross-phase obligations not represented as Phase 1 deletions:

- Stage 17 resolved the diagnostic-audience/volume handoff in F-034/F-047: transition-only bounded off-thread output, fixed enabled capture, and zero disabled callback work.
- After accepted fixes, Phase 4 and Stages 14/20 must prove the complete desired state, Timer update, common map, and per-entity restores are coherent under all three follow policies; current F-024/F-025 evidence shows this obligation is unmet.
- Generic JSON file/depth limits and unverified upstream NJClient user/channel/work budgets are explicit Stage 18 residuals outside timing-cleanup scope; they are not security-certified by this review.

## Phase 2 focused scenario assertions

1. **Empty join:** valid remote authority may seed Timer without inventing a local loop cursor; the first later audio/MIDI take captures disciplined geometry.
2. **Populated join:** zero and delayed job processing of the same observation produce the same join delta; unequal loop lengths and intentional offsets change by one common mapped elapsed amount.
3. **Continuous and block sync:** replacement precedes dependent discipline, one generation applies once, audio/MIDI cursors remain coherent, and each entity wraps by its own length.
4. **Stay local / invalid / disconnect:** exactly one complete `NoSync` transition clears map, anchors, and every session generation gate without moving local phase; local transport free-runs.
5. **Reconnect:** physical retry and explicit reconnect create a fresh epoch; no old command, map, remote buffer, station, prompt, or request survives into new authority.
6. **Late observation / wrap:** Timer-absolute, device-audio, scene, remote phase, source coordinate, loop cursor, and automation origin use distinct sentinel values; projection is stable across delays, device sample zero, `UINT32_MAX`, and sample-rate conversion at the last source sample.
7. **Real-time/lifetime:** no callback lock, allocation, wait, logging/I/O, forbidden NJClient getter, callback-side container destruction, or escaped connection-owned buffer borrow. Snapshot/refcount/map costs are measured at maximum configured station/take/loop counts.

Per the human decision, each major Phase 4 refactor must select one or two of the focused tests above as passing prerequisites before source movement begins. The first required pair is F-024/F-025's command-order and two-session reconnect regressions.

## Lean prerequisite suite (Phase 3 base, Phase 4 concrete refinements)

| ID | Contract | Planned GoogleTest name | Required before |
| --- | --- | --- | --- |
| P1 | Complete desired-state former `Invalidate -> Replace -> Discipline` intent sequence at the production audio boundary | `NinjamTimingProductionBoundary.CompleteDesiredStateSupersedesFormerCommandSequences` | F-024/F-025 desired-state refactor |
| P2 | Overlapping job/UI intents publish only one coherent complete desired value and one applied version | `NinjamTimingProductionBoundary.OverlappingIntentsPublishOneCoherentDesiredVersion` | F-009 producer move |
| P3 | Real `M`/`2M`/`3M` audio/MIDI takes with intentional offsets, session 1 generation >1, `NoSync`, then epoch-2 generation 1 | `NinjamTimingProductionBoundary.ReconnectPreservesM2M3MEntityOffsetsAcrossEpochOne` | F-005/F-006/F-025 structural work |
| P4 | Delayed restore-before-rebase preserves anchors and entity-relative phase | `NinjamTimingProductionBoundary.RestoreBeforeRebasePreservesEntityAnchors` | F-006 common-map consolidation |
| P5 | Deterministic concurrent observation/local-transport publication proves nonzero overlap and no mixed tuple | `NinjamTimingObservationMailbox.ConcurrentReadProvesOverlapAndCoherence` | F-021/F-023 publication work |
| P6 | Present-zero/absent/nonzero anchors, `UINT32_MAX` Timer crossing, and 96→48 final-source-sample conversion | `NinjamTiming.PresenceWidthAndDownsampleTailRemainDistinct` | F-026/F-029/F-030/F-031 |
| P7 | Connected → retrying/failed → connected produces one `NoSync` and a fresh epoch, including persisted/default start | `NinjamSessionTiming.PhysicalLossRetryCreatesOneNoSyncAndFreshEpoch` | F-027 lifecycle work |
| P8 | No-observation deadlines and valid → invalid → repeated invalid → valid are idempotent and recoverable | `NinjamTimingCoordinator.NoObservationAndInvalidTimingRecoverIdempotently` | F-028 recovery work |
| P9 | Controlled stop after connection-use acquisition but before synchronous stereo consumption | `NinjamSessionAudio.StopCannotInvalidateBorrowDuringSynchronousConsume` | F-033 lifetime fix |
| P10 | Diagnostics disabled produces zero captured work; enabled mode has fixed capacity/overflow and epoch/version correlation | `NinjamTimingDiagnostics.DisabledIsZeroWorkAndEnabledIsBounded` | F-019/F-034/F-047 diagnostics cleanup |
| P11 | Signed offsets apply the same correction to `M`/`2M`/`3M` audio/MIDI entities; missing state defaults to zero | `JamFile.SignedTransportOffsetPreservesM2M3MEntityPhases` | F-044 compatibility fix |
| P12 | NaN/infinite/out-of-range remote tempo is rejected before conversion and network egress | `NinjamTimingInput.RejectsNonFiniteTempoWithoutEgress` | F-032 remote-input hardening |
| P13 | Local seed policy covers default/zero/exact-fit/first-overflow/`UINT32_MAX`/min>max before Windows conversion | `Quantisation.SeedPolicyBoundsConversionBeforeCast` | F-048 local seed hardening |

## Phase 4 batch execution ledger

### B001 — F-033/F-041

- Prerequisite commit: `9b46803ea4bf37f0bb7b3488128da02f1ef4546b`; corrected overlap coverage: `73af93c2229f0073c59a1d2bfa2068daa7e6e74d`; final implementation under test: `5fd1b8062a87e444ccbde35dbccf87bf7f7b2ab8`.
- Every invocation below was preceded by a reread of `.vscode/tasks.json` and `doc/build.md`. Builds used `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe` only through `.github\skills\builder\invoke-msbuild.ps1`, with `/m /t:Build /p:Configuration=Debug /p:Platform=x64` and absolute `/p:SolutionDir=C:\Users\matto\OneDrive\Source\Jamma\`.
- 2026-08-31T23:24:39-06:00, commit `5fd1b806`: incremental `JammaLib\JammaLib.vcxproj` build, exit 0; incremental `test\JammaLib_Tests\JammaLib_Tests.vcxproj` build including references, exit 0.
- 2026-08-31T23:24:39-06:00, commit `5fd1b806`: `JammaLib_Tests.exe --gtest_filter=NinjamSessionAudio.StopCannotInvalidateBorrowDuringSynchronousConsume`, exit 0; passed 1, failed 0, skipped 0. The test contains the acquired-before-Stop case and 64 public-entry overlap epochs.
- 2026-08-31T23:24:39-06:00, commit `5fd1b806`: the same filter with `--gtest_repeat=10 --gtest_break_on_failure --gtest_brief=1`, exit 0; 10/10 repetitions passed, failed 0, skipped 0, covering 640 overlap epochs.
- 2026-08-31T23:24:39-06:00, commit `5fd1b806`: `JammaLib_Tests.exe --gtest_filter=StationRemote.IngestStereoBlockFeedsStationMixPath:StationRemote.ZeroThenIngestStereoBlockFeedsStationMixPath`, exit 0; passed 2, failed 0, skipped 0.
- Scope/static audit: only approved B001 production/test/project and canonical evidence files changed from `1de2ea8`; NJClient, authority/epoch/map, HUD, VST, tooling/window, unrelated MIDI/resources, persistence, and generated assets remain untouched. The callback acquires/releases via a finite sequence of lock-free atomics; the retirement wait and connection destruction remain on the Start/Stop caller.
- Limitations: no test hook can pause inside the private admission instructions, so P9 stress-samples that entry boundary while the sequential-consistency proof supplies timing-independent correctness. No live-server reconnect, ASan, page heap, Application Verifier, or race-tool run is claimed.

### B002 — F-032

- Prerequisite commit: `c9bec2fa0616de5fedda801959fc878a7e1d7370`; implementation under test: `e0d60f2d0141b42a7e3ade5cb161ff264b1200a6`.
- Every invocation below was preceded by a reread of `.vscode/tasks.json` and `doc/build.md`. The incremental build used `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe` only through `.github\skills\builder\invoke-msbuild.ps1` for `test\JammaLib_Tests\JammaLib_Tests.vcxproj`, with `/m /t:Build /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=C:\Users\matto\OneDrive\Source\Jamma\`.
- 2026-08-31T23:37:57-06:00, commit `e0d60f2d`: incremental native-test project build including references, exit 0.
- 2026-08-31T23:37:57-06:00, commit `e0d60f2d`: `JammaLib_Tests.exe --gtest_filter=NinjamTimingInput.RejectsNonFiniteTempoWithoutEgress:NinjamTiming.SharedValidityAcceptsPlausibleTiming`, exit 0; passed 2, failed 0, skipped 0.
- 2026-08-31T23:37:57-06:00, commit `e0d60f2d`: `JammaLib_Tests.exe --gtest_filter=NinjamTiming.SharedValidityRejectsPlaceholderTempo:NinjamTiming.ComputesIntervalLengthFromTempo:NinjamTempoCommand.PreservesUsefulFractionalBpmPrecision`, exit 0; passed 3, failed 0, skipped 0.
- Static no-egress audit: `NinjamConnection::RequestServerTempo` contains the sole four tempo-admin/vote sends and rejects before every formatting, allocation-bearing string construction, mutex, send, and success log. Its sole production chain is NetworkService to Session to the guarded Connection method. `IntervalSampsFromTempo` rejects before floating arithmetic or unsigned conversion. Rejection publishes no authority.
- Limitation: a dynamic send-count test would require a new connection/NJClient seam outside B002's approved boundary, so no-egress is structural plus return-policy evidence. Manual malformed observed/requested timing remains Stage 21.
- Review remediation at 2026-08-31T23:46:03-06:00, commit `93594c5a`: after rereading task/build authority, the wrapped incremental native-test build exited 0; corrected P12 alone passed 1, failed 0, skipped 0; the five-test review filter passed 5, failed 0, skipped 0. P12 now invokes both approved production boundaries. A disconnected owner cannot count sends and would also reject valid requests, so the first-statement source audit remains the no-send proof.

### B003 — F-048

- Prerequisite commit: `d1b6a7698a51078d1363032888d6ce3c67b633ca`; implementation under test: `749ff1ca98f86a061d2cc1732eca7291548afcb7`.
- Every invocation below was preceded by a reread of `.vscode/tasks.json` and `doc/build.md`. Builds used the task-derived MSBuild executable only through `.github\skills\builder\invoke-msbuild.ps1`, with `/m /t:Build /p:Configuration=Debug /p:Platform=x64` and absolute `/p:SolutionDir=C:\Users\matto\OneDrive\Source\Jamma\`.
- 2026-08-31T23:56:24-06:00, commit `749ff1ca`: incremental `JammaLib\JammaLib.vcxproj` build, exit 0; incremental `test\JammaLib_Tests\JammaLib_Tests.vcxproj` build including references, exit 0.
- 2026-08-31T23:56:24-06:00, commit `749ff1ca`: `JammaLib_Tests.exe --gtest_filter=Quantisation.SeedPolicyBoundsConversionBeforeCast:Quantisation.TimingFromSeedAndMasterDerivesBpmAndBpi`, exit 0; passed 2, failed 0, skipped 0.
- 2026-08-31T23:56:24-06:00, commit `749ff1ca`: `JammaLib_Tests.exe --gtest_filter=Quantisation.*`, exit 0; passed 33, failed 0, skipped 0.
- 2026-08-31T23:56:24-06:00, commit `749ff1ca`: related UserConfig filters `DeducesDefaultLoopTimingFromLongLoop`, `DeducesDefaultLoopTimingBelowThreeSecondsWhenPossible`, and `LoopTimingHonoursConfiguredTargetMaxGrain`, exit 0; passed 3, failed 0, skipped 0.
- Static audit: the target maximum remains `double` only until `_RoundedToUInt`, which clamps at `UINT_MAX` before its cast; only that bounded result widens to `unsigned long`. No new helper/class/header/parser/schema/remote request or authority change was introduced.
- Limitation: P13 exercises exact-fit/first-overflow/`UINT_MAX` through the shared safe minimum boundary and coherent target-policy derivation at ordinary/high supported rates. The formerly unsafe target-site extreme is proven structurally through the same checked helper; no parser/manual extreme-config request scenario is claimed.

For every major Phase 4 refactor, choose and pass only the one or two closest prerequisites before moving source. P11–P13 are Phase 4 refinements of already accepted F-044/F-032/F-048 verification obligations. Do not land P1–P13 as an umbrella test batch.

## Phase 3 manual acceptance additions

1. Launch from the shipped/default and a saved `.jam` NINJAM identity; timing lifecycle/epoch begins without manual reconnect and matches interactive connect choices.
2. Decide and verify signed-offset compatibility for `M`, `2M`, and non-divisor audio/MIDI loops; document or guard older-binary offset loss.
3. Run normal and verbose join, record, overdub, `Stay local`, disconnect, physical loss, retry, and reconnect traces; logging on/off produces identical phases and bounded output.
4. Do not change `.jam` password or work-directory writing under this cleanup. Record their current behavior as accepted residual risk and verify the cleanup-only diff contains no F-049/F-050 implementation.
5. Retain Phase 2's remote-join/local-loop scenarios and add malformed observed/requested timing plus extreme local seed-policy rejection without authority/network change.
