# Jamma architecture and ubiquitous language

This is Jamma's canonical short reference for core concepts, ownership, and terminology. Use these meanings consistently in code, tests, documentation, and discussion. For detailed timing algorithms and invariants, see [Loop alignment and NINJAM sync](loop-alignment-and-ninjam-sync.md).

## Core model

| Term | Exact meaning |
| --- | --- |
| Scene | The running session's job/UI orchestrator and top-level composition. It manages presentation and wiring across subsystems but should not absorb audio, loop, remote-timing, or persistence ownership. |
| Station | A performance/mixing channel and the owner of an ordered set of LoopTakes - typically a single instrument like drums, guitar, etc. It fans audio/timing operations to its takes and can host station-level routing and VST effects/instruments. |
| LoopTake / take | One recorded performance layer within a Station. It can contain multiple loops in cases where content is multichannel, like stereo keys, drum mics, etc. It owns its recording/playback state, MIDI/audio Loop material, effects, and entity timing anchors. |
| Loop | One audio or MIDI recording/playback entity within a LoopTake. It owns its audio buffers (or array of events in the case of MIDI Loops), logical length, read/write positions, offsets, fades, and loop-local playback phase. |
| `Station -> LoopTake -> Loop` | The ownership and processing hierarchy. Operations flow down this hierarchy; child-specific state and geometry stay with the child rather than being collapsed into a shared cursor. |
| Trigger | A physical input such as keyboard, touch, MIDI, or serial input, that initiates record, play, overdub, punch, ditch, or related actions. A Trigger requests behavior; it does not own transport state. |
| Record | Capture new audio/MIDI material into a take/loop according to the active action and latency/quantisation scheduling. |
| Overdub | Construct a new Loop consisting of a prior loop plus some amount of new material. It is a recording state/action and does not establish a new master interval. |
| Punch in/out | Temporarily replace or add material over a bounded part of a Loop being Overdubbed. |
| Ditch | Discard or cancel the relevant current recording/performance state associated with a Trigger. |
| AudioHost | The audio-device/callback boundary and owner of block-time application of published engine state. It coordinates callback work but does not own per-loop state or job-side policy. |
| Timer | The shared musical clock used by the audio engine. It tracks the master interval length, position within that interval, completed interval count, and a separate continuity counter. It does not own per-loop cursors. |
| Quantiser | The engine service that chooses quantisation geometry and event boundaries. It does not own NINJAM follow policy, transport mapping, or entity playback state. |
| NINJAM | The remote collaborative-jamming subsystem. Its job side owns remote connection/timing authority and publishes complete desired state; audio application remains with AudioHost. |
| Snapshot | An immutable published view used to cross an ownership/thread boundary safely. Publishing a snapshot does not transfer ownership of the underlying subsystem's decisions. |

## Ownership boundaries

- The NINJAM job side owns connection availability, session epoch, validated remote observations, tempo requests/prompts, follow policy, remote grid, and production of one complete desired transport value. It never mutates Timer or loop cursors directly.
- Scene is job/UI orchestration. It presents prompts and diagnostics and forwards complete values; it does not reconstruct remote timing authority or format diagnostics on the audio callback.
- AudioHost owns the audio-block application boundary. It accepts the latest complete desired value, replaces or disciplines Timer, owns the one common remote-to-local map calculation, and sends neutral reset/correction/restore operations down the engine hierarchy.
- Timer owns the shared musical clock and its continuity counter. It does not own any loop's cursor or remote-follow policy.
- `Station -> LoopTake -> Loop` owns membership and each entity's length, anchor, audio phase, MIDI event cursor, and automation origin. These objects apply neutral timing operations using their own geometry; they do not own NINJAM policy or the common map.
- Quantiser owns quantisation choices and boundary calculations. Local grain-based inference and an authoritative remote grid are distinct inputs; Quantiser does not own remote follow policy or transport mapping.
- Cross-thread timing travels as complete coherent values through the established mailbox/snapshot patterns. Diagnostics may mirror timing identity for correlation but never become authority.

Audio-callback work must remain bounded, allocation-free, exception-free, lock-free, and free of waits, formatting, blocking I/O, and unbounded hierarchy traversal.

## Timing, synchronisation, and quantisation language

