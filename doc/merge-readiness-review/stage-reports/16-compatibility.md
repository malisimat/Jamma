# Stage 16 — Compatibility and persistence

## Assignment

- Stage: Phase 3, Stage 16 — Compatibility and persistence.
- Primary ownership: well-formed compatibility, version, default, migration, and fallback contracts that affect remote timing/NINJAM session authority. The reviewed surface includes `.jam` NINJAM configuration, the new persisted local transport offset, session-start defaults, and the runtime timing artifacts passed between NINJAM producers and timing consumers.
- Governing direction: preserve the protected timing glossary and the human-approved latest-complete-desired-state model. Session epoch, follow policy, complete validated remote geometry, and timestamped remote master phase remain distinct; mapped elapsed time is not a shared loop cursor, and a persisted local offset must not erase entity-specific phase for unequal loop lengths.
- Explicit exclusions: hostile or malformed-input handling (Stage 18); retained HUD/resources, VST3 state/parity/mapping, window placement, build/tooling, unrelated MIDI/resource contracts, and upstream NJClient edits. Existing F-021–F-034 runtime/concurrency defects are cited where they constrain compatibility but are not re-reported.
- Inputs read: `AGENTS.md`; `merge-readiness-plan.md`; `phase-3-maintainability-and-contracts.md`; `decisions.md`; `00-scope-and-inventory.md`; `phase-packets/phase-1.md`; `phase-packets/phase-2.md`; `findings.md`; `verification-matrix.md`; `cleanup-backlog.md`; `stage-reports/13-simplification.md`; `doc/loop-alignment-and-ninjam-sync.md`; and `doc/ninjam.md`.
- Comparison: production diff `master...e72f3b0` plus current surrounding code at review-artifact `HEAD` `1f4d200`; relevant history includes `be373b3`, `6dc0c73`, `4c1c0e1`, `f36bfe7`, `6abf7c7`, `d6fdb88`, and `e72f3b0`. No review-artifact-only commit is treated as production behavior.
- Output boundary: only this report is written. No production source, tests, canonical findings, verification rows, cleanup backlog, project files, or other stage report is changed; no cleanup is implemented and no commit is created by this investigator.

## Coverage

### Files and contracts actually covered

- `.jam` schema and round trip: `JammaLib/src/io/JamFile.{h,cpp}`, `IoSessionExporter.{h,cpp}`, `test/JammaLib_Tests/src/io/JamFile_Tests.cpp`, and the `Scene::FromFile` consumers.
- Persisted NINJAM connection identity/default start: `JamFile::DefaultJson`, `NinjamController::{LoadConfig,Connect}`, `NinjamSession::Start`, `NinjamNetworkService` timing lifecycle, and Scene's persisted versus interactive connect paths.
- Persisted local timing state where it meets remote following: `TransportOffsetLoopFrac`, `NormalizeLoopFraction`, Scene publication, AudioHost's master-length conversion, and LoopTake's entity-specific modulo application.
- Runtime timing artifacts: `NinjamRemoteTiming`, `NinjamTiming`, the observation mailbox, coordinator proposal/default/fallback behavior, tracker connection state, and the current command mailbox as constrained by accepted F-024/F-025/F-027 and S13-02.
- Rig/UserConfig/JSON were checked for timing-authority schema changes. The seed-timing settings and deduction policy predate this branch's remote-authority model; current branch changes only move the deduction implementation from `TimingQuantiser` to `Quantiser`. No new timing compatibility finding is assigned there. Project/resource metadata does not change timing compatibility.

### Commands and queries used

- `git status --short`; `git rev-parse HEAD master`; `git merge-base master HEAD`; `git diff --name-status master...HEAD`; `git diff --unified=<n> master...HEAD -- <io/ninjam/timing paths>`.
- `git log --oneline -- <paths>`; `git log -S<symbol/text>`; `git show <commit>:<path>`; `git show --unified=<n> <commit> -- <paths>`; and `git blame -L <range> <path>` for the lifecycle and offset-migration chains.
- Repository `rg -n` searches for `.jam`, `NinjamConfig`, `LoadConfig`, `PrepareTempoSyncOnConnect`, `ResetTempoSyncOnDisconnect`, `Version`, defaults/fallbacks, BPM/BPI, transport offsets, timing validity, producer/consumer call sites, and tests.
- Numbered `Get-Content` reads of all cited ranges and surrounding producer/consumer code.

