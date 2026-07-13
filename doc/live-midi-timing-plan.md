# Live MIDI Timing Plan

## Goal

Deliver physical keyboard MIDI to station-hosted VST instruments without waiting for `Scene::_JobLoop()`, while keeping the audio callback as the sole VST-processing thread.

The current RtMidi callback timestamps each event, but `MidiRouter::PumpMidi()` does not forward it to a station until the job loop polls, currently every 20 ms. VST2 and VST3 then treat the event as realtime and clamp an event outside the current block to offset zero. The resulting extra input latency varies by roughly 0-20 ms before normal MIDI-device, audio-block, and output latency.

This change removes the job-poll delay from audible live playback only. Trigger actions, MIDI recording, MIDI learn, and automation remain on the existing job-thread path.

## Decisions

- The audio callback processes at most **64 live events per station per block**, shared across immediate and synthetic ingress.
- Events from different physical devices are preserved as raw MIDI. If two devices send the same channel/note, both NoteOns and both NoteOffs reach the plugin.
- Physical MIDI is rewritten to the forced channel once, at callback ingress, before the event is copied to the legacy and immediate paths.
- Live events are classified in the raw audio-sample domain, then rebased into the station's transport-shifted VST block domain.
- All 32-bit sample comparisons use signed modular deltas. Absolute relational comparisons are forbidden.
- A dropped NoteOff for a note that was delivered is retried from fixed dispatcher-owned state. Queue-drop counters alone are not sufficient stuck-note handling.
- `Scene::_JobLoop()` is the only runtime producer of synthetic station MIDI and the only runtime live-route publisher. UI mutations must post a job/control action before they enqueue synthetic events or publish routes.

## Non-Negotiable Real-Time Invariants

The following callback-owned code must remain lock-free, allocation-free, exception-free, free of logging and blocking waits, and bounded by explicit fixed capacities:

- `AudioHost::_OnAudio`
- `Station::WriteBlock`
- `Station::_RunVstBlock`
- `VstChain::ProcessBlockMulti`

Do not add a mutex, condition variable, OS wait, shared scene traversal, mutable trigger traversal, dynamic route lookup, `shared_ptr` publication, or unbounded drain to those functions.

The audio callback remains the only caller of `BeginMidiBlock`, `SendMidiEvent`, and plugin `process`. The RtMidi callback and live dispatcher never call a VST method.

The RtMidi callback may perform only bounded POD work, lock-free atomic loads/stores, two SPSC pushes, and one non-blocking work notification. Existing verbose iostream logging in `MidiDevice::_OnMidiData()` violates this target and must be disabled for this path or moved to a queue drained off the callback before claiming the invariant is met.

## Time Model

### Coherent audio-to-wall-clock anchor

The current callback reads `audioSampleCounter` and `midiAnchorMicros` as separate atomics. Those values can come from different audio-block updates and produce a false future or late timestamp. Replace them with one coherently published anchor:

```cpp
struct MidiClockAnchor
{
	std::atomic<std::uint32_t> Sequence{ 0u };
	std::atomic<std::uint64_t> Sample{ 0u };
	std::atomic<std::int64_t> SteadyMicros{ 0 };
};
```

The audio-side writer increments `Sequence` to odd, stores both fields, then increments it to even with release ordering. The MIDI callback makes at most two attempts to read the same even sequence before and after the fields. If both attempts race the writer, it uses the endpoint's last coherent anchor. Do not spin in the MIDI callback.

`MapMidiTimestampToAudioSample()` continues to calculate in 64 bits. Only the final `MidiEvent::sampleOffset` is truncated to the existing 32-bit modular sample domain.

### Wrap-safe classification

At 48 kHz, a 32-bit sample counter wraps in about 24.9 hours. Never implement block membership as `event >= start && event < start + count`.

For an event and raw audio block start, calculate:

```cpp
const auto delta = static_cast<std::int32_t>(eventSample - rawBlockStart);
```

- `delta < 0`: late; dispatch at block offset zero.
- `0 <= delta && delta < numSamples`: due in this block at `delta`.
- `delta >= numSamples`: future; leave it queued.

