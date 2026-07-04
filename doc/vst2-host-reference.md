# VST2 Host — Canonical Reference & Implementation Handoff

**Branch context:** This document was produced on `feature/midi-war` after successfully fixing Battery 4 MIDI.
**Purpose (dual use):**
1. A canonical reference for how Jamma must correctly host VST2 plugins.
2. A handoff instruction set for a fresh agent session on a clean branch to implement the proper, production-quality fix.

---

## Part A — Handoff Instructions (for fresh agent)

### Mission

Implement a clean, production-quality VST2 host in Jamma, incorporating all findings from the `feature/midi-war` investigation.  The previous branch proved exactly what was wrong and what works; this branch should implement it correctly from the start, without the investigation scaffolding.

### Source of truth

| File | Role |
|---|---|
| `JammaLib/src/vst/Vst2Plugin.cpp` | Main VST2 host implementation |
| `JammaLib/src/vst/Vst2Plugin.h` | Interface and private contract |
| `JammaLib/src/engine/Station.cpp` | Host integration — when/how VST2 is driven |
| `JammaLib/src/midi/MidiVstOutputSink.cpp` | MIDI delivery into the VST2 event queue |
| `AGENTS.md` | Repo coding policy (must follow) |
| `doc/realtime-audio.md` | Real-time safety rules |
| `doc/vst2-battery-midi-fix.html` | Investigation report with root cause and evidence |

### The root cause fix (must include in any clean implementation)

1. Store the host instance pointer in `AEffect::resvd2`, **not** `AEffect::user`.
   - `user` is reserved for plugin-internal use (NI Battery uses it for its own state).
   - JUCE uses `resvd2` for exactly this purpose.
2. The `resvd2` assignment must happen **after** all construction-time dispatcher calls (`effOpen`, `effSetSampleRate`, etc.), because those calls fire before the returned `AEffect*` reaches the host.
3. `HostCallback` must answer the critical bootstrap queries **statically** when `resvd2` is null (i.e., during `VSTPluginMain`):
   - `audioMasterVersion` → `kVstVersion`
   - `audioMasterGetSampleRate` → static fallback (e.g., 44100)
   - `audioMasterGetBlockSize` → static fallback (e.g., 512)
   - `audioMasterGetAutomationState` → 1 (read)
   - `audioMasterCanDo` → call `SupportsHostCanDo(ptr)`
   - Vendor/product/version strings → hard-coded

### What to clean up from `feature/midi-war` before merging

The investigation branch accumulated work that should be reviewed and either cleaned or kept with intention:

1. **Trace instrumentation volume** — The `JAMMA_VST2_TRACE` / `JAMMA_VST2_TRACE_FILE` env-var path logs every dispatcher call, MIDI event list, process block, and output peak.  It performs a stream flush on every log line.  Options:
   - Remove entirely if a separate production-diagnostics path exists.
   - Keep gated behind the env var but reduce flush frequency (batch or use `'\n'` not `std::endl`).
   - Recommended: keep the env-var gate but switch to `'\n'`, and strip the per-block/per-sample peak logging.

2. **`audioMasterGetCurrentProcessLevel` returns `0`** — Returning 0 ("not supported / unknown") was a safe fallback during investigation.  The correct production answer is:
   - `kVstProcessLevelRealtime` when called from the audio thread (`_audioThreadId`).
   - `kVstProcessLevelUser` when called from any other thread (UI / editor).
   - The per-thread-id approach previously in the codebase was correct in principle; re-introduce it.

3. **Bootstrap static sample rate and block size** — The static fallback values (44100, 512) are only ever returned during `VSTPluginMain`, before `resvd2` is set.  This is fine; document it clearly in a comment.

4. **`_clearfp()` before processReplacing** — This clears x87 FP exception flags.  It is a defensive measure (Battery can leave FP exceptions pending that would fire in subsequent Jamma code).  Keep it with a comment explaining why.

5. **`Jamma/src/Main.cpp`** — Confirm no investigation overrides remain (forced audio format, forced state, timing hacks).  The clean commit should have removed these, but verify by reading `Main.cpp`.

6. **Dead `_isLoaded` guard in HostCallback** — An earlier version short-circuited the callback with `if (!self->_isLoaded) return 0;` before the switch.  This was removed; ensure the replacement is clean and doesn't gate on `_isLoaded` for legitimate callbacks like `audioMasterAutomate`.

---

## Part B — Canonical VST2 Host Reference

