# Editable HUD connections

## Outcome and scope

Replace the HUD's prototype cables with a real, editable view of rig-owned
routing:

- ADC capture channel -> trigger
- MIDI capture/live-input device -> trigger
- trigger -> JAM station

The rig owns those routes and trigger definitions. The JAM continues to own
stations, takes, loops, effects, and transport/session state. `StationType`
remains a compatibility field; it is not route identity.

Keyboard/serial pairs and the optional MIDI note/CC trigger binding are
**activation bindings**, not capture cables. Preserve and dispatch them, but do
not make them cable-editable in this feature. A newly added trigger has no
external activation binding but remains usable from its two on-screen pedals.

No undo, dynamic rig reload, or runtime JAM replacement is included. The app
currently constructs one `Scene`, and `Window` owns it by reference. Validate
name-based resolution by constructing scenes from reordered/renamed JAM data;
do not add scene swapping to this change.

## Existing-code constraints

The implementation must account for these current facts:

- `Scene::FromFile` attaches rig trigger `i` to JAM station `i`.
- `GuiHud::_RebuildCableVertices` draws source `i % triggerCount` and trigger
  `i -> station i`; neither vector is routing authority.
- `Station::_triggers` is a mutable vector read by `OnTriggerEvent`, `OnTick`,
  `OnBounce`, and `AcceptsLiveMidiFromDevice`. `OnTick` and `OnBounce` are on
  the audio callback path.
- `Trigger::_receiver`, `_inputChannels`, and `_midiInputDevices` are mutable.
  Never rewrite them on a live trigger from the UI thread.
- `MidiRouter` separately publishes trigger routes and live-MIDI recipients.
  Adding a revision number to those separate publications would not make them
  coherent.
- `AudioHost::_audioStations` is the useful precedent: a complete immutable
  value is built off-thread and acquired once by the callback.
- `RigFile::ToStream` is diagnostic output, not JSON serialization, and the
  app has no rig-save command. Persistence therefore needs an explicit app
  callback and serializer; there is no existing "normal rig-save path" to use.
- `RigFile::Trigger::FromJson` rejects a name-only trigger today; that must be
  relaxed for the `+` workflow.

## Persisted model

Extend `io::RigFile::Trigger` with:

```cpp
std::optional<std::string> StationTarget;
MidiInputMode MidiInputs; // LegacyAny, None, Any, or Selected
```

Use JSON keys `stationtarget` and `midiinputmode`, with mode values `none`,
`any`, and `selected`. `LegacyAny` is an internal parse result only; the writer
emits it as explicit `any`.

`StationTarget` rules:

- absent means legacy positional input and is eligible for the one-time
  migration below;
- present but empty means deliberately unbound and must never use positional
  fallback;
- present and non-empty resolves only against `JamFile::Station::Name`.

`MidiInputMode` removes the current ambiguity in an empty
`MidiInputDevices` vector:

- when the new field is absent, a non-empty device list becomes `Selected`,
  while an empty list becomes `LegacyAny` to preserve current behavior;
- `None` means no MIDI capture/live-input route and is used by a new trigger;
- `Any` is an explicit wildcard route;
- `Selected` requires at least one unique, non-empty device name.

Keep `InputChannels` as the ADC capture routes. Keep `TriggerPairs` and
`MidiTrigger` unchanged as activation configuration.

Add a real JSON writer that round-trips every known rig/user/trigger field,
including all activation bindings, and JSON-escapes strings. Do not repurpose
the diagnostic `ToStream` silently; give persistence an unambiguous API such
as `RigFile::ToJsonStream`. Add an optional save callback/path to the app/Scene
wiring. Saving must write a sibling temporary file and atomically replace the
rig file. If serialization or replacement fails, keep the current live and
displayed revision and show/log a concise error.

Preserving unknown JSON fields is not supported by the current parsed model;
document that limitation in the serializer test rather than pretending they
survive a round trip.

### Trigger identity

Do not invent persistence identity from a trigger name. Existing names are not
validated unique. Inside one immutable revision use value handles:
`{revision, rigTriggerIndex, routeKind, routeIndex}`. Cancel transient HUD state
whenever the revision changes. `Trigger-N` naming is only a friendly-name rule,
not identity.

## Pure resolution

