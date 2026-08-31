# Stage 13 — Simplification and code size

## Assignment

- Stage: Phase 3, Stage 13 — Simplification and code size.
- Primary ownership: live abstractions, adapters, indirections, duplication, conditionals, responsibility placement, and production-code size in the remote timing/NINJAM sync scope, with explicit attention to slimming `Scene`, `Station`, and `LoopTake`.
- Governing direction: preserve every protected timing distinction in `00-scope-and-inventory.md`; implement the human-approved replacement for rejected F-024 only as the latest complete desired remote transport state (session epoch, follow policy, full remote geometry, and timestamped remote phase), never as queued delta/invalidation commands.
- Explicit exclusions: Stage 6 dead-code candidates (F-018/F-019/F-020) are consumed as dependencies and are not re-reported; retained HUD, VST3 parity, window/tooling, unrelated MIDI/resources, upstream NJClient, test-quality judgment, documentation wording, compatibility, and security are outside this stage. The compile-time-disabled export-latency path is dormant rather than a live abstraction and was not converted into a deletion finding.
- Inputs read in full: `AGENTS.md`; `doc/merge-readiness-review/merge-readiness-plan.md`; `phase-3-maintainability-and-contracts.md`; `decisions.md`; `00-scope-and-inventory.md`; `phase-packets/phase-1.md`; `phase-packets/phase-2.md`; `findings.md`; `verification-matrix.md`; `cleanup-backlog.md`; `stage-reports/06-stale-code.md`; `doc/loop-alignment-and-ninjam-sync.md`; and `doc/realtime-audio.md`. Stage 2, Stage 5, and Stage 8 reports were also consulted for ownership/history/hot-path handoffs.
- Comparison: production diff `master...e72f3b0` plus current surrounding code and relevant `master..e72f3b0` history. Current `HEAD` is review-artifact commit `b6a8853`; no later production change was attributed to the branch.
- Output/single-writer boundary: only this report is written. No production source, test, project/build file, canonical artifact, human decision, other stage report, or cleanup implementation is changed; no commit is created by this investigator.

## Coverage

### Production surface actually reviewed

- End-to-end desired-state/update/command/application path: `ninjam/NinjamTimingCoordinator.{h,cpp}`, `NinjamNetworkService.{h,cpp}`, `NinjamAudioTimingCommand.h`, `NinjamTimingObservationMailbox.h`, `engine/Scene.{h,cpp}`, `audio/AudioHost.{h,cpp}`, and `utils/Timer.{h,cpp}`.
- Common map and hierarchy fan-out: `ninjam/NinjamLoopAlignment.h`, `AudioHost`, `engine/Station.{h,cpp}`, `engine/LoopTake.{h,cpp}`, `engine/Loop.{h,cpp}`, and the audio/MIDI entity anchor/cursor call sites.
- Timing observation adapters and upstream seam: `NinjamController.{h,cpp}`, `NinjamSession.{h,cpp}`, `NinjamConnection.{h,cpp}`, and the read-only contract in `lib/njclient/njclient.h` as already accepted by Phase 2.
- Timing diagnostics that enlarge the requested classes: Scene timing-policy/join/applied-command logging; Station before/after hierarchy logging; AudioHost applied receipt; coordinator diagnostics. The supported field set/audience/volume remains Stage 17 ownership.
- Live repeated cursor movement in `LoopTake`: queued correction, unified boundary correction, and local transport-offset application. No reachability/deadness finding was made for any queue API.
- Remote-tempo prompt identity comparison across Scene and the coordinator.
- Header/include and member footprint for the focal classes, including the NINJAM dependency imported by `LoopTake.h` and the five copied map fields/API surface.

The focal production files currently total 2,477 lines in `Scene.cpp`, 2,645 in `Station.cpp`, and 3,265 in `LoopTake.cpp`. Against `master...e72f3b0`, they account respectively for +607/−35, +521/−57, and +452/−40 lines. Those totals include retained out-of-scope feature work, so proposals below cite only timing-owned ranges rather than treating total class size as a defect.

### Commands and queries used