This is valid while no queued event is more than `INT32_MAX` samples from the block being compared. Fixed queues and prompt draining make that invariant comfortably true. Put the comparison in one small tested helper and use it in station scheduling and VST defensive checks.

### Raw and transport-shifted domains

Physical MIDI timestamps use the raw audio sample counter. `Station::_RunVstBlock()` currently calls `BeginMidiBlock()` with `shiftedBlockStartSample`, which includes `transportOffsetSamps`. Therefore:

1. Classify immediate and synthetic live events against the unshifted `blockStartSample` passed to `_RunVstBlock()`.
2. For a due event, copy it and set `sampleOffset = shiftedBlockStartSample + delta` before calling `SendMidiToVstChain()`.
3. For a late event, copy it and set `sampleOffset = shiftedBlockStartSample`.
4. Recorded loop events remain in their existing transport-shifted domain.

Without this rebase, a connected NINJAM transport offset can incorrectly defer or mark physical MIDI late.

## Architecture

### 1. Duplicate ingress at the MIDI device boundary

Add a second fixed-capacity SPSC queue to each `MidiRouter::MidiInputEndpoint`:

- `Ingress`: RtMidi callback producer, job-thread consumer. Retains trigger actions, MIDI-loop recording, CC automation, MIDI learn, held-note recording snapshots, and UI work.
- `LiveIngress`: RtMidi callback producer, live-dispatch-thread consumer. Carries audible physical MIDI only.

Use a separate POD wrapper for the immediate path:

```cpp
struct LiveMidiIngressEvent
{
	MidiEvent Event;
	std::uint32_t RoutingGeneration;
	std::uint32_t Sequence;
};
```

`Sequence` is endpoint-local and increments in the RtMidi callback. It gives deterministic ordering for equal timestamps and diagnostics for dropped events.

The callback performs this exact order:

1. Read one coherent clock anchor and map the timestamp.
2. Load one packed atomic live-input configuration containing the forced channel and routing generation.
3. Rewrite the event channel once using that forced-channel value.
4. Push the rewritten `MidiEvent` to `Ingress`.
5. Push `{ event, generation, sequence }` to `LiveIngress`.
6. Signal the dispatch work event even if `LiveIngress::Push()` failed, so pending NoteOff recovery is not starved.

Pack the generation and forced channel into a `std::uint64_t` and require `std::atomic<std::uint64_t>::is_always_lock_free` with a `static_assert`. Do not substitute an atomic `shared_ptr` load in the MIDI callback.

The legacy `PumpMidi()` path must stop rewriting the channel and must stop audibly enqueueing physical events. It consumes the already rewritten event for triggers, recording, automation, learn, and recording-held-note observation.

Both queues use drop-newest. `MidiQueue<1024>` has 1023 usable slots because one ring slot distinguishes full from empty; tests and diagnostics must use the usable capacity rather than assuming 1024 events.

### 2. Dedicated live-MIDI dispatcher

`MidiRouter` owns one live-dispatch thread for all endpoints. It waits outside the audio path on Windows events:

- an auto-reset work event signalled by MIDI callbacks and route/config publication;
- a manual-reset stop event signalled during teardown.

Use `WaitForMultipleObjects`. The dispatcher scans all inputs once before its first wait and after every wake. Event notifications may coalesce; correctness comes from scanning queues, not from assuming one wake per MIDI event.

Each dispatch pass is bounded:

1. Load one immutable `LiveMidiRoutingSnapshot`.
2. Apply a newer route generation and release held notes invalidated by the old-to-new route change.
3. Retry pending NoteOffs before forwarding new NoteOns.
4. Peek each endpoint's `LiveIngress` head and perform a wrap-aware k-way merge by timestamp. Break ties by `DeviceSlot`, then endpoint `Sequence`.
5. Discard immediate events whose `RoutingGeneration` is older than the active snapshot. Their legacy copies still reach trigger/recording processing.
6. Push each event to every recipient resolved by the snapshot.

Do not drain endpoint A completely before endpoint B. Doing so can put a future A event ahead of an already-due B event in a station FIFO, causing `Peek()` to block both.

