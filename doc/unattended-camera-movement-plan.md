# Unattended camera movement — implementation plan

Status: implementation in progress; product decisions confirmed in the interactive session. Tests/builds and test additions deferred by explicit user instruction.
Repository: Jamma.

## Intended behaviour

After approximately 8 seconds without mouse, touch, or keyboard interaction delivered to Jamma, the normal front camera takes over with slow, heavy movement. It settles into an overview of all stations, then immediately moves close to each newly recording LoopTake as recording starts. A later recording redirects the motion immediately. Once all recording activity has finished and remained quiet for approximately 5 seconds, it gradually pulls back to the overview.

This is purposeful automatic framing, not random wandering. No global input hooks, cursor polling, or OS-wide idle detection. MIDI/serial triggers and audio/MIDI performances do not reset the human-interaction timer. Ordinary loop playback and its repeated boundaries do not keep the camera zoomed in forever.

## Defaults and decisions

- Idle delay: 8 seconds; post-recording quiet delay: 5 seconds. Use named tunable constants initially; a settings UI is outside this feature's first pass.
- Count genuine app-delivered pointer movement (including hover), button/touch down and up, dragging, wheel scrolling, and keyboard events, including shortcuts and keyboard performance triggers.
- Ignore internal GUI commands, deferred hover/picking refresh, synthetic capture cancellation, and pointer events generated solely by camera/UI refresh. Window::_ResamplePointer posts synthetic WM_MOUSEMOVE into the ordinary input path; use explicit provenance/suppression for these app-generated events, not position equality alone. Do not count nonzero relative movement twice or add new raw-input/global registration.
- Every accepted new layer causes immediate automatic focus once camera control is eligible, whether initiated by the UI or MIDI/serial. MIDI/serial does not count as human UI interaction. Notes, audio level changes, and continuous playback are neither idle resets nor new-recording focus events.
- Roaming is allowed only in Camera::View::Front. Station-central means the existing Camera::View::StationInterior; TopDown is also excluded. Scene's station/take selection depth is a separate concept.
- Disable roaming while the loop-grid editor owns the camera or a modal interaction is active. An actual pointer capture/drag or tracked held key must not time out mid-gesture. Clear held-input tracking on app focus/capture loss so a missing key-up cannot disable roaming permanently; this uses existing app messages only. Enumerate the real popup/modal states during implementation rather than assuming a general modal API. On returning to an eligible view or closing the editor/modal, start a fresh idle period.
- Overview framing includes every visible station and each visible LoopTake, including empty stations and takes; audio/MIDI content does not determine whether geometry is included. Hidden or ditched objects are excluded.
- If there are no visible stations, retain the current valid front pose rather than inventing a moving target.
- Every accepted new layer, including an overdub layer, gains focus (confirmed by the user). Punch/tail prevent the 5-second return while capture remains active, but do not steal focus without a new layer.
- Minimizing, hiding/dismissing, or deactivating the app suspends control; returning starts a fresh 8-second idle period (confirmed by the user).
- Per user instruction, do not add tests or run builds/tests in this session; implementation checkpoints use static review, with validation criteria deferred.

## Behaviour and priority

| Condition/event | Result |
| --- | --- |
| Genuine app pointer/key activity in any UI surface | Cancel automatic movement before dispatching the same input; retain the current pose; restart idle timing. |
| Excluded view, editor, modal, or held interaction | Suspend automatic control. Recording continues normally. |
| Eligible front view reaches 8 seconds idle, no capture active | Start slow inertial movement toward the all-stations overview. |
| Eligible front view reaches 8 seconds idle during a recording | Focus the latest currently recording new take immediately, without another idle delay. |
| New recording starts, including during overview return | Replace the focus target on the next eligible window update and start moving toward it, preserving motion continuity. A UI-held start is retained until that interaction ends; starts from hidden/excluded periods are not replayed on return. |
| Recording ends while another capture remains active | Keep close framing; do not start the overview countdown. |
| Last capture finishes, including end/tail processing | Start the 5-second quiet period. Any subsequent relevant start/end/cancel transition restarts it. |
| Five quiet seconds elapse with no active capture | Move gradually to the overview. |
| Target is ditched/deleted or scene is replaced | Discard invalid identities safely, re-evaluate live captures/bounds, and reset scene-local state as appropriate. |

