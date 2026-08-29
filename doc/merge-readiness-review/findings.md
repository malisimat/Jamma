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
- Human decision: pending.

## F-002 — Decide whether VST3 parity/state/mapping belongs in this merge

- Stage / reviewer: S01-02; corroborated by S05 history.
- Scope reviewed / exclusions: branch scope and interface expansion; no VST correctness/lifetime judgment.
- Severity: must fix before merge.
- Evidence: merge `880112d438e21983688395f60f970427d7f1d0b9` brought the independent VST3 parity lineage into the timing branch (25 files, +2,079/−347 versus first parent). Current surface includes `JammaLib/src/vst/Vst3Plugin.h:29`, `JammaLib/src/vst/IVstPlugin.h:24`, `:73`, `:84`–`:97`, and `:132`–`:137`.
- Why it matters: plugin hosting, persistence, editor, MIDI mapping, GL/resource, and project/test contracts substantially expand the merge surface.
- Recommended disposition: retain with an explicit combined-scope rationale, or move the VST3 lineage to a separately reviewed branch.
- Protected timing concepts affected: monotonic scene coordinate and plugin-facing musical transport remain distinct consumers if retained.
- Verification: human scope decision; if retained, later VST interface/lifetime/persistence/MIDI/manual coverage; if moved, verify intended musical-transport consumers remain.
- Human decision: pending.

## F-003 — Decide whether window-placement persistence belongs in this merge

- Stage / reviewer: split from S01-03.
- Scope reviewed / exclusions: objective commit/file scope only; persistence correctness belongs to later phases.
- Severity: follow-up.
- Evidence: commits `efe496292e2046f856a684a98fd956aac3e56a05`, `8f50e136d18d7632f8b852ad4170bd73ce7b5acd`, and `704828f6abaf136a1327bfb3b750f712088519e2` add and repair window/default persistence retained at `Jamma/src/Main.cpp:349`, `:359`–`:368`, and `:451`–`:461`.
- Why it matters: this is a separable persistence contract not implied by remote-join alignment.
- Recommended disposition: retain with release-scope rationale, or move to a separately reviewed change.
- Protected timing concepts affected: none.
- Verification: human scope decision; if retained, Stage 16 persistence matrix and focused window restore behavior.
- Human decision: pending.

## F-004 — Decide whether repository build/agent tooling belongs in this merge

- Stage / reviewer: split from S01-03.
- Scope reviewed / exclusions: objective commit/file scope only; no build-wrapper quality judgment.
- Severity: follow-up.
- Evidence: commits `5daefa3161f972203216b91f28bc3d55d748392a`, `b43abbebfdf16147808fbe6a3bcc89934fd07d3a`, `4a74ca978da2068f1fef97648cd52b12740e7cdf`, and `f37e9c0f3cc9ad097fec5b4ef24f0740b05571ef` add/update the worktree skill and MSBuild environment wrapper, now normative at `AGENTS.md:29`–`:31`.
- Why it matters: repository policy/tooling is independently useful but separable from product timing behavior and requires its own review rationale.
- Recommended disposition: retain as an explicit build prerequisite, or move to a separately reviewed tooling change.
- Protected timing concepts affected: none.
- Verification: human scope decision; if retained, Stage 21 command validation using local `.vscode/tasks.json` and the wrapper.
- Human decision: pending.

## F-005 — Translate NINJAM follow policy before entering the core loop hierarchy

- Stage / reviewer: S02-01.
- Scope reviewed / exclusions: dependency direction; no behavioral-correctness claim.
- Severity: follow-up.
- Evidence: `JammaLib/src/engine/LoopTake.h:21` imports the NINJAM command header; `LoopTake::ApplyTimingCommand` and Station fan-out accept `ninjam::NinjamLocalFollowPolicy` at `JammaLib/src/engine/LoopTake.h:235`–`:239` and `JammaLib/src/engine/Station.h:113`–`:128`. `AudioHost` already interprets disable/invalidation at `JammaLib/src/audio/AudioHost.cpp:174`–`:186` and `:298`–`:305`.
- Why it matters: low-level local loop state depends on session policy and duplicates the `NoSync` decision.
- Recommended disposition: move policy interpretation to NINJAM/AudioHost and expose neutral accepted correction/invalidation operations to engine entities.
- Protected timing concepts affected: follow policy, `NoSync` invalidation, per-loop phase; all remain separate.
- Verification: include-graph check plus continuous/block/no-sync, reconnect, and invalidation tests.
- Human decision: pending.

