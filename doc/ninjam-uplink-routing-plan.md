# NINJAM Uplink Routing Fix Plan

## Scope

This plan covers two confirmed defects in Jamma's current NINJAM integration and folds in the follow-up requirement to optionally send live ADC input alongside routed station output.

1. Remote users hear only live ADC input, not post-router/post-station-master station audio.
2. The number of local NINJAM channels advertised to the server is derived from ADC input channels instead of the routed station export pairs that should be published.

The revised target behavior is hybrid:

- Published NINJAM channels are anchored to local DAC output pairs, which already carry post-router, post-station-master station audio.
- Live ADC input may also be published as its own NINJAM channel when the lane budget allows.
- When the lane budget does not allow separate ADC channels, ADC and DAC pairs are merged into a fixed number of lanes by modulo index.

## Confirmed Findings

### 1. Uplink audio source is wrong

- `JammaLib/src/audio/AudioHost.cpp:169` calls `ProcessAudioBlock(inBuf, ...)`.
- `inBuf` is raw interleaved ADC input for the callback.
- Station processing and loop playback are written later into `_channelMixer->Sink()` via `station->WriteBlock(...)` before `_channelMixer->ToDac(...)` sends the mixed result to the hardware outputs.

Result: NINJAM receives only the live input capture path, so remote users do not hear loop playback or post-router station output.

### 2. Advertised local NINJAM channels are derived from inputs, not routed outputs

- `JammaLib/src/ninjam/NinjamConnection.cpp:516` iterates while `inputChannel < _numInputChannels`.
- `JammaLib/src/ninjam/NinjamConnection.cpp:532` registers each local NINJAM channel via `SetLocalChannelInfo(...)` using those input indices.
- `JammaLib/src/ninjam/NinjamConnection.cpp:317` and `:325` do track `_numOutputChannels`, and `config_remote_autochan_nch` is set from outputs, but that only controls remote receive/output capacity, not the local channels this client publishes.

Result: other clients only see as many local stereo channels as Jamma advertises from ADC inputs. A machine with many DAC outs but only 2 or 4 ADC ins will still publish only 1 or 2 stereo channels.

## Non-Goals For This Change

- Do not redesign the full station engine.
- Do not change server tempo handling.
- Do not broaden remote-user visualisation beyond what is required for local send/export correctness.

## Export Policy

### Terminology

- A `NINJAM local channel` means one advertised NINJAM source lane. In practice this should usually be a stereo pair, because the current code already uses stereo local channels via `SetLocalChannelInfo(..., sourceChannel | 1024, ...)`.
- A `DAC export pair` means one used routed stereo output pair carrying post-router, post-station-master station audio.
- An `ADC pair` means one stereo pair of raw live input channels. If input count is odd, the final input may be published as mono or duplicated to stereo, but that should be treated as a secondary edge case.

### Budget Rule

- Jamma's public-server scan path does not expose any per-server local-channel cap today.
- Jamma's parsed public server metadata currently includes name, host, topic, status, users, capacity, and tempo, but not any advertised max local-channel count.
- After join, the only concrete in-tree hook is `NJClient::GetMaxLocalChannels()`.
- Because the vendored header does not prove whether `GetMaxLocalChannels()` is server-negotiated or only a client-side maximum, implementation should treat it as the best available runtime limit but still keep a Jamma-owned fallback constant in an existing NINJAM class.
- Convert both ADC and DAC hardware channel counts into stereo publish lanes before applying the ceiling.

Recommended resolution order:

1. If connected and `GetMaxLocalChannels()` returns a positive value, use that as the effective local-channel slot limit.
2. Otherwise fall back to a new constant attached to an existing NINJAM class, preferably `NinjamConnection`.

Recommended constant name:

- `NinjamConnection::DefaultLocalChannelSlotLimit`

Important normalization rule:

- The effective limit above is in NINJAM local-channel slots, not raw hardware channels.
- Jamma should convert that slot limit into a stereo publish-lane budget based on how many `SetLocalChannelInfo(...)` calls it intends to make.
- Because Jamma's intended export format is stereo lanes, one stereo publish lane should consume one local-channel slot.

