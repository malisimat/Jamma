# Phase 2 packet — Runtime safety, real-time performance, and correctness

## Inputs received

- Governing policy and design: `AGENTS.md`, the merge-readiness plan, Phase 2 specification, `doc/loop-alignment-and-ninjam-sync.md`, `doc/realtime-audio.md`, and `doc/build.md`.
- Human constraints and accepted Phase 1 baseline: `../decisions.md`, `../00-scope-and-inventory.md`, `phase-1.md`, `../findings.md`, `../verification-matrix.md`, and `../cleanup-backlog.md`.
- Six complete immutable reports: `../stage-reports/07-thread-safety.md` through `12-lifetimes.md`.
- Phase 2 kickoff: branch `bugfix/align-remote-join`, `HEAD` `a202d27a6923288846577b03cd9b305bfa6405db`, production tip under review `e72f3b0f489cfa12ea697e966d3c0f619e31d1f6`, merge base/master `4941b780f7ff5a46f742167d79338e3ab592a565`, clean worktree before artifact edits.

The Phase 1 human decision was reconciled into canonical tracking before dispatch. It authorizes investigation only. No production source, test, project, upstream NJClient, or human decision text was changed in Phase 2.

## Coverage and exclusions

| Stage | Actual coverage | Deliberate exclusions / handoff |
| --- | --- | --- |
| 07 Thread safety | 15 shared-state ownership rows across AudioHost, Scene, Timer, Station/LoopTake, NINJAM session/connection/coordinator/mailboxes, MIDI anchors | Cost to S08; post-ownership destruction to S12; unrelated GUI/VST/persistence/tooling |
| 08 Hot paths | Every callback named by `doc/realtime-audio.md`, active replacements, call chains/frequency/cost, logging and map traversal | Concurrency proof consumed from S07; supported diagnostics to S17; measurement remains future work |
| 09 Timing correctness | Local/free-run, empty/populated join, Continuous/Block/NoSync, late observation, replacement/discipline, disconnect/reconnect, unequal lengths/offsets, audio/MIDI | Arithmetic to S11; generic recovery to S10 |
| 10 State/failure paths | 24 transitions covering startup, connect, request/prompt, invalid/absent timing, physical loss/retry, disconnect, teardown and idempotence | Timing math to S09/S11; lifetime proof to S12 |
| 11 Numerics | Conversion/domain table for Timer/device/scene/remote/source/entity/grid values, rounding, sentinels, widths, sign, wrap and invalid values | Policy intent consumed from S09/S10 |
| 12 Lifetimes | Audio callback, controller/session/connection, connection guards, NJClient, scratch/delay buffers, mailboxes, Timer and shutdown ordering | Live-state race proof consumed from S07; non-timing VST/HUD/window lifetimes excluded |

The human-directed expansion into Scene/Station/LoopTake bloat was covered where timing/logging and callback resets are involved. HUD, VST3 parity, window persistence, tooling, unrelated MIDI routing, and non-timing resource ownership remain deliberately excluded from cleanup review. NJClient's header was inspected only as an upstream contract; it is not editable.

No unowned Phase 2 investigation gap remains. Runtime measurement, sanitizers, driver-specific shutdown validation, interactive NINJAM/manual scenarios, and the newly required regression tests are explicit verification obligations, not claims of completed evidence.

## Integrated system model

### End-to-end timing flow

The intended flow has four owners. The NINJAM job/network side runs `NJClient::Run`, validates a complete remote interval observation, and drives the coordinator's request/acknowledgement/follow-policy state. Scene currently serializes job/UI coordinator operations and command publication. AudioHost consumes a command at the top of an audio block, applies accepted Timer geometry and the common source map, then fans one mapped source correction through `Station -> LoopTake -> Loop` before ordinary advancement. Every take/loop keeps its own anchor and length, so common mapped elapsed time is never a shared cursor. `NoSync` is a policy/invalidation transition: it clears remote map/anchors and returns local timing to free-run.

That high-level coordinate model remains sound, but its handoffs are not yet safe. Live remote timing is currently pulled by unsupported NJClient getters from the callback instead of being published by the job owner. Commands are coherent as individual values but not complete substitutes for prior invalidation/replacement transitions. Invalidation does not reach every per-take generation gate. Physical loss is represented as “no snapshot,” not a session epoch transition. The callback also has an empty-scene path into job-owned state and a remote-buffer borrow that escapes the connection lifetime guard.