No build, native test, network session, or manual save/reload was run. This stage is a read-only contract investigation; existing Phase 2 build/test evidence remains the baseline.

### Deliberate exclusions and negative decisions

- VST state format/version, window persistence, MIDI mappings, resources, and project metadata were not re-reviewed because the human decision retains them outside timing-only cleanup.
- Malformed JSON, non-finite/out-of-range fields, path traversal, and resource exhaustion remain Stage 18. This report discusses missing fields, older well-formed producer shapes, and downgrade behavior only.
- The `.jam` `bpm`/`bpi` fields are not treated as persisted remote authority. They are optional connection metadata parsed and re-emitted at `JamFile.cpp:368`–`:377` and `:489`–`:504`; `NinjamSession::Start` consumes only host/user/pass/workdir at `NinjamSession.cpp:383`–`:417`. Accepted remote geometry/phase/map/epoch are deliberately runtime-only.
- Existing latest-command loss, generation reset, physical loss, observation timestamp, and conversion defects remain F-024–F-031. The compatibility findings below concern distinct producer/start/schema paths.

## System understanding

The repository has two different notions of durable state. A `.jam` stores local session construction and NINJAM connection identity. It does not store accepted remote authority: remote geometry, phase observations, follow policy, map anchors, command generation, and the future session epoch are live-session values and must start fresh. That non-persistence is correct for the approved model because reconnect must never revive an old remote ruler or entity anchor.

The current `.jam` format is an additive, effectively unversioned JSON object. `JamFile::FromStream` assigns `VERSION_V` to every successfully parsed object at `JamFile.cpp:85`, while `ToStream` writes no version key at `:400`–`:465`; the enum at `JamFile.h:34`–`:38` therefore does not negotiate a wire format. Missing known fields generally receive in-memory defaults. Unknown fields are ignored and are not preserved on re-save.

NINJAM connection identity has two consumers. The persisted path calls `NinjamController::LoadConfig`, which starts `NinjamSession` directly. The interactive path first initializes the timing coordinator and invalidates old authority, then starts the same controller/session. With the branch's timing model those paths are not equivalent: only the interactive path creates the timing tracker's connected state and request/follow lifecycle.

### Compatibility matrix

