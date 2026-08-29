# Stage 02 — Logical layout

## Assignment

- Primary ownership: architectural placement, dependency direction, layer leakage, and duplicated subsystem ownership across `audio`, `engine`, `ninjam`, `midi`, and `utils` for `master...HEAD`; relevant history is `master..HEAD`.
- Explicit exclusions: naming/style/conventions; proof of runtime or timing correctness; test quality; stale/dead-code reachability; broad simplification of live abstractions.
- Required output: boundary concerns must identify the current owner, proposed owner, and dependency-direction evidence. This report uses only local IDs `S02-##`.
- Write discipline: investigation was read-only except for this report. No production, test, build/project, canonical-artifact, inventory, or other stage-report file was edited.

## Coverage

- Read in full before investigation: `AGENTS.md`; `doc/merge-readiness-review/merge-readiness-plan.md`; `doc/merge-readiness-review/phase-1-baseline-and-structure.md`; `doc/merge-readiness-review/00-scope-and-inventory.md`; `doc/loop-alignment-and-ninjam-sync.md`; `doc/realtime-audio.md`; and `doc/build.md`.
- Reviewed the complete `master...HEAD` name/status and statistics for all 55 changed files under `JammaLib/src/{audio,engine,ninjam,midi,utils}` (7,821 insertions and 542 deletions in that partition), then inspected the relevant current headers/implementations and branch history around the timing path.
- Current code covered in depth: `audio/AudioHost.{h,cpp}`, `engine/{Scene,Station,LoopTake,Loop,Quantiser}.{h,cpp}`, `ninjam/{NinjamAudioTimingCommand,NinjamLoopAlignment,NinjamTimingCoordinator,NinjamNetworkService,NinjamTiming}.{h,cpp}`, `midi/{MidiBlockTiming,MidiClockAnchor,MidiRouter,MidiLoop}.{h,cpp}`, and `utils/{Timer,MusicalTransport,MathUtils}.{h,cpp}`. Include direction across all source/header files in the five partitions was queried, not merely spot-checked.
- History inspected for the structural changes included `f36bfe7`, `d6fdb88`, `4c1c0e1`, `6abf7c7`, `a3f72b4`, `e0f669c`, `1d665d2`, `e2dc3f8`, `0d90bac`, `bef7943`, and `e72f3b0`, plus line-level blame for the cited current code.
- Commands/queries used: `git diff --name-status master...HEAD -- <partitions>`; `git diff --stat master...HEAD -- <partitions>`; `git diff --find-renames --unified=30 master...HEAD -- <files>`; `git log --oneline --no-merges master..HEAD -- <partitions>`; `git show --stat --oneline <commit>`; `git blame -L <range> -- <file>`; `rg -n '^#include' <partitions> -g '*.h' -g '*.cpp'`; focused `rg -n -C` symbol/call-site queries; and numbered `Get-Content` reads of surrounding implementations.
- Deliberate exclusions: GUI/graphics/resource, VST, persistence, and app/tooling layout was not exhaustively reviewed except where a focal header pulled those layers into a timing boundary. Runtime safety, synchronization, allocation/locking cost, numerical correctness, test assertions, and reachability were not judged. No build or tests were run because this stage is read-only structural review.

## System understanding

- The intended direction is remote observation/session state in `ninjam` -> accepted immutable timing command -> `audio::AudioHost` at one block boundary -> `utils::Timer` plus local `Station -> LoopTake -> Loop` fan-out. `Scene` is the composition/UI glue, while per-loop audio and MIDI phases remain owned by the engine entities.
- `AudioHost` is correctly positioned as the one audio-boundary integrator: it owns the timing-command mailbox, clock reference, station snapshot, and the shared remote-to-local source ruler. It is the only layer with all inputs needed to translate an accepted remote command into coherent Timer and local-entity changes.
- `utils::Timer` owns local master transport and the monotonic scene coordinate; `utils::MusicalTransport` derives musical position. NINJAM owns observation validity, request/acknowledgement state, and follow policy. Engine entities own local audio/MIDI cursors and their per-entity source anchors.
- The protected concepts remain distinct in all recommendations below. In particular, centralizing ownership of the common sync map does not turn mapped elapsed time into a shared loop cursor: each `LoopTake`/`Loop` would still wrap one supplied source coordinate by its own length and retain its own anchor.

