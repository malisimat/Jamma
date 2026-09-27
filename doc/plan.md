# First-run audio and MIDI configuration plan

## Goal and scope

A fresh Windows installation should open with one usable Station and one Trigger, try the system's default ASIO device and every discovered MIDI input, and remember a configuration that actually connected. A user who points `defaults.json` at an existing valid rig or jam keeps those files. This is a startup and JSON-file workflow; it does not require a configuration GUI.

The two persistent concepts remain separate: a **rig** describes the machine, devices, Trigger bindings, and audio input selection; a **jam** describes the session and its Stations. In rig JSON, `triggers[0].input` holds zero-based ADC channel indices: `[0,1]` means physical inputs 1 and 2. A Trigger requests recording on a Station; it is not itself a Station.

## Current experience and gaps

| Area | What the code does now | Gap for a new user or remote support |
| --- | --- | --- |
| Defaults and files | `Main.cpp` creates `%APPDATA%\Jamma\defaults.json` (or uses `JAMMA_DEFAULTS_PATH`). `LoadRig` and `LoadJam` write `RigFile::DefaultJson` and `JamFile::DefaultJson` as soon as a referenced file cannot be read. The defaults serializer later writes the selected paths and window settings. | Files should be missing at first startup, and only then are they to be created. Existing files never to be overwritten. Files may be called "default" despite never having connected. Missing, unreadable, and explicitly selected paths are not distinguished. An unreadable existing file must not be replaced during recovery. |
| Built-in session | `JamFile::DefaultJson` has one `Station1` and no takes. `RigFile::DefaultJson` has one `Trig1`, keyboard pairs, and `input:[0,1]`. `EmptyJam()` clears takes and live session state. | The one-Station/one-Trigger shape is close, but its stereo connection can be unavailable and the fallback is not saved only after validation. The built-in rig's `name:"default"` and MIDI `name:"default"` are placeholders.  Keep the key bindings for first trigger. |
| ASIO | `AudioDevice::ResolveAsioDeviceCandidates` probes ASIO devices and orders a named device, the OS defaults, then others. `Open` clamps requested input/output counts to the device counts and chooses the supported sample rate nearest the requested rate. It logs each open attempt and the successful stream parameters. | It does not list every probed device's name, input/output channel counts, supported rates, or default status. A fresh rig requests only 2 in/2 out, so a larger device is never opened at full available channel count. The successful negotiated values do not flow back to the rig. The selected open may still fail when started. |
| MIDI | `MidiDevice::Open` lists ports for each configured device and reports success or failure. `MidiRouter::InitMidi` attempts enabled rig entries only, and only after audio starts. | A fresh rig enables one `"default"` entry, so it opens the first port only. Unconfigured ports are not connected. Missing configured names can silently select another port after a warning; several configured names can converge on one physical port. The routing layer treats configured names as available before proving that they opened. Failed MIDI device connections are still shown in the GuiHud, but should be removed if failed connect. |
| Diagnostics | A console TUI catches `cout`/`cerr`; `Main.cpp` prints paths, jam summary, and rig settings. Scene creation reports some unresolved targets. | There is no durable startup log to send for support, no single outcome summary, and no reliable report of discovered versus connected devices. Reported rig values can differ from actual stream values. Do not dump jam contents or credentials to the log. |
| Session save | `IoSessionExporter` serializes a jam for explicit export, with temporary-file publication. | Startup does not persist the generated working jam, and export is not a suitable way to overwrite a user's selected jam. |

Relevant code: `Jamma/src/Main.cpp`; `JammaLib/src/io/{InitFile,RigFile,JamFile,IoSessionExporter}.cpp`; `JammaLib/src/audio/{AudioDevice,AudioHost}.cpp`; `JammaLib/src/midi/{MidiDevice,MidiRouter}.cpp`; `JammaLib/src/engine/Scene.cpp`. The rig's MIDI JSON shape and routing semantics are documented in `doc/midi-trigger-mapping.md`.

## Startup contract

