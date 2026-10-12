# Jamma: Scene responsibility extraction plan

Reviewed: 2026-10-12
Scope: Refactor `JammaLib::engine::Scene` in small, reviewable steps. Implementation progress and validation limits are recorded below.

## Goal and feasibility

`Scene` is the job/UI orchestrator and top-level composition owner. It should continue to coordinate subsystem lifecycle, scene membership/publication, and presentation. This plan extracts cohesive implementation details while keeping audio, loop, remote-timing, and persistence ownership with their current subsystem owners, as required by [the glossary](glossary.md#ownership-boundaries).

The options below are feasible as a sequence of independent changes. They are not one mechanical move: selection and MIDI refresh cross existing ownership boundaries, session loading needs an explicit value-oriented interface, and audio advancement changes the callback seam. Complete one step at a time, validate it, then continue in the same implementation session if the resulting diff remains reviewable. Stop and report if a step requires changing the ownership contract described in `doc/glossary.md` or timing invariants in `doc/loop-alignment-and-ninjam-sync.md`.

## Current boundaries to preserve

- `Scene` is explicitly documented as the job/UI orchestrator; it does not own callback application, remote timing authority, or per-entity loop state. See [`Scene.h`](../JammaLib/src/engine/Scene.h#L3).
- `SceneSelector` recognizes selection gestures and retains hover/paint-path state. [`Scene::_UpdateSelection`](../JammaLib/src/engine/Scene.cpp#L2967) applies committed selection and mute behavior to `Station`, `LoopTake`, audio loops, and MIDI models under `_sceneMutex`.
- `IoInputSubsystem` already owns MIDI enumeration, router initialization/refresh, and closure. `Scene` currently owns refresh cadence/retry policy, connection snapshots, lost-device confirmation, rig availability publication, and HUD routing updates. See [`IoInputSubsystem.h`](../JammaLib/src/io/IoInputSubsystem.h) and [`Scene::_RefreshMidiIfNeeded`](../JammaLib/src/engine/Scene.cpp#L1909).
- `GuiMainPanel` composes settings pages; its contract says `Scene` supplies controls and command identity. Keep engine behavior out of generic GUI composition. See [`GuiMainPanel.h`](../JammaLib/src/gui/GuiMainPanel.h#L18).
- `IoSessionExporter` is an existing persistence boundary, but `Scene::FromFile` also constructs the Scene and participates in startup composition. A loader must return data to Scene instead of reaching into its private state. See [`Scene::FromFile`](../JammaLib/src/engine/Scene.cpp#L517).
- NINJAM diagnostic records are drained and formatted on the job thread. `NinjamNetworkService` owns neither diagnostic state nor formatting; preserve this boundary and the `verbose` gate. See [`doc/ninjam.md`](ninjam.md#bounded-timing-diagnostics) and [`Scene::OnJobTick`](../JammaLib/src/engine/Scene.cpp#L1831).
- `graphics::Skybox` currently owns GL resources and drawing; `Scene::DrawBackground` owns animation start time and transform composition. See [`Skybox.h`](../JammaLib/src/graphics/Skybox.h) and [`Scene::DrawBackground`](../JammaLib/src/engine/Scene.cpp#L783).
- `AudioHost` owns callback entry, timing application, and the published station snapshot. `Scene::InitAudio` currently injects `Scene::OnTick` as a callback; `Scene::OnTick` ticks the quantisation clock and advances the borrowed station list. See [`AudioHost.h`](../JammaLib/src/audio/AudioHost.h) and [`Scene::InitAudio`](../JammaLib/src/engine/Scene.cpp#L2352).

## Ordered implementation steps

Each step has a narrow completion check. Do not combine steps if doing so hides a behavior change or makes a regression difficult to localize.

### 0. Establish a behavior baseline

Before editing, inspect the current implementation and applicable tests. Record the existing lock, thread, ordering, and publication contract for the selected extraction. Keep this plan and `doc/glossary.md` aligned if implementation discoveries change a boundary.

**Complete when:** the implementation diff for the first extraction identifies the old owner, new owner, callers, state moved, and preserved behavior; no speculative shared “manager” is introduced.

### 1. Move skybox animation math behind the graphics boundary

`Scene::DrawBackground` computes elapsed time and yaw/pitch/roll before drawing. Extend `graphics::Skybox` with a small transform/animation calculation interface, leaving the animation start time and scene/camera context in Scene. Do not move GL context ownership, resource lifetime, or unrelated editor shader timing.

**Validation:** build `JammaLib`; inspect the background render path and perform a visual smoke check when the app is available. Confirm the initial transform and animation remain continuous and the transform still excludes camera translation.

**Risk:** low. There is no current dedicated Skybox test seam; keep calculations deterministic and independently testable if practical.

### 2. Extract NINJAM diagnostic presentation bookkeeping

The job thread drains timing diagnostics and tracks duplicate/overflow presentation state. A small presenter may own formatting and presentation deduplication, but it must consume diagnostics already produced by `NinjamTimingCoordinator`; it must not create timing authority, add callback work, or move mutex-serialized service policy. Keep output off the audio callback and behind the existing verbose-event setting.

**Validation:** add focused coverage for event formatting/deduplication where a stable seam exists; run the relevant NINJAM diagnostics tests and native test target. Compare emitted diagnostics for sequence changes and overflow against current behavior.

**Risk:** low to medium. Avoid duplicating or reinterpreting coordinator diagnostics. `doc/ninjam.md` must remain accurate if the presentation owner changes.

### 3. Extract remote-tempo prompt presentation

Move popup construction, proposal text/layout, and displayed proposal identity to a `gui::NinjamTempoPrompt` only if it can communicate a typed accept/reject decision without depending on the network service. Scene continues to coordinate the decision with `NinjamNetworkService`; UI actions remain on their current owning thread.

**Validation:** cover accept, reject, replacement by a newer proposal, and prompt dismissal. Manually verify popup routing/capture and that no stale proposal is accepted after replacement.

**Risk:** medium. Confirm thread ownership before moving callbacks: prompt rendering/actions are UI work, while proposal updates may arrive during job-thread pumping.

### 4. Extract settings-control construction and bindings

Move concrete settings widgets, labels, ranges, and control-to-typed-change conversion out of Scene. `GuiMainPanel` should remain a generic page composer; Scene should continue forwarding accepted setting changes to subsystem owners. Do not move engine side effects into `gui` classes. Inspect [`Scene` constructor control setup](../JammaLib/src/engine/Scene.cpp#L126) and [`Scene::OnAction(GuiAction)`](../JammaLib/src/engine/Scene.cpp#L1523) at implementation time because line numbers can drift.

**Validation:** retain command IDs and existing numeric bounds/defaults; cover every settings page/control, including invalid or absent numeric values; manually check touch, keyboard focus, hide/release behavior, and settings persistence.

**Risk:** medium. `GuiNumericInput` parsing semantics and control lifetime/receiver binding must be established from its implementation before designing the typed-change interface.

### 5. Extract hierarchy selection application

Keep gesture recognition and path capture in `gui::SceneSelector`. Move committed selection/mute application only after defining a caller-owned locked-station-view contract. The applying component must preserve station-to-take aggregate selection, single-selected loop/model behavior, mute painting, picking/hover visuals, and rack collapse rules. Avoid making generic GUI code depend on engine entities; prefer an engine/presentation-side component if it must name `Station`, `LoopTake`, or loop models. Scene retains `_sceneMutex` ownership and coordinates editor/rack consequences unless the extraction proves a narrower safe contract.

**Validation:** extend the existing selection tests in `test/JammaLib_Tests/src/engine/Trigger_Tests.cpp` and GUI gesture tests in `test/JammaLib_Tests/src/gui/GuiControls_Tests.cpp`. Cover station/take/loop depth, MIDI and audio loop selection, add/remove, clear, mute/unmute painting, hover changes, and rack visibility.

**Risk:** medium to high. `_UpdateSelection` is not just gesture handling; it accesses a mutable hierarchy under `_sceneMutex`. Do not pass an unlocked `_stations` reference or move locking to a controller that can deadlock through Scene callbacks.

### 6. Narrow the MIDI refresh policy boundary

Do not move `_RefreshMidiIfNeeded` wholesale into `IoInputSubsystem`: that subsystem already performs inventory and router operations, while refresh consequences include Scene-owned rig, trigger, audio-publication, and HUD state. First isolate the refresh decision/state (poll interval, explicit-refresh retry behavior, inventory equality) behind a small value-based policy seam or explicit request/result API. Keep device operations in `IoInputSubsystem`; keep lost-device confirmation and rig/HUD consequences in Scene/RigCoordinator unless a new owner can accept explicit snapshots without reaching into Scene internals.

**Validation:** cover periodic versus requested refresh, transient inventory errors, unchanged inventory with failed/missing connections, successful changes, and shutdown races. Verify connected-name deduplication and that a transient enumeration failure never tears down working ports. Existing input/router tests should be reused; add tests for any extracted policy.

**Risk:** high. `_RefreshMidiIfNeeded` currently serializes lifecycle with `_midiLifecycleMutex`, reads published `_midiState`, calls device operations, and then mutates trigger/rig/UI state. Preserve that ordering and do not move locks across callback boundaries.

### 7. Separate session reconstruction from Scene installation

Consider an `io::IoSessionLoader` complementary to `IoSessionExporter`. It may parse/validate saved content and return reconstructed stations plus validated restoration values. It must not construct or mutate Scene through private access. Keep ordered installation, rig publication, timing-clock setup, and startup/shutdown coordination in Scene. Reuse `Station::FromFile` and `RigCoordinator::BuildInitial` only after checking their current contracts and ownership.

**Validation:** exercise successful load, malformed/unsupported input, empty/recovery session behavior, trigger-history restoration, and saved transport validation with existing `Scene::FromFile` coverage in `test/JammaLib_Tests/src/audio/AudioFlow_Tests.cpp`. Confirm failure still leaves the caller able to apply its documented recovery path.

**Risk:** high. `Scene::FromFile` currently crosses parsing, entity construction, rig setup, and Scene installation. If a clean value result would duplicate a large ownership graph or force persistence policy into engine code, defer this extraction and record the blocker instead of creating a loader that merely renames the coupling.

### 8. Move block advancement to AudioHost, only with an explicit callback contract

The current `Scene::OnTick` body ticks the quantisation clock and calls `Station::OnTick` on `AudioHost::AudioStationsForCallback()`. `AudioHost` already owns the callback and timing clock, but Scene supplies the clock and installs the callback closure. Prototype a narrow advancement callback/interface that gives AudioHost the same clock and station snapshot it already owns; avoid adding Scene/job/UI work, locks, allocations, or unbounded traversal to the callback. Preserve ordering relative to desired timing application, trigger advancement, and mixing exactly.

**Validation:** extend the AudioHost boundary coverage in `test/JammaLib_Tests/src/audio/AudioFlow_Tests.cpp` (including `AudioCallbackRealtime.PreparedCallbackWithSceneTickDoesNotAllocate`) to verify clock tick count, station tick count, block size, stream parameters, timing replacement ordering, empty station snapshots, and stream reinitialization. Manually audit `Scene::OnTick`, `AudioHost::AudioCallback`, and `AudioHost::_OnAudio` against [`doc/realtime-audio.md`](realtime-audio.md#hot-path-review-checklist). Build affected projects and run the native suite.

**Risk:** highest. A regression changes the real-time callback behavior. The step is blocked if AudioHost cannot access the exact shared Timer instance without making it own Quantiser policy or introducing unsafe publication. Do not proceed by copying a clock or maintaining a second station list.

## Responsibilities that remain in Scene

- Station membership, enumeration, snapshots, and publication.
- Ordered subsystem startup and shutdown.
- The rig-publication handshake in `_AdvanceRigPublication`; this coordinates existing owners and is a Scene orchestration responsibility.
- Root draw ordering, resource traversal, and camera coordination.
- Thin forwarding for quantisation and NINJAM timing values where the real policy already lives in the corresponding owner.

## Session-level completion and validation

After every completed extraction, inspect the diff for accidental subsystem coupling, changed public behavior, stale comments/includes, and duplicated state. Update this plan or `doc/glossary.md` only when implementation proves the documented boundary should change.

For code changes, read the local `.vscode/tasks.json` before each build or native-test run; use its explicit command and tool path. Follow [`doc/build.md`](build.md#build-rules), build only affected projects, and add/update tests for behavior changes. Likely targets are `JammaLib\JammaLib.vcxproj` and `test\JammaLib_Tests\JammaLib_Tests.vcxproj`; GUI/application changes may also require `Jamma\Jamma.vcxproj`. If the applicable local task is missing, record the validation limitation rather than guessing an MSBuild path. Use incremental builds.

The later master session is complete when each attempted step has its focused regression coverage and validation evidence, the app preserves the ownership and real-time constraints above, and every deferred extraction has a concrete blocker recorded here or in the session report.

## Implementation session — 2026-10-12

- Steps 1–2 implemented: deterministic Skybox animation transform and a job-thread diagnostic presenter.
- Step 3 implemented with the agreed UI-owned prompt, session/revision decision token, visible replacement, stale-decision rejection, and separate user-dismissal versus obsolete-UI cleanup. Scene only reconciles presentation and forwards decisions. The serialized Coordinator owns token validation and resolution. Phase observations retain the token; reconnects and replacements invalidate it. Pending prompts are cleared on disconnect/source loss even when transport is already local; the existing observation-deadline exception is retained.
- Verification so far is source/diff review and the repository hot-path audit only. No tests were added or run, and no builds or live smoke checks were run, per the session instruction. These extractions remain uncompiled and unverified in the live app.
- Step 4 implemented: `gui::SceneSettingsControls` owns widget construction, settings entries, command identity, and typed action conversion. Existing edit notifications and owner-value fallbacks are retained. As agreed, finite numeric MIDI-channel values clamp before integer conversion; nonfinite values retain the accepted channel.
- Step 5 implemented: `engine::HierarchySelection` applies committed selection/mute through a caller-owned locked station view. Scene retains locking, gestures/picking, and rack/editor consequences.
- Step 6 implemented: `midi::MidiRefreshPolicy` isolates cadence, inventory comparison, and explicit missing/failed-device retry decisions. Device operations, lifecycle gates, connection confirmation, and rig/HUD publication remain in their existing owners.
- The hot-path audit script flags the moved, pre-existing scene lock in the settings UI action handler. Manual diff review confirms no audio-callback lock/wait additions or callback changes.

- Step 7 implemented: `io::IoSessionLoader` reconstructs stations and matching descriptors, prepares routing presentation values, validates saved master-position values, and restores histories into a fresh initial rig. Scene retains creation, rig acceptance, installation/publication order, timing-clock setup, and failure/recovery behavior.
- Existing test helper accesses to moved settings widgets were mechanically redirected to the new owner. No assertions or test cases were added or changed, and nothing was run.

- Step 8 implemented: `AudioHost::InitWithBlockAdvancement` prepares a callback retaining the exact shared Timer once off-callback, publishes that same clock after stream teardown and before startup, and advances the existing borrowed station list at the unchanged callback point. The existing custom callback API remains available; `Scene::OnTick` is a compatibility forwarding adapter. No clock copy, second station list, callback locking, or callback allocation was introduced. Build/native/real-time allocation and live validation remain pending explicit user request.

- Final static review found an adjacent pre-existing ownership risk: job-thread MIDI refresh calls `GuiHud::SetRoutingConfig`, which can close the shared popup manager on a routing revision change while UI input dispatch accesses that manager without the Scene lock. Tempo-prompt extraction does not remove this trigger-delete-popup path. The user subsequently authorized fixing this by moving routing presentation reconciliation to the UI thread, with class-scoped responsibility and minimal Scene plumbing. The fix is now implemented: GuiHud owns an atomic immutable latest-value routing presentation handoff; runtime producers publish under their existing Scene lock and the UI-owned CommitChanges path applies it before resource traversal, VUs and drawing. Existing SetRoutingConfig rebuilding and revision invalidation are reused; obsolete delete confirmations are removed with an exact-element popup close, including buried ones, preserving source-identity and tempo popups. Audio/input publication and admission ordering remain unchanged. See Jamma-Scene-refactoring-review.md for caller ownership, shared-state lifetime, static evidence and validation limits.

- Authorized source work is complete for steps 1–8 and the HUD ownership fix. Final git diff --check passes. The threading audit still flags the moved pre-existing settings UI lock; manual callback/diff review found no new callback synchronization or allocation. No builds, tests or live checks were added/run, per the user instruction; compilation and behavioral confirmation remain pending explicit authorization.