Latest recording wins globally across stations, not whichever station happens to be last in a container scan. If the newest take ends before an older concurrent recording, hold the most recent valid framing until all captures finish; if that target disappears, fall back to the newest remaining recording, otherwise a safe overview.

Immediate response means update the movement target at recording start, normally by the next rendered frame after a held UI interaction ends. It does not mean teleport the camera. Human input cancels current automatic motion, but a new recording start remains pending through the held interaction that initiated it and then gains focus immediately. Starts from hidden or excluded periods are not replayed on return; ongoing captures can be focused after the fresh idle period. MIDI/serial starts focus immediately and do not reset the human-interaction timer.

## Existing code and integration points

| Area | Verified references | Implication |
| --- | --- | --- |
| App input | JammaLib/src/graphics/Window.cpp: Window::OnAction(TouchAction), OnAction(TouchMoveAction), OnAction(KeyAction), CancelMouseCapture, _ResamplePointer; app message dispatch and relative pointer handling | Mark genuine user activity here before input can be consumed by console/UI shortcuts or Scene handlers. Filter synthetic cancellation and refresh. |
| Frame update | JammaLib/src/graphics/Window.cpp: Window::Render; JammaLib/src/engine/Scene.cpp: Scene::UpdateCamera | Camera state is explicitly window-thread-owned. Extend the update gate to advance unattended motion as well as drag/transition motion. |
| Views and manual motion | JammaLib/src/graphics/Camera.h/.cpp: Camera::View, CycleView, SetViewTarget, _TickTransition, TickBackgroundDrag, _CoastBackgroundDrag | Existing transitions are short eased interpolations; actual inertia is currently drag-specific. Add a distinct automatic velocity-based target follower. |
| Editor ownership | JammaLib/src/engine/Scene.cpp: Scene::_OnLoopGridEditorOpened, _loopEditor.IsEngaged; Camera::CaptureEditorReturnState, RestoreEditorReturnState, SetEditorPerspective | Front-view enumeration alone is insufficient: the editor can use its own camera perspective and must remain in control. |
| Station observation | Scene::UpdateCamera; Camera::ObserveStation, CompleteStationObservation; Station::LoopTakeRevision | Preserve existing station-interior tracking. Membership revisions are not recording events. |
| Recording lifecycle | JammaLib/src/engine/Station.cpp: Station::OnAction(TriggerAction), _SetTakePresentation; JammaLib/src/engine/LoopTake.cpp/.h: Record, GetPresentation, IsArmed, IsCompletedRecording, CaptureFirstRecordBlockSceneAtAudioBoundary, FirstRecordBlockSceneSamps | Reuse published lifecycle state and accepted start ordering; distinguish request, applied start, capture, stop, and tail completion. |
| Safe membership | JammaLib/src/engine/Station.cpp: Station::GetLoopTakeSnapshot, GetVisualState; Scene::UpdateCamera and CommitChanges | Collect observations through established snapshots and existing off-audio serialization. StationVisualState alone loses take identity. |
| Geometry/projection | Scene::_StationCentre; Station::Draw3d, TopCapModelPosition; LoopTake::Draw3d; Camera::Projection | Centre averaging is not bounds fitting. Child world positions require parent translation and scale. |
| Tests | test/JammaLib_Tests/src/graphics/Window_Tests.cpp; test/JammaLib_Tests/src/engine/StationVisualState_Tests.cpp | Extend relevant input/lifecycle coverage and add deterministic controller/geometry tests in the existing native target. |

## Design

### 1. Single-owner controller

Add a small typed graphics/controller component owned by Scene, with Manual/Waiting, Overview, RecordingFocus, and QuietHold states (or equivalent explicit state). It receives time, eligibility, user activity, immutable recording observations, viewport information, and world-space bounds. It produces a desired camera target, not engine commands.

Use a monotonic UI clock compatible with Timer::GetTime/GetElapsedSeconds. Calculate 8-second/5-second deadlines from real elapsed time, separately from the capped physics step. Summing Scene::UpdateCamera's capped delta would incorrectly lengthen idle waits after frame stalls.

Keep focus as validated weak identity plus recording generation/order, never a retained raw pointer. Reset tracking on scene replacement. Observe lifecycle while manual/excluded as well, so re-entering unattended mode can find an ongoing recording without replaying old completed starts.

### 2. Reliable recording observations

