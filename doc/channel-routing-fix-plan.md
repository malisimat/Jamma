# Channel Routing Fix Plan

## Scope
This plan captures confirmed implementation bugs discovered while validating ADC -> ChannelMixer -> Station -> LoopTake -> Loop recording and Loop -> LoopTake -> Station -> DAC playback routing.

## Bug 1: LoopTake conflates channel index with loop slot index

### Why this is a bug
Current code paths assume that incoming channel index directly equals loop array index. That breaks whenever selected input channels are non-contiguous or not zero-based.

### Evidence
- `LoopTake::_InputChannel(...)` routes record sources (`AUDIOSOURCE_ADC`, `AUDIOSOURCE_MONITOR`, `AUDIOSOURCE_BOUNCE`) via `_loops[channel]`.
- `LoopTake::AddLoop(unsigned int chan, ...)` seeds each `Loop` output wire channels with `{ chan }`.
- Playback handoff (`AUDIOSOURCE_MIXER`) also indexes `_audioBuffers[channel]`.
- `_audioBuffers` count equals number of loops, not max channel index.

### Observable impact
- Recording from channels like `[2,3]` into a 2-loop take can drop writes (no `_loops[2]` / `_loops[3]`).
- Playback mapping can write to non-existent LoopTake buffer indices when loop channel tags are sparse.
- User mental model "loop slot 0/1" diverges from runtime behavior if source channels are not contiguous from zero.

### Fix strategy
1. Add/extend tests:
   - Record from source channels `[2,4]` with two loops and test if both loops receive data (should fail).
2. Introduce explicit channel-to-loop-slot mapping in `LoopTake`.
3. For record sources, map incoming channel to loop slot by trigger source (`WireMixBehaviour`) / loop metadata (`Loop::LoopChannel()`) instead of raw array index.
4. Decouple playback handoff from physical input channel tags: route Loop output into LoopTake buffer by loop slot index.
5. Ensure map is accurate though all relevant transitions:
   - `AddLoop`, `Record`, `Overdub`, `_CommitChanges`, load-from-file path.
6. Add/extend tests:
   - Record from source channels `[2,4]` with two loops and verify both loops receive data.
   - Playback for sparse channel-tag loops verifies no dropped or misrouted samples.
   - Regression test for contiguous channel case to ensure no behavior regression.

### Risk notes
- This code runs in hot/audio paths. Prefer precomputed vector/array lookup over per-sample dynamic search.
- Maintain RT-safety (no heap allocation inside callback-time methods).

## Bug 2: ChannelMixer cannot shrink channel buffer count

### Why this is a bug
`ChannelMixer::BufferMixer::SetNumChannels(...)` tries to shrink buffers when `numChans < numInputs`, but calls `resize(numInputs)` instead of `resize(numChans)`.

### Evidence
- In `ChannelMixer.cpp`, shrink branch keeps size unchanged.

### Observable impact
- Reducing configured channel count leaves stale channels alive.
- Can leak stale routing/output state and produce surprising index behavior after channel-count changes.

### Fix strategy
1. Change shrink branch to `resize(numChans)`.
2. Add regression test:
   - Start with N channels, shrink to M < N.
   - Assert `NumInputChannels`/`NumOutputChannels` and backing channel access are exactly M.
3. Validate re-expand still works after shrink.

## Suggested implementation order
1. Fix Bug 2 first (small, isolated, low risk).
2. Run tests.
3. Rubber duck review.
4. Implement Bug 1 with tests before and after each refactor step.
5. Run full audio flow and flip-buffer tests after Bug 1 changes.
6. Rubber duck review.