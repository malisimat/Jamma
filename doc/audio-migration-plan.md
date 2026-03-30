# Audio Hierarchy Migration Plan

## Overview

This document describes the migration from the current mixed source/destination audio
API toward a consistent destination-centric `OnBlockWrite(request, writeOffset)` model.

## Current Architecture

The audio hierarchy currently uses two patterns:

| Pattern | Method | Direction |
|---------|--------|-----------|
| Source-driven | `AudioSource::OnPlay(dest, offset, numSamps)` | Source reads its buffer, pushes per-sample to dest |
| Destination-driven | `AudioSink::OnMixWrite(samp, fade, ...)` | Dest receives one sample at a time |

### Problems

1. **Per-sample virtual dispatch** in hot paths (e.g., `AudioBuffer::OnPlay` calls
   `dest->OnMixWrite()` once per sample — N virtual calls per block).
2. **Pointer arithmetic scattered** across source and destination layers.
3. **No zero-copy path** — every transfer copies sample-by-sample.
4. **Inconsistent responsibility** — sometimes the source manages offsets, sometimes
   the destination does.

## New API

### `AudioWriteRequest` (defined in `AudioSink.h`)

```cpp
struct AudioWriteRequest
{
    const float* samples;           // Source sample buffer
    unsigned int numSamps;          // Number of samples to write
    unsigned int stride;            // 1 = contiguous, N = interleaved
    float fadeCurrent;              // Fade factor for existing content
    float fadeNew;                  // Fade factor for new content
    Audible::AudioSourceType source;
};
```

### `AudioSink::OnBlockWrite(request, writeOffset)`

- Destination-centric: the *sink* owns index arithmetic and mixing.
- Default implementation falls back to per-sample `OnMixWrite` for backward
  compatibility.
- Concrete classes override for optimized block-level writes.

### `MultiAudioSink::OnBlockWriteChannel(channel, request, writeOffset)`

- Routes block writes to individual channel sinks.
- Default implementation delegates to `_InputChannel(channel)->OnBlockWrite()`.

## Migration Status

### Phase 1 — Non-breaking API + First Hot Paths (this PR)

| Component | Status | Notes |
|-----------|--------|-------|
| `AudioSink::OnBlockWrite` | ✅ Added | Default fallback to `OnMixWrite` |
| `MultiAudioSink::OnBlockWriteChannel` | ✅ Added | Routes to channel sink |
| `AudioBuffer::OnBlockWrite` | ✅ Optimized | Direct buffer access, no virtual dispatch per sample |
| `AudioBuffer::OnPlay` | ✅ Migrated | Uses `dest->OnBlockWrite()` with contiguous pointer |
| `Loop::OnBlockWrite` | ✅ Optimized | Direct `BufferBank` access, no virtual dispatch |
| `ChannelMixer::FromAdc` | ✅ Migrated | Uses `OnBlockWrite` with stride for interleaved ADC data |
| Tests | ✅ Added | `AudioBlockWrite_Tests.cpp` |

### Phase 2 — Migrate Remaining Write Paths

| Component | Current Pattern | Migration |
|-----------|----------------|-----------|
| `Station::OnWriteChannel` | Delegates to `LoopTake::OnWriteChannel` | Can adopt `OnBlockWriteChannel` |
| `LoopTake::_InputChannel` routing | Per-sample via `OnMixWrite` | Override `OnBlockWriteChannel` |
| `MultiAudioSource::OnPlay` base | Iterates channels, calls `OnWriteChannel` | Could call `OnBlockWriteChannel` |
| `AudioMixer::OnPlay` | Per-sample mixing with level/pan | Block-level with pre-computed gains |

### Phase 3 — Migrate Playback Read Paths

| Component | Current Pattern | Migration |
|-----------|----------------|-----------|
| `Loop::OnPlay` | Reads `BufferBank` per-sample, calls mixer per-sample | Provide contiguous spans from `BufferBank` |
| `LoopTake::OnPlay` | Iterates loops, reads `AudioBuffer` per-sample | Use block read from `AudioBuffer` |
| `ChannelMixer::ToDac` | Reads `AudioBuffer` per-sample to interleaved output | Block read with stride |

### Phase 4 — Deprecate Legacy Path

Once all call sites are migrated:

1. Mark `OnMixWrite` as `[[deprecated]]`.
2. Mark `OnMixWriteChannel` as `[[deprecated]]`.
3. Remove default fallback in `AudioSink::OnBlockWrite`.
4. Remove deprecated methods after one release cycle.

## Performance Characteristics

### Before (per-sample dispatch)

For a block of N samples:
- `AudioBuffer::OnPlay`: N virtual calls to `dest->OnMixWrite()`
- `ChannelMixer::FromAdc`: N virtual calls to `buf->OnMixWrite()` per channel

### After (block dispatch)

For a block of N samples:
- `AudioBuffer::OnPlay`: 1-2 calls to `dest->OnBlockWrite()` (2 if buffer wraps)
- `ChannelMixer::FromAdc`: 1 call to `buf->OnBlockWrite()` per channel

Virtual dispatch overhead is reduced from O(N) to O(1) per block per channel.

## Design Decisions

1. **Stride field** enables zero-copy from interleaved hardware buffers (ADC/DAC)
   without intermediate de-interleaving.

2. **Default fallback** ensures all existing `AudioSink` subclasses work unchanged —
   they get block writes decomposed into per-sample `OnMixWrite` calls automatically.

3. **`writeOffset` parameter** replaces the accumulating `indexOffset` return value
   pattern, giving the caller explicit control over write positioning.

4. **Non-breaking addition** — all existing call sites continue to work. Migration is
   opt-in per class.
