# Merge-readiness scope and inventory

## Kickoff baseline

- Captured: 2026-08-27 (America/Mexico_City).
- Review branch: `bugfix/align-remote-join`.
- `HEAD`: `e72f3b0f489cfa12ea697e966d3c0f619e31d1f6`.
- Merge base with `master`: `4941b780f7ff5a46f742167d79338e3ab592a565`.
- `master` tip: `4941b780f7ff5a46f742167d79338e3ab592a565` (equal to the merge base at kickoff).
- Commit range: `master..HEAD`; comparison: `master...HEAD`.
- Commit count: 123 total, including 3 merge commits.
- Diff summary: 199 files changed, 14,691 insertions, 1,507 deletions.
- Change kinds: 71 additions, 124 modifications, and 4 renames.
- Worktree before review-artifact creation: no tracked modifications; `doc/merge-readiness-review/` was an untracked directory containing the five supplied plan/phase documents. Those files are review inputs and must not be overwritten. No other pre-existing change was reported by `git status --porcelain=v2 --branch`.
- Review writes authorized by the plan: only this file, the canonical review artifacts, assigned stage reports, phase packets, and later batch reviews. Production code, tests, project/build files, and unrelated changes are read-only during Phase 1.

## Protected timing glossary

These distinctions are review constraints, not simplification opportunities.

| Term | Protected meaning |
| --- | --- |
| Local master transport | `utils::Timer` geometry and position: interval/master length, loop count, sample offset/master phase, absolute sample position, and the monotonic scene coordinate. |
| Monotonic scene coordinate | `Timer::SceneSamplePos`; a durable, unwrapped coordinate that does not reset when NINJAM replaces Timer geometry. It is not wrapped Timer geometry. |
| Master phase | Position inside the current master interval. It is not a per-loop cursor. |
| Per-loop phase | An entity-specific wrapped cursor using that loop/entity's own logical length. Audio uses `Loop::BodyPlayIndex`; MIDI event playback uses the `LoopTake` MIDI event cursor. |
| Sync phase map | The bridge from a remote master ruler to local-source progress. It retains a scene anchor, old local master length, new remote master length, and per-entity source/phase anchors. It separates master phase correction from source/scene coordinates. |
| Mapped elapsed time | Common local-master progress derived from remote elapsed time. Every entity receives the same elapsed amount and wraps it by its own length; it is not a shared loop cursor. |
| Source/scene anchor | A durable relationship between the monotonic scene coordinate and an entity's phase/source coordinate. It survives accepted restores within one follow session and is invalidated before an independent session. |
| Local timing | Device-rate local transport and loop advancement, including intentionally different loop lengths and offsets. It remains distinct from remote authority. |
| Remote timing | NINJAM interval/phase observations validated for plausible BPM/BPI, interval, and sample rate, then converted to the device sample rate before use. |
| Remote join | Session-level request/acknowledgement and follow decision followed by audio-boundary application of an accepted timing command. It is not itself a coordinate system. |
| Loop alignment | Restoration of each local audio/MIDI entity from its own anchor plus common mapped elapsed time, preserving intentional relative offsets. |
| `ContinuousSync` | Follow policy for a remote tempo close to local timing; normally small corrections. |
| `BlockSync` | Follow policy for a materially different accepted tempo; same map/anchor model, potentially larger corrections. |
| `NoSync` | Follow policy for staying local, invalid timing, or disconnect. It clears the map and scene anchors and leaves local timing free-running. |
| Follow policy | Chooses whether/how accepted remote authority disciplines local state. It is not a clock, cursor, or coordinate system. |
| Local grain | Exact audio construction unit used for local loop geometry. It is not a remote beat and need not equal the active quantisation grid step. |
| Active quantisation grid | A division count of the current interval, evaluated by rounded boundaries. Its migration is separate from sync-map phase preservation. |

Any proposed merge of these concepts is out of bounds unless it proves behavioural equivalence for different loop lengths, intentional offsets, reconnects, and `NoSync` invalidation.

## Scope inventory

### Objective branch shape

The branch is much broader than its name: timing/remote-join work is interleaved with engine refactors, MIDI timing/routing, VST3 parity and state, GUI/HUD/resources, persistence, build tooling, documentation, and extensive native tests. Phase 1 must determine which breadth is intentional and which is accidental without treating breadth alone as a defect.