Add a pure resolver in JammaLib which takes a `RigFile` and the JAM station
descriptors and returns both resolved and unresolved records. The records are
plain values: trigger index/name, source kind/key, optional target name,
scene-local station index, availability, and warning reason. They contain no
GUI objects or raw pointers.

Resolve every rig trigger independently:

1. Validate/normalise its persisted capture and activation fields.
2. Resolve its target by exact station name.
3. If exactly one station matches, record that route.
4. If none match, retain the trigger as unbound with `target missing`.
5. If multiple stations have that name, retain it as unbound with
   `target ambiguous`; never select the first match.
6. Mark unavailable ADC channels or MIDI devices on their source edges, but do
   not reject an otherwise valid station target. Unavailable sources dispatch
   no input and remain visible for diagnosis.

Several triggers may deliberately target one station. Equivalent capture
routes on one trigger are duplicates and must be normalised/rejected.

For a trigger whose `StationTarget` is absent, resolve index-to-index only when
that JAM index exists. On success, set `StationTarget` to the resolved station
name in the candidate rig, emit one migration warning, and save it through the
new rig writer. An out-of-range legacy trigger remains unresolved and absent;
do not guess by type, proximity, or another index.

Construct a runtime `Trigger` for every syntactically valid rig trigger so an
unbound trigger can still be shown and operated by its HUD pedals. Set its
receiver and include its external activation dispatch routes only when its
station target resolves.

## Runtime ownership and publication

### Immutable revision values

Introduce one `RoutingRuntimeSnapshot` built entirely off the audio path. It
contains:

- monotonic `Revision`;
- the resolved/unresolved value graph used by the HUD;
- runtime trigger objects and their immutable station receivers;
- each local station's complete immutable trigger list;
- one complete input-dispatch value covering MIDI-trigger activation routes,
  live-MIDI recipients, and serial/keyboard traversal policy.

The candidate must own every `shared_ptr` needed by its readers. HUD drag state
stores only the value handles described above.

An edit never mutates a published `Trigger`. For a changed trigger, construct a
replacement from the candidate rig and assign its receiver before publication.
Unchanged triggers may be reused. This avoids racing the plain
`ActionSender::_receiver` or the trigger's mutable input vectors.

### Edit eligibility

Expose one atomic display accessor such as `Trigger::CanEditRouting()`,
published by the audio-owned state machine. It is true only when:

- the trigger is in `TRIGSTATE_DEFAULT`;
- neither activate nor ditch input is down;
- its external-action queue is empty and it has no delayed/in-flight action;
- it owns no take/history that would be stranded by replacing its receiver.

Disable source edits, station moves, and deletion unless this predicate is
true. This deliberately strengthens the original "not recording" rule:
moving a merely playing trigger is also unsafe because its take IDs belong to
the old station.

The UI value is advisory. A release must gate input for the affected trigger
and obtain a fresh audio-boundary quiescence acknowledgement before saving or
publishing the candidate; this closes the race where a physical event arrives
after the UI's last state read.

### Two-boundary commit protocol

UI, input/job, and audio readers must never observe independently assembled
parts of a route. Use this protocol:

1. On pointer release, build and validate the desired model mutation without
   changing the current rig. Gate new HUD/keyboard/hardware trigger actions for
   the affected trigger. MIDI/serial ingress may continue queueing with the old
   revision tag.
2. At the next audio boundary, drain already accepted trigger actions and
   acknowledge quiescence only if the edit predicate is still true. On
   rejection, remove the gate and leave the old revision unchanged. On success,
   construct all replacement runtime objects off-thread and persist the
   candidate rig; any failure likewise removes the gate without publication.
3. Publish the complete candidate as `pending`. Events tagged with the previous
   revision must now be dropped rather than delivered to a different route.
4. At the start of an audio block, `AudioHost` consumes the pending revision,
   publishes each complete station trigger snapshot, then records
   `audioAppliedRevision`. Do all stores before processing any station in that
   block.
5. The job/input boundary observes the audio acknowledgement, atomically swaps
   one complete input-dispatch snapshot, stamps/accepts events only for that
   revision, and records `inputAppliedRevision`.
6. The UI promotes the graph and rig to `displayed/current` only after both
   acknowledgements equal the candidate revision, then removes the gate.

