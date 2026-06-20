# Jamma Automation Agent Brief

Purpose: concise orientation for agents touching automation recording/playback/visualization.

Related deep dive: [doc/automation-deep-dive.html](doc/automation-deep-dive.html)

## Scope

This covers:
- Editor-driven automation capture (VST param drags)
- CC-driven lane writes
- Audio-thread playback dispatch back to VST params
- Automation visuals (play position, rotation, alpha trail)
- Thread ownership and synchronization assumptions

---

## 1) End-to-End Flow (Editor Drag -> Lane -> VST)

1. VST2 editor touch triggers `audioMasterAutomate` in the host callback.
  - Host publishes the touched tuple: plugin pointer, parameter index, value, sequence.
  - Host does not echo the value back through `setParameter`; the plugin already updated its own parameter state before notifying the host.
  - refs: [JammaLib/src/vst/Vst2Plugin.cpp](JammaLib/src/vst/Vst2Plugin.cpp), [JammaLib/src/vst/IVstPlugin.h](JammaLib/src/vst/IVstPlugin.h)
2. Job-thread MIDI pump runs periodically and consumes touched tuples only while automation hold is armed.
  - The pump advances the last-seen sequence even while disarmed, so stale pre-arm touches are not replayed when the user arms later.
  - refs: [JammaLib/src/engine/Scene.cpp](JammaLib/src/engine/Scene.cpp), [JammaLib/src/midi/MidiRouter.cpp](JammaLib/src/midi/MidiRouter.cpp)
3. On each fresh touch sequence, the recorder resolves the target loop/lane, wires mapping if needed, computes the current loop sample, and overwrites a bounded future window with a held value.
  - Current implementation writes a point at the touch position and another at `touch + cooldown`, removing points that fall inside that window.
  - No lane points are written on idle pump cycles.
  - refs: [JammaLib/src/midi/MidiRouter.cpp](JammaLib/src/midi/MidiRouter.cpp), [JammaLib/src/midi/MidiLoop.cpp](JammaLib/src/midi/MidiLoop.cpp)
4. Non-audio thread rebuilds flat automation dispatch entries when wiring/topology changes.
  - ref: [JammaLib/src/engine/Station.cpp](JammaLib/src/engine/Station.cpp)
5. Audio callback reads the dispatch list, samples the lane at the latest sample in the block, and calls `SetParameter` at most once per mapped parameter per block.
  - Playback still uses per-parameter suppression and value delta gating.
  - refs: [JammaLib/src/engine/Station.cpp](JammaLib/src/engine/Station.cpp), [JammaLib/src/midi/MidiLoop.cpp](JammaLib/src/midi/MidiLoop.cpp)

---

## 2) Arming and State Landmarks

- Ctrl+Shift+A down:
  - sets record hold true
  - resets editor overwrite sessions
  - refs: [JammaLib/src/midi/MidiRouter.cpp](JammaLib/src/midi/MidiRouter.cpp)
- First touch after arm/expiry:
  - starts a fresh cooldown session for that plugin+param
  - writes one bounded hold window immediately
- No touch for cooldown window:
  - touch state expires
- Ctrl+Shift+A up:
  - disables hold and rebuilds dispatch
  - refs: [JammaLib/src/midi/MidiRouter.cpp](JammaLib/src/midi/MidiRouter.cpp)

Key distinction:
- Arm instant != record-start instant.
- Record starts on first consumed touch sequence after arm.
- Current implementation intentionally keeps the existing arm gesture; editor touches do not auto-arm recording.

---

## 3) Overwrite Semantics (Actual Current Behavior)

Current implementation is event-driven bounded overwrite, not progressive sweep and not hard whole-lane wipe:
- A fresh editor touch rewrites the half-open window `[touchSample, touchSample + cooldown)` with a held value.
- The implementation removes existing points inside that window, then writes one point at the window start and one point at the window end.
- Wrapped windows split naturally across the loop boundary.
- Idle PumpMidi cycles do not add points.
- Point insertion still merges near-equal fractional positions with epsilon `1 / 2048`.