- `git status --short`; `git log --oneline --decorate`; `git diff --numstat|--stat master...e72f3b0 -- <timing paths>`; and `git diff --unified=5 master...e72f3b0 -- <focal source/header paths>`.
- Repository `rg -n` call/reference searches for timing updates/commands, follow policy, map begin/rebase/restore, scene anchors, correction reasons, cursor shifting, timing getters, controller exposure, diagnostics, prompt comparison, and the fixed snapshot/mailbox protocols.
- Numbered `Get-Content` reads of every cited current range and surrounding owner/caller code.
- `git log -S`, path history, `git show --stat`, and `git blame -L` for the relevant chains, especially `5f76564`, `f36bfe7`, `d0d208e`, `d6fdb88`, `4c1c0e1`, `6abf7c7`, `a3f72b4`, `e0f669c`, `0d90bac`, `bef7943`, and `e72f3b0`.
- Current line counts for the focal implementation/header files.

No build, native test, runtime session, profiler, or source edit was run. This is the investigation stage, and the requested output is design evidence for the human gate rather than cleanup implementation.

### Deliberate exclusions and negative review decisions

- No HUD, VST3, window/tooling, upstream NJClient, or unrelated MIDI/resource cleanup is proposed.
- F-018 and F-019 are excluded from deletion estimates below. In particular, S13-01's estimate does not count the dead `AlignmentReceipt` surface already owned by Stage 6.
- The three field-wise fixed mailboxes/snapshots were compared, but no generic seqlock/template abstraction is proposed. Their writer/reader semantics, retry counts, consume-vs-read behavior, and field layouts differ; an abstraction would hide memory-ordering and callback-cost evidence and is unlikely to reduce verified code after serializers are supplied.
- The explicit `Station -> LoopTake -> Loop` fan-out is retained. Small neutral fan-outs remain clearer in a real-time path than a callback/lambda abstraction; the useful reduction is to remove redundant operations and payloads, not to disguise traversal.
- The `NinjamNetworkService` integration owner is retained. Commit `5f76564` deliberately moved NINJAM responsibility out of Scene; deleting that owner would reverse the desired architecture. Proposals instead finish that move.

## System understanding

The branch's intended timing model needs four live owners, not one merged abstraction. NINJAM connection/integration owns physical availability, epoch, remote observation validity, request/prompt state, and follow-policy selection. `AudioHost` owns audio-boundary comparison/application, local Timer mutation, and the one common remote-to-local source map. `Timer` owns local master geometry plus the separate monotonic scene coordinate. Each `LoopTake`/`Loop` owns only its entity-specific audio/MIDI anchors and wrapped cursors.

The present code crosses those boundaries with three redundant representations:

1. The coordinator emits a sum-of-optionals `NinjamTimingUpdate` (`NinjamTimingCoordinator.h:95`-`:103`); Scene turns it into event/delta commands (`Scene.cpp:403`-`:486`) and also fabricates connect/disconnect invalidations (`Scene.cpp:254`-`:305`); AudioHost then reconstructs durable state from command history (`AudioHost.cpp:165`-`:330`). This is the exact seam that the approved latest-complete-desired-state decision replaces.
2. AudioHost owns `SyncPhaseMap` (`AudioHost.h:141`-`:144`), but copies its origin and both lengths into every take (`AudioHost.cpp:352`-`:364`; `LoopTake.h:406`-`:410`), where every take recomputes the same mapped source coordinate on every followed block (`LoopTake.cpp:640`-`:648`). Only the per-entity anchor and modulo are actually entity-owned.
3. The same neutral operation—shift every playable audio cursor by one signed sample delta and move the MIDI event/automation cursor coherently—is repeated in the direct timing-command and local-offset paths (`LoopTake.cpp:537`-`:563` and `:746`-`:773`). The queued path at `:466`-`:513` has related mechanics but deliberately interleaves correction with ordinary block advancement and has different empty/requeue gates, so it is not assumed equivalent.

Diagnostics are a separate responsibility problem, not timing authority. Scene's command/policy log routines occupy `Scene.cpp:403`-`:570`, while Station owns a 152-line string/vector/console hierarchy formatter at `Station.cpp:2327`-`:2478` plus callback-reachable call sites at `:1107`, `:1197`, and `:2301`. These routines only mirror operational state. Removing them from Scene/Station does not authorize removal of useful observability; Stage 17 must select a bounded off-thread signal contract first.

The equivalence baseline for every structural proposal is:

\[
C(S)=C_0+\operatorname{round}((S-S_0)M_l/M_r),\qquad q_i(S)=(C(S)-a_i)\bmod L_i.
\]

