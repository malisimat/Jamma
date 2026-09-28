# First-run setup and support

Jamma stores startup configuration in `%APPDATA%\Jamma\defaults.json`. On a fresh launch it inventories ASIO output and MIDI input devices, starts an audio stream, and creates a rig and jam in that directory. A generated rig is saved only after audio starts. The generated jam contains an empty Station. If audio cannot start, Jamma leaves the rig in memory for diagnosis and does not publish defaults that point to a missing rig.

See the [interactive startup flow](onboarding-flow.html) for the file and device decisions.

Set `JAMMA_DEFAULTS_PATH` before launching Jamma to choose a different `defaults.json`. Use a full path, such as `D:\JammaData\defaults.json`. Generated rig, jam, and startup log files go in that defaults file's directory. A relative override is resolved from Jamma's working directory.

## Files and fields

- `defaults.json`: `rig` and `jam` are paths to the selected files. `rigorigin` and `jamorigin` identify generated files. `rigload` and `jamload` are retained legacy fields; startup uses the file paths, not those values, to choose files.
- `.rig`: `user.audio.name` requests an ASIO device; `samplerate`, `bufsize`, `numbuffers`, `numchannelsin`, and `numchannelsout` request stream settings. The started values can differ and are shown in `startup.log`. `user.midi.devices` lists named MIDI inputs with `enabled` flags. `triggers[].stationtarget` names a Station in the jam. `triggers[].input` uses zero-based physical ADC channel indices, so `[0,1]` is inputs 1 and 2. `midiinputmode` and `midiinputdevices` control loop MIDI recording separately from Trigger bindings. See [MIDI trigger mapping](midi-trigger-mapping.md).
- `.jam`: `stations` contains the session's Stations, takes, and loop state. A generated first-run jam is empty apart from its Station. A selected jam is loaded without changing its file at startup.

To select another audio device, close Jamma, copy your `.rig` as a backup, then edit `user.audio.name` to the exact ASIO name shown in `startup.log`. Adjust channel counts if needed. To select MIDI inputs, edit `user.midi.devices` using the exact port names in the log. Restart Jamma and check the `ASIO` and `MIDI` attempt and outcome lines. A requested device can fall back at runtime; that does not rewrite a selected rig during startup.

## Startup log

Send `startup.log` from the defaults directory when asking for support. It is UTF-8 text with `BOOT`, `ASIO`, `MIDI`, `RIG`, and `JAM` lines covering paths, device inventory, attempts, outcomes, and a final summary. Jamma also prints those lines in its console. Each launch rotates the previous log to `startup.log.1`, keeping up to three earlier logs; each log is capped at 1 MiB. The startup log does not include raw jam JSON, credentials, or MIDI packets. Inspect the paths and device names before sharing if they are sensitive to you.

If an existing `defaults.json` is invalid or unreadable, Jamma leaves it untouched. After a successful recovery it writes `defaults-recovered.json` (or a numbered free name) beside it. The log gives the exact recovery path. Set `JAMMA_DEFAULTS_PATH` to that path on the next launch to use the recovered setup, or inspect and repair the original file yourself.

## Deliberate regeneration

Close Jamma and copy `defaults.json`, the selected `.rig`, and the selected `.jam` to a backup location. To regenerate only the rig, edit the `rig` path in `defaults.json` to a new, nonexistent path in the same data directory. To regenerate only the jam, do the equivalent for `jam`. On the next launch Jamma retains the old file, builds the missing counterpart, writes it under a free generated name, and updates `defaults.json` after validation. For a complete fresh configuration, move `defaults.json` aside and restart; keep the old rig and jam files in place. Jamma chooses free generated filenames and does not overwrite those old files. If audio cannot start, resolve the audio issue and launch again; the new rig is not committed until audio starts.