This section is the definitive guide for how Jamma must implement VST2 hosting, based on the VST2.4 SDK, JUCE's reference implementation, and validated live evidence from this investigation.

---

### 1. Host Pointer Slot

**Rule:** Store the host instance pointer in `AEffect::resvd2`, never in `AEffect::user`.

```cpp
_effect->resvd2 = reinterpret_cast<VstIntPtr>(this);

// In HostCallback:
auto* self = (effect && effect->resvd2)
    ? reinterpret_cast<Vst2Plugin*>(effect->resvd2) : nullptr;
```

- `AEffect::user` is a general-purpose slot the plugin is allowed to use for its own state.  Many NI plugins (Battery, Kontakt) use it.
- `AEffect::resvd2` is documented as host-reserved but is de-facto used by all major reference hosts (JUCE, minihost) for exactly this purpose.
- The assignment must occur **after** the construction-time dispatches, because plugins fire callbacks during `VSTPluginMain` before the `AEffect*` is returned to the host.

---

### 2. Instantiation Sequence (Construction)

All of the following must run **on the UI thread** (the thread that will later host the editor).

```
LoadLibraryW(path)
→ mainProc = GetProcAddress("VSTPluginMain") || "main"
→ _effect = mainProc(HostCallback)         // plugin's own ctor; callbacks fire here
→ verify _effect->magic == kEffectMagic
→ effIdentify(0, 0, null, 0)               // optional; some hosts skip this
→ effSetSampleRate(0, 0, null, sampleRate) // tell plugin the initial rate
→ effSetBlockSize(0, blockSize, null, 0)   // tell plugin the initial block size
→ effOpen(0, 0, null, 0)                   // finish construction; editor machinery binds here
→ [query pin properties / canBeAutomated / name / category — optional metadata]
→ _effect->resvd2 = this                   // ONLY NOW set the host pointer
```

**Why this order matters:**
- `effOpen` is often where NI/JUCE plugins bind their internal thread ID, GUI dispatch, and state machine.  It must precede any audio configuration.
- `resvd2` is set after `effOpen` because `VSTPluginMain` and `effOpen` callbacks must receive static answers (see §3).

**Thread requirement:** JUCE, minihost, and NI all bind editor idle/repaint dispatch to the thread that called `VSTPluginMain`.  If construction runs on a job thread and `effEditOpen` runs on the UI thread, the editor will paint once then freeze.  **PreInit must run on the UI thread.**

---

### 3. Host Callback — Bootstrap Answers (Before resvd2 Is Set)

During `VSTPluginMain` and immediately after, `resvd2` is null.  The callback will be called with no valid `self`.  The following queries must be answered from static values:

| opcode | Return value | Notes |
|---|---|---|
| `audioMasterVersion` | `kVstVersion` (2400) | Identity |
| `audioMasterGetSampleRate` | static fallback (44100) | Real value will be re-queried after `effSetSampleRate` |
| `audioMasterGetBlockSize` | static fallback (512) | Same |
| `audioMasterGetAutomationState` | 1 (read) | Tells plugin automation is valid |
| `audioMasterCanDo` | `SupportsHostCanDo(ptr) ? 1 : 0` | See §7 |
| `audioMasterGetVendorString` | `"Jamma"` | |
| `audioMasterGetProductString` | `"Jamma"` | |
| `audioMasterGetVendorVersion` | `1000` | |
| all others | `0` | Safe default |

These static answers are deliberately simple.  The correct sample rate and block size are communicated via `effSetSampleRate` / `effSetBlockSize` before `effOpen`, so the static fallbacks are only ever seen by the plugin's internal bootstrap code.

---

### 4. Load Sequence (After Construction)

Called from the job thread (construction has already run on UI thread via PreInit).  This configures the plugin for the actual audio engine settings:

```
effSetSampleRate(0, 0, null, sampleRate)       // confirm real rate
effSetBlockSize(0, blockSize, null, 0)         // confirm real block size
effSetProgram(0, 0, null, 0)                   // select program 0
effGetPlugCategory(...)                        // optional metadata
effCanDo(0, 0, "receiveVstEvents", 0)          // probe MIDI capability
effCanDo(0, 0, "receiveVstMidiEvent", 0)       // probe MIDI capability
for each input:  effConnectInput(pin, 1, ...)  // mark pins connected
for each output: effConnectOutput(pin, 1, ...) // mark pins connected
effSetSpeakerArrangement(0, inputArr, outputArr, 0)   // channel layout
if effFlagsCanDoubleReplacing: effSetProcessPrecision(0, kVstProcessPrecision32, null, 0)
effMainsChanged(0, 1, null, 0)                 // mains on
effStartProcess(0, 0, null, 0)                 // begin processing
```