| Term | Exact meaning |
| --- | --- |
| Transport | A shared musical clock: it says how long the repeating master interval is, where playback is within that interval, and how many intervals have completed. Transport may be relocated or resynchronised; it is not the audio content or any individual loop's playback cursor. |
| Master interval | The top-level repeating span used by Timer. In local-only use it is normally established from the master loop; while following NINJAM it is the accepted NINJAM interval. Other loops may be longer, shorter, or offset from it. |
| Master length | The duration of one master interval, measured in audio samples. Changing it changes how master phase and master loop count are interpreted. |
| Master phase | The current sample position within the master interval, from zero up to but not including the master length. It may move forward normally or jump when the user or remote synchronisation relocates the transport. It is not a per-loop cursor. |
| Master loop count | The number of times Timer has crossed the end of the current master interval since the current timing setup began. Timing replacement may reset it, so it is not a permanent count for the whole Scene. |
| Local master transport | The current state of Jamma's shared musical clock: master length, master phase, and master loop count. `AbsoluteSamplePos` is a value derived from those three fields; `SceneSamplePos` is a separate continuity counter rather than part of the musical position. |
| Master absolute sample position | `AbsoluteSamplePos`: master loop count multiplied by master length, plus master phase. It is an unwrapped position on the **current master timeline**, not a count of all audio processed since the Scene began. It may jump or reset when master timing is replaced or relocated. |
| Scene sample counter | `SceneSamplePos`: the number of audio samples processed by Timer's `Tick` calls. The current implementation only increases it and does not reset it when Timer is cleared, phase-corrected, or given new remote timing. It exists so timing maps can measure elapsed audio across those changes; it is not the musical playhead and should not be used for UI seeking or scratching. |
| Phase | A position within one repeating interval or one entity's length. Phase wraps to zero at the end, so always name the interval or entity: master phase, remote-master phase, audio-loop phase, and so on. |
| Per-entity phase | A cursor wrapped by that audio/MIDI/automation entity's own logical length. Different entities may intentionally have different phases and lengths. |
| Remote timing | A validated NINJAM observation containing authoritative BPM, full BPI, interval geometry, phase, sample rate, and observation anchors. It is converted to device-rate units before application. |
| Local timing | Device-rate transport and loop advancement owned locally, including intentionally different loop lengths and offsets. It remains distinct from remote authority. |
| BPM | Beats per minute: tempo, not interval length or phase. |
| BPI | Beats per NINJAM interval. Remote authority requires the full observed BPI; disconnected local inference is a separate Quantiser calculation. |
| Grain | The exact local audio construction unit used when deriving local loop geometry. It is not a remote beat and need not equal a quantisation-grid step. |
| Quantisation | Choosing or snapping to allowed event boundaries. It controls when an action occurs; it is not transport synchronisation or phase restoration. |
| Active quantisation grid | The current set of rounded boundaries across an interval, derived from its division count. Changing the grid does not itself move or restore loop phase. |
| Remote grid step | A remote-authority grid spacing derived from accepted remote interval/BPI information. It is distinct from local grain. |
| Sync phase map | AudioHost's conversion between elapsed NINJAM time and elapsed local-source time. It stores the two interval lengths and their starting points, not a shared loop cursor. |
| Mapped elapsed time | Common local-source progress derived from remote elapsed time. Every entity receives the same elapsed amount and wraps it by its own length. |
| Anchor | A captured relationship between a coordinate and an entity-specific phase/source position. Anchors preserve relative offsets and are invalidated before an independent follow session. |
| Origin | The zero/reference point of a particular mapping or counter. Always name its domain; remote-grid origin, map origin, and automation origin are not interchangeable. |
| Correction | A signed adjustment applied within a named domain. A master-phase correction is not automatically a loop-phase replacement. |
| Timing geometry | The fixed measurements needed to interpret a position, such as interval length, sample rate, BPI, and grid divisions. It describes the scale being measured; it does not say where playback currently is or whether remote timing should be followed. |
| Session epoch | Identity of one physical remote-authority lifetime. Disconnect/replacement prevents stale state from crossing into a new epoch. |
| Desired version | Monotonically newer identity for complete desired transport publications within the lifecycle. It is the primary latest-value application order. |
| Command generation | Identity of accepted replacement, join-alignment, or phase-discipline work. Per-entity generation gates reject stale/repeated application. |
| Desired transport state | One complete, latest-substitutable job-to-audio value containing identity, intent, policy, geometry, and timestamped remote phase. It is not a sequence of partial commands. |
| Applied receipt | The audio boundary's coherent acknowledgement of the desired identity it accepted. It is diagnostic/correlation data, not new authority. |
| Follow policy | The choice of whether and how accepted remote authority disciplines local state. It is not a clock, cursor, or coordinate system. |
| `ContinuousSync` | Follow policy for compatible remote/local tempo, normally using smaller ongoing corrections. |
| `BlockSync` | Follow policy for materially different accepted tempo, using the same map/anchor model with potentially larger corrections. |
| `NoSync` / Stay local | No remote transport authority. It clears remote map, anchors, and generation gates without moving local audio, MIDI, or automation cursors. |
| Remote join | Session request/acknowledgement and follow decision followed by audio-boundary application of accepted desired state. It is not itself a coordinate system. |
| Loop alignment | Restoring each local entity from its own anchor plus common mapped elapsed time, preserving intentional relative offsets. |
| Mailbox | A bounded coherent cross-thread latest-value transport. A mailbox carries authority or observations owned elsewhere; it does not acquire ownership itself. |

Do not merge these concepts merely because their numeric units match. Any simplification must preserve behavior for unequal loop lengths, intentional offsets, reconnects, delayed observations, and `NoSync` invalidation.
