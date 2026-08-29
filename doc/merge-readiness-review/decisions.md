# Human decisions

Below are ALL the human-written decisions following Phase 1 of the merge-readiness review.

## Primary Notes

* This branch intentionally has multiple features merged into it, so we must restrict focus of the review to the ninjam timing / remote tempo sync changes and not HUD, VST3 parity or window/tooling updates (see [02 - Diff Inventory] below).
* Almost all findings approved.
* Two stages highlighted a potential duplication of the same concept, which must be carefully addressed: S05-01 and S06-01 (see [05 - History] below).
* In order to perform these more complex updates, we must have sufficient unit test coverage to verify the updates have not broken the critical functionality.  This means one or two high quality tests to be identified/created before each major refactor.
* We added a LOT of code to Scene, Station and LoopTake, but we must attempt to keep these as slim as possible - especially Scene.  If bloat due to logging, then we can try to reduce this significantly.  If due to increased responsibility, then try to move functionality out and into other existing classes.

This last point may add to scope of review - that is desired.

## Findings

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

## Stage Reports

Below we respond in detail to all the points raised in the separate stages run as part of phase 1.

### [01 - Diff Inventory](stage-reports/01-diff-inventory.md)

Indeed this branch contains changes for the new HUD UI, VST3 parity, window/tooling changes, and also remote timing/sync structural and behavioural changes.

For the purposes of this merge review, these changes MUST be included in the merge, and are considered OUT OF SCOPE for cleanup.  In other words, as much as possible attempt to bring the following changes/features into master branch without modification:

* HUD UI feature
* VST3 parity
* Structural window / tooling changes

Therefore we solely focus the review and potential cleanup ONLY to:

* Remote timing / sync structural and behavioural changes / fixes

### [02 - Logical Layout](stage-reports/02-logical-layout.md)

* S02-01 - Approved. Cleanup and keep sync modes out of redundant Loop/LoopTake/Station classes.
* S02-02 - Approved.
* S02-03 - Approved, extract the value contracts, but we MUST NOT create more classes for this work - find appropriate existing classes to move to.

### [03 - Conventions](stage-reports/03-conventions.md)

* S03-01 - Approved, but keep use of BPI / BPM in comments in upper case.  Local vars can stay bpm and bpi if used.
* S03-02 - Approved.
* S03-03 - Approved.

### [04 - Vocabulary](stage-reports/04-vocabulary.md)

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

### [05 - History](stage-reports/05-history.md)

* S05-01 - Approved.  THIS IS PROBABLY THE MOST CRITICAL FINDING, and exactly why we are carrying out this review.  We must safely ensure we have one authoritative owner and implementation of each core feature (such as local and remote transport / coordinate systems) and mclarify both its responsibility boundary and its naming to be semantically correct.  Spend effort (e.g. dedicated fleet of subagents) ensuring we do what we can to clean this up, and safely.
* S05-02 - Approved with note.  We do need to align/clean the debugging parts added in this branch.  We need to unify where possible, but we must retain some logging for loop / timing sync (gated behind a logging configuration param) for later use.  Agreed unacceptable to keep large Scene and Station level "_Log*()" functions as-is, must be slimmed.  Reduce code size and ensure ZERO performance hit if toggled off.

### [06 - Stale Code](stage-reports/06-stale-code.md)

* S06-01 - Approved, same comments as S05-01.
* S06-02 - Approved.
* S06-03 - Approved.