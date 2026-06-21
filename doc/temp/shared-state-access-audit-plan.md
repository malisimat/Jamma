# Shared-State Access Audit Plan

## Scope

This audit started from the recent render-time crash in `graphics::MidiModel::_InitAutomationGl(...)` and looked for similar bugs where shared state is mutated on one thread and read on another without a consistent ownership rule, mutex, atomic, or snapshot handoff.

The search focused on these surfaces:

- render/resource state in `GuiModel` and derived models
- `Scene` state shared across render, input, and job flows
- GUI tree mutation versus traversal
- lazy resource lifecycle flags
- existing snapshot-based patterns that should be preserved

## Summary

The codebase has two high-confidence shared-state bug families and two medium-confidence families.

High-confidence:

1. `GuiModel` resource vectors are protected for writes but still read unsafely in base and derived getters.
2. `Scene::_stations` has inconsistent synchronization and should move to a single snapshot/lock discipline.

Medium-confidence:

1. `GuiElement::_children` is mutated dynamically but only some traversals use snapshots.
2. `ResourceUser` lifecycle flags are plain `bool`s even though nearby code treats resource init as a cross-phase concern.

The strongest recommendation is to standardize on one of two patterns per state family:

- immutable snapshot handoff for collections shared across threads
- mutex-protected snapshot getters for mutable vectors/maps used by render code

Note: locking the audio thread is forbidden, audio hotpaths must be no heap, lock free and its performance prioritised.

## Findings

### A. High Confidence: `GuiModel` resource state still has unsynchronized reads

Relevant files:

- `JammaLib/src/gui/GuiModel.h`
- `JammaLib/src/gui/GuiModel.cpp`
- `JammaLib/src/graphics/LoopModel.cpp`
- `JammaLib/src/graphics/StationModel.cpp`
- `JammaLib/src/graphics/MidiModel.cpp`
- `JammaLib/src/midi/MidiModel.cpp`

Evidence:

- `GuiModel` has `mutable std::mutex _modelStateMutex` plus write-side locking for `_modelTextures` and `_modelShaders`.
- `GuiModel::InitTextures(...)` and `GuiModel::InitShaders(...)` assign those vectors under `_modelStateMutex` in `JammaLib/src/gui/GuiModel.cpp`.
- The default inline getters in `JammaLib/src/gui/GuiModel.h` still read `_modelTextures` and `_modelShaders` without taking that mutex.
- `GuiModel::Draw3d(...)` calls `GetTexture()` and `GetShader()` in `JammaLib/src/gui/GuiModel.cpp`, so render-time reads still rely on unsynchronized access.
- Derived overrides in these files also bypass the mutex:
  - `JammaLib/src/graphics/LoopModel.cpp`
  - `JammaLib/src/graphics/StationModel.cpp`
  - `JammaLib/src/graphics/MidiModel.cpp`
  - `JammaLib/src/midi/MidiModel.cpp`

Why this is similar to the crash:

- It is the same shape as the bug already fixed in `graphics::MidiModel`: shared vectors updated under one synchronization rule and read elsewhere with none.
- Even when a single read looks harmless, `std::vector` internals are not safe to inspect concurrently with assignment/reallocation.

Fix direction:

- Move the default `GetTexture()` and `GetShader()` implementations out of the header and into `GuiModel.cpp`.
- Make them take `_modelStateMutex` and return a weak-pointer snapshot captured under the lock.
- Patch every derived override to either:
  - lock `_modelStateMutex` before choosing an entry, or
  - call new protected helpers such as `GetShaderAtSnapshot(index)` / `GetFirstShaderSnapshot()`.
- Do not duplicate ad hoc locking in each caller. Centralize the access pattern.

Files to fix first:

- `JammaLib/src/gui/GuiModel.h`
- `JammaLib/src/gui/GuiModel.cpp`
- `JammaLib/src/graphics/LoopModel.cpp`
- `JammaLib/src/graphics/StationModel.cpp`
- `JammaLib/src/midi/MidiModel.cpp`

Notes:

- `JammaLib/src/graphics/MidiModel.cpp` was already patched locally for this exact pattern. The rest of the family should be normalized to the same rule.

### B. High Confidence: `Scene::_stations` needs a single synchronization model

Relevant files:

- `JammaLib/src/engine/Scene.h`
- `JammaLib/src/engine/Scene.cpp`
- `JammaLib/src/io/IoInputSubsystem.cpp`
- `JammaLib/src/io/IoSessionExporter.cpp`
- `JammaLib/src/timing/TimingQuantiser.cpp`

