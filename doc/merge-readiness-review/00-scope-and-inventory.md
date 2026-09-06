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
| Live remote timing observation | Job-owned `NinjamConnection::_UpdateSnapshot`; AudioHost combines it with the block-local transport value | Audio callback fixed-value read, then Scene job consumes the combined observation | Stable-even AudioProc bracket for the remote tuple plus fixed atomic odd/even combined snapshot | Connection remote owner and AudioHost combined owner; callback bounded | B004 `88e3ecc`: only `AudioProc` remains upstream on the callback; focused-verified, live/race evidence deferred |
| Complete desired remote transport state | Serialized NetworkService calls into the sole Coordinator producer; Scene forwards the returned value without constructing transport state | Audio callback through private `AudioHost::ApplyDesiredTimingAtAudioBoundary` | Immutable complete latest value with session epoch, version, generation, intent, policy, geometry, phase, and observation presence; AudioHost rejects stale versions and compares desired with applied | AudioHost mailbox/member; reader hot and bounded | B006+B007 corrected `da0db24`: P1/P2 cover complete coalescing, real concurrent production, coherence, version precedence, and idempotence; evidence-only rejection `f181cbb` corrected at `19e9ca8`; independently approved `b3f2beb` |
| Applied desired-state receipt | Audio callback after accepted boundary application | Coordinator correlation through a mutex-serialized NetworkService forwarder; Scene presents only off callback | Atomic odd/even complete epoch/version/generation/intent/policy/scene/delta receipt | AudioHost member; writer hot and bounded | B014 corrected `1b98fe2`, independently approved `6150f5f`; live trace deferred |
| Local transport-offset mailbox | UI/setup through `Scene.cpp:2196`–`:2208`; audio consumes at `AudioHost.cpp:332`–`:350` | Audio callback | Atomic latest value preserves explicit zero | AudioHost member; reader hot | Race-free under current one-writer assumption; concurrent test required |
| Published station membership | Scene publishes after mutations at `Scene.cpp:2147`–`:2301` | AudioHost/Scene audio paths; job-owned empty-state detection uses per-station take snapshots | Immutable `shared_ptr<const vector<shared_ptr<Station>>>`, release/acquire | Snapshot pins stations; hot | Initial B008 review `50410a9` found the remote-snapshot branch bypassed publication; corrected `6e908ca` shares one `GetLoopTakeSnapshot()`-derived value across both job timing branches; independently approved `e6cdd6f` |
| AudioHost policy/common map | Audio callback desired-versus-applied transaction | Audio callback | Epoch/`NoSync` clears authority without moving cursors; replacement/discipline restores the prior mapped coordinate before rebase; new maps capture entity anchors after local offset application | AudioHost member; hot | B009 `8d0bc53` owns the sole common calculation; correction `9eb1575` verifies 704 calculations over 704 calls at a 3,072-entity explicit saturation ceiling (no configured product maximum exists), versus 720,896 in the per-take reference; independently approved `3e50698` |
| Per-entity map anchors/cursors | Audio callback through neutral Station correction/reset/capture/restore fan-out | Audio plus atomic diagnostic/UI readers | Each audio/MIDI entity retains its own anchor, length, and modulo; no common map geometry/policy remains in Station/LoopTake; queued local corrections stay independent | Take pinned by Station snapshot; hot | B009 P4 proves unchanged anchors across restore/rebase; B010 `9b81a23` consolidates only the two equivalent direct shifts behind one private helper while retaining caller gates/accounting and leaving queued correction unchanged; independently approved `1662514b` |
| Timer transport tuple | Audio callback `Timer` mutation and `ObserveTransport` capture | Audio plus coordinator/job through the combined timing observation | One audio-captured 64-bit length/phase/count/absolute/scene value crosses the existing fixed mailbox | Shared Timer with AudioHost publication; hot | B004 `18663f3`: coherent and wide at the coordinator boundary |
| Timer musical transport | Audio callback remote reanchor/advance/reset | Audio callback/plugin transport consumers | Thread-confined mutable value | Timer-owned; hot | Safe if ownership remains audio-only |
| Coordinator/tracker/options | Job/UI under `_sceneMutex` | Job/UI | Plain compound state machine under intended owner | Network service owned; non-audio | B008 removes the empty-scene callback read of `HasConnectedTiming`; corrected `6e908ca` gives Observe/Tick the same job-owned content state; live/race evidence remains Stage 21 |
| Remote MIDI grid | Coordinator creates a grid value paired with each accepted desired geometry; Scene forwards it to Quantiser | Audio quantisation reader; job owner performs empty-state cleanup | Producer-paired geometry/generation/origin; Quantiser publishes its atomic odd/even complete snapshot | Take lifetime; reader hot | Corrected P2 proves desired/grid coherence; corrected B008 preserves connected-empty timing and performs disconnected clear once off callback; independently approved `e6cdd6f` |
| NJClient connection/timing | Job calls `Run()`/snapshot timing getters; audio calls only `AudioProc` | Job publishes fixed tuple; audio performs a bounded coherent read | Session guard pins object; odd/even AudioProc sequence rejects overlapping tuples; job loss clears publication | Connection-owned; hot | B004 getter contract retained; B005 `0fcf7c4` invalidates stale remote timing on physical loss |
| Connection identity and physical epoch | Lifecycle under `_lifecycleMutex`; job Pump derives availability and runtime-only epoch | Job lifecycle/coordinator plus audio via `NinjamConnectionUse` | Atomic pointer plus active-user retirement; unavailable→available increments epoch; replacement forces unavailable edge through Pump-shared seam | Live guard pins connection; acquire is hot/unbounded | B001 lifetime and corrected B005 epoch edges focused-verified; live retry remains Stage 21 |
| Whole connection/UI snapshot | Job `_UpdateSnapshot` and physical-loss Pump edge | Job/UI consumers | Mutex-protected value copies; loss clears connection and Controller pending snapshots | Non-audio | B005 `0fcf7c4`: stale snapshot/station state is cleared on the loss edge |
| MIDI clock anchor | Audio publisher | MIDI/control readers | Atomic sequence plus two bounded reads | AudioHost member; writer hot | No proven race; retain targeted concurrency test obligation |
| Alignment diagnostics | Coordinator job owner captures rejection and desired/applied transitions | Scene job path presents only when Event logging is verbose | Fixed 32-event ledger, per-reason first occurrence/power-of-two suppression summaries, and bounded overflow summaries; diagnostic state remains separate from operational timing | Disabled capture returns before diagnostic work; no callback formatter, I/O, hierarchy traversal, or dead receipt | B014 corrected `1b98fe2`, independently approved `6150f5f`; live trace deferred |

