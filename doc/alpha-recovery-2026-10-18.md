# Alpha crash recovery — 18 October 2026

Continue the approved sequence in `alpha-handover-2026-10-18.md` and the five Desktop plans. This is a recovery record, not a completion claim.

## Verified persisted state

- Current feature branch: `codex/station-vst-rack`, HEAD `65f4245e`.
- Trigger names merged locally into `bugfix/alpha-cleanup` as `65f4245e` after independent source review. The user explicitly accepted both final visual fixes in chat.
- Full naming Debug native regression passed 1,831/1,831; 17 disabled. Log: `alpha-naming-full-debug.log` (384301 ms).
- `alpha-naming-release-app.log` records a successful Release app target with zero warnings/errors. It did not rebuild JammaLib: current complete Release validation remains pending.
- Rack foundation persisted as uncommitted changes in Station, VstChain, plugin adapters, session schema/loader/exporter and StationMidiInstrument tests. It includes additive rendering/path latency, initial cached capabilities, 16 prepared live channel lists, identity slot reservations and initial persistence/legacy policy handling.
- `alpha-rack-lib-checkpoint.log` ends during compilation and contains trailing NUL bytes. It provides no successful build result. No build/test/Jamma process was running at recovery.
- Existing verification logs and unrelated files are preserved. Nothing pushed, tagged, released or merged into master.

## Remaining work

1. Audit/fix and validate the recovered foundation; build results are not yet established.
2. Finish the complete rack plan: stable slot retry/removal and pending cancellation; live-only cables/channel panel/collapse and below-fader stack; explicit missing-target silence; bounded recovery preserving raw imported data; legacy serial compatibility; additive audio/latency validation; note/sustain release ownership, retries, retired processing and UI destruction; persistence and tests.
3. Independent rack review, real UI/plugin acceptance, local feature commit and validated integration.
4. Trigger learn and explicit rig overwrite remain unimplemented. Read-only investigation established the existing serialized input pumps as the observation owner, callback-arrival MIDI time and serial enqueue sequence as freshness boundaries, a scrollable dedicated modal with an atomic multi-row draft, and separate Save/Save As/startup-default actions in RigDefaultSave.
5. Final complete Debug and appropriate Release builds/tests, threading audit/manual callback inspection, integrated performance/persistence acceptance and independent Sol review.

Read `.vscode/tasks.json` before every build/native test; use incremental affected projects, explicit absolute SolutionDir and documented process PATH preflight. One source editor and one build at a time. This record must be updated when further checkpoints actually pass.

## Resumed engine checkpoint

The crash damaged JammaLib Debug compiler artifacts. The original damaged PDB and object were preserved as `.crash` files inside the ignored Debug intermediate directory; the normal incremental command subsequently succeeded without a permanent project/task workaround.

`alpha-rack-engine-checkpoint-build-2.log` records successful Debug library/test builds with no warnings/errors. `alpha-rack-engine-checkpoint-tests-2.log` records 94/94 passing focused native tests. Earlier failed runs remain preserved; the final fixes retain synthetic punch/source-loss releases and suppress only already-delivered duplicate block releases. New coverage includes additive replacing processors/path latency, many-to-many explicit routes, note/sustain rejection retry and removal, stable failed-slot retry state/bypass and pending cancellation.

This is an incomplete engine foundation checkpoint, not the finished rack. It adds release-aware retained destination processing, initial bounded managed admission, stable missing recorded-target silence and static insertion/conversion latency checks. Remaining engine work includes automation lifetime/persistence, device-loss controller tracking, complete load-path limits, latency-change recovery and inactive-import recovery/diagnostics. GUI, app/Release builds, full combined regression and manual acceptance are pending.

The threading script flags five mutex additions in Station UI/job methods because it scans whole-file diffs. Manual callback inspection found no new locks/waits/allocations/logging in the added processing paths; this is a qualified manual review, not a clean static-audit claim. New destination/binding note/sustain fields are callback-owned; `RetirementReleased` is audio-to-structural atomic acknowledgement; scratch is allocated per snapshot off callback and mutated only by its audio reader. Routes are serialized structural state published as immutable prepared lists; plugin destruction remains queued to UI.

## Automation checkpoint

The engine foundation was committed locally as `d365f608`. Subsequent automation changes replace raw dispatch pointers with bounded leased snapshots that pin processor and child ownership off callback, and refresh after child topology changes. Stable target metadata and curves survive unavailable/inactive rack save/export; sidecar v0.4 adds bounded UTF-8 IDs while retaining v0.2/v0.3 import compatibility.

`alpha-rack-automation-tests-build-2.log` records successful incremental Debug library/native-test builds. `alpha-rack-automation-focused-tests.log` records 105/105 focused passes. Diff checks and the focused threading audit passed. Dispatch generations are serialized non-audio state; callback cursors are callback-owned; retirement collection and final UI destruction stay off callback. Saved identity strings are never read by the callback. Child notifications use weak Station ownership.

This remains an incomplete rack feature. Remaining engine, GUI, acceptance and trigger-learn work listed above is still pending; no rack merge or release has occurred.
