# Alpha crash recovery — 18 October 2026

Continue the approved sequence in `alpha-handover-2026-10-18.md` and the five Desktop plans. This is a recovery record, not a completion claim.

## Current checkpoint — implementation and automated validation complete

- Current branch: `codex/trigger-learn`. Source checkpoint: `3e7c5da77d75cbfe992b14e26e2034e987acea84`; subsequent documentation-only commits do not change the tested source.
- Station labels and trigger names are accepted and integrated at `bugfix/alpha-cleanup` / `65f4245e`.
- Rack/cables are complete and independently cleared by Astra on `codex/station-vst-rack` / `af0fa5e4`. Learn/explicit save are complete on the dependent branch, cleared by an independent feature reviewer and final Sol source review.
- Final full Debug native run: **1848/1848 passed**, 216 suites, 17 disabled, 209554 ms (`alpha-final-debug-tests.log`).
- Final Release affected run: **730/730 passed**, 73 suites, 12 disabled, 7620 ms (`alpha-final-release-affected-tests.log`). Debug and Release library, tests and app build successfully with zero warnings/errors. Release logs: `alpha-final-release-{lib,tests,app}-build.log`.
- User explicitly deferred manual acceptance with “I can check later.” Actual UI/plugin/audio/hardware and integrated paired JAM+rig reload acceptance are **not passed**. No Jamma app automation was retried after the access refusal; the user chose manual checks.
- Rack and learn remain unmerged pending their acceptance gates. Next: user checks the fresh Debug app, fix any reported issues, then locally integrate rack followed by learn into `bugfix/alpha-cleanup` and verify the resulting merges. Do not push, tag, release or merge master automatically.
- Fresh Debug app: `Jamma/bin/x64/Debug/Jamma.exe`. No source/build owner remains delegated and no build/test process remains running.
- Requested interactive HTML explainer: `C:/Users/matto/.codex/visualizations/2026/10/18/01a14fcd-913f-7120-8348-9198c4a35124/jamma-field-guide.html`. It is a standalone teaching model with no hardware/file mutations. Its six tabs/interactions and 736/390/320px layouts passed browser checks; desktop/mobile screenshots were visually inspected.

The sections below preserve the recovery chronology. Earlier “pending” statements describe their historical checkpoint and are superseded by this status.
## Initial recovered state

- Recovered feature branch: `codex/station-vst-rack`, based on integration commit `65f4245e`. Subsequent checkpoints are recorded below.
- Trigger names merged locally into `bugfix/alpha-cleanup` as `65f4245e` after independent source review. The user explicitly accepted both final visual fixes in chat.
- Full naming Debug native regression passed 1,831/1,831; 17 disabled. Log: `alpha-naming-full-debug.log` (384301 ms).
- `alpha-naming-release-app.log` records a successful Release app target with zero warnings/errors. It did not rebuild JammaLib: current complete Release validation remains pending.
- Rack foundation persisted as uncommitted changes in Station, VstChain, plugin adapters, session schema/loader/exporter and StationMidiInstrument tests. It includes additive rendering/path latency, initial cached capabilities, 16 prepared live channel lists, identity slot reservations and initial persistence/legacy policy handling.
- `alpha-rack-lib-checkpoint.log` ends during compilation and contains trailing NUL bytes. It provides no successful build result. No build/test/Jamma process was running at recovery.
- Existing verification logs and unrelated files are preserved. Nothing pushed, tagged, released or merged into master.

## Initial remaining work (historical)

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

## Complete rack engine checkpoint

`alpha-rack-engine-complete-gate-build.log` records a successful exact-source incremental Debug library/test build with zero warnings/errors. `alpha-rack-engine-complete-gate-tests.log` records 294/294 relevant engine, MIDI, latency, exporter, automation, Overdub and staging checks passing; 12 disabled. The first broader run passed 255/265: older fake processors needed explicit role/capability declarations, and the former INT_MAX Station activation expectation was replaced with an explicit admission-rejection assertion while retaining child-sum overflow coverage.

The engine now includes bounded slot paging/recovery commands, atomic 16-channel route edits, complete load admission bounds, latency suspension/resumption, device and recorded pedal release retries, channel-mask delta cleanup, and preserved raw inactive imports. Independent findings addressed failed VST3 stale output, original render width during bus shrink/retirement, sole-plugin bypass release cleanup, additive unknown-capability filtering, and automation resolution after Loop-index compaction.