### Packing Rule

1. Build a candidate list of used DAC export pairs in ascending hardware pair order.
2. Build a candidate list of ADC pairs in ascending input pair order.
3. Resolve an `effectiveLaneBudget` from the runtime NINJAM slot limit when available, otherwise from `NinjamConnection::DefaultLocalChannelSlotLimit`.
4. If `effectiveLaneBudget <= 0`, publish nothing and log the condition.
5. If `dacPairCount + adcPairCount <= effectiveLaneBudget`, publish all DAC pairs as separate NINJAM lanes, then append all ADC pairs as separate NINJAM lanes.
6. If `dacPairCount + adcPairCount > effectiveLaneBudget`, switch to modulo packing across exactly `effectiveLaneBudget` stereo publish lanes.

### Modulo Packing Rule When Over Budget

When over budget, both DAC export pairs and ADC pairs should be packed into the available stereo publish lanes separately by modulo index.

Rule:

- Publish exactly `effectiveLaneBudget` stereo lanes.
- For DAC export pair index `i`, mix that pair into publish lane `i % effectiveLaneBudget`.
- For ADC pair index `i`, mix that pair into publish lane `i % effectiveLaneBudget`.
- ADC and DAC are packed independently, then summed into the same publish-lane buffers.
- If there are no DAC pairs, ADC pairs still use the same modulo rule across the available lanes.
- If there are no ADC pairs, DAC pairs still use the same modulo rule across the available lanes.

This matches the desired behavior where a low lane budget causes both live inputs and routed station outputs to be merged predictably rather than dropping one source class.

### Ordering Rule

- Published channel ordering must be stable and deterministic.
- When under budget, DAC export pairs come first, ordered by routed hardware pair number, and dedicated ADC lanes come after them, ordered by ADC pair number.
- When over budget, publish lanes are numbered `0..effectiveLaneBudget-1` and keep that stable ordering.
- In over-budget mode, lane identity is based on lane index, not on any single underlying source pair, because each lane may contain multiple DAC and ADC contributors.

### Examples

1. `3` used DAC export pairs, `1` ADC pair, effective lane budget `10`.
   - Publish `4` channels total.
   - Channels `1..3` are DAC export pairs.
   - Channel `4` is the dedicated ADC pair.

2. `3` ADC pairs and `4` DAC export pairs, effective lane budget `2`.
   - Publish exactly `2` stereo lanes.
   - Lane `1/2` contains ADC `1/2`, ADC `5/6`, DAC `1/2`, DAC `5/6`.
   - Lane `3/4` contains ADC `3/4`, DAC `3/4`, DAC `7/8`.

3. `1` used DAC export pair, `2` ADC pairs, effective lane budget `1`.
   - Publish `1` channel total.
   - DAC export pair `1/2`, ADC `1/2`, and ADC `3/4` all fold into the same lane.

4. `0` used DAC export pairs, `2` ADC pairs, effective lane budget `10`.
   - Publish `2` ADC channels directly.

## Architecture Decision (read first)

This section resolves an over-engineered assumption in earlier drafts.

### Key codebase fact

The DAC sink already contains exactly the audio we want to publish.

- In `AudioHost::_OnAudio` (`JammaLib/src/audio/AudioHost.cpp`), each local station writes its output into `_channelMixer->Sink()` via `station->WriteBlock(_channelMixer->Sink(), ...)`.
- `Station::WriteBlock` writes post-router, post-station-master audio into the DAC sink channel that the station's router targets.
- Multiple stations targeting the same output pair are summed automatically because they write into the same sink channel.
- `_channelMixer->ToDac(outBuf, NumOutputChannels, numSamps)` then copies those sink channels into the interleaved hardware buffer `outBuf`.

Consequence: after `ToDac`, `outBuf` is the post-router, post-master station mix, laid out per DAC output channel. That is precisely what the user wants remote users to hear.

### Chosen approach (simplest correct)

