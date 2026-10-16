# Alpha fixes progress

Execution baseline: `602d3abf90b5f62013d1e2634c6342833a168842` (local master and fetched origin/master, 16 October 2026). Master was clean. Integration branch: `codex/alpha-fixes`; worktree: `C:\Users\matto\Source\Repos\Jamma.worktrees\Jamma-alpha-fixes`.

## Scope and ownership

| Lane | Requests | Branch | Worktree | Owner | State |
| --- | --- | --- | --- | --- | --- |
| D | A05, A11 | codex/alpha-diagnostics-vst3 | Jamma-alpha-diagnostics-vst3 | Sol implementation agent | In progress |
| S | A03, A10 | codex/alpha-settings | Jamma-alpha-settings | Sol implementation agent | In progress |
| F | A07, A09 | codex/alpha-faders | Jamma-alpha-faders | Pending | Pending |
| V | A13, A12, A08 | codex/alpha-take-visuals | Jamma-alpha-take-visuals | Sol implementation agent | In progress |
| C | A01, A04 | codex/alpha-camera | Jamma-alpha-camera | Pending | Pending |
| H | A02 | codex/alpha-help | Jamma-alpha-help | Pending combined branch | Pending |

The integration and first three lane worktrees have `.vscode` and `.agents` contents copied without overwrite and isolated `x64-windows` manifest dependencies installed. No `Directory.Build.local.props` override was present. The shell-created checkouts are not app-managed, so app attachment is unsupported.

## Shared contracts

- Diagnostics: D owns event routing and sink lifetime; H later owns Main/parser help and must preserve D's routing. Callback-originated failures use bounded publication and job-side formatting.
- Settings: S owns SceneSettingsControls widgets and typed decoding; Scene only forwards accepted override changes through the existing subsystem path.
- Visuals: V owns Station/LoopTake layout and rendering methods. F later owns mixer/meter methods in those files. Take bounds must govern note geometry and core clearances coherently; core is decorative with existing picking preserved. Waveform gain is visual only.
- Camera: C later owns camera activity policy and Front zoom. Its framing must be checked against V's final geometry. Secondary-view extension remains pending user identification per plan and does not block Front.
- Faders: F later owns GuiSlider/GuiRack and meter source wiring/publication. Meters represent their owning gain path with bounded callback work.

## Reviews and validation

No lane has been accepted, reviewed, merged, or built yet. Record fixed SHA ranges, findings, fixes, and verification below as work completes. Proposed P01–P03 remain outside implementation scope pending triage.

## Proposal triage on selected baseline

- P01: `ReadMe.md:7` still embeds an AWS signed screenshot URL with a 2026-06-10 timestamp and 300-second expiry. Valid defect, deferred because this item is not accepted in the twelve-request implementation scope.
- P02: the historical `doc/ninjam-live-loop-latency-sync-planC.md` references are absent from the selected baseline. `rg` found current latency comments in `Loop.h`, `Station.cpp`, and `IVstPlugin.h`; the stated stale-reference defect is already satisfied, with no implementation required.
- P03: `Vst3Plugin.cpp` currently caches processor latency, so the historical interface comment is stale. Full rendered/audio timing compensation remains a separate behavior question requiring a latency-producing plugin smoke test. Deferred pending separate acceptance; no claim of full compensation.

## Baseline build

- 16 October 2026: integration worktree at baseline SHA `602d3abf90b5f62013d1e2634c6342833a168842`, incremental Debug x64 `JammaLib/JammaLib.vcxproj` with absolute `SolutionDir`, exit 0. The local `.vscode/tasks.json` was read immediately before the build. Output: `JammaLib/bin/x64/Debug/JammaLib.lib`. This is a setup/baseline check, not validation of any alpha fix.

## S lane frozen for review

- Base `602d3abf90b5f62013d1e2634c6342833a168842`; reviewed candidate `b3b15bf5ad77d56949804dae32a1b19caf6cb040`; worktree clean. Independent Sol review dispatched. No merge yet.
- A03: settings frame natural width measured as 498 px including widest subdivision row, header, tab, padding, and scrollbar; viewport clamping and narrow row wrapping. A10: 16 exclusive channel toggles; active click clears; keyboard presentation refresh and existing setter/held-note semantics retained.
- Lane verification: incremental Debug x64 JammaLib and native test project builds exit 0; 210/210 focused tests pass, exit 0. Filter `Gui*:Scene.Settings*:Scene.HidingSettings*:MidiRouterChannelOverride.*:StationMidiInstrument.SettingsOverride*:SceneRoutingIntegrationTest.FocusLoss*`. Test log in S worktree `test/JammaLib_Tests/bin/x64/Debug/settings-test-results.log`. Rendered appearance and physical MIDI remain manual gates.