### Reconciled concurrency model

The complete ownership matrix is in `../00-scope-and-inventory.md`. Accepted-by-inspection components are:

- fixed-size observation, command, local-offset, receipt, MIDI-anchor and remote-grid snapshots use bounded atomic sequence publication when their serialized-writer contracts are obeyed; the exceptional callback remote-grid writer is rejected under F-022;
- station/take membership uses immutable published snapshots that pin objects for the block;
- AudioHost and LoopTake phase maps are audio-thread-confined;
- Scene `_sceneMutex` serializes current job/UI coordinator access;
- session connection identity is unpublished before waiting for active guarded users;
- full shutdown joins the Scene job thread and stops/closes RtAudio before controller/session destruction.

Non-atomic shared state requiring explicit human disposition:

| State | Current protection | Disposition proposed |
| --- | --- | --- |
| Coordinator/tracker/options | Job/UI serialized by `Scene::_sceneMutex` | Retain job ownership; remove exceptional callback read/reset (F-022) |
| AudioHost policy/common map | Audio-thread confinement | Retain |
| Per-take map geometry | Audio-thread confinement plus atomic anchors for observers | Retain until F-006 consolidation; compute common coordinate once |
| Timer musical transport | Audio-thread confinement | Retain and document; do not expose to job mutation |
| Connection/UI snapshot values | Job/UI mutexes | Retain off callback |
| NJClient internal timing | No supported cross-thread getter contract | Reject current use; job-owned immutable publication (F-021) |
| Borrowed `_outScratch` pointers | Guard ends before consumer finishes | Reject current use; scoped/preallocated consumption (F-033) |

### Hot-path risk register

| Path / frequency | Work observed | Assessment / owner |
| --- | --- | --- |
| Every audio block: AudioHost remote timing | Repeated connection-use acquire plus four unsupported NJClient getters | Merge blocker F-021; no lock-based fix |
| Every audio block while followed | Full Station/take restore traversal; repeated per-take 64-bit map calculation; atomic `shared_ptr`/weak locking | No correctness defect by itself; measure worst-case, consolidate calculation with F-006 |
| Every audio block: immutable membership | `atomic<shared_ptr>` loads/refcount traffic, not guaranteed lock-free by standard | Explicit residual platform cost pending measurement; no alternative synchronization scheme proposed yet |
| Empty-scene callback edge | Job-owned state read/mutation, raw hierarchy traversal, possible shared/string/container destruction | Must fix F-022 |
| Alignment diagnostics | Callback-reachable logging entry and large Station formatting implementation | Must fix F-034; bounded capture only when enabled, off-thread formatting |
| Remote stereo ingestion | Raw connection-owned pointers used after lifetime guard ends | Merge blocker F-033 |
| Normal Loop/LoopTake/Station/Trigger callbacks | No new blocking primitive proven in covered changed timing paths | Retain repository real-time rules; re-audit after fixes |

Intentional non-real-time locks/allocations proposed for acceptance are limited to lifecycle, job/UI snapshot, coordinator serialization, network operations, and off-thread logging. No audio-thread mutex, wait, I/O, dynamic formatting, heap ownership transfer, or final destruction is accepted. Disabled timing diagnostics must impose zero callback work; when enabled, only bounded preallocated value capture may occur on the callback.

### Timing-transition narrative

| Transition | Required invariant | Phase 2 conclusion |
| --- | --- | --- |
| Local/`NoSync` | Remote authority does not move Timer/entity cursors; local offsets remain independent | Correct in isolation |
| Empty join | Remote geometry may seed Timer without inventing an entity cursor | Plausible; focused first-record runtime/test gap remains |
| Populated join | Local and remote phase compared at one observation instant | Violated by F-026 under delayed initial processing |
| Different-tempo replacement | Replacement geometry is applied before dependent discipline | Violated when latest-wins coalesces transitions: F-024 |
| Continuous sync | Small correction, common mapped elapsed, per-entity modulo/offset preserved | Correct if prerequisite command state was consumed |
| Block sync | Same anchor/map model, potentially larger correction | Correct if replacement was consumed |
| Stay local / invalid / disconnect | Exactly one complete invalidation clears map, anchors, and gates without phase movement | Invalid/absent recovery incomplete (F-028); gate reset broken (F-025) |
| Physical loss / retry | Old authority ends and successful retry starts a fresh epoch | Not represented: F-027 |
| Late observation | Remote phase projects from anchored device time; local comparison uses matching snapshot | Ordinary discipline supported; initial join F-026, Timer tuple F-023, zero sentinel F-030 |
| Reconnect | No old command/map/gate/buffer survives; session-2 generation applies once | Violated by F-024/F-025/F-027/F-033 |

