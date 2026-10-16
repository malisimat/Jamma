# Rig cleanup integration semantic and threading review

Reviewed against integration baseline `f6d4c939`, source `5d7c2859`, and common
ancestor `cff00825`. Build, migration and executable evidence is recorded in the
integration handoff; this record describes the intended behavior and code review.

## Schema and runtime behavior

- Rig persistence accepts canonical version `2.0` only. The strict reader rejects
  malformed JSON, duplicate/unknown fields, unsupported versions, invalid UTF-8,
  invalid device references and out-of-capacity bindings. Required capture lists
  distinguish an explicit empty subscription from omitted configuration.
- MIDI registry IDs identify controls, capture routes and recorded streams;
  hardware names select ports. IDs are unique across MIDI, serial and the reserved
  keyboard device. Hardware matching is exact; `default` selects the first
  unclaimed input. Persisted duplicate hardware names remain unavailable where
  WinMM cannot establish a safe physical identity.
- Activate and ditch bindings are independent. Textual control-section order and
  order within each list determine priority. Normalization removes an entire
  later overlapping binding, with a warning, so a retained press cannot lose its
  release. Any-channel MIDI matchers expand to sixteen slots; each control has a
  bounded compiled capacity of 64. Sharing inputs across triggers remains valid.
- Audio and MIDI capture selections are independent of control devices. Missing
  hardware or Station targets retain their configuration and report availability;
  they do not cause a curated rig to be replaced. Reopened devices resolve by
  registry ID. First-run routes use the successfully opened input subset without
  renumbering the configured registry.
- The canonical profile remains `%APPDATA%/Jamma`. Invalid/unreadable definitions
  use the existing recovery policy and preserve the original selected file.
  Saving normalizes, serializes and reparses the exact bytes before replacing a
  rig. Performance routing/membership edits remain in memory until explicit save;
  confirmed hardware preferences use the existing separate save workflow.
- Station membership preserves receiver identity through reordering/renaming and
  uses a null target when explicitly unbound. Historical takes resolve across
  the restored session by take ID; ditch uses the historical target's owner,
  never a newly selected rig target. Missing or ambiguous history stays unresolved.
- MIDI settings retain each registered ID even when hardware selectors coincide.
  Newly registered inventory choices allocate IDs against the entire registry.
  Reconciliation caching and endpoint reuse retain registry identity separately
  from physical endpoint matching; success and partial recovery keep connected
  hardware descriptors aligned with their registry IDs.

## Timing units

`preDelay` becomes `preDelaySamps` without numerical conversion: alignment,
playback offsets and ADC compensation already consume samples. The legacy field
`debounceSamps` was misnamed: baseline `Scene.cpp` assigned it directly to
`TriggerParams::DebounceMs`, while `Trigger::Debounce` and `OnTick` compared it
with elapsed seconds multiplied by 1000. Therefore conversion copies its number
to `debounceMs`; a sample-rate conversion would change established behavior.

## Cross-thread ownership

| State | Writer / owner | Readers and publication | Teardown |
| --- | --- | --- | --- |
| Rig availability, diagnostics and display-name handles | Serialized non-callback coordinator/Scene lifecycle | Immutable rig snapshots; existing bounded audio snapshot leases and input publication | Readers stop before coordinator snapshot release; existing retirement collection |
| Trigger display name | Job/UI transition owner | Atomic immutable string handle; job/UI presentation readers only | Trigger/snapshot ownership retains strings; no callback shared-pointer loads added |
| Connected MIDI and serial IDs | MIDI lifecycle owner | Complete immutable `MidiState` published atomically to UI/job readers | Lifecycle closes endpoints before input ownership is released |
| MIDI desired settings and connected-ID arrays | Reconciliation owner, under existing router publication mutex | Cached values remain off callback; immutable input dispatch uses endpoint slots and registry IDs | Dispatcher joins and backend callbacks stop before releasing endpoints |
| MIDI choice IDs/toggles | UI owner | UI events and device-page reconstruction only | Scene UI lifetime |
| Historical take receivers | Loader before publication, or serialized trigger replacement | Existing capture-session pins and bounded audio history ledger | Existing capture-session retirement and scene reader shutdown |

The threading-review audit reports no new forbidden lock/wait operations in the
audio hot-path diff. Manual review of changed `Trigger::OnTick`, capture-route
swaps, AudioHost transition checks and MIDI ingress confirms that new allocation,
formatting and registry reconciliation remain off the audio callback. Audio route
changes swap prepared storage; frozen `BoundReceiver` identity remains distinct
from mutable prepared handles. The JSON change appends null alternatives, keeping
legacy variant indices stable; legacy JAM/defaults readers retain their parser.

Review found and corrected merge hazards involving automatic saving, removed
availability caches, empty-string targets, loader ownership, MIDI endpoint reuse,
connected-ID recovery, and settings UI registry identity. Hardware acceptance
still requires the user's device/performance checks; native suites establish only
their exercised behavior.
