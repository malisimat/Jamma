# Rig cleanup integration delivery — 16 October 2026

## Delivered state

`feature/rig-model-cleanup` (`5d7c2859`) is merged through reviewed preparation
commit `85a1f4be` into `codex/integrate-ready-features`, integration merge
`09dbdc39`. All seven textual conflicts and the semantic overlaps were resolved.
Master remains `cff00825781eadb18fefa9504e0893f5d3b8b9ea`; nothing was pushed.
The source worktree was left untouched.

The native console, explicit rig save workflow, session/device coordination,
station membership and immutable publication behavior remain integrated.
Follow-up `323e4f0d` makes the migration regression harness return exit 0 after
all expected failure cases pass; its final 51-check run exited 0, and an
independent failure-path probe still exited 1.
See [semantic/threading review](rig-cleanup-review.md) and
[migration utility](../tools/rig-migrator/README.md).

## Profile checkpoint and conversion

Verified checkpoint:
`C:/Users/matto/AppData/Local/Jamma-rig-integration/20261016-171815-217/`.
`manifest.json` records original path, backup path, size and SHA256 for 398 files:
complete profiles, eight rig originals (including archival/suffixed copies),
referenced external VST DLLs, and matching pre-upgrade Debug/Release builds and
resources. Every backup hash was checked before publication. No externally
referenced rig was found. Embedded media remains inside its backed-up JAM.

All four active source rigs were staged separately, compared field by field
against their originals, accepted by the merged C++ parser, and transactionally
published into `C:/Users/matto/AppData/Roaming/Jamma/`:

| Installed file | Authority and purpose |
| --- | --- |
| `default.rig` | Canonical profile's converted curated rig; current startup selection |
| `default-1.rig` | Canonical profile's converted first-run variant |
| `default-from-rig-cleanup.rig` | Alternate profile's different device/settings variant, preserved separately |
| `default-1-from-rig-cleanup.rig` | Alternate profile's first-run variant, preserved separately |

The alternate directory remains intact. Its archival `default.rig` files and
atomic originals remain historical backups, not selected active rigs. The
canonical legacy `.rig.backup-*` original also remains intact. The migrator's
two unique `.rig.migration-backup-220b7c5ce3b6465181b4968090fadd71` files remain
beside the replaced live rigs. No live interrupted transaction journal remains.

Both profiles' five active JAM files were byte-identical, so no divergent session
needed copying. Existing JAMs, recordings, media, defaults selection, settings
and external assets were hash-checked unchanged at publication. Startup/close
also retained every active rig/JAM hash and profile selection/settings; normal
startup log rotation and defaults serialization are expected. Canonical paths
do not depend on the alternate profile.

## Validation and executable

Baseline: 1,687 Debug / 1,681 Release native tests passed. Final merged native
suites: 1,697 Debug / 1,691 Release passed, with one opt-in migration-validation
test skipped in each routine suite. That test passed separately for all four
active rigs and the converter's canonical, overlap, capture and input-only
fixtures. Eleven regression/validation tests were added; legacy schema assertions
were updated to canonical equivalents while runtime regression coverage was retained.
The recovered utility passed 51 isolated conversion/recovery checks and both
command wrappers' dry runs. Independent implementation and migration reviews
and the threading hot-path audit passed.

Incremental x64 Debug/Release engine, test and app builds passed before and after
the target-branch merge. Final executable hashes and full evidence are listed
in the checkpoint's `evidence/final-state.json`.

Runnable Debug app:
`C:/Users/matto/Source/Repos/Jamma.worktrees/Jamma-integrate-ready-features/Jamma/bin/x64/Debug/Jamma.exe`.
Release is at the corresponding `Release/Jamma.exe` path.

Canonical startup was smoke-checked with no `JAMMA_DEFAULTS_PATH` override. It
loaded `Roaming/Jamma/default.rig`, `session.jam`, five stations and five triggers;
Focusrite USB ASIO opened at 44,100 Hz, 8 in / 8 out, 1,024-sample buffer, and
Focusrite USB MIDI plus CASIO USB-MIDI connected. The observed driver latencies
were 2,108 samples each; the curated rig's saved request was retained. Shutdown
through the actual `JammaWindow` completed with exit 0. The passive console's
close action only hides that console. The app is stopped for hardware testing.
This establishes startup and automated behavior, not live performance acceptance.

Evidence under the checkpoint's `evidence/` includes final native XML/logs,
build logs, `canonical-startup.log`, `smoke-result.log`, `rig-mapping.json`,
`published-manifest.json`, staged/canonical/installed-byte copies, native
roundtrips, converter logs/fixtures and the threading audit. Earlier failed
fixture/stale-object runs are retained separately for traceability.
The evidence records retain their original worktree staging paths; the complete
directory was relocated into this checkpoint after verification.

## Rollback

Close Jamma first. Verify `manifest.json` backup hashes. Preserve any new work
before restoring only the affected canonical rig files from `profiles/Jamma/`
(and original defaults if selection was changed). Do not recursively replace
the live profile or erase new recordings. Retain the four upgraded rigs under
new names before a legacy rollback if they have since been edited.

Use the corresponding legacy executable from `legacy-build/Jamma/Debug/` or
`Release/` only with a matching legacy profile. For an isolated legacy launch,
copy the checkpoint profile into a separate writable directory and rebase that
copy's `defaults.json` rig/JAM paths into the copied directory before setting
`JAMMA_DEFAULTS_PATH`; the unmodified backed-up defaults contain original live
absolute paths. Never point a legacy executable at upgraded rigs. Keep the
verified checkpoint untouched. Converter recovery refuses independently changed
files and missing original backups; investigate rather than deleting its journal.

## Next hardware checks

1. Launch Debug normally and check the native console and current device choices.
2. Exercise MIDI/serial activate and ditch press/release, including distinct
   release bindings, overlap priority, repeated presses and held releases.
3. Record audio-only, MIDI-only and combined selected inputs; check routing,
   alignment and ditch history across trigger/station changes.
4. Disconnect/reconnect a MIDI device and test an unavailable serial/device or
   station target; verify saved selections survive and held notes settle.
5. Load another session, change device settings, explicitly save a new rig,
   close/reopen and confirm settings, membership and bindings return.
6. After those pass, test Release in a realistic performance session. Keep the
   checkpoint until hardware acceptance; consider promotion in a separate review.

## Final executable SHA256

- `C:\Users\matto\Source\Repos\Jamma.worktrees\Jamma-integrate-ready-features\Jamma\bin\x64\Debug\Jamma.exe`: `B2AABA6C0808533314EDDE0CE28FF0C7B9C7E23377606803F8E8B9E4BE5416E6`.
- `C:\Users\matto\Source\Repos\Jamma.worktrees\Jamma-integrate-ready-features\Jamma\bin\x64\Release\Jamma.exe`: `0F7816C0A2F2054DEF81C5136B62ED2DE0F7CBFA6E032AC97DC15BFDC8328316`.