Evidence:

- `Scene` has `_sceneMutex` in `JammaLib/src/engine/Scene.h`.
- Some call paths already acknowledge cross-thread access by passing `_sceneMutex` into input/export flows.
- Many direct `_stations` reads and writes still happen with no lock or snapshot helper. Concrete examples in `JammaLib/src/engine/Scene.cpp` include:
  - `InitResources(...)` copying `_stations` directly
  - `_AddStation(...)` pushing into `_stations`
  - `_ApplyGlobalMidiQuantStateToAllLoopTakes()` iterating `_stations`
  - `_UpdateRemoteStationsFromSnapshot(...)` passing `_stations` onward
  - `_ResetIfEmpty()` iterating `_stations`
  - `_PublishAudioStations()` building a snapshot from `_stations`
  - `_ChildFromPath(...)` indexing `_stations`
- Audio already uses a safer published snapshot via `_PublishAudioStations()` and `AudioHost::SetStations(...)`, which is the right model for cross-thread collection sharing.

Why this is risky:

- The codebase already mixes render, input, networking/job, and export activity around station state.
- A collection that is sometimes lock-protected and sometimes read directly is effectively unprotected.
- Even a plain copy like `auto stations = _stations;` is unsafe if another thread can mutate the source vector at the same time.

Fix direction:

- Decide that `_stations` has one owner and every non-owner gets either:
  - a locked snapshot, or
  - an immutable published snapshot.
- Preferred plan:
  - Add a tiny helper on `Scene`, for example `SnapshotStations()`, that takes `_sceneMutex`, copies `_stations`, and returns the copy.
  - Use that snapshot for render/resource/export/read-only traversals.
  - Keep mutation paths like `_AddStation(...)` under `_sceneMutex`.
  - Avoid holding `_sceneMutex` while calling deep station methods when a snapshot is sufficient.
- Review any code that stores references into `_stations` and convert it to request snapshots instead.

Files to fix first:

- `JammaLib/src/engine/Scene.cpp`
- `JammaLib/src/engine/Scene.h`
- `JammaLib/src/io/IoInputSubsystem.cpp`
- `JammaLib/src/io/IoSessionExporter.cpp`
- `JammaLib/src/timing/TimingQuantiser.cpp`

### C. Medium Confidence: GUI child-tree mutation versus traversal is inconsistent

Relevant files:

- `JammaLib/src/base/GuiElement.cpp`
- `JammaLib/src/base/GuiElement.h`
- `JammaLib/src/gui/GuiRouter.cpp`
- `JammaLib/src/engine/LoopTake.cpp`
- `JammaLib/src/engine/Station.cpp`

Evidence:

- `GuiElement::InitResources(...)` already uses a snapshot of `_children` and even comments that child initialization may mutate the vector.
- `GuiElement::AddChild(...)` mutates `_children` directly.
- `GuiElement::TryGetChild(...)` iterates `_children` directly.
- `GuiElement::_ReleaseResources()` iterates `_children` directly.
- Several higher-level classes dynamically add/remove children by pushing and erasing on `_children`, especially router and loop/station flows.

Why this is only medium confidence:

- Many of these operations may be intentionally single-threaded on the UI/render side.
- The code clearly knows mutation-during-traversal is possible, but only one traversal path was hardened.
- Without a documented ownership rule, this remains a likely source of rare iterator invalidation bugs.

Fix direction:

- First decide whether the GUI tree is strictly single-thread confined.
- If yes:
  - document that rule clearly and keep fixes minimal
  - still consider snapshot iteration for mutation-prone traversals
- If no:
  - add a child-list mutex and centralize safe snapshot traversal helpers
  - convert direct `_children` iteration in base traversal methods to snapshot iteration

Files to inspect/fix after the two high-confidence families:

- `JammaLib/src/base/GuiElement.cpp`
- `JammaLib/src/gui/GuiRouter.cpp`
- `JammaLib/src/engine/LoopTake.cpp`
- `JammaLib/src/engine/Station.cpp`

### D. Medium Confidence: `ResourceUser` lifecycle flags are unsafely informal

Relevant files:

- `JammaLib/src/base/ResourceUser.h`
- `JammaLib/src/gui/GuiModel.cpp`
- `JammaLib/src/graphics/QuantisationModel.cpp`
- `JammaLib/src/graphics/QuantisationDivisionModel.cpp`
- `JammaLib/src/graphics/StationModel.cpp`
- `JammaLib/src/gui/GuiSlider.cpp`