`C(S)` is one mapped source coordinate, not a loop cursor. `a_i` and `L_i` remain entity-specific. An epoch change invalidates `C`'s map and every `a_i`/generation gate before new authority is accepted. `NoSync` clears authority without moving `q_i` and local transport then free-runs. None of the proposals merges master phase, scene coordinate, source coordinate, mapped elapsed time, per-loop phase, local/remote timing, or follow policy.

## Candidate findings

### Ranked simplification map

Estimates are net production-line ranges after the described replacement, not promises from an unimplemented patch. They deliberately exclude Stage 6 dead-code deletions and are not additive: S13-01/S13-02 share some Scene logging/translation lines, and S13-02/S13-04 share command-boundary lines.

| Rank | Candidate | Estimated net deletion | Coupling reduction | Behavioural risk | Dependencies |
| ---: | --- | ---: | --- | --- | --- |
| 1 | S13-01 move bounded timing diagnostics out of Scene/Station | 80-150 lines; 160-230 removed from focal classes before a smaller existing-owner sink | high: removes observability state/formatting from engine hierarchy | low for playback, medium for supported diagnostic contract | Stage 17, F-034; coordinate but do not count F-019 |
| 2 | S13-02 replace update/event/delta translation with latest complete desired state | 70-120 lines | very high: one NINJAM producer contract; Scene forwards only | high | approved F-024 direction, F-009, F-021/F-023/F-025-F-031; prerequisite regressions |
| 3 | S13-03 keep one common map and fan out one source coordinate | 45-75 lines | very high: removes N copied map geometries and three lifecycle payloads | medium-high | F-006, F-018, F-025; unequal-length/offset/reconnect tests |
| 4 | S13-04 neutralize the Station/LoopTake correction boundary | 25-40 lines | high: removes engine-to-NINJAM policy dependency and redundant command classification | medium | F-005, F-025, S13-02 epoch semantics |
| 5 | S13-05 consolidate direct LoopTake cursor-shift mechanics | 20-30 lines | medium: one local invariant for direct audio/MIDI movement | low | preserve each caller's gates/accounting; focused cursor tests |
| 6 | S13-06 delete the live timing-getter adapter chain after owned publication | 15-35 lines net; 45-60 gross adapter/callback lines | high: one callback/upstream seam instead of Controller -> Session -> Connection getter pass-through | high | F-021, F-023, F-026, F-030; upstream remains read-only |
| 7 | S13-07 centralize remote-tempo proposal identity comparison | 6-12 lines | medium: Scene stops duplicating NINJAM proposal equality | low | F-013 naming; prompt tests |

### S13-01 — Remove timing-diagnostic formatting and state from Scene/Station

- Stage / reviewer: Stage 13 — Simplification and code size; code-size/responsibility refinement of accepted F-034, with the field/audience decision handed to Stage 17.
- Scope reviewed / exclusions: live timing-policy, before/after alignment, applied-command, record/overdub/delete logging in Scene and Station. It does not re-report F-019's dead receipt and does not decide which bounded signals Stage 17 retains.
- Severity: must fix before merge under the explicit human zero-disabled-cost and slim-Scene/Station decision.
- Evidence: Scene interprets/prints policy while materializing commands at `JammaLib/src/engine/Scene.cpp:403`-`:486`, prints request-state diagnostics at `:489`-`:515`, and formats applied command plus per-station snapshots at `:517`-`:570`; related state occupies `Scene.h:377`-`:381`. Station's public/private logger and string-key snapshot storage are at `Station.h:183`, `:298`-`:299`, `:365`-`:370`, and `:398`; callback-reachable entry sites are `Station.cpp:1107`, `:1197`, and `:2301`; the formatter/hierarchy walker is `:2327`-`:2478`. `dcfaf1d` introduced the debugging logger, `e0f669c` expanded before/after evidence, and `b9619b9` added coordinator telemetry.
- Why it matters: diagnostics account for the single largest timing-only reduction available in the requested classes. Station currently owns string keys, a mutable vector, hierarchy traversal, arithmetic, formatting, and console I/O that are not part of station playback. Scene also knows command variants solely to name log events.
- Recommended disposition: after Stage 17 names the minimum signal set, use the existing AudioHost applied receipt/coordinator diagnostics and an existing fixed handoff to capture only bounded numeric identifiers when enabled; format/write on the job/integration side. Remove Station's `_LogLocalLoopAlignment`, `_ninjamBeforePositions`, its public logger, and callback entries; reduce Scene to forwarding/presentation rather than command-aware log formatting. Add no logging service/class. Disabled callback code must contain no diagnostic call, branch, traversal, allocation, or I/O.
- Estimated deletion / coupling / risk: remove roughly 160-230 lines from Scene/Station and add a smaller 40-100-line off-thread presentation path only if Stage 17 retains those signals, for approximately 80-150 net deletion. Coupling reduction is high; playback risk is low because state is observational, while diagnostic-regression risk is medium.
- Dependencies: Stage 17 signal-to-symptom map; accepted F-034; coordinate the already-approved F-019 deletion without including its lines in this estimate.
- Equivalence argument: operational Timer/map/anchor/cursor mutation is untouched. For unequal loop lengths and intentional offsets, any retained event records entity-specific length/phase values but never feeds them back. Reconnect records the new epoch/applied state, and `NoSync` may record one policy transition, but both continue to clear authority through the operational path. Logging enabled/disabled must produce identical cursor results.
- Verification: callback call-chain audit; disabled-build/disassembly or instrumentation showing zero diagnostic work; verbose remote join/replacement/discipline/record/overdub/disconnect trace with generation/epoch and station/take identity selected by Stage 17; compare unequal-length/offset cursor results with logging off/on.
- Human decision: pending Phase 3 gate.

