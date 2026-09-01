# Human decisions

This is the append-only ledger of human decisions for Phases 1–3. The Phase 1/2 text below was restored verbatim from commit `70e48a5` after commit `8cd8725` accidentally replaced it while recording the Phase 3 outcome. No human rationale was rewritten during reconciliation.

## Phase 1 and 2 decisions

### Primary Notes

* This branch intentionally has multiple features merged into it, so we must restrict focus of the review to the ninjam timing / remote tempo sync changes and not HUD, VST3 parity or window/tooling updates (see [02 - Diff Inventory] below).
* Almost all findings approved.
* Two stages highlighted a potential duplication of the same concept, which must be carefully addressed: S05-01 and S06-01 (see [05 - History] below).  Also S08-01 and S12-02 seem related.
* In order to perform these more complex updates, we must have sufficient unit test coverage to verify the updates have not broken the critical functionality.  This means one or two high quality tests to be identified/created before each major refactor.
* Start with larger structural / behavioural changes first.  Each and every stage must consist of at least one separate git commit (multiple if writing tests, refactoring/fixing, etc) and commit message should include finding and stage number.
* We added a LOT of code to Scene, Station and LoopTake, but we must attempt to keep these as slim as possible - especially Scene.  If bloat due to logging, then we can try to reduce this significantly.  If due to increased responsibility, then try to move functionality out and into other existing classes.

This last point may add to scope of review - that is desired.

### Findings

Decisions on the [overall findings doc](findings.md):

* F-001 - Keep HUD/visual/resource feature in merge.
* F-002 - Keep VST3 parity/state/mapping in this merge.
* F-003 - Keep window-placement persistence.
* F-004 - Keep build./agent tooling.
* F-005 - Approved.
* F-006 - Approved.
* F-007 - Approved.
* F-008 - Approved.
* F-009 - Approved.  Very useful boundary clarification update, but needs care.
* F-010 - Approved.
* F-011 - Approved.
* F-012 - Approved.
* F-013 - Approved.
* F-014 - Approved.
* F-015 - Approved.  Note the naming queries raised in Vocabulary section.
* F-016 - Approved.
* F-017 - Approved.
* F-018 - Approved.
* F-019 - Approved.
* F-020 - Approved.
* F-021 - Approved.  Check notes from S08-01 and S12-02 below.
* F-022 - Approved.
* F-023 - Approved.
* F-024 - Not approved, although a simpler approach is proposed in S09-01 below, which is approved.
* F-025 - Approved.
* F-026 - Approved.
* F-027 - Approved.
* F-028 - Approved.
* F-029 - Approved.
* F-030 - Approved.
* F-031 - Approved.
* F-032 - Approved.
* F-033 - Approved.
* F-034 - Approved.

### Stage Reports

Below we respond in detail to all the points raised in the separate stages run as part of phase 1 and 2.

#### [01 - Diff Inventory](stage-reports/01-diff-inventory.md)

Indeed this branch contains changes for the new HUD UI, VST3 parity, window/tooling changes, and also remote timing/sync structural and behavioural changes.

For the purposes of this merge review, these changes MUST be included in the merge, and are considered OUT OF SCOPE for cleanup.  In other words, as much as possible attempt to bring the following changes/features into master branch without modification:

* HUD UI feature
* VST3 parity
* Structural window / tooling changes

Therefore we solely focus the review and potential cleanup ONLY to:

* Remote timing / sync structural and behavioural changes / fixes

#### [02 - Logical Layout](stage-reports/02-logical-layout.md)

* S02-01 - Approved. Cleanup and keep sync modes out of redundant Loop/LoopTake/Station classes.
* S02-02 - Approved.
* S02-03 - Approved, extract the value contracts, but we MUST NOT create more classes for this work - find appropriate existing classes to move to.

#### [03 - Conventions](stage-reports/03-conventions.md)

* S03-01 - Approved, but keep use of BPI / BPM in comments in upper case.  Local vars can stay bpm and bpi if used.
* S03-02 - Approved.
* S03-03 - Approved.

#### [04 - Vocabulary](stage-reports/04-vocabulary.md)

Approved most name changes:
* S04-01 - Approved use of RemoteGridStepSamps
* S04-02 - Approved RemoteMasterIntervalLengthSamps, RemoteTimingReplacement (do we really need "Accepted" prepended here? Can do it required to clarify, but prefer simpler if still correct), RemoteMasterPhaseSamps (do we really need "Observed" prepended?), RemotePhaseDeviceSample (do we need "Observation" inserted?), RemoteMasterPhaseCorrectionSamps, localSourceCorrectionSamps and remoteMasterCorrectionSamps
* S04-03 - Approved all. Note: as for S04-02, "Observation" can be used if really needed for clarification/semantic meaning, ensure we match that convention.  Ensure naming is such that counters/vars are always used in the correct domains, with appropriate authority.
* S04-04 - Approved
* S04-05 - Potentially approved, but we CANNOT edit NJClient library code, that is upstream and can change externally.

Also, in terms of naming, I'm happy with the meaning of all terms in [the glossary](00-scope-and-inventory.md), however I'm dubious on the following terms.  Strongly consider these suggestions but feel free to reject if there is a compelling case to leave them alone:
* SyncPhaseMap - can this class name be improved to clarify its use?
* Ruler - not sure exactly its meaning, do we really need another term or can it be expressed using existing terminology?
* Geometry - guessing this is related to loop lengths being contructed from grains, being multiples of each other. The term is a bit overloaded though, maybe a better one can be used?
* Anchor / Origin - happy with Anchor, but anchor and origin both feel like they mean the same but we use both.  Check if can be merged / clarified.
* Mailbox - seems like leaking implementation and adding additional vocab needlessly. Simple clearer working if possible.