The kickoff approximation was reconciled by Stage 1 into this exact, non-overlapping partition of all 199 diff rows. Binary files count as files but not textual additions/deletions:

| Area | Files | Insertions | Deletions | Binary files |
| --- | ---: | ---: | ---: | --- |
| Native tests | 35 | 4,123 | 106 | 0 |
| Engine/local loop state (including quantiser renames) | 12 | 2,438 | 666 | 0 |
| NINJAM/session timing | 20 | 2,268 | 107 | 0 |
| VST | 12 | 1,268 | 81 | 0 |
| GUI | 17 | 1,245 | 46 | 0 |
| Documentation | 8 | 676 | 17 | 0 |
| MIDI | 11 | 603 | 51 | 0 |
| Audio/timing application | 6 | 557 | 12 | 0 |
| Graphics/model/window | 10 | 390 | 51 | 0 |
| Utilities/transport | 6 | 301 | 2 | 0 |
| App resources | 25 | 193 | 16 | 19 |
| Repository tooling/policy | 6 | 181 | 8 | 0 |
| Persistence/I/O | 13 | 177 | 51 | 0 |
| Remaining library/build | 9 | 117 | 16 | 0 |
| Library resources | 6 | 100 | 58 | 0 |
| App shell/project | 3 | 54 | 219 | 0 |
| **Total** | **199** | **14,691** | **1,507** | **19** |

Top added-line concentration: `GuiHud.cpp` (721), `Vst3Plugin.cpp` (720), `NinjamTimingIntegration_Tests.cpp` (650), `Scene.cpp` (607), `NinjamTimingCoordinator_Tests.cpp` (547), `Station.cpp` (521), `LoopTakeTiming_Tests.cpp` (476), `LoopTake.cpp` (452), `NinjamConnection.cpp` (428), and `NinjamTimingCoordinator.cpp` (388).

### Subsystem partition and ownership

| Partition | Primary implementation ownership | Phase 1 questions |
| --- | --- | --- |
| App/build/tooling | `Jamma`, project files, `.github`, `.vscode` | Intentional scope, project/resource membership, thin wiring, accidental branch growth. |
| Audio/timing application | `audio/AudioHost`, `utils/Timer`, `utils/MusicalTransport` | Boundary between accepted commands, Timer geometry, scene coordinate, and station restores. |
| Engine/local loop state | `engine/Scene`, `Station`, `LoopTake`, `Loop`, `Trigger`, `Quantiser` | Station -> LoopTake -> Loop ownership; per-entity phase and quantisation placement. |
| NINJAM/session timing | `ninjam/*` | Remote observation, validity, follow policy, coordinator, audio-boundary command, map/alignment, connection/session ownership. |
| MIDI | `midi/*` and engine MIDI cursors | Event cursor vs automation anchor, block timestamps, routing, quantisation, source-coordinate restores. |
| GUI/graphics/resources | `gui/*`, `graphics/*`, `resources/*`, app assets | HUD/popup/resource scope, model boundaries, new assets, and intentional UX work. |
| VST | `vst/*` | Plugin parity/state/mapping scope and interface ownership. |
| Persistence/I/O | `io/*` | Public/persisted contracts and whether timing/UI/VST changes leaked into formats. |
| Tests/docs | `test/JammaLib_Tests/*`, `doc/*` | Evidence and intent inputs only in Phase 1; quality is Phase 3 ownership. |

### Public/interface surface changed

The diff changes 61 header paths: 18 additions, 41 modifications, and 2 renames. Only `JammaLib/include/Constants.h` is in the explicit public include directory; the other 60 are source-tree interfaces, many consumed across subsystems and by tests. Particularly consequential surfaces include `AudioHost.h`, `Loop.h`, `LoopTake.h`, `Scene.h`, `Station.h`, `Trigger.h`, `Timer.h`, `MidiRouter.h`, `NinjamConnection.h`, `NinjamSession.h`, `NinjamTiming*.h`, `NinjamAudioTimingCommand.h`, `NinjamLoopAlignment.h`, `MusicalTransport.h`, `IVstPlugin.h`, and `Vst3Plugin.h`.

### Hot-path map