### S13-02 — Make NINJAM publish only the latest complete desired remote transport state

- Stage / reviewer: Stage 13 — Simplification and code size; implements the human-approved replacement for rejected F-024 and consolidates accepted F-009.
- Scope reviewed / exclusions: coordinator/integration output, Scene translation/forwarding, timing mailbox payload, and AudioHost's event-history reconstruction. This does not propose an ordered queue, standalone invalidation, or phase-delta command.
- Severity: must fix before merge; it is the approved resolution of a Phase 2 merge blocker.
- Evidence: the producer exposes three optional payloads plus three flags/reasons in `JammaLib/src/ninjam/NinjamTimingCoordinator.h:45`-`:103`; Scene prioritizes those combinations and copies fields into another type at `Scene.cpp:403`-`:467`, publishes at `:469`-`:482`, and constructs separate invalidations at `:254`-`:305`. The mailbox stores command type, delta, anchor invalidation, and partial geometry at `NinjamAudioTimingCommand.h:16`-`:51` and `:58`-`:145`. AudioHost must reconstruct prior state through command-type conditionals at `AudioHost.cpp:165`-`:330`. `f36bfe7` created the coordinator/update split; `d6fdb88` introduced the unified command but left translation in Scene; `a3f72b4`, `e0f669c`, and `e72f3b0` extended the payload/translation.
- Why it matters: the same authority is represented as coordinator optionals, a Scene decision tree, mailbox event variants, and AudioHost remembered state. This makes Scene responsible for protocol semantics and makes correctness depend on receiving non-substitutable history through a latest-wins mailbox.
- Recommended disposition: have the existing NINJAM coordinator/integration owner publish one value containing session epoch, follow policy, complete validated device-rate remote geometry, and remote master phase at an explicit observation sample/presence. Scene forwards that value and separately presents prompts/remote-grid UI; it does not construct transport events. At each block AudioHost compares the value with its last applied value: epoch change clears map/anchors/gates; geometry change applies replacement; unchanged geometry projects the timestamped remote phase and derives discipline; `NoSync` clears authority. A first accepted state in an epoch supplies join semantics without a delta command.
- Estimated deletion / coupling / risk: approximately 70-120 net production lines, primarily from `Scene.cpp`, duplicated intermediate types, and event switches. Coupling reduction is very high; risk is high because it changes the authority handoff.
- Dependencies: F-009 owner/publication contract; approved F-024 complete-state direction; F-021 coherent upstream observation; F-023/F-029/F-030 coherent wide local observation; F-025/F-027 epoch; F-026 observation instant; F-028 validity/loss; F-031 conversion bound. Before refactor, the Phase 2 command-order and two-session reconnect regressions must pass.
- Equivalence argument: full state keeps policy, geometry, phase, observation coordinate, and epoch as separate fields. AudioHost still computes one common source correction; every entity wraps it by its own `L_i`, so unequal lengths and intentional anchor offsets remain unchanged. Reconnect is strictly stronger: a changed epoch invalidates old map/anchors/gates before accepting session 2. `NoSync` is a complete desired policy state that clears authority once and moves no cursor; repeated identical publications are idempotent. Continuous and Block policies remain distinct choices with the same map model and different expected correction magnitude.
- Verification: production-faithful rapid publication test; same-state idempotence; geometry-change replacement; unchanged-geometry discipline; first accepted state/join; unequal audio/MIDI lengths and offsets; epoch 1 generation >1 -> `NoSync` -> epoch 2 generation 1; invalid/physical loss; valid zero observation; delayed observation; no standalone invalidation/delta symbol or producer remains.
- Human decision: pending Phase 3 gate, with implementation shape already constrained by the Phase 2 human decision.

