# Ninjam Integration: Architecture Comparison & Consolidation

This document compares the two prototype branches and explains the design decisions made in the consolidated PR.

## Branches Compared

| Branch | AI Model | Description |
|--------|----------|-------------|
| `feature/ninjam-integration` | GPT 5.4 | Full architecture with lazy init, snapshot-driven reconciliation |
| `feature/ninjam-integration-gem` | Gemini 3.1 Pro | Leaner footprint, simpler connectivity model |

---

## Detailed Comparison Table

| Aspect | GPT 5.4 | Gemini 3.1 Pro | Consolidated PR |
|--------|---------|----------------|-----------------|
| **Namespace** | `io::NinjamConnection` | `jamma::io::NinjamConnection` | `io::NinjamConnection` (consistent with codebase) |
| **NinjamConnection lifetime** | Lazy — created only when `JamFile.Ninjam` config is present | Eager — unconditionally created in `Scene` constructor | ✅ Lazy (GPT) |
| **Connection params** | Constructor takes host/user/pass/workDir | Separate `Connect(host, user, pass)` call | ✅ Constructor (GPT) — params bound at creation, connect is a separate action |
| **Scratch buffers** | `std::vector<std::vector<float>>` (RAII) | `float**` raw pointer array (manual `new`/`delete`) | ✅ RAII vectors (GPT) |
| **Work directory** | C++ `std::filesystem` — portable, RAII | `GetTempPath` + `CreateDirectoryA` — Windows-only Win32 | ✅ `std::filesystem` (GPT) |
| **LoopRemote base class** | Extends `Loop` — correct abstraction, actual audio buffering | Extends `LoopTake` — wrong level, no audio buffering | ✅ Loop-based (GPT) |
| **LoopRemote audio** | Writes decoded samples into `BufferBank`; played back through standard `Loop::ReadBlock` | Visual envelope only; no ring-buffer playback | ✅ Full playback buffering (GPT) |
| **StationRemote wiring** | Owns a `LoopTake` + two `LoopRemote` (L/R) loops | Thin subclass, no LoopRemote wiring | ✅ Full LoopRemote wiring (GPT) |
| **Remote station tracking** | `_remoteStations: unordered_map<string, StationRemote>` in Scene, reconciled per-tick | Remote stations found by scanning `_stations` with `dynamic_cast` | ✅ Dedicated map (GPT) |
| **`IsRemote()` check in audio callback** | `IsRemoteStation()` helper using `dynamic_pointer_cast` — not real-time ideal | `dynamic_cast<StationRemote*>` inline in hot path | ✅ `virtual bool IsRemote()` on `Station` (consolidated fix) |
| **User reconciliation** | `_ReconcileRemoteStations(snapshot)` — creates/removes `StationRemote` per user diff | No reconciliation; remote user tracking is implicit | ✅ Explicit reconciliation (GPT) |
| **Ninjam interval tracking** | `NinjamRemoteSnapshot.IntervalPositionSamps / IntervalLengthSamps` | Not tracked | ✅ Full interval tracking (GPT) |
| **User channel management** | `_AssignOutputChannel()` — stable per-user stereo pair assignment persisted across ticks | Channel assigned by user-name lookup per block | ✅ Stable assignment (GPT) |
| **Output channel routing** | `ConsumeStereoPair(outChannelLeft, ...)` indexed by assigned output | `ConsumeRemoteMixForUser(userName, ...)` indexed by name | ✅ Index-based (GPT) — more cache-friendly in hot path |
| **`_ConfigureLocalChannels()`** | Announces local input channels to server | Not present | ✅ Present (GPT) |
| **`JamFile.NinjamConfig`** | Full struct + `FromJson()` + `ToStream()` + `DefaultJson` updated | Not added | ✅ Full round-trip (GPT) |
| **`JamFile.ToStream()`** | Rewritten to emit valid JSON | Rewritten to emit valid JSON | ✅ Both identical — taken as-is |
| **Loop.ExportSamples/ToJamFile** | Added | Added | ✅ Both identical — taken as-is |
| **LoopTake.GetLoops()** | Added | Added | ✅ Both identical — taken as-is |
| **Station.GetLoopTakes()** | Added | Added | ✅ Both identical — taken as-is |
| **PathUtils.PickDirectory()** | Added (IFileOpenDialog, COM) | Added (IFileOpenDialog, COM) | ✅ Both identical — taken as-is |
| **Ctrl+S export** | Added | Added | ✅ Both identical — taken as-is (skips remote stations) |
| **Tests** | `StationRemote_Tests.cpp` (3 tests incl. `IsRemote()`) | None | ✅ Tests from GPT + new `IsRemote()` test |
| **Project files** | `NinjamRoot` / `NinjamIncludeDir` UserMacros, `_CRT_SECURE_NO_WARNINGS` | Hardcoded relative paths, `/FS` flag | ✅ UserMacros (GPT) — overridable without editing project files |
| **XML BOM** | Preserved | Removed | ✅ Preserved (GPT) |