**Key timing rules:**
- `effSetSpeakerArrangement` must use `AEffect::numInputs` / `numOutputs` (the actual channel counts the plugin declared), not Jamma's own requested channel count.  Sending the wrong channel count here causes silence or crashes in channel-count-sensitive plugins.
- `effSetProcessPrecision` should only be called if `effFlagsCanDoubleReplacing` is set (i.e., the plugin supports it).  Most float-only plugins ignore it, but calling it unconditionally can confuse some.
- `effMainsChanged(1)` then `effStartProcess` is the VST2.4 resume sequence.  The reverse (`effStopProcess` → `effMainsChanged(0)`) is required on unload.
- Parameters and chunk state (`effSetChunk` / `setParameter`) should be applied **after** `effMainsChanged(1)` and before the first `processReplacing` call.  Some plugins (NI) reset their internal state on `effMainsChanged` and expect the host to restore state immediately after.

---

### 5. Audio Processing (per-block)

Every audio callback follows this exact order:

```
1. UpdateHostTime(state)          // update VstTimeInfo for this block
2. BeginMidiBlock(startSample, numSamples)   // reset MIDI event accumulator
3. [for each pending MIDI event: SendMidiEvent(event)]
4. ProcessBlock / ProcessBlockStereo / ProcessBlockMulti
     └─ CopyInputs
     └─ DispatchPendingMidiEvents()   // effProcessEvents — IMMEDIATELY before processReplacing
     └─ zero output buffers
     └─ _clearfp()                   // clear x87 FP exception flags (Battery leaves these set)
     └─ effect->processReplacing(effect, inputs, outputs, numSamples)
     └─ accumulate sample position
     └─ CopyOutputsToMixer
```

**Rules:**
- `effProcessEvents` must be called **immediately before** `processReplacing`.  Calling it earlier (e.g., at the start of the block) allows the plugin to process events in the wrong sample-accurate context.
- The MIDI event list must be **valid for the lifetime of processReplacing**.  The `VstEvents` struct and all `VstEvent*` pointers in it must not be freed or modified until `processReplacing` returns.  Use pre-allocated fixed-size storage (e.g., `VstMidiEvent[256]` as a member), not stack-allocated arrays.
- `_midiEventBlock.events[i]` must point to the actual `VstMidiEvent` objects, not to copies.  The plugin receives raw pointers.
- Zero the output buffers before `processReplacing`.  Plugins are not required to zero outputs if they have nothing to write (instrument plugins with no active voices may leave output undefined).
- `_clearfp()` clears x87 FP status flags.  Battery 4 leaves pending FP exceptions after some processing paths; without clearing them, Jamma's subsequent FP operations will signal unexpected exceptions.

---

### 6. MIDI Event Layout

Each MIDI event in the `VstEvents` / `VstMidiEvent` struct must be:

```cpp
VstMidiEvent ev = {};
ev.type        = kVstMidiType;           // 1
ev.byteSize    = sizeof(VstMidiEvent);   // 32
ev.deltaFrames = clamp(offset - blockStart, 0, numSamples - 1);
ev.flags       = 0;                     // NEVER set kVstMidiEventIsRealtime (NI rejects flagged events)
ev.midiData[0] = status;                // e.g. 0x90 note-on ch0
ev.midiData[1] = note;
ev.midiData[2] = velocity;
ev.midiData[3] = 0;
```

**Rules:**
- `flags = 0` always.  `kVstMidiEventIsRealtime` (0x1) causes NI/Battery to silently discard the event.
- `deltaFrames` must be in range `[0, numSamples-1]`.  Events outside the current block should be either deferred to the next block or clamped to frame 0.
- `byteSize = sizeof(VstMidiEvent)` (32 bytes).  Do not set this to any other value.
- `VstEvents::numEvents` must exactly equal the number of valid event pointers.  Any surplus pointer slots can remain unchanged (they are not accessed).
- `VstEvents::reserved` must be 0.
- The `VstEvents` struct must be passed as a `void*` via the `ptr` argument of `effProcessEvents` (opcode 25).

---

### 7. Host `canDo` Declarations

