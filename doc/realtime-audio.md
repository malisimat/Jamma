# Real-Time Audio Guidance

## Callback and hot-path rules

- No heap allocation.
- No exceptions.
- No blocking I/O or unpredictable locks.
- No heavy logging or system calls.
- Prefer the fastest safe read/write path.
- Pre-allocate and reuse buffers and resources.

## Hot-path review checklist

After edits in audio-thread code, manually inspect these callback-owned functions before finishing a change:

- `Scene::OnTick`
- `AudioHost::AudioCallback`
- `AudioHost::_OnAudio`
- `AudioHost::AdvanceBlock`
- `Loop::WriteBlock`
- `LoopTake::Zero`
- `LoopTake::WriteBlock`
- `LoopTake::EndMultiPlay`
- `LoopTake::EndMultiWrite`
- `LoopTake::_InputChannel`
- `Station::Zero`
- `Station::WriteBlock`
- `Station::EndMultiPlay`
- `Station::OnBlockWriteChannel`
- `Station::EndMultiWrite`
- `Station::OnBounce`
- `Station::_InputChannel`
- `Trigger::OnTick`
- `NinjamConnection::ProcessAudioBlock`
- `NinjamConnection::ConsumeStereoPair`

Reject any addition of blocking or lock-based primitives inside those bodies, including `std::mutex`, `std::scoped_lock`, `std::lock_guard`, `std::unique_lock`, `std::condition_variable`, `EnterCriticalSection`, `WaitForSingleObject`, `SleepConditionVariableCS`, and `SleepConditionVariableSRW`.

## General C++ guidance

- Prefer value semantics, pure transformations, and explicit inputs/outputs.
- Isolate side effects such as I/O, audio device access, rendering, filesystem work, and threading.
- Use `std::vector`, `std::array`, `std::string`, RAII, and smart pointers when performance allows.
- Prefer `std::optional`, `std::variant`, and strong enums over sentinel values.
- Use algorithms and ranges only when they improve clarity without hurting hot paths.
- Use `const` aggressively; pass large objects by `const&`.
- Use `constexpr` and `noexcept` when correct.

## Avoid

- Raw owning pointers or manual `new` and `delete` unless measurably better in hot paths.
- Hidden global mutable state.
- Monolithic functions mixing state changes, I/O, and control flow.
- Exception-driven control flow in real-time paths.

## Published audio snapshots

`PublishedAudioSnapshot<T>` keeps `shared_ptr` ownership on non-callback
threads and gives the audio reader a bounded, move-only borrow. Keep
`load`, `store`, `exchange`, and `collect` off the callback; serialize writer
calls to `store`, `exchange`, and `collect`. Each snapshot publisher supports
eight simultaneous leases across all readers, and each `borrow` makes at most
four publication attempts.
Check `ReadSucceeded()` to distinguish a successfully read empty publication
from a bounded read failure; a successful empty lease has no value.

Keep published topology and ownership immutable while a lease can reference
them. Mutable processor state still requires its own thread ownership or
atomic publication rules; a lease protects lifetime only. Before
destroying the snapshot owner, stop its audio reader and release all leases.
Call `collect` from a writer or another non-callback pump so retired objects
whose leases ended can be reclaimed even when no further publication occurs.


### Session reset and rack retirement

Both session reset modes share SessionControl's existing lifecycle writer and
callback drain. Reset Session retains Station VST instances and exact pending
requests. Reset Session + VSTs closes native editors outside the Scene lock,
invalidates all original rack requests, clears every slot and its plugin routes,
and publishes operational empty audio, MIDI delivery and automation state.
It does not use Station's shutdown-only force-unload path. Removed takes use their
existing terminal retirement path; surviving stations remain editable.

Rack completions must match the original staged request ID, slot identity and job
type before SDK load and installation. A stale request cannot decrement another
request's pending count. UI PreInit and job queueing preserve the original token;
compatibility producers explicitly stage before either step. Managed SDK failure
finishes its exact pending identity and retires the local processor on the UI
queue, then preserves Scene's existing fatal job failure policy.

Only the drained VST-removal reset makes a bounded final note/pedal release
attempt before removing destinations. Ordinary reset retains existing deferred
release retries on resume. Plugin destruction remains on the UI destroy queue
once all published-state/editor pins have been released. No reset preparation,
slot allocation, SDK lifecycle call or plugin retirement runs on the callback.
