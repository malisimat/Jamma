# Stage 19 — Cross-review reconciliation

## Assignment

- **Primary ownership:** compare the canonical findings and all approved phase packets for duplicate root causes, contradictory evidence or dispositions, incompatible timing models, missing inventory coverage, and accidental erosion of protected concepts.
- **Mode and boundary:** bounded planning/design reconciliation only. This stage did not widen the review with a speculative source sweep, choose batch priority, implement cleanup, edit tests or production code, run build/test/cleanup verification, or alter canonical findings, decisions, backlog, verification, inventory, plan, or phase packets.
- **Required inputs read:** `AGENTS.md`; `merge-readiness-plan.md`; `phase-4-reconciliation-and-merge-evidence.md`; `00-scope-and-inventory.md`; `phase-packets/phase-1.md` through `phase-3.md`; `findings.md`; `decisions.md`; `cleanup-backlog.md`; `verification-matrix.md`; and `doc/loop-alignment-and-ninjam-sync.md`.
- **Baseline:** branch `bugfix/align-remote-join`, review-artifact `HEAD` `8cd8725bd4447d4125f671b9524f95e277396939`, production tip `e72f3b0f489cfa12ea697e966d3c0f619e31d1f6`, and merge base/current `master` tip `4941b780f7ff5a46f742167d79338e3ab592a565`. The paths after the production tip and through `HEAD` are review artifacts only; no later production change was found by `git diff --name-only e72f3b0..HEAD`.
- **Output ownership:** this report is the only file written. It supplies reconciliation decisions and exceptions to the Phase 4 integrator; it does not itself change a canonical disposition.

## Coverage

### Evidence and queries used

Read-only work used numbered `Get-Content`, `rg --files`, `git status --short`, `git log --oneline`, `git show`, `git diff --name-only`, `git diff --stat`, `git rev-parse`, and `git merge-base`. The comparison covered every F-001–F-050 heading, every phase packet's coverage/model/findings/duplicate/verification/gate/outcome sections, every current human decision, the full protected glossary, the Phase 2 scenario assertions, the Phase 3 P1–P10 prerequisites, and the current empty backlog state.

No build, native test, runtime/manual scenario, profiler, sanitizer, race tool, or cleanup verification was run. This is required by the Stage 19 design-only boundary, not a claim that existing verification is sufficient. The latest executed baseline remains the Phase 2 incremental test build and 821/822 native result, with the hardware-dependent MIDI test skipped and none of the new blockers covered (`verification-matrix.md:3`; `phase-packets/phase-2.md:157`–`:162`).

### Inventory and phase coverage reconciliation

- Phase 1 accounts for all 199 production diff rows, 123 commits, 61 changed headers, and the complete 14,691-addition/1,507-deletion partition (`phase-packets/phase-1.md:15`–`:34`; `00-scope-and-inventory.md:49`–`:89`).
- Phase 2 covers the timing thread, callback, transition, numeric, and lifetime surfaces with explicit handoffs rather than claiming runtime verification (`phase-packets/phase-2.md:14`–`:25`).
- Phase 3 covers timing maintainability, production-faithful prerequisites, timing documentation, compatibility, observability, and input robustness in two non-colliding waves (`phase-packets/phase-3.md:13`–`:24`).
- The retained HUD, VST3, window/tooling, unrelated MIDI/resources, and upstream NJClient surfaces remain in the merge but outside **cleanup**. That is an intentional human-accepted exception, not a missing Stage 19 inventory slice (`decisions.md:7`; `00-scope-and-inventory.md:171`–`:177`). Stage 20/21 may map their merge/regression or hygiene obligations, but no Phase 4 cleanup batch may edit them merely to close an old follow-up.
- Generic JSON file/depth/element limits and unverifiable upstream NJClient parser/user/channel/work budgets are accepted residual exclusions rather than unowned timing-cleanup coverage (`verification-matrix.md:58`–`:62`; `phase-packets/phase-3.md:115`–`:122`; `decisions.md:16`).