### S13-03 — Keep the common sync map only in AudioHost and fan out one source coordinate

- Stage / reviewer: Stage 13 — Simplification and code size; consolidates accepted F-006 and its S08-03 enrichment.
- Scope reviewed / exclusions: live common-map geometry and begin/rebase/restore APIs only. Per-loop audio/MIDI anchors, lengths, cursor modulo, and NoSync invalidation remain entity-owned and are not deletion candidates.
- Severity: follow-up structural simplification, to be batched only after blocker prerequisites.
- Evidence: AudioHost owns `_syncPhaseMap` at `JammaLib/src/audio/AudioHost.h:141`-`:144`, computes/rebases it at `AudioHost.cpp:167`-`:318`, then sends its four geometry values through begin/rebase fan-outs at `:352`-`:364`. Station repeats three map wrappers at `Station.h:122`-`:128` and `Station.cpp:764`-`:795`. Every take stores another map at `LoopTake.h:406`-`:410`, receives it at `LoopTake.cpp:602`-`:638`, and independently executes the same mapped-elapsed calculation at `:640`-`:648` before entity-specific restore at `:649`-`:678`. The N+1 map grew through `a3f72b4`, `0d90bac`, and `bef7943`.
- Why it matters: five copied fields and three lifecycle verbs in every layer imply multiple map owners, repeat two 64-bit divisions per take per followed block, and thicken Station/LoopTake with remote-ruler responsibility.
- Recommended disposition: let AudioHost call its one `SyncPhaseMap::SourceCoordinateAt(scene)` once per boundary/block. Replace begin/rebase/restore geometry propagation with two neutral entity operations: capture missing anchors against a supplied source coordinate, and restore cursors from a supplied source coordinate. Rebase changes only the AudioHost map; it must not recapture entity anchors. Keep explicit Station fan-out rather than adding a traversal abstraction.
- Estimated deletion / coupling / risk: approximately 45-75 net production lines, five LoopTake fields, and one repeated common calculation per take. Coupling reduction is very high; risk is medium-high because map ordering is subtle.
- Dependencies: accepted F-006/S08-03; F-018 may remove its separate dead field in the same dependency order but is not counted here; F-025 epoch invalidation; prerequisite unequal-length/offset and reconnect tests.
- Equivalence argument: current every-take calculation yields the same `C(S)` because every copy receives identical `S_0`, `C_0`, `M_l`, and `M_r`. Computing `C(S)` once and passing it down is algebraically identical. Each audio/MIDI entity still evaluates `(C-a_i) mod L_i`, so different lengths and intentional offsets remain distinct. A rebase changes `C`'s ruler only and retains `a_i`. Reconnect/`NoSync` invalidates each `a_i` and the AudioHost map before a new epoch, so no old coordinate survives.
- Verification: test instrumentation proving one map calculation independent of take count; unequal audio/MIDI lengths and offsets; map rebase without anchor recapture; remote wraps; offset application before first capture; reconnect; `NoSync` free-run; callback benchmark at maximum hierarchy size.
- Human decision: pending Phase 3 gate.

### S13-04 — Replace the NINJAM-aware engine command API with neutral correction and epoch-reset operations