When the plugin calls `audioMasterCanDo(ptr)`, answer `1` for strings the host genuinely supports, `0` for anything else.  The strings Jamma supports:

```
supplyIdle           — host will call effIdle / effEditIdle
sendVstEvents        — host sends VstEvents to plugin
sendVstMidiEvent     — host sends MIDI events
sendVstTimeInfo      — host provides audioMasterGetTime
receiveVstEvents     — host receives VstEvents from plugin (output)
receiveVstMidiEvent  — host receives MIDI from plugin (output)
supportShell         — host can load shell plugins
sizeWindow           — host can resize the editor window
shellCategory        — host understands shell categories
```

**Do not claim:**
- `realtimeMidiFlag` — NI plugins interpret this as a signal to enable a strict MIDI mode that may reject events not delivered with the realtime flag set.  Since we never set `kVstMidiEventIsRealtime` (see §6), claiming this would cause all MIDI to be dropped.
- `openFileSelector`, `closeFileSelector` — only claim if the host implements the full file selector protocol.
- `reportConnectionChanges`, `acceptIOChanges` — only if the host handles dynamic bus reconfiguration.

---

### 8. Time Info (`audioMasterGetTime`)

When the plugin calls `audioMasterGetTime`, return a pointer to a `VstTimeInfo` struct valid for the current block.  The struct must be valid until the next `processReplacing` call returns.

Minimum fields Jamma populates:

```cpp
VstTimeInfo ti = {};
ti.samplePos          = currentSamplePosition;
ti.sampleRate         = sampleRate;
ti.tempo              = bpm;
ti.timeSigNumerator   = bpi;  // Jamma uses bpi as beats per measure numerator
ti.timeSigDenominator = 4;
ti.ppqPos             = (samplePos / sampleRate) * (tempo / 60.0);
ti.flags = kVstPpqPosValid | kVstTempoValid | kVstTimeSigValid;
if (isPlaying) ti.flags |= kVstTransportPlaying;
```

**Rules:**
- The pointer must remain valid until the end of the current `processReplacing` call.  Using a per-instance member (not a local) is correct.
- Do not set `kVstTransportChanged` on every block — only set it on the block where transport state actually changes.
- `kVstBarsValid` and `kVstCyclePosValid` are optional; only set them if you also populate `barStartPos` and `cycleStartPos` / `cycleEndPos`.
- `kVstNanosValid` is rarely needed; only set if `nanoSeconds` is accurate.

---

### 9. Current Process Level (`audioMasterGetCurrentProcessLevel`)

| Call origin | Return value |
|---|---|
| Audio thread (during `processReplacing`) | `kVstProcessLevelRealtime` (2) |
| UI / editor thread (outside process) | `kVstProcessLevelUser` (1) |
| Unknown | `0` (not supported) |

**Implementation:**

```cpp
case audioMasterGetCurrentProcessLevel:
{
    const DWORD audioTid = self->_audioThreadId.load(std::memory_order_relaxed);
    if (audioTid != 0 && GetCurrentThreadId() == audioTid)
        return kVstProcessLevelRealtime;
    return kVstProcessLevelUser;
}
```

Returning `kVstProcessLevelRealtime` from the UI thread causes editor repaint failures in plugins that gate their UI update path on being outside the realtime context.  The thread-id approach correctly distinguishes the two.

**Note:** During the `feature/midi-war` investigation this was temporarily simplified to return `0` for safety.  The production implementation should use the thread-id pattern above.

---

### 10. Latency / Channel Counts

After the plugin is loaded:
- `AEffect::initialDelay` — report this to the audio mixer as the plugin's algorithmic latency.  Jamma currently ignores this; it should be forwarded to the latency compensation system.
- `AEffect::numInputs` / `numOutputs` — use these for speaker arrangement and buffer allocation, not Jamma's requested channel count.
- `AEffect::numParams` — use for parameter iteration, not a hardcoded maximum.

**Do not** call `effGetNumMidiInputChannels` / `effGetNumMidiOutputChannels` for routing decisions.  These are informational only; the plugin decides internally which channels it acts on.  Always send events on channel 0 unless a user-configured override exists.

---

### 11. Editor Window Lifecycle

All editor calls must happen on the **UI/render thread** (same thread as `PreInit` / `VSTPluginMain`).