#### [05 - History](stage-reports/05-history.md)

* S05-01 - Approved.  THIS IS PROBABLY THE MOST CRITICAL FINDING, and exactly why we are carrying out this review.  We must safely ensure we have one authoritative owner and implementation of each core feature (such as local and remote transport / coordinate systems) and clarify both its responsibility boundary and its naming to be semantically correct.  Spend effort (e.g. dedicated fleet of subagents) ensuring we do what we can to clean this up, and safely.
* S05-02 - Approved with note.  We do need to align/clean the debugging parts added in this branch.  We need to unify where possible, but we must retain some logging for loop / timing sync (gated behind a logging configuration param) for later use.  Agreed unacceptable to keep large Scene and Station level "_Log*()" functions as-is, must be slimmed.  Reduce code size and ensure ZERO performance hit if toggled off.

#### [06 - Stale Code](stage-reports/06-stale-code.md)

* S06-01 - Approved, same comments as S05-01.
* S06-02 - Approved.
* S06-03 - Approved.

#### [07 - Thread Safety](stage-reports/07-thread-safety.md)

* S07-01 - Approved.
* S07-02 - Approved.
* S07-03 - Approved.
* S07-04 - Approved.

#### [08 - Hot Paths](stage-reports/08-hot-paths.md)

* S08-01 - Approved.  One implementation caveat: the existing NinjamRemoteSnapshot is not suitable as-is for the callback—it is vector-backed and Snapshot() takes _snapshotMutex ([NinjamConnection.cpp (line 736)](../../JammaLib/src/ninjam/NinjamConnection.cpp#L736)). A fixed-size lock-free timing mailbox is needed. To retain the exact block-start timing coordinate, the best shape is for AudioProc itself to publish/capture the pre-advance timing internally, then expose that data through the mailbox—without another NJClient call from the callback.
* S08-02 - Approved.  Although logging is desirable it must NEVER interfere with audio hotpath performance and so do whatever sliming/moving to prevent std::cout or string heap creation or heavy synchronous operations in audio thread, even at the expense of logging info.
* S08-03 - Approved.
* S08-04 - Approved.

#### [09 - Timing Correctness](stage-reports/09-timing-correctness.md)

* S09-01 - Not approved.  Attempting to cope with all scenarios would add too much complexity, so instead try something simple like: Publish only the latest complete desired remote transport state: session epoch, follow policy, full remote geometry, and remote phase at an observation sample. Never publish standalone invalidation or phase-delta commands. At each audio boundary, AudioHost compares the desired state with the last applied state: epoch changes clear anchors/map, geometry changes replace Timer timing, and unchanged geometry derives phase correction from the timestamped remote phase.
* S09-02 - Approved.
* S09-03 - Approved, although go for simplicity and surgical fix.

#### [10 - State Machine](stage-reports/10-state-machine.md)

* S10-01 - Approved.
* S10-02 - Approved.  Use common and simple approach, use tests to confirm fix and avoid regressions.

#### [11 - Numerics](stage-reports/11-numerics.md)

* S11-01 - Approved.
* S11-02 - Approved, but must keep simple.
* S11-03 - Approved.
* S11-04 - Approved.

#### [12 - Lifetimes](stage-reports/12-lifetimes.md)

* S12-01 - Approved.
* S12-02 - Approved, bear in mind we covered similar before in S08-01.

## Phase 3 decisions

## Gate 3 Decisions

* G3-1 - Accepted. HUD, VST3, window/tooling, unrelated MIDI/resources, and upstream NJClient remain in the merge but outside cleanup. All protected timing concepts remain distinct.
* G3-2 - Accepted. Simplication order as required, take extra care on risky items.
* G3-3 - Accepted.
* G3-4 - Accepted.
* G3-5 - It is imperative to to retain per-loop alignment, including when some loops are double/triple the master length.  For instance, we have three loops, [1] Master of length M, [2] one of length 2M, [3] one of length 3M, and they are all have different relative playpos at current transport.  If recovering, they must retain their relative playpos and not be reduced to modulo the master length.  Support whatever behaviour permits that to be fully recovered correctly.
* G3-6 - Silent downgrading is fine, we just default it to zero on load if not present.
* G3-7 - Full BPI is mandatory (and always guaranteed by ninjam, when its info is available).
* G3-8 - Accepted.
* G3-9 - Leave .jam file writing as-is, no need to add complexity or redaction features here.
* G3-10 - Accepted.

## Finding Details

In general follow recommendations and prior acceptance of findings and stages.  Here we address some specific items raised in phase 3 canonical finding gates:

* F-005, F-006, F-009 - Approved.
* F-021, F-024 - Approved.
* F-027, F-032, F-034 - Approved.

### Small simplifications
* F-035, F-036 - Approved.
* F-037 - Approved.
* F-038 to F-041 - Approved.
* F-042, F-043 - Approved.
* F-044 - Refer to notes on G3-5 above.
* F-045 - See G3-6, silent downgrade is fine.
* F-046 - See G3-7.  When not connected to remote session, local BPI is to be inferred by calculations in pure functions based off local master loop length / grain.
* F-047, F-048 - Accept (but avoid code bloat; surgical only).
* F-049, F-050 - Reject (leave .jam writing as-is).

## Phase 4 decisions

## Gate 4 decisions

* B001 to B017 - Approved all batches.

Ensure we do include addressing S05-01 (may have already done this in prior gates - confirm).
Refer back to full list of fixes to ensure we have accounted for all accepted/approved in some way.