Removed bypassed/unloaded processors are terminal and silent; teardown ends their voices. A guarded panic receipt is retained for any later reuse of a suspended instance. Active bypass retains pending note/pedal cleanup through unbypass. GUI removal closes the captured editor before structural removal. No MIDI output transformation exists in this feature.

GUI/cables, app/Release builds, independent complete-feature review, real plugin/hardware acceptance, trigger learn/save and final integrated gates remain pending. This checkpoint is not a rack feature completion or merge.

## Rack presentation in progress

The shared presentation is named `GuiVstRack` following the user's clarification. It accepts an optional existing channel panel; Station supplies live routing controls, while Take/Loop retain their established controls and serial audio policy. The new view stages identity/revision commands through VstRackControls and reuses existing textures, scroll panels and cable geometry/shader.

`alpha-rack-ui-second-build.log` records a passing Debug library build. `alpha-rack-ui-tests-build.log` records a passing native-test build, and `alpha-rack-ui-first-tests.log` records 76/76 focused GUI rack/slider/control tests (1 disabled), including captured press identity/modifiers, scroll preservation and collapsed logical cable identity. Subsequent small cable-drop/footer/modal fixes still require combined validation.

Independent follow-up found four engine edge cases being corrected serially: hosted width after destination recreation; old playback-release acknowledgement before latency resume; malformed legacy automation IDs during inactive export; and combined latency validation after complete import reconstruction. The user requested a dedicated Astra subagent review of the complete rack/cabling design, safety, performance and cleanliness near completion, before merge. That review remains pending.

## Rack/cable review checkpoint

The combined shared-view and engine follow-up changes build successfully for Debug JammaLib, native tests and Jamma, with zero warnings/errors. `alpha-rack-review-fixes-combined-tests-2.log` records 460/460 engine/UI/cable/Scene/JSON checks passing (13 disabled). A final test-only embedded-NUL assertion passed in the 16/16 JSON rerun (`alpha-rack-review-fixes-json-final-tests.log`). Exact final build logs: `alpha-rack-review-fixes-tests-build-2.log`, `alpha-rack-review-fixes-tests-build-3.log`, `alpha-rack-review-fixes-app-build-2.log`.

The four engine follow-ups are corrected. Hosted width is cached with the processor, latency resume waits for old release receipts, complete imported child latency is checked before admission, and invalid legacy automation identities migrate to explicit unavailable references. Malformed raw rack IDs survive valid JSON through a raw-byte field; hierarchy validation still protects Station/Take/Loop identities while deferring inactive Station rack recovery to its owner. The legacy JSON reader now preserves embedded NUL with a direct regression assertion.

`GuiVstRack` provides the slot stack/append/scroll, identity/revision-bearing gestures, optional reused live channel controls, many-to-many cable editing, collapsed/clipped endpoint projection, explicit recovery and conversion controls. VstRackControls applies intents under existing Scene serialization and revalidates after native message pumping. View/capture/draft state is window-owned; command handlers stage values only. Plugin/route state remains structural and callback delivery remains prepared. Static threading audit plus manual callback inspection found no new callback lock, allocation or wait; reported mutex additions are non-RT UI/structural sites.

Dedicated Astra review requested by the user, fresh Debug UI/plugin acceptance, Release and final integrated validation remain pending. No rack merge, push, release/tag or master merge has occurred. Trigger learn/save is still queued.

## Astra review and manual acceptance

At the user's request, an independent `gpt-6-astra` subagent reviewed the entire `65f4245e..8dbc038c` rack feature. It found the ownership/design boundaries sound and no new callback allocation, lock or formatting in the inspected bodies. Three must-fix findings are being corrected before integration:

1. Recorded CC64-up needs all-destination release acceptance/retry under asymmetric fanout backpressure.
2. Pending older pedal-up must be ordered before admitting a new live/recorded pedal-down so stale recovery cannot lift the new owner.
3. Preserved malformed live-route IDs need lossless raw encoding through valid JSON, matching slot-ID recovery.