## S independent review closure

- Independent Sol review of fixed `602d3abf..b3b15bf5` found no actionable P0–P3 issues. It inspected all eight changed files and surrounding layout/input paths. No new shared state or callback changes. No review fixes required. A03/A10 are accepted for integration, pending the combined smoke and rendered/manual gates. Reviewer did not rerun builds/tests and explicitly noted geometry tests do not prove rendered text fit.

## F lane start

- `codex/alpha-faders` created at baseline SHA in `Jamma-alpha-faders`; local `.vscode`/`.agents` copied without overwrite and isolated x64-windows dependencies installed. Same Sol implementer as S, in a separate checkout. F owns `GuiSlider`, `GuiRack`, meter/mixer code and only Station/LoopTake rack/mixer methods; V retains layout/render methods. Native builds remain serialized.

## V lane frozen for review

- Base `602d3abf90b5f62013d1e2634c6342833a168842`; candidate `ea15a7e6dc8a2e8dfefb3548886c27d768f1f6ac`; worktree clean. Independent Sol review dispatched; no merge yet.
- A13: scene MIDI note outer faces fit a 94% take-height band, with shared snapshot pitch ranges and semitone cells. A12: 1.3 waveform excursion target, limited by one common per-take contraction in dense stacks. A08: one decorative disc/annulus per take outside interactive children/picker pass.
- First focused run found four child-order failures (243/247); implementation corrected core ownership outside `_children`. Final incremental Debug x64 JammaLib and native test builds exit 0; focused native run passes 247/247 tests in 31 suites, exit 0. Shader compile/link/render checks pass for midi_note, station, waveform, picker; synthetic previews and JUnit/log evidence in the `take-visuals` visualization directory. Full app picking, recording/edit/undo/reload and many-station performance remain manual/integration gates.
- Camera integration contract: `LoopTake::CameraBoundsInParent()` includes fitted MIDI/audio and core. Local usable take band is ±0.47 take height, with radial-scale compensation in shader coordinates.

## User correction during V review

- A13 single-pitch MIDI events must be capped at a modest maximum height rather than occupying the full usable take band. The frozen V head currently makes a one-pitch note about 92% of its usable band, so it is not accepted as final. Independent reviewer is recording this as an actionable finding. V implementer will add the cap and focused assertion in a follow-up commit, then re-review and revalidate. D currently owns the serialized build slot.
- A13 recording clarification: fit note height and semitone spacing from the min/max MIDI pitches recorded so far, updating existing visible notes coherently when a new extreme arrives. The single-pitch cap applies until the range expands. V follow-up must test recording-growth publication/refresh, not just static material geometry.

## V review finding V-01

- **V-01 (P2), `MidiModel.cpp:990` at `ea15a7e6`:** one-pitch note height is about 92% of the usable band, exceeding the user's clarified maximum-height intent. Owner V; fix pending separate commit. Required verification: radial-scale-consistent cap, centered single-pitch note, and recording-growth test as lower/higher extremes arrive. Independent reviewer found no other actionable findings. Recording publication path already refreshes geometry from ingress/periodic snapshots at roughly 30 Hz; test coverage is still required. New SHA and reviewer closure pending.

## Baseline correction for H and final build scope

- The accepted baseline contains the native `Jamma/src/ConsoleWindow.*` within Jamma.exe and `console/*` shared headers. `JammaConsole/src/Main.cpp` and a JammaConsole project are absent from tracked files and `Jamma.sln`; only an ignored stale `JammaConsole/obj` directory exists in the primary checkout. `doc/performance-console.md` confirms no companion executable. H must inventory the actual native console input implementation, and final builds should target JammaLib/Jamma/native tests; the plan's JammaConsole companion-build item is obsolete on this baseline. No source should be invented for a missing project.

## F lane frozen for review

- Base `602d3abf90b5f62013d1e2634c6342833a168842`; logical commits `59fe939121ca462d69ab2c31781fda84e9362fd5` and `06d3bfc5295be9f2c09904962af7c4a0705879fe`; worktree clean. Independent Sol review dispatched; no merge yet.
- A07: gain faders opt into double-click exact linear 1.0 through existing smoothing/undo path. A09: take channel meters bind to actual Loop gain mixers; stopped loops publish zero, UI visibility does not govern audio publication; no callback locks/allocation/logging/UI traversal.
- Incremental Debug x64 JammaLib and native test builds exit 0; focused native run 239/239 passed, exit 0. Filter `Gui*:StationRack.*:AudioMixerMeter.*:BlockApi.*:CaptureSourceMute.*:ChannelMixer.*:StationRouting.*:StationFlipBuffer.*:StationRemote.*`; log in F worktree test output. Rendered meter/handle clearance, listening, resource recreation remain manual gates.
- Existing local loop depth has no dedicated gain control; remote station racks remain visible across depths. MIDI-only take master controls note velocity into a shared station instrument and has no independently attributable take audio signal. Do not display station-wide audio as a false take meter; verify station instrument output at station meter in manual checks.