| Artifact/version or producer | Producer and shape | Current consumer | Default / fallback | Compatibility result and explicit break |
| --- | --- | --- | --- | --- |
| Base/master `.jam` | Any pre-`6dc0c73` writer; no `transportoffsetloopfrac` | `JamFile::FromStream` -> `Scene::FromFile` | `TransportOffsetLoopFrac = 0.0` at `JamFile.cpp:90`; Scene publishes zero | Backward read is compatible. No local transport offset is invented. |
| Intermediate signed-offset `.jam` | `6dc0c73`; `transportoffsetloopfrac` accepted/written in `[-1,1]` | Current parser at `JamFile.cpp:204`–`:224` | Negative values are wrapped by `NormalizeLoopFraction`; test expects `-0.25 -> 0.75` at `JamFile_Tests.cpp:276`–`:282` | **Unresolved semantic migration (S16-02).** Equivalent on an `M`-length loop but not on a `2M`/other entity; this cannot be called compatible without an explicit break decision. |
| Current `.jam` | `4c1c0e1+`; writer emits normalized `transportoffsetloopfrac` at `JamFile.cpp:406` | Current parser/Scene/AudioHost | Missing -> `0.0`; finite value becomes current absolute target in master-length samples | Current round trip is structurally supported, though there is no direct writer round-trip test for this field. |
| Current `.jam` opened by master/older binary | Current additive timing key; no wire-version marker | Master parser ignores unknown key; master writer has no corresponding key | Runtime behaves as offset zero; re-save drops the field | **Intentional downgrade break requiring explicit acceptance (S16-03).** The file remains parseable, but timing state is silently unavailable/lost. |
| `.jam` with populated `ninjam` identity, including `DefaultJson` | `host`, `user`, `pass`, `workdir`; optional inert `bpm`/`bpi` | `Scene::FromFile:664` -> `NinjamController::LoadConfig:7`–`:19` -> `NinjamSession::Start:383`–`:417` | Valid host/user auto-starts connection | Network compatibility is retained, but timing lifecycle is bypassed (**S16-01**): no coordinator connect/epoch/request/follow initialization. |
| `.jam` with missing or semantically empty `ninjam` | Older/local-only file, or empty NINJAM object | `NinjamConfig::FromJson` and `LoadConfig` | Missing/all-empty identity -> `nullopt`; session is stopped; local timing free-runs | Compatible and consistent with `NoSync`. No remote authority is restored. |
| Current full remote observation | Both live and snapshot producers populate interval/rate/BPM/BPI and apply `IsValidRemoteTiming` at `NinjamConnection.cpp:709`–`:733` and `:922`–`:946` | `ToDeviceTiming` -> mailbox -> coordinator | Invalid/incomplete producer value produces no accepted timing state | Full-geometry contract matches the protected glossary and approved desired state, subject to existing F-021/F-023/F-026/F-030/F-031 fixes. |
| Claimed older/partial remote observation with BPI absent (`Bpi == 0`) | No current production producer marks it valid | Coordinator contains deduction fallback at `NinjamTimingCoordinator.cpp:296`–`:304` | In production, validity rejects BPI 0 first at `NinjamTiming.h:45`–`:57`, and `Observe` returns at `NinjamTimingCoordinator.cpp:76`–`:77` | **Compatibility fallback is not reachable from current producers (S16-04).** Recommended intentional break: require full BPI and remove the pseudo-fallback, consistent with complete remote geometry. |
| Current event/delta timing command | Scene/coordinator -> latest-value mailbox | AudioHost | No publication leaves local state unchanged; default policy is `NoSync` | Superseded by accepted F-024/S13-02. It must migrate to one latest complete desired state; no compatibility shim may preserve standalone invalidation/delta events. |
| Approved complete desired remote state | Future existing-owner producer: epoch + policy + full remote geometry + timestamped phase | AudioHost at block boundary | New epoch clears old map/anchors/gates; `NoSync` clears authority without moving cursors | Intentional internal contract break from current command variants, already human-directed. Runtime-only; it must never be persisted in `.jam`. |

The only intentional product-format break proposed for acceptance is downgrade loss of the new local transport-offset feature in older binaries. The current command/update representation is also intentionally broken internally by the already-approved complete-state replacement. No `.jam` migration should persist remote phase, map, anchor, generation, or epoch merely to make that internal change look wire-compatible.

## Candidate findings

### S16-01 — Route persisted NINJAM auto-connect through the timing lifecycle