1. Read and parse `defaults.json`, recording whether it existed **before** any bootstrap write. Do not infer user intent merely from a path's presence: `InitFile::DefaultJson` itself inserts `rig` and `jam` paths, so a file created by the old bootstrap looks like a selected file. For new installs, use the pre-existing-file decision and explicit generated-file provenance (for example, a versioned `defaults.json` origin field) to distinguish generated paths from paths the user selected. For legacy installs without provenance, preserve any existing rig/jam; offer an explicit regenerate route rather than guessing whether the user edited them. Treat the parsed `rigload`/`jamload` fields carefully: they are serialized today but `Main.cpp` does not actually branch on them. Document or implement their meaning before relying on them. Use a single `%APPDATA%\Jamma` directory derived from the resolved defaults path only if that is the intended location; an override path should have an explicit, predictable generated-file directory.
2. Classify each referenced file independently as **valid selected**, **missing**, or **present but invalid/unreadable**. A parseable file is not necessarily usable: validate a rig's required device values and Trigger graph and validate that a jam can construct a Scene with at least one Station. Preserve a valid selected file byte for byte. Preserve an invalid existing file for diagnosis too; place recovery output at a new generated path, and report the original path and reason.
3. Inventory ASIO and MIDI on the UI/job thread before constructing the final rig. Keep a structured discovery result, not just console output. Inventory every ASIO device, including probe failures, and all MIDI input ports. Identify OS default ASIO input and output IDs separately. Do not promise a duplex stream if they are different drivers.
4. If a valid selected rig exists, use it as the requested configuration. Try its named audio device first and its enabled MIDI devices by exact name. Runtime fallback may keep the app usable, but never rewrite the selected rig; report every divergence. If no valid selected rig exists, build a generated candidate from real discovery and the hard-coded one-Trigger template.
5. Open and **start** the ASIO stream. Then attempt each desired MIDI connection independently. Establish the final result from actual opened/running endpoints. Construct and validate the one-Station/one-Trigger generated configuration against that result. If a selected jam is valid, load it; otherwise generate the one-Station empty jam.
6. Persist generated files only after the corresponding result is validated. Write a generated rig after audio is running and MIDI attempts have completed, including successful MIDI ports only. Write a generated jam after a Scene with its Station and Trigger has been constructed successfully. Update `defaults.json` to the generated paths only after the referenced files are safely published. A missing MIDI port is a warning, not a reason to lose a working audio rig. If audio fails, do not mark a rig as working; keep the app available for diagnostics if possible.  Any changes to rig should automatically get persisted on exiting the app, if they are valid and do not pose a risk during next load (the rig files seem to automatically get updated correctly already).
7. On subsequent launch, load those generated files directly. Do not keep generating or replacing them on every run. If hardware later changes, try the stored request and log the negotiated runtime result; provide a separate, explicit recovery path for generating a new rig so an existing working or user-edited file is never silently overwritten.

### Negotiation rules

- **ASIO order:** For an explicit rig, named device first, then the default candidate(s), then other probed ASIO devices as the current fallback does. For first run, start with the OS default output device when it supports a usable output stream; prefer the same driver's input channels for duplex capture. If that driver cannot run, try the default input device and then other probed devices in a stable order. Log the reason for each skipped or failed candidate.
- **Channels:** For a generated rig, request the candidate's reported maximum input and output channels, independently, then verify the stream actually opens and starts at those counts. This preserves access to all channels for later JSON/HUD routing. If a driver rejects its advertised maximum, try a deterministic descending set of channel counts and record every attempt; choose the highest combination that starts. Prefer a duplex stream with at least one output and at least two inputs when such a candidate exists. Do not claim stereo if only one or zero inputs can be opened. For an explicit rig, honor requested counts subject to physical limits; any clamp/fallback is a runtime warning, not an edit to that rig.
- **Sample rate and buffer:** Prefer the current default sample rate from `UserConfig` if supported; otherwise choose the nearest reported supported rate, with a deterministic tie rule. If a reported rate fails at open/start, try the other supported rates in deterministic proximity order. Record requested versus actual rate, buffer size, buffer count, and input/output latencies from `AudioStreamParams`. Avoid an unbounded Cartesian retry across device, rates, channels, and buffer sizes: cap attempts and state the order. Use the actual started sample rate for MIDI timestamp mapping; `MidiRouter::InitMidi` currently reads `cfg.Audio.SampleRate`, which can disagree with the stream.
- **MIDI:** On generated first run, attempt every enumerated MIDI input by exact port identity/name, once each. Keep each successful endpoint active independently. Persist `{ "name": actualPortName, "enabled": true }` for successful ports only. For a selected rig, attempt each enabled listed port and warn on missing or failed ports. Change `MidiDevice::Open` so configured names do not silently fall back or partially match during this path; retain any intentional legacy fallback only behind an explicit compatibility decision. Port indices are diagnostic because enumeration order can change; persisted names are the present schema. If duplicate names exist, report ambiguity and avoid binding the wrong port silently (but do connect to first if specified to).
- **MIDI routing:** Opening a MIDI port is distinct from arming it to record loops or assigning it as a Trigger binding. A generated Trigger may list opened ports in `midiinputdevices` with `midiinputmode:"selected"` if the intended baseline includes loop MIDI recording; make this decision explicit and test it. Do **not** invent MIDI note/CC Trigger bindings for unknown controllers. Keep the hard-coded keyboard Trigger pair as the immediately usable activation path. Resolve warnings and HUD inventory against actually opened ports, not merely `enabled` rig entries.  If default trigger used (first startup) then this trigger should also be configured in the rig file (persisted) as to be activated by note 1 (and ditch by note 2) note-on event(s) for the first midi device successfully connected (channel 1).  No live MIDI is explicitly wired up to trigger as recording source by default.
- **Audio input routing:** For the generated Trigger, set `input:[0,1]` when at least two inputs were opened, `[0]` when one was opened, and `[]` when none were opened. Give it an explicit `stationtarget:"Station1"` and stable ID; avoid relying on positional migration. State in logs that `input:[0,1]` is physical inputs 1+2. Keep all other opened input channels available for later routing.

