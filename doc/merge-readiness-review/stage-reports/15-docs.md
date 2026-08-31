# Stage 15 — Docs and comments

## Assignment

- Stage: Phase 3, Stage 15 — Docs and comments.
- Primary ownership: statement-level truthfulness and protected-glossary consistency of changed documentation, source comments, UI text, log text, and test descriptions in the remote timing/NINJAM sync scope.
- Explicit exclusions: runtime logging cost/placement (Stages 8/17 and accepted F-034); production-symbol naming (Stage 4 and accepted F-010–F-017); test strength and missing executable contracts (Stage 14); implementation of any correction; upstream NJClient; and retained HUD, VST3 parity, window/tooling, export-latency behavior, or unrelated MIDI work except where a timing statement directly names one of their boundaries.
- Required inputs read: `AGENTS.md`; `merge-readiness-plan.md`; `phase-3-maintainability-and-contracts.md`; `decisions.md`; `00-scope-and-inventory.md`; `phase-packets/phase-1.md`; `phase-packets/phase-2.md`; `findings.md`; `verification-matrix.md`; `cleanup-backlog.md`; `stage-reports/04-vocabulary.md`; `doc/loop-alignment-and-ninjam-sync.md`; `doc/ninjam.md`; `doc/ninjam-midi-quantisation-investigation.md`; and the relevant current source/tests.
- Comparison: production changes at `master...e72f3b0f489cfa12ea697e966d3c0f619e31d1f6`; Phase 3 review baseline at `b6a8853`. This report is the only file written. No production code, test, existing documentation, canonical artifact, decision, or upstream file was edited, and no commit was made.

## Coverage

Actual coverage:

- Read all three required timing documents statement by statement and traced every operational assertion about observation ownership, command publication/consumption, map/anchor lifecycle, MIDI cursor/automation correction, remote-grid publication, authoritative BPI, and `NoSync`/disconnect to current code or an approved Phase 1/2 decision.
- Screened all eight documentation paths changed by `master...e72f3b0`; deep review stayed with the three timing documents. `doc/README.md`, `doc/build.md`, `doc/midi-trigger-mapping.md`, `doc/overlay-controls.md`, and `doc/vscode-tasks.example.json` were excluded after their changed content proved to be link/build/UI/tooling material outside timing cleanup.
- Screened added/changed timing comments and strings in `AudioHost.*`, `Scene.*`, `Station.*`, `LoopTake.*`, `Loop.*`, `Timer.*`, `ninjam/*`, and the related MIDI/Quantiser seams. Deep traces covered the command/observation mailboxes, invalidation, sync-map restore, automation correction, remote tempo prompt, and alignment logger.
- Screened test names and explanatory comments in all changed `test/JammaLib_Tests/src/ninjam/*` files plus `LoopTakeTiming_Tests.cpp`, `Timer_Tests.cpp`, `Quantisation_Tests.cpp`, `MidiLoop_Tests.cpp`, and `MidiQuantisation_Tests.cpp`. Test-quality judgments and new-test design are handed to Stage 14.
- Relevant history was traced with `git log`, `git blame`, and `git show`: `4299c89` (early integration guide), `d6fdb88` (command/comments and zero-anchor convention), `b41b5a8` (observation anchor), `b9619b9` (model integration tests), `e0f669c` (sync-map lifecycle docs), `92e8907` (active-grid docs), `9a8bc91` (MIDI quantisation TDD plan), and `e72f3b0` (later remote-grid/BPI implementation).

Commands/queries used included:

- `git status --short`, `git log --oneline`, `git diff --name-status master...e72f3b0 -- doc ...`, and focused `git diff --unified=0` scans of changed comments/strings.
- `rg -n` searches for timing/glossary terms, comments, UI strings, log labels, test names, mailbox publication, invalidation, sync-map restore, remote-grid publication, and direct remote-boundary evaluation.
- Line-numbered `Get-Content` reads of the governing artifacts, three timing documents, current source seams, and the relevant test harnesses.
- `git blame -L`, `git log -- <paths>`, and `git show --stat e72f3b0` to distinguish stale intermediate prose from current behavior.

No build or test was run: this is an investigation-only stage and changes no executable artifact. Runtime log reachability/volume was not re-investigated beyond consuming F-034; no current statement was declared true merely because a textual match existed.

## System understanding