- Stage / reviewer: Stage 16 — Compatibility and persistence.
- Scope reviewed / exclusions: well-formed `.jam` NINJAM identity and default auto-connect versus interactive connect; no hostile configuration handling or physical-loss re-investigation.
- Severity: must fix before merge.
- Evidence: the shipped default `.jam` contains non-empty NINJAM host/user identity at `JammaLib/src/io/JamFile.cpp:21`, and its presence is asserted at `test/JammaLib_Tests/src/io/JamFile_Tests.cpp:528`–`:534`. `Scene::FromFile` restores it through `GetController()->LoadConfig` at `JammaLib/src/engine/Scene.cpp:664`; `NinjamController::LoadConfig` starts the session directly at `JammaLib/src/ninjam/NinjamController.cpp:7`–`:19`. By contrast, interactive `Scene::ConnectNinjam` calls `SetTempoJoinOptions`, `PrepareTempoSyncOnConnect`, closes stale prompts, publishes invalidation, and only then connects at `Scene.cpp:249`–`:288`. `PrepareTempoSyncOnConnect` is the sole production call to `_timingCoordinator.Connect` at `NinjamNetworkService.cpp:36`–`:40`. Without it, `NinjamTimingTracker` remains `_connected == false` and rejects observations at `NinjamTimingTracker.cpp:28`–`:34`. The asymmetry combines the older persisted start path from `be373b3`/`27032de` with timing lifecycle added by `f36bfe7` and manual initialization completed by `6abf7c7`.
- Why it matters: a normal/default startup can connect and receive NINJAM audio while its timing coordinator never establishes an epoch, never requests/pushes local tempo, never offers the follow decision, and never emits accepted authority. Local playback silently remains free-running even though the persisted artifact requested the same session that the interactive path would follow. This is a compatibility regression in the primary `.jam` producer/consumer path, not malformed input.
- Recommended disposition: make the existing NINJAM integration owner expose one session-start operation that initializes fresh timing lifecycle/epoch, applies the configured/default join options, clears stale prompt/authority state, and starts the connection for both persisted and interactive callers. Do not move this orchestration into lower engine entities and do not persist live epoch/authority. Coordinate with the approved complete desired state so startup begins at `NoSync` and accepts only a fresh full observation.
- Protected timing concepts affected: remote join, follow policy, remote timing, session epoch, `NoSync`, sync-map/anchor invalidation; all remain separate.
- Verification: a production-faithful persisted-config start test proving coordinator connected state and fresh epoch before the first observation; default `.jam` start with and without local timing/content; same behavior as interactive connect for push/prompt defaults; missing/empty config remains local; disconnect/reload creates exactly one invalidation and no old command/map/anchor survives. Manual scenario: launch from default `.jam`, verify the remote tempo request/prompt/follow path and local-loop alignment without a manual reconnect.
- Canonical handoff: likely enrich F-027's availability/epoch owner and F-009/S13-02's single integration owner rather than create a competing lifecycle abstraction.
- Human decision: pending Phase 3 gate.

### S16-02 — Preserve or explicitly break signed transport-offset migration for unequal loop lengths

- Stage / reviewer: Stage 16 — Compatibility and persistence.
- Scope reviewed / exclusions: well-formed branch-intermediate `.jam` files produced after `6dc0c73`; no malformed/non-finite parsing judgment.
- Severity: must fix before merge unless the human explicitly declares intermediate branch artifacts unsupported.
- Evidence: `6dc0c73` introduced a persisted signed `transportoffsetloopfrac` and UI range `[-1,1]`; its Station consumer converted that signed fraction directly to `fraction * masterLength`. `4c1c0e1` changed the reader and writer to `utils::NormalizeLoopFraction` (`JamFile.cpp:204`–`:224`, `:406`; `MathUtils.h:17`–`:25`). The only migration test asserts `-0.25 -> 0.75` at `JamFile_Tests.cpp:276`–`:282`. Current AudioHost converts the normalized value to an absolute target `fraction * masterLength` at `AudioHost.cpp:332`–`:349`, and LoopTake shifts each entity by that target/delta before wrapping in its own length at `LoopTake.cpp:739`–`:773`. For `M = 1000`, entity length `L = 2000`, and phase `q = 100`, the signed artifact restores `q' = (100 - 250) mod 2000 = 1850`; the current migration restores `q' = (100 + 750) mod 2000 = 850`. They differ by one master interval and are not equivalent modulo the entity length.
- Why it matters: the migration assumes positions equivalent modulo master length are equivalent for every local entity. That is exactly the protected distinction the sync model rejects: master phase is not per-loop phase, and loops may intentionally be longer than the master. A passing scalar parser test therefore masks a real persisted behavior change.
- Recommended disposition: first decide whether files produced by the signed branch format are a supported artifact version. If supported, retain enough signed/turn information to apply the same local source correction to every entity before adopting the current canonical representation; do not recover it by assigning every loop a master-wrapped cursor. If unsupported, record the unequal-length phase change as an explicit intentional break and rename/remove the misleading `Legacy` compatibility claim in the test during an approved cleanup batch.
- Protected timing concepts affected: local timing, master phase, per-loop phase, mapped/common correction, intentional offsets, unequal loop lengths.
- Verification: load signed `-0.25`, `-1.0`, positive, zero, and endpoint values with audio and MIDI entities of `M`, `2M`, and non-divisor lengths; compare pre/post migration cursor deltas rather than only the parsed scalar; round trip through the current writer; join/`NoSync` must not reinterpret the persisted local offset as remote authority.
- Human decision: pending Phase 3 gate.