Refs:
- touch handling: [JammaLib/src/midi/MidiRouter.cpp](JammaLib/src/midi/MidiRouter.cpp)
- window overwrite helper: [JammaLib/src/midi/MidiLoop.cpp](JammaLib/src/midi/MidiLoop.cpp)
- point write/merge: [JammaLib/src/midi/MidiLoop.cpp](JammaLib/src/midi/MidiLoop.cpp)

Important correction:
- Header and implementation comments now describe cooldown-window overwrite semantics rather than a sweeping write-head.

---

## 4) Playback Control and Suppression

Audio-thread dispatch behavior:
- Reads pre-baked dispatch list
- Skips entries under per-parameter suppression window
- Computes frac from the latest sample in the current block and loop anchor
- Interpolates lane value with cursor progression
- Calls SetParameter only if delta exceeds AutomationEpsilon

Refs:
- dispatch loop: [JammaLib/src/engine/Station.cpp](JammaLib/src/engine/Station.cpp)
- suppression table check/update: [JammaLib/src/midi/MidiRouter.cpp](JammaLib/src/midi/MidiRouter.cpp)

Suppression semantics:
- Refresh happens only on actual received editor changes.
- Idle pump cycles do not keep suppression alive.
- Expiry is still held in the sample domain, not wall-clock time.

Plugin support detail:
- VST2 `audioMasterAutomate` now publishes the touch without host-side parameter echo: [JammaLib/src/vst/Vst2Plugin.cpp](JammaLib/src/vst/Vst2Plugin.cpp)
- VST2 playback SetParameter is wired: [JammaLib/src/vst/Vst2Plugin.cpp](JammaLib/src/vst/Vst2Plugin.cpp)
- VST3 SetParameter currently no-op in host: [JammaLib/src/vst/Vst3Plugin.cpp](JammaLib/src/vst/Vst3Plugin.cpp)

---

## 5) Time/Phase Math You Must Preserve

Core frac equation shape (recorder + playback):

frac = fmod(globalSample - loopPhaseAnchor, loopLen) / loopLen

Phase anchor is set when MIDI loop recording ends so play index and loop position align.
- ref: [JammaLib/src/engine/LoopTake.cpp](JammaLib/src/engine/LoopTake.cpp)

Recorder note:
- Editor-touch capture converts `globalSampleNow` to loop-sample space first, then rewrites the cooldown window in sample domain.

Visual play fraction uses loop/take index logic and is pushed into midi models each draw tick.
- refs: [JammaLib/src/engine/LoopTake.cpp](JammaLib/src/engine/LoopTake.cpp)

---

## 6) Visual Automation Quick Map