The protected model remains coherent. Remote interval/phase observations, local Timer absolute position, the monotonic device/scene ruler, remote master phase, common mapped local-source progress, and each entity's wrapped phase are different coordinates. AudioHost is the audio-boundary integrator: it applies accepted remote Timer geometry, owns the common sync map, and restores each entity through its own anchor/modulo. The follow policy selects authority behavior; it is not a clock. `NoSync` is a complete authority/invalidation transition, not a phase command.

Current implementation does not yet meet that whole contract. The callback obtains live timing through `NinjamController::GetLiveTiming` and publishes it at `AudioHost.cpp:437`–`:453`, contrary to accepted F-021's job-owned/internal-`AudioProc` publication direction. The command mailbox is coherent for one value but latest-command coalescing can erase non-substitutable transitions (`NinjamAudioTimingCommand.h:53`–`:119`, F-024). AudioHost clears the common map/anchors for invalidation at `AudioHost.cpp:174`–`:185`, but skips the Station/take `ApplyTimingCommand` fan-out when sync is disabled at `:298`–`:302`, so `LoopTake.cpp:524`–`:527` does not reset every consumer gate (F-025). Physical loss and invalid timing also lack the complete epoch/authority transitions required by F-027/F-028.

The MIDI remote-grid path changed after its TDD plan was written. Commit `e72f3b0` now publishes `RemoteTransportGeometry` through `Scene.cpp:443`–`:449`, `Quantiser.cpp:234`–`:247`, and `LoopTake.cpp:2550` onward; remote MIDI events use direct `origin + round(k * interval / divisions)` boundaries at `MidiQuantisation.cpp:214`–`:272`; and authoritative nonzero server BPI is retained at `NinjamTimingCoordinator.cpp:291`–`:313` with a regression at `NinjamTimingCoordinator_Tests.cpp:150`–`:161`. The TDD document still describes the immediately preceding state as current.

## Candidate findings

### S15-01 — The integration guide reverses timing and MIDI thread ownership

- Stage / reviewer: Stage 15 — Docs and comments.
- Scope reviewed / exclusions: `doc/ninjam.md` timing, MIDI/automation, and tempo-choice statements; runtime correctness findings are consumed rather than duplicated.
- Severity: must fix before merge.
- Evidence: `doc/ninjam.md:12`–`:17` says job-thread observations carry remote timing, while current observations are constructed/published by the callback at `AudioHost.cpp:437`–`:453`; accepted F-021 requires a different job-owned/internal-`AudioProc` handoff. `doc/ninjam.md:31`–`:44` says MIDI is “re-anchored ... at each wrap” and `_midiAnchorCorrection` is written on the job thread by `RepositionFromAnchor`; no such current method exists, the active map restores cursors every followed block at `AudioHost.cpp:456`–`:459`, and the correction is changed from callback-owned cursor moves at `LoopTake.cpp:518`–`:559` and `:681`–`:688`. `doc/ninjam.md:58`–`:61` says `Stay local` publishes nothing, while rejection sets `InvalidatePendingCorrections` at `NinjamTimingCoordinator.cpp:343`–`:352` and Scene publishes an `Invalidate` at `Scene.cpp:460`–`:481`. The stale MIDI wording entered in `4299c89`; the observation wording entered in `92e8907`; the sync-map/invalidation model evolved through `d6fdb88`, `b41b5a8`, and `e0f669c`.
- Additional statement defect: `doc/ninjam.md:7` describes an interval as “BPM × BPI beats.” An interval contains BPI beats played at BPM; `IntervalSampsFromTempo` computes its duration as `60 * sampleRate * BPI / BPM` at `NinjamTiming.h:126`–`:138`. The current wording multiplies incompatible concepts and obscures which value is authoritative.
- Why it matters: the guide assigns mutable timing state to the wrong thread, describes a retained session anchor as a per-wrap re-anchor, and hides the required `NoSync` transition. Those are precisely the ownership/lifecycle distinctions that future real-time changes must preserve.
- Recommended disposition: correct the guide only after the accepted F-021/F-024/F-025/F-027/F-028 contract is implemented; until then, explicitly distinguish “current implementation” from “approved target.” Describe the automation value as an automation global-sample origin plus audio-thread-owned correction, not a MIDI event cursor or job-thread re-anchor.
- Protected timing concepts affected: remote timing observation, audio ownership, source/scene anchor, MIDI event cursor, automation origin, follow policy, `NoSync`.
- Verification: statement/source audit after cleanup; static callback/job ownership trace; reconnect and `Stay local` manual trace.
- Human decision: pending.

### S15-02 — Command and sync-map prose promises guarantees the latest-command path does not provide