## F-006 — Consolidate the common sync-map owner without deleting live entity state prematurely

- Stage / reviewer: S02-02; reconciled with S06 current-reachability evidence.
- Scope reviewed / exclusions: future structural ownership, not a dead-code finding or correctness proof.
- Severity: follow-up.
- Evidence: authoritative-looking map state is held by `AudioHost` at `JammaLib/src/audio/AudioHost.h:141`–`:144`, then copied through begin/rebase APIs at `JammaLib/src/audio/AudioHost.cpp:352`–`:364`; each `LoopTake` stores and recomputes parallel map geometry at `JammaLib/src/engine/LoopTake.h:406`–`:410` and `JammaLib/src/engine/LoopTake.cpp:602`–`:679`.
- Why it matters: N+1 mutable representations enlarge a repeatedly changed consistency surface.
- Recommended disposition: simplify later so AudioHost owns the common ruler and supplies one mapped source coordinate; retain per-entity audio/MIDI anchors and modulo restore. Current per-take fields are live until that replacement is implemented.
- Protected timing concepts affected: sync map, mapped elapsed time, source/scene anchors, per-loop phase, monotonic scene coordinate; none may be collapsed.
- Verification: unequal loop lengths, intentional offsets, rebase/wrap, reconnect, and `NoSync` invalidation.
- Human decision: pending.

## F-007 — Separate value-only local timing contracts from the Quantiser/UI aggregate

- Stage / reviewer: S02-03.
- Scope reviewed / exclusions: module placement and dependency direction; no naming/numerical judgment.
- Severity: follow-up.
- Evidence: timing value types share `JammaLib/src/engine/Quantiser.h:43`–`:137` with action/graphics/GUI dependencies at `:3`–`:18` and stateful controller declarations at `:170` and `:353`; `JammaLib/src/ninjam/NinjamTimingCoordinator.h:6`–`:9` includes the aggregate to exchange `engine::QuantisationTiming`.
- Why it matters: session timing consumers inherit unrelated presentation/control coupling.
- Recommended disposition: move immutable local geometry/timing values into a small engine timing-contract header; keep Quantiser/controller in the engine interaction layer.
- Protected timing concepts affected: local timing, local grain, active grid, remote geometry remain separate types/fields.
- Verification: include-graph check and coordinator/local-timing tests.
- Human decision: pending.

## F-008 — Move the local transport-offset mailbox out of the NINJAM command module

- Stage / reviewer: S02-04.
- Scope reviewed / exclusions: class placement only; synchronization and hot-path behavior belong to Phase 2.
- Severity: follow-up.
- Evidence: `LocalTransportOffsetLoopFracMailbox` is declared in namespace/file `ninjam` at `JammaLib/src/ninjam/NinjamAudioTimingCommand.h:147`–`:193`, while its production flow is local UI/Scene -> `AudioHost` -> local stations at `JammaLib/src/audio/AudioHost.h:126` and `JammaLib/src/audio/AudioHost.cpp:332`–`:350`.
- Why it matters: a disconnected/local-only control is semantically owned by a remote-session module.
- Recommended disposition: move to AudioHost-private local control or a proven generic latest-value utility; preserve latest-wins/explicit-zero behavior.
- Protected timing concepts affected: local timing and follow policy remain independent.
- Verification: mailbox tests plus disconnected and `NoSync` local-offset scenarios.
- Human decision: pending.

## F-009 — Complete update-to-command materialization in the NINJAM integration layer

- Stage / reviewer: S02-05.
- Scope reviewed / exclusions: orchestration placement; UI presentation and runtime correctness excluded.
- Severity: follow-up.
- Evidence: the coordinator decides policy/generation/update semantics, but `engine::Scene` constructs every command field and invalidation at `JammaLib/src/engine/Scene.cpp:254`–`:305` and `:403`–`:482`; AudioHost then applies it.
- Why it matters: command meaning and construction are split across coordinator and Scene, so field changes require synchronized mechanical edits and thicken glue code.
- Recommended disposition: have the NINJAM integration layer emit the complete immutable audio command; keep Scene for prompts/forwarding and AudioHost for block-boundary application.
- Protected timing concepts affected: remote join, follow policy, command lifecycle, `NoSync` invalidation remain distinct.
- Verification: coordinator command-contract tests and integration forwarding tests.
- Human decision: pending.