All callback-owned implementation files named by the real-time guide are changed where applicable: `Scene.cpp`, `Loop.cpp`, `LoopTake.cpp`, `Station.cpp`, `Trigger.cpp`, and `NinjamConnection.cpp`. The timing path additionally crosses changed `AudioHost.cpp`, `Timer.cpp`, `MidiRouter.cpp`, and NINJAM timing command/coordinator code. Phase 1 reviews placement and vocabulary only; synchronization, cost, runtime correctness, and lifetime belong to Phase 2.

### Generated/binary/assets scope

- 19 TGA binaries were added under `Jamma/resources/textures/`.
- Four shader files were added and existing shader/resource-list files changed.
- Project/filter files changed for the app, library, and native tests.
- No file is classified as generated solely from its extension; Stage 1 must identify provenance and whether these assets/build entries intentionally belong to the branch.

### Commit-to-feature map (kickoff grouping)

The 123-commit history contains repeated plans, implementations, debugging, cleanup, and merges. The stable intent groups for archaeology are:

| Feature/history group | Representative commits/subjects |
| --- | --- |
| Initial NINJAM UX and tempo policy | `0125b4c`, `5258f85`, `3ca53c7`, `92fc5e7` |
| Transport phase and MIDI anchor sync | `eb9db69`, `1cba2d6`, `aa76ab0`, `fbc76ba`, `6dc0c73`, `f68f4c8` |
| NINJAM audio wiring/latency/metronome | `adb7b38`, `782b8a8`, `c4c0607`, `e0bc33b` |
| MIDI routing/jitter/quantisation | `7668c29`, `95dea73`, `3db18e9`, `5bfebcd` |
| HUD/trigger/station visuals and resources | merge chain around `034c3ca` through `24a43a0`, followed by HUD fixes |
| VST3 parity/state/mapping | `711e24f`, `62fc990`, `e5081de`, merged by `880112d` |
| Timing refactor and unified command model | `9c7ca51`, `d0d208e`, `f36bfe7`, `d6fdb88`, `b9619b9` |
| Local-loop alignment/source-coordinate map | `b41b5a8`, `4c1c0e1`, `f74d4ec`, `6abf7c7`, `e0f669c`, `0d90bac`, `bef7943`, `e72f3b0` |
| Build/resource/tooling | `2d1b02c`, `70ba98c`, `5daefa3`, `b43abbe`, `4a74ca9`, `f37e9c0` |
| Active-grid/local geometry migration | `92e8907`, with remaining work described in the timing design document |

Stage 5 owns commit-backed reversions, superseded experiments, and direction changes; this grouping does not itself declare residue.

## Phase 1 scope partition

| Stage | Primary ownership | Explicit exclusions | Required report |
| --- | --- | --- | --- |
| 1 — Diff and ownership inventory | Objective scope, stats, partition, churn, public surface, accidental growth | Layout/code-quality judgments | `stage-reports/01-diff-inventory.md` |
| 2 — Logical layout | Placement, dependency direction, layer leakage, duplicate ownership | Naming/style; runtime correctness | `stage-reports/02-logical-layout.md` |
| 3 — Conventions | Policy/style rules, headers/implementations, naming form, RAII/value semantics, hidden globals | Architecture, semantic vocabulary, dead reachability | `stage-reports/03-conventions.md` |
| 4 — Naming and vocabulary | Coordinate, authority, lifetime, thread ownership, protected glossary | General style; merging protected concepts | `stage-reports/04-vocabulary.md` |
| 5 — Git-history archaeology | Intent, reversions, superseded experiments, fixup chains, changed direction | Declaring current code dead/incorrect | `stage-reports/05-history.md` |
| 6 — Stale/dead-code sweep | Reachability/redundancy and dead/dormant/duplicated/complex distinctions | Broad simplification of live abstractions; repeat archaeology | `stage-reports/06-stale-code.md` |

## Artifact directories and single-writer ownership

- Lead only: `00-scope-and-inventory.md`, human entries in `decisions.md`, backlog status.
- Stage investigator only while active: its assigned immutable `stage-reports/NN-short-name.md`.
- Phase integrator only: `findings.md`, `verification-matrix.md`, and the active phase packet (`phase-packets/phase-1.md`, `phase-packets/phase-2.md`).
- Reserved for later phases: `cleanup-backlog.md`, `batch-reviews/`; `merge-brief.md` must not be created before Phase 4.
- Chat is coordination only. Every dispatched stage must create its report, including a complete `no findings` report when applicable.