## V-01 implementation follow-up

- V fix commit `6fc4e8fecccfddd30b7bd75c6c5cb373bd4f8825` over `ea15a7e6`. Single-pitch note max is `min(18 rendered take units, 25% usable take band)`, centered and invariant under radial scale. Multi-pitch mapping remains semitone proportional. New recording-growth test follows immutable snapshots at each ~30 Hz refresh and checks range `[60,60] -> [58,60] -> [58,84]`, refit of old notes, pitch identity, spacing and containment.
- Final incremental Debug x64 JammaLib/native-test builds exit 0; focused suite 249/249 passed in 31 suites, exit 0. Worktree clean. Reviewer closure at new SHA pending; no merge yet.

## V-01 reviewer closure and C lane start

- Independent Sol re-review of `ea15a7e6..6fc4e8fe` closed V-01 at fixed head `6fc4e8fecccfddd30b7bd75c6c5cb373bd4f8825`, with no remaining actionable findings. Reviewed rendered-scale cap, centering, shared range after snapshot consumption, ~30 Hz recording refresh and 249/249 focused test evidence. A08/A12/A13 accepted for integration, with full app smoke pending.
- `codex/alpha-camera` created at baseline SHA in `Jamma-alpha-camera`; local `.vscode`/`.agents` copied without overwrite and isolated x64-windows dependencies installed. Sol implementation agent owns A01/A04. C may rely on V's final camera-bounds contract but works in its own baseline branch; integration will check combined framing. Front zoom is confirmed; secondary view remains pending user identification per plan.

## F review finding F-01

- **F-01 (P2), `GuiSlider.cpp:147,263` at `06d3bfc`:** an in-progress second click can reset a hidden fader after Tab changes selection depth, because GuiRack ancestor hide does not clear the captured slider's reset-on-release state and Scene still delivers release directly. Reviewer also requests ancestor disable coverage. Owner F; separate fix commit and focused tests pending, followed by independent re-review. No other actionable A07/A09 findings; meter signal paths and MIDI-only exception were accepted by reviewer. No merge yet.

## D frozen for review and F-01 follow-up

- D base `602d3abf`; candidate head `2c5cf16a33ac18bd0ac90e4e30e521322c80d30c` after A05 commit `55d92f82f642eb66dffc0564dcf96171b1fee0e6`. Worktree clean; independent Sol review dispatched, no merge. Final Debug x64 JammaLib/Jamma/native-test builds exit 0 with zero warnings/errors; focused run 177/177 passed across 15 suites, exit 0. Native COM cancellation teardown took ~65s, so the focused run took ~88s. Audit script flags two off-callback Scene queue/startup locks by whole-file scope; manual callback-body verification and reviewer judgement pending. `doc/diagnostics.md` documents actual coverage and limits; real plugin/device/startup smoke remains pending.
- F-01 fix `6bf67f276b70cdd3b22bfa5ae492f4166d12df1a` over `06d3bfc`: captured slider release cancels when any GUI ancestor becomes hidden/disabled; hidden/disabled moves cannot alter gain. Added rack-hide and outer-ancestor-disable regressions. Incremental Debug x64 JammaLib/test builds exit 0; focused suite 241/241 passed, exit 0. Worktree clean. Independent focused re-review dispatched; no merge.

## F second review finding F-02

- At `6bf67f27`, reviewer confirms the original hidden double-click reset F-01 is fixed, but finds **F-02 (P2)**: cancelling an already-published normal drag after ancestor hide clears `_valueOffset` and visually restores the old gain. The hidden rack rejects the rollback action, leaving slider and mixer inconsistent, with no undo. Owner F; fix must preserve/commit the published drag gain coherently and test a real rack receiver with changed gain and undo behavior. Independent re-review required at the next SHA. F is not accepted for integration yet.

## D independent review findings