## F-010 — Normalize new timing aggregate acronym casing

- Stage / reviewer: S03-01.
- Scope reviewed / exclusions: naming form only, not BPM/BPI semantics.
- Severity: follow-up.
- Evidence: new `LocalAudioGeometry::BPI` and `RemoteTransportGeometry::BPI`/`BPM` at `JammaLib/src/engine/Quantiser.h:52`, `:97`, and `:99` conflict with neighboring `Bpm`/`Bpi` at `:135`–`:136` and `JammaLib/src/ninjam/NinjamTiming.h:16`–`:17`.
- Why it matters: adjacent public timing aggregates expose inconsistent identifier forms.
- Recommended disposition: mechanically rename to `Bpi`/`Bpm` without semantic changes.
- Protected timing concepts affected: none.
- Verification: compile affected targets and identifier audit.
- Human decision: pending.

## F-011 — Apply the immediate NINJAM aggregate member convention consistently

- Stage / reviewer: S03-02; reconciled with S04-05.
- Scope reviewed / exclusions: immediate NINJAM aggregate neighborhood only; not a repository-wide public-field rule.
- Severity: follow-up.
- Evidence: lower-camel public fields in `JammaLib/src/ninjam/ExportLaneTiming.h:19`, `:20`, `:32`, `:48`, `:54` and `NinjamMetronomeTiming.h:16`, `:18`, `:27`, `:28`, `:44`, `:46` contrast with neighboring PascalCase NINJAM aggregates at `JammaLib/src/ninjam/NinjamTiming.h:27`–`:32`.
- Why it matters: adjacent public NINJAM value APIs switch naming form without a domain reason.
- Recommended disposition: rename these public fields to PascalCase. If F-017 is also approved, implement overlapping export fields once using its semantic PascalCase names.
- Protected timing concepts affected: none.
- Verification: focused helper/metronome tests and retired-name search.
- Human decision: pending.

## F-012 — Move substantial non-template NINJAM runtime bodies out of public headers

- Stage / reviewer: S03-03.
- Scope reviewed / exclusions: header/implementation placement only; no algorithm, performance, or ownership claim.
- Severity: follow-up.
- Evidence: substantial mailbox/timing bodies remain at `JammaLib/src/ninjam/NinjamAudioTimingCommand.h:61`, `:83`, `:154`, `:162`; `NinjamTiming.h:45`–`:140`; and `NinjamTimingObservationMailbox.h:16`, `:37`, unlike paired `.cpp` neighbors such as `ExportLaneTiming`, `NinjamMetronomeTiming`, and `NinjamTimingTracker`.
- Why it matters: mutable mailbox mechanics and runtime calculations expand recompilation and implementation coupling for every includer.
- Recommended disposition: retain tiny/compile-time helpers inline; move non-template runtime bodies behind declarations, coordinated with any accepted owner move from F-008.
- Protected timing concepts affected: none.
- Verification: incremental library build and relevant NINJAM timing/mailbox tests.
- Human decision: pending.

## F-013 — Reserve “grain” for local audio construction, not remote grid cells

- Stage / reviewer: S04-01.
- Scope reviewed / exclusions: vocabulary through coordinator, prompt, and command; active-grid runtime correctness excluded.
- Severity: must fix before merge.
- Evidence: `NinjamTempoChange::GrainSamps` at `JammaLib/src/ninjam/NinjamTimingCoordinator.h:56` is calculated as remote interval / authoritative remote BPI at `JammaLib/src/ninjam/NinjamTimingCoordinator.cpp:306`–`:313`, copied to quantisation at `:316`–`:327`, and displayed as `Grain` at `JammaLib/src/engine/Scene.cpp:378`–`:382`.
- Why it matters: it directly contradicts the protected glossary: local grain is an audio construction unit, not a remote beat/grid cell.
- Recommended disposition: rename the remote-derived value and UI text to `RemoteGridStepSamps` / “Remote grid step”; reserve `GrainSamps` for local construction geometry.
- Protected timing concepts affected: local grain, active/remote grid, remote timing, local timing.
- Verification: symbol/UI audit proving remote-BPI consumers use remote-grid vocabulary and remaining grain names are local construction.
- Human decision: pending.