```
OpenEditor(parentHwnd):
  if !(effect->flags & effFlagsHasEditor) → return false
  GlContextScope scope           // snapshot + restore OpenGL context
  effEditOpen(0, 0, parentHwnd, 0)
  effEditGetRect(0, 0, &eRect, 0) → read editor dimensions
  store parentHwnd + set _isEditorOpen = true

IdleEditor():  (called every frame from UI/render thread)
  if editor open:
    GlContextScope scope
    effEditIdle(0, 0, null, 0)

CloseEditor():
  if editor open:
    GlContextScope scope
    effEditClose(0, 0, null, 0)
  set _isEditorOpen = false, clear parentHwnd

HostCallback audioMasterSizeWindow:
  PostMessage(parentHwnd, WM_SIZE_HINT, w, h)  // async resize from plugin; handle in UI msg pump
HostCallback audioMasterUpdateDisplay:
  PostMessage(parentHwnd, WM_REPAINT_HINT, 0, 0)
```

**Critical:** Battery 4 (and other NI plugins) make their own OpenGL context current during `effEditOpen`, `effEditIdle`, and occasionally `effEditClose`.  Without a `GlContextScope` guard that snapshots and restores Jamma's `wglGetCurrentContext()` / `wglGetCurrentDC()`, the Jamma framebuffer becomes incomplete and the entire UI paints white.

**`audioMasterSizeWindow` and `audioMasterUpdateDisplay`** must be dispatched from `HostCallback` (which may be called on the audio thread) to the UI thread via `PostMessage`.  Never call Win32 painting APIs directly from `HostCallback` — it can be called from any thread.

---

### 12. Destruction Sequence

```
CloseEditor()                        // effEditClose on UI thread
effStopProcess(0, 0, null, 0)        // stop processing
effMainsChanged(0, 0, null, 0)       // mains off
[clear buffer pointers before clearing storage]
effClose(0, 0, null, 0)              // plugin destructor
_effect = nullptr
FreeLibrary(_moduleHandle)           // unload DLL
_moduleHandle = nullptr
```