- **D-01 (P2), `ConsoleCapture.h:151`, `DiagnosticThread.h:47,67` at `2c5cf16a`:** explicit runtime job diagnostics still tee to startup.log while the global 30-second startup window is open (`GeneralStartup || StartupOwned()`). Require tri-state origin so explicit runtime overrides the general window; add interleaved runtime/startup job and backend tests before cutoff.
- **D-02 (P2), `Scene.cpp:1935,1941,2134,3839,3844` and `Trigger.cpp:1389`:** new catch surrounds only one queued receiver call; other job-thread operations can throw and terminate process without A05 diagnostic. Require outer worker boundary, deterministic off-callback diagnostic, safe stop/shutdown, and queued throwing-work test.
- Reviewer found no further actionable A11 issue and verified new Scene locks are off callback, with bounded atomic/TLS callback additions. D implementer owns fixes; fixed SHA/build/tests and independent re-review pending. D is not accepted for integration yet.

## F-02 follow-up and C frozen for review

- F-02 fix `3072c3d7103c427a098eb3301b0d78062820736a` over `6bf67f27`: captured release after ancestor hide/disable preserves already-published drag gain locally and returns pre-drag undo; no hidden rollback action. Real Station/GuiRack regression covers depth hide/disable, unavailable move, re-show and undo. Debug JammaLib/test builds exit 0; focused 242/242 passed, exit 0. Worktree clean. Independent re-review dispatched; F-01/F-02 closure pending.
- C candidate branch head `b65ab96ba08783324e4bc3c56dd3d8aa9ba3f888`, after A01 commit `16137752371f2118fcd2a7e191c1d2181c20e0cc`; worktree clean. Debug JammaLib/native-test builds exit 0; relevant camera/window/scene/selection regression filter 126/126 passed, exit 0, including five new cases. Fixed-head independent review pending; no merge. Secondary-view extension remains pending identification; Front work is complete for review.

## F reviewer closure and overlap map

- Independent Sol re-review at `3072c3d7103c427a098eb3301b0d78062820736a` closed F-02 with no actionable findings; F-01 remains fixed. Reviewer checked local drag commit, Scene undo capture, real rack test and gesture recovery. A07/A09 accepted for integration, with rendered/listening/manual checks pending.
- Expected shared-file overlap at integration: D/S/C all edit distinct `Scene.cpp` regions; D/C also edit `Scene.h` and `graphics/Window.cpp`; F/V both edit `Loop.cpp`, `Loop.h`, `LoopTake.cpp`. Resolve exact hunks deliberately and independently review any merge-only semantic edit. D -> S -> F -> V -> C order remains preferred. A clean Git merge will still receive combined behavioral checks.

## C independent review closure

- Independent Sol review of fixed `602d3abf..b65ab96ba08783324e4bc3c56dd3d8aa9ba3f888` found no actionable findings. It inspected Window->Scene input classification, held/repeat policy, actual navigation resets, sightline/aspect-derived Front capacity, projection/picking, V bounds aggregation and window-thread ownership. A01/A04 accepted for integration. Reviewer did not rerun builds/tests. Rendered many-station picking and combined V/C appearance remain pending; secondary view is intentionally not extended without identification.

## D-01/D-02 implementation follow-up

- D-01 fix `71b6b442ae288d73ab82bc64c59872e8f0f4f080`: explicit runtime origin overrides an open general startup window; unspecified output follows general window and explicit startup stays in startup.log. Added nested origin and interleaved Scene job/backend tests before and after cutoff.
- D-02 fix `f174d0cfb930e7c71c4c0ca68fea670e6b9e1587`: whole job-thread initialization/pump/final-save exception boundary, one failure diagnostic and atomic UI observation; orderly owner shutdown and no rig/default persistence after failed worker. Added public throwing-save, initialization, final-save/Shutdown, receiver and repeated-shutdown tests.
- Frozen D head `f174d0cf`, worktree clean. Final Debug x64 JammaLib/Jamma/native-test builds exit 0 with zero warnings/errors; expanded focused native suite 184/184 passed in 17 suites, exit 0. Native Windows picker cancellation returned correctly after ~122s shell teardown; total suite ~136s. No process kill. Independent focused re-review dispatched; D not yet accepted for integration.

## D second review finding D-03

- At `f174d0cf`, independent reviewer closes D-01 and confirms D-02 worker catch/park/join mechanics, but finds **D-03 (P2)**: job-thread failure flag is observed before captured fatal line is drained, and console window/transcript may be destroyed before worker final-save fails during Shutdown. With startup tee closed, runtime/final-save diagnostic can be lost entirely. Owner D must preserve failure information through join and provide owner-side visible/durable fallback without recursive logging, with tests for failure after last UI drain and final-save failure after console destruction. D-02 remains open pending this delivery fix and re-review; no D merge yet.