Keep the previous runtime snapshot strongly owned by the coordinator until
both acknowledgements advance. Retire it on the UI/job side so the audio
callback cannot perform last-reference destruction. Coalesce pending edits to
the latest complete candidate only when no confirmation dialog or drag depends
on the superseded revision; otherwise disable further edits while applying.

`Station` trigger membership becomes an
`atomic<shared_ptr<const vector<shared_ptr<Trigger>>>>`. Every existing
traversal loads one snapshot once and iterates that local value. `Reset()` must
stop clearing rig trigger membership; only the routing coordinator changes or
empties that snapshot.
No callback-owned function may allocate, lock, log, wait, resolve names, or
rebuild routes.

Replace `MidiRouter`'s independently mutable trigger-route vector and live
recipient publication with the single input-dispatch revision. Preserve its
current device-generation stale-event protection and make the routing revision
part of that gate. Serial should use a station snapshot and the same revision,
not mutable `Scene::_stations`; retaining bounded broadcast-and-match behavior
is acceptable.

The feature must not introduce a second producer into the existing trigger
external-action SPSC queue. Preserve the current producer contract or give UI
and job input separate SPSC queues drained by audio; do not turn it into an
unsafe multi-producer ring. Treat broader trigger event-thread refactoring as a
separate change unless tests prove it is required here.

### Shutdown

On shutdown, stop accepting edits, cancel the drag/dialog, publish an empty
input routing revision, stop/join input and job threads, stop audio, then
release routing snapshots and triggers. Keep the existing `Scene::Shutdown`
ordering for plugins/devices. `CloseAudio`/`Shutdown` must remain safe when the
app has already closed audio once.

## HUD model and interaction

`GuiHud` receives only the completed graph revision and per-frame station
anchors. Add the scene-local station index/name to each `StationAnchor`; never
infer identity from anchor vector order. Remove all modulo/index cable
generation.

Reuse existing UI infrastructure:

- make the cable editor/HUD return itself as `ActionResult::ActiveElement` on
  pointer-down so `Scene::_touchDownElement` provides capture;
- add uncaptured HUD hover routing from `Scene::OnAction(TouchMoveAction)`,
  because Scene currently forwards moves only to a captured element/camera;
- route Escape to the active HUD drag before global shortcuts;
- interpret right-click from the existing touch index/button state;
- use `GuiScrollPanel` for the trigger list and a separate fixed footer for
  `+`; explicitly size the scroll content to its logical height;
- use `GuiPopup`/`GuiPopupManager` for named delete confirmation.

Extract pure helpers for endpoint layout, compatibility, nearest-socket choice,
cable-body closest-end selection, and hysteresis. Keep OpenGL code limited to
rendering the resulting control points/colors.

Interaction rules:

- Show a visible socket plus a larger DPI-scaled hit target for each available
  source, trigger capture input, trigger output, and station target.
- A trigger may have many distinct ADC/MIDI capture edges and one station edge.
  Represent explicit `Any` MIDI as a labelled wildcard source. Never draw an
  activation binding as a capture cable.
- Keep trigger bodies fixed height. Spread two or more input sockets evenly
  over the available vertical span; keep the output socket distinct.
- A station may receive many triggers. Spread occupied sockets plus one
  creation socket horizontally across a fixed span above its tube. The tube is
  the larger target for a new station connection; an existing edit snaps back
  to its own occupied socket.
- Down on a socket/cable begins immediately. A cable-body down selects the
  nearest end in screen space. Keep the original live cable unchanged and draw
  a separate provisional cable to the pointer/snap target.
- Evaluate only directional, compatible targets and exclude an equivalent
  existing capture route. Within radius `R`, select the nearest viable socket;
  retain it until distance exceeds `R + H`.
- Release on a valid snap commits one candidate revision. Release in empty
  space removes the selected route. Escape/right-click cancels without model
  mutation. A station move replaces the old target; it never adds a second
  target.
- During pending publication, show `applying` and disable new edits. Invalid,
  unavailable, occupied/reconnectable, and locked states need a non-color cue
  (shape/icon or short label) as well as color.
- `x` captures before the trigger body/pedals, cancels any related drag, and
  opens confirmation only when `CanEditRouting()` is true. Confirm removes the
  rig trigger and all of its routes; cancel changes nothing.