### Numerical-domain table

| Conversion/value | Domain/formula/rounding | Bound/result | Finding |
| --- | --- | --- | --- |
| Timer Tick/absolute | `phase + increment`, then loop quotient/remainder; `loopCount * masterLength + phase` | Currently uses Windows 32-bit `unsigned long`; Tick can undercount a near-max wrap and absolute time wraps about 24 h 51 min at 48 kHz | F-029 |
| Scene coordinate | Monotonic device-rate sample count | 64-bit; not wrapped Timer geometry | Retain |
| Remote source interval→device interval | nearest integer scaled by device/source rate | Positive plausibility required | Retain, but F-031 couples phase bound |
| Remote wrapped phase→device phase | currently nearest integer independently from length | Can equal converted length on source tail and manufacture wrap | F-031 |
| Observation age projection | device block minus device observation; Timer absolute now minus Timer observation | Zero is independently valid in both domains but treated as absent/fallback | F-030 |
| Local Timer observation tuple | length/count/phase/absolute read separately | Each atomic, no coherent version | F-023 |
| Remote elapsed→local source | whole intervals plus rounded remainder in integer arithmetic | 64-bit practical horizon accepted; per-entity modulo retained | Retain; compute once under F-006 |
| Phase correction | signed shortest delta; ongoing discipline bounded | Signed 64-bit; join may reach half interval | Retain existing boundary tests |
| Active grid boundary | `round(k * interval / divisions)` | Endpoint-preserving; distinct from local grain | Retain |
| Tempo/BPI/rate validity | positive/plausible finite inputs before casts | Non-finite BPM needs explicit total validation | F-032 |

`uint64_t` device/scene/source horizons and signed 64-bit correction horizons are accepted as practical residual risks for this merge; the 32-bit Timer truncation is not.

### Lifetime and shutdown constraints

- Full shutdown order is sound by inspection: stop/join Scene job work, then stop/close RtAudio before stopping/destroying the NINJAM controller/session.
- Live disconnect must unpublish the connection, prevent new guards, and wait for all guarded operations; current identity retirement follows this model.
- Every borrow from connection-owned storage must remain inside the guard. Current stereo pointers escape it (F-033).
- No fix may transfer last-reference destruction, connection/vector teardown, blocking wait, or allocation onto the callback.
- NJClient callback user data remains valid only while guarded `Run`/`AudioProc` ownership and the documented thread roles are obeyed (F-021).
- A replacement job-owned timing snapshot must retain source and observation timestamps so AudioHost can project it to its distinct device/Timer boundary anchors.

## Canonical findings added/changed

Phase 2 adds 14 canonical findings: five merge blockers, eight must-fix-before-merge items, and one follow-up. Existing F-006, F-009, and F-019 were enriched rather than duplicated.

| Canonical | Candidates | Severity | Integrated disposition |
| --- | --- | --- | --- |
| F-021 | S07-01, S08-01, S12-02 | merge blocker | Job-owned coherent remote observation; `AudioProc` only on callback |
| F-022 | S07-02, S08-04 | must fix | Publish empty edge; job-owned cleanup; bounded audio invalidation |
| F-023 | S07-03 | must fix | Versioned local Timer transport observation |
| F-024 | S09-01 | merge blocker | Complete latest state or bounded ordered commands with explicit coalescing |
| F-025 | S09-02 | merge blocker | Reset all consumer gates or use explicit session epoch |
| F-026 | S09-03 | must fix | Initial join anchors local/remote at one observation instant |
| F-027 | S10-01 | merge blocker | Explicit physical availability + fresh timing epoch |
| F-028 | S10-02 | must fix | Observation-independent deadline and validity-loss recovery |
| F-029 | S11-01 | must fix | 64-bit Timer Tick intermediates and absolute arithmetic end to end |
| F-030 | S11-02 | must fix | Explicit observation-anchor presence; zero remains valid |
| F-031 | S11-03 | must fix | Wrapped phase conversion preserves final-sample ordering/bound |
| F-032 | S11-04 | follow-up | Reject non-finite tempo before arithmetic/cast |
| F-033 | S12-01 | merge blocker | Scoped/preallocated remote stereo consumption under guard |
| F-034 | S08-02 | must fix | No callback logger; bounded enabled capture and off-thread formatting |