- Stage / reviewer: Stage 15 — Docs and comments.
- Scope reviewed / exclusions: design/source comments describing command delivery, invalidation, and Timer replacement; command implementation correctness remains F-024/F-025/F-027/F-028.
- Severity: must fix before merge.
- Evidence: `doc/loop-alignment-and-ninjam-sync.md:111`–`:115` says every accepted command is applied coherently and establishes/rebases the map; `:130`–`:133` and `:162`–`:172` say new sessions receive fresh anchors and invalidation/disconnect clears map, anchors, and the generation gate. Current latest-value consumption at `NinjamAudioTimingCommand.h:53`–`:119` can overwrite `Invalidate -> Replace` or `Replace -> Discipline`, and AudioHost's disabled-sync branch skips the take gate reset (`AudioHost.cpp:174`–`:185`, `:298`–`:302`; `LoopTake.cpp:524`–`:527`). The command comments likewise claim one job-thread publication is consumed exactly once at `NinjamAudioTimingCommand.h:31`–`:57`, `AudioHost.h:70`–`:72`, and `Scene.cpp:405`–`:408`, although Phase 2 found multiple Scene job/UI producer contexts and only “latest publication at most once” semantics. Finally, `doc/loop-alignment-and-ninjam-sync.md:219`–`:222` proposes that remote geometry should never replace local Timer timing, contradicting the approved F-024 direction that epoch changes clear authority, geometry changes replace Timer timing, and unchanged geometry derives phase correction. The prose originated mainly in `d6fdb88`, `e0f669c`, and `92e8907`.
- Why it matters: these statements turn known missing transition semantics into apparent guarantees and give two incompatible design directions for Timer geometry. A maintainer could preserve the unsafe latest-command shape because the comments say it is already coherent.
- Recommended disposition: document the approved complete desired remote transport state: session epoch, follow policy, full remote geometry, and timestamped remote phase. At each audio boundary compare desired with last applied state; epoch changes invalidate, geometry changes replace Timer timing, and unchanged geometry derives correction. State explicitly that per-entity anchors/modulo remain separate and that a publication cannot be described as “consumed exactly once.”
- Protected timing concepts affected: remote authority, Timer geometry, follow policy, command lifecycle, sync map, scene/source anchors, per-entity phase.
- Verification: correction audit against the eventual complete-state type and the F-024/F-025 prerequisite regressions; search for obsolete “latest command,” “each publication exactly once,” and “remote geometry never replaces Timer” claims.
- Human decision: pending.

### S15-03 — The MIDI quantisation TDD plan still labels the pre-`e72f3b0` state as current

- Stage / reviewer: Stage 15 — Docs and comments.
- Scope reviewed / exclusions: truth/status of `doc/ninjam-midi-quantisation-investigation.md`; no independent judgment that the later implementation fully meets every MIDI/visual acceptance case.
- Severity: must fix before merge.
- Evidence: the document says Scene only calls `SetMidiGrain` and publishes no remote origin/phase at `:90`–`:95`, calls `RemoteTransportGeometry` unused at `:101`–`:105`, says remote snapping uses only a repeated constant step at `:107`–`:141`, and says `_MakeProposal` replaces server BPI with locally derived BPI at `:197`–`:201`. Commit `e72f3b0` instead publishes the geometry/origin (`Scene.cpp:443`–`:449`; `Quantiser.cpp:234`–`:247`; `LoopTake.cpp:2550` onward), evaluates direct remote boundaries (`MidiQuantisation.cpp:214`–`:272`), and retains authoritative nonzero server BPI (`NinjamTimingCoordinator.cpp:291`–`:313`; test `:150`–`:161`). The same plan tells later work to reuse “existing latest-wins” publication at `doc/ninjam-midi-quantisation-investigation.md:274`–`:279` and retain “existing generation/invalidation semantics” at `:324`–`:336`, both superseded by accepted F-024/F-025. The plan was added in `9a8bc91`; `e72f3b0` changed production and tests without updating it.
- Why it matters: this is titled and written as a live TDD plan/verified-state reference. Following it now would reimplement completed work, misdiagnose current behavior, or retain command/session semantics the human has explicitly rejected.
- Recommended disposition: convert it to a dated implementation record with an explicit `implemented by e72f3b0 / residual verification` status, or rewrite its verified-state and remaining-work sections from current code. Remove model-specific agent dispatch instructions from the durable behavior contract, and replace latest-command/generation-preservation language with the approved complete-state/epoch direction. Do not claim the remaining overlay/manual acceptance cases are green unless verified.
- Protected timing concepts affected: active remote grid, local grain, authoritative BPI, remote phase/origin, command lifecycle, session epoch.
- Verification: statement-by-statement diff against current production and focused tests; Stage 14 identifies which residual behavioral/visual contracts are actually executable.
- Human decision: pending.

