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
- `Scene::AudioCallback`
- `Scene::_OnAudio`
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
