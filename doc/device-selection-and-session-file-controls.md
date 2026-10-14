# Feature plan: live device selection and session file controls

## Scope and settled behavior

Add **Audio**, **MIDI** and **File** pages to the existing main panel. This document is the implementation plan; it does not describe completed implementation.

- Audio selects one ASIO device and MIDI selects multiple MIDI inputs without restarting Jamma. Changes apply automatically; there are no Apply buttons.
- Audio device, sample-rate and buffer edits become eligible on the next control-loop turn. Input/output channel counts apply only after both counts have stopped changing for **three seconds**.
- Audio changes wait until no recording, overdub, punch or pending capture action is active. Keep only the latest selection while waiting. A short playback interruption is acceptable.
- Sample-rate changes are allowed only when the session has **no takes**, including empty takes. Update plugin formats safely while audio is quiescent.
- Preserve unavailable channel routes in memory only if existing route validation supports this through a small, localized change. Unavailable routes are silent and omitted from saved sessions. Do not add a separate persistent disabled-route model.
- Save may pause audio while copying session state. The first Save chooses a destination folder; subsequent saves create increasing numbered `.jam` files there. **Alt-click Save** chooses another folder. Ctrl+S uses the same normal Save path.
- Load uses the existing `.jam` picker. Cancellation or construction failure retains the current session. Loading does not change hardware preferences.

Hardware preferences belong to the rig save path. Session content and its assets belong to the session exporter. Do not modify vendored RtAudio/RtMidi code or introduce a second exporter, background device worker, or general-purpose transaction framework.

## Existing implementation to reuse

| Area | Starting points | Required work |
| --- | --- | --- |
| Panel | [`GuiMainPanel.h`](../JammaLib/src/gui/GuiMainPanel.h), [`GuiMainPanel.cpp`](../JammaLib/src/gui/GuiMainPanel.cpp), settings assembly in [`Scene.cpp`](../JammaLib/src/engine/Scene.cpp) | Expose Audio, MIDI, Timing and File tabs in that order; reuse existing page slots where practical. Keep the existing MIDI routing page distinct from device enablement. |
| Audio | [`AudioDevice.h`](../JammaLib/src/audio/AudioDevice.h), [`AudioDevice.cpp`](../JammaLib/src/audio/AudioDevice.cpp), [`AudioHost::Init`](../JammaLib/src/audio/AudioHost.cpp) | Reuse ASIO inventory and lifecycle ownership; add explicit exact-selection reconfiguration and complete status. |
| MIDI | [`MidiDevice.cpp`](../JammaLib/src/midi/MidiDevice.cpp), [`MidiRouter.cpp`](../JammaLib/src/midi/MidiRouter.cpp), Scene job-side refresh | Reuse endpoint lifecycle and immutable publications; reconcile enabled preferences with discovered ports. |
| Rig | [`UserConfig.h`](../JammaLib/src/io/UserConfig.h), accepted rig snapshots, existing `SaveRig` path | Persist accepted preferences without losing concurrent rig edits. |
| Load | Ctrl+L handling in [`Window.cpp`](../JammaLib/src/graphics/Window.cpp) and [`Main.cpp`](../Jamma/src/Main.cpp) | Feed the panel command into the same application-loop flow. |
| Save | Ctrl+S in Scene; [`IoSessionExporter.h`](../JammaLib/src/io/IoSessionExporter.h), [`IoSessionExporter.cpp`](../JammaLib/src/io/IoSessionExporter.cpp) | Reuse snapshot/serialization and WAV/MIDI sidecars; repair quiescence handling and parameterize output naming. |

The existing exporter pauses the device and locks Scene while copying most state, then writes files after resuming. It currently treats a failed pause like an already stopped stream, reads some session fields before the protected region, and replaces shared sidecar filenames before publishing `session.jam`. These are requirements to fix, not guarantees to assume.

Read [glossary](glossary.md), [real-time guidance](realtime-audio.md), and [loop/NINJAM alignment](loop-alignment-and-ninjam-sync.md) before changing lifecycle or timing behavior. Station -> LoopTake -> Loop retains ownership of its material, routes and phase; Scene orchestrates and AudioHost owns the callback/lifecycle boundary.

## Audio and MIDI controls and scheduling

### Audio controls