- Do not build a separate per-station export bus.
- Tap `outBuf` (post `ToDac`) for DAC pair content.
- Tap `inBuf` (raw interleaved ADC) for ADC pair content.
- Pack both directly into `NinjamConnection`'s existing per-channel `_inScratch` lanes, which are already what `NJClient::AudioProc` consumes. This avoids an extra interleave/deinterleave step.
- Decouple the NINJAM local-send channel count from hardware ADC count. NINJAM "input" channels become the packed stereo lanes, not the sound card inputs.

### Conflict resolved

- Earlier drafts warned about a private local monitor pair on outputs `1/2`. There is no monitor-only output concept in the DAC path today. The user explicitly wants remote users to hear what is sent to the DAC. Therefore all used DAC output pairs are publishable; do not special-case output `1/2`.

### Scope simplification

- v1 (ship this): treat every DAC output pair as publishable, so `dacPairs = NumOutputChannels / 2`. This fixes both defects with no station-routing introspection.
- v2 (optional later): suppress DAC pairs that have no station routed to them, to avoid publishing silent lanes. This is a refinement, not a blocker, and requires a routing snapshot.

## Real-Time / Hot-Path Rules (mandatory)

The packing/mixing runs inside `AudioHost::_OnAudio`, which is the RtAudio callback and is performance critical. Treat every rule below as a hard requirement.

- No heap allocation on the audio thread. All scratch buffers (`_inScratch`, `_inPtrs`, and any export scratch) must be pre-sized off the audio thread and only reused in the callback.
- No mutex locking on the audio thread. Do not call anything that may take `_connectionMutex` or `_snapshotMutex` from the packing path.
- Do not call `NJClient::GetMaxLocalChannels()` from the audio thread. It touches client state of unknown cost. Resolve the budget on the job/pump thread and cache it.
- Publish packing parameters using a small trivially-copyable POD in a `std::atomic<...>` so the audio thread reads them lock-free. This mirrors the existing published-immutable-snapshot pattern used for `_audioStations`.
- Size all scratch to the worst case (`2 * resolvedLaneBudget` channels, `blockSize` frames) when the audio format or budget changes, under the existing init lock, never in the callback.
- The packing loop must be branch-light and allocation-free: zero the active lanes, then accumulate with `+=` in flat index loops.
- A one-block disagreement between advertised lane count and packed lane count must be harmless: extra lanes carry silence, missing contributors are simply skipped. Never index outside the pre-sized scratch.
- Keep the existing `numFrames > scratch depth` early-return guard in the block-processing path.

## Implementation Strategy

Ordered by dependency. Each step lists the file, the function, and the thread it runs on. Steps 1 through 4 are all inside `NinjamConnection`; step 5 wires the audio callback; step 6 is a thin controller passthrough.

### Shared data structure

Add a trivially-copyable packing descriptor near `NinjamConnection` (header):

```cpp
struct NinjamLanePacking
{
    std::uint16_t LaneCount = 0;    // number of stereo publish lanes actually used
    std::uint16_t DacPairs = 0;     // NumOutputChannels / 2
    std::uint16_t AdcPairs = 0;     // NumInputChannels / 2
    std::uint8_t  Modulo = 0;       // 0 = direct mapping, 1 = modulo packing
    std::uint8_t  FromFallback = 0; // 0 = runtime GetMaxLocalChannels, 1 = fallback constant
};
```

Store it as `std::atomic<NinjamLanePacking> _lanePacking;`. It is 8 bytes and trivially copyable, so it is lock-free on x64. The audio thread only ever reads it with `load(std::memory_order_acquire)`; the job thread writes it with `store(..., std::memory_order_release)`.

### Step 1. Add the fallback constant and budget resolver (job thread)

- File: `JammaLib/src/ninjam/NinjamConnection.h` / `.cpp`.
- Add `static constexpr unsigned int DefaultLocalChannelSlotLimit = /* agreed value, e.g. 8 */;`.
- Add a private method `NinjamLanePacking _ResolveLanePacking() const;` intended to run on the job thread.
- Resolution logic:
  1. `slotLimit = (connected && _client && _client->GetMaxLocalChannels() > 0) ? GetMaxLocalChannels() : DefaultLocalChannelSlotLimit;` and record which source was used.
  2. `dacPairs = _numOutputChannels / 2; adcPairs = _numInputChannels / 2;`
  3. `budget = slotLimit;` (one stereo lane consumes one local-channel slot).
  4. If `budget == 0`: `laneCount = 0; modulo = 0;` (publish nothing; log it).
  5. Else if `dacPairs + adcPairs <= budget`: `laneCount = dacPairs + adcPairs; modulo = 0;` (direct mapping).
  6. Else: `laneCount = budget; modulo = 1;` (modulo packing across all budgeted lanes).