**Rules:**
- `effStopProcess` → `effMainsChanged(0)` must be symmetric with `effMainsChanged(1)` → `effStartProcess`.
- Clear `_inputChannelPtrs` / `_outputChannelPtrs` **before** clearing `_inputScratchStorage` / `_outputScratchStorage` so a concurrent `ProcessBlock` (guarded by `_isActivated`) cannot chase stale pointers into freed memory.
- `effClose` must be called on the same thread as `effOpen` (the UI thread in Jamma's design).  Doing it on a job thread when `effOpen` ran on the UI thread is undefined behaviour for many plugins.
- `FreeLibrary` must be called after `effClose` — the plugin's code must be resident until the destructor completes.

---

### 13. State Save / Restore

VST2 plugins support two state formats.  Check `effFlagsProgramChunks`:

```
if (effect->flags & effFlagsProgramChunks):
    GetState: effGetChunk(bank=0, size, &ptr) → store raw bytes
    SetState: effSetChunk(bank=0, size, ptr)
else:
    GetState: for each param: getParameter(i) → store as float array
    SetState: for each param: setParameter(i, value)
```

When restoring a chunk, call `effGetProgramNameIndexed` for each program after `effSetChunk` — JUCE does this and it appears to flush plugin state correctly in some NI plugins.

The host also handles FXB/FXP wrapped blobs (magic `CcnK`): detect the magic, extract the inner chunk, and pass only the raw chunk bytes to `effSetChunk`, not the FXB wrapper.

**Timing:** State restore must happen **after** `effMainsChanged(1)` (mains on) and **before** the first `processReplacing` call.

---

### 14. `audioMasterAutomate` — Parameter Automation

```cpp
case audioMasterAutomate:
    // Plugin has changed parameter 'index' to 'opt' value via its own UI.
    // Record it for MIDI automation binding on the UI thread.
    // Do NOT call setParameter here — plugin already changed its own state.
    PublishLastTouchedParameter(self, index, opt);
    return 0;
case audioMasterBeginEdit:
case audioMasterEndEdit:
    return 1; // acknowledge gesture bracket; do not publish value
```

`audioMasterAutomate` may be called from the audio thread (when the plugin internally modulates a parameter).  The publish must be lock-free and RT-safe.

---

### 15. `audioMasterGetDirectory`

Return the directory of the plugin's own DLL, not Jamma's executable directory:

```cpp
case audioMasterGetDirectory:
{
    static thread_local char dir[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(self->_moduleHandle, dir, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        // Truncate at last separator to get the directory.
        for (auto i = len; i > 0; --i)
            if (dir[i-1] == '\\' || dir[i-1] == '/') { dir[i-1] = '\0'; break; }
        return reinterpret_cast<VstIntPtr>(dir);
    }
    return 0;
}
```

Battery, Kontakt, and other NI plugins use this to find their own preset libraries, sampler databases, and resource DLLs.  Returning 0 or returning Jamma's directory causes them to silently fail to load presets.

---

### 16. Thread Safety Summary

| Operation | Thread |
|---|---|
| `PreInit` / `VSTPluginMain` / `effOpen` | UI thread (must be the same thread as `effEditOpen`) |
| `Load` / `Unload` / state save-restore | Any non-RT thread (not while ProcessBlock is live) |
| `effEditOpen` / `effEditClose` / `effEditIdle` | UI thread |
| `processReplacing` / `effProcessEvents` | Audio thread |
| `HostCallback` | Any thread (plugin may call from audio or UI) |
| `audioMasterSizeWindow` / `audioMasterUpdateDisplay` | Must PostMessage to UI; do not call Win32 paint directly |
| `audioMasterAutomate` | May be called from audio thread; publish must be RT-safe |
| `_audioThreadId` | Written at start of each ProcessBlock; read in HostCallback |

---

### 17. What Jamma Does Not Currently Implement (Known Gaps)

These are gaps that exist in the current implementation and should be addressed in a full production version:

| Gap | Impact | Priority |
|---|---|---|
| `AEffect::initialDelay` not forwarded to latency compensation | Latency-compensated plugins (reverbs, lookahead limiters) will be out of sync | High |
| `audioMasterGetCurrentProcessLevel` returning 0 instead of thread-aware value | Plugins that adapt UI behaviour based on process level may behave oddly | Medium |
| `kVstTransportChanged` not signalled on transport state changes | Some arpeggiators/sequencers won't reset correctly | Medium |
| No output MIDI bus (`audioMasterProcessEvents`) | Plugins that output MIDI (arpeggiators, MIDI effects) have no sink | Low (instrument use case only) |
| `effBeginLoadBank` / `effBeginLoadProgram` not called before `effSetChunk` | Some plugins expect these lifecycle calls before chunk restore | Low |
| Shell plugin support | Plugins that load via a shell (`effShellGetNextPlugin`) are not scanned | Low |
| Double-precision processing path (`processDoubleReplacing`) | Not implemented; single precision only | Low |

---

### 18. JUCE Reference Implementation

When in doubt, cross-reference JUCE's implementation at:

```
C:\Users\matto\Source\Repos\JuceDemo\modules\juce_audio_processors_headless\format_types\juce_VSTPluginFormatImpl.h
```

Key differences from JUCE to keep in mind:
- JUCE uses a message-loop-driven architecture; Jamma uses a render/job-thread model.  Some JUCE idle callbacks rely on the message loop that Jamma does not have in the same form.
- JUCE builds FXB wrappers for state; Jamma stores raw chunks.  Both pass the same bytes to the plugin.
- JUCE's `resvd2` usage is the pattern Jamma now follows.

---

## Appendix: Trace Instrumentation

The `JAMMA_VST2_TRACE` / `JAMMA_VST2_TRACE_FILE` env vars enable detailed tracing.  To use:

```powershell
$env:JAMMA_VST2_TRACE = '1'
$env:JAMMA_VST2_TRACE_FILE = 'C:\trace.log'
# Run Jamma
# Then parse:
rg "plugin='Battery 4' context=multi-after outputPeak=(?!0\.0+\b)" trace.log
rg "plugin='Battery 4' context=dispatch-before" trace.log | head -20
```

Trace output schema:
```
[JAMMA_VST2_TRACE] plugin='<name>' context=<ctx> dispatch opcode=<n>(<name>) index=<n> value=<n> ptr=<addr> opt=<f> result=<n>
[JAMMA_VST2_TRACE] plugin='<name>' context=<ctx> midiBlock=<addr> numEvents=<n> reserved=<n>
[JAMMA_VST2_TRACE] plugin='<name>' context=<ctx> eventIndex=<n> type=<n> deltaFrames=<n> flags=<n> data0=<n> ...
[JAMMA_VST2_TRACE] plugin='<name>' context=<ctx> outputPeak=<f> outputChannels=<n> numSamples=<n>
[JAMMA_VST2_TRACE] plugin='<name>' context=host-callback opcode=<n> ... [canDo='<str>']
```