The tool refused computer-use launch because Jamma access was not approved. The user chose to perform the fresh Debug app checks themselves. Do not retry computer-use access or treat UI/audio/hardware acceptance as complete. Supply the rebuilt executable/checklist after review fixes pass. Astra source recheck is also required before the rack merge. Trigger learn/save remains queued.

## Astra fixes validated

The three findings above are corrected. Recorded sustain release requires all destinations to retain delivery; a retained older pedal release is delivered before a replacement attack is admitted. Malformed live-route identities use a canonical complete raw byte array, preserving Explicit mode, order, empty IDs and invalid UTF-8 through valid JSON.

`alpha-rack-astra-fixes-tests-build-3.log` and `alpha-rack-astra-fixes-app-build.log` record successful Debug builds with zero warnings/errors. `alpha-rack-astra-fixes-combined-tests.log` records 460/460 passing, 13 disabled. Threading audit/manual ownership inspection found no new shared fields, callback locks or allocation.

A narrow GuiVstRack button size guard avoids dirtying nine-patch buffers during unchanged per-frame layout. Debug library, tests and app builds passed (`alpha-rack-layout-*-build.log`); `alpha-rack-layout-ui-tests.log` records 150/150 UI/cable/Scene tests passing, 1 disabled. Independent Astra recheck and user visual/plugin/audio acceptance remain pending. Trigger learn/save remains queued; no rack merge or release has occurred.

## Rack source-level sustain recheck

Astra identified one further source-level variant: a recorded CC64-down could overtake a pending fanout pedal-up even after the destination-local ordering fix. MidiLoop now suppresses new sustain-down on that channel until the pending release is acknowledged, paralleling its existing note attack guard. The extended actual playback-to-Station regression exercises asymmetric queue pressure, intervening attack suppression, successful retry, a later fresh owner and final cleanup.

`alpha-rack-source-pedal-tests-build.log` and `alpha-rack-source-pedal-app-build.log` passed Debug builds with zero warnings/errors. `alpha-rack-source-pedal-focused-tests.log` records 145/145 Station/MidiLoop checks passing. Astra independently rechecked this exact frozen diff and cleared all original findings plus the source-level variant, with no remaining must-fix findings. Its verification was source inspection, separate from executed tests. Release library build passed; app link is pending. User manual Debug/plugin/audio acceptance is still pending.

Release library and app validation completed with zero warnings/errors (`alpha-rack-source-sustain-release-lib-build.log`, `alpha-rack-source-sustain-release-app-build.log`). No build processes remain. The rack source/review gates are complete; manual acceptance remains explicitly pending. To keep progress independent of manual-check availability, subsequent learn implementation may use a dependent local feature branch from this preserved rack checkpoint; do not merge the rack or dependent feature into bugfix/alpha-cleanup until their applicable acceptance gates pass.

The user replied "I can check later" to the rack checklist. Manual acceptance is deferred, not passed. Continue independent learn/save implementation on the dependent local branch; keep rack and learn integration blocked on applicable acceptance. No further app automation was attempted.

Full Debug regression against the preserved rack af0fa5e4 executable passed 1843/1843 tests across 214 suites, 17 disabled, in 301142 ms (`alpha-rack-full-debug-tests.log`). This run used the rack binary before any learn source was compiled; it is rack baseline evidence, not learn verification. Build/test lane returned to the sole learn source owner afterward.

## Trigger learn implementation in progress

Current dependent branch: `codex/trigger-learn`, based on rack `af0fa5e4`; neither feature is merged into integration. Sole source/build ownership is with the learn implementation agent; root edits only this recovery/progress record and performs read-only review. Partial learn source is now persisted: TriggerBindingLearn value conversion/validation, GuiTriggerLearn UI draft, MidiRouter passive observation/freshness requests, and HUD/Scene integration. These changes are unbuilt/unverified and incomplete; explicit Save work and focused tests/review remain. Do not interpret this partial record as completion. No further user acceptance request is needed for the already deferred rack check; preserve that pending status.

## Learn independent review in progress