## Phase 2 scope and reconciled thread/ownership matrix

The human gate restricts deep Phase 2 review to remote timing/sync structural and behavioral changes. HUD, VST3 parity, window persistence, and tooling remain in the merge but are excluded from cleanup review except where a timing seam requires evidence. Stage ownership was: S07 concurrency/publication; S08 proven callback/hot-path cost; S09 timing behavior; S10 failure transitions; S11 numeric/clock domains; S12 timing/session lifetimes.

| Shared state / item | Writer / owner | Reader / thread | Handoff and compound invariant | Lifetime / hot-path status | Conclusion |
| --- | --- | --- | --- | --- | --- |
| Live remote timing observation | Audio callback at `AudioHost.cpp:437`–`:453` | Scene job at `Scene.cpp:336`–`:347` | Fixed atomic odd/even snapshot; complete value or no value | AudioHost member; writer hot | Mailbox coherent, but upstream getter source violates thread contract: F-021 |
| Unified timing command | Scene job/UI sites at `Scene.cpp:254`–`:305`, `:394`–`:482`, serialized by `_sceneMutex` | Audio callback at `AudioHost.cpp:162`–`:330` | Atomic complete latest value; producer contract externally enforced; latest value does not subsume all transitions | AudioHost member; reader hot | Race-free publication, semantically unsafe ordering: F-024; producer ownership enriches F-009 |
| Applied-command receipt | Audio callback at `AudioHost.cpp:321`–`:329` | Job logger at `Scene.cpp:517`–`:535` | Atomic odd/even complete receipt | AudioHost member; writer hot | Race-free |
| Local transport-offset mailbox | UI/setup through `Scene.cpp:2196`–`:2208`; audio consumes at `AudioHost.cpp:332`–`:350` | Audio callback | Atomic latest value preserves explicit zero | AudioHost member; reader hot | Race-free under current one-writer assumption; concurrent test required |
| Published station membership | Scene publishes after mutations at `Scene.cpp:2147`–`:2301` | AudioHost/Scene audio paths | Immutable `shared_ptr<const vector<shared_ptr<Station>>>`, release/acquire | Snapshot pins stations; hot | Safe except raw `_stations` bypass in empty reset: F-022 |
| AudioHost policy/common map | Audio callback at `AudioHost.cpp:167`–`:365` | Audio callback | Thread-confined transaction | AudioHost member; hot | Race-free; common coordinate should be computed once under F-006 |
| Per-take map/anchors/cursors | Audio callback through Station fan-out at `LoopTake.cpp:518`–`:679` | Audio plus atomic diagnostic/UI readers | Map thread-confined; anchor value published before presence; immutable membership snapshots | Take pinned by Station snapshot; hot | Race-free, but invalidation skips generation reset: F-025 |
| Timer transport tuple | Audio callback `Timer.cpp:40`–`:58`, `:205`–`:237`; some Scene/Quantiser paths | Audio plus coordinator/job reads | Individual atomics only; no version spans length/count/phase/scene | Shared Timer; hot | Scalar-race-free but compound tuple incoherent: F-023; width: F-029 |
| Timer musical transport | Audio callback remote reanchor/advance/reset | Audio callback/plugin transport consumers | Thread-confined mutable value | Timer-owned; hot | Safe if ownership remains audio-only |
| Coordinator/tracker/options | Job/UI under `_sceneMutex` | Job/UI, plus exceptional audio `HasConnectedTiming` read | Plain compound state machine under intended owner | Network service owned; exceptional reader hot | Audio read and reset path unsafe: F-022 |
| Remote MIDI grid | Job/UI Quantiser writer | Audio quantisation reader | Atomic odd/even complete snapshot | Take lifetime; reader hot | Safe with serialized writers; audio empty-reset creates second writer: F-022 |
| NJClient connection/timing | Job calls `Run()`/snapshot getters; audio calls `AudioProc` and current timing getters | Job and audio | Session guard pins object, but no permitted coherent publication protects getters | Connection-owned; hot | Only `AudioProc` is supported on audio thread: F-021 |
| Connection identity | Lifecycle under `_lifecycleMutex` | Job/audio via `NinjamConnectionUse` | Atomic pointer plus active-user retirement | Live guard pins connection; acquire is hot/unbounded | Identity publication sound; borrowed stereo escapes guard: F-033 |
| Whole connection/UI snapshot | Job `_UpdateSnapshot` | Job/UI consumers | Mutex-protected value copies | Non-audio | Race-free; physical loss needs explicit epoch transition: F-027 |
| MIDI clock anchor | Audio publisher | MIDI/control readers | Atomic sequence plus two bounded reads | AudioHost member; writer hot | No proven race; retain targeted concurrency test obligation |
| Alignment diagnostics | Audio/job atomic snapshots plus job-owned before/after state | Off-thread logger, with callback entry sites | Operational state is separate; dead receipt has no writer | Reads touch hot-owned state | F-019 removes dead receipt; F-034 removes callback logging and formats off-thread |