Intentional non-real-time synchronization retained for the human gate: Scene `_sceneMutex` serializes job/UI coordinator and command-publication work; connection/snapshot/controller mutexes protect lifecycle and job/UI snapshots; request/network operations remain off callback. No intentional callback lock, allocation, wait, or I/O is accepted. `atomic<shared_ptr>` snapshot loads and reference-count traffic remain callback costs whose platform lock-freedom/performance must be measured at maximum configured hierarchy size.

## Task board

Phase 1 status: **human gate accepted** in [`decisions.md`](decisions.md). Phase 2 status: **human gate accepted with the F-024 complete-desired-state direction** in [`decisions.md`](decisions.md). Phase 3 status: **human gate accepted** in [`phase-packets/phase-3.md`](phase-packets/phase-3.md). Phase 4 execution was approved at `2e770b743d9f2466b2edafff5c92faf139d93108`; B001–B005 are independently approved through superseding B005 review `ae5d862e553f787afcc72b5a5cea89fad992b5d3`. Merged B006+B007 is implemented, verified, and independently approved at superseding review `b3f2bebd06ea9b35daff14a29bcb4e29ef19e298` after the evidence-only rejection/correction sequence `f181cbb`/`19e9ca8`/`8ba4353`. B008 was corrected after initial rejection `50410a9` and independently approved at superseding review `e6cdd6ffcc28b8926c07808cc2dbf60e122d3050`. B009 implementation `8d0bc53` was initially rejected at `ba44ded` only for missing callback-benchmark evidence; test-only saturation correction `9eb1575` was independently approved at `3e50698b6af7f280bbf2967c8142ac8c1043978a`. B010 test-only characterization `64ff124` and implementation `9b81a23` were independently approved at `1662514b8acb2a5e456888e96c1ba64f27016a2d`. B011 characterization `897f33e` and signed-offset implementation `7e3d517` were independently approved at `feca48d90b4b2d1925f1af92d65bc03c8da5c29d`. Under human amendment `67778633d60b0292709b4e459402b6bc3099be48`, B012 commits `2473d50`/`8c15044`/`c7f668a`/`9473d34` are implemented and verified, with independent review pending; B013 remains gated.

The B012 pending status above is superseded by independent approval `e80dbdb717e535be23dc2ca3c8616b2a3de61f24`, which repeated the wrapped build, prerequisite, exact focused 50/50, disconnected/`NoSync`, full 834-test suite, and static/diff audits at clean canonical `a2d2689e4e4dcb399ea7de50fc0971b11ae1e863`. B013 may begin under the sequential gate.

B013 characterization `3fbb0122a5b73eff2a315a5b4a6c6e65c40f8dfb` truthfully failed its three new proposal-authority tests before implementation. At `ee5e574465c697b539f120a4cb144f6c1b0206da`, the one value-level proposal identity is used by Coordinator and Scene, remote BPI is mandatory without a local-deduction fallback, and disconnected local Quantiser inference is unchanged. The prerequisite-plus-new contract passed 5/5, the exact focused timing filter passed 68/68, and the full suite passed 836/837 with only the expected hardware MIDI skip. Status: implemented and verified, pending independent `batch-reviews/B013.md`; B014 remains gated.

