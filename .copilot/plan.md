# Feature: midi-loops

## Summary
Record and playback MIDI loops alongside existing audio loops, with audio and MIDI takes coexisting in the same station from the first integration slice.

## Goals & Acceptance Criteria
- Can record a MIDI loop from hardware input and play it back in sync with audio loops
- Audio and MIDI takes can coexist in the same station
- Audio pipeline is completely unchanged

## Scope

### In scope
- MIDI loop engine (in-memory): event storage, sample-relative scheduling, state machine
- MIDI ingress queue: callback-safe preallocated ring, timestamped events
- Station media seam: Station can own audio and MIDI takes at once
- MIDI device setup in Scene (parallel to audio, isolated from audio callback)
- MIDI overdub + punch semantics (post-basic record/playback)
- RigFile store/recall of active MIDI device(s)

### Out of scope (deferred)
- Jam-file persistence of audio loop data (deferred until in-memory behavior is stable)
- VST-hosted MIDI / plugin routing
- Trigger MIDI (MIDI only used as the data recorded to the new loop type, not affect Trigger state machine)
- NINJAM MIDI

### Non-goals
- Audio signal pipeline changes
- Wall-clock MIDI timing (all timestamps are sample-relative)

## Architecture Decisions
1. **Sibling MIDI implementation** — `MidiLoop` is a peer of `Loop`. Audio loop storage (BufferBank<float>) is not used for MIDI events.  `LoopTake` can contain a mix of audio loops and MIDI loops.
2. **MIDI for trigger and MidiLoop** can be used for both triggering loops, and also as the actual data kept inside a loop
2. **LoopTake orchestration seam** — LoopTake owns both `Loop` and `MidiLoop` through a minimal shared interface; callers never branch heavily on media type.
3. **Callback-safe ingress** — MIDI device callback only enqueues to a preallocated fixed-size ring. No heap alloc, no locks, no blocking in callback.
4. **Sample-relative timestamps** — MIDI events stored as offset-within-loop in samples. Musical presentation derives from Timer, not vice versa.
5. **Explicit wrap/punch semantics** — Note-off at loop wrap and punch-out boundaries are defined in tests before implementation.

## Implementation Invariants (Real-Time Safety)
- No heap allocation in audio or MIDI callbacks
- No blocking locks in audio or MIDI callbacks
- MIDI timestamps: extremely accurate sample-relative inside loops, never wall-clock (do not quantise to block sizes)
- Station callers must not branch on media type in hot path
- Ingress queue overflow policy: drop oldest (preallocated fixed capacity, never grows)

## Vertical Slices (TDD order)

### Slice 3 — MIDI event + lock-free ingress queue
**Goal:** A preallocated, callback-safe event queue that stores timestamped MIDI notes and can be drained deterministically.  
**New files:**
- `JammaLib/src/engine/MidiEvent.h` — POD: `{uint32_t sampleOffset; uint8_t status; uint8_t data1; uint8_t data2;}`
- `JammaLib/src/engine/MidiQueue.h` — fixed-capacity (e.g. 1024) single-producer/single-consumer lock-free ring using `std::atomic<uint32_t>` head/tail.  
**New test file:** `test/JammaLib.Tests/src/engine/MidiQueue_Tests.cpp`  
**Tests cover:** push/pop round-trip, wrap-around, overflow drop-oldest policy, empty/full sentinel, timestamp ordering.  
**Exit gate:** All MidiQueue_Tests pass; no heap alloc in push/pop paths (static-assert or code review).

### Slice 4 — In-memory MIDI loop engine
**Goal:** Record synthetic MIDI events into a loop, play them back through a fake MIDI sink with sample-relative scheduling, verify timing across block boundaries and loop wrap.  
**New files:**
- `JammaLib/src/engine/MidiLoop.h/.cpp` — stores `std::array<MidiEvent, N>` (fixed capacity), tracks record count and loop length; `RecordEvent(event)`, `ReadBlock(sampleStart, sampleEnd, sink)`. 
**New test file:** `test/JammaLib.Tests/src/engine/MidiLoop_Tests.cpp`  
**Tests cover:** inject events → record → play back; note-on/note-off across block boundary; loop wrap issues forced note-off for held notes; empty loop plays silence.  
**Exit gate:** Record + loop-playback round-trip confirmed in tests with a fake sink.