Show device, input count, output count, sample rate and requested buffer size, plus the active stream's actual values. Present a short state: ready, settling, waiting for recording, changing, restored after failure, or stopped/error. Keep pending values visually distinct from active values.

- Inventory supplies device names, probe status, channel limits and sample rates. Retain saved/selected missing or unprobed devices as unavailable rows. A failed enumeration retains the last valid inventory and shows an error.
- Channel selections represent the first N channels, matching the existing configuration; arbitrary channel subsets are outside scope. Validate input/output counts independently, including whether zero in one direction is supported. Never offer a zero/zero stream. Inventory limits are preliminary validation; stream open decides whether the combination works.
- On device change, retain compatible draft counts/rate. If incompatible, choose valid counts within the reported limits and retain the active session rate whenever takes exist. Do not silently choose a different rate to make a device open with existing takes. Explain invalid/unavailable combinations and leave audio unchanged.
- RtAudio's public inventory does not expose supported buffer sizes. Seed the control from saved/active values and retain a bounded, in-memory set of sizes actually accepted for that device. Permit positive integer requests, with overflow validation. Label them requested until open reports the negotiated value. Do not invent a supported range or alter dependencies to discover one.
- Preserve existing advanced rig values, including buffer count and latency overrides; this addition does not expose or reset them.

### Automatic audio request rules

Maintain one complete UI-owned draft, a draft revision, and a channel-settling deadline using a monotonic clock. Every change updates the draft; do not enqueue a FIFO of edits.

1. Device selection rebuilds dependent choices and supersedes the old device's channel-settling deadline. Its resulting complete valid draft is eligible on the next control turn.
2. Each channel-count edit restarts the shared three-second deadline. A rate/buffer edit must not bypass an outstanding channel deadline; the complete draft waits until channel editing settles.
3. Once eligible, submit only if different from the actual active configuration, valid, permitted by the no-takes rate rule, and safe with respect to capture. Recheck these conditions at the lifecycle boundary; a UI check alone races with a new recording action.
4. While recording or another user operation is active, retain only the latest draft. When the blocker clears, reevaluate that draft and its deadline. Do not interrupt a capture to force a change.
5. Edits during an in-flight change remain drafts. Complete the admitted operation safely, then reevaluate the newest draft. A late completion updates the actual active state but never replaces a newer draft.
6. Failed requests do not retry on every control tick. Retry requires an edit or an explicit Retry action; successful inventory refresh may restore eligibility for a previously missing device, but must not repeatedly reopen a failing stream.

A same-rate device or buffer change preserves recorded material, transport and per-entity positions across the pause. Drain/discard old device-latency scratch data rather than feeding it into the new capture geometry. Do not advance the musical clock for the pause. Connected NINJAM may require reestablishing its mapping through existing ownership rules after restart; do not invent a second timing authority.

### MIDI controls and reconciliation

Show each discovered input as a checkbox, along with selected missing inputs. Checkbox changes become eligible on the next control turn and use the latest complete enabled set. They are independent of the audio recording deferral; endpoint removal must still terminate held notes/trigger actions safely through existing engine ownership.

- Report connected, disabled, missing, ambiguous, failed and capacity-exceeded status per input. Multiple inputs may be enabled within the existing bounded limit.
- Use backend stable identity where available. Numeric inventory IDs identify only the current inventory, not persistent hardware. Without stable identity, duplicate names are ambiguous after refresh/reordering: report them and do not guess. Disambiguated labels alone do not solve persistence identity.
- Preserve desired enabled flags for missing/failed ports so hotplug can retry. Successful endpoints remain usable when another endpoint fails. An inventory-wide failure leaves working endpoints and accepted preferences unchanged.
- Reuse unchanged endpoints where practical. Serialize opens/closes and refresh reconciliation on the existing MIDI job owner. Retire old snapshots/endpoints only after their callback readers and publications can no longer reference them; release per-source held state when disabling.
- Persist the accepted **desired enabled set**, including missing/failed selections, after reconciliation accepts the request. Connected state is diagnostic and need not equal desired state. This deliberately differs from audio, where only an actually started configuration is persisted.

## Thread ownership and lifecycle contract