- Stage / reviewer: Stage 13 — Simplification and code size; implementation sharpening for accepted F-005 and F-025.
- Scope reviewed / exclusions: the unified AudioHost -> Station -> LoopTake boundary. Follow-policy selection remains in NINJAM; per-entity correction remains in the engine.
- Severity: follow-up boundary simplification, with the reconnect reset itself required by F-025.
- Evidence: `LoopTake.h:21` imports `NinjamAudioTimingCommand.h`; `LoopTake::ApplyTimingCommand` takes policy, reason, generation, delta, and an unused scene-coordinate parameter at `LoopTake.h:235`-`:239` and `LoopTake.cpp:518`-`:535`. Station mirrors the payload at `Station.h:113`-`:119` and `Station.cpp:729`-`:742`. AudioHost first classifies NINJAM command type into `LoopTake::TimingCorrectionReason` at `AudioHost.cpp:281`-`:297`, even though the engine uses that reason only for invalidation and policy only for `NoSync`; it already decides `disablesSync` at `:174`-`:186` and suppresses the correction fan-out at `:298`-`:305`. This split entered through `d6fdb88`, `6abf7c7`, and `a3f72b4`.
- Why it matters: the low-level loop hierarchy depends upward on a session-policy header and receives fields it does not use. Two layers must agree that `NoSync` does not move cursors, while the invalidation generation reset is currently skipped on the production path (F-025).
- Recommended disposition: AudioHost interprets desired follow policy once. Expose a neutral accepted correction `(delta, epoch/generation)` and a separate explicit epoch reset/invalidate operation to Station/LoopTake; remove NINJAM policy, event reason, and scene-coordinate parameters from this engine path. Do not merge Continuous/Block/NoSync: the policies remain at the boundary and only accepted correction/reset effects enter the engine.
- Estimated deletion / coupling / risk: approximately 25-40 lines and the `engine -> ninjam` include edge. Coupling reduction is high; risk is medium.
- Dependencies: S13-02's explicit epoch; accepted F-005/F-025; two-session reconnect regression. Keep any separate local-only queued-offset API out of this change unless Phase 4 explicitly batches it.
- Equivalence argument: Continuous and Block both currently deliver the same neutral signed source correction after AudioHost policy/map interpretation, so removing their enum from the entity call changes no cursor formula. Each loop still wraps the delta by its own length, preserving offsets. On reconnect, explicit epoch reset makes session-2 generation valid. `NoSync` invokes reset/invalidate and no correction, leaving local cursors free-running.
- Verification: include audit; accepted correction for Continuous/Block; no movement for `NoSync`; epoch reset reaches every take; stale old epoch rejected; unequal length/offset audio and MIDI tests.
- Human decision: pending Phase 3 gate.

### S13-05 — Use one private LoopTake primitive for direct audio/MIDI cursor shifting

- Stage / reviewer: Stage 13 — Simplification and code size.
- Scope reviewed / exclusions: duplicated direct cursor mutation only; caller-specific generation/accounting and applied-offset gates remain separate. The queued `EndMultiPlay` correction keeps its current advancement/requeue ordering and no deadness claim is made for any API.
- Severity: follow-up.
- Evidence: unified boundary correction traverses audio loops and performs signed MIDI modulo/move at `JammaLib/src/engine/LoopTake.cpp:537`-`:563`; local transport offset repeats that same immediate operation at `:746`-`:773`. `d6fdb88` added the first body and `4c1c0e1` added the second; both call `Loop::ShiftPlayIndex` for audio and `_MoveMidiVisualCursor` for MIDI/automation correction. The queued path at `:466`-`:513` is similar but advances MIDI within the block and is deliberately excluded from the initial extraction.
- Why it matters: the direct-correction invariant that audio body cursors and the MIDI event/automation cursor move by the same signed amount is maintained in two bodies. Fixes to wrapping, playable-state handling, or MIDI automation correction can drift between them.
- Recommended disposition: add one small private, allocation-free `LoopTake` method that snapshots current playable state, shifts its audio loops and MIDI cursor by a signed delta, and returns whether anything moved. Keep generation/accounting in `ApplyTimingCommand`, applied-target state in `_TryApplyLocalTransportOffset`, and the queued `EndMultiPlay` path unchanged unless a separate test-backed equivalence proof covers its ordering.
- Estimated deletion / coupling / risk: approximately 20-30 lines. Coupling reduction is medium and localized; risk is low because the two selected bodies have the same immediate mutation order.
- Dependencies: focused tests must characterize each caller before extraction; this can follow S13-04 so the neutral boundary has its final shape.
- Equivalence argument: the helper still applies one delta to every entity and each entity wraps by its own `L_i`; unequal lengths and intentional offsets therefore remain unchanged. It does not own epoch, map, or follow policy. Reconnect and `NoSync` gating stays with callers and can prevent invocation exactly as before.
- Verification: positive/negative/zero delta; unequal audio lengths; MIDI-only and audio-only takes; automation correction sign; empty/nonplayable take; local offset accounting; reconnect/`NoSync` no-call behavior; confirm queued requeue/advancement behavior is byte-for-byte untouched.
- Human decision: pending Phase 3 gate.

### S13-06 — Remove the Controller/Session/Connection live-timing getter chain after fixed publication exists

