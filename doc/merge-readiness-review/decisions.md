# Human decisions

Below are ALL the human-written decisions following Phase 3.  It responds to the human decisions in the [phase 3 packet](phase-packets/phase-3.md).

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
