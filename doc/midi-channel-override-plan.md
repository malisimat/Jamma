# MIDI Channel Override Plan

## Goal

Add a global live MIDI input override that can force any incoming channel-voice MIDI message to appear as though it arrived on a different channel.

User-facing behavior:

- Override state is numeric `0` through `16`, where `0` means `Omni` and `1` through `16` force that channel.
- `Alt+PageUp` increments the forced channel.
- `Alt+PageDown` decrements the forced channel.
- Decrementing from `1` goes to `0`.
- Incrementing from `0` goes to `1`.
- Pressing `Alt+PageUp` and `Alt+PageDown` together resets to `0`.
- The reset gesture is dual-held, order-independent, and should fire once when the second key goes down.
- The shortcut must work regardless of focused text or numeric editors.
- A single always-visible global numeric control shows `0` through `16`.
- The control is editable and clamped to the inclusive range `0..16`.
- Value `0` acts as `Omni` internally. Showing the text `Omni` is out of scope for this change; the control can display `0` for now.
- The override is runtime-only and resets to `0` on startup or session load.
- Only channel-voice MIDI messages (`0x80` through `0xEF`) are rewritten. System/common/realtime messages pass through unchanged.
- The MIDI pump only reads a narrow atomic snapshot and rewrites a local event copy. It never mutates GUI state, never calls into the audio thread, and should stay allocation-free and lock-free on this path.

## Agreed Semantics

The override is applied once at MIDI ingress, before any downstream routing or interpretation.

That means the effective channel is the one seen by all of these paths:

- station live MIDI channel filtering
- trigger dispatch
- MIDI learn capture
- automation CC recording/wiring
- live MIDI recording into takes
- held-note tracking and synthetic note-off reconciliation that uses the rewritten event

This avoids split-brain behavior where one subsystem sees the physical channel and another sees the forced channel.

## Implementation Shape

### 1. Own the override state in `MidiRouter`

Primary code owner: `JammaLib/src/midi/MidiRouter.h` and `JammaLib/src/midi/MidiRouter.cpp`.

Add a compact global state owned by `MidiRouter`:

- forced channel, stored as `0..16`, where `0` means `Omni` and `1..16` map directly to MIDI channels
- pressed-state tracking for `Alt+PageUp` and `Alt+PageDown`, owned by the main/UI shortcut path
- a small helper that determines whether a key transition should increment, decrement, or reset

Recommended internal representation:

- `std::atomic<std::uint8_t> _forcedInputChannelOneBased{ 0u };`
- non-atomic key-held booleans if mutated only on the main/UI thread; if the shortcut handler ever moves off that thread, promote them to atomics before wiring the callback
- the MIDI pump must not read or write the key-held booleans

Recommended helper split:

- `static std::uint8_t RewriteIncomingChannel(std::uint8_t status, std::uint8_t forcedOneBased) noexcept;`
- `actions::ActionResult HandleChannelOverrideKey(const actions::KeyAction& action);`
- `void StepChannelOverrideUp() noexcept;`
- `void StepChannelOverrideDown() noexcept;`
- `void ResetChannelOverride() noexcept;`
- `std::uint8_t ForcedChannelOverrideOneBased() const noexcept;`

Keep the rewrite helper tiny and branch-light so it stays safe to use in the pump path. The only shared state read on the pump side should be the forced-channel atomic snapshot.

### 2. Rewrite MIDI at ingress before dispatch

Primary code owner: `JammaLib/src/midi/MidiRouter.cpp`, inside `PumpMidi(...)`.

Current flow already drains `MidiEvent ingress` from each input queue and then fans out to:

- `_DispatchMidiTriggerEvent(...)`
- station live MIDI delivery
- CC learn and automation handling
- note recording into armed takes

Plan:

- Create a local `effectiveIngress` copy per popped message.
- If override is `0`, `effectiveIngress = ingress`.
- If override is `1..16` and the status is channel-voice, replace only the low nibble of the status byte.
- Use `effectiveIngress` consistently for all downstream consumers listed above.
- Keep the original `ingress` only if logging or future diagnostics need access to the physical source message.
- Load the forced-channel atomic once per event, then branch locally. Do not query GUI state, take extra locks, or allocate while rewriting in the pump.