Trace quantised Trigger scheduling through JammaLib/src/engine/Trigger.cpp: Trigger::StartRecording queues a structural command; Trigger::ProcessStructuralActionsOnJob dispatches it, reached through Scene::_PumpTriggerStructuralActions. Station creates the take and calls Record at the accepted structural start. This is the camera's preferred start event, beyond the initial physical input request; it is not proof that the first audio samples have already arrived. Verify scheduling and both audio and MIDI-only paths so the camera responds when the recording becomes active, without waiting for an arbitrary UI delay.

Get per-take presentation/lifecycle observations from the existing published membership and atomic presentation mechanisms. Track all active captures, not just the focus target. Observe Record, Overdub, Punch, Tail, completion, ditch, and removal without treating playback cycles as activity. Overdub/punch transitions update the global lifecycle-change time even when they do not steal focus. Verify Tail lifetime and clearing: presentation alone is not proof of ongoing capture, and a stale decorative tail must not prevent pullback indefinitely.

Station::_SetTakePresentation uses a station-local serial. These serials cannot order starts across different stations. Stamp accepted new-record starts with a Scene/job-owned monotonically increasing sequence at the serialized structural application point, handed to the window side as part of a complete coherent observation, or use an already coherent globally ordered applied-start observation if one is found during implementation. Do not assign ordering from the audio callback or from a window-thread container scan.

Do not rely solely on per-frame mode differences: a short recording may start and finish between rendered frames. Preserve latest start identity/order and last lifecycle change in the observation, alongside the current active capture set. Complete latest-value snapshots can retain that history without an unbounded event queue.

FirstRecordBlockSceneSamps is useful corroboration for audio capture but zero is also a valid scene counter, and MIDI-only recording need not produce an audio record block. It must not be used as the sole readiness flag. Prefer the applied recording transition; if additional audio-boundary evidence is essential, publish a bounded coherent observation using the established pattern.

### 3. Automatic camera physics and arbitration

Add automatic target-following position/zoom velocity to Camera or a dedicated graphics component. Use a damped spring or equivalent stable second-order follower, with bounded speed/acceleration and capped/substepped integration. Target changes preserve current pose and velocity so switching takes feels continuous.

Keep the front camera's existing orientation and projection initially; move its eye in X/Y/Z to pan and dolly. Avoid layering roaming, drag coasting, wheel zoom, and view transitions as simultaneous pose writers. There must be exactly one active motion owner.

On human input, cancel autonomous velocity/targets before dispatch, leaving the current pose usable by the normal drag/wheel handlers. Existing handlers can consume input while _transitioning, so ensure automatic motion does not swallow the takeover gesture. Preserve existing explicit view transitions and editor return-state semantics.

Do not call SetViewTarget every frame: it restarts the existing 0.4-second transition and ends drag state. Automatic target updates need their own continuous-motion path. Keep remembered manual poses distinct from unattended targets so mode cycling does not accidentally remember an automatic target as a user's view.

### 4. Overview and close-focus targets

Compute conservative world-space bounds from the same station/take transforms and geometry used by rendering: parent translation/scale, station deck/cap, populated take layers and loop extents. Prefer small explicit bounds accessors owned by their visual entities rather than duplicating mesh constants inside Scene.

For front orientation, use actual vertical FOV and aspect ratio to fit both width and height with padding; include depth extent. For a centred axis-aligned bound, a conservative eye distance is halfDepth + max(halfHeight/tan(vFov/2), halfWidth/(aspect*tan(vFov/2))), with padding and near-plane margin. Validate the resulting projected corners. The current normal front FOV is 80 degrees.

Overview is allowed to exceed the existing manual wheel front-zoom maximum (1350 world units) if necessary to fit all stations. Reconcile far clipping against the complete bounds; Camera::Projection currently derives its far plane from _StationCentre, which is insufficient for arbitrary deep layouts.

A recording-focus target uses that take's world-space visual centre and a tuned close dolly distance with near-plane safety. Do not fit the entire take: deliberately allow large loop/take geometry to extend outside the viewport, as requested. Derive centre/bounds from stable geometry, not audio-level animation, and follow take rearrangement without jitter.

Recompute targets when membership, layout, scale, or viewport aspect changes. A quiet overview can settle rather than moving perpetually. Invalid/minimized viewport dimensions must not generate non-finite targets or accumulated physics leaps.

### 5. Thread ownership and teardown