### Coverage exceptions requiring explicit final evidence, not more investigation

| Exception | Reconciled status | Later evidence owner |
| --- | --- | --- |
| No production-faithful AudioHost seam and no deterministic fault/buffer-borrow seam | Accepted findings F-038–F-041 supply prerequisites; this is a verification gap, not a missing review stage | Proposed prerequisite-test batches, then implementation batches |
| No Phase 3/4 build, runtime, profiler, sanitizer, hostile-file, or manual evidence | Deliberately not executed before batch approval | Stage 21 execution section after cleanup and per-batch reviews |
| Retained HUD/VST3/window/tooling and unrelated resources | Accepted out of cleanup by G3-1; still require final merge-hygiene/regression accounting where applicable | Stages 20/21 and final merge brief |
| F-020's ten unregistered HUD textures | Earlier accepted conditional deletion conflicts with the later timing-only cleanup boundary; G3-1 controls, so do **not** batch this resource deletion | Record as an accepted no-change/out-of-scope residual unless a later human explicitly widens scope |
| Generic JSON and unavailable upstream bounds | Accepted residual, with no broad security-certification claim | Final risk statement; future non-timing owner if desired |

No other inventory partition is unrepresented by a phase packet, an accepted exclusion, or a later verification obligation.

## System understanding

The reconciled target has four distinct operational owners. The Jamma-owned NINJAM integration path owns physical availability/session epoch and publishes one complete desired remote transport value. Scene presents prompts and forwards values but does not construct timing authority. AudioHost compares desired with last-applied state at the audio-block boundary and owns Timer replacement, common-map lifecycle, and one common mapped source-coordinate calculation. `Station -> LoopTake -> Loop` performs explicit neutral fan-out; each entity retains its own length, phase anchor, audio cursor, MIDI event cursor, and automation origin.

This is one authority pipeline, not one coordinate. Remote master phase is wrapped remote geometry; Timer absolute position, device-audio sample position, and the monotonic scene coordinate are different rulers; mapped elapsed time is a common amount, not a shared cursor. Each entity applies the common amount modulo its own length. Follow policy selects `ContinuousSync`, `BlockSync`, or `NoSync`, but is not itself a clock or coordinate (`00-scope-and-inventory.md:17`–`:41`; `phase-packets/phase-3.md:26`–`:34`).

G3-5 strengthens, rather than changes, that protected model. Recovery must preserve the different relative play positions of `M`, `2M`, and `3M` loops and must never reduce all entities to master modulo (`decisions.md:11`). The design reference gives the controlling rule: every entity receives the same mapped elapsed amount and wraps by its own `L_i`, with anchors retained through the follow session and invalidated only before an independent session (`doc/loop-alignment-and-ninjam-sync.md:24`–`:32`, `:47`–`:81`, `:83`–`:102`). Therefore:

1. F-006 may centralize only the **common source-coordinate calculation** in AudioHost; it may not remove per-entity anchors, lengths, or modulo.
2. F-024/F-025/F-027 may reset authority, map, and generation/epoch gates on `NoSync` or reconnect, but invalidation itself must not move local cursors.
3. F-044's supported signed-offset migration must retain signed/turn information long enough to apply an equivalent common correction to each entity. Normalizing only modulo `M` is not behaviorally equivalent for `2M`/`3M` entities.
4. F-005 may remove NINJAM policy types from low-level engine APIs only after AudioHost has interpreted the policy into neutral correction/reset operations; it may not merge the three policies.

The current design document still describes the pre-cleanup command event path and generation semantics at `doc/loop-alignment-and-ninjam-sync.md:109`–`:133` and `:156`–`:172`. F-024 and F-042 already classify those statements as implementation/documentation work. They are not an approved competing model: the human-approved complete desired-state model in `findings.md:290`–`:301` and Phase 3 packet `:76`–`:81` controls.

## Candidate findings

