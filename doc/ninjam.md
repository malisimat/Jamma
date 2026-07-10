# Ninjam Integration Guide

Jamma uses vendored Ninjam client files for networked collaborative jamming.

## Timing and Sync

NINJAM organises collaborative sessions around a fixed **interval** (BPM × BPI beats). Every
participant receives the same interval length and position from the server.

### Transport model

Jamma promotes the NINJAM interval to an authoritative external transport while connected, via
the `timing::ExternalTransport` layer (see `JammaLib/src/timing/ExternalTransport.h`):

- **Disconnected** — the local `Timer` free-runs as normal; no sync.
- **Connected** — on every job-thread tick, `NinjamNetworkService::_FeedExternalTransport`
  ingests the current remote `IntervalPositionSamps` and `IntervalLengthSamps`.

### Wrap-gated phase discipline

Drift correction is applied **once per remote interval wrap** rather than every tick, to avoid
continuous micro-nudges during playback:

1. `ExternalTransport::IngestSnapshot` detects a wrap (position rolls back to near zero).
2. `TimingQuantiser::DisciplineRemotePhase` seeds `Timer::SetMasterLoopIndexFrac` with the
   authoritative remote phase.
3. All local `LoopTake` play positions are re-derived from their stored **master-relative anchor**
   via `ExternalTransport::TakePositionFromAnchor`, avoiding a snap-to-zero reset.

### MIDI and automation coherence

MIDI note playback is cursor-driven (`_midiVisualPlayIndex`); it is re-anchored alongside
audio loops at each wrap. Automation playback and CC recording derive a fractional loop
position from a **frozen phase anchor** (`MidiLoop::_loopPhaseAnchor`, set once at
`EndRecord`) plus a live **transport correction** (`LoopTake::_midiAnchorCorrection`):

```
effectiveAnchor = loopPhaseAnchor + midiAnchorCorrection
frac = (globalSample - effectiveAnchor) % loopLength / loopLength
```

`_midiAnchorCorrection` is an `atomic<int32_t>` on `LoopTake`, written on the job thread by
`RepositionFromAnchor` and read on the audio thread by `Station::_RunAutomationDispatch` via
a baked pointer in `AutomationDispatch`. No dispatch rebuild is required at wrap time, and
`MidiLoop` itself remains a pure recording container.

### Export-lane latency compensation (send path)

The above covers the **receive side** (aligning local playback to the remote NINJAM
interval). The **send side** — the DAC/ADC content `NinjamConnection::ProcessExportBlock`
packs into local NINJAM channels — has its own latency-alignment design; see
[ninjam-live-loop-latency-sync-planC.md](ninjam-live-loop-latency-sync-planC.md) (the
current, implemented design; it supersedes the earlier
[plan](ninjam-live-loop-latency-sync-plan.md) and
[planB](ninjam-live-loop-latency-sync-planB.md)). Status: the delay-line compensation,
`ExportLaneTiming` helper, generation-reset/anomaly-detection safety valve, and VST
latency plumbing are implemented and unit-tested, but gated **off** by default
(`NinjamConnection::ExportLatencyCompensationEnabled`) until a physical DAC-to-ADC
loopback session against a real/test NINJAM server validates it (planC §5/§10).

For detailed design rationale and implementation history see
[ninjam-sync-implementation-plan.md](ninjam-sync-implementation-plan.md) and
[ninjam-sync-handoff.md](ninjam-sync-handoff.md).

## Client Files Structure

Files are located under the `lib/` directory:

- Header include root: `lib\njclient\njclient.h`
- x64 Debug lib: `lib\njclient\x64\Debug\MD\njclient.lib`
- x64 Release lib: `lib\njclient\x64\Release\MD\njclient.lib`

Note: A local `Directory.Build.local.props` is no longer needed for Ninjam paths.

## Refreshing Vendored Ninjam Files (Only When Updating Them)

You only need this step when you intentionally update the vendored Ninjam artifacts.

1. Build Ninjam in the other repo for `x64` `Debug` and `Release` (`MD` runtime).
2. Copy updated headers/libs into this repo:

```powershell
$ninjam = "C:\Users\<you>\Source\Repos\NinjamLib\ninjam"

Copy-Item "$ninjam\ninjam\njclient.h" ".\lib\njclient\njclient.h" -Force

Copy-Item "$ninjam\bin\x64\Debug\MD\njclient.lib" ".\lib\njclient\x64\Debug\MD\njclient.lib" -Force
Copy-Item "$ninjam\bin\x64\Debug\MD\njclient.pdb" ".\lib\njclient\x64\Debug\MD\njclient.pdb" -Force
Copy-Item "$ninjam\bin\x64\Debug\MD\njclient.idb" ".\lib\njclient\x64\Debug\MD\njclient.idb" -Force
Copy-Item "$ninjam\bin\x64\Release\MD\njclient.lib" ".\lib\njclient\x64\Release\MD\njclient.lib" -Force
```

## Troubleshooting: Ninjam Include and Link Errors

If you see errors like:

- `Cannot open include file: 'njclient.h'`
- Linker errors for `njclient.lib`

Verify the vendored files above exist in `lib\njclient`.

### Required Ninjam Link Dependencies

To link successfully, the compilation requires:
- `njclient.lib` from the appropriate `lib\njclient\x64` configuration folder.
- `ogg.lib`, `vorbis.lib`, `vorbisenc.lib`, `vorbisfile.lib` (provided via `vcpkg`).
- `ws2_32.lib` (system library from Windows SDK).