## F-014 — Make accepted remote command authority and coordinates explicit

- Stage / reviewer: S04-02.
- Scope reviewed / exclusions: naming at coordinator/Scene/AudioHost boundary; no mathematical-correctness claim.
- Severity: must fix before merge.
- Evidence: generic `NinjamClockSettings` fields at `JammaLib/src/ninjam/NinjamTimingCoordinator.h:69`–`:84` become `AbsolutePhaseSamps` and `PhaseObservationSample` at `JammaLib/src/engine/Scene.cpp:432`–`:449` and `JammaLib/src/ninjam/NinjamAudioTimingCommand.h:35`–`:50`; `AudioHost` then overwrites `stationDelta` from remote correction to mapped source correction at `JammaLib/src/audio/AudioHost.cpp:169` and `:241`–`:247`.
- Why it matters: “absolute” names a wrapped remote phase, while one variable spans remote-master and local-source coordinates at the most sensitive boundary.
- Recommended disposition: apply the report's rename group: accepted remote replacement, remote master length/phase, device observation sample, remote master correction, and separate local source correction.
- Protected timing concepts affected: remote timing/authority, follow policy, master phase, sync map, mapped source correction, per-loop phase remain separate.
- Verification: command/integration tests and a manual trace of replacement plus discipline coordinates.
- Human decision: pending.

## F-015 — Name Timer-absolute and device-audio observation anchors distinctly

- Stage / reviewer: S04-03.
- Scope reviewed / exclusions: timestamp vocabulary; arithmetic/seqlock correctness excluded.
- Severity: must fix before merge.
- Evidence: adjacent `LocalBlockStartSample`/`AudioBlockStartSample` at `JammaLib/src/ninjam/NinjamTiming.h:21`–`:37` are populated respectively from `Timer::AbsoluteSamplePos(...)` and the raw device counter at `JammaLib/src/audio/AudioHost.cpp:444`–`:452`; the first becomes generic `LocalSample` at `JammaLib/src/ninjam/NinjamTimingCoordinator.cpp:99`–`:105`. MIDI's same device coordinate is merely `Sample` at `JammaLib/src/midi/MidiClockAnchor.h:8`–`:28`.
- Why it matters: Timer absolute geometry can change under accepted timing while the device audio counter remains monotonic; current names invite substitution and confusion with scene time.
- Recommended disposition: use explicit `LocalMasterAbsoluteSampleAtObservation` and `DeviceAudioSampleAtObservation`/`DeviceAudioSamplePosition` names.
- Protected timing concepts affected: local master absolute position, device counter, monotonic scene coordinate, remote timing remain distinct.
- Verification: tests with distinct sentinel values and unqualified-name audit.
- Human decision: pending.

## F-016 — Rename the MIDI automation global-sample origin

- Stage / reviewer: S04-04.
- Scope reviewed / exclusions: automation-anchor vocabulary; playback correctness/lifetime excluded.
- Severity: must fix before merge.
- Evidence: `MidiLoop::LoopPhaseAnchor` at `JammaLib/src/midi/MidiLoop.h:175`–`:183` is documented as global sample mapping to loop-relative zero; `EndRecord` stores `startGlobalSample` at `JammaLib/src/midi/MidiLoop.cpp:253`–`:259`, and Station copies it for automation dispatch at `JammaLib/src/engine/Station.h:334`–`:344`.
- Why it matters: the name competes with actual per-loop event phase and source/scene anchors even though it is an automation recording origin.
- Recommended disposition: rename accessor/member/dispatch copy to `AutomationGlobalSampleOrigin` with repository member casing.
- Protected timing concepts affected: automation origin, MIDI event cursor/per-loop phase, source/scene anchor remain separate.
- Verification: automation-use search and focused automation/timing tests.
- Human decision: pending.

## F-017 — Give export-lane remote/local cursors semantic unit-bearing names