Stage 19 found no new production defect and recommends no new F-ID. The following are canonical-artifact reconciliation actions for the Phase 4 integrator.

### S19-01 — Restore one complete, append-only human-decision record

- **Classification:** canonical provenance correction, not a production finding.
- **Evidence:** current `decisions.md:3` says it contains all human decisions following Phase 3 but now contains only G3 and Phase 3 finding text (`:5`–`:35`). Commit `8cd8725` replaced 145 lines of prior Phase 1/2 human decisions with the Phase 3 text; the prior human text remains recoverable from parent review commit `70e48a5`. Current canonical findings still link to removed headings, for example F-001 at `findings.md:15`, F-005 at `:64`, F-021 at `:264`, and F-024 at `:301`.
- **Required reconciliation:** preserve the new G3 text verbatim while restoring the prior Phase 1/2 human decisions verbatim (or an equally audit-complete canonical ledger that links immutable historical text). Do not silently rewrite human rationale. Repair canonical links so every accepted/rejected disposition resolves to current evidence.
- **Why it matters:** Phase 4 cannot prove every finding is human-decided from the canonical ledger while earlier decisions and link targets are absent, even though Phase 1/2 packets summarize their outcomes.

### S19-02 — Apply the Phase 3 gate to canonical statuses

- **Classification:** canonical status reconciliation, not a new finding.
- **Evidence:** `findings.md:436`, `:448`, `:460`, `:472`, `:484`, `:496`, `:508`, `:520`, `:532`, `:544`, `:556`, `:568`, `:580`, `:592`, `:604`, and `:616` still say “pending Phase 3 gate.” The packet likewise still says “Pending” at `phase-packets/phase-3.md:159`–`:165`. Current decisions accept F-035–F-048 subject to G3-5–G3-8 details and reject F-049/F-050 (`decisions.md:27`–`:35`).
- **Required reconciliation:** update the canonical records and Phase 3 outcome without allocating new IDs. F-044 is accepted as mandatory behavior-preserving long-loop support; F-045 resolves as accepted zero-default downgrade behavior with no schema guard; F-046 requires full remote BPI while local disconnected BPI remains pure local inference; F-047/F-048 remain surgical; F-049/F-050 are rejected/no-change accepted risks.

### S19-03 — Remove rejected and out-of-scope work from the proposed cleanup set without erasing risk

- **Classification:** backlog/verification reconciliation handoff, not a new finding.
- **Evidence:** verification rows still describe F-049/F-050 remedial tests as though the fixes were candidates (`verification-matrix.md:55`–`:56`, `:98`), while G3-9 and the explicit finding decision reject both changes (`decisions.md:15`, `:35`). F-020's resource deletion remains accepted at `findings.md:241`–`:251`, but G3-1 later excludes unrelated resources from cleanup (`decisions.md:7`).
- **Required reconciliation:** no proposed batch may implement F-020, F-049, or F-050. Retain their evidence and decisions as explicit accepted residual/no-change records. Verification may confirm no accidental collateral change, but must not smuggle a rejected redaction/path policy or out-of-scope HUD deletion into another batch.

### S19-04 — Keep test findings as prerequisites, not duplicate production findings

- **Classification:** deduplication rule.
- **Evidence:** F-038/F-039 prove that current test models encode or hide F-024/F-025/F-030 behavior (`findings.md:462`–`:484`); F-040 supplies concurrency proof for F-021/F-023 (`:486`–`:496`); F-041 supplies controllable seams for F-027/F-028/F-033 (`:498`–`:508`). The verification matrix requires only one or two closest prerequisites before each major refactor and rejects an umbrella P1–P10 batch (`verification-matrix.md:74`–`:91`).
- **Required reconciliation:** preserve all four test IDs and their independent evidence, but associate each passing prerequisite commit with the production refactor it gates. Do not count a prerequisite test as fixing the production defect, and do not merge all P1–P10 into one batch.

### Canonical-ID merge/split table