### S15-04 — User and diagnostic labels still collapse protected timing vocabulary

- Stage / reviewer: Stage 15 — Docs and comments; overlaps accepted F-013–F-016 and F-034 rather than renaming production symbols independently.
- Scope reviewed / exclusions: visible prompt/log labels only; log retention/placement belongs to Stage 17/F-034.
- Severity: must fix before merge.
- Evidence: the remote-tempo prompt calls the accepted remote interval “Master loop” and the BPI-derived remote grid cell “Grain” at `Scene.cpp:378`–`:381` (introduced by `5f76564`/`f36bfe7`); accepted F-013 requires “Remote grid step,” and the approved glossary distinguishes remote master interval from any per-loop/master-loop cursor. The timing-policy log emits unqualified `observationSample` at `Scene.cpp:423`–`:430`, despite F-015's accepted device-audio observation name. The alignment logger labels `Timer::QuantiseSamps()` as `grain` at `Station.cpp:2357`–`:2371`, which can be remote-derived and therefore is not necessarily a local construction grain. Its `masterAnchor` at `:2410`–`:2422` and `:2446`–`:2469` is calculated from Timer absolute position modulo each entity length, not the protected monotonic scene anchor; if retained under F-034/Stage 17, the label must name that coordinate rather than compete with source/scene anchor terminology.
- Why it matters: users and support traces would use “grain,” “master loop,” “observation sample,” and “anchor” for values in different authorities/coordinates, undermining the glossary exactly where a human must diagnose a remote join.
- Recommended disposition: use “Remote master interval” and “Remote grid step” in the prompt; use the approved `DeviceAudioSampleAtObservation` wording in diagnostics; and either delete the large logger under F-034 or rename retained bounded fields to their exact Timer-absolute/local-entity coordinates. Keep BPM/BPI uppercase in prose/UI as approved.
- Protected timing concepts affected: local grain, active/remote grid, remote master interval, Timer absolute coordinate, monotonic scene/source anchor, per-entity phase.
- Verification: UI/log string audit; normal/verbose remote-join trace after F-034; no remote-BPI-derived value remains labeled `grain`.
- Human decision: pending.

### S15-05 — Test descriptions overstate model fidelity and encode rejected transition/sentinel contracts

- Stage / reviewer: Stage 15 — Docs and comments; test mechanics and minimum suite are handed to Stage 14.
- Scope reviewed / exclusions: names and explanatory comments, not assertion quality except where needed to prove the description false.
- Severity: must fix before merge.
- Evidence: `NinjamTimingIntegration_Tests.cpp:12`–`:24` calls a model-take harness deterministic end-to-end coverage and says it is “exactly” AudioHost's fan-out, while its `ModelTake` at `:39`–`:48` omits production loop modulo, sync maps, anchors, snapshots, and MIDI/automation; the newer TDD document itself says this harness cannot prove MIDI event alignment at `doc/ninjam-midi-quantisation-investigation.md:214`–`:218`. `NinjamAudioTimingCommand_Tests.cpp:55`–`:62` names and explains latest-publication-wins as benign supersession, but F-024 rejects that command contract. `NinjamTimingIntegration_Tests.cpp:475`–`:519` claims disconnect resets every consumer generation, but the model does so while production invalidation skips the take fan-out (F-025). `JobSchedulingDelayDoesNotChangeCorrectionTarget` at `:553`–`:604` covers an ordinary wrap only and overstates the still-broken initial-join contract in F-026. `NinjamTiming_Tests.cpp:150`–`:160` treats numeric zero as an unusable/absent device anchor, the exact sentinel behavior rejected by F-030. These descriptions entered in `d6fdb88`, `b41b5a8`, and `b9619b9`.
- Why it matters: passing model tests currently read as evidence for the production command/reconnect/join guarantees that Phase 2 proved missing. They can conceal regressions by defining unsafe overwrite, fake invalidation fan-out, and zero-as-absent as intended behavior.
- Recommended disposition: rename the harness as a limited coordinator/Timer model and enumerate omissions; replace the command coalescing description with the approved complete-desired-state contract; make reconnect/invalidation descriptions production-faithful; narrow the delay test name to wrap discipline and add a separate initial-join contract; distinguish explicit anchor absence from valid zero. Stage 14 owns whether to rewrite/remove each test and the prerequisite suite.
- Protected timing concepts affected: command lifecycle, session epoch, generation gate, observation presence, initial join vs ordinary wrap, per-loop phase/automation origin.
- Verification: Stage 14 test-to-contract map; F-024/F-025 prerequisite regressions through production seams; F-026/F-030 focused tests; search audit for “exactly”/“end-to-end” claims attached only to model harnesses.
- Human decision: pending.

