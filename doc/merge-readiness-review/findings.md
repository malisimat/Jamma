# Canonical findings

Canonical IDs were assigned by the Phase 1 integrator after reconciling Stage 1–6 reports. Approval records the proposed disposition for later Phase 4 batching or accepted scope; it does not authorize source edits.

## F-001 — Decide whether the HUD/visual/resource feature belongs in this merge

- Stage / reviewer: S01-01; corroborated by S05 history and S06 asset sweep.
- Scope reviewed / exclusions: branch scope and feature lineage; no assertion that HUD behavior is defective.
- Severity: must fix before merge.
- Evidence: merge `24a43a05a53ecfb433452d5e4e517f47e43278aa` brought the independent `feature/merge-hud` lineage into the timing branch (56 files, +2,213/−187 versus first parent). Current integration remains at `JammaLib/src/gui/GuiHud.h:34`, `JammaLib/src/engine/Scene.h:28`, `JammaLib/src/engine/Scene.h:372`, and `Jamma/resources/ResourceList.txt:20` and `:68`–`:76`.
- Why it matters: it materially expands UI, graphics, binary asset, project-membership, and regression scope beyond the branch name.
- Recommended disposition: retain with an explicit combined-scope rationale, or move the HUD lineage to a separately reviewed branch.
- Protected timing concepts affected: none.
- Verification: human scope decision; if retained, later GUI/graphics/resource and manual HUD coverage; if moved, regenerate `master...HEAD` inventory.
- Human decision: accepted; retain HUD in this merge and keep it outside timing-cleanup scope ([decision](decisions.md#01---diff-inventory)).

## F-002 — Decide whether VST3 parity/state/mapping belongs in this merge

- Stage / reviewer: S01-02; corroborated by S05 history.
- Scope reviewed / exclusions: branch scope and interface expansion; no VST correctness/lifetime judgment.
- Severity: must fix before merge.
- Evidence: merge `880112d438e21983688395f60f970427d7f1d0b9` brought the independent VST3 parity lineage into the timing branch (25 files, +2,079/−347 versus first parent). Current surface includes `JammaLib/src/vst/Vst3Plugin.h:29`, `JammaLib/src/vst/IVstPlugin.h:24`, `:73`, `:84`–`:97`, and `:132`–`:137`.
- Why it matters: plugin hosting, persistence, editor, MIDI mapping, GL/resource, and project/test contracts substantially expand the merge surface.
- Recommended disposition: retain with an explicit combined-scope rationale, or move the VST3 lineage to a separately reviewed branch.
- Protected timing concepts affected: monotonic scene coordinate and plugin-facing musical transport remain distinct consumers if retained.
- Verification: human scope decision; if retained, later VST interface/lifetime/persistence/MIDI/manual coverage; if moved, verify intended musical-transport consumers remain.
- Human decision: accepted; retain VST3 parity in this merge and keep it outside timing-cleanup scope ([decision](decisions.md#01---diff-inventory)).

## F-003 — Decide whether window-placement persistence belongs in this merge

- Stage / reviewer: split from S01-03.
- Scope reviewed / exclusions: objective commit/file scope only; persistence correctness belongs to later phases.
- Severity: follow-up.
- Evidence: commits `efe496292e2046f856a684a98fd956aac3e56a05`, `8f50e136d18d7632f8b852ad4170bd73ce7b5acd`, and `704828f6abaf136a1327bfb3b750f712088519e2` add and repair window/default persistence retained at `Jamma/src/Main.cpp:349`, `:359`–`:368`, and `:451`–`:461`.
- Why it matters: this is a separable persistence contract not implied by remote-join alignment.
- Recommended disposition: retain with release-scope rationale, or move to a separately reviewed change.
- Protected timing concepts affected: none.
- Verification: human scope decision; if retained, Stage 16 persistence matrix and focused window restore behavior.
- Human decision: accepted; retain window-placement persistence in this merge and keep it outside timing-cleanup scope ([decision](decisions.md#01---diff-inventory)).

## F-004 — Decide whether repository build/agent tooling belongs in this merge

- Stage / reviewer: split from S01-03.
- Scope reviewed / exclusions: objective commit/file scope only; no build-wrapper quality judgment.
- Severity: follow-up.
- Evidence: commits `5daefa3161f972203216b91f28bc3d55d748392a`, `b43abbebfdf16147808fbe6a3bcc89934fd07d3a`, `4a74ca978da2068f1fef97648cd52b12740e7cdf`, and `f37e9c0f3cc9ad097fec5b4ef24f0740b05571ef` add/update the worktree skill and MSBuild environment wrapper, now normative at `AGENTS.md:29`–`:31`.
- Why it matters: repository policy/tooling is independently useful but separable from product timing behavior and requires its own review rationale.
- Recommended disposition: retain as an explicit build prerequisite, or move to a separately reviewed tooling change.
- Protected timing concepts affected: none.
- Verification: human scope decision; if retained, Stage 21 command validation using local `.vscode/tasks.json` and the wrapper.
- Human decision: accepted; retain repository build/agent tooling in this merge and keep it outside timing-cleanup scope ([decision](decisions.md#01---diff-inventory)).

## F-005 — Translate NINJAM follow policy before entering the core loop hierarchy

- Stage / reviewer: S02-01; refined by S13-04.
- Scope reviewed / exclusions: dependency direction; no behavioral-correctness claim.
- Severity: follow-up.
- Evidence: `JammaLib/src/engine/LoopTake.h:21` imports the NINJAM command header; `LoopTake::ApplyTimingCommand` and Station fan-out accept `ninjam::NinjamLocalFollowPolicy` at `JammaLib/src/engine/LoopTake.h:235`–`:239` and `JammaLib/src/engine/Station.h:113`–`:128`. `AudioHost` already interprets disable/invalidation at `JammaLib/src/audio/AudioHost.cpp:174`–`:186` and `:298`–`:305`.
- Why it matters: low-level local loop state depends on session policy and duplicates the `NoSync` decision.
- Recommended disposition: move policy interpretation to NINJAM/AudioHost and expose neutral accepted correction/invalidation operations to engine entities.
- Phase 3 refinement: use separate neutral engine operations for an accepted signed correction carrying epoch/generation and for explicit epoch reset/invalidation. Remove the NINJAM policy, command reason, and unused scene-coordinate payload from `Station`/`LoopTake`; `ContinuousSync`, `BlockSync`, and `NoSync` remain distinct at the AudioHost boundary.
- Protected timing concepts affected: follow policy, `NoSync` invalidation, per-loop phase; all remain separate.
- Verification: include-graph check plus continuous/block/no-sync, reconnect, and invalidation tests.
- Human decision: accepted for later reconciliation ([decision](decisions.md#findings)).

## F-006 — Consolidate the common sync-map owner without deleting live entity state prematurely

- Stage / reviewer: S02-02; reconciled with S06 current-reachability evidence and refined by S08-03/S13-03.
- Scope reviewed / exclusions: future structural ownership, not a dead-code finding or correctness proof.
- Severity: follow-up.
- Evidence: authoritative-looking map state is held by `AudioHost` at `JammaLib/src/audio/AudioHost.h:141`–`:144`, then copied through begin/rebase APIs at `JammaLib/src/audio/AudioHost.cpp:352`–`:364`; each `LoopTake` stores and recomputes parallel map geometry at `JammaLib/src/engine/LoopTake.h:406`–`:410` and `JammaLib/src/engine/LoopTake.cpp:602`–`:679`.
- Why it matters: N+1 mutable representations enlarge a repeatedly changed consistency surface.
- Recommended disposition: simplify later so AudioHost owns the common ruler and supplies one mapped source coordinate; retain per-entity audio/MIDI anchors and modulo restore. Current per-take fields are live until that replacement is implemented.
- Protected timing concepts affected: sync map, mapped elapsed time, source/scene anchors, per-loop phase, monotonic scene coordinate; none may be collapsed.
- Verification: unequal loop lengths, intentional offsets, rebase/wrap, reconnect, and `NoSync` invalidation.
- Phase 2 enrichment: S08-03 shows every followed block recomputes the same 64-bit common mapped source coordinate independently in each take (`AudioHost.cpp:456`–`:459`; `LoopTake.cpp:640`–`:679`). The approved consolidation should compute it once per block, then apply per-entity anchors/modulo without collapsing per-loop phase.
- Phase 2 enrichment decision: accepted; compute the common mapped source coordinate once per block while preserving entity-specific anchors and modulo ([decision](decisions.md#findings)).
- Phase 3 refinement: replace begin/rebase/restore geometry propagation with neutral entity operations that capture a missing anchor against, or restore from, the single AudioHost-computed source coordinate. Rebasing changes only the AudioHost ruler and must not recapture entity anchors. Estimated net production deletion: 45–75 lines plus five per-take map fields.
- Human decision: accepted for later reconciliation ([decision](decisions.md#findings)).

## F-007 — Separate value-only local timing contracts from the Quantiser/UI aggregate

- Stage / reviewer: S02-03.
- Scope reviewed / exclusions: module placement and dependency direction; no naming/numerical judgment.
- Severity: follow-up.
- Evidence: timing value types share `JammaLib/src/engine/Quantiser.h:43`–`:137` with action/graphics/GUI dependencies at `:3`–`:18` and stateful controller declarations at `:170` and `:353`; `JammaLib/src/ninjam/NinjamTimingCoordinator.h:6`–`:9` includes the aggregate to exchange `engine::QuantisationTiming`.
- Why it matters: session timing consumers inherit unrelated presentation/control coupling.
- Recommended disposition: move immutable local geometry/timing values into a small engine timing-contract header; keep Quantiser/controller in the engine interaction layer.
- Protected timing concepts affected: local timing, local grain, active grid, remote geometry remain separate types/fields.
- Verification: include-graph check and coordinator/local-timing tests.
- Human decision: accepted with the no-new-class constraint ([decision](decisions.md#02---logical-layout)).

## F-008 — Move the local transport-offset mailbox out of the NINJAM command module

- Stage / reviewer: S02-04.
- Scope reviewed / exclusions: class placement only; synchronization and hot-path behavior belong to Phase 2.
- Severity: follow-up.
- Evidence: `LocalTransportOffsetLoopFracMailbox` is declared in namespace/file `ninjam` at `JammaLib/src/ninjam/NinjamAudioTimingCommand.h:147`–`:193`, while its production flow is local UI/Scene -> `AudioHost` -> local stations at `JammaLib/src/audio/AudioHost.h:126` and `JammaLib/src/audio/AudioHost.cpp:332`–`:350`.
- Why it matters: a disconnected/local-only control is semantically owned by a remote-session module.
- Recommended disposition: move to AudioHost-private local control or a proven generic latest-value utility; preserve latest-wins/explicit-zero behavior.
- Protected timing concepts affected: local timing and follow policy remain independent.
- Verification: mailbox tests plus disconnected and `NoSync` local-offset scenarios.
- Human decision: accepted for later reconciliation ([decision](decisions.md#findings)).

## F-009 — Complete update-to-command materialization in the NINJAM integration layer

- Stage / reviewer: S02-05; refined by S07-04/S13-02.
- Scope reviewed / exclusions: orchestration placement; UI presentation and runtime correctness excluded.
- Severity: follow-up.
- Evidence: the coordinator decides policy/generation/update semantics, but `engine::Scene` constructs every command field and invalidation at `JammaLib/src/engine/Scene.cpp:254`–`:305` and `:403`–`:482`; AudioHost then applies it.
- Why it matters: command meaning and construction are split across coordinator and Scene, so field changes require synchronized mechanical edits and thicken glue code.
- Recommended disposition: have the existing NINJAM integration owner emit the complete immutable desired remote transport state defined by F-024; keep Scene for prompts/forwarding and AudioHost for block-boundary comparison/application.
- Protected timing concepts affected: remote join, follow policy, command lifecycle, `NoSync` invalidation remain distinct.
- Verification: coordinator command-contract tests and integration forwarding tests; producer call-site audit; overlapping job/UI publication with one coherent reader; assert one explicit integration owner and restrict the public publication surface.
- Phase 2 enrichment: S07-04 found the mailbox's stated single job-thread producer contract is inaccurate; job and UI publication sites are currently serialized only by outer `Scene::_sceneMutex`. The approved owner move must make producer serialization explicit and self-enforcing.
- Phase 2 enrichment decision: accepted; make command production explicit and self-enforcing within the approved owner move ([decision](decisions.md#findings)).
- Phase 3 refinement: remove Scene's optionals-to-event translation and separate connect/disconnect invalidation construction. The producer value contains session epoch, follow policy, full validated device-rate remote geometry, and timestamped remote master phase; no ordered queue, standalone invalidation, or phase-delta command is retained.
- Human decision: accepted for later reconciliation ([decision](decisions.md#findings)).

## F-010 — Normalize new timing aggregate acronym casing

- Stage / reviewer: S03-01.
- Scope reviewed / exclusions: naming form only, not BPM/BPI semantics.
- Severity: follow-up.
- Evidence: new `LocalAudioGeometry::BPI` and `RemoteTransportGeometry::BPI`/`BPM` at `JammaLib/src/engine/Quantiser.h:52`, `:97`, and `:99` conflict with neighboring `Bpm`/`Bpi` at `:135`–`:136` and `JammaLib/src/ninjam/NinjamTiming.h:16`–`:17`.
- Why it matters: adjacent public timing aggregates expose inconsistent identifier forms.
- Recommended disposition: mechanically rename to `Bpi`/`Bpm` without semantic changes.
- Protected timing concepts affected: none.
- Verification: compile affected targets and identifier audit.
- Human decision: accepted with the documented acronym-casing constraint ([decision](decisions.md#03---conventions)).

## F-011 — Apply the immediate NINJAM aggregate member convention consistently

- Stage / reviewer: S03-02; reconciled with S04-05.
- Scope reviewed / exclusions: immediate NINJAM aggregate neighborhood only; not a repository-wide public-field rule.
- Severity: follow-up.
- Evidence: lower-camel public fields in `JammaLib/src/ninjam/ExportLaneTiming.h:19`, `:20`, `:32`, `:48`, `:54` and `NinjamMetronomeTiming.h:16`, `:18`, `:27`, `:28`, `:44`, `:46` contrast with neighboring PascalCase NINJAM aggregates at `JammaLib/src/ninjam/NinjamTiming.h:27`–`:32`.
- Why it matters: adjacent public NINJAM value APIs switch naming form without a domain reason.
- Recommended disposition: rename these public fields to PascalCase. If F-017 is also approved, implement overlapping export fields once using its semantic PascalCase names.
- Protected timing concepts affected: none.
- Verification: focused helper/metronome tests and retired-name search.
- Human decision: accepted for later reconciliation ([decision](decisions.md#03---conventions)).

## F-012 — Move substantial non-template NINJAM runtime bodies out of public headers

- Stage / reviewer: S03-03.
- Scope reviewed / exclusions: header/implementation placement only; no algorithm, performance, or ownership claim.
- Severity: follow-up.
- Evidence: substantial mailbox/timing bodies remain at `JammaLib/src/ninjam/NinjamAudioTimingCommand.h:61`, `:83`, `:154`, `:162`; `NinjamTiming.h:45`–`:140`; and `NinjamTimingObservationMailbox.h:16`, `:37`, unlike paired `.cpp` neighbors such as `ExportLaneTiming`, `NinjamMetronomeTiming`, and `NinjamTimingTracker`.
- Why it matters: mutable mailbox mechanics and runtime calculations expand recompilation and implementation coupling for every includer.
- Recommended disposition: retain tiny/compile-time helpers inline; move non-template runtime bodies behind declarations, coordinated with any accepted owner move from F-008.
- Protected timing concepts affected: none.
- Verification: incremental library build and relevant NINJAM timing/mailbox tests.
- Human decision: accepted for later reconciliation ([decision](decisions.md#03---conventions)).

## F-013 — Reserve “grain” for local audio construction, not remote grid cells

- Stage / reviewer: S04-01.
- Scope reviewed / exclusions: vocabulary through coordinator, prompt, and command; active-grid runtime correctness excluded.
- Severity: must fix before merge.
- Evidence: `NinjamTempoChange::GrainSamps` at `JammaLib/src/ninjam/NinjamTimingCoordinator.h:56` is calculated as remote interval / authoritative remote BPI at `JammaLib/src/ninjam/NinjamTimingCoordinator.cpp:306`–`:313`, copied to quantisation at `:316`–`:327`, and displayed as `Grain` at `JammaLib/src/engine/Scene.cpp:378`–`:382`.
- Why it matters: it directly contradicts the protected glossary: local grain is an audio construction unit, not a remote beat/grid cell.
- Recommended disposition: rename the remote-derived value and UI text to `RemoteGridStepSamps` / “Remote grid step”; reserve `GrainSamps` for local construction geometry.
- Protected timing concepts affected: local grain, active/remote grid, remote timing, local timing.
- Verification: symbol/UI audit proving remote-BPI consumers use remote-grid vocabulary and remaining grain names are local construction.
- Human decision: accepted with `RemoteGridStepSamps` terminology ([decision](decisions.md#04---vocabulary)).

## F-014 — Make accepted remote command authority and coordinates explicit

- Stage / reviewer: S04-02.
- Scope reviewed / exclusions: naming at coordinator/Scene/AudioHost boundary; no mathematical-correctness claim.
- Severity: must fix before merge.
- Evidence: generic `NinjamClockSettings` fields at `JammaLib/src/ninjam/NinjamTimingCoordinator.h:69`–`:84` become `AbsolutePhaseSamps` and `PhaseObservationSample` at `JammaLib/src/engine/Scene.cpp:432`–`:449` and `JammaLib/src/ninjam/NinjamAudioTimingCommand.h:35`–`:50`; `AudioHost` then overwrites `stationDelta` from remote correction to mapped source correction at `JammaLib/src/audio/AudioHost.cpp:169` and `:241`–`:247`.
- Why it matters: “absolute” names a wrapped remote phase, while one variable spans remote-master and local-source coordinates at the most sensitive boundary.
- Recommended disposition: apply the report's rename group: accepted remote replacement, remote master length/phase, device observation sample, remote master correction, and separate local source correction.
- Protected timing concepts affected: remote timing/authority, follow policy, master phase, sync map, mapped source correction, per-loop phase remain separate.
- Verification: command/integration tests and a manual trace of replacement plus discipline coordinates.
- Human decision: accepted with the naming preferences recorded by the human reviewer ([decision](decisions.md#04---vocabulary)).

## F-015 — Name Timer-absolute and device-audio observation anchors distinctly

- Stage / reviewer: S04-03.
- Scope reviewed / exclusions: timestamp vocabulary; arithmetic/seqlock correctness excluded.
- Severity: must fix before merge.
- Evidence: adjacent `LocalBlockStartSample`/`AudioBlockStartSample` at `JammaLib/src/ninjam/NinjamTiming.h:21`–`:37` are populated respectively from `Timer::AbsoluteSamplePos(...)` and the raw device counter at `JammaLib/src/audio/AudioHost.cpp:444`–`:452`; the first becomes generic `LocalSample` at `JammaLib/src/ninjam/NinjamTimingCoordinator.cpp:99`–`:105`. MIDI's same device coordinate is merely `Sample` at `JammaLib/src/midi/MidiClockAnchor.h:8`–`:28`.
- Why it matters: Timer absolute geometry can change under accepted timing while the device audio counter remains monotonic; current names invite substitution and confusion with scene time.
- Recommended disposition: use explicit `LocalMasterAbsoluteSampleAtObservation` and `DeviceAudioSampleAtObservation`/`DeviceAudioSamplePosition` names.
- Protected timing concepts affected: local master absolute position, device counter, monotonic scene coordinate, remote timing remain distinct.
- Verification: tests with distinct sentinel values and unqualified-name audit.
- Human decision: accepted with explicit clock-domain naming ([decision](decisions.md#04---vocabulary)).

## F-016 — Rename the MIDI automation global-sample origin

- Stage / reviewer: S04-04.
- Scope reviewed / exclusions: automation-anchor vocabulary; playback correctness/lifetime excluded.
- Severity: must fix before merge.
- Evidence: `MidiLoop::LoopPhaseAnchor` at `JammaLib/src/midi/MidiLoop.h:175`–`:183` is documented as global sample mapping to loop-relative zero; `EndRecord` stores `startGlobalSample` at `JammaLib/src/midi/MidiLoop.cpp:253`–`:259`, and Station copies it for automation dispatch at `JammaLib/src/engine/Station.h:334`–`:344`.
- Why it matters: the name competes with actual per-loop event phase and source/scene anchors even though it is an automation recording origin.
- Recommended disposition: rename accessor/member/dispatch copy to `AutomationGlobalSampleOrigin` with repository member casing.
- Protected timing concepts affected: automation origin, MIDI event cursor/per-loop phase, source/scene anchor remain separate.
- Verification: automation-use search and focused automation/timing tests.
- Human decision: accepted for later reconciliation ([decision](decisions.md#04---vocabulary)).

## F-017 — Give export-lane remote/local cursors semantic unit-bearing names

- Stage / reviewer: S04-05; coordinated with F-011.
- Scope reviewed / exclusions: value vocabulary only; latency formula correctness and dormant-path retention excluded.
- Severity: follow-up.
- Evidence: `ExportLaneTimingInput::n`, `pos`, and `length` at `JammaLib/src/ninjam/ExportLaneTiming.h:12`–`:25` represent a delay-line cursor, remote interval phase, and remote interval length and are mixed at `JammaLib/src/ninjam/ExportLaneTiming.cpp:20`–`:66`.
- Why it matters: algebraic names obscure coordinate/lifetime distinctions in a formula combining remote interval and local device timing.
- Recommended disposition: rename to semantic unit-bearing PascalCase names such as `DelayWriteCursorSamps`, `RemoteIntervalPhaseSamps`, and `RemoteIntervalLengthSamps`, plus corresponding state names, if the helper is retained.
- Protected timing concepts affected: remote wrapped phase/length and local delay-line/device timing remain distinct.
- Verification: pure-helper tests and call-site initialization review.
- Human decision: accepted only for Jamma-owned code; upstream NJClient is immutable ([decision](decisions.md#04---vocabulary)).

## F-018 — Remove duplicated stored `SyncPhaseMap::SourcePhaseAtOrigin`

- Stage / reviewer: S05-01 merged with S06-01.
- Scope reviewed / exclusions: historical residue plus current references; no timing redesign.
- Severity: follow-up.
- Evidence: history `1d665d2` -> `0d90bac` -> `bef7943` -> `e72f3b0` replaced phase-origin consumers with a monotonic source coordinate. The member remains at `JammaLib/src/ninjam/NinjamLoopAlignment.h:54` and is assigned at `:84`–`:90`, but has no current read; derived `SourcePhaseAt(...)` uses `SourceCoordinateAt(...)` at `:65`–`:81`.
- Why it matters: duplicated stored representation suggests two authorities and risks reviving the superseded phase-origin model.
- Recommended disposition: remove only the stored member/comment/assignment; retain derived phase, source coordinate, scene coordinate, and per-entity anchors.
- Protected timing concepts affected: sync map, mapped elapsed, source/scene anchor, master phase, per-loop phase all remain distinct.
- Verification: incremental build; sync-map, unequal-length/offset, reconnect/rebase, and `NoSync` tests.
- Human decision: accepted as a critical single-authority cleanup ([decision](decisions.md#05---history)).

## F-019 — Remove the never-published `AlignmentReceipt` path

- Stage / reviewer: S05-02 split and confirmed by S06-02.
- Scope reviewed / exclusions: dead receipt subset only; live/dormant snapshots and coordinator telemetry are explicitly retained for Phase 2/3 review.
- Severity: follow-up.
- Evidence: type/API at `JammaLib/src/engine/LoopTake.h:93`–`:106` and `:252`; eleven zero-initialized atomics at `:420`–`:430`; only loads at `JammaLib/src/engine/LoopTake.cpp:691`–`:714`, with immediate `nullopt` at `:695`–`:697`; no writer exists, making the logger branch at `JammaLib/src/engine/Station.cpp:2378`–`:2394` unreachable.
- Why it matters: every take carries unused atomics/API/logger complexity that obscures reachable diagnostics.
- Recommended disposition: remove the receipt type, API, atomics, and unreachable logger branch unless a human defines a new supported publication contract. Retain `_ninjamBeforePositions`, verbose before/after snapshots, and coordinator diagnostics pending Stages 8/17.
- Protected timing concepts affected: none operational; diagnostic mirrors only.
- Verification: incremental build; normal/verbose logging; focused remote-join/local-loop regression.
- Phase 2 enrichment: S08-02 separates this dead subset from live callback-reachable logging. F-034 owns removal of callback logging and off-thread bounded diagnostics; F-019 remains the narrow dead-receipt deletion.
- Human decision: accepted with retained, configuration-gated timing logging and zero disabled-path cost ([decision](decisions.md#05---history)).

## F-020 — Remove ten unregistered coloured trigger-back textures if HUD is retained

- Stage / reviewer: S06-03; dependent on F-001.
- Scope reviewed / exclusions: resource reachability/project copy; no visual-design judgment.
- Severity: follow-up.
- Evidence: ten green/red `trigger_back*` variants (164,020 bytes total) have no source or `ResourceList.txt` reference. Only neutral `trigger_back` is registered at `Jamma/resources/ResourceList.txt:76` and used at `JammaLib/src/gui/GuiHud.cpp:675`–`:679`; `Jamma/Jamma.vcxproj:200`–`:202` nevertheless copies the full resource tree.
- Why it matters: unreachable binary payload is shipped and expands provenance/review surface.
- Recommended disposition: if F-001 retains HUD, remove the ten variants; if HUD is moved, resolve them with that lineage and do not create a duplicate cleanup.
- Protected timing concepts affected: none.
- Verification: app build/output audit and manual trigger default/hover/down/out rendering.
- Human decision: accepted, conditional on retained HUD scope ([decision](decisions.md#findings)).

## F-021 — Remove non-`AudioProc` NJClient access from the audio callback

- Stage / reviewer: S07-01, corroborated by S08-01/S12-02 and refined by S13-06.
- Scope reviewed / exclusions: NJClient caller/thread contract and callback cost; upstream NJClient remains read-only.
- Severity: merge blocker.
- Evidence: `AudioHost::_OnAudio` obtains live timing at `JammaLib/src/audio/AudioHost.cpp:437`–`:453`, reaching `_client->GetPosition`, `GetSampleRate`, `GetActualBPM`, and `GetBPI` at `JammaLib/src/ninjam/NinjamConnection.cpp:709`–`:730`, while the job thread calls `Run()` and snapshot getters. The compile-time-disabled export-compensation path would also call `_client->GetPosition` at `NinjamConnection.cpp:579`–`:605`. The bundled contract says `AudioProc`, and only `AudioProc`, belongs on the audio thread at `lib/njclient/njclient.h:165`–`:172`.
- Why it matters: the callback violates the upstream thread-affinity contract, can assemble timing fields from different upstream states, and repeatedly enters an unbounded connection-use acquisition path. Adding a mutex would violate real-time rules.
- Recommended disposition: publish one coherent, timestamped remote observation from the permitted job/network owner and combine it with separate device/Timer block anchors at the audio boundary. Keep only `AudioProc` reachable from the callback and do not edit NJClient.
- Phase 3 refinement: capture/publish the exact pre-advance block timing through the existing Jamma-owned `AudioProc` seam without another NJClient call, then delete the Controller/Session/Connection `GetLiveTiming` adapter chain. Estimated net reduction after bounded publication: 15–35 lines; upstream remains read-only.
- Protected timing concepts affected: remote observation, remote phase/rate, device-audio anchor, and Timer-absolute anchor remain distinct.
- Verification: static audio-call-chain audit; concurrent sentinel-publication test; delayed-observation projection; join/reconnect/disconnect and export-compensation scenarios; race tooling where practical.
- Human decision: accepted ([decision](decisions.md#findings)).

## F-022 — Move the empty-scene timing reset off the audio callback

- Stage / reviewer: S07-02, corroborated by S08-04.
- Scope reviewed / exclusions: callback-reachable Scene/network/Quantiser state and destruction cost; general empty-scene product behavior excluded.
- Severity: must fix before merge.
- Evidence: `Scene::OnTick` calls `_ClearTimingState(false)` when the audio snapshot has no takes at `JammaLib/src/engine/Scene.cpp:1359`–`:1378`. That path reads plain job-owned tracker state and mutates Quantiser/remote-grid state through raw `_stations` at `Scene.cpp:2271`–`:2275` and `JammaLib/src/engine/Quantiser.cpp:132`–`:153`, `:203`–`:247`.
- Why it matters: this is a data race/compound-state violation and can release strings/shared pointers/containers on the callback. It also bypasses the immutable station snapshot pattern.
- Recommended disposition: make empty/non-empty an edge published to the job owner, perform non-real-time cleanup there, and send only a bounded immutable invalidation to the audio boundary. Use the existing published station snapshot for any callback traversal.
- Protected timing concepts affected: `NoSync` invalidation, local grid state, and remote timing availability remain distinct.
- Verification: prerequisite contracts that an empty connected session preserves accepted remote timing and removing the final local take while disconnected clears local timing exactly once; race those transitions with connect/disconnect and remote-grid updates; callback audit for locks/allocation/destruction/raw `_stations`; ThreadSanitizer or equivalent where practical.
- Human decision: accepted ([decision](decisions.md#findings)).

## F-023 — Publish a coherent local Timer transport observation

- Stage / reviewer: S07-03; numerical width is separately F-029.
- Scope reviewed / exclusions: cross-thread compound consistency; timing policy and arithmetic mechanics excluded.
- Severity: must fix before merge.
- Evidence: Timer fields are individually atomic at `JammaLib/src/utils/Timer.h:69`–`:111`, but audio applies/ticks a multi-field geometry at `Timer.cpp:40`–`:58`, `:205`–`:237`, while the job coordinator independently reads length, phase, and absolute position at `JammaLib/src/ninjam/NinjamTimingCoordinator.cpp:216`–`:240`.
- Why it matters: a reader can observe a length/count/phase tuple that never existed, producing a wrong correction even though no scalar data race occurs.
- Recommended disposition: publish one versioned local-transport observation from the audio boundary and consume it as a value in the coordinator. Reuse an existing owner/value contract; do not add a class solely for this change.
- Protected timing concepts affected: Timer absolute position, master phase, scene coordinate, and device-audio position remain distinct fields/domains.
- Verification: alternating sentinel-geometry concurrency test that never yields a mixed tuple; replacement/tick boundary test; late-observation tests using one coherent local snapshot.
- Human decision: accepted ([decision](decisions.md#findings)).

## F-024 — Publish one latest complete desired remote transport state

- Stage / reviewer: S09-01; disposition replaced by the human decision and refined by S13-02/S15-02.
- Scope reviewed / exclusions: complete desired-state semantics across integration publication and audio-boundary comparison; no ordered command queue or standalone delta/invalidation compatibility.
- Severity: merge blocker.
- Evidence: the mailbox retains only the latest command at `JammaLib/src/ninjam/NinjamAudioTimingCommand.h:53`–`:119`, while Scene can publish `Invalidate -> Replace` and `Replace -> PhaseDiscipline` between callbacks at `JammaLib/src/engine/Scene.cpp:254`–`:305`, `:403`–`:481`; AudioHost consumes only one at `JammaLib/src/audio/AudioHost.cpp:162`–`:165`.
- Why it matters: a delta does not subsume replacement geometry or session invalidation. Coalescing can lose an accepted tempo change, apply a correction against old geometry, or retain old-session gates/anchors.
- Recommended disposition: after prerequisite regressions, publish only the latest complete desired remote transport state: session epoch, follow policy, full validated device-rate remote geometry, and remote master phase at an explicit observation sample/presence. AudioHost compares it with the last applied state: epoch change clears map/anchors/gates; geometry change replaces Timer timing; unchanged geometry derives phase correction; `NoSync` clears authority without moving cursors. Keep production with the NINJAM integration owner per F-009.
- Protected timing concepts affected: remote authority, follow policy, Timer geometry, sync map, anchors, mapped elapsed time, and per-loop phase remain distinct.
- Verification: production-faithful former `Invalidate -> Replace` and `Replace -> Discipline` intent sequences before one callback must resolve to one complete final desired state; test same-state idempotence, epoch/geometry/phase comparisons, Timer/map/audio/MIDI/generation coherence, both sync policies, unequal loop lengths/offsets, and valid zero observation.
- Documentation obligation: replace claims that each event publication is consumed exactly once or that remote geometry never replaces Timer timing with the approved desired-versus-applied rule; clearly label current defects versus target behavior until implemented.
- Human decision: accepted only in this replacement form; the ordered/delta command choice was rejected ([decision](decisions.md#09---timing-correctness)).

## F-025 — Reset every timing generation gate at a reconnect boundary

- Stage / reviewer: S09-02.
- Scope reviewed / exclusions: direct AudioHost invalidation fan-out and two-session behavior.
- Severity: merge blocker.
- Evidence: coordinator connect resets generation to zero at `JammaLib/src/ninjam/NinjamTimingCoordinator.cpp:10`–`:29`. AudioHost handles invalidation as `disablesSync`, then skips Station/LoopTake command application at `JammaLib/src/audio/AudioHost.cpp:174`–`:186`, `:298`–`:320`; consequently the reset branch in `LoopTake::ApplyTimingCommand` at `JammaLib/src/engine/LoopTake.cpp:518`–`:531` is unreachable and `_audioTimingGeneration` survives reconnect.
- Why it matters: reconnect generations `1..N` can be rejected by takes after a prior session reached generation N, splitting Timer/map authority from audio and MIDI cursors.
- Recommended disposition: add a two-session production-path regression, then reset all consumer gates atomically at invalidation or introduce an explicit session epoch plus within-session generation. A globally monotonic generation alone does not solve F-024.
- Protected timing concepts affected: session authority/generation, `NoSync`, anchors, Timer geometry, and per-loop phase remain separate.
- Verification: session 1 beyond generation 1; production-path invalidate; session 2 generation 1 accepted by Timer and real audio/MIDI takes; stale session-1 command rejected; free-run and offsets preserved.
- Human decision: accepted ([decision](decisions.md#findings)).

## F-026 — Anchor the initial join delta to one observation instant

- Stage / reviewer: S09-03.
- Scope reviewed / exclusions: observation-time behavior; numerical wrap/overflow is Stage 11.
- Severity: must fix before merge.
- Evidence: the first observation carries a callback Timer anchor at `JammaLib/src/audio/AudioHost.cpp:441`–`:453`, but coordinator begins alignment with job-time `clock.SampOffset()` at `JammaLib/src/ninjam/NinjamTimingCoordinator.cpp:136`–`:146`. Tracker freezes that live local phase against the older remote observation at `NinjamTimingTracker.cpp:88`–`:98`, and the later join event bypasses normal late-observation projection at coordinator lines `219`–`:240`.
- Why it matters: populated joins become scheduler-dependent by approximately the job delay in samples.
- Recommended disposition: project local phase to the initial observation anchor before beginning alignment, or retain both anchors and derive the correction at the audio boundary.
- Protected timing concepts affected: remote master phase, local Timer phase, observation anchors, join policy, and scene/source mapping remain distinct.
- Verification: zero and multi-block delays before processing the same initial observation must emit the same join delta; positive/negative/half-interval cases; unequal loop lengths and offsets.
- Human decision: accepted ([decision](decisions.md#findings)).

## F-027 — Model physical loss and auto-reconnect as timing-session epoch transitions

- Stage / reviewer: S10-01; persisted-start compatibility path added by S16-01.
- Scope reviewed / exclusions: connection availability state/recovery; lifetime and command mechanics are F-033/F-024.
- Severity: merge blocker.
- Evidence: `NinjamConnection::Pump` can report failure/retry while `NinjamSession::Pump` returns only `nullopt`; Scene does nothing when no snapshot arrives. Coordinator/tracker connected state, requests/prompts, remote stations, and AudioHost's old sync map can therefore remain active. Automatic retry can resume snapshots without `Connect` establishing a fresh timing epoch (`JammaLib/src/ninjam/NinjamConnection.cpp:299`–`:404`; `NinjamSession.cpp:486`–`:500`; `JammaLib/src/engine/Scene.cpp:1382`–`:1396`).
- Phase 3 evidence: well-formed/default `.jam` auto-connect calls `Scene::FromFile -> NinjamController::LoadConfig -> NinjamSession::Start` without the interactive `PrepareTempoSyncOnConnect` path (`Scene.cpp:249`–`:288`, `:664`; `NinjamController.cpp:7`–`:19`). The timing tracker can therefore remain disconnected even while NINJAM audio connects.
- Why it matters: physical loss is indistinguishable from “no update,” so stale remote authority may continue and a new physical connection may reuse old state.
- Recommended disposition: publish explicit connection availability and epoch from the connection/integration owner, and route persisted/default and interactive starts through one existing-owner lifecycle operation. Every start begins at `NoSync`; loss emits one complete `NoSync` transition; successful retry accepts fresh valid timing only in a new epoch. Do not persist the live epoch/authority.
- Protected timing concepts affected: connection availability, remote session authority, follow policy, sync map, and generation remain distinct.
- Verification: cable/server loss, retry failure/backoff/success, manual disconnect during retry, exactly-once invalidation, fresh epoch/generation, remote-station cleanup, and local free-run.
- Human decision: accepted ([decision](decisions.md#findings)).

## F-028 — Recover explicitly from invalid/absent timing and tick deadlines without observations

- Stage / reviewer: S10-02.
- Scope reviewed / exclusions: coordinator failure transitions; numeric validation formulas excluded.
- Severity: must fix before merge.
- Evidence: coordinator returns early for invalid/disconnected/zero timing before validity-loss recovery or request-deadline logic in `JammaLib/src/ninjam/NinjamTimingCoordinator.cpp:68`–`:94`. If observations stop, sent request deadlines do not progress; after accepted authority, malformed timing silently leaves the old map active.
- Why it matters: request/prompt state can hang indefinitely and invalid remote geometry can leave stale authority disciplining local playback.
- Recommended disposition: add an observation-independent job tick/deadline transition and distinguish pre-authority waiting from post-authority loss. Make `NoSync` invalidation edge-triggered/idempotent and coordinate it with F-027.
- Protected timing concepts affected: request/acknowledgement, validity, follow policy, and remote authority remain distinct.
- Verification: no-observation timeout/retry exhaustion; valid→invalid→valid; malformed/zero interval/rate/BPM/BPI; one invalidation per loss edge; reconnect recovery.
- Human decision: accepted ([decision](decisions.md#findings)).

## F-029 — Widen Timer absolute arithmetic before projecting long-session observations

- Stage / reviewer: S11-01; consumes S09/S07 handoffs.
- Scope reviewed / exclusions: Windows integer width/overflow; coherent publication is F-023.
- Severity: must fix before merge.
- Evidence: `Timer::AbsoluteSamplePos` returns `unsigned long` and multiplies `_loopCount * loopLength` in that type at `JammaLib/src/utils/Timer.h:72`–`:82`. `Timer::Tick` also computes `phase + sampsIncrement` in Windows 32-bit `unsigned long` before division/modulo at `Timer.cpp:49`–`:57`, so a near-`UINT32_MAX` seed can undercount wraps even while individual inputs are representable. The absolute coordinate wraps after about 24 h 51 min at 48 kHz; AudioHost widens only after truncation at `JammaLib/src/audio/AudioHost.cpp:445`–`:452`.
- Why it matters: delayed-observation age/projection can collapse after wrap and produce wrong timing decisions during long sessions.
- Recommended disposition: use 64-bit intermediate and result arithmetic for Tick addition, loop multiplication, and absolute coordinates end to end in the existing Timer/timing value boundary; combine it with F-023's coherent snapshot.
- Protected timing concepts affected: Timer absolute, scene coordinate, and device-audio position remain separate 64-bit rulers.
- Verification: values spanning `UINT32_MAX`, near-maximum seed/phase plus increment with exact loop-count/phase result, multi-interval multiplication before widening, distinct Timer/device sentinels, and long-running projection.
- Human decision: accepted ([decision](decisions.md#findings)).

## F-030 — Separate observation validity from the valid sample-zero coordinate

- Stage / reviewer: S11-02.
- Scope reviewed / exclusions: boundary/sentinel semantics.
- Severity: must fix before merge.
- Evidence: zero is valid independently in both observation domains, yet replacement treats device-audio sample zero as an absent anchor at `JammaLib/src/ninjam/NinjamTiming.h:100`–`:123`, and coordinator treats Timer-absolute sample zero as absent/fallback at `NinjamTimingCoordinator.cpp:219`–`:236`; existing tests explicitly encode these fallbacks.
- Why it matters: a valid first-block observation follows different projection semantics solely because its coordinate is zero.
- Recommended disposition: carry explicit anchor validity/presence separately from the numeric coordinate, using an existing value contract rather than a sentinel.
- Protected timing concepts affected: device-audio observation coordinate, Timer-absolute observation coordinate, and each domain's validity remain distinct.
- Verification: valid zero/absent/nonzero anchors independently in both device-audio and Timer-absolute domains; delayed first-block replacement/discipline; no ambiguity after reconnect.
- Human decision: accepted ([decision](decisions.md#findings)).

## F-031 — Prevent downsampling conversion from manufacturing an early remote wrap

- Stage / reviewer: S11-03.
- Scope reviewed / exclusions: sample-rate conversion rounding and wrapped-domain bounds.
- Severity: must fix before merge.
- Evidence: interval length and wrapped phase are converted independently with nearest rounding in `JammaLib/src/ninjam/NinjamTiming.h:140`–`:174`. For source phase 999 of length 1000 at 96 kHz converted to 48 kHz, the results are phase 500 and length 500; later modulo/wrap logic reads that as zero/new interval although the source had not wrapped.
- Why it matters: the tracker can manufacture an early wrap and emit join/discipline at the wrong boundary.
- Recommended disposition: convert wrapped phase with an explicit rule that preserves `0 <= phase < convertedLength` and source ordering near the final sample; document the rounding contract.
- Protected timing concepts affected: remote wrapped phase and remote interval length remain related but distinct values.
- Verification: exhaustive small-ratio/source-tail cases; 96→48 and non-integer ratios; first/last source samples; monotonicity/no premature wrap.
- Human decision: accepted ([decision](decisions.md#findings)).

## F-032 — Reject non-finite tempo values at conversion and network egress

- Stage / reviewer: S11-04; trust-boundary refinement S18-01.
- Scope reviewed / exclusions: numeric input hardening only.
- Severity: follow-up.
- Evidence: `IntervalSampsFromTempo` rejects non-positive BPM/zero BPI/rate, then casts `samples + 0.5` to `unsigned int` at `JammaLib/src/ninjam/NinjamTiming.h:126`–`:138`; NaN bypasses its comparisons and reaches the conversion. Current production callers first apply stricter shared validity, so this is hardening rather than a demonstrated live failure.
- Phase 3 evidence: `NinjamConnection::RequestServerTempo` repeats the incomplete `bpm <= 0` predicate at `JammaLib/src/ninjam/NinjamConnection.cpp:742`–`:779`; NaN or positive infinity can be serialized into four malformed admin/vote messages.
- Why it matters: malformed upstream values can reach undefined or implementation-dependent conversion behavior even though ordinary plausibility checks reject common bad values.
- Recommended disposition: require finite, product-plausible BPM/BPI at each Jamma-owned conversion and outgoing NINJAM command boundary before arithmetic or formatting; reject the request and send nothing. Keep upstream NJClient unchanged.
- Protected timing concepts affected: remote BPM/BPI inputs and the derived source-rate interval length remain distinct.
- Verification: table-drive NaN, positive/negative infinity, zero, negative, plausible endpoints, just-outside endpoints, and extreme finite BPM/BPI/rate values; helper returns zero, outgoing request returns false and sends nothing, valid formatting remains stable.
- Human decision: accepted ([decision](decisions.md#findings)).

## F-033 — Keep remote stereo buffer borrows inside the connection lifetime guard

- Stage / reviewer: S12-01.
- Scope reviewed / exclusions: live disconnect/reconnect lifetime; data-race proof while live excluded.
- Severity: merge blocker.
- Evidence: `NinjamConnection::ConsumeStereoPair` returns pointers into connection-owned `_outScratch` vectors at `JammaLib/src/ninjam/NinjamConnection.cpp:793`–`:813`. `NinjamSession::ConsumeStereoPair` releases its `NinjamConnectionUse` on return at `NinjamSession.cpp:508`–`:518`; AudioHost and `StationRemote` consume the pointers afterward at `JammaLib/src/audio/AudioHost.cpp:419`–`:435` and `JammaLib/src/engine/StationRemote.cpp:141`–`:173`. Concurrent `Stop` can then destroy the old connection/storage.
- Why it matters: live disconnect/reconnect creates a callback-side use-after-free window.
- Recommended disposition: keep the guard alive through synchronous ingestion, or copy into caller-owned preallocated storage while guarded. Do not add callback allocation, blocking locks, or callback-side final destruction.
- Protected timing concepts affected: none directly.
- Verification: controlled stop between acquire and consume; repeated live start/stop/reconnect under ASan/page heap/Application Verifier where supported; silence/clean handoff and callback-allocation audit.
- Human decision: accepted ([decision](decisions.md#findings)).

## F-034 — Remove callback-side alignment logging and slim Station diagnostics

- Stage / reviewer: S08-02; refined by S13-01/S17-01 and related to approved F-019.
- Scope reviewed / exclusions: callback reachability, disabled-path cost, code size, supported diagnostic audience/fields/volume; operational state is excluded.
- Severity: must fix before merge.
- Evidence: alignment logging remains reachable from callback-owned Station paths while `Station::_LogLocalLoopAlignment` and related before/after formatting occupy a large Station implementation surface (`JammaLib/src/engine/Station.cpp:809`, `:1107`, `:1197`, `:2301`, `:2327`–`:2470`). The early gate avoids string construction when disabled but still leaves callback entry/branching and large engine-level diagnostic responsibility.
- Why it matters: the human decision requires retained timing logging to be configuration-gated with zero disabled-path performance cost and rejects large Scene/Station `_Log*` implementations. Callback logging also risks unbounded formatting/I/O when enabled.
- Recommended disposition: keep compact coordinator state and the coherent AudioHost applied receipt, correlated by session epoch plus desired/applied state version, and format transition-only/rate-bounded records off-thread in an existing NINJAM/job owner. Remove `Scene::_LogAppliedNinjamLoopAlignment`, the Station hierarchy logger/before-state vector/call sites, and F-019's dead receipt. Optional entity detail has fixed capacity plus overflow count. Disabled callback code has no diagnostic call, branch, traversal, allocation, or I/O; add no logging class.
- Estimated simplification: remove roughly 160–230 lines from Scene/Station before a smaller 40–100-line off-thread presentation path, for about 80–150 net production-line deletion.
- Protected timing concepts affected: diagnostic mirrors only; operational timing authority remains unchanged.
- Verification: static callback call-chain proving no formatter/I/O/hierarchy traversal; disabled instrumentation or disassembly proves zero work; bounded enabled capture tests capacity/overflow and epoch/version correlation; normal/verbose empty/populated join, record/overdub, `Stay local`, disconnect, reconnect, and logging-on/off phase equivalence.
- Human decision: accepted ([decision](decisions.md#findings)).

## F-035 — Consolidate direct LoopTake audio/MIDI cursor shifting

- Stage / reviewer: S13-05.
- Scope reviewed / exclusions: duplicated immediate cursor mutation in accepted timing correction and local transport offset; queued `EndMultiPlay` ordering and caller gates remain separate.
- Severity: follow-up.
- Evidence: unified boundary correction shifts playable audio loops and the MIDI/automation cursor at `JammaLib/src/engine/LoopTake.cpp:537`–`:563`; local transport offset repeats the same immediate mechanics at `:746`–`:773`. The queued path at `:466`–`:513` interleaves ordinary advancement and is not equivalent.
- Why it matters: the invariant that audio body cursors and MIDI event/automation coordinates move by the same signed amount is duplicated and can drift.
- Recommended disposition: add one small private, allocation-free `LoopTake` primitive for the two equivalent direct paths. Preserve generation/accounting, applied-target gates, and the queued path in their callers. Estimated net deletion: 20–30 lines.
- Protected timing concepts affected: per-loop audio phase, MIDI event cursor, and automation origin remain distinct values moved by one common correction.
- Verification: positive/negative/zero delta; unequal audio lengths; MIDI-only/audio-only/empty takes; automation correction sign; local-offset accounting; reconnect/`NoSync` no-call; queued behavior unchanged.
- Human decision: pending Phase 3 gate.

## F-036 — Centralize remote-tempo proposal identity

- Stage / reviewer: S13-07.
- Scope reviewed / exclusions: prompt proposal identity only; prompt wording and tempo validity remain F-013/F-032.
- Severity: follow-up.
- Evidence: Scene manually compares interval, source rate, grid step, BPI, and BPM tolerance at `JammaLib/src/engine/Scene.cpp:336`–`:360`; the coordinator repeats the same identity comparison at `JammaLib/src/ninjam/NinjamTimingCoordinator.cpp:274`–`:281`.
- Why it matters: field additions or renames require synchronized edits and can make prompts close on phase-only refresh or retain stale geometry.
- Recommended disposition: put one value-level identity comparison in the existing NINJAM value/owner and let Scene ask whether the proposal changed. Exclude observation phase/sample from identity; add no class. Estimated deletion: 6–12 lines.
- Protected timing concepts affected: remote geometry identity remains distinct from remote phase/observation and prompt lifecycle.
- Verification: same geometry with new phase/sample keeps the prompt; changes to interval/rate/grid/BPI/BPM replace it; disconnect/reconnect clears it; one comparison remains.
- Human decision: pending Phase 3 gate.

## F-037 — Delete the uncompiled obsolete remote-phase test suite

- Stage / reviewer: S14-01.
- Scope reviewed / exclusions: test reachability and duplication; no production API deletion.
- Severity: follow-up.
- Evidence: `test/JammaLib_Tests/src/timing/RemotePhaseDiscipline_Tests.cpp:11`–`:156` calls removed Quantiser remote-discipline APIs and is absent from `JammaLib_Tests.vcxproj:204`–`:216`. Commit `f36bfe7` removed it from the project after current-owner tests replaced the old contract.
- Why it matters: 156 lines appear to be regression evidence but neither compile nor represent the current owner model.
- Recommended disposition: delete the file; do not re-register it or recreate the removed Quantiser APIs. Retain current `NinjamTiming`, coordinator, Timer, and real `LoopTake` coverage.
- Protected timing concepts affected: none.
- Verification: project-membership and retired-symbol audit; incremental native build; current circular-delta/coordinator/Timer/LoopTake tests remain registered and passing.
- Human decision: pending Phase 3 gate.

## F-038 — Replace the pseudo-integration harness with a production-faithful timing boundary

- Stage / reviewer: S14-02; test-description corroboration S15-05.
- Scope reviewed / exclusions: whether tests observe AudioHost/real LoopTake behavior; production defects remain F-024/F-025.
- Severity: must fix before merge.
- Evidence: `NinjamTimingIntegration_Tests.cpp:12`–`:24`, `:124`–`:126` claims exact AudioHost behavior, but its model always fans out invalidation/resets gates (`:142`–`:157`, `:194`–`:200`), accepts equal generations (`:202`–`:207`), and never uses `ModelTake::Length` to wrap (`:42`–`:48`, `:178`–`:180`). Production skips take invalidation at `AudioHost.cpp:174`–`:187`, `:298`–`:320`, and real `LoopTake` rejects equal generations at `LoopTake.cpp:524`–`:534`.
- Why it matters: the suite passes by simulating the missing behavior and cannot protect the complete-state, reconnect, or common-map refactors.
- Recommended disposition: test a narrow production-owned seam in an existing owner using Timer, AudioHost map, Station, and real LoopTake objects. Move complete-state and two-session reconnect contracts there; retain only distinct coordinator/telemetry simulations and remove duplicated model-phase tests after replacements pass.
- Protected timing concepts affected: epoch, policy, Timer geometry, common map, entity anchors/lengths, audio phase, MIDI cursor, automation origin, and `NoSync` remain independently asserted.
- Verification: P1–P4 from Stage 14: complete desired-state sequences, two real unequal-length audio/MIDI takes with intentional offsets, session-2 generation 1, stale/equal rejection, restore-before-rebase.
- Human decision: pending Phase 3 gate.

## F-039 — Rewrite tests that preserve rejected command and zero-anchor semantics

- Stage / reviewer: S14-03; description defects S15-05.
- Scope reviewed / exclusions: expected test behavior; production implementation remains F-024/F-030.
- Severity: must fix before merge.
- Evidence: `NinjamAudioTimingCommand_Tests.cpp:55`–`:81` encodes benign independent-command coalescing/invalidation, while the human approved complete desired state. `NinjamTiming_Tests.cpp:150`–`:160`, `:188`–`:198` treats numeric zero as absent, contrary to accepted F-030.
- Why it matters: unchanged tests would make approved fixes fail by design and leave the dangerous coalescing/first-block cases untested.
- Recommended disposition: rewrite around complete desired state plus session epoch and a presence-bearing observation value. Keep latest-value/projection contracts but replace their semantics; do not add compatibility shims for rejected behavior.
- Protected timing concepts affected: epoch, policy, geometry, timestamped remote phase, device observation, and Timer-absolute observation remain separate fields.
- Verification: P1/P2/P6: both former command orderings, final applied epoch/geometry/phase, valid zero/explicit absent/nonzero anchors, translated pairs, delayed first-block behavior.
- Human decision: pending Phase 3 gate.

## F-040 — Make the timing-mailbox concurrency test prove overlap

- Stage / reviewer: S14-04.
- Scope reviewed / exclusions: deterministic test construction; mailbox memory model remains F-021/F-023.
- Severity: must fix before merge.
- Evidence: `NinjamTimingObservationMailbox_Tests.cpp:69`–`:119` reads only while `writerFinished` is false; if the writer finishes first, the loop executes zero times and every coherence assertion is skipped. There is no start barrier, read counter, or required final sentinel.
- Why it matters: the sole claimed concurrent complete-value proof can pass vacuously and is not a reliable publication prerequisite.
- Recommended disposition: add deterministic start/overlap coordination, keep the writer active until reads occur, require a nonzero completed-observation count, and use incompatible related sentinels across every field with generation-rich failures.
- Protected timing concepts affected: remote observation and local Timer transport remain separate complete values.
- Verification: run the corrected P5 repeatedly and under race tooling where practical; assert overlap/read counts and no mixed generation.
- Human decision: pending Phase 3 gate.

## F-041 — Add controllable session-loss and buffer-borrow test seams

- Stage / reviewer: S14-05.
- Scope reviewed / exclusions: absence/observability of native prerequisites; correctness remains F-027/F-028/F-033.
- Severity: must fix before merge.
- Evidence: no focused `NinjamSession`/`NinjamConnection` native test is registered in `JammaLib_Tests.vcxproj:204`–`:216`; coordinator disconnect tests call `Disconnect()` directly and deadline tests keep supplying valid observations (`NinjamTimingCoordinator_Tests.cpp:229`–`:240`, `:517`–`:546`). No test forces physical retry/failure or stop between buffer acquire and consume.
- Why it matters: the highest-risk recovery and lifetime changes otherwise lack deterministic passing prerequisites.
- Recommended disposition: add minimal injectable/fake state and clock hooks to an existing session/integration owner plus test-only synchronization around scoped synchronous consumption. Do not expose raw connection/buffer APIs or add callback allocation.
- Protected timing concepts affected: physical availability, epoch, timing validity, follow authority, local free-run, and buffer lifetime remain separate.
- Verification: P7–P9: exactly-once loss invalidation/fresh epoch, no-observation deadline, repeated-invalid idempotence, controlled stop-during-consume; follow with sanitizer/page-heap/Application Verifier and manual reconnect.
- Human decision: pending Phase 3 gate.

## F-042 — Correct the NINJAM integration guide's timing ownership and Stay-local contract

- Stage / reviewer: S15-01; related command-target corrections attach to F-024.
- Scope reviewed / exclusions: statement truthfulness; runtime fixes remain F-021/F-024/F-025/F-027/F-028.
- Severity: must fix before merge.
- Evidence: `doc/ninjam.md:12`–`:17` assigns timing observation to the job path although current callback getters are the F-021 defect; `:31`–`:44` describes nonexistent job-thread MIDI re-anchoring at each wrap; `:58`–`:61` says `Stay local` publishes nothing although current/approved behavior requires explicit `NoSync`. Line `:7` also describes interval duration dimensionally as “BPM × BPI beats” instead of BPI beats at BPM.
- Why it matters: the guide assigns mutation to the wrong thread and hides the authority-loss transition maintainers must preserve.
- Recommended disposition: after each accepted contract is implemented, document the final owner; until then mark current defect versus approved target. Describe retained session anchors, audio-thread cursor/automation correction, explicit desired `NoSync`, and interval duration `60 * BPI / BPM` seconds.
- Protected timing concepts affected: observation ownership, source/scene anchor, MIDI cursor, automation origin, follow policy, and `NoSync` remain distinct.
- Verification: statement/source audit plus callback/job ownership trace and manual `Stay local`/reconnect trace.
- Human decision: pending Phase 3 gate.

## F-043 — Reconcile the MIDI quantisation investigation with the implemented remote-grid path

- Stage / reviewer: S15-03.
- Scope reviewed / exclusions: document status/truth; no claim that all residual visual/manual acceptance cases pass.
- Severity: must fix before merge.
- Evidence: `doc/ninjam-midi-quantisation-investigation.md:90`–`:141`, `:197`–`:201` labels remote geometry/origin, direct-boundary evaluation, and authoritative BPI as absent, but commit `e72f3b0` implements them through `Scene.cpp:443`–`:449`, `Quantiser.cpp:234`–`:247`, `MidiQuantisation.cpp:214`–`:272`, and `NinjamTimingCoordinator.cpp:291`–`:313`. Lines `:274`–`:279`, `:324`–`:336` also preserve rejected latest-command/generation semantics.
- Why it matters: a live TDD plan now directs maintainers to reimplement completed work and retain contracts explicitly rejected at the human gate.
- Recommended disposition: convert it to a dated implementation record with `implemented by e72f3b0 / residual verification` status, or rewrite verified-state/remaining-work from current code. Replace latest-command wording with complete desired state/epoch and retain unverified acceptance cases as such.
- Protected timing concepts affected: active remote grid, local grain, authoritative BPI, phase/origin, desired-state lifecycle, and epoch remain distinct.
- Verification: statement-by-statement source/test audit; link executable residual contracts and do not mark overlay/manual cases green without evidence.
- Human decision: pending Phase 3 gate.