Every F-001–F-050 ID is represented below. “Keep separate” means no canonical merge; related IDs may still share a narrowly justified batch only when Stage 20 proves one invariant and inseparable verification.

| Canonical IDs | Root-cause relationship | Reconciled canonical treatment |
| --- | --- | --- |
| F-001/F-020 | HUD scope versus conditional dead-resource deletion | Keep separate. F-001 is decided retain-scope; later G3-1 removes F-020 from timing cleanup despite its earlier conditional acceptance. No resource batch. |
| F-002 | VST3 scope | Keep as decided retain-scope record; no timing cleanup. |
| F-003 | Window-persistence scope | Keep as decided retain-scope record; no timing cleanup. |
| F-004 | Build/tooling scope | Keep as decided retain-scope record; Stage 21 validates its use but does not redesign it. |
| F-005/F-035 | Neutral engine timing boundary versus duplicated direct cursor shift | Keep separate. F-005 owns policy translation/reset/correction API; F-035 is a private mechanical primitive after caller semantics are stable. |
| F-006/F-018 | Live common-map ownership versus one dead duplicated map field | Keep separate. F-018 can remove only unread `SourcePhaseAtOrigin`; F-006 later replaces live copied geometry while retaining entity anchors/modulo. |
| F-007 | Value-only timing contract placement | Keep; no new class and no protected-term collapse. |
| F-008/F-012 | Local mailbox ownership versus non-template header bodies | Keep separate; coordinate the file move once, but ownership and compilation-coupling evidence are different. |
| F-009/F-024/F-038/F-039 | Producer ownership, complete desired-state semantics, production-faithful seam, and rejected-expectation rewrite | Keep all four IDs. F-009 and F-024 are one dependency-ordered production refactor; F-038/F-039 are prerequisite evidence, not duplicate fixes. |
| F-010/F-011/F-013/F-014/F-015/F-016/F-017 | Naming/convention groups | Keep IDs because each protects a different semantic domain. Apply overlapping F-011/F-017 export member renames once; F-017 remains conditional on retaining the helper. |
| F-019/F-034/F-047 | Dead receipt, callback logging responsibility, and missing diagnostic correlation | Keep separate. F-019 deletes only unwritten state; F-034 moves/bounds presentation; F-047 adds the minimum epoch/version/reason signal. One implementation batch is permissible only if it remains net-slim and P10 gates it. |
| F-021/F-023/F-040 | Remote observation publication, local Timer publication, and deterministic mailbox proof | Keep separate. F-021 and F-023 publish different owners and clock-domain tuples; F-040 is their prerequisite test technique. |
| F-022 | Empty-scene callback ownership | Keep distinct from F-027/F-028; it is callback/job ownership and destruction, not network loss. |
| F-025 | Per-consumer reconnect gate reset | Keep distinct from F-024 and F-027. Complete-state publication and physical epoch reporting do not themselves reset every take's gate. |
| F-026 | Initial-observation instant | Keep distinct from tuple coherence/width/presence; it is the scheduler-dependent join comparison instant. |
| F-027/F-028/F-041 | Physical lifecycle, validity/deadline recovery, and controllable test seams | Keep all three. F-027 owns availability/epoch, F-028 owns observation-independent invalid/deadline recovery, F-041 gates both plus F-033. |
| F-029/F-030/F-031 | Width, presence sentinel, and source-tail conversion | Keep separate numerical contracts; they may share P6 but one fix does not imply the others. |
| F-032/F-048 | Remote/request finite plausibility versus local seed-policy conversion bounds | Keep separate. Both are surgical validation, but they protect different producers and authority directions. |
| F-033 | Guard-scoped remote audio consumption | Keep as an independent lifetime blocker; timing-publication work does not extend the buffer borrow. |
| F-036 | Remote proposal identity | Keep as a small existing-owner simplification after geometry vocabulary stabilizes. |
| F-037 | Obsolete uncompiled test deletion | Keep as a direct deletion after confirming replacement suites remain registered. |
| F-042/F-043 | Integration guide versus historical MIDI investigation | Keep separate documents/status contracts; update alongside the implementation that makes each statement true. |
| F-044/F-045/F-046 | Signed-offset migration, downgrade behavior, and remote-BPI producer compatibility | Keep separate. F-044 is mandatory long-loop recovery; F-045 is an accepted zero-default downgrade; F-046 removes only partial **remote** BPI fallback and preserves pure local inference when disconnected. |
| F-049/F-050 | Password serialization versus work-directory authority | Keep separate rejected findings as two explicit accepted risks. Neither may enter cleanup or be represented as security-certified. |