## Candidate findings

### S02-01 — Keep NINJAM follow policy out of the core loop hierarchy

- Stage / reviewer: Stage 02 — Logical layout.
- Scope reviewed / exclusions: `AudioHost` timing-command fan-out and `Station -> LoopTake -> Loop` timing APIs; no claim about whether the current policy checks are behaviorally correct.
- Severity: follow-up.
- Evidence: `JammaLib/src/engine/LoopTake.h:21` imports the NINJAM command header into the core loop type; its public correction API takes `ninjam::NinjamLocalFollowPolicy` at `JammaLib/src/engine/LoopTake.h:235`-`239`, and `Station` repeats that NINJAM-specific parameter at `JammaLib/src/engine/Station.h:113`-`128`. Inside the engine, policy is used only to reject `NoSync` after generation handling at `JammaLib/src/engine/LoopTake.cpp:518`-`535`. The upstream boundary already computes `disablesSync`, clears anchors/maps, and suppresses the engine call at `JammaLib/src/audio/AudioHost.cpp:174`-`186` and `JammaLib/src/audio/AudioHost.cpp:298`-`305`. The unified engine fan-out arrived in `d6fdb88`; NINJAM policy entered the engine API through `6abf7c7`/`a3f72b4`.
- Current owner: follow policy is correctly declared in `ninjam`, but the decision is redundantly represented in `audio::AudioHost`, `engine::Station`, and `engine::LoopTake`.
- Proposed owner: keep `ContinuousSync`/`BlockSync`/`NoSync` interpretation in `ninjam` and the `AudioHost` integration boundary; expose only a neutral accepted correction/invalidation operation to the engine hierarchy.
- Dependency-direction evidence: the current include makes low-level local loop state depend upward on a session protocol header, while the call already flows downward from the NINJAM-aware `AudioHost`. Removing the policy parameter would preserve `audio -> engine` and eliminate `engine -> ninjam` for this decision.
- Why it matters: the core hierarchy cannot be reused or reasoned about as local loop state without importing remote-session policy, and there are two places that must agree that `NoSync` moves nothing.
- Recommended disposition: move/simplify. Retain the distinct follow policies at the boundary; translate them once into neutral engine operations.
- Protected timing concepts affected: follow policy, `NoSync` invalidation, per-loop phase. The recommendation explicitly preserves all three and does not merge policy with a clock or coordinate.
- Verification: compile dependency check showing `LoopTake.h`/`Station.h` no longer include a NINJAM command header; existing continuous/block/no-sync timing tests; focused reconnect and `NoSync` invalidation scenarios.
- Human decision: pending.

### S02-02 — Give the common sync-phase map one runtime owner

