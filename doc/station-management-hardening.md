# Station management hardening

## Goal

Fix the reported delete -> add -> reconnect orphaned trigger exception, verify station changes against the existing live trigger editing protocol, and implement station motion only if it stays small and presentation-owned.

## Serial stages

1. **Research (read-only subagent).** Trace focus loss and routing callbacks, including synchronous Windows reentrancy. Compare station transactions with GuiHud rig edits: writer serialization, input closure, audio/input acknowledgements, frozen receiver identity, delayed actions, retirement and shutdown. Inspect existing animation utilities. Record concrete findings before editing.
2. **Implementation (subagent).** Fix proven defects using existing owner/locked-helper conventions. Add regression coverage for delete/add/reconnect and focus loss, plus any safety gaps found in research. Document owners and lock ordering. Avoid recursive mutexes, callback locks and broad refactors.
3. **Validation and independent review.** Read local build tasks before each incremental build/test. Build affected projects and run relevant native tests, then the full suite when appropriate. Run the threading audit and manually classify findings. A separate read-only subagent reviews ownership, publication ordering, delayed work and lifetime; fix actionable findings serially and repeat affected validation.
4. **Animation assessment and implementation.** After safety review, use existing UI interpolation if feasible: new stations enter from the appropriate side, survivors slide on prepend/removal, append keeps existing stations fixed. Keep authoritative topology/route identities final immediately, and animation data on the graphics owner. Hit testing and endpoint projection must match drawn positions. Deletion must not extend audio participation or release resources on the callback. If this needs a new framework or extensive coupling, document the reason and defer it.

## Completion evidence

- Research findings and exact fix recorded below.
- Delete/add/reconnect and focus-loss regressions pass.
- New shared state has explicit owner, synchronization and teardown.
- Review addresses input/publication acknowledgement ordering, held inputs, queued results and removal lifetime.
- Incremental builds, native tests and hot-path review results recorded.
- Animation implemented and verified, or a concrete feasibility decision recorded.

## Results

Research identified a deterministic recursive acquisition: Scene's locked HUD touch dispatch called GuiHud's candidate submission callback, which called the newly locked public `RequestRigEdit`. The same thread already owned `_sceneMutex`. The implementation now uses the existing explicit locked-helper convention: the public API locks once, and the HUD callback calls a private helper under its dispatch lock. Native editor teardown and synchronous settings finalization were also checked and corrected for focus-loss reentrancy.

### Safety implementation

- HUD routing submission uses `_RequestRigEditLocked` under its existing dispatch lock; the public API retains its single lock. This removes deterministic recursive acquisition without changing synchronization primitives.
- Focus-loss cleanup acquires Scene once and uses locked quantisation context/path helpers. It also cancels HUD cable capture and resets the Scene press target, preventing a later release completing a stale drag.
- Membership commit detaches topology under Scene's mutex. A strong local Station pin carries editor closure outside that lock. Retired Stations are transferred out of the locked retirement list only after all other pins have ended; graphics release and plugin unload run on the window owner outside Scene's mutex. Editor registry removal precedes native destruction, which runs outside its registry mutex.
- Receiver removal now strips latency-delayed mixer/punch borrowers by immutable history token before history/session retirement, including when the trigger has already rebound to a surviving station. Arrays remain callback-owned and bounded; no new callback lock/allocation.
- Added real Scene/HUD geometry-and-touch delete/add/reconnect regression and public routing submit/focus-loss regression. Tests align the initial accepted audio publication through the quiescent path and supply the callback liveness hint; transition staging, producer gating, quiescent application and input acknowledgement remain production paths. Native plugin focus reentrancy still requires a manual Windows editor walkthrough.

Ownership: the HUD and native focus handlers are window-thread entrypoints; locked helpers require Scene's mutex. `closeStationEditors` and `reclaimStations` are frame-local strong pins on the window owner. The editor window registry retains its existing mutex for membership, but native operations run after releasing it. Delayed action arrays remain audio-owner state; removal readiness is published only after their borrowers are discarded.