The B013 pending status above is superseded by independent approval `2d3836b3f5e0602ea65695043345b21e76b33b5f`, which repeated the wrapped build, prerequisites 2/2, new tests 3/3, exact focused filter 68/68, and full 837-test suite at clean canonical `46ee7e4499c37756711f5374983032ff2b419971`. B013 is implemented, verified, and independently approved; B014 may begin under the sequential gate. Live prompt/grid/local-inference traces and final Release/tooling/audit work remain Stage 21.

B014 characterization `6312b8863dd58f65d0fe62e7f339999d7d04cc72` changed tests only and failed its wrapped build as expected against the absent bounded API. Human amendment `483c5d4` authorized the two minimum NetworkService forwarding operations. Implementation `058c486ae2588da914703a9efd55a0cd1f811a1e` makes the Coordinator the sole fixed-capacity diagnostic owner, correlates desired/applied receipts, moves presentation wholly to the Scene job path, and removes the dead receipt plus Scene/Station hierarchy logging. The final build, diagnostic contract 5/5, and exact timing regression filter 40/40 are green after a one-time Rebuild corrected stale incremental class layouts. Full-suite and live normal/verbose traces are not claimed. Status: implemented and verified, pending independent `batch-reviews/B014.md`; B015 remains gated.

Independent review `7d55c510d503b3aa287e41aaac580987f82f5b16` rejected B014 despite a green 841-pass full suite because repeated post-capacity anomalies could still print each job tick and one local-master-length guard was silent. Correction `1b98fe26cb4b3cc944aab9519624f827ea661696` adds first-occurrence/power-of-two suppression summaries and the missing zero/oversized local-master-length reason without changing NetworkService or callback code. The corrected build, diagnostics 7/7, exact B014 filter 42/42, and full suite 843 pass/one expected skip/zero fail are green. Status: corrected and verified, pending fresh independent B014 rereview; B015 remains gated and live verbose tracing remains Stage 21.

Superseding independent review `6150f5f3604bb4a483c118ae3c53ce9dfbbca3a8` approved the corrected B014 range and repeated all automated evidence successfully. B014 is implemented, corrected, verified, and independently approved; B015 may proceed. Live verbose timing traces remain Stage 21.

B015 passing-first prerequisites passed 2/2 at approved baseline `1ed040d`. Six mechanical, rollback-safe commits `e4fae40`/`7df0ee9`/`a4ef843`/`376f0b0`/`5a9facf`/`03f3d1e` apply only the approved acronym, remote-grid, clock-domain/correction, automation-origin, metronome, and export/UI vocabulary. Each wrapped incremental build and focused filter passed; final broad coverage passed 188/188 and the full suite passed 843 with one expected hardware skip and zero failures. Retired-name/UI audits are clean, local grain and protected glossary concepts remain, and no persistence or upstream file changed. Status: implemented and verified, pending independent `batch-reviews/B015.md`; B016 remains gated.

Independent review `b055a20b94910c10afb19d17ed50f110455f7dea` approved B015 after repeating the wrapped build, prerequisites 2/2, expanded broad filter 227/227, full 843-pass suite, and all semantic/static audits. B015 is implemented, verified, and independently approved; B016 may proceed.

B016 documentation-only commit `9927bdb56ba2777c3354439161e0afa5baa121cf` reconciles exactly the three owned NINJAM guides with implemented owners/contracts and explicit residual/manual status. Static source/test/link/identifier/scope/diff audits passed; no executable or manual evidence is invented. Status: implemented and statically verified, pending independent `batch-reviews/B016.md`; B017 remains gated.

Independent review `65709c163df80551a609c74b7fa7ae7263b35065` approved B016 after confirming every material statement/residual, all links, exact scope, and clean diff. B016 is implemented, statically verified, and independently approved; B017 may proceed.

B017 commit `a7678b48399f969f3148989f32da382541f986da` deletes only the unregistered obsolete 156-line remote-phase pseudo-test after its two registered replacements passed. Post-deletion build, prerequisites 2/2, required timing filter 115/115, full suite 843 pass/one expected skip/zero fail, membership/retired-symbol/active-symbol audits, and `master...HEAD` diff-check are green. Status: implemented and verified, pending independent `batch-reviews/B017.md`.

### Phase 4 kickoff baseline

- Branch: `bugfix/align-remote-join`.
- Phase 4 kickoff `HEAD`: `8cd8725bd4447d4125f671b9524f95e277396939`.
- Production tip under review remains `e72f3b0f489cfa12ea697e966d3c0f619e31d1f6`; later commits contain review evidence and human decisions only.
- Merge base and `master` tip: `4941b780f7ff5a46f742167d79338e3ab592a565`.
- Worktree at kickoff: clean.
- Cleanup scope remains remote timing/sync structural and behavioral work only. G3-5 explicitly strengthens the protected per-entity recovery invariant for `M`, `2M`, and `3M` loops with different relative play positions.
- Stage 21 execution, cleanup verification, batch reviews, and `merge-brief.md` are deferred until the proposed batches are approved.

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
| 19 Cross-review reconciliation | integrated |
| 20 Change impact and regression surface | integrated |
| 21 Build, test, and merge hygiene | design integrated; execution deferred |