The first independent source review found a required cross-close keyboard fix: a Ctrl+L (or Ctrl+grave) press consumed inside the modal could reach Window's shortcut on release after the modal closed, before Scene's consumed-key ledger saw it. The source owner is extending the existing ledger/query boundary and regression before the feature checkpoint. The review retracted an initial opener-Alt focus-loss concern after tracing Window's existing synthetic modifier release path. Other reviewed learn/save paths had no substantiated defect, but the required fix and final builds/tests remain pending. No manual acceptance or integration merge has occurred.

## Reviewed learn/save checkpoint

Trigger learn and explicit rig save are implemented on dependent `codex/trigger-learn`. The independent feature reviewer cleared the cross-close consumed-key release fix and the save command-ID collision fix (Save/Save As now 8410/8411, distinct from station confirmations). No remaining substantiated must-fix findings from that review. Source-only review is separate from execution evidence.

`alpha-learn-review-fixes-tests-build.log` records the current Debug library/native-test build with zero warnings/errors; `alpha-learn-reviewed-app-build.log` records the current Debug app build with zero warnings/errors. `alpha-learn-reviewed-focused-tests.log` records 422/422 checks across 43 suites passing. Coverage includes fresh passive MIDI/serial input and exact capture counts, inferred inverse edges, row rollback/import preservation/capacity/conflicts, native window modal-close releases, held shortcut cleanup, stale modal, all three save button dispatch paths, and save path/revision/failure semantics.

All learn state is off callback. GUI draft/row/pressed-key state is window-owned. Requests, arm/results and observation conversion use existing Scene serialization between UI/job owners; results are complete immutable values. Serial enqueue sequence uses the existing ingress mutex; job-owned repeated-report baseline resets by lifecycle generation. Lifecycle owners publish only an atomic invalidation generation. No new callback-owned learn state, hardware reader or callback locks/allocations were added. The static audit flags four whole-Scene non-RT lock sites; manual inspection distinguishes those from callback bodies. Shell path/saved revision is window-owned; the detached save result uses the existing atomic shared_ptr handoff and job drain on shutdown.

Source/build ownership returned to root, with no running commands or app session. Final combined Debug/Release validation and independent Sol review are next. Manual UI/plugin/hardware acceptance remains deferred by the user. Rack and learn remain unmerged; no push/tag/release/master merge.

## Final combined candidate review

Final independent Sol review cleared pinned `3e7c5da77d75cbfe992b14e26e2034e987acea84` against planning baseline `b61f5104`, with no additional substantiated must-fix findings. It inspected learn/save freshness/ownership, modal key suppression and exact releases, rig metadata promotion, rack processing/retirement, automation lifetime and persistence recovery. This was source-only clearance; final execution gates and manual acceptance remain separate.

The standard threading script reads HEAD-to-working-tree, so a clean postcommit result cannot audit this committed feature. `alpha-final-threading-baseline-audit.log` explicitly scans `b61f5104..3e7c5da7` with the same whole-file lock heuristic. Its 20 additions belong to UI pointer/key/rename/save orchestration, job-side physical MIDI observation, and structural/plugin load/remove/job methods. Manual callback-body review and independent Astra/Sol inspection found no new callback locks, waits, allocation or logging. Whole-file findings are classified, not presented as a static hot-path pass. Full combined Debug regression is currently running against the pinned source; Release validation follows serially.

## Final validation and delivery

No source changes followed the independently reviewed `3e7c5da7` checkpoint. Full Debug passed 1848/1848 (17 disabled); the combined affected Release gate passed 730/730 (12 disabled). Current Release library, native test executable and app builds passed. The final baseline-to-pin threading record and manual callback classification are described above; a clean HEAD-to-working-tree script run is not presented as committed-diff evidence.

The user also requested a bold interactive tabbed explainer. The standalone field guide covers before/current approaches, live versus recorded ownership, additive versus legacy serial mixing, learn draft cancellation/import limits/conflicts, explicit save/path/revision failures, exact affected files, source ownership and remaining acceptance. Browser checks exercised all six tabs and key simulations, verified no script errors or overflow at 736/390/320px, and desktop/mobile/rack screenshots were inspected. It does not control Jamma or write rig/session files.

All implementation and available automated/source-review work is complete. Manual acceptance and gated local integration are the remaining actions; these are deliberately not claimed complete. Preserve all logs, ignored crash artifacts and feature branches for that acceptance.