- Stage / reviewer: Stage 02 — Logical layout.
- Scope reviewed / exclusions: ownership and fan-out shape of the protected sync map; no claim that current copies are observably divergent at HEAD.
- Severity: follow-up.
- Evidence: `AudioHost` declares the audio-thread-owned common map at `JammaLib/src/audio/AudioHost.h:141`-`144`, uses it to restore/rebase source coordinates at `JammaLib/src/audio/AudioHost.cpp:167`-`214` and `JammaLib/src/audio/AudioHost.cpp:270`-`318`, then copies its origin, both lengths, and source coordinate through `Station` into every take at `JammaLib/src/audio/AudioHost.cpp:352`-`364`. Every `LoopTake` stores a parallel map (`JammaLib/src/engine/LoopTake.h:406`-`410`) and independently recomputes mapped elapsed/source coordinates (`JammaLib/src/engine/LoopTake.cpp:602`-`679`). `a3f72b4` introduced per-take map state, `0d90bac` added rebasing, and `bef7943` converted both owners to monotonic source coordinates.
- Current owner: split between one authoritative-looking `audio::AudioHost::_syncPhaseMap` and a copied map in every live `engine::LoopTake`.
- Proposed owner: `AudioHost` should own the one common remote-to-local ruler and compute the current mapped source coordinate once per boundary/block. `LoopTake` should own only its audio/MIDI entity anchors and restore each cursor from the supplied source coordinate.
- Dependency-direction evidence: `AudioHost` already has the Timer scene coordinate, accepted policy, remote/local lengths, correction delta, and station snapshot; takes have only entity phases/lengths. Therefore map computation naturally flows `AudioHost common map -> Station fan-out -> LoopTake per-entity modulo`, not `AudioHost map -> N copies -> N repeated map computations`.
- Why it matters: the protected common ruler currently has N+1 mutable representations and three propagation APIs (`Begin`, `Rebase`, `Restore`). This expands the consistency surface precisely where recent commits repeatedly changed the map origin/coordinate model.
- Recommended disposition: simplify. Replace map-geometry propagation with explicit capture/invalidate operations plus a source-coordinate restore fan-out. Do not share a raw loop cursor.
- Protected timing concepts affected: sync phase map, mapped elapsed time, source/scene anchor, per-loop phase, monotonic scene coordinate. The proposed owner change preserves their distinctions.
- Verification: unit tests with unequal loop lengths and intentional offsets; repeated rebase/remote-wrap tests; reconnect and `NoSync` invalidation; instrumentation/assertion in tests that all takes receive the same source coordinate while producing entity-specific wrapped indices.
- Human decision: pending.

### S02-03 — Split value-only local timing contracts from the Quantiser/UI aggregate

- Stage / reviewer: Stage 02 — Logical layout.
- Scope reviewed / exclusions: header/module placement and dependency direction for local timing snapshots; no naming or numerical-validity judgment.
- Severity: follow-up.
- Evidence: value types such as `LocalAudioGeometry`, `RemoteTransportGeometry`, `QuantisationPolicy`, and `QuantisationTiming` live at `JammaLib/src/engine/Quantiser.h:43`-`137`, but that same header imports actions, graphics, GUI/base, Timer, constants, and common UI types at `JammaLib/src/engine/Quantiser.h:3`-`18` and also declares the stateful `Quantiser` and `QuantiserController` at `JammaLib/src/engine/Quantiser.h:170` and `JammaLib/src/engine/Quantiser.h:353`. The NINJAM coordinator includes this aggregate header at `JammaLib/src/ninjam/NinjamTimingCoordinator.h:6`-`9` only to exchange `engine::QuantisationTiming` in its APIs and state (`JammaLib/src/ninjam/NinjamTimingCoordinator.h:158`-`180`); `NinjamNetworkService.h:31`-`44` exposes the same dependency. Commit `e2dc3f8` deliberately moved the former timing aggregate into `engine/Quantiser`, after `f36bfe7` separated NINJAM coordination from the old external-transport/quantiser implementation.
- Current owner: the engine timing values and the stateful engine/UI quantiser/controller are co-owned by one large `engine/Quantiser.h` module; NINJAM therefore depends on the whole aggregate.
- Proposed owner: keep local geometry/timing values in a small value-only engine timing contract header (for example an engine timing/geometry types module); keep `Quantiser` and its interaction controller in the engine implementation layer. NINJAM should consume an immutable local-timing snapshot, not the concrete quantiser module.
- Dependency-direction evidence: the semantic data flow is `engine local timing snapshot -> ninjam policy comparison`; no NINJAM coordinator operation needs graphics, actions, loop objects, or the quantiser controller. A value-only header preserves that direction without pulling presentation/control dependencies into the session layer.
- Why it matters: changes to engine interaction/rendering declarations propagate into the NINJAM timing coordinator and its consumers, obscuring the otherwise clean coordinator boundary and increasing compile/interface coupling.
- Recommended disposition: move. Extract the value contracts without collapsing local grain, active grid, remote geometry, or follow policy.
- Protected timing concepts affected: local timing, local grain, active quantisation grid, remote timing. The extraction must keep these as distinct types/fields.
- Verification: include-graph check that `NinjamTimingCoordinator.h` no longer includes `Quantiser.h`; compile coordinator tests against the value-only contract; existing local-timing/follow-policy tests.
- Human decision: pending.