### S16-03 — Explicitly accept or guard the unversioned downgrade loss of local timing state

- Stage / reviewer: Stage 16 — Compatibility and persistence.
- Scope reviewed / exclusions: current well-formed `.jam` opened by the master/older application; no request for broad persistence versioning outside the timing key.
- Severity: follow-up compatibility decision.
- Evidence: `JamFile` declares `VERSION_V`/`VERSION_LEGACY` at `JammaLib/src/io/JamFile.h:34`–`:38`, but every parsed JSON is assigned `VERSION_V` at `JamFile.cpp:85` and `ToStream` writes no version field at `:400`–`:465`. The branch adds `transportoffsetloopfrac` at `:406`; `master` has neither its parser nor writer. Both parsers ignore unknown keys and writers rebuild only recognized keys, so an older binary silently runs with offset zero and drops the new field on re-save.
- Why it matters: the additive schema remains parseable, but a user can lose a musically relevant local phase setting without an incompatibility signal. This does not justify persisting remote session authority, but it is an explicit product compatibility choice that the current enum does not represent.
- Recommended disposition: at the Phase 3 gate, either accept and document that downgrading to pre-branch binaries loses `transportoffsetloopfrac`, or require a focused guard (a real schema marker/warning or unknown-field preservation) scoped to preventing silent timing-state loss. Do not expand this into VST/window/resource cleanup.
- Protected timing concepts affected: local transport offset and per-entity phase only; no remote epoch/map state should be added to the file.
- Verification: current reader loads master `.jam` with zero default; current writer/current reader round-trips offset endpoints and ordinary fractions; old-reader behavior is documented or guarded; old re-save loss is an explicit acceptance test/manual case if the break is accepted.
- Human decision: pending Phase 3 gate.

### S16-04 — Make full BPI geometry the explicit compatibility boundary

- Stage / reviewer: Stage 16 — Compatibility and persistence.
- Scope reviewed / exclusions: well-formed older/partial timing producer shapes, not hostile numeric values (Stage 18) and not upstream NJClient edits.
- Severity: follow-up contract cleanup.
- Evidence: both current Jamma-owned producers populate BPI and derive `IsValid` through `IsValidRemoteTiming` at `JammaLib/src/ninjam/NinjamConnection.cpp:709`–`:733` and `:922`–`:946`. Shared validity requires `bpi >= 1` at `NinjamTiming.h:45`–`:57`, and coordinator observation exits for invalid timing at `NinjamTimingCoordinator.cpp:68`–`:77`. Nevertheless, `_MakeProposal` claims an older/partial compatibility case and derives missing BPI when `timing.Bpi == 0` at `:291`–`:304` (added by `e72f3b0`). No current producer can deliver that case as valid. The protected glossary and approved S13-02 state both require complete validated remote geometry.
- Why it matters: the code advertises a fallback contract that the producer boundary rejects, leaving reviewers and future maintainers uncertain whether BPI is authoritative/required or locally synthesized. Supporting both would weaken the approved complete-state invariant; pretending to support both provides no compatibility.
- Recommended disposition: declare BPI-less observations an intentional unsupported older shape, keep full server BPI in the desired-state contract, and remove the unreachable deduction fallback/comment in a later approved cleanup. If the human instead requires partial-producer compatibility, validation and tests must make that an explicit versioned producer contract without replacing a supplied server BPI. The first option is strongly preferred and matches the accepted timing model.
- Protected timing concepts affected: remote geometry, remote grid/BPI authority, local seed deduction; they remain distinct.
- Verification: producer/consumer contract test that complete plausible BPI is accepted and BPI-absent observation produces no authority change; retired fallback search if removal is approved; local seed deduction remains used only for local timing, never to overwrite supplied remote BPI.
- Human decision: pending Phase 3 gate.