The dispatcher performs no trigger actions, loop recording, model mutation, `_sceneMutex` acquisition, GUI calls, logging, or VST calls. It may allocate only while applying a newly published immutable snapshot, never while routing an event. Prefer fixed arrays/bitsets for per-event work.

### 3. Immutable routing snapshots and generations

`Scene::_JobLoop()` is the sole runtime publisher of live routing. Build the snapshot while the existing scene/config ownership lock makes station and trigger state stable, then publish it to `MidiRouter` off the audio thread. Initialization may publish before the job thread starts, and shutdown may publish after it joins; those lifecycle phases must not overlap a runtime publisher.

A snapshot contains:

```cpp
struct LiveMidiRecipient
{
	std::shared_ptr<engine::Station> Station;
	std::uint16_t AllowedChannelMask;
};

struct LiveMidiRoutingSnapshot
{
	std::uint32_t Generation;
	std::vector<std::vector<LiveMidiRecipient>> RecipientsByDeviceSlot;
};
```

The vectors are immutable after publication. Strong station references ensure a dispatcher-held snapshot cannot dangle. The dispatcher uses the snapshotted channel mask and never calls `Station::AcceptsLiveMidiFromDevice()` or traverses mutable `Station::_triggers`.

Preserve the current device-eligibility semantics exactly:

- a local station with no triggers accepts every active input device;
- if any station trigger has an empty `MidiInputDevices` list, the station accepts every active input device;
- otherwise the station accepts only devices named by at least one trigger;
- remote stations are never recipients;
- a zero allowed-channel mask rejects every channel.

Publish a new generation after:

- MIDI input initialization, reinitialization, or close;
- local station add/remove/reset;
- trigger add/remove or `MidiInputDevices` mutation;
- `Station::SetAllowedMidiChannels()`;
- forced-channel override changes.

Publication order is: store the immutable snapshot, then release-store the packed `{ generation, forcedChannel }` ingress configuration, then signal dispatcher work. A callback either tags an event with the old generation or the new one; the dispatcher never guesses which route configuration produced it.

When applying a new generation, the dispatcher compares held physical notes with the new recipient/channel eligibility. It queues NoteOffs to the old station for notes whose route disappeared before accepting new-generation NoteOns. If a release cannot be queued immediately, retain the old station reference in fixed pending-release state until the release is accepted.

### 4. Split station ingress without creating MPSC queues

Replace `Station::_liveMidiIngress` with:

- `_immediateLiveMidiIngress`: live dispatcher producer, audio callback consumer.
- `_syntheticLiveMidiIngress`: Scene job-thread producer, audio callback consumer.

Expose two narrowly named methods:

- `TryEnqueueImmediateLiveMidi(const MidiEvent&) noexcept`: dispatcher only; route and channel eligibility are already resolved.
- `TryEnqueueSyntheticLiveMidi(const MidiEvent&) noexcept`: Scene job thread only; used for punch transitions, ditch/flush releases, channel-removal releases, and other generated events.

Both return the queue push result. Add debug-only producer-thread assertions outside the audio callback. Do not retain a public generic `EnqueueLiveMidiEvent()` that obscures producer ownership.

Runtime callers of synthetic enqueue must be audited. Trigger/punch `Station::OnAction()` and `_DitchLoopTake()` already run from job dispatch, but channel override currently calls `FlushLiveHeldMidiNotes()` directly from `Scene::OnAction(KeyAction)` on the UI thread. Change channel override, runtime `SetAllowedMidiChannels()`, station/trigger route mutations, and any similar UI path to post one job/control action. That job performs held-note release, state mutation, route rebuild, and generation publication in order. Construction-time calls before audio starts are allowed but must not overlap runtime production.

### 5. Separate audible held notes from recording held notes

The existing physical `Station::EnqueueLiveMidiEvent(event, deviceName)` does two unrelated jobs: it updates `_liveHeldMidi` under a mutex for recording/punch seeding, and it pushes audible MIDI. Splitting the audible path without replacing both responsibilities breaks recording tests or duplicates playback.

Use two explicit states:

- **Recording-held state:** updated by delayed `PumpMidi()` through a non-audible `Station::ObservePhysicalMidiForRecording(event, deviceName)`. Preserve the current empty-device aggregate and per-device snapshots used when recording begins. This path never pushes to either station audio queue.
- **Audible-held state:** fixed dispatcher-owned bitsets keyed by station route and device slot. Update it only after an immediate NoteOn was successfully queued. A successful NoteOff clears it.

Because raw duplicate-device behavior is required, held state remains per device. Route invalidation may therefore emit duplicate NoteOffs for the same channel/note if two devices delivered duplicate NoteOns.

If an immediate queue is full:

- failed NoteOn: count the drop and do not mark it held;
- failed NoteOff for a held note: mark that device/station/channel/note in a fixed pending-release bitset and retry it before new NoteOns on later dispatch passes;
- failed NoteOff for a note not marked held: count the drop but do not create pending state.

While pending releases exist, use a bounded periodic wait timeout in addition to callback notifications so recovery does not depend on another physical event. No busy loop is allowed.

## Block-Accurate Station Scheduling

### `MidiQueue::Peek()`

Add `bool Peek(value_type& out) const noexcept` to `MidiQueue`. It is consumer-side only:

1. relaxed-load `_head`;
2. acquire-load `_tail`;
3. return false when equal;
4. copy `_buffer[head]` without modifying `_head`.

`Peek()` and `Pop()` must be called by the same sole consumer. Add queue tests for empty, non-destructive repeated peek, peek-then-pop identity, full/wrapped rings, and concurrent SPSC publication under Thread Sanitizer where available.

### Monotonic FIFO contract

`Peek()` can defer future events only if each station queue is monotonic in modular sample order.

- The dispatcher k-way merge guarantees immediate events are enqueued in timestamp order across devices.
- Track the last immediate timestamp per station. If a new timestamp modularly precedes it, clamp the queued copy to the last timestamp and increment `ReorderedLiveTimestampCount`.
- Track the last synthetic timestamp on the job-thread producer side and apply the same modular clamp before `TryEnqueueSyntheticLiveMidi()`.
- Timestamp generated releases with the current raw audio sample, not literal zero, where the current sample is available. A zero timestamp remains valid as an intentionally late emergency release and is then clamped if necessary to preserve FIFO order.

### Shared 64-event callback budget

`Station::_RunVstBlock()` merges the heads of the immediate and synthetic queues. It consumes at most 64 late-or-due live events total per station per block. A future head consumes no budget and remains queued.

For each iteration:

1. Peek both queue heads.
2. Classify each against raw `blockStartSample` with the signed-delta helper.
3. Ignore future heads when choosing an event.
4. Choose the earlier late/due event. For equal timestamps, choose NoteOff before NoteOn, then synthetic before immediate, then FIFO order.
5. Pop only the selected queue and verify the popped value matches the peeked value in debug builds.
6. Rebase its timestamp to `shiftedBlockStartSample` and send it as realtime MIDI if `vstActive`.
7. Increment late counters for negative deltas.

When `vstActive` is false, consume late/due events with the same 64-event budget but do not send them. Do not drain future events merely to avoid backlog. Queue overflow and budget exhaustion are separate diagnostics.

After live scheduling, dispatch recorded MIDI-loop events through the existing path, then call `VstChain::ProcessBlockMulti()` exactly once.

VST2/VST3 keep their defensive out-of-window clamp, rewritten with the same wrap-safe delta helper. Their fixed 256-event-per-plugin block limit remains authoritative. Add per-plugin atomic overflow counters so events rejected after that capacity are observable; do not report station enqueue as proof that every plugin accepted an event.

## Startup and Teardown

### Startup

1. `InitMidi()` first calls idempotent `CloseMidi()`.
2. Create work/stop events and an empty routing snapshot.
3. Create endpoints and open enabled inputs. Callback captures keep each endpoint and shared dispatch-notification state alive.
4. Atomically publish the endpoint vector.
5. Start the dispatcher. Its first action is a queue scan before waiting, so events received during setup are not stranded.
6. Publish the first Scene-built route generation and signal work.