Evidence:

- `ResourceUser` stores `_resourcesNeedInitialising` and `_resourcesInitialised` as plain `bool`s.
- The flags are read/written from generic init/release paths in `JammaLib/src/base/ResourceUser.h`.
- Other code toggles `_resourcesNeedInitialising` from render/model flows when resource-backed state changes.
- The class exposes no mutex or atomic contract for those flags.

Why this is only medium confidence:

- Many uses may currently remain on the render thread.
- The type and API do not enforce that ownership, so future fixes will keep reintroducing ambiguous access unless the contract is made explicit.

Fix direction:

- Pick one rule:
  - render-thread-only lifecycle flags, documented and asserted where practical, or
  - `std::atomic_bool` for the flags if they are intentionally set from other threads.
- If atomics are used, keep the design simple: atomic flags, but no deep shared mutable resource state behind them.

## Patterns That Look Good And Should Not Be Churned Blindly

These already use the right shape and should be preserved unless a specific bug is found:

- `AudioHost` published station snapshots
- `Loop`, `LoopTake`, and `Station` audio-state snapshot publication via `std::atomic<std::shared_ptr<...>>`
- `NinjamController` pending snapshot mutex handoff
- `NinjamConnection` explicit snapshot/connection mutexes
- `ResourceLib` map protection after the recent mutex hardening

## Proposed Fix Plan

### Phase 1: Normalize model resource access

Goal:

- eliminate all unsynchronized reads of `_modelTextures` and `_modelShaders`

Work:

1. Move `GuiModel` getters out of the header and make them lock `_modelStateMutex`.
2. Add protected snapshot helpers for indexed shader/texture access.
3. Convert every derived override to use those helpers.
4. Recheck all classes derived from `GuiModel` for direct `_modelShaders` / `_modelTextures` access.

Validation:

- build `JammaLib` Debug x64
- run the hot-path audit script to confirm no locks leaked into callback-owned code
- smoke test render startup under the debugger

### Phase 2: Put `Scene::_stations` behind one access pattern

Goal:

- stop direct concurrent reads/writes of the station vector

Work:

1. Add `SnapshotStations()` and, if needed, `WithStationsLocked(...)` helpers on `Scene`.
2. Convert render/resource/export/read-only traversals to snapshot-based iteration.
3. Guard mutation paths with `_sceneMutex`.
4. Audit helpers that keep a reference to `_stations` and convert them to snapshots or locked access.
5. Preserve the existing audio snapshot publication path.

Validation:

- build `JammaLib` and `Jamma`
- run native tests that cover scene/station flows if available
- manually exercise station add/remove while rendering and while MIDI/network updates are active

### Phase 3: Decide GUI tree ownership and enforce it

Goal:

- remove ambiguity around `_children`

Work:

1. Document whether GUI tree mutation/traversal is single-thread confined.
2. If single-threaded, make that explicit and convert only mutation-prone traversals to snapshots.
3. If cross-thread, add a child-list mutex plus snapshot traversal helpers in `GuiElement`.
4. Replace direct `_children` reads in base traversal methods first, then the dynamic router/station/looptake sites.

Validation:

- build `JammaLib`
- UI smoke test with router and loop/midi child creation/removal

### Phase 4: Make resource lifecycle flags explicit

Goal:

- stop relying on undocumented plain-bool cross-phase state

Work:

1. Audit all writes to `_resourcesNeedInitialising` and `_resourcesInitialised`.
2. If writes can happen off the render thread, convert the flags to atomics.
3. Otherwise document render-thread ownership in `ResourceUser` and derived classes.
4. Avoid adding more ad hoc flag writes until the contract is settled.

Validation:

- build `JammaLib`
- resource init/release smoke test during resize, scene changes, and dynamic model updates

## Recommended Order

1. Fix the `GuiModel` getter family first.
2. Fix `Scene::_stations` next.
3. Audit and settle GUI tree ownership.
4. Clean up `ResourceUser` lifecycle flags.

That order keeps the first two changes tightly aligned with the actual crash shape and reduces the chance of broad speculative churn.

## Suggested Deliverables

When implementing this plan, keep the work split into small reviewable commits or patches:

1. `GuiModel` snapshot getter normalization
2. `Scene::_stations` snapshot/lock normalization
3. GUI child-tree ownership cleanup
4. `ResourceUser` lifecycle contract cleanup

Each patch should end with a focused build and, where applicable, the repo hot-path audit.