Camera/controller, human-activity timestamp, and automatic velocities: window thread writes/reads; destroyed with Scene/window lifecycle.

Recording ordering and observation publication: existing serialized job/engine owner writes; window thread reads immutable snapshots. Take presentations retain their existing atomic publication. Membership/layout reads remain under the established safe snapshot/scene-lock rules; gather values briefly and integrate camera physics after unlocking.

For every new shared value, record its writer, readers, publication primitive, and teardown path. Do not introduce callback locks, allocation, logging, camera calls, or callbacks into UI ownership. No transport, quantisation, loop-phase, or recording semantics change is required for camera policy.

## Implementation sequence

Complete these steps in order in one implementation session. Keep intermediate commits or checkpoints reviewable if the session is split. A step is complete only when its stated criteria pass; do not move camera motion into an audio callback to simplify publication.

1. **Confirm existing contracts and product defaults.** Trace accepted new-record, Overdub/Punch, completion, tail, ditch, MIDI-only, and serial-trigger paths. Confirm the proposed active-station definition and which modal/popup states own interaction. Confirm whether a dismissed window or minimized viewport suspends and restarts the idle timer. Record any deviation in the plan before coding. **Complete when** each observed lifecycle state has a concrete existing source or an explicitly scoped publication change, and the open decisions below are resolved.
2. **Add deterministic policy and observation models.** Implement the window-owned idle/quiet controller and the serialized-owner recording observation value, including scene generation, global accepted-start order, lifecycle-change identity, current active captures, and latest-start identity. Use Timer's monotonic elapsed-time source for deadlines, not accumulated capped frame delta. **Complete when** pure controller tests cover every policy-table transition, same-frame start/end, overlapping captures, and stale scene/take identity; publication ownership and teardown are documented beside the new state.
3. **Track real user interaction and takeover.** Mark genuine delivered pointer/key/touch/wheel activity before ordinary dispatch can consume it. Add explicit provenance for `_ResamplePointer` and other generated/cancellation messages; do not classify synthetic events by position equality. Track held gestures/keys and clear them on focus/capture loss. **Complete when** tests prove real consumed shortcuts reclaim control, synthetic refresh/cancellation does not reset idle, and the first takeover event reaches its existing handler without being swallowed by autonomous motion.
4. **Publish recording lifecycle observations.** Stamp accepted new-layer starts at the serialized structural application point and publish them with coherent current capture state; cover keyboard, MIDI, and serial routes, audio and MIDI-only takes, tail completion, ditch/removal, and concurrent starts. Keep the callback path bounded and lock-free; read membership through established snapshots/scene locking, and do controller work after releasing locks. **Complete when** tests prove cross-station global ordering, events between render frames are retained, Overdub/Punch/Tail prevent premature return according to the verified capture lifetime, and playback boundaries/notes/mute/layout changes do not create false starts.
5. **Add bounds and target calculation.** Obtain conservative world-space bounds from rendering transforms for all visible stations and takes, including empty takes, parent position/scale, and MIDI-only geometry. Implement overview fitting from actual front FOV/aspect and a deliberately clipped but near-plane-safe close target. Reconcile projection far clipping with the complete visible bounds. **Complete when** deterministic tests cover portrait/landscape, deep layouts, scale/rearrangement, empty layouts, invalid viewport sizes, and projected overview corners with padding.
6. **Add automatic motion and arbitration.** Implement bounded damped following with continuity across retargets, one pose writer at a time, and cancellation on human activity. Keep automatic targets separate from remembered manual view poses and explicit view transitions. **Complete when** deterministic tests cover variable frame rates, zero delta, long stalls, speed/acceleration bounds, switching during overview return, and manual drag/wheel/coasting behavior.
7. **Integrate eligibility and lifecycle in Scene/Window.** Advance policy from `Scene::UpdateCamera`, observe state even while manual/excluded, and gate on Front view, editor ownership, enumerated modal/popup states, held interaction, valid viewport, and scene/take validity. Reset to a fresh idle period when eligibility returns. **Complete when** the existing editor return state, StationInterior tracking, TopDown behavior, select depth, transitions, target deletion/replacement, and scene teardown tests pass without regression.
8. **Tune and validate end-to-end.** Run focused native tests and the required JammaLib native target using the repository's local build instructions; before each build/test read `.vscode/tasks.json` if present, and build incrementally. Then perform the manual acceptance sequence below with mouse/keyboard idle and MIDI/serial recording. **Complete when** timing, framing, and takeover satisfy acceptance without changing recording/transport timing; adjust only camera policy/tuning unless a separately justified engine contract change is required.