Independent review found a focus-finalization regression: `ClearFocus` synchronously commits real numeric settings and their Scene receivers acquire their own lock. `OnInputFocusLost` now performs this window-owned finalization before acquiring Scene's mutex, then performs the shared quantisation/HUD cleanup under one lock. Added real focused MIDI-channel and local-phase-offset regressions verifying committed owner values and cleared focus.

### Animation implementation and scope

Stations now keep a window-owned horizontal presentation offset alongside their immediately final `ModelPosition`. Existing UI frame time drives a 250ms cubic ease in/out, with each elapsed step bounded to 50ms. Prepend animates the new station from the left and slides survivors from their previous positions; append animates only the new station from the right; deletion slides surviving stations into the gap. Draw and picker use the same presentation transform. Top-cap HUD/bin anchors, endpoint buttons and rack visibility follow that transform, and Window repicks a stationary cursor during motion including its settling frame. Further membership controls stay disabled until motion settles; routing identities, audio membership and captured rig configuration are final throughout.

An outgoing deletion ghost is deliberately deferred: removal detaches the hierarchy and closes plugin editors before normal retirement, and rendering that detached hierarchy safely would add a second presentation lifetime and special draw path. Deletion still has a visible slide for survivors; audio and resource retirement remain the established acknowledgement path. Resource release snaps window-owned offsets, and no animation state is read by job/audio threads.

Added tests for prepend/append/delete presentation versus authoritative topology, retained route identities and save availability during motion; a focused interpolation test checks top-cap alignment, monotonic movement, bounded elapsed time and the settling-frame invalidation contract. Existing topology integration tests finish prior presentation motion because their fixture does not run a graphics frame pump.

Final frame-order review found that advancing motion after control resource initialization could create uninitialized controls, and control placement used the previous camera projection. Window now updates camera, advances motion, then initializes scene resources before any picking/drawing. Animation advancement refreshes the current view projection before HUD/rack placement without creating controls; `InitResources` computes the same current projection and runs the sole control Update + Init pass. Added a graphics-independent stale-projection regression for the UI frame preparation path.

### Validation and review receipts (2026-10-12)

| Stage | Evidence |
| --- | --- |
| Research | Read-only `station_research` subagent traced the recursive HUD submission lock, native editor teardown, cancellation ordering and animation feasibility. |
| Implementation | `station_implementation` subagent implemented each stage serially, including fixes requested by review. |
| Independent review | `station_review` subagent found focused-settings finalization and animation frame-order defects. All findings were corrected; the final read-only review confirmed both frame-order findings resolved. |
| Build | Incremental Debug x64 JammaLib, native test project and Jamma app builds succeeded using the worktree's local task command. |
| Targeted checks | 79 focused routing, topology, audio-flow, callback allocation and cancellation checks passed before the final projection fix. Focused settings/focus-loss and orphan reconnect checks also passed independently. |
| Final suite | **1,424 / 1,424 native tests passed** after all code changes, including the final projection regression. |
| Diff hygiene | `git diff --check` passed. |
| Threading audit | The script returned 1 because it flags lock additions throughout Scene.cpp, including UI/job methods. Every added lock was manually classified as off callback; `Scene::OnTick`, AudioHost callback/boundary work and Trigger removal work were inspected. No new callback locks, waits, allocation, formatted logging or I/O were found; callback allocation probes passed. This is a manually reviewed result, not a clean script result. |

Animation offsets are plain fields owned and accessed by the window/graphics thread; Scene layout/drawing uses its existing mutex. They are never part of published topology or job/audio commands. Resource teardown resets them on that owner. The controller, input gates, cancellation atomics, retirement receipts and snapshots retain their previously documented synchronization and acknowledgement ordering.

Automated implementation and review stages are complete. A manual Windows walkthrough remains for actual plugin-editor focus transitions and the visual feel across camera modes/resizing; no native GUI walkthrough is claimed here. The optional outgoing deletion ghost remains assessed and deferred as described above.