- This method must only be called from the job thread (from Pump/format handling), because it may read `_client`.

### Step 2. Cache packing and size scratch (job thread)

- File: `JammaLib/src/ninjam/NinjamConnection.cpp`, in `SetAudioFormat(...)` and wherever local channels are (re)applied after connect.
- Compute `auto packing = _ResolveLanePacking();`.
- Size scratch for the worst case: ensure `_inScratch` has `2 * packing.LaneCount` channels (or `2 * budget` if you prefer a fixed upper bound), each `blockSize` frames; rebuild `_inPtrs` accordingly. Reuse `_ResizeScratchBuffers` but drive its channel count from the lane channel count, not from `_numInputChannels`.
- `_lanePacking.store(packing, std::memory_order_release);`
- Then call `_ApplyLocalChannels()` so advertisement matches the cached packing.
- All of this is job-thread only and may allocate; it must never run from the audio callback.

### Step 3. Rewrite `_ApplyLocalChannels()` to iterate lanes (job thread)

- File: `JammaLib/src/ninjam/NinjamConnection.cpp` (currently around the `while ((configuredChannel < maxLocalChannels) && (inputChannel < _numInputChannels))` loop).
- Load the cached `NinjamLanePacking`.
- Delete existing local channels first (unchanged).
- For `lane` in `0 .. LaneCount-1`, register one stereo local channel:
  - `srcch = (2 * lane) | 1024;` (stereo pair flag, matching current convention).
  - Name:
    - Direct mode, DAC lane (`lane < DacPairs`): `"Jamma Out " + (2*lane+1) + "/" + (2*lane+2)`.
    - Direct mode, ADC lane (`lane >= DacPairs`): `"Jamma In " + ...` using the ADC pair index `lane - DacPairs`.
    - Modulo mode: `"Jamma Mix " + (lane+1)` because each lane can carry multiple contributors.
  - Call `SetLocalChannelInfo(lane, name, true, srcch, true, 96, true, true)` exactly as today, but indexed by lane.
- Keep the trailing `NotifyServerOfChannelChange()`.

### Step 4. Pack DAC + ADC into `_inScratch` (audio thread, RT-critical)

- File: `JammaLib/src/ninjam/NinjamConnection.cpp`. Replace the current `ProcessAudioBlock(interleavedInput, ...)` semantics with a packing entry point, e.g. `ProcessExportBlock(const float* dacOut, unsigned int numOutCh, const float* adcIn, unsigned int numInCh, unsigned int numFrames, unsigned int sampleRate)`.
- Steps (all allocation-free, lock-free):
  1. Early-out if not connected, no client, or `numFrames == 0`.
  2. `packing = _lanePacking.load(std::memory_order_acquire);` and `laneCh = 2 * packing.LaneCount;`.
  3. Guard: if `laneCh == 0`, or `_inScratch.size() < laneCh`, or `numFrames > _inScratch[0].size()`, return.
  4. Zero `_inScratch[0 .. laneCh)` for `numFrames`.
  5. DAC accumulate (only if `dacOut != nullptr`): for `p` in `0 .. DacPairs-1`, `lane = packing.Modulo ? (p % packing.LaneCount) : p;` if `lane < LaneCount`, add `dacOut[frame*numOutCh + 2p]` into `_inScratch[2*lane][frame]` and `+1` into `[2*lane+1][frame]`.
  6. ADC accumulate (only if `adcIn != nullptr`): for `q` in `0 .. AdcPairs-1`, `lane = packing.Modulo ? (q % packing.LaneCount) : (DacPairs + q);` if `lane < LaneCount`, add `adcIn[frame*numInCh + 2q]` and `+1` similarly.
  7. Point `_inPtrs[0 .. laneCh)` at the lane buffers and call `AudioProc(_inPtrs.data(), laneCh, _outPtrs.data(), _numOutputChannels, numFrames, sampleRate)`.
  8. Store `_lastNumFrames`.