## Verification and acceptance

Deterministic native tests should cover:
- Idle threshold just below/at 8 seconds; real deadlines after a frame stall; no motion in excluded modes/editor/modal.
- Hover/move, drag, wheel, touch, keyboard and consumed shortcuts reset idle; synthetic hover/capture cancellation do not. MIDI/serial triggers and audio/MIDI notes do not reset idle.
- Held gestures do not time out, and the first real input cancels automatic movement without swallowing that input.
- Every new recording layer retargets on the next eligible frame, including a layer initiated through mouse/keyboard input after its held interaction ends and a start arriving during overview return.
- Globally newest take wins across stations; two starts in one frame and start/end between frames are retained deterministically.
- No return during a long recording or overlapping capture; end/tail completion followed by 5 quiet seconds returns gradually. New activity cancels the countdown.
- Ordinary playback, mute changes, layout revisions, and performance notes do not masquerade as new recordings.
- Quantised applied starts, audio capture at scene sample zero, MIDI-only recording, overdub/punch/tail, ditch and stale generation rejection.
- Deleting/replacing focused takes/stations and resetting scenes leaves no dangling target or stale event.
- Overview projected bounds fit landscape/portrait viewports with padding, including more stations than manual zoom limits accommodate.
- Close focus intentionally permits clipping of large take extents while avoiding near-plane collision.
- Retargeting preserves pose/velocity continuity; zero delta and long stalls are finite and bounded; comparable behaviour at differing frame rates.
- Editor return state, remembered manual views, selection depth, manual inertia/wheel and existing StationInterior tracking retain their behaviour.

Manual acceptance: leave the front view untouched for 8 seconds; watch slow overview settling; trigger recordings via MIDI/serial; verify immediate smooth focus, switching, sustained recording, tails, and five-second pullback; move/click/type to reclaim control; repeat in TopDown/StationInterior/editor and verify no roaming.

Before every implementation build/native test run, read local .vscode/tasks.json and follow doc/build.md. Build incrementally only affected projects; use absolute SolutionDir with exactly one trailing backslash for direct project builds. Run relevant and required native tests for JammaLib changes. If tasks are absent/inapplicable, report the limitation instead of guessing a tool path.

Apply .agents/skills/threading-review/SKILL.md during implementation: run the hot-path audit and inspect any touched callback-owned functions manually. Avoid anonymous namespaces; attach helpers/constants to explicit classes.

## Open decisions and implementation risks

- **Overview bounds:** include every visible station and visible take regardless of audio/MIDI content, including empty stations and takes.
- **Tail/capture lifetime:** `PresentationMode::Tail` is display state and may outlive active writes. Trace the exact writer lifetime before using it to hold framing; expose only a bounded explicit capture-active observation if existing state cannot answer this safely.
- **Global start order:** station presentation serials are local. If accepted starts cannot be stamped at one serialized Scene/job mutation point for every input route, stop and resolve the ownership boundary before implementing “latest wins”; do not approximate it by scanning stations.
- **Window interaction scope:** Win32 input messages, relative-pointer resampling, capture/focus loss, and generated cancellation must be tested together. The pointer-refresh path can otherwise keep the app perpetually non-idle or falsely reclaim control.
- **Geometry/projection:** actual take transforms and the far clipping plane may prevent reliable bounds fitting without small visual-entity accessors. Keep these accessors presentation-only and avoid duplicating mesh constants in Scene.
- **Explicit view transitions:** the automatic follower must not overwrite the camera while a view transition/editor restore owns it. If arbitration cannot preserve current view cycling semantics, resolve a single motion-owner contract before broad integration.
- **Review/build constraints:** no transport, quantisation, loop-phase, or audio-recording semantics are part of this feature. Any need to change them is a scope change. Follow `AGENTS.md`, `doc/realtime-audio.md`, and `doc/build.md`; do not guess a build tool when local task configuration is missing.

## Plan review record

Static review checked the referenced symbols and ownership boundaries against the current worktree. This plan separates manual idle from performance activity, applied recording starts from trigger requests, global ordering from station-local serials, front-camera eligibility from selection depth, and elapsed deadlines from capped physics integration. No build or tests were run as part of plan review.