Changed existing findings:

- F-006 now includes S08-03: calculate the common mapped source coordinate once per followed block, while retaining entity-specific anchors/modulo. This enrichment is pending the Phase 2 gate; the original Phase 1 disposition remains accepted.
- F-009 now includes S07-04: current multi-context producers are serialized only by Scene's outer mutex; the integration owner must make publication ownership self-enforcing. This enrichment is pending the Phase 2 gate; the original Phase 1 disposition remains accepted.
- F-019 remains only the dead `AlignmentReceipt` deletion; F-034 separately owns live callback logging/diagnostic placement.

## Duplicates and contradictions resolved

- S07-01/S08-01/S12-02 are one root cause, F-021. Thread-contract violation is the controlling merge-blocker severity; cost and lifetime evidence are corroboration.
- S07-02/S08-04 are one callback-side empty-reset root cause, F-022.
- S08-03 is not a new correctness finding and does not delete live maps; it enriches accepted F-006.
- S07-04 is not a proven present data race because `Scene::_sceneMutex` serializes writers; it enriches F-009 rather than creating another cleanup.
- S08-02 is not duplicate F-019. F-019 removes a never-written receipt; F-034 handles reachable logging, zero-disabled-cost, and Station bloat.
- S09-01 and S09-02 remain separate blockers: ordered/complete publication does not make invalidation reach take gates, and gate reset does not preserve a lost replacement.
- S09-03 remains separate from S07-03/F-029/F-030: even with coherent wide anchors, the initial join currently chooses the wrong observation instant.
- S10-01 consumes F-024/F-025 but remains distinct: even a correct command channel cannot react to physical loss if loss is never represented.
- S11-01 and S07-03 are separate width and compound-coherence defects, intended to share one future Timer observation boundary.
- S12-01 is independent of NJClient getter access: the raw remote-audio buffer borrow can escape even if timing getters are removed.

No investigator contradiction remains. The protected timing concepts were retained in every disposition.

## Verification additions

The canonical matrix now includes exact obligations for F-021–F-034 plus seven focused scenario assertions. The two most important prerequisite tests before structural work are:

1. a production-faithful command-order test covering `Invalidate -> Replace` and `Replace -> Discipline` before one callback; and
2. a two-session reconnect test where session 1 exceeds generation 1, production invalidation is consumed, and session 2 begins at generation 1 across Timer plus real audio/MIDI takes.

Phase 2 integration executed the existing baseline after reading `.vscode/tasks.json` and `doc/build.md`: incremental `JammaLib_Tests` build succeeded through `invoke-msbuild.ps1`; 821/822 native tests passed with only the hardware-dependent `MidiDevice.OpensPreferredDeviceWhenAvailable` skipped. This does not cover any newly identified blocker. No manual NINJAM session, profiler, race sanitizer, ASan, page heap, or driver teardown evidence was executed.

## Gate questions

For each item, record **accept**, **reject with rationale**, or **defer with named owner and accepted risk** in `../decisions.md`. Acceptance authorizes later Phase 4 reconciliation, not source edits.

### Model and residual-risk gates