- Stage / reviewer: S04-05; coordinated with F-011.
- Scope reviewed / exclusions: value vocabulary only; latency formula correctness and dormant-path retention excluded.
- Severity: follow-up.
- Evidence: `ExportLaneTimingInput::n`, `pos`, and `length` at `JammaLib/src/ninjam/ExportLaneTiming.h:12`–`:25` represent a delay-line cursor, remote interval phase, and remote interval length and are mixed at `JammaLib/src/ninjam/ExportLaneTiming.cpp:20`–`:66`.
- Why it matters: algebraic names obscure coordinate/lifetime distinctions in a formula combining remote interval and local device timing.
- Recommended disposition: rename to semantic unit-bearing PascalCase names such as `DelayWriteCursorSamps`, `RemoteIntervalPhaseSamps`, and `RemoteIntervalLengthSamps`, plus corresponding state names, if the helper is retained.
- Protected timing concepts affected: remote wrapped phase/length and local delay-line/device timing remain distinct.
- Verification: pure-helper tests and call-site initialization review.
- Human decision: pending.

## F-018 — Remove duplicated stored `SyncPhaseMap::SourcePhaseAtOrigin`

- Stage / reviewer: S05-01 merged with S06-01.
- Scope reviewed / exclusions: historical residue plus current references; no timing redesign.
- Severity: follow-up.
- Evidence: history `1d665d2` -> `0d90bac` -> `bef7943` -> `e72f3b0` replaced phase-origin consumers with a monotonic source coordinate. The member remains at `JammaLib/src/ninjam/NinjamLoopAlignment.h:54` and is assigned at `:84`–`:90`, but has no current read; derived `SourcePhaseAt(...)` uses `SourceCoordinateAt(...)` at `:65`–`:81`.
- Why it matters: duplicated stored representation suggests two authorities and risks reviving the superseded phase-origin model.
- Recommended disposition: remove only the stored member/comment/assignment; retain derived phase, source coordinate, scene coordinate, and per-entity anchors.
- Protected timing concepts affected: sync map, mapped elapsed, source/scene anchor, master phase, per-loop phase all remain distinct.
- Verification: incremental build; sync-map, unequal-length/offset, reconnect/rebase, and `NoSync` tests.
- Human decision: pending.

## F-019 — Remove the never-published `AlignmentReceipt` path

- Stage / reviewer: S05-02 split and confirmed by S06-02.
- Scope reviewed / exclusions: dead receipt subset only; live/dormant snapshots and coordinator telemetry are explicitly retained for Phase 2/3 review.
- Severity: follow-up.
- Evidence: type/API at `JammaLib/src/engine/LoopTake.h:93`–`:106` and `:252`; eleven zero-initialized atomics at `:420`–`:430`; only loads at `JammaLib/src/engine/LoopTake.cpp:691`–`:714`, with immediate `nullopt` at `:695`–`:697`; no writer exists, making the logger branch at `JammaLib/src/engine/Station.cpp:2378`–`:2394` unreachable.
- Why it matters: every take carries unused atomics/API/logger complexity that obscures reachable diagnostics.
- Recommended disposition: remove the receipt type, API, atomics, and unreachable logger branch unless a human defines a new supported publication contract. Retain `_ninjamBeforePositions`, verbose before/after snapshots, and coordinator diagnostics pending Stages 8/17.
- Protected timing concepts affected: none operational; diagnostic mirrors only.
- Verification: incremental build; normal/verbose logging; focused remote-join/local-loop regression.
- Human decision: pending.

## F-020 — Remove ten unregistered coloured trigger-back textures if HUD is retained

- Stage / reviewer: S06-03; dependent on F-001.
- Scope reviewed / exclusions: resource reachability/project copy; no visual-design judgment.
- Severity: follow-up.
- Evidence: ten green/red `trigger_back*` variants (164,020 bytes total) have no source or `ResourceList.txt` reference. Only neutral `trigger_back` is registered at `Jamma/resources/ResourceList.txt:76` and used at `JammaLib/src/gui/GuiHud.cpp:675`–`:679`; `Jamma/Jamma.vcxproj:200`–`:202` nevertheless copies the full resource tree.
- Why it matters: unreachable binary payload is shipped and expands provenance/review surface.
- Recommended disposition: if F-001 retains HUD, remove the ten variants; if HUD is moved, resolve them with that lineage and do not create a duplicate cleanup.
- Protected timing concepts affected: none.
- Verification: app build/output audit and manual trigger default/hover/down/out rendering.
- Human decision: pending.