### Statement-level correction table

| Current statement | Required correction tied to behavior/decision |
| --- | --- |
| `doc/ninjam.md:7`–`:8`: interval is “BPM × BPI beats” | Say an interval contains BPI beats at the session BPM; duration is `60 * BPI / BPM` seconds (or the equivalent sample formula). |
| `doc/ninjam.md:12`–`:17`: job-thread observations currently carry remote timing | Mark current callback getter/publication as the F-021 defect; document the accepted target as timestamped job-owned/internal-`AudioProc` publication with no other NJClient callback calls. |
| `doc/ninjam.md:31`–`:44`: MIDI is re-anchored each wrap; job-thread `RepositionFromAnchor` writes correction | Say the session anchor is retained, the active map restores each entity from common mapped progress, and audio-thread cursor translation updates the automation-global-origin correction; `RepositionFromAnchor` is not current. |
| `doc/ninjam.md:58`–`:61`: `Stay local` publishes none | Say it publishes/expresses one complete `NoSync` desired state that invalidates remote authority without moving local phase. |
| `doc/loop-alignment-and-ninjam-sync.md:111`–`:115`: every accepted command establishes/rebases coherently | Qualify this as the intended invariant and record that the current latest-command mailbox violates it; after F-024, describe desired-state comparison rather than independent commands. |
| `doc/loop-alignment-and-ninjam-sync.md:130`–`:133`, `:162`–`:172`: invalidation/disconnect clears every map/anchor/gate | Mark this as the required F-025/F-027/F-028 invariant until implemented; current production does not reset every take gate or represent physical loss. |
| `doc/loop-alignment-and-ninjam-sync.md:219`–`:222`: target is never to replace Timer timing with remote geometry | Replace with the approved rule: geometry change replaces Timer timing while local loop lengths/source anchors remain independent; unchanged geometry derives phase correction. |
| `NinjamAudioTimingCommand.h:31`–`:57`, `AudioHost.h:70`–`:72`, `Scene.cpp:405`–`:408`: one job-thread publication is consumed exactly once | State current serialized producer contexts and latest-value semantics accurately, then update to the explicit self-enforcing complete-state owner required by F-009/F-024. |
| `NinjamTimingCoordinator.cpp:219`–`:222`: zero means anchor absent | Replace the sentinel comment with explicit presence semantics; sample zero is valid per F-030. |
| `doc/ninjam-midi-quantisation-investigation.md:90`–`:105`: remote grid/origin is not published and geometry is unused | Record the `e72f3b0` geometry/origin flow through Scene -> Quantiser -> LoopTake, then list only residual unverified work. |
| `doc/ninjam-midi-quantisation-investigation.md:107`–`:141`: current remote path repeats a rounded constant step | Record the direct-boundary remote path at `MidiQuantisation.cpp:214`–`:272`; retain constant-step wording only for the local/no-remote-grid fallback. |
| `doc/ninjam-midi-quantisation-investigation.md:197`–`:201`: supplied server BPI is replaced by deduction | State that nonzero server BPI is authoritative; local deduction remains only for older/partial BPI-zero observations. |
| `doc/ninjam-midi-quantisation-investigation.md:274`–`:279`, `:324`–`:336`: preserve latest-wins generation/invalidation | Replace with approved complete desired state plus explicit session epoch; retain immutable publication, not the defective transition semantics. |
| `Scene.cpp:378`–`:381`: “Master loop” / “Grain” for remote prompt | “Remote master interval” / “Remote grid step”; BPM/BPI remain uppercase. |
| `Scene.cpp:423`–`:430`: `observationSample` | `deviceAudioSampleAtObservation` (or the approved casing at the final log boundary), distinct from Timer absolute and scene coordinates. |
| `Station.cpp:2357`–`:2474`: `grain`, `masterAnchor`, `automationAnchor` | If retained after F-034, label remote/local grid source explicitly; label the Timer-absolute modulo origin and `AutomationGlobalSampleOrigin` by their actual coordinates. |
| `NinjamTimingIntegration_Tests.cpp:12`–`:24`: model harness is exact end-to-end AudioHost fan-out | Describe it as a limited coordinator/mailbox/Timer model and enumerate omitted production map/entity/MIDI behavior. |
| `NinjamAudioTimingCommand_Tests.cpp:55`–`:62`: latest publication benignly supersedes prior command | Replace with the accepted latest-complete-desired-state comparison and test geometry/epoch/phase changes, not independent command loss. |
| `NinjamTimingIntegration_Tests.cpp:475`–`:519`: model invalidation proves production consumer reset | State this is model-only or replace it with a production-fan-out two-session regression where session 2 can restart at generation 1. |
| `NinjamTimingIntegration_Tests.cpp:553`–`:604`: scheduling delay never changes correction target | Narrow to ordinary wrap discipline; F-026 requires a separate initial-join delayed-observation contract. |
| `NinjamTiming_Tests.cpp:150`–`:160`: zero is an unusable anchor | Test explicit absent separately from present-zero/nonzero after F-030. |