- Stage / reviewer: Stage 13 — Simplification and code size; adapter/code-size consequence of accepted F-021.
- Scope reviewed / exclusions: Jamma-owned callback adapters only; upstream NJClient is read-only and its implementation is not changed.
- Severity: merge blocker dependency through F-021.
- Evidence: AudioHost obtains timing at `JammaLib/src/audio/AudioHost.cpp:437`-`:453`; `NinjamController::GetLiveTiming` is a pass-through at `NinjamController.cpp:102`-`:105`, `NinjamSession::GetLiveTiming` acquires another connection use and forwards at `NinjamSession.cpp:502`-`:506`, and `NinjamConnection::GetLiveTiming` assembles/validates the value at `NinjamConnection.cpp:709`-`:733`; matching declarations are at `NinjamController.h:44`, `NinjamSession.h:106`, and `NinjamConnection.h:102`. The job snapshot independently assembles the same remote fields at `NinjamConnection.cpp:922`-`:946`. `d0d208e` introduced the getter surface and `f36bfe7` placed it in the callback.
- Why it matters: three live adapters add code and acquisition/coupling without defining an authority boundary; they also duplicate remote-field assembly/validation and violate the accepted upstream callback contract.
- Recommended disposition: extend the existing Jamma-owned `ProcessExportBlock`/`AudioProc` seam to capture/publish the exact pre-advance block timing value through the fixed mailbox, without an additional NJClient callback call. Let the integration/job owner consume that coherent value and combine it with the distinct local Timer/device observation. Delete all three `GetLiveTiming` pass-through surfaces. Only NJClient `AudioProc` remains reachable from the callback; do not edit upstream.
- Estimated deletion / coupling / risk: 45-60 gross adapter/callback lines; after the bounded publication code, approximately 15-35 net. Coupling reduction is high; behavioural risk is high because observation time is correctness-critical.
- Dependencies: accepted F-021 plus coherent local observation F-023, initial observation F-026, explicit presence F-030, and the scoped connection lifetime work F-033. The exact block-start capture contract must be test-backed before deletion.
- Equivalence argument: this changes observation transport, not timing domains. Remote phase remains timestamped in its own observation; Timer absolute, device sample, and scene coordinate remain separate. The downstream map still preserves unequal lengths/offsets. Reconnect uses the published epoch/availability and `NoSync` invalidates the map rather than treating absence as a coordinate.
- Verification: callback marker proving only upstream `AudioProc` is called; alternating coherent publication generations; exact pre-advance observation sample; delayed projection; connect/disconnect/reconnect; invalid/absent/zero timing; no retired getter references.
- Human decision: pending Phase 3 gate.

### S13-07 — Give remote-tempo proposals one identity comparison

- Stage / reviewer: Stage 13 — Simplification and code size.
- Scope reviewed / exclusions: prompt identity only; no prompt UX wording or tempo-validity judgment.
- Severity: follow-up.
- Evidence: Scene snapshots the pending proposal and manually compares interval length, source rate, grid step, BPI, and BPM tolerance at `JammaLib/src/engine/Scene.cpp:336`-`:360`. The coordinator implements the same identity comparison at `NinjamTimingCoordinator.cpp:274`-`:281`. `5f76564` moved prompt/network responsibility out of Scene, while `f36bfe7` added the second coordinator comparison.
- Why it matters: adding or renaming a proposal identity field requires synchronized edits in Scene and the coordinator; a disagreement can close a prompt on a phase-only refresh or leave stale geometry displayed.
- Recommended disposition: define one value-level proposal identity comparison in the existing NINJAM value/owner and let Scene ask whether the pending proposal changed. Keep observation phase/sample out of identity so a fresh observation of the same proposed geometry does not churn the dialog.
- Estimated deletion / coupling / risk: approximately 6-12 lines. Coupling reduction is medium; risk is low.
- Dependencies: coordinate F-013's `RemoteGridStepSamps` rename; no new class.
- Equivalence argument: prompt identity cannot move Timer or entity cursors. Unequal lengths/offsets are not involved. Reconnect/`NoSync` still clear the pending proposal/dialog through lifecycle state; the comparison only distinguishes same versus changed remote geometry.
- Verification: same geometry with updated phase/sample keeps prompt; changed interval/rate/grid/BPI/BPM closes/replaces it; disconnect/reconnect clears it; one comparison implementation remains.
- Human decision: pending Phase 3 gate.

