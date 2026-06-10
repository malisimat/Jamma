# VST2 Host Implementation Review & Fix Plan

Reference: `D:\Install\Audio\vst\vstsdk2.4\public.sdk\samples\vst2.x\minihost`

---

## Root cause of the Serum crash

The crash was **Serum internally faulting because its preset folder was missing**, not a sequencing bug in Jamma. Confirmed: the same crash occurs in Reaper until presets are installed. That said, the investigation exposed several real correctness issues described below.

---

## Issue 1 — `effEditOpen` / `effEditGetRect` ordering (REGRESSION — NEEDS REVERT)

**Reference behaviour (Win32, `minieditor.cpp` `WM_INITDIALOG`):**
```
1. effEditOpen(hwnd)        ← open the editor first
2. effEditGetRect(&eRect)   ← query size after it has been created
3. SetWindowPos(...)        ← resize the host window
```

**Current Jamma behaviour (after the attempted fix):**
```
1. effEditGetRect(&eRect)   ← queried before the editor exists
2. effEditOpen(hwnd)
```

The reference is unambiguous on Windows: get-rect is called **after** open so the plugin already has a window and can report the actual rendered size. The "fix" that swapped the order was incorrect and should be reverted.

**Fix:** Swap back to `effEditOpen` → `effEditGetRect`.

---

## Issue 2 — `audioMasterGetTime` returned 0 (FIXED in current code, but incomplete)

Before the recent fix, `HostCallback` returned `0` for every opcode except `audioMasterVersion`. Serum (and most synths) call `audioMasterGetTime` during `effEditOpen` to read tempo for LFO rate display. Returning `0` gave a null `VstTimeInfo*`, which the plugin then dereferenced.

The fix adds a valid `VstTimeInfo` struct. However, `_timeInfo.samplePosition` is currently always `0.0` — it should advance by `blockSize` each audio block so tempo-synced content stays in sync with the transport position.

**Fix:** In `ProcessBlock` / `ProcessBlockStereo` / `ProcessBlockMulti`, after `processReplacing`, increment `_timeInfo.samplePosition += numSamples`.  
Also add `kVstTransportPlaying` to `_timeInfo.flags` once transport is running.

---

## Issue 3 — `audioMasterGetTime` `value` (filter mask) ignored

The VST2 spec says the `value` parameter of `audioMasterGetTime` is a bitmask of `kVstTempoValid`, `kVstTimeSigValid`, etc. specifying which fields the plugin wants populated. Jamma populates them unconditionally, which is harmless but non-compliant. Not a crash risk.

---

## Issue 4 — `user` field is null during `mainProc` call

In `Load()`, `_effect->user = this` is set **after** `mainProc(HostCallback)` returns. Some plugins call back into `HostCallback` during their construction (e.g. `audioMasterGetTime`, `audioMasterVersion`). At that point `effect->user` is either null or uninitialised.

The reference `minihost.cpp` never sets `user` at all — it gets away with this only because its `HostCallback` is a free function that ignores it. Jamma's callback now correctly null-checks `effect->user` before casting, so this is safe, but it means `audioMasterGetTime` will return `0` during plugin construction.

**Fix (optional):** Use a thread-local bootstrap pointer set immediately before `mainProc` is called and cleared immediately after, so `HostCallback` can resolve `self` even before `_effect->user` is set.

---

## Issue 5 — Name query opcodes called before `effOpen`

**Reference sequence:**
```
mainProc(HostCallback)   → AEffect*
effOpen
effSetSampleRate
effSetBlockSize
(then query properties)
```

**Jamma sequence:**
```
mainProc(HostCallback)   → AEffect*
effGetEffectName         ← before effOpen
effGetVendorString       ← before effOpen
effOpen
effSetSampleRate
effSetBlockSize
```

The VST2 spec does not explicitly prohibit querying names before `effOpen`, and most plugins tolerate it, but the canonical order is `effOpen` first. Some instruments initialise their string tables inside `effOpen`.

**Fix:** Move name queries after `effOpen`.

---

## Issue 6 — Missing `effSetProgram 0` after `effOpen`

Many synths (including Serum) expect the host to explicitly select the initial program with `effSetProgram, 0` before activating. Without it, the plugin may be in an undefined program slot.

**Fix:** Add `_effect->dispatcher(_effect, effSetProgram, 0, 0, nullptr, 0.0f)` after `effSetBlockSize` and before `effMainsChanged`.

---

## Issue 7 — `effMainsChanged(resume)` before audio format is fully negotiated

The reference calls `effMainsChanged(0, 1)` (resume) after `effSetSampleRate` and `effSetBlockSize`. Jamma does the same. No issue here.

However, if `Load()` is called from a background job thread while the audio callback is already running, `_isActivated` is set to `true` at the end of `Load()` after all setup is complete — this is correct. ✓

---

## Summary of required actions

| # | Severity | Action |
|---|----------|--------|
| 1 | **High** — regression | Revert `effEditGetRect` swap: call `effEditOpen(hwnd)` first, then `effEditGetRect` |
| 2 | Medium | Advance `_timeInfo.samplePosition` each audio block; add `kVstTransportPlaying` when live |
| 4 | Low | Optional thread-local bootstrap pointer for callbacks during `mainProc` |
| 5 | Low | Move name queries after `effOpen` |
| 6 | Low | Call `effSetProgram, 0` after `effSetBlockSize` |

---

## Reference sequence (correct Windows order)

```
LoadLibraryW / VSTPluginMain(HostCallback)  → AEffect*
_effect->user = this
effOpen
effSetSampleRate(sampleRate)
effSetBlockSize(blockSize)
effSetProgram(0)
effGetEffectName / effGetVendorString        ← name query after open
effMainsChanged(resume)

--- editor open (UI thread) ---
effEditOpen(hwnd)
effEditGetRect(&eRect)                       ← size query after open
SetWindowPos(hwnd, ..., width, height, ...)

--- audio loop ---
BeginMidiBlock / SendMidiEvent / effProcessEvents
processReplacing
_timeInfo.samplePosition += blockSize        ← advance transport

--- editor close ---
effEditClose

--- teardown ---
effMainsChanged(suspend)
effClose
FreeLibrary
```