### Contradiction resolutions

| Apparent contradiction | Controlling resolution |
| --- | --- |
| Phase 2 F-024 allowed “complete latest state or bounded ordered commands” (`phase-packets/phase-2.md:121`–`:132`) | Superseded by the Phase 2 human outcome and canonical replacement: only latest complete desired state is approved; no ordered queue, standalone invalidation, or delta publication (`phase-packets/phase-2.md:203`–`:205`; `findings.md:290`–`:301`). |
| Current design guide says one timing command/generation is consumed/applied (`doc/loop-alignment-and-ninjam-sync.md:109`–`:133`, `:166`–`:172`) | This documents current/pre-cleanup mechanics. F-024's desired-versus-applied state and F-042's documentation correction are the approved target. Update docs with the implementation batch per G3-4; do not preserve obsolete command semantics for documentation compatibility. |
| F-006 “one common map owner” could be read as one shared cursor | Rejected reading. AudioHost owns one common calculation; every entity keeps its own `L_i`, anchor, cursor, and modulo. The glossary and G3-5 are non-negotiable (`00-scope-and-inventory.md:25`–`:33`; `decisions.md:11`). |
| Current negative offset normalization treats `-0.25` as `+0.75` modulo master while F-044 shows that differs for `2M` loops (`findings.md:534`–`:543`) | G3-5 selects behavior-preserving support. Preserve signed/turn information or an equivalent common correction through per-entity restore; never normalize away a master turn before applying it to long loops. |
| F-046 removes BPI deduction, while G3-7 mentions local BPI inference (`decisions.md:13`, `:33`) | Full BPI is mandatory whenever remote timing authority exists. Pure local inference from local master length/local grain remains allowed only when disconnected; it is not a fallback for a partial remote observation. |
| Phase 3 classified F-049/F-050 as must-fix (`phase-packets/phase-3.md:69`–`:72`) | G3-9 explicitly rejects both. Keep the defects/risks visible, mark no-change/rejected, and exclude them from batches and “must-fix remaining” counts. |
| F-045 proposed document-or-guard, while G3-6 accepts silent downgrade (`findings.md:546`–`:556`; `decisions.md:12`, `:32`) | No schema guard is required. Missing state defaults to zero on load; a focused regression may prove that behavior without adding schema complexity. |
| Earlier F-020 conditional deletion versus later timing-only scope | G3-1 is later and explicit: unrelated resources remain outside cleanup. Retain the files for this effort and record the dead-resource residual rather than widening a batch. |
| Canonical findings say Phase 3 outcomes are pending | Administrative staleness only. `decisions.md:22`–`:35` controls; canonical reconciliation must update statuses before backlog approval. |

## Handoffs