Use existing command and snapshot mechanisms. One bounded user operation may be in flight across audio changes, MIDI changes, Load and Save. Latest device drafts are retained while busy; file actions are disabled while busy rather than queued. Automatic hotplug uses the same subsystem lifecycle serialization and is deferred where it conflicts with capture/replacement. Do not hold an application gate mutex while waiting for the job thread.

| State/work | Owner | Cross-thread rule | Teardown |
| --- | --- | --- | --- |
| Drafts, deadlines, displayed status, save folder | Main/UI orchestration | Commands carry complete values; UI receives coherent completion/status | Invalidate Scene identity, discard drafts on successful replacement |
| User-operation admission, request identity | Existing application/control owner | Serialize dispatch; pair increasing operation ID with Scene lifetime ID | Cancel unstarted requests; let admitted lifecycle work reach a safe boundary |
| Audio device and callback resources | AudioHost lifecycle owner | Existing admission/drain protocol; publish one complete active stream value | Backend stop confirmation before replacing device/userdata resources |
| MIDI endpoints and desired selection | Existing Scene job/MIDI owner | Existing immutable input snapshots and serialized publication | Stop ingress, retire readers, then reclaim |
| Mutable session material and plugin state | Existing engine/plugin owners | Quiescent capture with all writers excluded; plugin calls obey thread affinity | Snapshot owns copied content; no borrowed Scene pointers in disk work |
| Rig persistence | Existing accepted-rig coordinator/save path | Merge the relevant hardware fields into the latest accepted rig | Failed disk write retains prior persisted state |

For every introduced shared field, implementation must record owner, readers, writers, synchronization and teardown. Immutable snapshots protect lifetime, not mutable processor contents. Snapshot publisher store/exchange/collect remain off the callback and writer-serialized; check bounded borrow failures using the existing API.

### Audio change sequence

1. Capture the last known-good actual configuration and previous running/paused/stopped state. Validate exact requested device/rate/counts against a fresh inventory. Disable startup fallback behavior for explicit user requests; only buffer-size negotiation is permitted.
2. Atomically admit the change against capture-start actions using the existing control/audio handoff. If capture wins, defer. Once admitted, hold new capture actions until the change finishes; do not replay delayed trigger presses unexpectedly.
3. Close engine callback admission and drain admitted callbacks through AudioHost. Stop/close the backend before destroying or replacing callback userdata, driver/output geometry or device resources. If backend stop fails, retain resources it could still reference, fail the operation and report the actual state; do not attempt replacement on an uncertain backend lifetime.
4. Open the exact candidate. While quiescent, prepare mixer scratch, station buffers, plugin sample rate/block size, MIDI clock mapping and NINJAM audio format from driver-accepted parameters. Run plugin lifecycle calls on their required thread, without waits under locks they need.
5. Reopen admission/start in the order required by the existing AudioHost protocol, then publish the complete active configuration only on success. Callback work cannot observe mixed old/new geometry.
6. If preparation/open/start fails after replacing the old stream, restore the last known-good configuration and its engine/plugin geometry once. If restoration fails, close engine admission and show stopped with both errors. Never claim successful rollback based only on a requested reopen.
7. After success, merge actual hardware values into the accepted rig and save through the existing path. A disk failure leaves the working stream active, reports **applied, preferences not saved**, and offers persistence retry without reopening audio.

Reject sample-rate differences whenever any Station contains a take. Recheck immediately before transition. In a no-takes session, safely update existing plugins and reset/rederive rate-dependent empty transport, quantisation and MIDI timestamp state; coordinate connected NINJAM through its existing format/timing lifecycle. Do not retain stale sample-unit timing merely because no recorded buffers exist.

### Channel-route policy

Inspect existing mixer bounds checking and route export first. Preferred implementation: retain route indices in runtime, ignore out-of-range destinations safely, warn about unavailable channels, and omit those destinations from exported route arrays. Increasing counts restores retained runtime routes. Export an explicit empty array when all destinations are unavailable so legacy default routing is not restored accidentally.

Keep this change inside existing channel/mixer validation and route-export boundaries. If runtime preservation needs new route-state plumbing across subsystems, use the simpler fallback: remove invalid routes once, only after the new stream starts successfully, and warn that affected routes were disconnected. Never silently remap channels. Do not mutate routes during a candidate that may roll back. Record which implementation was chosen in the PR and test it. Neither option persists unavailable routes.

