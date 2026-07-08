# NINJAM Sync — Handoff for Next Session

Ref plan: `doc/ninjam-sync-implementation-plan.md`

## What is complete

### Phase 1 — done
- `timing::ExternalTransportSnapshot`, `ExternalTransportState`, `PendingJoinAlignment` POCOs.
- `ExternalTransport` class: wrap detection, double-buffered atomic `shared_ptr` publication
  (wrap-gated, lock-free readers), `BeginJoinAlignment`, `Connect`/`Disconnect` lifecycle,
  diagnostics toggle.
- All plan-specified state fields present.

### Phase 2 — items 1-4 and 8 done
- `ExternalTransport` wired into `NinjamNetworkService`:
  `PrepareTempoSyncOnConnect` → `Connect()`, `ResetTempoSyncOnDisconnect` → `Disconnect()`.
- `_FeedExternalTransport` called every job tick: builds `ExternalTransportSnapshot` from the
  live NINJAM snapshot + `Timer::AbsoluteSamplePos`, ingests it, records the one-shot join
  alignment, disciplines the clock at every wrap.
- `TimingQuantiser::DisciplineRemotePhase` + `RemotePhaseCorrectionOffset`: continuous
  wrap-gated phase correction via the existing `SetMasterLoopIndexFrac` mechanism.
  Tempo/BPI/SR changes auto-bypass via seeded-length guard.
- `LoopTake::AudioLoopsShareLength()` + debug-mode warning in `Play` (item 8).

### Phase 2 math (items 5+6) — APIs done, engine not wired
Three static helpers on `ExternalTransport` — all tested, ready to call:
```cpp
AbsoluteMasterSample(wrapCount, intervalLen, intervalPos)
TakeAnchorSample(absoluteMasterSample, takePlayPos, takeLen)
TakePositionFromAnchor(absoluteMasterSample, anchor, takeLen)
```
26 unit tests (ExternalTransport + ExternalTransportReanchor + RemotePhaseCorrection +
DisciplineRemotePhase) all green.

### Phase 3 — partially done
- Transport policy (disconnected=free-running, connected=always synced): enforced.
- Behavior that ignored phase when tempo unchanged: narrowed by `DisciplineRemotePhase`.
- Join/disconnect/reconnect/BPM+BPI+SR change transitions: handled.
- Runtime state not serialized to jam files.

---

## What is NOT done — the next session's job

### P2 item 6: Store a master-relative anchor on `LoopTake` and restore it at wrap

**Step 1 — add `_masterAnchorSample` to `LoopTake`.**

In `LoopTake.h` private section:
```cpp
// Master-relative anchor: the absolute master-timeline sample at which this
// take is at loop-relative position 0.  Set on Play, used to re-derive
// _playIndex at authoritative remote wrap without snapping to zero.
unsigned long _masterAnchorSample = 0ul;
```

**Step 2 — set the anchor in `LoopTake::Play`.**

`Play` already receives `index` (the audio `_playIndex` at play start) and `loopLength`.
After the loop-play call, compute the anchor using the master clock's absolute position:
```cpp
// In LoopTake::Play, after the loops->Play() loop:
if (auto clock = /* Scene-owned clock passed down, or read from Station */) {
    const auto absMaster = ExternalTransport::AbsoluteMasterSample(
        clock->LoopCount(), clock->SeedSourceLength(), clock->SampOffset());
    _masterAnchorSample = ExternalTransport::TakeAnchorSample(absMaster, index, loopLength);
}
```

The simplest seam: `Station` already holds `std::shared_ptr<utils::Timer> _clock`.
`LoopTake::Play` is called from `Station`, so the clock can be passed as an argument or
read from a new getter.  Match the existing pattern; `_clock` is the right source.

**Step 3 — add `RepositionFromAnchor` to `LoopTake`.**

```cpp
// Called from Scene/NinjamNetworkService at each authoritative remote wrap.
// Re-derives _playIndex for all loops from the stored anchor without snapping to zero.
void LoopTake::RepositionFromAnchor(unsigned long absoluteMasterSample);
```

Implementation:
```cpp
void LoopTake::RepositionFromAnchor(unsigned long absoluteMasterSample)
{
    const auto loopLength = VisualLoopLengthSamps();
    if (loopLength == 0ul || _masterAnchorSample == 0ul) return;
    const auto newPos = ExternalTransport::TakePositionFromAnchor(
        absoluteMasterSample, _masterAnchorSample, loopLength);
    for (auto& loop : _loops)
        if (loop) loop->SetPlayIndex(newPos);  // see Loop note below
}
```