### S02-04 — Move the explicitly local-only offset mailbox out of the NINJAM command module

- Stage / reviewer: Stage 02 — Logical layout.
- Scope reviewed / exclusions: class placement only; mailbox synchronization and real-time behavior are Phase 2 concerns.
- Severity: follow-up.
- Evidence: `LocalTransportOffsetLoopFracMailbox` is declared inside namespace `ninjam` and `NinjamAudioTimingCommand.h` at `JammaLib/src/ninjam/NinjamAudioTimingCommand.h:147`-`193`, although its own comment calls it “local-only.” Its only production owner is `AudioHost` at `JammaLib/src/audio/AudioHost.h:126`, and its value is consumed to update local Station/LoopTake offsets at `JammaLib/src/audio/AudioHost.cpp:332`-`350`; no remote observation, policy, interval, or session state participates. Commit `4c1c0e1` added the local offset path to the NINJAM command header.
- Current owner: `ninjam::NinjamAudioTimingCommand` module owns an unrelated local UI-to-audio mailbox.
- Proposed owner: `audio::AudioHost` as a private local control mailbox, or a narrow generic latest-value mailbox in `utils` if another proven consumer needs the mechanism.
- Dependency-direction evidence: the data flow is UI/Scene -> AudioHost -> local Station/LoopTake, entirely parallel to the NINJAM command flow. Putting the mailbox under `ninjam` reverses semantic ownership and causes local transport code/tests to depend on a remote-session module.
- Why it matters: the file and namespace imply remote authority for a control that must remain valid when disconnected and under `NoSync`, making future lifecycle or cleanup work likely to couple unrelated concepts.
- Recommended disposition: move. Preserve the absolute/latest-wins local offset semantics unchanged.
- Protected timing concepts affected: local timing and follow policy. The move reinforces that local offset is independent of remote authority and does not change its coordinate semantics.
- Verification: existing mailbox latest-publication/explicit-zero tests moved to the new owner; local offset tests with disconnected and `NoSync` states; include-graph check.
- Human decision: pending.

### S02-05 — Complete NINJAM update-to-command translation inside the NINJAM integration layer

- Stage / reviewer: Stage 02 — Logical layout.
- Scope reviewed / exclusions: orchestration placement across coordinator, Scene, and AudioHost; UI presentation itself is not criticized and command behavior is not re-proved.
- Severity: follow-up.
- Evidence: `NinjamTimingCoordinator`/`NinjamNetworkService` return a domain update, but `engine::Scene` owns the full update-to-command switch and field-by-field materialization at `JammaLib/src/engine/Scene.cpp:403`-`467`, publishes it at `JammaLib/src/engine/Scene.cpp:469`-`482`, and independently creates invalidation commands for connect/disconnect at `JammaLib/src/engine/Scene.cpp:254`-`305`. The coordinator already owns the command type/follow-policy contract through `JammaLib/src/ninjam/NinjamTimingCoordinator.h:6`-`9` and decides generations, replacement/correction/invalidation, and follow policy. `f36bfe7` moved timing state-machine responsibility into `NinjamTimingCoordinator`; `d6fdb88` introduced the unified audio command but left its materialization in `Scene`.
- Current owner: command meaning is decided in `ninjam::NinjamTimingCoordinator`, command construction is in `engine::Scene`, and command application/translation is in `audio::AudioHost`.
- Proposed owner: have the NINJAM coordinator/integration service emit the complete immutable `NinjamAudioTimingCommand` (or include it directly in its update). Keep `Scene` responsible for UI prompts, forwarding the command, and explicitly applying the separate engine MIDI-grid output; keep `AudioHost` responsible for audio-boundary application.
- Dependency-direction evidence: `Scene` currently has to understand every coordinator result variant and every audio-command field even though it contributes no transport decision. Moving the mechanical mapping back to the producer leaves the intended direction `ninjam decision -> Scene forwarding/UI -> AudioHost application` and makes Scene thinner.
- Why it matters: adding or changing a command field requires coordinated edits across coordinator outputs, Scene translation, and AudioHost consumption; reconnect/disconnect invalidation is also constructed outside the policy owner.
- Recommended disposition: move/simplify. Do not move UI prompts into NINJAM and do not move audio-thread application out of `AudioHost`.
- Protected timing concepts affected: remote join, follow policy, sync phase map command lifecycle, `NoSync` invalidation. The boundary split must retain each concept and the single audio-boundary command.
- Verification: coordinator tests assert emitted commands for replace/join/discipline/invalidate; integration tests assert Scene forwards without reinterpretation; existing audio-boundary generation/invalidation tests.
- Human decision: pending.