## Handoffs

- Phase 3 integrator: S13-02 is not a new alternative to F-024. It is the approved complete-desired-state disposition and should update/restate F-024 accordingly while enriching F-009. Reject any synthesis that reintroduces ordered commands, standalone invalidation, or phase-delta publication.
- Phase 3 integrator: merge S13-03 into accepted F-006/S08-03; merge S13-04 into F-005 and the epoch-reset portion of F-025; merge S13-06 into F-021. S13-01 sharpens F-034 but must await Stage 17. S13-05 and S13-07 are new bounded simplification candidates.
- Stage 14: provide one or two passing prerequisite contracts before each major refactor. Highest priority remains complete-state coalescing and two-session reconnect. S13-03 additionally needs unequal lengths/offsets/rebase; S13-05 needs caller-gate characterization.
- Stage 15: any accepted boundary change must update comments that describe event commands, copied per-take maps, or command-thread ownership; retain the protected glossary.
- Stage 17: select the minimum useful timing signal set, identifiers, levels, and volume for S13-01. The implementation constraint is fixed: zero disabled callback work, bounded enabled capture, off-thread formatting, no new logger class.
- Phase 4 batching: recommended dependency order is prerequisite tests -> F-021/F-023 observation boundary and epoch value -> S13-02 complete desired state -> S13-04 neutral engine API/F-025 reset -> S13-03 common-map consolidation -> S13-05 local deduplication. S13-01 waits for Stage 17 but can be an independent diagnostic batch. S13-07 is a small later cleanup.
- F-007/F-008/F-012 remain approved coupling cleanups but are mainly moves rather than code-size wins. Coordinate their value-header/mailbox placement with S13-02 without adding a class solely for the value extraction or hiding fixed real-time publication behind a broad generic abstraction.
- Stage 6/F-018/F-019: retain their exact deletion ownership. None of their dead members/branches is assigned a new S13 ID or counted in estimates.

## Uncertainties

- Line-deletion estimates are based on current exact ranges and intended replacement shapes, not a patch. Stage 17's retained diagnostics and the final complete-state value layout can move the net totals within the stated bands.
- The complete desired-state refactor is intentionally high risk despite reducing code. The current integration tests mirror Scene's translation in a test harness; Stage 14 must decide whether they independently prove the production seam before Phase 4 relies on them.
- The exact supported way to capture pre-advance remote timing at the Jamma-owned AudioProc seam must be proven without editing NJClient or calling another forbidden getter from the callback. This stage establishes adapter deletion potential, not that mechanism's correctness.
- Consolidating the map owner removes repeated common arithmetic but does not remove per-entity snapshot loads, weak locks, modulo, or cursor writes. Those costs and `atomic<shared_ptr>` lock-freedom remain measured residual obligations.
- S13-05 intentionally leaves subtle caller gates separate. In particular, queued corrections, direct accepted corrections, and local offsets do not currently share identical empty/MIDI-only accounting; folding those conditions together would be a behavioral change outside this simplification proof.
- `Scene`, `Station`, and `LoopTake` contain substantial retained HUD/VST/MIDI work. This report does not claim their total class size can be reduced to the timing-only estimates without touching excluded scope.
- No build/runtime evidence was generated in this read-only stage. All implementation proposals remain at the human gate.

## Conclusion

Stage 13 found seven bounded simplification opportunities in the approved timing-only scope. The largest focal-class reduction is to remove diagnostic formatting/state from Scene and Station after Stage 17 defines a bounded off-thread contract. The most important structural reduction is the already-directed latest complete desired remote transport state, which removes Scene's command materialization and makes latest-wins publication semantically complete. The next major reduction gives `SyncPhaseMap` one AudioHost owner and sends one common source coordinate to entity-owned anchors/moduli.

Smaller safe reductions neutralize the Station/LoopTake correction API, consolidate repeated LoopTake cursor movement, delete the live timing-getter adapter chain once fixed publication exists, and centralize prompt identity. Together they slim all three requested classes without turning mapped elapsed time into a shared cursor or merging master phase, per-loop phase, scene/source coordinates, local/remote timing, follow policy, or epoch.

The review explicitly rejected a generic mailbox/seqlock abstraction, generic hierarchy traversal, removal of the NINJAM integration owner, and any simplification of dormant/dead paths already owned by Stage 6. No cleanup, source/test edit, build, or commit was performed; all seven candidates stop at the Phase 3 human gate.
