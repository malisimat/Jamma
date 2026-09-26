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

## Implemented architecture

The editable-routing implementation now uses these boundaries:

- persisted Trigger GUIDs, rather than vector positions or names, identify a
  Trigger across reorder and route edits;
- `RigSnapshot` is the only complete routing/dispatch authority;
- Trigger owns its operational state and current capture route on audio. Its
  fixed audio history holds stable tokens plus narrow punch targets, while a
  job-owned ledger holds immutable IDs and strong receiver ownership;
- Station owns only `Station -> LoopTake -> Loop` state and never retains,
  ticks, or queries Triggers;
- HUD/keyboard and MIDI/serial use separate bounded SPSC lanes into audio,
  stamped with the immutable dispatch revision;
- `RigTriggerInputGate` closes both producer domains asynchronously before
  audio-boundary transition is requested;
- retained route changes exchange prebuilt capture values at the audio
  boundary, while unchanged Triggers are untouched;
- structural Station work crosses a fixed-capacity audio-to-job command/result
  handoff, so the callback never invokes Station mutation directly;
- active overdub source/writer linkage is owned by the target LoopTake;
- `AudioHost::_audioStations` is the useful precedent: a complete immutable
  value is built off-thread and acquired once by the callback.
- `RigFile::ToStream` remains diagnostic output; persistence uses the explicit
  JSON serializer and app save callback.
- `RigFile::Trigger::FromJson` accepts a name-only trigger for the `+` workflow.

## Persisted model

Extend `io::RigFile::Trigger` with:

```cpp
std::optional<std::string> StationTarget;
MidiInputMode MidiInputs; // None or Selected
```

Use JSON keys `stationtarget` and `midiinputmode`, with mode values `none` and
`selected`.

`StationTarget` rules:

- absent means legacy positional input and is eligible for the one-time
  migration below;
- present but empty means deliberately unbound and must never use positional
  fallback;
- present and non-empty resolves only against `JamFile::Station::Name`.

`MidiInputMode` removes the current ambiguity in an empty
`MidiInputDevices` vector:

- when the new field is absent, a non-empty device list becomes `Selected`,
  while an empty list becomes `None`;
- `None` means no MIDI capture/live-input route and is used by a new trigger;
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

Use one `engine::RigSnapshot` built entirely off the audio path. It
contains:

- monotonic `Revision`;
- the resolved/unresolved value graph used by the HUD;
- the complete ordered runtime Trigger list, including stable IDs;
- explicit retained-route-change and retired-instance records;
- one complete input-dispatch value covering MIDI-trigger activation routes,
  live-MIDI recipients, and serial/keyboard traversal policy.

The candidate must own every `shared_ptr` needed by its readers. HUD drag state
stores only the value handles described above.

Activation-definition changes construct replacement Triggers. Capture-source
or station-target edits reuse the stable Trigger instance so its history is
preserved; the candidate owns prebuilt route values which audio exchanges at
the publication boundary. Unchanged Triggers are not mutated.

### Edit eligibility

Expose atomic display accessors published by the audio-owned state machine.
`Trigger::CanEditRouting()` is true only when:

- the trigger is in `TRIGSTATE_DEFAULT`;
- neither activate nor ditch input is down;
- its external-action queue is empty and it has no delayed/in-flight action;
- it owns no take/history that would be stranded by replacing its receiver.

Capture-source and station-route edits instead use an idle-only predicate and
retain the existing trigger instance. At the audio boundary they exchange the
prepared capture configuration and receiver. Each recorded take retains the
receiver that created it, so end, punch, and ditch actions continue to reach
the right station in LIFO order after a station move. Deletion or an edit that
replaces a trigger still requires `CanEditRouting()`.

The UI value is advisory. A release must gate input for the affected trigger
and obtain a fresh audio-boundary transition acknowledgement before saving or
publishing the candidate; this closes the race where a physical event arrives
after the UI's last state read.

### Two-boundary commit protocol

UI, input/job, and audio readers must never observe independently assembled
parts of a route. Use this protocol:

1. On pointer release, build and validate the complete candidate without
   changing the current rig, then close the accepted revision's UI ingress.
2. At the top of a job tick, stop MIDI/serial acceptance and acknowledge that
   producer. Only after both producer acknowledgements does Scene request an
   audio-boundary transition decision for the affected Trigger instances.
3. Publish the complete candidate as `pending`. Events tagged with the previous
   revision must now be dropped rather than delivered to a different route.
4. At the start of an audio block, `AudioHost` consumes the pending revision,
   applies only explicit retained-route changes, adopts the applied snapshot,
   then records `audioAppliedRevision`.
5. The job/input boundary observes the audio acknowledgement, atomically swaps
   one complete input-dispatch snapshot, stamps/accepts events only for that
   revision, and records `inputAppliedRevision`.
6. The job/UI coordinator promotes the graph and rig only after both
   acknowledgements equal the candidate revision, rebuilds the HUD, and opens
   the candidate revision last.

Keep the previous runtime snapshot strongly owned by the coordinator until
both acknowledgements advance. Retire it on the UI/job side so the audio
callback cannot perform last-reference destruction. Coalesce pending edits to
the latest complete candidate only when no confirmation dialog or drag depends
on the superseded revision; otherwise disable further edits while applying.

Audio ticks every Trigger exactly once from its applied `RigSnapshot`.
Station tick remains responsible only for Station/LoopTake tail and visual
state. There is no Station Trigger membership or reverse Trigger traversal.
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
  MIDI capture edges always name a configured device. Never draw an activation
  binding as a capture cable.
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

### HUD icon TGA assets

Use the new `.agents/skills/tga-icon-gen` skill whenever this feature needs a
new raster HUD control asset. In particular, create the trigger `+` and close
(`x`) button TGAs, including their `_over` and `_down` states, through that
skill rather than hand-authoring or directly converting final textures.

Follow its staged SVG/CSS-to-TGA pipeline: draw the symbolic glyphs as vector
paths (never rasterized text), render at an integer supersample factor,
box-downsample with `render-tga-icon.ps1`, and validate the staged alpha and
dimensions against existing button textures. Keep shading flattened on opaque
fills and use the full-canvas edge/fill color before applying the silhouette
mask so the engine's straight-alpha blending cannot fringe. Keep the controls
at or below 64x64 unless their HUD layout requires an approved exception, and
match the existing rounded-rect family and multi-state suffix convention.

After the staged TGAs are accepted, copy them to
`Jamma/resources/textures/` under new names and add their plain `1 <name>`
entries to `Jamma/resources/ResourceList.txt`; these controls are not
nine-patch backgrounds. Do not register assets or alter the resource list
during staging.

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
   - Publish complete immutable RigSnapshot Trigger and input-dispatch values;
     Station has no Trigger membership or reverse traversal.
   - Add the pending/audio/input acknowledgement coordinator and one complete
     input-dispatch snapshot; remove append-only MIDI trigger registration.
   - Add trigger audio-boundary transition publication, stale-event gating, retirement, and
     shutdown tests. Run the hot-path audit before enabling editing.

4. **Pure cable interaction**
   - Implement/test endpoint geometry, hit testing, direction/compatibility,
     closest-end selection, nearest snap, hysteresis, preview, cancel, and
     release-to-candidate operations without GL assertions.
   - Wire HUD capture, uncaptured hover, Escape, and right-click.

5. **Lifecycle and layout**
   - Use the new `.agents/skills/tga-icon-gen` skill to stage, verify, then
     install/register the `+`/`x` (and `_over`/`_down`) button textures before
     wiring the controls.
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
  `None`/`Selected` MIDI modes; unbound trigger; unique `Trigger-N`;
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
- A valid release is all-or-nothing across persisted rig, audio routing,
  input dispatch, and displayed graph; cancel/failure preserves the old route.
- No route edit mutates a published trigger or strands its take history.
- No index/type fallback occurs except the documented one-time absent-target
  migration, and duplicate station names fail safely.
- Audio callback paths remain bounded, allocation-free, lock-free, wait-free,
  exception-free, and free of logging.