- `+` creates `Trigger-N` using the first unused positive suffix, with no ADC
  input, `MidiInputMode::None`, no activation binding, and an empty
  `StationTarget`. Clamp scroll after add/delete/resize and reveal the new item.

## Implementation slices

Each slice should compile and test before the next begins.

1. **Schema and resolver**
   - Add `StationTarget`, explicit MIDI input mode, name-only trigger parsing,
     complete rig JSON serialization, atomic app save callback, pure mutation
     helpers, resolver, migration diagnostics, and tests.
   - Keep runtime construction positional until the resolver tests are green.

2. **Read-only real graph**
   - Add resolved graph/runtime value types.
   - Change `Scene::FromFile` to build every station first, resolve every rig
     trigger, attach/register only resolved targets, retain unresolved HUD
     nodes, and publish the initial station list once.
   - Make `GuiHud` render this graph without editing; remove synthetic cables.

3. **Thread-safe runtime publication**
   - Add station trigger snapshots and convert all four traversals.
   - Add the pending/audio/input acknowledgement coordinator and one complete
     input-dispatch snapshot; remove append-only MIDI trigger registration.
   - Add trigger quiescence publication, stale-event gating, retirement, and
     shutdown tests. Run the hot-path audit before enabling editing.

4. **Pure cable interaction**
   - Implement/test endpoint geometry, hit testing, direction/compatibility,
     closest-end selection, nearest snap, hysteresis, preview, cancel, and
     release-to-candidate operations without GL assertions.
   - Wire HUD capture, uncaptured hover, Escape, and right-click.

5. **Lifecycle and layout**
   - Add fixed-footer `+`, scroll/reveal behavior, `x`, popup confirmation,
     edit lock cues, and route cleanup.
   - Connect successful candidates to the revisioned commit protocol and
     persistence callback.

6. **Integration and hardening**
   - Exercise reordered, renamed, missing, duplicate, and fewer/more stations
     by constructing fresh scenes.
   - Verify unavailable sources, rapid edits, stale input, failed persistence,
     close during apply, and repeated close. Manually inspect every callback
     body named in `doc/realtime-audio.md`.

## Verification matrix

- **Rig/resolver:** full parse/serialize round trip; target absent/empty/name;
  `None`/`Any`/`Selected` MIDI modes; unbound trigger; unique `Trigger-N`;
  legacy in/out-of-range; reordered/missing/ambiguous stations; many triggers
  to one station; unavailable source; duplicate capture rejection.
- **Runtime:** only resolved triggers receive station/input dispatch; one
  snapshot load per traversal; replacement triggers keep receivers immutable;
  active/history-bearing triggers reject edits; audio then input acknowledgement;
  stale events dropped; old snapshots retired off audio; shutdown during apply.
- **HUD:** real graph only; endpoint spacing; anchor identity; hover/capture;
  closest-end body selection; directional nearest snap; hysteresis; original +
  preview; duplicate exclusion; unplug/replace/cancel; non-color status cues.
- **Lifecycle/layout:** add, confirm/cancel delete, control precedence, fixed
  footer, scroll range/clamp, resize preservation, and reveal-new-trigger.

Before every build/test run, read `.vscode/tasks.json`. Build incrementally and
run `JammaLib_Tests`. After the threading slice and final integration, run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .agents/skills/threading-review/audio-hotpath-audit.ps1
```

Then manually inspect `Scene::OnTick`, `Scene::AudioCallback`, `Scene::_OnAudio`,
`Station::OnBounce`, all converted trigger traversals, and the input dispatch
loops for new locks, waits, allocation, formatting, or mutable-container reads.

## Definition of done

- HUD fixed cables exactly match the current persisted rig revision; unresolved
  and unavailable endpoints are deliberate and diagnosable.
- A valid release is all-or-nothing across persisted rig, audio membership,
  input dispatch, and displayed graph; cancel/failure preserves the old route.
- No route edit mutates a published trigger or strands its take history.
- No index/type fallback occurs except the documented one-time absent-target
  migration, and duplicate station names fail safely.
- Audio callback paths remain bounded, allocation-free, lock-free, wait-free,
  exception-free, and free of logging.