## File operations

### Save and Alt-click Save As

Use a folder picker, matching the existing export format. A successful Load initializes the save folder to the loaded manifest's parent directory; a new session has no folder. Ordinary Save chooses a folder only when none is set. Alt-click Save always opens the folder picker. Cancel retains the previous destination and performs no capture/write. Adopt a newly chosen folder only after successful publication.

Use `session-0001.jam`, `session-0002.jam`, etc. in the chosen folder, selecting one greater than the highest existing matching suffix; do not fill gaps or overwrite old saves. Four digits are minimum padding, not a maximum. Handle overflow/path errors explicitly. Do not add dirty tracking, autosave, pruning, or base-name editing in this feature.

Each version owns distinct, relative WAV/MIDI asset names prefixed with its version stem and a unique attempt token. Preserve stable engine entity IDs separately from asset names. Keep filenames compatible with the current loader (`Loop.Name` currently supplies the WAV path); avoid a schema change unless required by a demonstrated limitation.

Save sequence:

1. Admit the operation and resolve the folder before pausing. Retain previous stream state. Reject Save while capture/overdub/punch or a pending capture action is active, with a short message; playback may pause. This avoids committing an undefined half-recorded take. The automatic defer-until-capture-ends behavior applies to audio changes, not file commands.
2. Obtain explicit quiescence from AudioHost; a false `AudioDevice::Pause` is not sufficient evidence that audio is stopped. Exclude all job/UI/trigger/session writers using established ownership boundaries. Recheck capture eligibility there. If exclusion fails, abort without copying mutable state.
3. Copy one coherent owned export snapshot: audio samples, MIDI events/automation, routes, levels, phases/transport, quantisation, plugin state and trigger history. Move pre-lock session field reads into this capture boundary. Respect plugin thread affinity; every mutable snapshot field must have an identified writer exclusion mechanism. Portable exports remain local-only and exclude live NINJAM credentials/anchors, matching the existing exporter.
4. Release writer exclusion and restore exactly the previous stream state before serializing/writing. A failure to resume must be reported separately and leave an explicit stopped status. Never copy samples or serialize files inside the callback. A large copy can take noticeable time; show capturing/saving status rather than promise a fixed pause duration.
5. Write attempt-owned assets and a temporary manifest on the destination filesystem. Check write/flush/close results, validate serialized manifest and referenced assets, and publish all assets before the manifest. Allocation/disk failures leave the Scene usable and previous saves intact.
6. Publish the numbered manifest with a **no-overwrite** commit operation. If another process claimed that suffix, choose a new suffix and rebuild the manifest/asset association safely or fail with a retryable collision; never overwrite it. Atomicity applies to the manifest commit on a supported local filesystem, not a multi-file replacement transaction. Reject/report unsupported commit behavior rather than claiming guarantees.
7. Report the completed path and adopt the folder only after commit. Cleanup on failure touches only uncommitted files owned by this attempt. Once committed, its referenced assets must remain. File success and audio-resume success are separate outcomes.

Parameterize and reuse IoSessionExporter for both panel and Ctrl+S. Return structured success/cancel/error information rather than swallowing the export result. Do not leave an independent direct Ctrl+S path that bypasses operation admission or quiescence.

### Load

Panel Load and Ctrl+L feed the same application-loop request. Reject during active/pending capture with a concise message. Resolve the picker before audio suspension where the existing UI permits; cancellation must not alter content, hardware or save destination.

Construct the candidate using the latest accepted rig, retaining the outgoing Scene until parsing/construction succeeds. On failure, retain its content and restore its prior stream state; report restoration failure explicitly. Only then use existing shutdown/window replacement ownership to commit the candidate. Successful replacement discards old draft device requests, initializes the save folder, and invalidates old completion identities. If audio startup fails after replacement, retain the successfully loaded session and report stopped/retryable audio; do not claim the old session was retained after it was shut down. Preserve current loader semantics for tolerated missing/corrupt assets, but surface its warnings instead of reporting an unqualified complete load.

## Implementation steps and suggested subagents

Do not spawn all implementation agents at once. Establish shared contracts first, then assign disjoint files. Subagents must read repo guidance, cite files/lines in findings, avoid anonymous namespaces, and use the supplied local build commands. These are suggested assignments for the implementation run, not a request to create separate sidebar chats.