Important detail:

- Rewrite only the channel nibble.
- Preserve message type and data bytes unchanged.
- Do not touch system/common/realtime messages because they have no channel nibble.

### 3. Handle hotkeys before focused editors

Primary code owner: `JammaLib/src/engine/Scene.cpp`.

Current `Scene::OnAction(KeyAction action)` gives focused widgets first refusal, then blocks global shortcuts while editing text. That ordering must change for this feature because the shortcut is explicitly meant to bypass focus. The shortcut path itself stays on the scene/UI thread; do not route it through any audio callback or background worker.

Plan:

- Add a dedicated early shortcut branch before the focused-control block.
- Detect `Alt+PageUp` and `Alt+PageDown` on key down and key up.
- Forward those transitions to `_inputSubsystem`, which forwards to `MidiRouter`.
- If the router reports that it consumed the shortcut, return immediately so text boxes never see it.

Recommended shape:

- Add `IoInputSubsystem::HandleChannelOverrideKey(...)`.
- Add `MidiRouter::HandleChannelOverrideKey(...)`.
- Call that path in `Scene::OnAction(...)` immediately after popup handling and before focused-control dispatch.

Reason:

- This preserves the existing focus-first model for normal keys.
- Only the explicit channel-override hotkey gets elevated above editor focus.

### 4. Implement dual-held reset cleanly

Primary code owner: `JammaLib/src/midi/MidiRouter.cpp`.

Gesture requirements:

- order-independent
- both keys must be held simultaneously
- fire reset once on transition into dual-held state
- no repeated resets while both keys remain held

Recommended approach:

- Track `pageUpHeld` and `pageDownHeld`.
- On each key-down transition, update the relevant held flag.
- After updating the flag, if both are now held and they were not both held before, reset to `0` and consume the event.
- On key-up transitions, clear the relevant flag and do not step channels.
- Only perform increment/decrement on key-down transitions when the dual-held reset did not just trigger.

This avoids timer-based gesture ambiguity and keeps behavior deterministic.

### 5. Add a single global always-visible numeric control

Primary code owner: `JammaLib/src/engine/Scene.h` and `JammaLib/src/engine/Scene.cpp`.

There is already a scene-level overlay draw path used for the version label and overlay controls. Reuse that surface instead of putting this feature inside the per-station rack UI, because station racks already represent station-local MIDI channel filtering.

Plan:

- Add one new scene-level numeric GUI control near the existing overlay label.
- The displayed value can be raw numeric text for now: `0` through `16`.
- Interpret displayed value `0` as omni mode. A prettier `Omni` label is explicitly out of scope for this change.
- Clamp typed or committed values to the inclusive range `0..16`.
- Have `Scene` poll a read-only getter from `IoInputSubsystem` or `MidiRouter` on the scene/UI thread and update the control during the normal scene update/draw path. Do not push GUI changes from `MidiRouter` or any pump thread.

Practical UI options:

- create a tiny dedicated scene-level numeric control if the current generic controls do not already support constrained integer editing cleanly
- or extend an existing numeric/text control only if that can be done without inventing new cross-thread GUI callbacks or broad new validation plumbing

Recommended bias:

- prefer the smallest existing numeric-editing primitive that already supports focus, commit, and clamped integer validation on the UI thread
- otherwise use an existing control to keep the diff small

### 6. Support clamped numeric editing on the control

Primary code owner: the chosen scene-level GUI control implementation.

Editing behavior:

- the user can enter or adjust a numeric value in the inclusive range `0..16`
- committing a value below `0` clamps to `0`
- committing a value above `16` clamps to `16`
- value `0` maps to omni mode, with no special display formatting required yet

Plan:

- when the control commits a numeric value, write the clamped result through the narrow main-thread API into `MidiRouter`
- refresh the displayed value immediately after the change
- keep this interaction on the scene/UI thread alongside keyboard handling; the control should only call into the router through a narrow main-thread API

If the current GUI layer does not expose a small constrained-integer edit surface, add the smallest possible targeted extension instead of over-generalizing the input model.

### 7. Keep station-local MIDI channel filters unchanged