## Handoffs

- Stage 05/06: `engine::Quantiser::LogNinjamTempoEvent` and `LogNinjamManualTempoCommands` are declared/defined only at `JammaLib/src/engine/Quantiser.h:228`-`234` and `JammaLib/src/engine/Quantiser.cpp:651`-`673`; `rg` found no call sites. The unused `#include "../ninjam/NinjamTiming.h"` at `JammaLib/src/engine/Quantiser.cpp:2` also has no symbol use. Stage 06 should determine dead versus dormant status using Stage 05 intent rather than treating this Stage 02 note as a finding.
- Stage 01/21: `JammaLib/src/ninjam/NinjamLoopAlignment.h` is a new tracked public-style header used by production code but is not listed by focused `rg` in `JammaLib/JammaLib.vcxproj`/filters. Project-membership and merge/build hygiene are outside this stage.
- Stage 04: the names around `QuantisationTiming`, `SeedSamps`, `MasterLoopSamps`, and `NinjamLoopAlignment` may obscure authority/coordinate meaning, but no rename proposal is developed here.
- Stages 07-09/11: mailbox atomics, repeated map computation, map underflow/overflow/rounding, ordering, generation checks, and actual join/discipline behavior were intentionally not assessed; those stages should cite the structural ownership map above rather than infer correctness from it.

## Uncertainties

- `Scene` is historically both an engine object and the JammaLib composition/UI controller. S02-05 does not propose relocating Scene wholesale; it is limited to mechanical NINJAM command materialization. A broader shell split would exceed Phase 1 Stage 02.
- A single common map owner still requires a choice between computing one source coordinate per block in `AudioHost` or passing an immutable map value/reference. The first is the clearer ownership direction, but Phase 2 must validate hot-path cost and lifetime implications before any implementation decision.
- The exact name/location of a value-only engine timing header is intentionally left open to Stage 04 and integration. Its semantic owner should remain the local timing/engine domain rather than `ninjam`.
- Some include coupling predates the final refactor, but the cited branch commits moved or extended the relevant ownership and are in `master..HEAD`; the report does not attribute unrelated pre-branch architecture to this branch.

## Conclusion

Five structural candidates are reported. The branch’s high-level audio-boundary direction is sound: NINJAM decides, `AudioHost` applies coherently, and entities retain individual phase anchors. The remaining merge-readiness concern is incomplete boundary consolidation: remote policy leaks into core engine APIs, the common sync ruler has N+1 mutable copies, value-only local timing contracts are bundled with the interactive Quantiser, a local-only mailbox is owned by the NINJAM module, and command materialization remains split between coordinator and Scene. All proposed moves preserve the protected distinctions and should be treated as structural follow-ups unless Phase 2 shows that one creates a correctness or real-time blocker.