---

## Key Strengths and Drawbacks

### GPT 5.4 — `feature/ninjam-integration`

**Strengths:**
- Full round-trip JamFile serialisation with `NinjamConfig`
- Lazy connection initialisation from config
- RAII scratch buffers — no memory leaks
- `_ReconcileRemoteStations` — dynamic user join/leave handled correctly
- `LoopRemote` extends `Loop` at the right level — audio actually buffered for playback/visualisation
- Stable output-channel assignment per user — no reordering between ticks
- `std::filesystem` for work directory — portable across Windows SDK versions
- Tests covering the main path

**Drawbacks:**
- `IsRemoteStation()` uses `dynamic_pointer_cast` inside the audio callback — latency risk on some runtimes
- Remote stations exist in both `_stations` *and* `_remoteStations` — some redundancy

### Gemini 3.1 Pro — `feature/ninjam-integration-gem`

**Strengths:**
- Simpler overall footprint — fewer new types introduced
- `Connect(host,user,pass)` signature is arguably more explicit for runtime re-connection

**Drawbacks:**
- `jamma::io` namespace inconsistent with the whole codebase (`io::`)
- Unconditional connection object creation — wastes resources when ninjam is unused
- `float**` manual buffer management — leak risk
- `GetTempPath` / `CreateDirectoryA` — Win32-only, fragile
- `LoopRemote` extends `LoopTake` — wrong abstraction; no actual audio buffering
- No `JamFile.NinjamConfig` — server config lost on save/load cycle
- No user reconciliation — can't track join/leave events
- No tests

---

## Consolidated Design Decisions

### Primary base: GPT architecture

The GPT branch is taken as the primary foundation because of its correct abstraction levels, RAII discipline, and complete round-trip serialisation.

### Key improvement over GPT: `virtual bool IsRemote()`

Both branches use a cast to skip remote stations in the audio callback recording/monitor path. The consolidated PR replaces the `IsRemoteStation(dynamic_pointer_cast)` helper with:

```cpp
// Station.h
virtual bool IsRemote() const noexcept { return false; }

// StationRemote.h
virtual bool IsRemote() const noexcept override { return true; }
```

This is a **zero-allocation, no-exception virtual dispatch** in the hot path — the correct real-time audio idiom for type-based dispatch.

### Remote stations in `_stations` vs `_remoteStations`

Remote stations are kept in **both** containers:
- `_stations` — for drawing, selection, and the GUI hierarchy
- `_remoteStations` — for O(1) lookup by user name, and for audio ingestion without scanning `_stations`

The audio path skips remote stations in the record/monitor loops (via `IsRemote()`), then explicitly iterates `_remoteStations` for the ninjam ingest path — avoiding a second full scan of `_stations`.

---

## Extension Points for Future Features

The design is structured to make the following features straightforward to add:

### 1. Listing users on current server (UI)
`Scene::_remoteStations` is a `unordered_map<string, shared_ptr<StationRemote>>` populated by `_ReconcileRemoteStations`. A UI component only needs to read this map (on the job/UI thread) to enumerate connected users and their channel counts.

```cpp
// Future: expose a snapshot of connected users
std::vector<std::string> Scene::GetRemoteUserNames() const;
```

### 2. Creating associated `StationRemote` per user
Already implemented: `_ReconcileRemoteStations` creates a `StationRemote` for each user in the `NinjamRemoteSnapshot` and removes stale ones. New users automatically get a station when they join.

### 3. Per-user mute/solo/volume control
`StationRemote` owns an `AudioMixer` (via the `Station` base). Adding `SetMixerLevel()` / `SetMuted()` calls on `StationRemote` will automatically wire through to the existing `AudioMixer` path.

### 4. Beat-synchronised looping
`NinjamRemoteSnapshot.IntervalLengthSamps` and `IntervalPositionSamps` are already tracked. `StationRemote::SetRemoteInterval(length, position)` propagates these to the `LoopRemote` instances, which store audio in a ring buffer aligned to the ninjam interval. A future `BeatSync` feature can consume this directly.

### 5. Local channel broadcasting
`NinjamConnection::_ConfigureLocalChannels()` is already called on connect and format change. A future "broadcast" feature adds local `Loop` audio into `inBuf` before calling `ProcessAudioBlock`, requiring only a mix step before the ninjam callback.