- Model rotation angle: TWOPI * loopIndexFrac
  - ref: [JammaLib/src/graphics/MidiModel.cpp#L136](JammaLib/src/graphics/MidiModel.cpp#L136)
- Play position fed to automation shader as PlayFrac
  - ref: [JammaLib/src/graphics/MidiModel.cpp#L415](JammaLib/src/graphics/MidiModel.cpp#L415)
- Lane geometry sampled from sparse points (piecewise linear)
  - ref: [Jamma/resources/shaders/automation.vert#L26](Jamma/resources/shaders/automation.vert#L26)
- Alpha/brightness trail around playhead via wrapped distance
  - ref: [Jamma/resources/shaders/automation.frag#L18](Jamma/resources/shaders/automation.frag#L18)

---

## 7) Thread Ownership (Do Not Blur)

Audio callback thread (RT):
- station WriteBlock, automation dispatch playback, plugin parameter writes
- refs: [JammaLib/src/audio/AudioHost.cpp#L107](JammaLib/src/audio/AudioHost.cpp#L107), [JammaLib/src/engine/Station.cpp#L340](JammaLib/src/engine/Station.cpp#L340)

Job thread:
- pump midi/serial, consume editor touches, refresh suppression
- refs: [JammaLib/src/engine/Scene.cpp#L1229](JammaLib/src/engine/Scene.cpp#L1229), [JammaLib/src/io/IoInputSubsystem.cpp#L32](JammaLib/src/io/IoInputSubsystem.cpp#L32)

UI thread:
- key handling for arm/wire/clear/lane controls
- ref: [JammaLib/src/engine/Scene.cpp#L470](JammaLib/src/engine/Scene.cpp#L470)

Render thread:
- model rotation push and automation draw/snapshot
- refs: [JammaLib/src/engine/LoopTake.cpp#L305](JammaLib/src/engine/LoopTake.cpp#L305), [JammaLib/src/graphics/MidiModel.cpp#L389](JammaLib/src/graphics/MidiModel.cpp#L389)

---

## 8) Safe Change Checklist for Agents

Before change:
- Confirm if change touches recorder, dispatcher, visuals, or all three.
- Verify thread context for every modified function.

If modifying overwrite behavior:
- Decide explicitly: bounded overwrite window vs whole-lane wipe.
- Keep docs/comments aligned across h/cpp.
- Validate lane point count and merge behavior under sustained writes.
- Validate wrapped overwrite windows across the loop boundary.

If modifying playback:
- Preserve suppression semantics and sample-domain comparisons.
- Preserve latest-in-block sampling unless deliberately changing timing behavior.
- Keep dispatch loop flat and RT-safe.
- Re-check VST2/VST3 behavior expectations.

If modifying visuals:
- Keep playFrac source consistent with LoopIndexFrac path.
- Validate alpha trail behavior around wrap boundary.

After change:
- Re-read all touched comments for semantic drift.
- Run relevant tests/builds and do a quick manual reasoning pass for race windows.

---

## 9) Remaining Items / Known Gaps

- VST3 parameter playback is still not implemented in the host. Editor/playback behavior described here is only fully true for VST2 today.
- Automation lane storage is still fixed-capacity (`2048` points per lane). The new editor-touch path avoids the old runaway PumpMidi writes, but the storage is not truly unbounded.
- Full-lane or dynamic-storage redesign has not been done. If dense automation still approaches the cap in real sessions, this needs a second pass.
- `AutomationEpsilon` in playback still exists as a delta gate. It is now far less likely to distort timing because playback samples block-end, but the policy is still conservative rather than eliminated.
- External CC-driven suppression refresh was left alone. The suppression fixes here are for editor-origin automation changes.
- `doc/automation-deep-dive.html` may still describe the old progressive overwrite model until separately refreshed.

---

## 10) Decisions To Confirm With Matt

- The existing Ctrl+Shift+A arm gesture was preserved. Editor touches do not auto-start recording.
- The VST2 host callback only publishes `audioMasterAutomate`; it does not echo the value back via `setParameter`.
- Playback now samples the latest sample in the block, not block start.
- The fix chose bounded overwrite windows over progressive sweep and over full-lane wipe.
- The fix kept the current fixed lane-point storage for now instead of widening scope into a larger container redesign.

If any of those decisions are wrong for the product direction, revisit them before expanding the automation work further.

---

## 11) Primary Files

- [JammaLib/src/midi/MidiRouter.cpp](JammaLib/src/midi/MidiRouter.cpp)
- [JammaLib/src/midi/MidiRouter.h](JammaLib/src/midi/MidiRouter.h)
- [JammaLib/src/midi/MidiLoop.cpp](JammaLib/src/midi/MidiLoop.cpp)
- [JammaLib/src/midi/MidiLoop.h](JammaLib/src/midi/MidiLoop.h)
- [JammaLib/src/engine/Station.cpp](JammaLib/src/engine/Station.cpp)
- [JammaLib/src/engine/LoopTake.cpp](JammaLib/src/engine/LoopTake.cpp)
- [JammaLib/src/graphics/MidiModel.cpp](JammaLib/src/graphics/MidiModel.cpp)
- [Jamma/resources/shaders/automation.vert](Jamma/resources/shaders/automation.vert)
- [Jamma/resources/shaders/automation.frag](Jamma/resources/shaders/automation.frag)
- [JammaLib/src/vst/Vst2Plugin.cpp](JammaLib/src/vst/Vst2Plugin.cpp)
- [JammaLib/src/vst/Vst3Plugin.cpp](JammaLib/src/vst/Vst3Plugin.cpp)
