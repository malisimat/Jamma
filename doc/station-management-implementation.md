# Runtime station management

The implementation follows [station-management.md](station-management.md). New stations are blank, unbound, accept MIDI channel 1 explicitly, and keep that channel assignment when saved in a Jam.

## Responsibilities and thread ownership

| Owner | Responsibility and synchronization |
| --- | --- |
| `StationManagement` | Identity/revision validation, consent and transaction receipts. Scene serializes every access with `_sceneMutex`; the window creates requests and commits presentation, and the existing job worker advances receipts. The controller pins prepared/outgoing stations until UI retirement. |
| `RigCoordinator` | Candidate membership and remapped bindings. Its inventory writer runs under Scene's off-callback lock. Accepted snapshots retain frozen `BoundReceiver` identities; mutable route payloads are swapped only at the audio boundary and cleared after acknowledgement. `TopologyRevision` survives later rig-only edits. |
| `AudioHost` | One callback-boundary route application and coherent station inventory. A bounded station-read failure uses the retained rig's topology until an inventory carrying that topology revision is observed. Callback-only pointers are backed by retained leases and reset under the existing quiescent teardown barrier. |
| `Trigger` | Removed-receiver cancellation. Job publishes an atomic target identity only after input producers close; audio invalidates affected activation/delayed work, drains results and retires bounded history borrowers, then publishes readiness. Job releases removed history/session pins after that receipt. Surviving-receiver work is preserved. |
| `Station` | Atomic removal gate, capture cancellation and live-note cleanup. New job/UI actions and new MIDI ingress cannot reach an outgoing hierarchy. Scene retains the station until audio and input leases release it, then unloads processors and releases graphics on the window owner. Existing GUI ownership detachment breaks child cycles. |
| `GuiStationControls`, `GuiMainPanel` | Window/render-owned controls. Endpoint positions use camera projection without screen-edge clamping. Disabled rectangles still consume clicks. Ctrl/focus cancellation consumes unfinished bin releases. The last local bin is dimmed and disabled. The main panel owns the explicit-save button. |
| `RigDefaultSave`, Main | Main's window owner serializes dialogs and in-memory defaults; the existing Scene worker writes detached captured values. An atomic immutable result returns to Main. Shutdown joins/drains the worker before consuming its final result and writing window placement, so it retains the selected rig. |

The first-run generated rig is still published explicitly after runtime routing is accepted. The running Scene retains no disk-save callback. Runtime rig/membership edits accept without file writes or a writable profile.

## Saving and loading

“Save rig as default...” is on the main panel's Timing page. The native filename dialog starts in Roaming/Jamma, confirms overwrite and accepts `.rig` files in that directory. Traversal, device names, `defaults` and temporary-name collisions are rejected. `JAMMA_DEFAULTS_PATH` still selects the active defaults file.

The worker first atomically writes the full captured RigFile, then atomically writes defaults with the selected rig path and user origin/load policy. A failed rig write leaves defaults alone. A failed defaults write preserves the rig and reports “Rig saved; startup default unchanged”. Neither failure retries automatically.

A rig saves trigger/source/target configuration and user settings. A Jam saves station order/configuration, explicit MIDI channels, takes and loop material. To reproduce changed membership and bindings on reload, save the Jam and explicitly save the rig, then load both through the existing loader. Saving a rig alone does not change the startup Jam or embed station/performance data. There are no format changes or automatic profile/session pairing.

## Validation

Native regression coverage includes identity-preserving prepend, explicit unbinding, stale/replaced targets, take-based consent, final-station click consumption, Ctrl-release cancellation, queued/held recording cancellation, stopped-stream commits, surviving selection and retirement without another edit. Fault-injection tests cover rig/default write ordering and independent failures. The callback allocation probe covers membership publication and a later rig edit while station leases are exhausted.

The threading audit script flags the new off-callback Scene locks because it scans complete files, rather than callback bodies. Manual review of `Scene::OnTick`, `AudioHost::_OnAudio`, the Trigger cancellation boundary and the listed Station/LoopTake/Loop callback bodies confirms that no locks, waits, logging or allocations were added to those paths.

Live device/plugin-editor interaction and camera placement across view modes still require interactive testing with the target audio hardware. Incremental Debug x64 builds of JammaLib, Jamma and the native test target passed, as did all 1,416 native tests; no hardware/UI walkthrough is claimed.