Intentional non-real-time synchronization retained for the human gate: Scene `_sceneMutex` serializes job/UI coordinator and command-publication work; connection/snapshot/controller mutexes protect lifecycle and job/UI snapshots; request/network operations remain off callback. No intentional callback lock, allocation, wait, or I/O is accepted. `atomic<shared_ptr>` snapshot loads and reference-count traffic remain callback costs whose platform lock-freedom/performance must be measured at maximum configured hierarchy size.

## Task board

Phase 1 status: **human gate accepted** in [`decisions.md`](decisions.md). Phase 2 status: **human gate accepted with the F-024 complete-desired-state direction** in [`decisions.md`](decisions.md). Phase 3 stages are integrated and stopped at the human gate in [`phase-packets/phase-3.md`](phase-packets/phase-3.md). Cleanup implementation remains prohibited until Phase 4 reconciliation, batching, and approval.

### Phase 3 kickoff baseline

- Branch: `bugfix/align-remote-join`.
- Phase 3 kickoff `HEAD`: `70e48a5403dd409aca73e7efc0490464c8636a00`.
- Production tip under review remains `e72f3b0f489cfa12ea697e966d3c0f619e31d1f6`; later commits contain review evidence and human decisions only.
- Merge base and `master` tip: `4941b780f7ff5a46f742167d79338e3ab592a565`.
- Worktree at kickoff: clean.
- Raw `master...HEAD` at kickoff: 223 files changed, 17,676 insertions, and 1,507 deletions. Phase 3 excludes `doc/merge-readiness-review/` from production-code conclusions except as governing evidence.
- Cleanup scope: remote timing/sync structural and behavioural changes only, including the human-directed review of timing/logging responsibility added to `Scene`, `Station`, and `LoopTake`. Retained HUD, VST3 parity, window/tooling, and unrelated changes are deliberate exclusions.

### Phase 2 kickoff baseline

- Branch: `bugfix/align-remote-join`.
- Review kickoff `HEAD`: `a202d27a6923288846577b03cd9b305bfa6405db`.
- Merge base and `master` tip: `4941b780f7ff5a46f742167d79338e3ab592a565`.
- Worktree at dispatch: clean.
- Production code tip under review remains `e72f3b0f489cfa12ea697e966d3c0f619e31d1f6`, with the Phase 1 inventory of 199 files, 14,691 insertions, and 1,507 deletions.
- The newer `a202d27` commit contains Phase 1 review artifacts and human decisions only. Raw `master...HEAD` now includes those artifacts (216 files, 16,364 insertions, and 1,507 deletions); Stages 7–12 exclude `doc/merge-readiness-review/` from production-code conclusions.

| Stage | Status |
| --- | --- |
| 01 Diff and ownership inventory | integrated |
| 02 Logical layout | integrated |
| 03 Conventions | integrated |
| 04 Naming and domain vocabulary | integrated |
| 05 Git-history archaeology | integrated |
| 06 Stale/dead-code sweep | integrated |
| 07 Thread safety | integrated |
| 08 Audio and other hot-path performance | integrated |
| 09 Timing and remote-join correctness | integrated |
| 10 State-machine and failure paths | integrated |
| 11 Numerical, boundary, and clock domains | integrated |
| 12 Resource and lifetime review | integrated |
| 13 Simplification and code size | integrated |
| 14 Unit-test quality | integrated |
| 15 Docs and comments | integrated |
| 16 Compatibility and persistence | integrated |
| 17 Observability and diagnosability | integrated |
| 18 Security and input robustness | integrated |
| 19 Cross-review reconciliation | pending |
| 20 Change impact and regression surface | pending |
| 21 Build, test, and merge hygiene | pending |