Opening a device may fail without aborting other endpoints. Device slots remain stable for the lifetime of that endpoint snapshot.

### Teardown

`CloseMidi()` is idempotent and uses this order:

1. Publish an empty/disabled route generation so new immediate events are rejected.
2. Retain the current endpoint snapshot locally.
3. Call `MidiDevice::Close()` for every endpoint. `cancelCallback()` and `closePort()` must complete before dispatcher shutdown, so no producer can enqueue after the final drain.
4. Signal stop. On observing stop, the dispatcher performs one final drain, attempts bounded held-note releases, clears pending state, and exits.
5. Join the dispatcher.
6. Atomically publish an empty endpoint vector and release endpoint/snapshot ownership.
7. Close the Windows event handles.

`Scene::Shutdown()` must stop/close MIDI before unloading station VSTs or destroying stations. A dispatcher may retain station references, but it must not enqueue into stations after teardown has moved to plugin/station destruction.

Do not hold `_sceneMutex`, a station mutex, or any device mutex while joining the dispatcher.

## Diagnostics

Use relaxed atomic counters only on callback/dispatcher/audio writers. Read and report them from the job/UI side at a controlled cadence:

- legacy `Ingress` drops by device;
- `LiveIngress` drops by device;
- stale-generation immediate events discarded;
- station immediate queue drops;
- station synthetic queue drops;
- pending NoteOff retries and unrecovered releases at shutdown;
- timestamp reorder clamps;
- live events dispatched late;
- maximum live lateness in samples, updated with bounded atomic compare/exchange;
- per-station live budget exhaustion;
- per-plugin VST MIDI capacity drops.

Do not log from RtMidi, dispatcher, or audio callbacks. Diagnostics must distinguish queue overflow, callback-budget deferral, stale-route discard, and VST adapter overflow.

## Thread Ownership

| State | Sole writer/producer | Reader/consumer | Synchronization and teardown |
| --- | --- | --- | --- |
| Coherent MIDI clock anchor | audio callback | RtMidi callbacks | sequence-counted atomics; callback makes at most two attempts |
| Device `Ingress` | one device callback | Scene job thread | SPSC atomics; device callback closes before endpoint release |
| Device `LiveIngress` | one device callback | live dispatcher | SPSC atomics; final drain after callbacks close |
| Packed live-input config | Scene job thread at runtime | device callbacks | one lock-free release/acquire atomic word |
| Live routing snapshot | Scene job thread at runtime | live dispatcher | atomic immutable `shared_ptr`; never read by audio callback |
| Audible held-note state | live dispatcher | live dispatcher | fixed local bitsets; no synchronization |
| Recording held-note state | Scene job thread | Scene/control recording path | existing station mutex/state, never audio callback |
| Station immediate queue | live dispatcher | audio callback | SPSC atomics |
| Station synthetic queue | Scene job thread | audio callback | SPSC atomics |
| VST chain and processing | audio callback | audio callback | existing published chain snapshot |
| Diagnostics | owning callback/dispatcher/audio thread | job/UI thread | relaxed atomics |

## Implementation Sequence

Keep each step buildable and testable. Do not implement the dispatcher and station scheduler as one large change.

1. **Time helpers and queue primitive**
   - Add coherent `MidiClockAnchor` publication/read helpers.
   - Add wrap-safe sample classification/rebase helpers.
   - Add `MidiQueue::Peek()` and focused tests.
2. **Station queue split**
   - Add immediate/synthetic queues and explicit enqueue APIs.
   - Move current synthetic callers and recording-held observation to the correct APIs.
   - Add the 64-event merge scheduler, raw-domain classification, shifted-domain rebase, counters, and station tests.
3. **Deterministic dispatcher core**
   - Implement a non-threaded `DispatchAvailableLiveMidi()` core that accepts endpoint heads, one immutable snapshot, and fixed held/pending state.
   - Test k-way ordering, routing, generation rejection, overflow, pending releases, and route invalidation without sleeps or hardware.
4. **Thread and device integration**
   - Add `LiveIngress`, Windows events, thread lifecycle, callback notification, and final-drain teardown.
   - Move forced-channel rewrite to callback ingress and remove audible forwarding from `PumpMidi()`.
