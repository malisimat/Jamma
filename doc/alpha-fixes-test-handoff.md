# Alpha fixes: ready for user testing

## Branch and launch

- Branch: `codex/alpha-fixes`, checked out at `C:\Users\matto\Source\Repos\Jamma.worktrees\Jamma-alpha-fixes`. Do not merge to master until the user's test feedback is addressed and the user explicitly asks.
- Accepted baseline: `602d3abf90b5f62013d1e2634c6342833a168842`. Final reviewed and fully tested source/test commit: `a20d52467824ce7d55fb5b77c471246c586dad16`. Later commits containing this handoff and progress ledger change documentation only. The final branch HEAD is reported with the handoff.
- Release executable: `C:\Users\matto\Source\Repos\Jamma.worktrees\Jamma-alpha-fixes\Jamma\bin\x64\Release\Jamma.exe`. Its `resources` directory is present alongside it. Debug executable: corresponding `Jamma\bin\x64\Debug\Jamma.exe`.
- For a separate test profile, set `JAMMA_DEFAULTS_PATH` to a new `defaults.json` path in a disposable directory before launch. On first run Jamma creates defaults, rig, JAM and startup.log there. The completed smoke used `C:\Users\matto\AppData\Local\Temp\Jamma-alpha-release-smoke-0b1143c53712451f800c2d4c368eb246` and left the live profile untouched.

## Included requests

| ID | Result |
| --- | --- |
| A01 | Selection, mute groups, cable reveal and tap tempo preserve the camera wander deadline; actual camera navigation still resets it. |
| A02 | `/`, `/help`, `/?` and console F1 show immediate offline help; `/n` and `/ninjam` retain server enumeration and refresh. Native console and performance-window F1 contexts are documented. Corrected the existing automation bracket shortcut comparisons to Windows virtual keys. |
| A03 | Settings width follows real content; controls wrap and tabs scroll in narrow viewports. |
| A04 | Front zoom fits larger and one-sided station layouts at wide and narrow aspect ratios. The unspecified optional secondary-view extension remains pending identification. |
| A05 | Bounded console capture, startup.log routing, backend diagnostics and retained worker-failure delivery cover the audited startup/runtime failure paths. See `doc/diagnostics.md` for precise sink and stream limits. |
| A07 | Double-clicking a gain fader returns it to exact unity through the normal smoothing/undo path. |
| A08 | One decorative disc or annulus per take marks its layer in take or loop mode without becoming an interactive picking target. |
| A09 | Visible gain faders use live, source-specific meters. MIDI-only take master controls note velocity into a shared station instrument, so it has no independently attributable audio meter. |
| A10 | Sixteen compact, mutually exclusive MIDI override toggles; all off means unchanged/pass-through. PageUp/PageDown, reset chord and held-note flushing remain connected to the existing router. |
| A11 | Windows VST3 outer bundle directories resolve to their x64 internal module at both load stages while retaining original saved paths and direct-file/VST2 behavior. |
| A12 | Waveform visual excursion is modestly increased (1.3× display upper target), preserving samples, sound and relative amplitude. |
| A13 | MIDI note volumes fit each take's separate band and recorded semitone range. A single pitch is centered and capped at `min(18 rendered take units, 25% of the usable band)`. During recording, new low/high notes refit existing notes and their spacing at the periodic geometry refresh. |

## Verification