- Odd hardware channel counts: integer-divide to pairs and ignore a dangling odd channel in v1. Document this.
- The remote-receive side (`_outScratch`, `ConsumeStereoPair`, `config_remote_autochan_nch`) is unchanged and still driven by `_numOutputChannels`.

### Step 5. Wire the audio callback (audio thread)

- File: `JammaLib/src/audio/AudioHost.cpp`, `AudioHost::_OnAudio`.
- Remove the current early NINJAM call `_ninjamController->ProcessAudioBlock(inBuf, ...)` that runs before `outBuf` is filled.
- Move the NINJAM send to after `_channelMixer->ToDac(outBuf, audioStreamParams.NumOutputChannels, numSamps)` in the `outBuf != nullptr` branch, so `outBuf` holds the post-router, post-master mix.
- Call `_ninjamController->ProcessExportBlock(outBuf, audioStreamParams.NumOutputChannels, inBuf, audioStreamParams.NumInputChannels, numSamps, audioStreamParams.SampleRate);`.
- In the `outBuf == nullptr` branch (offline/no output device), call with `dacOut = nullptr` so only ADC lanes contribute.
- No new buffers are created here; all packing scratch lives in `NinjamConnection` and is pre-sized.

### Step 6. Controller passthrough

- File: `JammaLib/src/ninjam/NinjamController.h` / `.cpp`.
- Add `ProcessExportBlock(...)` that forwards to `_session`/`NinjamConnection`, mirroring the existing `ProcessAudioBlock` passthrough. Keep or remove the old `ProcessAudioBlock` depending on whether anything else calls it (currently only `AudioHost` does).

### Step 7. Logging (job thread)

Emit these only when values change, to avoid console spam:

- On connect and on packing recompute: lane count, mode (direct/modulo), dacPairs, adcPairs, and whether the budget came from `GetMaxLocalChannels()` or `DefaultLocalChannelSlotLimit`.
- Local NINJAM channel registration count and names after `_ApplyLocalChannels()`.
- A one-line warning when modulo packing engages because `dacPairs + adcPairs` exceeded the budget.

## Suggested Code Touch Points

- `JammaLib/src/audio/AudioHost.cpp`
  - Remove the early `ProcessAudioBlock(inBuf, ...)` call.
  - After `ToDac`, call `ProcessExportBlock(outBuf, NumOutputChannels, inBuf, NumInputChannels, numSamps, sampleRate)`.
- `JammaLib/src/ninjam/NinjamController.h/.cpp`
  - Add a `ProcessExportBlock(...)` passthrough to the session/connection.
- `JammaLib/src/ninjam/NinjamConnection.h/.cpp`
  - Add `DefaultLocalChannelSlotLimit`, the `NinjamLanePacking` POD, and `std::atomic<NinjamLanePacking> _lanePacking`.
  - Add `_ResolveLanePacking()` (job thread) and call it from `SetAudioFormat`.
  - Size `_inScratch`/`_inPtrs` from the lane channel count instead of `_numInputChannels`.
  - Rewrite `_ApplyLocalChannels()` to iterate lanes.
  - Replace `ProcessAudioBlock` input semantics with `ProcessExportBlock` packing.

## Dependencies

- Confirmation that `outBuf` after `ToDac` is the desired publish signal (established in Architecture Decision).
- An agreed value for `DefaultLocalChannelSlotLimit` used when runtime discovery is unavailable.
- Nothing else. v1 deliberately avoids a station-routing snapshot; that is only needed for the optional v2 refinement that suppresses silent DAC pairs.

## Risks

### Real-time safety

- The audio callback must not allocate; all scratch is pre-sized off-thread (see Real-Time / Hot-Path Rules).
- The audio callback must not lock; packing parameters are read from `std::atomic<NinjamLanePacking>`.
- `GetMaxLocalChannels()` is resolved on the job thread only and cached.
- Scratch is sized to the worst case so a mid-change atomic read can never index out of bounds.