### Slice 5 — Quantised record-end for MIDI loops
**Goal:** MIDI loop finalisation snaps to the quantisation grid using the existing `Timer::QuantiseLength`.  
**Extend:** `MidiLoop_Tests.cpp` / `MidiLoop` — red tests for snapped loop length, playback alignment after snap.  
**Exit gate:** Quantised end mirrors audio loop behavior verified in Timer_Tests.

### Slice 6 — Mixed audio + MIDI station
**Goal:** One station owns one loop take with `Loop` (audio, playing) and one with `MidiLoop` (records then plays). Audio path is unchanged.  
**Changes:** Station gets a `std::vector<std::shared_ptr<IMidiTake>> _midiTakes` alongside `_loopTakes`. Routing, trigger targeting, and commit flows extended for MIDI takes.  
**New test file:** `test/JammaLib.Tests/src/engine/MidiStation_Tests.cpp`
**Tests cover:** mixed-station add/select/route; audio playback not degraded; MIDI take records and replays.  
**Exit gate:** All audio regression suites green. Mixed-station tests green.

### Slice 7 — MIDI overdub + punch
**Goal:** MIDI overdub merges new events on top of existing loop. Punch-in gates recording within a region. Forced note-off at punch-out and loop-wrap boundaries.  
**Extend:** `MidiLoop_Tests.cpp`, `MidiLoopTake.cpp`.  
**Exit gate:** Block-driven tests analogous to `Overdub_Tests` cover merge, held-note carryover, and punch boundary note-offs.

### Slice 8 — MIDI device in Scene
**Goal:** Global MIDI device lifecycle parallel to audio. Device callback enqueues to `MidiQueue`. Scene job-thread drains and routes to station/trigger.  
**New files:**
- `JammaLib/src/audio/MidiDevice.h/.cpp` — parallel to `AudioDevice`; open/close/enumerate; callback only writes to `MidiQueue`.  
**Extend:** `Scene.h/.cpp` — `InitMidi()`, `CloseMidi()`, job-thread drain + trigger dispatch.  
**New test file:** `test/JammaLib.Tests/src/audio/MidiDevice_Tests.cpp` — fake-device tests around open/close/reconnect/drain.  
**Exit gate:** Fake-device tests pass. Manual hardware smoke: record MIDI loop with audio running, no glitches, no underruns.

### Slice 9 — Jam-file persistence (deferred)
**Goal:** Extend `JamFile` schema for MIDI loop content; save/load round-trip; backward compat for pre-MIDI files.  
**Depends on:** Slices 4–6 complete.  
**Exit gate:** Round-trip and backward-compat tests in `JamFile_Tests.cpp` pass.

## Review Gates (subagent / swarm checkpoints)
| After slice | Focus |
|---|---|
| Slice 2 | Trigger refactor: audio regressions, MIDI source isolation |
| Slice 6 | Mixed-station: routing, ownership, audio regression |
| Slice 8 | Real-time safety: callback allocs, locking, timestamp drift, queue policy |
| Slice 9 | Schema: save/load compat, missing persistence tests |

## Swarm Lanes (when kicking off swarm)
1. **Core arch lane** — LoopTake media seam + take ownership
2. **Test lane** — characterization, synthetic MIDI, loop scheduler, mixed-station coverage  
3. **MIDI engine lane** — event queue, in-memory loop model, overdub semantics
4. **Integration lane** — Scene/device lifecycle and manual smoke support
5. **Review lane** — subagent review after each gate (audio regression, real-time safety, test gaps)

## Risks
- Audio callback timing: if MIDI device thread contends on `_audioMutex`, buffer underruns can occur → mitigate with lock-free queue, short lock hold in job thread
- Timing jitter: MIDI note timestamps must be sample-relative, not derived from wall clock
- Loop wrap note-off: held notes at loop boundary must get explicit note-off; define this in tests before implementing
- Station coupling: Station currently hardcodes `_loopTakes` as `vector<shared_ptr<LoopTake>>`; the seam must not change audio routing logic