- Every lane had independent Sol review. D-01/02/03, F-01/02, V-01 and H-01/02 were fixed and independently closed. A fresh review of the complete baseline-to-branch diff found no actionable P0–P3 product issues; it also closed the final one-line test fixture adjustment at `a20d5246`.
- After each merge, the affected integrated Debug targets built and focused native tests passed: diagnostics/VST3 **187/187**; settings/MIDI **71/71**; faders/meters **244/244**; take visuals/MIDI **533/533**; camera/input **64/64**; help/console **53/53**. These groups overlap and are not additive.
- Complete integrated Debug native suite at `a20d5246`: **1,763 passed, 1 intentionally skipped, 0 failed**, exit 0, 71.247 seconds. The skipped `RigFileMigrationValidation.StagedDirectoryRoundTrips` explicitly requires a staged migration validation request. The first full run exposed a stale test helper that focused the removed numeric MIDI channel control; its helper now focuses the remaining numeric offset input, the single test passed, and the complete suite passed on rerun.
- Incremental Release x64 JammaLib and Jamma builds passed, exit 0, with no compiler warnings/errors reported in their logs. Release resources copied beside Jamma.exe.
- Isolated Release startup created defaults/rig/JAM/startup.log, reported audio and MIDI device start, and completed initial jobs. A verified `WM_CLOSE` to that process's performance window exited cleanly with code 0. This establishes startup/shutdown, not listening quality, rendered responsiveness or plugin behavior.
- Retained validation logs and XML are under `%TEMP%\Jamma-alpha-integrated-*-focused.*`, `%TEMP%\Jamma-alpha-final-full-debug-fixed.*`, `%TEMP%\Jamma-alpha-final-Release-*.log`, and `%TEMP%\Jamma-alpha-help-evidence`. Shader-render previews are under `C:\Users\matto\.codex\visualizations\2026\10\16\01a1461b-9744-7141-9c28-3df34a1a0ed1\take-visuals`; they are synthetic preview evidence, not a live app test.

## Ordered manual test checklist

1. Launch the Release executable with a disposable profile. Confirm the console title points to `/help` or F1; `/`, `/help`, `/?` and console F1 show the same reference with no network. `/n` and `/ninjam` list/refresh servers; a listed number works with `/c` and `/connect`; disconnect aliases and ordinary chat still work. Test offline and unknown-command hints.
2. At normal and narrow window sizes, open every settings tab. Verify all labels/controls fit or scroll, all 16 MIDI override toggles can be selected, the active one can be cleared to unchanged, and PageUp/PageDown plus their reset chord update the display. Play held MIDI notes while switching channels; no stuck notes.
3. Use known distinct signals on station, take and loop racks. Check moving meters, mono/stereo/multichannel routing, mute, silence decay, rack rebuild and source replacement. Double-click each applicable gain fader to 0 dB, then undo. For a MIDI-only take, inspect the station instrument output meter instead of expecting a separate take audio signal.
4. Record mixed audio/MIDI takes, including two MIDI takes and then more. Check that all note volumes stay inside separate bands, a single-note loop remains modest in height, and notes already displayed change height/spacing as new lower and higher pitches arrive during recording. Edit/undo/delete/reload notes and test selection/picking from Front and angled views. Compare quiet/loud/asymmetric waveforms and take-core clearance.
5. Make a many-station session. Zoom out in Front at wide and narrow window sizes; verify framing, cursor-focused zoom and picking. Wait for camera wander, then change selection, mute groups, cable reveal and tap tempo without postponing it. Confirm actual navigation restarts the idle timer.
6. Load a known installed VST3 through its outer `.vst3` bundle, use editor/audio/MIDI, save/reload state and unload. Check an invalid or missing bundle, unavailable device and corrupt/missing session paths for contextual console diagnostics and startup.log where applicable. This also checks physical audio, device and plugin behavior unavailable to automated tests.
7. Save/reload a representative session and close normally. Confirm recorded media and selected settings restore, startup/shutdown are responsive, and no live user profile is changed when using the disposable override.

## Scope and limits

- Automated tests and the isolated hidden-window smoke cannot establish live rendered picking, audio quality, physical MIDI behavior, third-party plugin operation or responsiveness under a large performance session. Those are the manual checks above.
- P01 (expiring README screenshot) remains outside the confirmed alpha fixes scope. P02 stale latency documentation references were absent on the selected baseline. P03 full plugin latency compensation was not accepted into this small-fixes scope; no claim of compensation is made.
- The ambiguous A04 secondary-view extension is left pending user identification; Front zoom work is included and tested.