- **Phase 4 integrator:** restore audit-complete decision provenance, update Phase 3's human outcome and F-035–F-050 statuses, reconcile F-020/F-049/F-050 out of the cleanup set, retain explicit accepted risks, and do not assign a new canonical ID for S19-01–S19-04.
- **Stage 20:** map accepted cleanup candidates, not all raw findings. Treat F-038–F-041 as prerequisite-test work, F-001–F-004 as decided scope, F-020/F-049/F-050 as no-change, and F-045 as accepted behavior/default verification. Make G3-5's `M`/`2M`/`3M` per-loop recovery an explicit dependency and prohibited-collateral rule for every map/offset/epoch batch.
- **Stage 21 design:** plan an audit that distinguishes review-artifact commits after `e72f3b0` from production cleanup, validates project membership for test deletion/addition, and checks that no rejected or out-of-scope file changes enter the final diff. Do not execute the checks before the batch gate.
- **Cleanup backlog owner:** retain one or two closest focused prerequisites per major refactor, using the Phase 4 passing-first default or characterization-first exception as applicable. The initial architecture work must be gated by P1/P2 and the epoch/map work by P3/P4 as applicable; later publication, recovery, lifetime, and diagnostics work use P5/P6, P7/P8, P9, and P10 narrowly rather than as one umbrella batch (`verification-matrix.md:74`–`:91`).
- **Final risk statement:** explicitly list F-020 no-change/out-of-scope resource residue, F-045 accepted downgrade loss/zero default, rejected F-049 credential export and F-050 work-directory behavior, generic JSON resource limits, unavailable upstream NJClient bounds, and any verification tooling not executed. Do not call the branch security-certified.

## Uncertainties

- The current canonical decision file lost prior human prose, but the prose is recoverable from commit `70e48a5` and its deletion is visible in `8cd8725`. The integrator must decide the least invasive audit-complete representation; this stage does not edit human text.
- F-017 remains conditional on whether the disabled export-latency helper survives F-021-related adapter cleanup. Stage 20 should inspect the accepted implementation boundary and either attach the semantic rename to the retaining batch or record the conditional finding as resolved by removal; no speculative decision is needed here.
- Exact batch boundaries among the tightly coupled F-009/F-024/F-025/F-027 architecture changes depend on Stage 20's file/interface impact matrix. Their canonical IDs must remain separate even if one approved batch is the smallest safe implementation unit.
- The exact migration representation for F-044 is deliberately not chosen here. The outcome is not uncertain: `M`/`2M`/`3M` relative phase must survive. A batch design may choose retained signed turns or an equivalent common-correction representation only after P3/P4-style tests pass.
- No runtime evidence demonstrates the accepted target. This report reconciles evidence and decisions only.

### Unresolved questions for the proposed-batch human gate

1. Approve the canonical reconciliation that keeps F-001–F-050 as distinct IDs, with the dependency associations in this report rather than further merges.
2. Approve the controlling disposition set: F-020 excluded by G3-1; F-044 mandatory long-loop-preserving support; F-045 accepted zero-default downgrade; F-046 full remote BPI with disconnected local inference; F-049/F-050 rejected/no-change risks.
3. Approve restoration of complete Phase 1–3 decision provenance and canonical status/link repair without altering human rationale.
4. Approve the Stage 20 batch boundaries and prerequisite assignments only after they encode protected per-loop state, prohibited collateral, rollback, and exact verification; Stage 19 does not pre-approve a combined architecture batch.

No unresolved timing-model or product-policy question remains inside Stage 19. The questions above are approval of the reconciled record and proposed execution boundaries, not invitations to choose a different timing model.

## Conclusion

Cross-review reconciliation is complete. The three phase packets and F-001–F-050 describe one compatible target once later human decisions are applied: complete desired remote authority, explicit epoch/loss handling, AudioHost-owned common mapping, neutral engine operations, and entity-owned anchors/cursors/modulo. No canonical IDs should be merged or split further, and no new production finding is warranted.

The protected glossary survives intact. In particular, G3-5 makes `M`/`2M`/`3M` recovery a hard acceptance condition: common mapped elapsed time may be computed once, but it must be applied against each entity's own anchor and length; `NoSync`/epoch invalidation cannot move cursors; remote BPI cannot replace local disconnected inference; and follow policy remains separate from every coordinate.

Before proposed batches can be approved, the integrator must reconcile stale canonical statuses and decision provenance, remove F-020/F-049/F-050 from the cleanup set while retaining their risk records, and preserve test findings as narrow prerequisites. This report performs no cleanup, verification, batch review, or merge-brief work and stops at the Stage 19 planning handoff.