### File ownership and publication

| File state | Load behavior | Write behavior |
| --- | --- | --- |
| Valid path supplied in existing `defaults.json` | Load that exact rig/jam. Report runtime hardware mismatch. | Never replace that rig/jam during startup. Window-position updates to `defaults.json` must retain the same paths. |
| Fresh install with no pre-existing defaults or rig/jam files | Negotiate devices and construct the built-in one-Station/one-Trigger candidate. | Publish generated `default.rig` and `default.jam` (or clearly named generated equivalents), record their generated provenance, then point `defaults.json` at them. |
| Legacy defaults with no generated provenance | Load existing parseable files without assuming they are disposable bootstrap placeholders. | Never replace those files implicitly; use an explicit regeneration route to create new paths. |
| Referenced path missing | Log the missing path and use generated recovery values. | Write to a new safe generated path, without assuming the missing path was intended as a write target; update defaults only after success. |
| Existing rig/jam unreadable or semantically unusable | Log why, try an in-memory recovery candidate, and retain the original for support. | Do not truncate or replace the original. Publish recovery files under distinct names, then update defaults if successful. |
| Rig succeeds but jam fails, or vice versa | Handle each independently; keep the working selected file. | Publish only the missing/recovered counterpart. If Scene construction fails, do not point defaults at an untested generated jam. |

Use write-to-sibling-temp, flush, and atomic replace for generated files and defaults, with cleanup on failure. Never overwrite an existing file accidentally; reserve or choose a new generated path when necessary. If the file write fails, continue with the in-memory running configuration and give the user the exact path and error. Serialization should round-trip through `RigFile::FromStream` / `JamFile::FromStream` before publication. Keep bootstrap persistence off the audio callback. For a new jam with no recordings, serialize a minimal session rather than invoking full session export or creating audio sidecars.

### Concrete generated JSON shape

These examples show the required fields and relationships, not fixed hardware values. Use actual discovered names and negotiated numbers. The `input` array is zero-based; `allowedmidichannels` is one-based.

```json
// defaults.json (illustrative paths; remove comments in the file)
{"rig":"C:\\Users\\USER\\AppData\\Roaming\\Jamma\\default.rig","jam":"C:\\Users\\USER\\AppData\\Roaming\\Jamma\\default.jam","rigload":1,"jamload":1,"win":[0,0,1400,1000]}
```

```json
{
  "name": "First-run rig",
  "user": {
    "audio": {"name":"ACTUAL ASIO NAME","samplerate":44100,"bufsize":512,"numbuffers":4,"inlatency":256,"outlatency":256,"numchannelsin":8,"numchannelsout":8},
    "midi": {"devices":[{"name":"ACTUAL MIDI PORT","enabled":true}],"channelOverrideTriggers":false,"channelOverrideLive":true}
  },
  "triggers": [{"name":"Trig1","stationtype":0,"stationtarget":"Station1","pairs":[{"activatedown":49,"activateup":49,"ditchdown":50,"ditchup":50}],"input":[0,1],"midiinputmode":"none","midiinputdevices":[]}]
}
```

The implementation must populate the remaining rig user settings from `UserConfig` defaults or the parsed built-in template and use `RigFile::ToJsonStream`, rather than hand-building only the abbreviated example above. Persist a stable Trigger `id` in the real file. If no MIDI port connects, `devices` is empty. If only one audio input opens, use `[0]`.

```json
{"name":"First-run jam","stations":[{"name":"Station1","stationtype":0,"takes":[]}],"quantisesamps":1,"quantisation":"off"}
```

Generate the jam through `JamFile` serialization so current format/version and defaults stay consistent. No NINJAM credentials or connection should be invented in the generated jam.

## Required startup diagnostics

Write one persistent UTF-8 startup log under the Jamma data directory, and print the same concise status lines in the console. Include a timestamp, app version/build if available, defaults/rig/jam paths, file classification, and final outcome. Rotate or bound old logs. Do not log jam contents, NINJAM passwords, raw MIDI packets by default, or full user profile paths when a support bundle can use a redacted summary. Device names and exact JSON keys/values needed for support must remain visible.