**Loop::SetPlayIndex** — `Loop::_playIndex` is a public-facing atomic (already set directly
by `LoopRemote`). The pattern for push-style position updates is:
```cpp
_playIndex.store(constants::MaxLoopFadeSamps + newPos, std::memory_order_relaxed);
```
(The `MaxLoopFadeSamps` offset matches how `LoopRemote::SetMeasurePosition` does it.
Check `Loop::Play` to confirm the base offset used in local takes — it may be 0 for
locally-recorded loops. See `Loop.cpp:657`.)

### P2 item 5: Wire the reposition call into the wrap path

After `_FeedExternalTransport` detects a wrap in `NinjamNetworkService`:
```cpp
if (wrapAfter > wrapBefore)
{
    // Phase-discipline the clock.
    quantisation.DisciplineRemotePhase(snapshot.IntervalPositionSamps, intervalLen);

    // Re-anchor all takes from the new absolute master position.
    const auto state = _externalTransport.Published();
    const auto abs = ExternalTransport::AbsoluteMasterSample(*state);
    for (auto& station : stations)
        for (auto& take : station->GetLoopTakeSnapshot())
            take->RepositionFromAnchor(abs);
}
```

This requires passing `stations` into `_FeedExternalTransport`.  Its current signature:
```cpp
void _FeedExternalTransport(const NinjamRemoteSnapshot&, timing::TimingQuantiser&);
```
Add `const std::vector<std::shared_ptr<engine::Station>>&` as a third argument.
`HandleRemoteTempoSnapshot` already has stations; pass them through.

### P2 item 7: MIDI / automation / VST host-time reconciliation

Defer until P2 items 5+6 are verified against a live session.  The plan's intent:
- MIDI loop phase anchor (`_loopPhaseAnchor`) should be re-derived from `_masterAnchorSample`
  analogously to audio. `MidiLoop::EndRecord` already accepts an explicit `phaseAnchor`.
- VST host time (`hostTimeNanos`, `ppqPos`) should read from the authoritative master timeline.
- Automation phase reads use `LoopIndexFrac` which derives from `_playIndex`, so fixing
  the audio reposition will carry automation along.

### P3 item 3 (remaining): loop lifecycle while connected
- **Loop activation while connected**: `LoopTake::Play` will need to set `_masterAnchorSample`
  from the current master clock (step 2 above covers this).
- **Overdub while connected**: `_masterAnchorSample` should survive an overdub (it's the
  same take, same anchor).  Verify that overdub-end doesn't reset it.
- **Master-loop replacement while connected**: if the master loop changes length (tempo change),
  all anchors need recomputing from the new tempo epoch.  Tie this to
  `ApplyAcceptedRemoteTempo` — clear all `_masterAnchorSample` fields so the next
  `RepositionFromAnchor` re-derives from fresh phase.

### P3 item 5 (remaining): integration regression tests
Once engine wiring is in place:
- Harness test: create a Station + LoopTake, simulate a few intervals via
  `ExternalTransport::IngestSnapshot`, confirm `_playIndex` after wrap matches
  `TakePositionFromAnchor`.
- Test overdub-survives-wrap: anchor unchanged, position still correct.
- Test tempo-change clears anchors.

---

## Build / test baseline
```
MSBuild Jamma.sln /t:Build /p:Configuration=Debug /p:Platform=x64
.\test\JammaLib_Tests\bin\x64\Debug\JammaLib_Tests.exe
# 658 pass, 1 pre-existing unrelated failure (MidiRouterChannelOverride.DualHeldResetIsOrderIndependentAndSingleFire)
```

## Key files to edit next session
| File | What |
|------|------|
| `JammaLib/src/engine/LoopTake.h` | Add `_masterAnchorSample`, declare `RepositionFromAnchor` |
| `JammaLib/src/engine/LoopTake.cpp` | Implement `RepositionFromAnchor`; set anchor in `Play` |
| `JammaLib/src/engine/Loop.h` | Confirm / expose `SetPlayIndex` or document the direct-atomic pattern |
| `JammaLib/src/ninjam/NinjamNetworkService.h` | `_FeedExternalTransport` gets stations arg |
| `JammaLib/src/ninjam/NinjamNetworkService.cpp` | Wire `RepositionFromAnchor` call at wrap |
| `test/JammaLib_Tests/src/engine/` | Integration test for reposition round-trip |