| Gate | Exact decision required |
| --- | --- |
| G2-1 | Accept/reject/defer the reconciled concurrency model and complete thread/ownership matrix, including retained audio confinement and immutable snapshot publication. |
| G2-2 | Accept/reject/defer each listed non-atomic shared state and its proposed owner/protection. Explicitly reject current NJClient internal timing access and escaped buffer borrow if F-021/F-033 are accepted. |
| G2-3 | Accept/reject/defer intentional non-real-time locks/allocations in lifecycle, job/UI snapshot, network, and off-thread logging paths. Confirm that no callback lock, wait, I/O, allocation, formatting, or final destruction is acceptable. |
| G2-4 | Accept/reject/defer the hot-path risk register, including the residual need to measure `atomic<shared_ptr>`/refcount and hierarchy traversal at maximum configured size. |
| G2-5 | Accept/reject/defer the timing-transition narrative for local, empty/populated join, all follow policies, invalid/loss/disconnect, late observation, and reconnect. |
| G2-6 | Accept/reject/defer the numerical-domain table, including practical acceptance of 64-bit horizons and rejection of the current 32-bit Timer truncation/zero sentinel/early-wrap conversion. |
| G2-7 | Accept/reject/defer the lifetime/shutdown constraints and the remaining driver/sanitizer evidence gaps. |
| G2-8 | Accept/reject/defer the focused verification scenarios and the rule that one or two high-quality prerequisite regressions pass before every major Phase 4 refactor. |
| G2-9 | Accept/reject/defer the declared scope exclusions: retained HUD, VST3, window/tooling, unrelated MIDI and non-timing resources; accept their later Phase 3/4 coverage rather than Phase 2 expansion. |

### Canonical finding gates

| Finding | Exact decision required |
| --- | --- |
| F-006 Phase 2 enrichment | Accept computing the common mapped source coordinate once per block within the already-approved single-owner consolidation; reject; or defer with measured callback-cost rationale. |
| F-009 Phase 2 enrichment | Accept one explicit/self-enforcing command producer contract plus producer-overlap verification within the approved owner move; reject; or defer with outer-mutex dependency risk. |
| F-021 | Accept removing every non-`AudioProc` NJClient callback call via coherent timestamped job publication; reject; or defer with explicit upstream-thread/RT risk. |
| F-022 | Accept moving empty-scene timing cleanup off callback and using published ownership; reject; or defer with race/destruction risk. |
| F-023 | Accept one coherent versioned Timer transport observation in an existing owner/value contract; reject; or defer with mixed-tuple risk. |
| F-024 | Accept preserving non-substitutable transitions with complete-state or bounded ordered semantics after prerequisite test; reject; or defer with command-loss risk. |
| F-025 | Accept coherent session epoch/generation invalidation for Timer and every take after prerequisite test; reject; or defer with reconnect split-authority risk. |
| F-026 | Accept observation-instant anchoring for initial join; reject; or defer with scheduler-dependent join risk. |
| F-027 | Accept explicit physical availability and fresh retry epoch; reject; or defer with stale-authority risk. |
| F-028 | Accept observation-independent deadlines and explicit validity-loss recovery; reject; or defer with hung/stale-state risk. |
| F-029 | Accept 64-bit Timer Tick intermediates and absolute arithmetic; reject; or defer with near-max Tick undercount and ~24 h 51 min absolute-wrap risk at 48 kHz. |
| F-030 | Accept explicit anchor presence separate from valid sample zero; reject; or defer with first-block ambiguity. |
| F-031 | Accept bounded wrapped-phase conversion that cannot manufacture early wraps; reject; or defer with cross-rate boundary risk. |
| F-032 | Accept finite-input hardening; reject; or defer as a named follow-up risk. |
| F-033 | Accept scoped/preallocated remote audio consumption under the connection guard; reject; or defer with callback UAF risk. |
| F-034 | Accept eliminating callback logging and moving bounded enabled formatting off-thread; reject; or defer against the explicit zero-cost logging mandate. |

## Human outcome

Pending. Phase 2 is stopped at the human gate. Phase 3 investigation and all cleanup/source/test implementation remain unauthorized until the reviewer records G2-1–G2-9, the F-006/F-009 Phase 2 enrichment decisions, and F-021–F-034 outcomes in `../decisions.md`.

After accepting or otherwise deciding this packet, use the prompt below:

> Phase 2 of `doc/merge-readiness-review/merge-readiness-plan.md` is complete. I have recorded all G2-1–G2-9, F-006/F-009 Phase 2 enrichment, and F-021–F-034 decisions, residual-risk acceptances, and notes in `doc/merge-readiness-review/decisions.md`. Execute Phase 3 completely, using `phase-packets/phase-1.md`, `phase-packets/phase-2.md`, `findings.md`, `verification-matrix.md`, `00-scope-and-inventory.md`, and `cleanup-backlog.md`; preserve the protected timing glossary and the timing-only cleanup scope; run bounded non-colliding subagents; produce stages 13–18 and `phase-packets/phase-3.md`; stop at the next human gate without implementing cleanup.