Minimum events, each with a stable `[BOOT]`, `[ASIO]`, `[MIDI]`, `[RIG]`, or `[JAM]` prefix:

1. ASIO inventory: every ID, exact name, probe status/error, input and output channel counts, all reported sample rates, preferred rate, and whether it is default input/output. Include zero-device and enumeration-error cases.
2. ASIO attempts: requested device, direction/channel counts, rate, buffer size, rejection or exception text, and whether open and start succeeded. Final line includes actual stream values and a plain-language note such as `Inputs 1+2 -> Station1` or `No audio input connected; recording unavailable`.
3. MIDI inventory: every port index and exact name, including zero ports or enumeration failure. For each configured/generated port: attempted, connected, missing, disabled, ambiguous, or failed with reason. Report the number of active distinct endpoints.
4. Rig and jam decisions: `loaded existing`, `generated`, `recovery generated`, or `failed to persist`, with paths. Report Trigger name/ID, Station target, selected zero-based ADC indices plus their physical 1-based labels, selected loop MIDI ports, and any unresolved connection.
5. One final summary: audio running or unavailable, actual ASIO name/rate/channels, active MIDI names, Station count, Trigger count, and paths to the files and startup log. Warnings should say what still works and which JSON key/path to change for support.

The durable log should be started before defaults loading and should include all startup stages. The existing console TUI can display those lines, but console output alone is not a support artifact. Keep formatting, filesystem I/O, and device enumeration on non-real-time threads.

## Implementation order

1. **Tests and file decisions:** Add a small startup configuration coordinator with pure classification and decision functions in JammaLib `io`, then wire it from `Main.cpp`. Cover valid supplied files, missing files, invalid files, and independent rig/jam recovery. Verify that startup and subsequent shutdown do not change a valid selected rig or jam. Confirm the `rigload`/`jamload` policy or remove them from the decision path if they remain legacy.
2. **Discovery and audio negotiation:** Return structured ASIO inventory and attempt results from `AudioDevice`; keep the current fallback ordering for explicitly named rigs. Add bounded channel/rate retries for generated first-run rigs, an actual stream result after `Start`, and a clear failure status. Cover 0/1/2/many input channels, differing default input/output drivers, unsupported rates, advertised settings that fail at open/start, and no ASIO driver. Avoid callback-side logging or allocation.  Audio should attempt to connect with max number of reported channels, then reduce until we get down to a single stereo pair.  If that still fails then try next device (again at max reported channel count) - if different from default device - otherwise try next device until none left.
3. **MIDI connection result:** Enumerate once, open exact listed ports independently, report actual names/IDs and errors, and prevent duplicate physical-port connections. Feed the actual stream sample rate into MIDI timestamp mapping. Cover no ports, multiple ports, a missing configured port, duplicate names, and one failed port among successful ports.
4. **Generate and validate:** Build the generated rig from the successful stream and MIDI result, with one Trigger, explicit Station target, stable ID, keyboard pair, and available ADC inputs 1+2. Build one empty Station in the generated jam. Resolve routing against actual endpoints; round-trip both JSON forms; construct a Scene to prove the pairing works. Keep the generated files distinct from any valid supplied files.
5. **Publish and report:** Atomically publish only successful generated files, then defaults. Add a durable startup log and a concise final status. Document the data directory, `JAMMA_DEFAULTS_PATH`, the exact rig/jam fields, how to share the startup log, and how to select a different device by editing JSON. Provide a deliberate reset/regenerate procedure that preserves the old files.

## Acceptance checks

- Fresh launch with a multichannel duplex ASIO device and two MIDI ports starts audio at the highest working input/output counts, a supported sample rate, and two distinct MIDI connections; logs show inventory, attempts, and actual settings. The generated rig has one Trigger targeting the generated jam's one Station and routes inputs `[0,1]` for stereo recording.
- Restart without changing hardware loads the persisted generated rig and jam with no new file creation and no configuration drift.
- An existing valid rig and jam explicitly referenced by `defaults.json` remain byte-identical after startup and shutdown, even if hardware fallback is needed. Their runtime differences are warned about.
- With one input, the generated Trigger uses `[0]`; with no input, it uses `[]` and clearly warns that audio recording is unavailable. MIDI absence does not prevent audio startup.
- An invalid or unreadable existing rig or jam remains intact. Recovery uses new paths and logs the original failure. A failed write never leaves `defaults.json` pointing at a missing or partial file.
- The saved rig/jam parse and construct a Scene on the next run, and the startup log alone gives remote support the exact ASIO and MIDI names, available channel counts and rates, attempted values, successful values, routing, warnings, and file locations.