## Handoffs

- Phase 3 integrator: S15-01 and S15-02 should enrich/attach documentation obligations to F-021 and F-024–F-030 rather than create duplicate runtime findings. S15-04 mostly supplies the required Stage 15 correction table for accepted F-013–F-016/F-034.
- Stage 13: `doc/ninjam-midi-quantisation-investigation.md` is a cleanup candidate as a stale live plan, but retain its useful acceptance criteria/history rather than deleting the whole file without a durable replacement.
- Stage 14: own test rewrite/removal and minimum suite for S15-05. In particular, separate model tests from production-faithful F-024/F-025 prerequisites and add/narrow F-026/F-030 contracts.
- Stage 17: if any alignment logger survives F-034, use S15-04's exact field semantics when defining the supported symptom-to-signal map. Do not retain `grain`, `masterAnchor`, or unqualified `observationSample` merely for log compatibility.
- Stage 18: F-032's non-finite input hardening may require nearby comments/tests to say “finite and plausible”; this stage found no separate documentation defect beyond the already accepted boundary.
- Later documentation owner: update behavior docs in the same cleanup batch as the contract they describe, after its prerequisite tests pass. A standalone wording commit before implementation must explicitly label current defect versus approved target.

## Uncertainties

- `doc/ninjam-midi-quantisation-investigation.md` contains valuable acceptance criteria as well as stale implementation-state assertions. This investigation proves several statements obsolete after `e72f3b0`, but does not claim all visual boundary cases are implemented; Stage 14/20 must establish that before the document is marked completed.
- The Station diagnostic logger is accepted for major restructuring/removal under F-034. Exact replacement labels depend on which bounded values Stage 17 retains; the table gives semantic constraints, not a required log schema.
- Current Scene job/UI publication sites appear serialized by `_sceneMutex`, as Phase 2 concluded. The inaccurate “job thread only” comments are still a contract defect, but this report does not elevate them to a new race finding.
- Several documents intentionally describe design targets. The defect is absence of a clear “intended versus current” marker where current code is known not to provide the stated guarantee; correcting prose must not weaken the accepted target to match defective code.
- No interactive prompt or verbose logging session was run, so typography/line wrapping and actual support usability remain manual verification items. String values and their producing coordinates were verified statically.
- Upstream NJClient was not edited or proposed for edit. F-021's wording correction must describe the Jamma-owned publication seam only.

## Conclusion

Stage 15 completed the bounded timing-only truthfulness review and found five documentation/comment groups requiring correction. The integration guide assigns timing/MIDI mutation to the wrong threads and misstates `Stay local`; the design reference and source comments promise command/invalidation guarantees the current latest-command path lacks and contain a Timer-geometry direction that contradicts the approved F-024 design; the MIDI quantisation TDD plan still presents the pre-`e72f3b0` implementation as current; prompt/log labels violate the protected glossary; and model-test descriptions overclaim production fidelity while encoding the rejected latest-command, invalidation, initial-join, and zero-sentinel contracts.

No protected timing concepts are proposed for collapse. The correction direction preserves remote authority, Timer geometry, monotonic scene/source coordinates, mapped elapsed time, per-entity phase/anchors, automation origin, local grain, active remote grid, and all three follow policies as distinct. No cleanup was implemented, no upstream or unrelated retained scope was changed, and the statement-level table above is the complete Stage 15 handoff.