## Handoffs

- Phase 3 integrator: S16-01 should be reconciled with F-027 (physical availability/fresh epoch), F-009, and S13-02. Its unique evidence is the persisted/default auto-connect producer path; do not lose that test case when deduplicating lifecycle work.
- Phase 3 integrator: S16-02 is not a proposal to collapse per-loop phase into master phase. Its compatibility proof specifically relies on unequal lengths showing that `-0.25M` and `+0.75M` are not interchangeable for all entities.
- Phase 3 integrator/human gate: record two explicit break decisions: whether branch-intermediate signed-offset `.jam` files are supported, and whether older-binary downgrade loss of the current timing key is accepted. Internal event/delta command compatibility is already intentionally rejected by the approved complete-state direction.
- Stage 14 / verification synthesis: add a persisted-config start contract, a current transport-offset writer round trip, and signed migration cases with `M`, `2M`, and non-divisor audio/MIDI lengths. Current tests cover only parse-time scalar normalization and default NINJAM identity.
- Stage 15 / documentation synthesis: if S16-03 downgrade loss is accepted, document it at the `.jam` contract boundary. If S16-04 is accepted as recommended, remove claims that BPI-less partial observations are supported and describe full remote geometry as mandatory.
- Stage 18: malformed/non-finite offset, tempo/BPI bounds, hostile JSON, credential/path trust, and parsing resource limits remain yours. S16-01–S16-04 assume structurally well-formed inputs.
- Phase 4 batching: recommended order is prerequisite tests -> F-027/S16-01 unified session start and epoch -> complete desired-state work -> S16-02 migration decision/implementation -> small schema/fallback documentation or deletion. No compatibility shim should reintroduce standalone invalidation or phase-delta commands.

## Uncertainties

- The repository does not state whether builds between `6dc0c73` and `4c1c0e1` were distributed or used to create durable `.jam` files. That human/product fact determines whether S16-02 requires behavior-preserving migration or an explicit unsupported-artifact decision; the mathematical non-equivalence for longer loops is not uncertain.
- There is no production-faithful test seam for persisted auto-connect without opening a real session. The call graph proves that coordinator initialization is skipped; runtime/manual evidence is still required after an approved fix.
- The existing `JamFile::Version` enum may be historical scaffolding rather than a promised wire-version mechanism. S16-03 does not assume a broad versioning redesign; it asks for an explicit downgrade policy for this timing field.
- Optional `.jam` NINJAM `bpm`/`bpi` fields are parsed and re-emitted but not consumed as authority. Their historical purpose is unclear, but retaining inert round-trip metadata creates no new timing defect. Removing or redefining them would require a separate compatibility decision.
- Current full-observation compatibility remains conditional on accepted Phase 2 corrections for coherent publication, presence, sample-rate conversion, physical loss, and epoch. This report does not weaken or duplicate those findings.
- No build/test/manual session was performed, and no cleanup is authorized. Line references describe current production source under review-artifact commits.

## Conclusion

Stage 16 found one must-fix persisted-session defect, one must-fix-or-explicitly-break migration defect, and two focused compatibility decisions.

The default and any well-formed `.jam` NINJAM identity currently start the network connection without starting the timing coordinator, so the primary persisted startup path cannot enter the new request/follow/epoch authority lifecycle until the user manually reconnects. That should be unified with F-027/S13-02 under one existing integration owner.

The new local transport-offset key is backward-readable when absent, but the claimed signed-format migration is not behaviorally equivalent for loops longer than the master. The `.jam` format also has no real wire version, so older binaries silently ignore and then discard the new timing state. The human gate must decide support for signed branch artifacts and explicitly accept or guard downgrade loss.

Remote geometry, phase, map, anchors, generation, follow policy, and session epoch correctly remain runtime-only and must not be added to `.jam`. The recommended compatibility boundary requires complete authoritative BPI and removes the unreachable partial-BPI pseudo-fallback. No protected timing distinction is collapsed, no retained out-of-scope feature is changed, and no cleanup is implemented.