### Behavioral correctness of the publish signal

- `outBuf` after `ToDac` is the intended publish signal: post-router, post-master, summed per output pair.
- There is no private monitor pair to protect in the current DAC path; all used output pairs are publishable.
- Modulo packing writes only into the NINJAM scratch lanes; it never mutates `outBuf`, so local DAC monitoring is unaffected.

### Channel identity churn

- If export-pair ordering changes when station routing changes, remote users may see channel names/order flap.
- Use a stable, deterministic ordering based on output pair number.
- In modulo mode, use generic lane identities so contributor churn does not masquerade as channel renames.

### Budget surprises

- `GetMaxLocalChannels()` may differ across NINJAM client/server combinations.
- Jamma does not currently discover any server-side cap during public server scan.
- The meaning of `GetMaxLocalChannels()` should be treated as best-effort runtime evidence, not proven server metadata.
- Logging should print the discovered runtime limit or fallback constant so behavior is explainable when direct mapping flips to modulo packing.

### Backward compatibility

- Existing sessions that implicitly assumed mic-only NINJAM send will change behavior.
- Logging and release notes should call that out.

## Test Plan

### Focused manual cases

1. 6 DAC outputs, 3 stations routed to `1/2`, `3/4`, `5/6`.
   - Remote client should see 3 stereo channels from this user.
   - Each channel should carry only its assigned station's post-master audio.

2. 6 DAC outputs, 3 routed station pairs plus 1 ADC pair, with effective lane budget large enough to keep them separate.
   - Remote client should see 4 channels total.
   - The first 3 channels should be DAC export pairs.
   - The last channel should be the dedicated ADC pair.

3. 4 DAC outputs, 2 stations both routed to `1/2`.
   - Remote client should see 1 stereo channel if publishable channels are compacted by used export pair.
   - That channel should contain the sum of both stations.

4. 4 DAC outputs, 2 stations routed to `1/2` and `3/4`.
   - Remote client should see 2 stereo channels.

5. 8 DAC outputs, 3 ADC pairs, effective lane budget `2`.
   - Remote client should still see only 2 channels.
   - Lane `1` should contain DAC pairs `1` and `3`, plus ADC pairs `1` and `3`.
   - Lane `2` should contain DAC pairs `2` and `4`, plus ADC pair `2`.

6. Live mic present but no routed station exports.
   - Remote client should receive direct ADC channels up to the runtime limit.

7. Loop playback with muted or lowered station master.
   - Remote client level should track the station master and mute state.

8. Over-budget modulo case.
   - A diagnostic line should explain that modulo packing is active.
   - Another diagnostic line should show whether the lane budget came from `GetMaxLocalChannels()` or the fallback constant.

### Regression checks

- Local DAC playback remains unchanged.
- Remote-user receive playback in Jamma still works.
- Connect/disconnect does not leak stale channel registrations.

## Acceptance Criteria

- NINJAM uplink audio is sourced from `outBuf` (post `ToDac`, i.e. post-router/post-station-master), not raw ADC input.
- Advertised local NINJAM stereo channels are driven by the cached `NinjamLanePacking`, not by ADC input count alone.
- When lane budget allows, ADC appears as separate NINJAM channels in addition to DAC-routed station exports.
- When lane budget does not allow that, ADC and DAC are merged deterministically into a fixed number of publish lanes by modulo index.
- If runtime lane-limit discovery is unavailable, Jamma falls back to `NinjamConnection::DefaultLocalChannelSlotLimit`.
- Lane numbering is stable and deterministic.
- No allocations and no locks are introduced into the audio callback; all NINJAM scratch is pre-sized off-thread and packing params are read from an atomic POD.

## Follow-Up Investigation

The current local remote-user playback path appears stereo-only in `StationRemote` and may not yet represent multi-channel remote users inside Jamma itself. That is adjacent to, but separate from, the two confirmed defects above and should be handled as a follow-up unless implementation work naturally touches that area.