1. **Contracts and inspection — lead agent.** Trace current capture-state/admission, rig acceptance, route bounds, plugin affinity, MIDI teardown and exporter ownership. Define the minimal command/result types, Scene lifetime ID, latest-draft scheduling and quiescent export API. Resolve concrete route fallback based on code inspection. Record all new shared-field ownership. Deliver tests/acceptance cases and header contracts before parallel edits.
2. **Audio lifecycle — spawn `audio-lifecycle`.** Own AudioHost/AudioDevice changes, exact-selection open, capture-safe admission, rollback, negotiated values and format updates. Add focused lifecycle tests. Coordinate engine hierarchy changes with the lead; do not independently edit shared Scene wiring.
3. **MIDI reconciliation — spawn `midi-devices` alongside audio.** Own MidiDevice/MidiRouter reconciliation, identity/ambiguity, endpoint retirement, held-state release and desired-set status. Add focused reconciliation tests. Supply the lead an integration API; keep shared Scene edits with the lead.
4. **Export safety and versions — spawn `session-export` in the third child slot.** Own IoSessionExporter and exporter tests: owned capture contract, unique assets, numbered no-overwrite manifest commit, cleanup and structured results. Obtain quiescence through the agreed AudioHost API. Avoid shared Main/Scene edits; report required integration points.
5. **Panel and integration — lead agent while children work.** Own GuiMainPanel and shared Scene/Main/Window wiring, draft/debounce behavior, operation gate, folder picker/Alt-click semantics, status rendering and rig persistence integration. Merge each child change and inspect boundary behavior. Do not let independent branches implement different command contracts.
6. **Validation and focused review — after integration.** Reuse a finished agent for read-only threading/failure-path review. Run affected native tests/builds and manual hardware/file checks below. Correct findings before finalizing the feature. Parallel agents share the checkout: assign exclusive files and communicate before touching another owner's files.

## Acceptance and validation

- UI: immediate eligible device/rate/buffer edits, shared three-second channel debounce, no redundant reopen, latest draft after recording, new edits during a transition, explicit Retry without a retry storm, inventory failures retaining known choices.
- Audio: capture-start race resolves deterministically; recorded material/phase survives same-rate changes; no-takes rate guard is checked at admission; plugin formats and MIDI/NINJAM clocks use accepted geometry; exact selection never invokes startup fallback; stop/open/prepare/start/restore failures are distinguishable; failed rig persistence does not stop working audio.
- MIDI: multi-port enable/disable, partial open failure, missing-port persistence/reconnect, duplicate-name reordering, capacity limit, inventory-wide failure, and safe held-note/trigger cleanup. Stale refresh cannot undo a newer enabled set.
- Routes: reduction is safe and warns; rollback leaves routes untouched; exports omit unavailable destinations and preserve explicit disconnection; increasing counts restores routes only for the retained-runtime option.
- Files: picker cancellation, Save As destination adoption only on success, increasing versions across loads/restarts, collision handling, coherent audio/MIDI/plugin state, failed pause preventing mutable reads, pre-stopped audio remaining stopped, resume failure, asset/manifest failure preserving every previous version, and no stale completion reaching a replacement Scene. Test reopening a numbered save with all its assets.
- Use focused native tests for scheduling, lifecycle ordering, request identity, rig merge/persistence failure and export fault injection. Manually verify ASIO switching/negotiation, MIDI hotplug, plugin restart, NINJAM format transition, panel modifiers and numbered folder saves. Test large-session capture pause and disk-full/access-denied outcomes.
- Before **each** build/native-test run, read local `.vscode/tasks.json`; follow [build guidance](build.md). Use incremental affected-project builds. If no applicable local task exists, report the limitation rather than guessing tools.
- Run the threading-review hot-path audit and manually inspect changed callback-owned bodies. Device/filesystem I/O, allocation, logging, formatting, lifecycle locks and waits stay off the callback. Existing bounded handoffs must remain bounded.

Log state transitions and failures with Scene/operation identity, requested/actual hardware values, negotiation, recovery and persistence outcome, or file path/commit stage. Show the latest actionable error in the panel; avoid logging every refresh or turning diagnostics into lifecycle authority.