5. **Scene publication wiring**
   - Marshal UI-side channel, station, trigger, and device mutations to the Scene job thread when they can synthesize MIDI or change live routes.
   - Publish routes from every station/trigger/channel/device mutation point on that thread.
   - Change shutdown order so MIDI closes before VST/station teardown.
6. **VST fallback and diagnostics**
   - Make adapter window checks wrap-safe.
   - Add observable adapter-capacity drops.
   - Remove callback-side verbose logging.

## Required Tests

### Queue and time

- `Peek()` is non-destructive and matches the following `Pop()`.
- Wrapped ring indices preserve peek/pop FIFO behavior.
- Block classification handles late, exact start, last sample, exact end, and a block crossing `UINT32_MAX`.
- Coherent anchor reader never combines fields from different sequence generations and falls back after two failed attempts.

### Station scheduling

- Due live event reaches the fake plugin at its true in-block offset.
- Transport-shifted station rebases a raw live offset correctly.
- Future event remains queued until its block.
- Late event dispatches at offset zero and increments lateness diagnostics.
- Immediate and synthetic queues share one 64-event budget.
- A future head in one queue does not block a due head in the other.
- Equal-time NoteOff precedes NoteOn.
- `vstActive == false` consumes only late/due events and leaves future events queued.
- Existing punch, ditch, channel-removal, and recording-held-note tests still pass.

### Dispatcher core

- Two device queues are merged by timestamp rather than drained by device order.
- Equal timestamps use device slot and sequence deterministically.
- One physical event routes to every eligible local station and no remote station.
- Trigger device restrictions and allowed-channel masks match current semantics.
- Stale-generation events are discarded only from immediate playback; their legacy copies remain available.
- Failed NoteOn is not marked held.
- Failed held NoteOff becomes pending and is retried before a later NoteOn.
- Route removal, channel-mask removal, forced-channel change, and device close release affected held notes.
- Duplicate same-note events from two devices remain duplicated, including releases.
- Timestamp inversion is clamped and counted, preserving station FIFO monotonicity.

### Thread lifecycle

- Work signalled before the dispatcher begins waiting is still drained.
- Coalesced work signals do not lose queue contents.
- Callback activity racing `CloseMidi()` cannot enqueue after the final drain.
- `CloseMidi()` before initialization and repeated `CloseMidi()` calls are safe.
- Device-open failure does not leak handles or prevent other devices from dispatching.
- Tests use injected fake wait/device adapters or the deterministic core; do not depend on physical MIDI hardware or timing sleeps.

## Validation

1. Run the audio hot-path audit:

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File .github/skills/threading-review/audio-hotpath-audit.ps1
   ```

2. Manually inspect callback-owned functions for locks, waits, allocation, logging, unbounded loops, and `shared_ptr` operations.
3. Build `JammaLib` and `JammaLib_Tests` in Debug x64 using incremental Build.
4. Run focused `MidiQueue`, dispatcher, and `StationMidiInstrument` tests, then the full native test executable.
5. Run at least one debug session across the 32-bit sample wrap boundary using a seeded counter; do not wait 24.9 hours.
6. Exercise a local keyboard-to-VSTi session at 64 samples with NINJAM disconnected and connected. Verify the same physical strike lands at the expected block offset and connection status changes only the station transport rebase, not immediate-route latency.
7. Force each overflow independently and confirm its counter: device ingress, station immediate, 64-event callback budget, and VST adapter capacity.

## Explicit Non-Goals

- No mutex, condition variable, `shared_ptr` publication, or OS wait in the audio callback.
- No station/trigger traversal in the audio callback or MIDI device callback.
- No direct VST call from RtMidi or the live dispatcher.
- No general MPSC queue in the station/audio path.
- No change to MIDI recording quantisation or trigger semantics.
- No change to NINJAM synchronization, remote re-anchoring, or export-lane timing.
- No live VST latency compensation; plugin-internal latency remains separate.
- No sample-counter widening throughout the engine; this plan makes the existing 32-bit modular domain correct.