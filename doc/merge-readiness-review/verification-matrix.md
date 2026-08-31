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
| F-020 | App build and copied-resource audit | Relevant HUD/graphics tests | Trigger default/hover/down/out rendering | Dependent on F-001; Stages 12/16/20/21 |
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
| F-032 | Finite/plausible validation audit at helper and outgoing request | NaN, ±infinity, zero, negative, endpoints, just-outside and extreme finite inputs; request sends nothing on reject | Malformed observed/requested timing causes no authority or network change | Follow-up hardening; Stages 17/18/20 |
| F-033 | Borrow-scope/callback-destruction audit | Controlled Stop attempt between acquire and consume using preallocated buffers | Repeated live start/stop/reconnect under ASan/page heap/Application Verifier | Prerequisite lifetime stress; Phase 4 + Stages 14/20 |
| F-034 | No callback formatter/I/O/hierarchy traversal; zero disabled work; bounded fixed record | Capacity/overflow, epoch + desired/applied correlation, dead-receipt removal, logging on/off phase equivalence | Normal/verbose join/record/overdub/Stay-local/disconnect/reconnect trace; profiler | Coordinate F-019; Stages 13/17/20 before batch |
| F-035 | Private-helper call-site audit; queued path diff unchanged | Signed/zero shift across unequal audio, MIDI-only/audio-only/empty; automation sign and local-offset accounting | Reconnect/`NoSync` confirms direct helper is not invoked | Stage 13/20; after F-005 boundary shape |
| F-036 | One proposal identity implementation; retired comparison audit | Same geometry/new observation stable; each geometry/policy field change detected | Prompt persists/replaces correctly and clears on reconnect | Stage 13/20; coordinate F-013 |
| F-037 | File/project/retired-symbol audit; incremental tests build | Current timing/coordinator/Timer/LoopTake suites remain registered and passing | None | Stage 14/20; direct 156-line deletion |
| F-038 | Production-seam call audit; model omissions no longer claim end-to-end fidelity | P1–P4: complete state, real unequal audio/MIDI takes, two-session generation-1 reconnect, restore-before-rebase | Production-faithful join/reconnect trace | Stage 14/20; prerequisite before F-005/F-006/F-009/F-025 |
| F-039 | Retired command/sentinel expectation audit | Complete-state supersession; valid zero vs explicit absent vs nonzero in both clock domains; delayed first block | First-block join/reconnect trace | Stage 14/20; coordinate F-024/F-030 |
| F-040 | Deterministic overlap/start-barrier audit | P5 requires nonzero overlapping complete reads with incompatible field sentinels and generation diagnostics | Repeated run plus race tooling where practical | Stage 14/20; prerequisite F-021/F-023 publication changes |
| F-041 | Existing-owner fault/clock/consume seam audit; no raw-buffer test API | P7–P9 loss/retry/invalid/deadline and controlled stop-during-consume | Sanitizer/page heap/Application Verifier plus manual reconnect | Stage 14/20; prerequisite F-027/F-028/F-033 |
| F-042 | Statement/source and ownership audit after contract fixes | Relevant F-021/F-024/F-025/F-027/F-028 tests linked, not duplicated | `Stay local`, join and reconnect guide trace | Stage 15/20; docs update with implementation batch |

Cross-phase obligations not represented as Phase 1 deletions:

- Stage 8 must assess callback-side cost of retained before/after alignment diagnostics.
- Stage 17 must decide the supported audience and volume for live/dormant alignment snapshots and coordinator counters.
- After the accepted fixes, Phase 4 and Stages 14/20 must prove that the unified command, Timer update, common map, and per-entity restores are coherent under all three follow policies; current F-024/F-025 evidence shows this obligation is unmet.

## Phase 2 focused scenario assertions

1. **Empty join:** valid remote authority may seed Timer without inventing a local loop cursor; the first later audio/MIDI take captures disciplined geometry.
2. **Populated join:** zero and delayed job processing of the same observation produce the same join delta; unequal loop lengths and intentional offsets change by one common mapped elapsed amount.
3. **Continuous and block sync:** replacement precedes dependent discipline, one generation applies once, audio/MIDI cursors remain coherent, and each entity wraps by its own length.
4. **Stay local / invalid / disconnect:** exactly one complete `NoSync` transition clears map, anchors, and every session generation gate without moving local phase; local transport free-runs.
5. **Reconnect:** physical retry and explicit reconnect create a fresh epoch; no old command, map, remote buffer, station, prompt, or request survives into new authority.
6. **Late observation / wrap:** Timer-absolute, device-audio, scene, remote phase, source coordinate, loop cursor, and automation origin use distinct sentinel values; projection is stable across delays, device sample zero, `UINT32_MAX`, and sample-rate conversion at the last source sample.
7. **Real-time/lifetime:** no callback lock, allocation, wait, logging/I/O, forbidden NJClient getter, callback-side container destruction, or escaped connection-owned buffer borrow. Snapshot/refcount/map costs are measured at maximum configured station/take/loop counts.

Per the human decision, each major Phase 4 refactor must select one or two of the focused tests above as passing prerequisites before source movement begins. The first required pair is F-024/F-025's command-order and two-session reconnect regressions.