This document is a plan, not proof that the feature works. Implementation and the acceptance checks above are still to be completed.


## Implementation checkpoint

Implemented the policy, input provenance, lifecycle observation, visual bounds,
front framing, automatic follower and Scene/Window integration in this worktree.
This is a code checkpoint; the test-based completion criteria above and step 8
have **not** been completed. The user explicitly prohibited adding tests and
running tests/builds during this session. Manual application acceptance remains
pending; the 8-second delay and slower heavy-camera motion were set at the user's
request.

Confirmed product choices:
- Every accepted new layer gains focus, including a new overdub layer.
- Overview includes every visible station and LoopTake, including empty stations and takes; media content does not gate framing.
- Hiding/dismissing, minimizing or deactivating the app suspends automatic
  control; returning begins a fresh 8-second idle period.

Verified contracts and implementation sources:
- Station's accepted Record and Overdub starts stamp one Scene-owned publisher
  under the existing structural scene lock. Keyboard, MIDI and serial all reach
  these paths; neither MIDI notes nor ordinary playback stamp new layers.
- LoopTake publishes structural presentation changes, cancellation and applied
  EndRecording. MIDI completion is immediate at Play; audio tails finish through
  the existing completion flag -> CommitChanges -> JOB_ENDRECORDING path.
- Deferred audio punch application additionally publishes an odd/even atomic
  revision. The job pump reads current capture truth and republishes retained
  lifecycle history; even a complete punch between frames restarts quiet timing.
  This adds no clock, allocation, lock, logging, camera call or UI callback to
  TriggerPunchInAudio/TriggerPunchOutAudio.
- UnattendedCameraController uses Timer's real monotonic deadlines, validates
  weak membership identities and retains the latest accepted start timestamp.
  A completed start after the idle deadline survives a frame stall, while old
  completed starts from manual mode are not replayed.
- Pointer resampling uses a dedicated window message and explicit action origin.
  Capture cancellation and synthetic modifier release do not count as activity.
  Real input cancels the follower before shortcut/UI/Scene dispatch. Held keys,
  touch and pointer capture suspend control; external loss clears tracking, while
  a normal completed owned mouse release preserves genuinely held keys.
- Eligibility includes Front view, view transitions, loop-grid editor engagement,
  GuiPopupManager (dropdown lists, delete popup, source information panel and
  remote-tempo prompt), held interactions and valid active/visible/enabled window.
  Native modal dialogs disable/deactivate the owner; external editor focus also
  suspends control. Window move/resize uses the existing message boundaries.
- Bounds accessors belong to StationModel, LoopModel, MidiModel, Loop, LoopTake
  and Station, preserving render translation/scale and audio/MIDI geometry.
  Overview fits Front's shared 80-degree FOV and aspect with padding and depth;
  close focus retains the take origin and only requires near-plane-safe depth.
  Far clipping includes complete visible depth rather than station averaging.
- Camera's capped/substepped damped follower preserves pose/velocity on retarget,
  bounds speed/acceleration and keeps the remembered manual Front pose separate.
  Explicit transitions, drag/coasting and editor control remain separate owners.

New shared-state ownership:
- CameraRecordingPublisher state: Scene lifetime; job/UI structural writers
  serialized by Scene::_sceneMutex; window reads immutable atomic shared-pointer
  publications; Station/LoopTake retain only weak publisher references.
- LoopTake publisher binding, scene generation, accepted start order and consumed
  punch revision: structural owners under the scene lock, not audio-accessed.
- LoopTake audio punch revision: audio-only writer, bounded atomic odd/even updates;
  job/UI structural readers; existing callback snapshots/sessions pin take lifetime.
- LoopModel waveform bounds height: existing waveform mutex protects job updates
  and window reads; destroyed with the existing model after reader teardown.
- Controller, eligibility, held input and automatic velocity: window-only, destroyed
  with Scene/Window. Scene generation rejects retired scene-local publications.

Static review: git diff --check passed. The repository hot-path audit script was
run; its file-wide scan reports two added locks, in LoopTake::HasCameraContent
(UI presentation query) and Scene::_ConsumeTriggerOutcomes (job reset serialization).
Caller/body review confirms both are off callback. Touched audio punch bodies were
manually reviewed for paired revision updates and absence of new waits, allocation,
logging or camera work. This static review is not a build or runtime validation.