Primary code owners: `JammaLib/src/engine/Station.cpp` and `JammaLib/src/gui/GuiRack.cpp`.

No behavioral change is needed for station-local allowed MIDI channel filters.

Expected interaction:

- physical input arrives on any channel
- global override may rewrite that channel at ingress
- each station then applies its existing allowed-channel mask to the rewritten channel

This preserves current station semantics while making the global override a deliberate pre-filter transform.

## Validation Plan

### Unit or narrow native tests

Preferred location: `test/JammaLib_Tests/src/midi/`.

Add focused tests around extracted helpers where possible:

- rewriting `NoteOn`/`NoteOff`/`CC`/other channel-voice statuses updates only the channel nibble
- system messages are unchanged
- `0` leaves everything unchanged
- `Alt+PageUp` from `0` becomes `1`
- repeated increment clamps at `16`
- decrement from `1` becomes `0`
- repeated decrement at `0` stays `0`
- dual-held reset clears to `0`
- dual-held reset fires once and does not immediately also increment or decrement on the same transition
- committed numeric values clamp to `0..16`

If direct `Scene` GUI testing is awkward, keep most logic in tiny router or helper functions that native tests can exercise without constructing the full window stack.

### Manual verification

Manual checks after build:

- confirm the numeric control is visible immediately after startup and shows `0`
- confirm `Alt+PageUp/PageDown` work while a text box or numeric input is focused
- confirm both shortcuts together reset to `0`
- confirm CC learn and automation wiring report the forced channel, not the physical one
- confirm station MIDI filtering reacts to the forced effective channel
- confirm recording/playback captures events on the effective channel
- confirm direct numeric edits clamp correctly to `0..16`
- confirm `0` behaves as omni mode even though the control still displays numeric `0`

## Risk Notes

### Focus ordering risk

If the shortcut is implemented after focused controls, the feature will fail in exactly the cases you asked to support. The shortcut must be checked before editor controls consume keys.

### UI ownership risk

Do not place the numeric control inside `GuiRack` unless you explicitly want a station-local duplicate. The feature is global; the rack is station-local.

### Semantic drift risk

Do not rewrite only note routing while leaving trigger/automation paths untouched. That would create inconsistent effective channels across subsystems.

### Cross-thread state risk

Only the forced-channel byte should cross between threads, and only via atomic load/store. Keep the key-held gesture state and numeric control state on the scene/UI thread. Never let the MIDI pump call into scene or GUI objects.

### Hot-path risk

The rewrite must remain a local status-byte transform in `PumpMidi(...)`. No locks, no GUI reads, no allocations, and no extra subsystem hops belong in the MIDI pump path.

### Input gesture risk

Avoid timing windows or debounce heuristics for the reset gesture. The dual-held state transition is deterministic and simpler to test.

## Suggested Implementation Order

1. Extract and test the channel rewrite helper.
2. Add override state plus increment/decrement/reset methods in `MidiRouter`.
3. Apply the rewritten event in `MidiRouter::PumpMidi(...)`.
4. Add pre-focus `Alt+PageUp/PageDown` handling in `Scene` through `IoInputSubsystem` into `MidiRouter`.
5. Add the scene-level numeric control and wire it to the router state.
6. Add clamped numeric editing behavior.
7. Run targeted native tests and a narrow Debug build for `JammaLib` or `JammaLib_Tests`.

## Concrete File Touchpoints

Expected primary edits:

- `JammaLib/src/midi/MidiRouter.h`
- `JammaLib/src/midi/MidiRouter.cpp`
- `JammaLib/src/io/IoInputSubsystem.h`
- `JammaLib/src/io/IoInputSubsystem.cpp`
- `JammaLib/src/engine/Scene.h`
- `JammaLib/src/engine/Scene.cpp`
- one scene-level GUI control file, either existing or new
- `test/JammaLib_Tests/src/midi/...` for focused helper and shortcut-state tests

Expected non-edits unless implementation friction forces otherwise:

- `Station.cpp` channel acceptance logic
- `GuiRack.cpp` per-station MIDI channel filter behavior
- session serialization in `JamFile.cpp` and exporter paths